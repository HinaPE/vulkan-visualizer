#include "vk_engine.h"
#include "vv_camera.h"
#include <imgui.h>
#include <vulkan/vulkan.h>

#include <vector>
#include <string>
#include <cmath>
#include <algorithm>
#include <stdexcept>

#ifndef VK_CHECK
#define VK_CHECK(x) do{VkResult r=(x); if(r!=VK_SUCCESS) throw std::runtime_error("Vulkan error: "+std::to_string(r)); }while(false)
#endif

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
        for(int iy=0; iy<ny; ++iy){
            for(int ix=0; ix<nx; ++ix){
                vv::float3 p{ origin.x + ix*spacing, origin.y - iy*spacing, origin.z };
                x[idx(ix,iy)] = p;
            }
        }
        // Pin top corners (iy=0)
        inv_m[idx(0,0)] = 0.0f; inv_m[idx(nx-1,0)] = 0.0f;
        // Build edges
        edges.clear(); edges.reserve(nx*ny*4);
        auto add_edge = [&](int a, int b, float comp, Edge::Type t){ vv::float3 d = x[b]-x[a]; float rl = vv::length(d); edges.push_back(Edge{a,b,rl,comp,0.0f,t}); };
        float comp_struct = 0.0f, comp_shear = 0.0f, comp_bend = 0.001f;
        for(int iy=0; iy<ny; ++iy){
            for(int ix=0; ix<nx; ++ix){
                int a = idx(ix,iy);
                if (ix+1<nx) add_edge(a, idx(ix+1,iy), comp_struct, Edge::Structural);
                if (iy+1<ny) add_edge(a, idx(ix,iy+1), comp_struct, Edge::Structural);
                if (ix+1<nx && iy+1<ny){ add_edge(a, idx(ix+1,iy+1), comp_shear, Edge::Shear); add_edge(idx(ix+1,iy), idx(ix,iy+1), comp_shear, Edge::Shear); }
                if (ix+2<nx) add_edge(a, idx(ix+2,iy), comp_bend, Edge::Bend);
                if (iy+2<ny) add_edge(a, idx(ix,iy+2), comp_bend, Edge::Bend);
            }
        }
    }
};

class XPBDClothRenderer : public IRenderer {
public:
    void get_capabilities(const EngineContext&, RendererCaps& c) override {
        c = RendererCaps{};
        c.enable_imgui = true;
        c.presentation_mode = PresentationMode::EngineBlit;
        c.color_attachments = { AttachmentRequest{ .name = "color", .format = VK_FORMAT_B8G8R8A8_UNORM } };
        c.presentation_attachment = "color";
        c.depth_attachment = AttachmentRequest{ .name = "depth", .format = c.preferred_depth_format, .usage = VK_IMAGE_USAGE_DEPTH_STENCIL_ATTACHMENT_BIT, .samples = VK_SAMPLE_COUNT_1_BIT, .aspect = VK_IMAGE_ASPECT_DEPTH_BIT, .initial_layout = VK_IMAGE_LAYOUT_UNDEFINED };
        c.uses_depth = VK_TRUE;
    }

    void initialize(const EngineContext&, const RendererCaps&, const FrameContext&) override {
        cam_.set_mode(vv::CameraMode::Orbit);
        auto s = cam_.state();
        s.target = {0,0,0}; s.distance = 2.0f; s.pitch_deg = 15.0f; s.yaw_deg = -120.0f; s.znear = 0.01f; s.zfar = 100.0f;
        cam_.set_state(s);
        cloth_.build_grid(params_.grid_x, params_.grid_y, params_.spacing);
        apply_compliance_();
        recenter_cloth_at_origin_();
        update_scene_bounds_();
        cam_.frame_scene(1.12f);
        sim_accum_ = 0.0;
    }

    void destroy(const EngineContext&, const RendererCaps&) override {}

