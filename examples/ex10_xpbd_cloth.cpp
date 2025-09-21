#include "vk_engine.h"
#include "vv_camera.h"
#include <imgui.h>
#include <vulkan/vulkan.h>
#include <vk_mem_alloc.h>

#include <vector>
#include <string>
#include <cmath>
#include <algorithm>
#include <stdexcept>
#include <fstream>

#ifndef VK_CHECK
#define VK_CHECK(x) do{VkResult r=(x); if(r!=VK_SUCCESS) throw std::runtime_error("Vulkan error: "+std::to_string(r)); }while(false)
#endif

static std::vector<char> load_spv(const std::string& p){ std::ifstream f(p, std::ios::binary | std::ios::ate); if(!f) throw std::runtime_error("open "+p); size_t s=(size_t)f.tellg(); f.seekg(0); std::vector<char> d(s); f.read(d.data(), (std::streamsize)s); return d; }
static VkShaderModule make_shader(VkDevice d, const std::vector<char>& b){ VkShaderModuleCreateInfo ci{VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO}; ci.codeSize=(uint32_t)b.size(); ci.pCode=(const uint32_t*)b.data(); VkShaderModule m{}; VK_CHECK(vkCreateShaderModule(d,&ci,nullptr,&m)); return m; }

struct ClothXPBD {
    struct Edge { int i, j; float rest; float compliance; float lambda; enum Type: uint8_t { Structural, Shear, Bend } type; };
    int nx{16}, ny{16};
    float spacing{0.08f};
    std::vector<vv::float3> x;    // current positions
    std::vector<vv::float3> v;    // velocities
    std::vector<float> inv_m;     // inverse masses
    std::vector<Edge> edges;
    vv::float3 origin{ -0.6f, 0.8f, 0.0f };

    void build_grid(int gx, int gy, float dx){
        nx = std::max(2, gx); ny = std::max(2, gy); spacing = dx;
        x.resize(nx*ny); v.assign(nx*ny, {0,0,0}); inv_m.assign(nx*ny, 1.0f);
        auto idx = [&](int ix, int iy){ return iy*nx + ix; };
        for(int iy=0; iy<ny; ++iy){ for(int ix=0; ix<nx; ++ix){ vv::float3 p{ origin.x + ix*spacing, origin.y - iy*spacing, origin.z }; x[idx(ix,iy)] = p; }}
        inv_m[idx(0,0)] = 0.0f; inv_m[idx(nx-1,0)] = 0.0f;
        edges.clear(); edges.reserve(nx*ny*4);
        auto add_edge = [&](int a, int b, float comp, Edge::Type t){ vv::float3 d = x[b]-x[a]; float rl = vv::length(d); edges.push_back(Edge{a,b,rl,comp,0.0f,t}); };
        float comp_struct = 0.0f, comp_shear = 0.0f, comp_bend = 0.001f;
        for(int iy=0; iy<ny; ++iy){ for(int ix=0; ix<nx; ++ix){ int a = idx(ix,iy);
            if (ix+1<nx) add_edge(a, idx(ix+1,iy), comp_struct, Edge::Structural);
            if (iy+1<ny) add_edge(a, idx(ix,iy+1), comp_struct, Edge::Structural);
            if (ix+1<nx && iy+1<ny){ add_edge(a, idx(ix+1,iy+1), comp_shear, Edge::Shear); add_edge(idx(ix+1,iy), idx(ix,iy+1), comp_shear, Edge::Shear); }
            if (ix+2<nx) add_edge(a, idx(ix+2,iy), comp_bend, Edge::Bend);
            if (iy+2<ny) add_edge(a, idx(ix,iy+2), comp_bend, Edge::Bend);
        }}
    }
};

class XPBDClothRenderer : public IRenderer {
public:
    void get_capabilities(const EngineContext&, RendererCaps& c) override {
        c = RendererCaps{}; c.enable_imgui = true; c.presentation_mode = PresentationMode::EngineBlit;
        c.color_attachments = { AttachmentRequest{ .name = "color", .format = VK_FORMAT_B8G8R8A8_UNORM } }; c.presentation_attachment = "color";
        c.depth_attachment = AttachmentRequest{ .name = "depth", .format = c.preferred_depth_format, .usage = VK_IMAGE_USAGE_DEPTH_STENCIL_ATTACHMENT_BIT, .samples = VK_SAMPLE_COUNT_1_BIT, .aspect = VK_IMAGE_ASPECT_DEPTH_BIT, .initial_layout = VK_IMAGE_LAYOUT_UNDEFINED };
        c.uses_depth = VK_TRUE;
    }