    void update(const EngineContext&, const FrameContext& f) override {
        cam_.update(f.dt_sec, static_cast<int>(f.extent.width), static_cast<int>(f.extent.height));
        // cache viewport for event remap
        vp_w_ = static_cast<int>(f.extent.width);
        vp_h_ = static_cast<int>(f.extent.height);
        if (!params_.simulate) return;
        sim_accum_ += f.dt_sec;
        const double fixed = std::clamp<double>(params_.fixed_dt, 1.0/600.0, 1.0/30.0);
        int maxSteps = 4; while(sim_accum_ >= fixed && maxSteps--){ step_sim_((float)fixed); sim_accum_ -= fixed; }
    }

    void on_event(const SDL_Event& e, const EngineContext& eng, const FrameContext* f) override {
        SDL_Event ev = e;
        const int H = (f ? (int)f->extent.height : vp_h_);
        switch (ev.type) {
            case SDL_EVENT_MOUSE_MOTION:
                if (H > 0) { ev.motion.y = H - 1 - ev.motion.y; }
                ev.motion.yrel = -ev.motion.yrel; // invert vertical delta
                break;
            case SDL_EVENT_MOUSE_BUTTON_DOWN:
            case SDL_EVENT_MOUSE_BUTTON_UP:
                if (H > 0) { ev.button.y = H - 1 - ev.button.y; }
                break;
            default: break;
        }
        cam_.handle_event(ev, &eng, f);
    }

    void record_graphics(VkCommandBuffer cmd, const EngineContext&, const FrameContext& f) override {
        if (f.color_attachments.empty()) return;
        const auto& color = f.color_attachments.front();
        const auto* depth = f.depth_attachment;
        auto barrier_img = [&](VkImage img, VkImageAspectFlags aspect, VkImageLayout oldL, VkImageLayout newL, VkPipelineStageFlags2 src, VkPipelineStageFlags2 dst, VkAccessFlags2 sa, VkAccessFlags2 da){
            VkImageMemoryBarrier2 b{VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER_2}; b.srcStageMask=src; b.dstStageMask=dst; b.srcAccessMask=sa; b.dstAccessMask=da; b.oldLayout=oldL; b.newLayout=newL; b.image=img; b.subresourceRange={aspect,0,1,0,1};
            VkDependencyInfo di{VK_STRUCTURE_TYPE_DEPENDENCY_INFO}; di.imageMemoryBarrierCount=1; di.pImageMemoryBarriers=&b; vkCmdPipelineBarrier2(cmd, &di);
        };
        barrier_img(color.image, color.aspect, VK_IMAGE_LAYOUT_GENERAL, VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL, VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT, VK_PIPELINE_STAGE_2_COLOR_ATTACHMENT_OUTPUT_BIT, VK_ACCESS_2_MEMORY_WRITE_BIT, VK_ACCESS_2_COLOR_ATTACHMENT_WRITE_BIT);
        if (depth) barrier_img(depth->image, depth->aspect, VK_IMAGE_LAYOUT_UNDEFINED, VK_IMAGE_LAYOUT_DEPTH_ATTACHMENT_OPTIMAL, VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT, VK_PIPELINE_STAGE_2_EARLY_FRAGMENT_TESTS_BIT, 0, VK_ACCESS_2_DEPTH_STENCIL_ATTACHMENT_WRITE_BIT);
        VkClearValue clear_color{.color={{0.05f,0.06f,0.07f,1.0f}}}; VkClearValue clear_depth{.depthStencil={1.0f,0}};
        VkRenderingAttachmentInfo ca{VK_STRUCTURE_TYPE_RENDERING_ATTACHMENT_INFO}; ca.imageView=color.view; ca.imageLayout=VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL; ca.loadOp=VK_ATTACHMENT_LOAD_OP_CLEAR; ca.storeOp=VK_ATTACHMENT_STORE_OP_STORE; ca.clearValue=clear_color;
        VkRenderingAttachmentInfo da{}; if (depth){ da.sType=VK_STRUCTURE_TYPE_RENDERING_ATTACHMENT_INFO; da.imageView=depth->view; da.imageLayout=VK_IMAGE_LAYOUT_DEPTH_ATTACHMENT_OPTIMAL; da.loadOp=VK_ATTACHMENT_LOAD_OP_CLEAR; da.storeOp=VK_ATTACHMENT_STORE_OP_STORE; da.clearValue=clear_depth; }
        VkRenderingInfo ri{VK_STRUCTURE_TYPE_RENDERING_INFO}; ri.renderArea={{0,0}, f.extent}; ri.layerCount=1; ri.colorAttachmentCount=1; ri.pColorAttachments=&ca; ri.pDepthAttachment = depth? &da : nullptr;
        vkCmdBeginRendering(cmd, &ri);
        // We only clear; visualization uses ImGui overlay below
        vkCmdEndRendering(cmd);
        barrier_img(color.image, color.aspect, VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL, VK_IMAGE_LAYOUT_GENERAL, VK_PIPELINE_STAGE_2_COLOR_ATTACHMENT_OUTPUT_BIT, VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT, VK_ACCESS_2_COLOR_ATTACHMENT_WRITE_BIT, VK_ACCESS_2_MEMORY_READ_BIT | VK_ACCESS_2_MEMORY_WRITE_BIT);
    }

    void on_imgui(const EngineContext& eng, const FrameContext& f) override {
        auto* host = static_cast<vv_ui::TabsHost*>(eng.services);
        if (host) {
            host->add_overlay([this,&f]{ draw_overlay_(f); });
        }
        if (!host) return;
        host->add_tab("Cloth (XPBD)", [this]{
            ImGui::Text("XPBD cloth (CPU) — structural/shear/bend distance constraints");
            ImGui::Separator();
            ImGui::Checkbox("Simulate", &params_.simulate); ImGui::SameLine(); if (ImGui::Button("Step")) { step_sim_(std::clamp<float>((float)params_.fixed_dt, 1.f/600.f, 1.f/30.f)); }
            ImGui::SameLine(); if (ImGui::Button("Reset")) { cloth_.build_grid(params_.grid_x, params_.grid_y, params_.spacing); apply_compliance_(); recenter_cloth_at_origin_(); update_scene_bounds_(); cam_.frame_scene(1.12f); }
            ImGui::SameLine(); if (ImGui::Button("Frame Cloth")) { update_scene_bounds_(); cam_.frame_scene(1.12f); }
            ImGui::SliderFloat("Fixed dt (s)", &params_.fixed_dt, 1.0f/240.0f, 1.0f/30.0f, "%.4f");
            ImGui::SliderInt("Substeps", &params_.substeps, 1, 8);
            ImGui::SliderInt("Iterations", &params_.iterations, 1, 40);
            ImGui::SliderFloat("Damping", &params_.damping, 0.0f, 1.0f);
            ImGui::SliderFloat3("Gravity", &params_.gravity.x, -30.0f, 30.0f);
            ImGui::Separator();
            ImGui::SliderFloat("Compliance (struct)", &params_.comp_struct, 0.0f, 0.01f, "%.5f");
            ImGui::SliderFloat("Compliance (shear)", &params_.comp_shear, 0.0f, 0.01f, "%.5f");
            ImGui::SliderFloat("Compliance (bend)",  &params_.comp_bend,  0.0f, 0.05f, "%.5f");
            if (ImGui::Button("Apply compliance")) apply_compliance_();
            ImGui::Separator();
            ImGui::InputInt("Grid X", &params_.grid_x); ImGui::SameLine(); ImGui::InputInt("Grid Y", &params_.grid_y);
            ImGui::SliderFloat("Spacing", &params_.spacing, 0.02f, 0.2f);
            if (ImGui::Button("Rebuild Grid")) { cloth_.build_grid(params_.grid_x, params_.grid_y, params_.spacing); apply_compliance_(); recenter_cloth_at_origin_(); update_scene_bounds_(); cam_.frame_scene(1.12f); }
            ImGui::Separator();
            ImGui::Checkbox("Show Vertices", &params_.show_vertices); ImGui::SameLine(); ImGui::Checkbox("Show Constraints", &params_.show_constraints);
            ImGui::Checkbox("Show Structural", &params_.show_struct); ImGui::SameLine(); ImGui::Checkbox("Show Shear", &params_.show_shear); ImGui::SameLine(); ImGui::Checkbox("Show Bend", &params_.show_bend);
        });
        host->add_tab("Camera", [this]{ cam_.imgui_panel_contents(); });
    }

private:
    struct Params {
        bool simulate{false};
        float fixed_dt{1.0f/120.0f};
        int   substeps{2};
        int   iterations{10};
        float damping{0.02f};
        vv::float3 gravity{0.0f, -9.8f, 0.0f};
        int grid_x{20}, grid_y{20};
        float spacing{0.06f};
        float comp_struct{0.0f};
        float comp_shear{0.0f};
        float comp_bend{0.005f};
        bool show_vertices{true};
        bool show_constraints{true};
        bool show_struct{true};
        bool show_shear{true};
        bool show_bend{true};
    } params_{};