    void initialize(const EngineContext& e, const RendererCaps&, const FrameContext&) override {
        eng_ = e; dev_ = e.device; color_fmt_ = VK_FORMAT_B8G8R8A8_UNORM; depth_fmt_ = e.device? VK_FORMAT_D32_SFLOAT : VK_FORMAT_D32_SFLOAT;
        // build scene
        cloth_.build_grid(params_.grid_x, params_.grid_y, params_.spacing);
        apply_compliance_(); recenter_cloth_at_origin_(); build_gpu_buffers_(); build_pipelines_();
        cam_.set_mode(vv::CameraMode::Orbit); auto s = cam_.state(); s.target={0,0,0}; s.distance=2.0f; s.pitch_deg=15.0f; s.yaw_deg=-120.0f; s.znear=0.01f; s.zfar=100.0f; cam_.set_state(s);
        update_scene_bounds_(); cam_.frame_scene(1.12f);
        sim_accum_ = 0.0;
    }

    void destroy(const EngineContext& e, const RendererCaps&) override {
        destroy_gpu_buffers_(); destroy_pipelines_(); dev_ = VK_NULL_HANDLE; eng_ = {};
    }

    void update(const EngineContext&, const FrameContext& f) override {
        cam_.update(f.dt_sec, (int)f.extent.width, (int)f.extent.height); vp_w_=(int)f.extent.width; vp_h_=(int)f.extent.height;
        if (params_.simulate) { sim_accum_ += f.dt_sec; double fixed=std::clamp<double>(params_.fixed_dt, 1.0/600.0, 1.0/30.0); int maxSteps=4; while(sim_accum_>=fixed && maxSteps--){ step_sim_((float)fixed); sim_accum_-=fixed; } }
        // upload positions
        if (pos_buf_.mapped && !cloth_.x.empty()) { std::memcpy(pos_buf_.mapped, cloth_.x.data(), cloth_.x.size()*sizeof(vv::float3)); }
    }

    void on_event(const SDL_Event& e, const EngineContext& eng, const FrameContext* f) override {
        // Forward raw SDL events exactly like ex09 so CameraService handles vertical directions consistently.
        cam_.handle_event(e, &eng, f);
    }