    vv::CameraService cam_{};
    ClothXPBD cloth_{};
    double sim_accum_{0.0};
    int vp_w_{0}, vp_h_{0};

    void apply_compliance_(){
        for (auto& e : cloth_.edges){
            if (e.type==ClothXPBD::Edge::Structural) e.compliance = params_.comp_struct;
            else if (e.type==ClothXPBD::Edge::Shear) e.compliance = params_.comp_shear;
            else e.compliance = params_.comp_bend;
        }
    }

    void recenter_cloth_at_origin_(){
        if (cloth_.x.empty()) return;
        vv::float3 mn = cloth_.x[0], mx = cloth_.x[0];
        for(const auto& p : cloth_.x){
            mn.x = std::min(mn.x, p.x); mn.y = std::min(mn.y, p.y); mn.z = std::min(mn.z, p.z);
            mx.x = std::max(mx.x, p.x); mx.y = std::max(mx.y, p.y); mx.z = std::max(mx.z, p.z);
        }
        vv::float3 c{ (mn.x+mx.x)*0.5f, (mn.y+mx.y)*0.5f, (mn.z+mx.z)*0.5f };
        for(auto& p : cloth_.x){ p.x -= c.x; p.y -= c.y; p.z -= c.z; }
        // velocities unchanged; keep pins as-is (inv_m=0) relative to new positions
    }

    // New: compute bbox from cloth vertices and set scene bounds
    void update_scene_bounds_(){
        if (cloth_.x.empty()) { cam_.set_scene_bounds(vv::BoundingBox{}); return; }
        vv::float3 mn = cloth_.x[0];
        vv::float3 mx = cloth_.x[0];
        for(const auto& p : cloth_.x){
            mn.x = std::min(mn.x, p.x); mn.y = std::min(mn.y, p.y); mn.z = std::min(mn.z, p.z);
            mx.x = std::max(mx.x, p.x); mx.y = std::max(mx.y, p.y); mx.z = std::max(mx.z, p.z);
        }
        // add small thickness along Z to avoid too-thin frustum
        mn.z -= 0.2f; mx.z += 0.2f;
        cam_.set_scene_bounds(vv::BoundingBox{ .min = mn, .max = mx, .valid = true });
    }

    void step_sim_(float dt){
        // Save previous positions for velocity update later
        std::vector<vv::float3> x_prev = cloth_.x;
        // external forces
        for(size_t i=0;i<cloth_.x.size();++i){
            if (cloth_.inv_m[i]==0.0f) continue; // pinned
            cloth_.v[i] = cloth_.v[i] + params_.gravity * dt;
            cloth_.x[i] = cloth_.x[i] + cloth_.v[i]*dt; // predict
        }
        int sub = std::max(1, params_.substeps);
        int iters = std::max(1, params_.iterations);
        float subdt = dt / float(sub);
        for(int s=0;s<sub;++s){
            for(auto& e: cloth_.edges) e.lambda = 0.0f;
            for(int it=0; it<iters; ++it){ solve_distance_batch_(subdt); }
        }
        // velocities from displacement over dt + damping
        for(size_t i=0;i<cloth_.x.size();++i){
            if (cloth_.inv_m[i]==0.0f) { cloth_.v[i] = {0,0,0}; continue; }
            cloth_.v[i] = (cloth_.x[i] - x_prev[i]) / dt;
            cloth_.v[i] = cloth_.v[i] * std::max(0.0f, 1.0f - params_.damping);
        }
    }