    void record_graphics(VkCommandBuffer cmd, const EngineContext&, const FrameContext& f) override {
        if (f.color_attachments.empty() || !pipe_tri_.pipeline) return;
        const auto& color = f.color_attachments.front(); const auto* depth = f.depth_attachment;
        auto barrier_img = [&](VkImage img, VkImageAspectFlags aspect, VkImageLayout oldL, VkImageLayout newL, VkPipelineStageFlags2 src, VkPipelineStageFlags2 dst, VkAccessFlags2 sa, VkAccessFlags2 da){
            VkImageMemoryBarrier2 b{VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER_2}; b.srcStageMask=src; b.dstStageMask=dst; b.srcAccessMask=sa; b.dstAccessMask=da; b.oldLayout=oldL; b.newLayout=newL; b.image=img; b.subresourceRange={aspect,0,1,0,1}; VkDependencyInfo di{VK_STRUCTURE_TYPE_DEPENDENCY_INFO}; di.imageMemoryBarrierCount=1; di.pImageMemoryBarriers=&b; vkCmdPipelineBarrier2(cmd,&di);
        };
        barrier_img(color.image, color.aspect, VK_IMAGE_LAYOUT_GENERAL, VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL, VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT, VK_PIPELINE_STAGE_2_COLOR_ATTACHMENT_OUTPUT_BIT, VK_ACCESS_2_MEMORY_WRITE_BIT, VK_ACCESS_2_COLOR_ATTACHMENT_WRITE_BIT);
        if (depth) barrier_img(depth->image, depth->aspect, VK_IMAGE_LAYOUT_UNDEFINED, VK_IMAGE_LAYOUT_DEPTH_ATTACHMENT_OPTIMAL, VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT, VK_PIPELINE_STAGE_2_EARLY_FRAGMENT_TESTS_BIT, 0, VK_ACCESS_2_DEPTH_STENCIL_ATTACHMENT_WRITE_BIT);
        VkClearValue clear_color{.color={{0.05f,0.06f,0.07f,1.0f}}}; VkClearValue clear_depth{.depthStencil={1.0f,0}};
        VkRenderingAttachmentInfo ca{VK_STRUCTURE_TYPE_RENDERING_ATTACHMENT_INFO}; ca.imageView=color.view; ca.imageLayout=VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL; ca.loadOp=VK_ATTACHMENT_LOAD_OP_CLEAR; ca.storeOp=VK_ATTACHMENT_STORE_OP_STORE; ca.clearValue=clear_color;
        VkRenderingAttachmentInfo da{}; if (depth){ da.sType=VK_STRUCTURE_TYPE_RENDERING_ATTACHMENT_INFO; da.imageView=depth->view; da.imageLayout=VK_IMAGE_LAYOUT_DEPTH_ATTACHMENT_OPTIMAL; da.loadOp=VK_ATTACHMENT_LOAD_OP_CLEAR; da.storeOp=VK_ATTACHMENT_STORE_OP_STORE; da.clearValue=clear_depth; }
        VkRenderingInfo ri{VK_STRUCTURE_TYPE_RENDERING_INFO}; ri.renderArea={{0,0}, f.extent}; ri.layerCount=1; ri.colorAttachmentCount=1; ri.pColorAttachments=&ca; ri.pDepthAttachment = depth? &da : nullptr; vkCmdBeginRendering(cmd,&ri);
        VkViewport vp{}; vp.x=0; vp.y=0; vp.width=(float)f.extent.width; vp.height=(float)f.extent.height; vp.minDepth=0; vp.maxDepth=1; VkRect2D sc{{0,0}, f.extent}; vkCmdSetViewport(cmd,0,1,&vp); vkCmdSetScissor(cmd,0,1,&sc);
        const vv::float4x4 V = cam_.view_matrix(); const vv::float4x4 P = cam_.proj_matrix(); vv::float4x4 MVP = vv::mul(P, V);
        struct PC { float mvp[16]; float color[4]; float pointSize; float _pad[3]; } pc{};
        std::memcpy(pc.mvp, MVP.m.data(), sizeof(pc.mvp));
        VkDeviceSize offs = 0; vkCmdBindVertexBuffers(cmd, 0, 1, &pos_buf_.buf, &offs);
        if (params_.render_mode == 0 /*Mesh*/){
            pc.color[0]=0.55f; pc.color[1]=0.7f; pc.color[2]=0.95f; pc.color[3]=1.0f; pc.pointSize = params_.point_size;
            vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, pipe_tri_.pipeline);
            vkCmdPushConstants(cmd, pipe_tri_.layout, VK_SHADER_STAGE_VERTEX_BIT, 0, sizeof(PC), &pc);
            vkCmdBindIndexBuffer(cmd, tri_idx_.buf, 0, VK_INDEX_TYPE_UINT32);
            vkCmdDrawIndexed(cmd, tri_count_, 1, 0, 0, 0);
        } else if (params_.render_mode == 1 /*Vertices*/){
            pc.color[0]=1.0f; pc.color[1]=1.0f; pc.color[2]=1.0f; pc.color[3]=1.0f; pc.pointSize = params_.point_size;
            vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, pipe_point_.pipeline);
            vkCmdPushConstants(cmd, pipe_point_.layout, VK_SHADER_STAGE_VERTEX_BIT, 0, sizeof(PC), &pc);
            vkCmdDraw(cmd, (uint32_t)cloth_.x.size(), 1, 0, 0);
        } else { // Constraints
            // Structural lines
            pc.pointSize = params_.point_size;
            vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, pipe_line_.pipeline);
            // struct
            pc.color[0]=0.86f; pc.color[1]=0.86f; pc.color[2]=0.86f; pc.color[3]=1.0f; vkCmdPushConstants(cmd, pipe_line_.layout, VK_SHADER_STAGE_VERTEX_BIT, 0, sizeof(PC), &pc);
            if (line_struct_count_>0) { vkCmdBindIndexBuffer(cmd, line_struct_.buf, 0, VK_INDEX_TYPE_UINT32); vkCmdDrawIndexed(cmd, line_struct_count_, 1, 0, 0, 0); }
            // shear
            pc.color[0]=0.6f; pc.color[1]=0.85f; pc.color[2]=1.0f; pc.color[3]=1.0f; vkCmdPushConstants(cmd, pipe_line_.layout, VK_SHADER_STAGE_VERTEX_BIT, 0, sizeof(PC), &pc);
            if (line_shear_count_>0) { vkCmdBindIndexBuffer(cmd, line_shear_.buf, 0, VK_INDEX_TYPE_UINT32); vkCmdDrawIndexed(cmd, line_shear_count_, 1, 0, 0, 0); }
            // bend
            pc.color[0]=1.0f; pc.color[1]=0.78f; pc.color[2]=0.4f; pc.color[3]=1.0f; vkCmdPushConstants(cmd, pipe_line_.layout, VK_SHADER_STAGE_VERTEX_BIT, 0, sizeof(PC), &pc);
            if (line_bend_count_>0) { vkCmdBindIndexBuffer(cmd, line_bend_.buf, 0, VK_INDEX_TYPE_UINT32); vkCmdDrawIndexed(cmd, line_bend_count_, 1, 0, 0, 0); }
        }
        vkCmdEndRendering(cmd);
        barrier_img(color.image, color.aspect, VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL, VK_IMAGE_LAYOUT_GENERAL, VK_PIPELINE_STAGE_2_COLOR_ATTACHMENT_OUTPUT_BIT, VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT, VK_ACCESS_2_COLOR_ATTACHMENT_WRITE_BIT, VK_ACCESS_2_MEMORY_READ_BIT|VK_ACCESS_2_MEMORY_WRITE_BIT);
    }

    void on_imgui(const EngineContext& eng, const FrameContext&) override {
        auto* host = static_cast<vv_ui::TabsHost*>(eng.services);
        if (host) {
            host->add_overlay([this]{ cam_.imgui_draw_nav_overlay_space_tint(); });
            host->add_overlay([this]{ cam_.imgui_draw_mini_axis_gizmo(); });
        }
        if (!host) return;
        host->add_tab("Cloth (XPBD)", [this]{
            ImGui::Text("XPBD cloth (CPU sim + Vulkan draw)"); ImGui::Separator();
            ImGui::Checkbox("Simulate", &params_.simulate); ImGui::SameLine(); if (ImGui::Button("Step")) { step_sim_(std::clamp<float>((float)params_.fixed_dt, 1.f/600.f, 1.f/30.f)); }
            ImGui::SameLine(); if (ImGui::Button("Reset")) { cloth_.build_grid(params_.grid_x, params_.grid_y, params_.spacing); apply_compliance_(); recenter_cloth_at_origin_(); rebuild_indices_only_(); update_scene_bounds_(); cam_.frame_scene(1.12f); }
            ImGui::RadioButton("Mesh", &params_.render_mode, 0); ImGui::SameLine(); ImGui::RadioButton("Vertices", &params_.render_mode, 1); ImGui::SameLine(); ImGui::RadioButton("Constraints", &params_.render_mode, 2);
            ImGui::SliderFloat("Point Size", &params_.point_size, 1.0f, 12.0f);
            ImGui::SliderFloat("Fixed dt (s)", &params_.fixed_dt, 1.0f/240.0f, 1.0f/30.0f, "%.4f");
            ImGui::SliderInt("Substeps", &params_.substeps, 1, 8); ImGui::SliderInt("Iterations", &params_.iterations, 1, 40);
            ImGui::SliderFloat("Damping", &params_.damping, 0.0f, 1.0f); ImGui::SliderFloat3("Gravity", &params_.gravity.x, -30.0f, 30.0f);
            ImGui::Separator(); ImGui::SliderFloat("Comp struct", &params_.comp_struct, 0.0f, 0.01f, "%.5f"); ImGui::SliderFloat("Comp shear", &params_.comp_shear, 0.0f, 0.01f, "%.5f"); ImGui::SliderFloat("Comp bend", &params_.comp_bend, 0.0f, 0.05f, "%.5f"); if (ImGui::Button("Apply compliance")) apply_compliance_();
            ImGui::Separator(); ImGui::InputInt("Grid X", &params_.grid_x); ImGui::SameLine(); ImGui::InputInt("Grid Y", &params_.grid_y); ImGui::SliderFloat("Spacing", &params_.spacing, 0.02f, 0.2f);
            if (ImGui::Button("Rebuild Grid")) { cloth_.build_grid(params_.grid_x, params_.grid_y, params_.spacing); apply_compliance_(); recenter_cloth_at_origin_(); rebuild_all_buffers_(); update_scene_bounds_(); cam_.frame_scene(1.12f); }
            ImGui::SameLine(); if (ImGui::Button("Frame Cloth")) { update_scene_bounds_(); cam_.frame_scene(1.12f); }
        });
        host->add_tab("Camera", [this]{ cam_.imgui_panel_contents(); });
    }