    void solve_distance_batch_(float dt){
        const float eps = 1e-6f;
        for(auto& e : cloth_.edges){
            int i = e.i, j = e.j; float wi = cloth_.inv_m[i], wj = cloth_.inv_m[j];
            vv::float3 d = cloth_.x[i] - cloth_.x[j];
            float len = vv::length(d);
            if (len < eps) continue;
            vv::float3 n = d * (1.0f/len);
            float C = len - e.rest;
            float alpha = e.compliance / (dt*dt);
            float denom = wi + wj + alpha;
            if (denom < eps) continue;
            float dlambda = -(C + alpha*e.lambda) / denom;
            e.lambda += dlambda;
            vv::float3 corr = n * dlambda;
            if (wi>0.0f) cloth_.x[i] = cloth_.x[i] + corr * wi;
            if (wj>0.0f) cloth_.x[j] = cloth_.x[j] - corr * wj;
        }
    }

    void draw_overlay_(const FrameContext& f){
        // Project and draw constraints/points on ImGui foreground
        const vv::float4x4 V = cam_.view_matrix();
        const vv::float4x4 P = cam_.proj_matrix();
        auto* dl = ImGui::GetForegroundDrawList();
        ImGuiIO& io = ImGui::GetIO();
        const float sfx = (io.DisplayFramebufferScale.x != 0.0f) ? io.DisplayFramebufferScale.x : 1.0f;
        const float sfy = (io.DisplayFramebufferScale.y != 0.0f) ? io.DisplayFramebufferScale.y : 1.0f;
        ImGuiViewport* vp = ImGui::GetMainViewport();
        const ImVec2 base = vp ? vp->Pos : ImVec2(0,0);
        auto proj = [&](const vv::float3& p, ImVec2& out){
            float sx, sy; if (vv::project_to_screen(p, V, P, (int)f.extent.width, (int)f.extent.height, sx, sy)) {
                // Convert framebuffer pixels -> ImGui coordinates (DPI aware) and add viewport offset
                out = ImVec2(base.x + sx / sfx, base.y + sy / sfy);
                return true;
            }
            return false;
        };

        // Constraints
        if (params_.show_constraints){
            for(const auto& e: cloth_.edges){
                if ((e.type==ClothXPBD::Edge::Structural && !params_.show_struct) ||
                    (e.type==ClothXPBD::Edge::Shear && !params_.show_shear) ||
                    (e.type==ClothXPBD::Edge::Bend && !params_.show_bend)) continue;
                ImVec2 a,b; if (proj(cloth_.x[e.i], a) && proj(cloth_.x[e.j], b)){
                    ImU32 col = (e.type==ClothXPBD::Edge::Structural) ? IM_COL32(220,220,220,180)
                                : (e.type==ClothXPBD::Edge::Shear)     ? IM_COL32(180,220,255,160)
                                                                        : IM_COL32(255,200,120,140);
                    dl->AddLine(a, b, col, e.type==ClothXPBD::Edge::Bend? 1.0f : 2.0f);
                }
            }
        }
        // Vertices
        if (params_.show_vertices){
            for(size_t i=0;i<cloth_.x.size();++i){ ImVec2 p2; if (proj(cloth_.x[i], p2)){
                bool pinned = (cloth_.inv_m[i]==0.0f);
                ImU32 col = pinned? IM_COL32(255,80,80,255) : IM_COL32(240,240,240,255);
                float r = pinned? 4.0f : 3.0f;
                dl->AddCircleFilled(p2, r, col, 10);
            }}
        }
        // Mini axis gizmo
        cam_.imgui_draw_mini_axis_gizmo();
    }
};

int main(){
    try{
        VulkanEngine e; e.configure_window(1280, 720, "ex10_xpbd_cloth");
        e.set_renderer(std::make_unique<XPBDClothRenderer>());
        e.init(); e.run(); e.cleanup();
    } catch(const std::exception& ex){ std::fprintf(stderr, "Fatal: %s\n", ex.what()); return 1; }
    return 0;
}