private:
    struct Params { bool simulate{false}; float fixed_dt{1.0f/120.0f}; int substeps{2}; int iterations{10}; float damping{0.02f}; vv::float3 gravity{0.0f,-9.8f,0.0f}; int grid_x{20}, grid_y{20}; float spacing{0.06f}; float comp_struct{0.0f}; float comp_shear{0.0f}; float comp_bend{0.005f}; int render_mode{0}; float point_size{5.0f}; } params_{};

    vv::CameraService cam_{}; ClothXPBD cloth_{}; double sim_accum_{0.0}; int vp_w_{0}, vp_h_{0};

    struct GpuBuffer { VkBuffer buf{}; VmaAllocation alloc{}; void* mapped{}; size_t size{}; };
    GpuBuffer pos_buf_{}; // vec3 positions
    GpuBuffer tri_idx_{}; uint32_t tri_count_{0};
    GpuBuffer line_struct_{}; uint32_t line_struct_count_{0};
    GpuBuffer line_shear_{};  uint32_t line_shear_count_{0};
    GpuBuffer line_bend_{};   uint32_t line_bend_count_{0};

    struct Pipeline { VkPipeline pipeline{}; VkPipelineLayout layout{}; };
    Pipeline pipe_tri_{}, pipe_line_{}, pipe_point_{}; VkFormat color_fmt_{VK_FORMAT_B8G8R8A8_UNORM}; VkFormat depth_fmt_{VK_FORMAT_D32_SFLOAT}; VkDevice dev_{VK_NULL_HANDLE}; EngineContext eng_{};

    void apply_compliance_(){ for (auto& e : cloth_.edges){ if (e.type==ClothXPBD::Edge::Structural) e.compliance = params_.comp_struct; else if (e.type==ClothXPBD::Edge::Shear) e.compliance = params_.comp_shear; else e.compliance = params_.comp_bend; } }

    void recenter_cloth_at_origin_(){ if (cloth_.x.empty()) return; vv::float3 mn = cloth_.x[0], mx = cloth_.x[0]; for(const auto& p : cloth_.x){ mn.x=std::min(mn.x,p.x); mn.y=std::min(mn.y,p.y); mn.z=std::min(mn.z,p.z); mx.x=std::max(mx.x,p.x); mx.y=std::max(mx.y,p.y); mx.z=std::max(mx.z,p.z);} vv::float3 c{(mn.x+mx.x)*0.5f,(mn.y+mx.y)*0.5f,(mn.z+mx.z)*0.5f}; for(auto& p: cloth_.x){ p.x-=c.x; p.y-=c.y; p.z-=c.z; } }

    void update_scene_bounds_(){ if (cloth_.x.empty()) { cam_.set_scene_bounds(vv::BoundingBox{}); return; } vv::float3 mn=cloth_.x[0], mx=cloth_.x[0]; for(const auto& p: cloth_.x){ mn.x=std::min(mn.x,p.x); mn.y=std::min(mn.y,p.y); mn.z=std::min(mn.z,p.z); mx.x=std::max(mx.x,p.x); mx.y=std::max(mx.y,p.y); mx.z=std::max(mx.z,p.z);} mn.z-=0.2f; mx.z+=0.2f; cam_.set_scene_bounds(vv::BoundingBox{.min=mn,.max=mx,.valid=true}); }

    void step_sim_(float dt){ std::vector<vv::float3> x_prev = cloth_.x; for(size_t i=0;i<cloth_.x.size();++i){ if (cloth_.inv_m[i]==0.0f) continue; cloth_.v[i] = cloth_.v[i] + params_.gravity * dt; cloth_.x[i] = cloth_.x[i] + cloth_.v[i]*dt; } int sub=std::max(1,params_.substeps); int iters=std::max(1,params_.iterations); float subdt=dt/float(sub); for(int s=0;s<sub;++s){ for(auto& e: cloth_.edges) e.lambda=0.0f; for(int it=0; it<iters; ++it){ solve_distance_batch_(subdt); } } for(size_t i=0;i<cloth_.x.size();++i){ if (cloth_.inv_m[i]==0.0f) { cloth_.v[i]={0,0,0}; continue; } cloth_.v[i] = (cloth_.x[i] - x_prev[i]) / dt; cloth_.v[i] = cloth_.v[i] * std::max(0.0f, 1.0f - params_.damping); } }

    void solve_distance_batch_(float dt){ const float eps=1e-6f; for(auto& e: cloth_.edges){ int i=e.i, j=e.j; float wi=cloth_.inv_m[i], wj=cloth_.inv_m[j]; vv::float3 d=cloth_.x[i]-cloth_.x[j]; float len=vv::length(d); if (len<eps) continue; vv::float3 n=d*(1.0f/len); float C=len-e.rest; float alpha=e.compliance/(dt*dt); float denom=wi+wj+alpha; if (denom<eps) continue; float dlambda=-(C+alpha*e.lambda)/denom; e.lambda+=dlambda; vv::float3 corr=n*dlambda; if (wi>0.0f) cloth_.x[i]=cloth_.x[i]+corr*wi; if (wj>0.0f) cloth_.x[j]=cloth_.x[j]-corr*wj; } }

    // GPU helpers
    void create_buffer_(VkDeviceSize sz, VkBufferUsageFlags usage, VmaMemoryUsage memUsage, bool mapped, GpuBuffer& out){
        VkBufferCreateInfo bi{VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO}; bi.size=sz; bi.usage=usage; bi.sharingMode=VK_SHARING_MODE_EXCLUSIVE;
        VmaAllocationCreateInfo ai{}; ai.usage = memUsage; ai.flags = mapped? VMA_ALLOCATION_CREATE_HOST_ACCESS_SEQUENTIAL_WRITE_BIT | VMA_ALLOCATION_CREATE_MAPPED_BIT : 0;
        VK_CHECK(vmaCreateBuffer(eng_.allocator, &bi, &ai, &out.buf, &out.alloc, nullptr)); out.size=(size_t)sz; out.mapped=nullptr;
        if (mapped) { vmaMapMemory(eng_.allocator, out.alloc, &out.mapped); }
    }
    void destroy_buffer_(GpuBuffer& b){ if (b.mapped) { vmaUnmapMemory(eng_.allocator, b.alloc); b.mapped=nullptr; } if (b.buf) vmaDestroyBuffer(eng_.allocator, b.buf, b.alloc); b = {}; }

    void build_gpu_buffers_(){
        // positions
        create_buffer_(cloth_.x.size()*sizeof(vv::float3), VK_BUFFER_USAGE_VERTEX_BUFFER_BIT, VMA_MEMORY_USAGE_AUTO, true, pos_buf_);
        if (pos_buf_.mapped) std::memcpy(pos_buf_.mapped, cloth_.x.data(), cloth_.x.size()*sizeof(vv::float3));
        rebuild_indices_only_();
    }

    void rebuild_indices_only_(){
        // triangles from grid
        std::vector<uint32_t> idx; idx.reserve((cloth_.nx-1)*(cloth_.ny-1)*6);
        auto id = [&](int x,int y){ return (uint32_t)(y*cloth_.nx + x); };
        for(int y=0;y<cloth_.ny-1;++y){ for(int x=0;x<cloth_.nx-1;++x){ uint32_t a=id(x,y), b=id(x+1,y), c=id(x,y+1), d=id(x+1,y+1); // two tris: a,b,d and a,d,c (CCW in +Z toward viewer)
                idx.push_back(a); idx.push_back(b); idx.push_back(d); idx.push_back(a); idx.push_back(d); idx.push_back(c);
        }}
        tri_count_ = (uint32_t)idx.size();
        if (!tri_idx_.buf) create_buffer_(idx.size()*sizeof(uint32_t), VK_BUFFER_USAGE_INDEX_BUFFER_BIT, VMA_MEMORY_USAGE_AUTO, true, tri_idx_);
        else if (tri_idx_.size < idx.size()*sizeof(uint32_t)) { destroy_buffer_(tri_idx_); create_buffer_(idx.size()*sizeof(uint32_t), VK_BUFFER_USAGE_INDEX_BUFFER_BIT, VMA_MEMORY_USAGE_AUTO, true, tri_idx_); }
        std::memcpy(tri_idx_.mapped, idx.data(), idx.size()*sizeof(uint32_t));
        // lines by type
        auto fill_lines = [&](ClothXPBD::Edge::Type t, GpuBuffer& dst, uint32_t& count){ std::vector<uint32_t> L; L.reserve(cloth_.edges.size()*2); for(const auto& e: cloth_.edges){ if (e.type!=t) continue; L.push_back((uint32_t)e.i); L.push_back((uint32_t)e.j);} count=(uint32_t)L.size(); if (!dst.buf) create_buffer_(L.size()*sizeof(uint32_t), VK_BUFFER_USAGE_INDEX_BUFFER_BIT, VMA_MEMORY_USAGE_AUTO, true, dst); else if (dst.size < L.size()*sizeof(uint32_t)) { destroy_buffer_(dst); create_buffer_(L.size()*sizeof(uint32_t), VK_BUFFER_USAGE_INDEX_BUFFER_BIT, VMA_MEMORY_USAGE_AUTO, true, dst);} if (!L.empty()) std::memcpy(dst.mapped, L.data(), L.size()*sizeof(uint32_t)); };
        fill_lines(ClothXPBD::Edge::Structural, line_struct_, line_struct_count_);
        fill_lines(ClothXPBD::Edge::Shear,      line_shear_,  line_shear_count_);
        fill_lines(ClothXPBD::Edge::Bend,       line_bend_,   line_bend_count_);
    }

    void rebuild_all_buffers_(){ destroy_buffer_(pos_buf_); destroy_buffer_(tri_idx_); destroy_buffer_(line_struct_); destroy_buffer_(line_shear_); destroy_buffer_(line_bend_); build_gpu_buffers_(); }

    void build_pipelines_(){
        std::string dir(SHADER_OUTPUT_DIR); VkShaderModule vs = make_shader(dev_, load_spv(dir+"/cloth.vert.spv")); VkShaderModule fs = make_shader(dev_, load_spv(dir+"/cloth.frag.spv"));
        VkPipelineShaderStageCreateInfo st[2]{}; for(int i=0;i<2;++i) st[i].sType=VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO; st[0].stage=VK_SHADER_STAGE_VERTEX_BIT; st[0].module=vs; st[0].pName="main"; st[1]=st[0]; st[1].stage=VK_SHADER_STAGE_FRAGMENT_BIT; st[1].module=fs;
        VkVertexInputBindingDescription bind{0, sizeof(vv::float3), VK_VERTEX_INPUT_RATE_VERTEX}; VkVertexInputAttributeDescription attr{0,0,VK_FORMAT_R32G32B32_SFLOAT,0};
        VkPipelineVertexInputStateCreateInfo vi{VK_STRUCTURE_TYPE_PIPELINE_VERTEX_INPUT_STATE_CREATE_INFO}; vi.vertexBindingDescriptionCount=1; vi.pVertexBindingDescriptions=&bind; vi.vertexAttributeDescriptionCount=1; vi.pVertexAttributeDescriptions=&attr;
        VkPipelineViewportStateCreateInfo vp{VK_STRUCTURE_TYPE_PIPELINE_VIEWPORT_STATE_CREATE_INFO}; vp.viewportCount=1; vp.scissorCount=1;
        VkPipelineRasterizationStateCreateInfo rs{VK_STRUCTURE_TYPE_PIPELINE_RASTERIZATION_STATE_CREATE_INFO}; rs.polygonMode=VK_POLYGON_MODE_FILL; rs.cullMode=VK_CULL_MODE_NONE; rs.frontFace=VK_FRONT_FACE_COUNTER_CLOCKWISE; rs.lineWidth=1.0f;
        VkPipelineMultisampleStateCreateInfo ms{VK_STRUCTURE_TYPE_PIPELINE_MULTISAMPLE_STATE_CREATE_INFO}; ms.rasterizationSamples=VK_SAMPLE_COUNT_1_BIT;
        VkPipelineDepthStencilStateCreateInfo ds{VK_STRUCTURE_TYPE_PIPELINE_DEPTH_STENCIL_STATE_CREATE_INFO}; ds.depthTestEnable=VK_TRUE; ds.depthWriteEnable=VK_TRUE; ds.depthCompareOp=VK_COMPARE_OP_LESS;
        VkPipelineColorBlendAttachmentState ba{}; ba.colorWriteMask=0xF; VkPipelineColorBlendStateCreateInfo cb{VK_STRUCTURE_TYPE_PIPELINE_COLOR_BLEND_STATE_CREATE_INFO}; cb.attachmentCount=1; cb.pAttachments=&ba;
        const VkDynamicState dyns[2] = { VK_DYNAMIC_STATE_VIEWPORT, VK_DYNAMIC_STATE_SCISSOR }; VkPipelineDynamicStateCreateInfo dsi{VK_STRUCTURE_TYPE_PIPELINE_DYNAMIC_STATE_CREATE_INFO}; dsi.dynamicStateCount=2; dsi.pDynamicStates=dyns;
        // Push constants layout (96 bytes)
        VkPushConstantRange pcr{VK_SHADER_STAGE_VERTEX_BIT, 0, 96}; VkPipelineLayoutCreateInfo lci{VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO}; lci.pushConstantRangeCount=1; lci.pPushConstantRanges=&pcr; VK_CHECK(vkCreatePipelineLayout(dev_, &lci, nullptr, &pipe_tri_.layout));
        // share same layout for others
        pipe_line_.layout = pipe_tri_.layout; pipe_point_.layout = pipe_tri_.layout;
        VkPipelineRenderingCreateInfo rinfo{VK_STRUCTURE_TYPE_PIPELINE_RENDERING_CREATE_INFO}; rinfo.colorAttachmentCount=1; rinfo.pColorAttachmentFormats=&color_fmt_; rinfo.depthAttachmentFormat=depth_fmt_;
        auto make_pipeline = [&](VkPrimitiveTopology topo, Pipeline& out){ VkPipelineInputAssemblyStateCreateInfo ia{VK_STRUCTURE_TYPE_PIPELINE_INPUT_ASSEMBLY_STATE_CREATE_INFO}; ia.topology=topo; VkGraphicsPipelineCreateInfo pci{VK_STRUCTURE_TYPE_GRAPHICS_PIPELINE_CREATE_INFO}; pci.pNext=&rinfo; pci.stageCount=2; pci.pStages=st; pci.pVertexInputState=&vi; pci.pInputAssemblyState=&ia; pci.pViewportState=&vp; pci.pRasterizationState=&rs; pci.pMultisampleState=&ms; pci.pDepthStencilState=&ds; pci.pColorBlendState=&cb; pci.pDynamicState=&dsi; pci.layout=pipe_tri_.layout; VK_CHECK(vkCreateGraphicsPipelines(dev_, VK_NULL_HANDLE, 1, &pci, nullptr, &out.pipeline)); };
        make_pipeline(VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST, pipe_tri_);
        make_pipeline(VK_PRIMITIVE_TOPOLOGY_LINE_LIST, pipe_line_);
        make_pipeline(VK_PRIMITIVE_TOPOLOGY_POINT_LIST, pipe_point_);
        vkDestroyShaderModule(dev_, vs, nullptr); vkDestroyShaderModule(dev_, fs, nullptr);
    }

    void destroy_pipelines_(){ if (pipe_tri_.pipeline) vkDestroyPipeline(dev_, pipe_tri_.pipeline, nullptr); if (pipe_line_.pipeline) vkDestroyPipeline(dev_, pipe_line_.pipeline, nullptr); if (pipe_point_.pipeline) vkDestroyPipeline(dev_, pipe_point_.pipeline, nullptr); if (pipe_tri_.layout) vkDestroyPipelineLayout(dev_, pipe_tri_.layout, nullptr); pipe_tri_={}; pipe_line_={}; pipe_point_={}; }
    void destroy_gpu_buffers_(){ destroy_buffer_(pos_buf_); destroy_buffer_(tri_idx_); destroy_buffer_(line_struct_); destroy_buffer_(line_shear_); destroy_buffer_(line_bend_); }
};

int main(){ try{ VulkanEngine e; e.configure_window(1280, 720, "ex10_xpbd_cloth"); e.set_renderer(std::make_unique<XPBDClothRenderer>()); e.init(); e.run(); e.cleanup(); } catch(const std::exception& ex){ std::fprintf(stderr, "Fatal: %s\n", ex.what()); return 1; } return 0; }
