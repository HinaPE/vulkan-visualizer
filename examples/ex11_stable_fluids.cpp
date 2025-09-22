#include "vk_engine.h"
#include "vv_camera.h"
#include <vulkan/vulkan.h>
#include <vk_mem_alloc.h>
#include <cstdio>
#include <vector>
#include <string>
#include <fstream>
#include <stdexcept>
#include <cstring>

#ifndef VK_CHECK
#define VK_CHECK(x) do{VkResult r=(x); if(r!=VK_SUCCESS) throw std::runtime_error("Vulkan error: "+std::to_string(r)); }while(false)
#endif

static std::vector<char> load_spv(const std::string& p){ std::ifstream f(p, std::ios::binary | std::ios::ate); if(!f) throw std::runtime_error("open "+p); size_t s=(size_t)f.tellg(); f.seekg(0); std::vector<char> d(s); f.read(d.data(), (std::streamsize)s); return d; }
static VkShaderModule make_shader(VkDevice d, const std::vector<char>& b){ VkShaderModuleCreateInfo ci{VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO}; ci.codeSize=(uint32_t)b.size(); ci.pCode=(const uint32_t*)b.data(); VkShaderModule m{}; VK_CHECK(vkCreateShaderModule(d,&ci,nullptr,&m)); return m; }

struct Image3D { VkImage img{}; VkImageView view{}; VmaAllocation alloc{}; VkExtent3D extent{}; VkFormat fmt{}; };

class StableFluids final : public IRenderer {
public:
    void get_capabilities(const EngineContext&, RendererCaps& c) override {
        c = RendererCaps{};
        c.enable_imgui = true; // allow camera overlays similar to ex10
        c.presentation_mode = PresentationMode::EngineBlit;
        c.color_attachments = { AttachmentRequest{ .name = "color", .format = VK_FORMAT_R8G8B8A8_UNORM, .usage = VK_IMAGE_USAGE_STORAGE_BIT | VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT | VK_IMAGE_USAGE_TRANSFER_SRC_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT, .samples = VK_SAMPLE_COUNT_1_BIT, .aspect = VK_IMAGE_ASPECT_COLOR_BIT, .initial_layout = VK_IMAGE_LAYOUT_GENERAL } };
        c.presentation_attachment = "color";
    }

    void initialize(const EngineContext& e, const RendererCaps&, const FrameContext& f0) override {
        eng_ = e; dev_ = e.device; alloc_ = e.allocator; da_ = e.descriptorAllocator;
        create_all(f0.extent);
        create_pipelines_();
        // Setup camera like ex10 (orbit)
        vv::CameraState s = cam_.state(); s.mode = vv::CameraMode::Orbit; s.target = { (float)sim_w_*0.5f, (float)sim_h_*0.5f, (float)sim_d_*0.5f }; s.distance = std::max({sim_w_,sim_h_,sim_d_}) * 1.6f; s.yaw_deg = -35.0f; s.pitch_deg = 25.0f; s.znear=0.01f; s.zfar = std::max({sim_w_,sim_h_,sim_d_})*5.0f; cam_.set_state(s);
        vv::BoundingBox bb{ .min = {0,0,0}, .max = { (float)sim_w_, (float)sim_h_, (float)sim_d_ }, .valid = true }; cam_.set_scene_bounds(bb); cam_.frame_scene(1.08f);
    }
    void on_swapchain_ready(const EngineContext& e, const FrameContext& f) override { (void)e; recreate_for_extent_(f.extent); vv::BoundingBox bb{ .min = {0,0,0}, .max = { (float)sim_w_, (float)sim_h_, (float)sim_d_ }, .valid = true }; cam_.set_scene_bounds(bb); cam_.frame_scene(1.02f); }
    void on_swapchain_destroy(const EngineContext& e) override { (void)e; destroy_images_(); }

    void destroy(const EngineContext& e, const RendererCaps&) override {
        destroy_pipelines_();
        destroy_images_();
        eng_ = {}; dev_ = VK_NULL_HANDLE; alloc_ = nullptr; da_ = nullptr;
    }

    void update(const EngineContext&, const FrameContext& f) override {
        cam_.update(f.dt_sec, (int)f.extent.width, (int)f.extent.height);
    }

    void on_event(const SDL_Event& e, const EngineContext& eng, const FrameContext* f) override {
        cam_.handle_event(e, &eng, f);
    }

    void on_imgui(const EngineContext& eng, const FrameContext&) override {
        auto* host = static_cast<vv_ui::TabsHost*>(eng.services);
        if (!host) return;
        host->add_overlay([this]{ cam_.imgui_draw_nav_overlay_space_tint(); });
        host->add_overlay([this]{ cam_.imgui_draw_mini_axis_gizmo(); });
    }

    void record_compute(VkCommandBuffer cmd, const EngineContext&, const FrameContext& f) override {
        if (!images_ready_) return;

        if (!images_initialized_) {
            // One-time transition UNDEFINED->GENERAL and clear resources
            auto barrier_to_general = [&](VkImage img){
                VkImageMemoryBarrier2 b{VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER_2};
                b.srcStageMask = VK_PIPELINE_STAGE_2_TOP_OF_PIPE_BIT; b.dstStageMask = VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT;
                b.srcAccessMask = 0; b.dstAccessMask = VK_ACCESS_2_SHADER_WRITE_BIT | VK_ACCESS_2_SHADER_READ_BIT;
                b.oldLayout = VK_IMAGE_LAYOUT_UNDEFINED; b.newLayout = VK_IMAGE_LAYOUT_GENERAL;
                b.image = img; b.subresourceRange = { VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1 };
                VkDependencyInfo di{VK_STRUCTURE_TYPE_DEPENDENCY_INFO}; di.imageMemoryBarrierCount=1; di.pImageMemoryBarriers=&b; vkCmdPipelineBarrier2(cmd, &di);
            };
            barrier_to_general(velA_.img); barrier_to_general(velB_.img); barrier_to_general(denA_.img); barrier_to_general(denB_.img); barrier_to_general(pA_.img); barrier_to_general(pB_.img); barrier_to_general(div_.img);
            auto clear0 = [&](VkImage img){ VkClearColorValue z{}; z.float32[0]=0; z.float32[1]=0; z.float32[2]=0; z.float32[3]=0; VkImageSubresourceRange r{VK_IMAGE_ASPECT_COLOR_BIT,0,1,0,1}; vkCmdClearColorImage(cmd, img, VK_IMAGE_LAYOUT_GENERAL, &z, 1, &r); };
            clear0(velA_.img); clear0(velB_.img); clear0(denA_.img); clear0(denB_.img); clear0(pA_.img); clear0(pB_.img); clear0(div_.img);
            images_initialized_ = true; clear_pressure_ = true;
        }

        // Params
        float dt = (float)std::min<double>(f.dt_sec, 1.0/60.0);
        if (dt <= 0.0f) dt = 1.0f/60.0f;
        const float diss_vel = 0.999f; // slight damping
        const float diss_den = 0.9995f;
        const float force = 50.0f;
        uint32_t W = sim_w_, H = sim_h_, D = sim_d_;

        auto barrier_img = [&](VkImage img, VkImageAspectFlags aspect, VkPipelineStageFlags2 src, VkPipelineStageFlags2 dst, VkAccessFlags2 sa, VkAccessFlags2 da){
            VkImageMemoryBarrier2 b{VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER_2};
            b.srcStageMask=src; b.dstStageMask=dst; b.srcAccessMask=sa; b.dstAccessMask=da;
            b.oldLayout = VK_IMAGE_LAYOUT_GENERAL; b.newLayout = VK_IMAGE_LAYOUT_GENERAL;
            b.image=img; b.subresourceRange={aspect,0,1,0,1};
            VkDependencyInfo di{VK_STRUCTURE_TYPE_DEPENDENCY_INFO}; di.imageMemoryBarrierCount=1; di.pImageMemoryBarriers=&b; vkCmdPipelineBarrier2(cmd, &di);
        };

        auto bind_and_push8 = [&](VkPipeline p, VkPipelineLayout layout, VkDescriptorSet ds, float a,float b,float c,float d,float e,float g,float h,float k){
            vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_COMPUTE, p);
            vkCmdBindDescriptorSets(cmd, VK_PIPELINE_BIND_POINT_COMPUTE, layout, 0, 1, &ds, 0, nullptr);
            struct PC{ float x0,x1,x2,x3,x4,x5,x6,x7; } pc{a,b,c,d,e,g,h,k};
            vkCmdPushConstants(cmd, layout, VK_SHADER_STAGE_COMPUTE_BIT, 0, sizeof(PC), &pc);
        };

        // Inject source (velocity + density) near bottom-center of volume, upward (+Y)
        {
            update_ds_inject_();
            barrier_img(velA_.img, VK_IMAGE_ASPECT_COLOR_BIT, VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT, VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT, VK_ACCESS_2_MEMORY_READ_BIT|VK_ACCESS_2_MEMORY_WRITE_BIT, VK_ACCESS_2_SHADER_READ_BIT|VK_ACCESS_2_SHADER_WRITE_BIT);
            barrier_img(velB_.img, VK_IMAGE_ASPECT_COLOR_BIT, VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT, VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT, VK_ACCESS_2_MEMORY_READ_BIT|VK_ACCESS_2_MEMORY_WRITE_BIT, VK_ACCESS_2_SHADER_READ_BIT|VK_ACCESS_2_SHADER_WRITE_BIT);
            barrier_img(denA_.img, VK_IMAGE_ASPECT_COLOR_BIT, VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT, VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT, VK_ACCESS_2_MEMORY_READ_BIT|VK_ACCESS_2_MEMORY_WRITE_BIT, VK_ACCESS_2_SHADER_READ_BIT|VK_ACCESS_2_SHADER_WRITE_BIT);
            // push constants: dt, force, cx, cy, cz, radius, dirx, diry, dirz
            struct PCInject { float dt, force, cx, cy, cz, radius, dirx, diry, dirz; } pci{ dt, force, (float)(W*0.5f), 6.0f, (float)(D*0.5f), 12.0f, 0.0f, 1.0f, 0.0f };
            vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_COMPUTE, p_inject_);
            vkCmdBindDescriptorSets(cmd, VK_PIPELINE_BIND_POINT_COMPUTE, pl_inject_, 0, 1, &ds_inject_, 0, nullptr);
            vkCmdPushConstants(cmd, pl_inject_, VK_SHADER_STAGE_COMPUTE_BIT, 0, sizeof(PCInject), &pci);
            uint32_t gx=(W+7)/8, gy=(H+7)/8, gz=(D+7)/8; vkCmdDispatch(cmd,gx,gy,gz);
        }

        // Advect velocity: velA -> velB
        {
            update_ds_advect_vec_();
            barrier_img(velA_.img, VK_IMAGE_ASPECT_COLOR_BIT, VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT, VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT, VK_ACCESS_2_MEMORY_READ_BIT|VK_ACCESS_2_MEMORY_WRITE_BIT, VK_ACCESS_2_SHADER_READ_BIT);
            barrier_img(velB_.img, VK_IMAGE_ASPECT_COLOR_BIT, VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT, VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT, 0, VK_ACCESS_2_SHADER_WRITE_BIT);
            bind_and_push8(p_advect_vec_, pl_advect_vec_, ds_advect_vec_, dt, (float)W, (float)H, (float)D, diss_vel, 0,0,0);
            uint32_t gx=(W+7)/8, gy=(H+7)/8, gz=(D+7)/8; vkCmdDispatch(cmd,gx,gy,gz);
            std::swap(velA_, velB_);
        }

        // Project: compute divergence of velA into div_
        {
            update_ds_divergence_();
            barrier_img(velA_.img, VK_IMAGE_ASPECT_COLOR_BIT, VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT, VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT, VK_ACCESS_2_MEMORY_READ_BIT|VK_ACCESS_2_MEMORY_WRITE_BIT, VK_ACCESS_2_SHADER_READ_BIT);
            barrier_img(div_.img, VK_IMAGE_ASPECT_COLOR_BIT, VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT, VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT, 0, VK_ACCESS_2_SHADER_WRITE_BIT);
            bind_and_push8(p_divergence_, pl_divergence_, ds_divergence_, 0,(float)W,(float)H,(float)D,0,0,0,0);
            uint32_t gx=(W+7)/8, gy=(H+7)/8, gz=(D+7)/8; vkCmdDispatch(cmd,gx,gy,gz);
        }

        // Clear pressure to 0 on first frame
        if (clear_pressure_) {
            VkClearColorValue z{}; z.float32[0]=z.float32[1]=z.float32[2]=z.float32[3]=0;
            VkImageSubresourceRange r{VK_IMAGE_ASPECT_COLOR_BIT,0,1,0,1};
            vkCmdClearColorImage(cmd, pA_.img, VK_IMAGE_LAYOUT_GENERAL, &z, 1, &r);
            vkCmdClearColorImage(cmd, pB_.img, VK_IMAGE_LAYOUT_GENERAL, &z, 1, &r);
            clear_pressure_ = false;
        }

        // Jacobi iterations to solve Poisson: pA <-> pB
        {
            update_ds_jacobi_();
            const int iters = 10;
            for(int i=0;i<iters;++i){
                barrier_img(pA_.img, VK_IMAGE_ASPECT_COLOR_BIT, VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT, VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT, VK_ACCESS_2_MEMORY_READ_BIT|VK_ACCESS_2_MEMORY_WRITE_BIT, VK_ACCESS_2_SHADER_READ_BIT);
                barrier_img(div_.img, VK_IMAGE_ASPECT_COLOR_BIT, VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT, VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT, VK_ACCESS_2_MEMORY_READ_BIT|VK_ACCESS_2_MEMORY_WRITE_BIT, VK_ACCESS_2_SHADER_READ_BIT);
                barrier_img(pB_.img, VK_IMAGE_ASPECT_COLOR_BIT, VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT, VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT, 0, VK_ACCESS_2_SHADER_WRITE_BIT);
                bind_and_push8(p_jacobi_, pl_jacobi_, ds_jacobi_, 0,(float)W,(float)H,(float)D,0,0,0,0);
                uint32_t gx=(W+7)/8, gy=(H+7)/8, gz=(D+7)/8; vkCmdDispatch(cmd,gx,gy,gz);
                std::swap(pA_, pB_);
                update_ds_jacobi_();
            }
        }

        // Subtract gradient: velA - grad(pA) -> velB, then swap
        {
            update_ds_gradient_();
            barrier_img(pA_.img, VK_IMAGE_ASPECT_COLOR_BIT, VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT, VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT, VK_ACCESS_2_MEMORY_READ_BIT|VK_ACCESS_2_MEMORY_WRITE_BIT, VK_ACCESS_2_SHADER_READ_BIT);
            barrier_img(velA_.img, VK_IMAGE_ASPECT_COLOR_BIT, VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT, VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT, VK_ACCESS_2_MEMORY_READ_BIT|VK_ACCESS_2_MEMORY_WRITE_BIT, VK_ACCESS_2_SHADER_READ_BIT);
            barrier_img(velB_.img, VK_IMAGE_ASPECT_COLOR_BIT, VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT, VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT, 0, VK_ACCESS_2_SHADER_WRITE_BIT);
            bind_and_push8(p_gradient_, pl_gradient_, ds_gradient_, 0,(float)W,(float)H,(float)D,0,0,0,0);
            uint32_t gx=(W+7)/8, gy=(H+7)/8, gz=(D+7)/8; vkCmdDispatch(cmd,gx,gy,gz);
            std::swap(velA_, velB_);
        }

        // Advect density: denA -> denB, using velA
        {
            update_ds_advect_scalar_();
            barrier_img(velA_.img, VK_IMAGE_ASPECT_COLOR_BIT, VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT, VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT, VK_ACCESS_2_MEMORY_READ_BIT|VK_ACCESS_2_MEMORY_WRITE_BIT, VK_ACCESS_2_SHADER_READ_BIT);
            barrier_img(denA_.img, VK_IMAGE_ASPECT_COLOR_BIT, VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT, VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT, VK_ACCESS_2_MEMORY_READ_BIT|VK_ACCESS_2_MEMORY_WRITE_BIT, VK_ACCESS_2_SHADER_READ_BIT);
            barrier_img(denB_.img, VK_IMAGE_ASPECT_COLOR_BIT, VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT, VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT, 0, VK_ACCESS_2_SHADER_WRITE_BIT);
            bind_and_push8(p_advect_scalar_, pl_advect_scalar_, ds_advect_scalar_, dt,(float)W,(float)H,(float)D,diss_den,0,0,0);
            uint32_t gx=(W+7)/8, gy=(H+7)/8, gz=(D+7)/8; vkCmdDispatch(cmd,gx,gy,gz);
            std::swap(denA_, denB_);
        }

        // Render with camera raymarch
        if (!f.color_attachments.empty()){
            const auto& color = f.color_attachments.front();
            update_ds_render_(color);
            barrier_img(color.image, color.aspect, VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT, VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT, VK_ACCESS_2_COLOR_ATTACHMENT_WRITE_BIT|VK_ACCESS_2_MEMORY_READ_BIT|VK_ACCESS_2_MEMORY_WRITE_BIT, VK_ACCESS_2_SHADER_WRITE_BIT);
            barrier_img(denA_.img, VK_IMAGE_ASPECT_COLOR_BIT, VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT, VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT, VK_ACCESS_2_MEMORY_READ_BIT|VK_ACCESS_2_MEMORY_WRITE_BIT, VK_ACCESS_2_SHADER_READ_BIT);
            // Build camera params
            auto st = cam_.state();
            auto eye = cam_.eye_position();
            auto worldUp = vv::make_float3(0,1,0);
            vv::float3 fwd{}; if (st.mode==vv::CameraMode::Orbit) { vv::float3 toT = vv::make_float3(st.target.x - eye.x, st.target.y - eye.y, st.target.z - eye.z); fwd = vv::normalize(toT); } else {
                float yaw = st.fly_yaw_deg * 3.1415926535f/180.0f; float pitch = st.fly_pitch_deg * 3.1415926535f/180.0f;
                fwd = vv::make_float3(std::cos(pitch)*std::cos(yaw), std::sin(pitch), std::cos(pitch)*std::sin(yaw));
            }
            vv::float3 right = vv::normalize(vv::cross(fwd, worldUp));
            vv::float3 up = vv::normalize(vv::cross(right, fwd));
            float aspect = (f.extent.height>0)? (float)f.extent.width/(float)f.extent.height : 1.777f;
            float tanHalfFovY = std::tan((st.fov_y_deg * 3.1415926535f/180.0f)*0.5f);
            struct PCR {
                float camEye[3]; float tanHalfFovY;
                float camRight[3]; float aspect;
                float camUp[3]; float steps;
                float camFwd[3]; float W;
                float H; float D; float pad0; float pad1;
            } pc{};
            pc.camEye[0]=eye.x; pc.camEye[1]=eye.y; pc.camEye[2]=eye.z; pc.tanHalfFovY=tanHalfFovY;
            pc.camRight[0]=right.x; pc.camRight[1]=right.y; pc.camRight[2]=right.z; pc.aspect=aspect;
            pc.camUp[0]=up.x; pc.camUp[1]=up.y; pc.camUp[2]=up.z; pc.steps=(float)std::min<uint32_t>(D, 96);
            pc.camFwd[0]=fwd.x; pc.camFwd[1]=fwd.y; pc.camFwd[2]=fwd.z; pc.W=(float)W; pc.H=(float)H; pc.D=(float)D; pc.pad0=0; pc.pad1=0;
            vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_COMPUTE, p_render_);
            vkCmdBindDescriptorSets(cmd, VK_PIPELINE_BIND_POINT_COMPUTE, pl_render_, 0, 1, &ds_render_, 0, nullptr);
            vkCmdPushConstants(cmd, pl_render_, VK_SHADER_STAGE_COMPUTE_BIT, 0, sizeof(PCR), &pc);
            uint32_t gx=(f.extent.width+15)/16, gy=(f.extent.height+15)/16; vkCmdDispatch(cmd,gx,gy,1);
        }
    }

    void record_graphics(VkCommandBuffer, const EngineContext&, const FrameContext&) override {}

private:
    EngineContext eng_{}; VkDevice dev_{VK_NULL_HANDLE}; VmaAllocator alloc_{}; DescriptorAllocator* da_{};
    vv::CameraService cam_{};

    // sim resources
    uint32_t sim_w_{0}, sim_h_{0}, sim_d_{0}; bool images_ready_{false}; bool images_initialized_{false}; bool clear_pressure_{true};
    // Velocity stored as a single RGBA32F 3D image (xyz used), double-buffered
    Image3D velA_{}, velB_{};
    Image3D denA_{}, denB_{}; // r32f
    Image3D pA_{}, pB_{};     // r32f
    Image3D div_{};           // r32f

    // pipelines
    VkShaderModule sm_advect_vec_{}, sm_advect_scalar_{}, sm_divergence_{}, sm_jacobi_{}, sm_gradient_{}, sm_inject_{}, sm_render_{};
    VkDescriptorSetLayout dsl_advect_vec_{}, dsl_advect_scalar_{}, dsl_divergence_{}, dsl_jacobi_{}, dsl_gradient_{}, dsl_inject_{}, dsl_render_{};
    VkPipelineLayout pl_advect_vec_{}, pl_advect_scalar_{}, pl_divergence_{}, pl_jacobi_{}, pl_gradient_{}, pl_inject_{}, pl_render_{};
    VkPipeline p_advect_vec_{}, p_advect_scalar_{}, p_divergence_{}, p_jacobi_{}, p_gradient_{}, p_inject_{}, p_render_{};
    VkDescriptorSet ds_advect_vec_{}, ds_advect_scalar_{}, ds_divergence_{}, ds_jacobi_{}, ds_gradient_{}, ds_inject_{}, ds_render_{};

    void recreate_for_extent_(VkExtent2D e){ destroy_images_(); create_all(e); }

    void create_all(VkExtent2D e){
        // pick sim grid ~ quarter resolution in X/Y to avoid long compute, and moderate depth
        sim_w_ = std::max(64u, e.width / 4u);
        sim_h_ = std::max(64u, e.height/ 4u);
        sim_d_ = std::max(32u, std::min(64u, e.height/4u));
        // velocity
        create_image3D_(sim_w_, sim_h_, sim_d_, VK_FORMAT_R32G32B32A32_SFLOAT, velA_);
        create_image3D_(sim_w_, sim_h_, sim_d_, VK_FORMAT_R32G32B32A32_SFLOAT, velB_);
        // scalars
        create_image3D_(sim_w_, sim_h_, sim_d_, VK_FORMAT_R32_SFLOAT, denA_);
        create_image3D_(sim_w_, sim_h_, sim_d_, VK_FORMAT_R32_SFLOAT, denB_);
        create_image3D_(sim_w_, sim_h_, sim_d_, VK_FORMAT_R32_SFLOAT, pA_);
        create_image3D_(sim_w_, sim_h_, sim_d_, VK_FORMAT_R32_SFLOAT, pB_);
        create_image3D_(sim_w_, sim_h_, sim_d_, VK_FORMAT_R32_SFLOAT, div_);
        images_ready_ = true; images_initialized_ = false; clear_pressure_ = true;
    }

    void destroy_images_(){
        auto di=[&](Image3D& t){ if (!t.img) return; if (t.view) vkDestroyImageView(dev_, t.view, nullptr); vmaDestroyImage(alloc_, t.img, t.alloc); t = {}; };
        di(velA_); di(velB_);
        di(denA_); di(denB_); di(pA_); di(pB_); di(div_);
        images_ready_ = false; images_initialized_ = false; clear_pressure_ = true;
    }

    void create_image3D_(uint32_t w, uint32_t h, uint32_t d, VkFormat fmt, Image3D& out){
        VkImageCreateInfo ci{VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO};
        ci.imageType = VK_IMAGE_TYPE_3D; ci.extent = {w,h,d}; ci.mipLevels=1; ci.arrayLayers=1; ci.format=fmt; ci.tiling=VK_IMAGE_TILING_OPTIMAL; ci.usage = VK_IMAGE_USAGE_STORAGE_BIT | VK_IMAGE_USAGE_TRANSFER_SRC_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT; ci.samples=VK_SAMPLE_COUNT_1_BIT; ci.sharingMode=VK_SHARING_MODE_EXCLUSIVE; ci.initialLayout = VK_IMAGE_LAYOUT_UNDEFINED;
        VmaAllocationCreateInfo ai{}; ai.usage = VMA_MEMORY_USAGE_AUTO; ai.requiredFlags = VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT;
        VK_CHECK(vmaCreateImage(alloc_, &ci, &ai, &out.img, &out.alloc, nullptr));
        VkImageViewCreateInfo vi{VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO}; vi.image=out.img; vi.viewType=VK_IMAGE_VIEW_TYPE_3D; vi.format=fmt; vi.subresourceRange={VK_IMAGE_ASPECT_COLOR_BIT,0,1,0,1};
        VK_CHECK(vkCreateImageView(dev_, &vi, nullptr, &out.view));
        out.extent = {w,h,d}; out.fmt = fmt;
    }

    // Descriptor updates per pass (image3D) matching shader bindings
    void update_ds_advect_vec_(){
        // advect_vec3_3d: binding 0 srcField (rgba32f), binding 1 dstField (rgba32f)
        VkWriteDescriptorSet w[2]{};
        VkDescriptorImageInfo src{.sampler=VK_NULL_HANDLE,.imageView=velA_.view,.imageLayout=VK_IMAGE_LAYOUT_GENERAL};
        VkDescriptorImageInfo dst{.sampler=VK_NULL_HANDLE,.imageView=velB_.view,.imageLayout=VK_IMAGE_LAYOUT_GENERAL};
        for(int i=0;i<2;++i){ w[i].sType=VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET; w[i].dstSet=ds_advect_vec_; w[i].descriptorCount=1; w[i].descriptorType=VK_DESCRIPTOR_TYPE_STORAGE_IMAGE; }
        w[0].dstBinding=0; w[0].pImageInfo=&src; w[1].dstBinding=1; w[1].pImageInfo=&dst;
        vkUpdateDescriptorSets(dev_,2,w,0,nullptr);
    }
    void update_ds_advect_scalar_(){
        // advect_scalar_3d: binding 0 velField (rgba32f), 1 src (r32f), 2 dst (r32f)
        VkWriteDescriptorSet w[3]{};
        VkDescriptorImageInfo v{.sampler=VK_NULL_HANDLE,.imageView=velA_.view,.imageLayout=VK_IMAGE_LAYOUT_GENERAL};
        VkDescriptorImageInfo src{.sampler=VK_NULL_HANDLE,.imageView=denA_.view,.imageLayout=VK_IMAGE_LAYOUT_GENERAL};
        VkDescriptorImageInfo dst{.sampler=VK_NULL_HANDLE,.imageView=denB_.view,.imageLayout=VK_IMAGE_LAYOUT_GENERAL};
        for(int i=0;i<3;++i){ w[i].sType=VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET; w[i].dstSet=ds_advect_scalar_; w[i].descriptorCount=1; w[i].descriptorType=VK_DESCRIPTOR_TYPE_STORAGE_IMAGE; }
        w[0].dstBinding=0; w[0].pImageInfo=&v; w[1].dstBinding=1; w[1].pImageInfo=&src; w[2].dstBinding=2; w[2].pImageInfo=&dst;
        vkUpdateDescriptorSets(dev_,3,w,0,nullptr);
    }
    void update_ds_divergence_(){
        // divergence_3d: binding 0 velField (rgba32f), 1 outDiv (r32f)
        VkWriteDescriptorSet w[2]{};
        VkDescriptorImageInfo v{.sampler=VK_NULL_HANDLE,.imageView=velA_.view,.imageLayout=VK_IMAGE_LAYOUT_GENERAL};
        VkDescriptorImageInfo out{.sampler=VK_NULL_HANDLE,.imageView=div_.view,.imageLayout=VK_IMAGE_LAYOUT_GENERAL};
        for(int i=0;i<2;++i){ w[i].sType=VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET; w[i].dstSet=ds_divergence_; w[i].descriptorCount=1; w[i].descriptorType=VK_DESCRIPTOR_TYPE_STORAGE_IMAGE; }
        w[0].dstBinding=0; w[0].pImageInfo=&v; w[1].dstBinding=1; w[1].pImageInfo=&out;
        vkUpdateDescriptorSets(dev_,2,w,0,nullptr);
    }
    void update_ds_jacobi_(){
        // jacobi_3d: binding 0 pSrc (r32f), 1 divergence (r32f), 2 pDst (r32f)
        VkWriteDescriptorSet w[3]{};
        VkDescriptorImageInfo psrc{.sampler=VK_NULL_HANDLE,.imageView=pA_.view,.imageLayout=VK_IMAGE_LAYOUT_GENERAL};
        VkDescriptorImageInfo divi{.sampler=VK_NULL_HANDLE,.imageView=div_.view,.imageLayout=VK_IMAGE_LAYOUT_GENERAL};
        VkDescriptorImageInfo pdst{.sampler=VK_NULL_HANDLE,.imageView=pB_.view,.imageLayout=VK_IMAGE_LAYOUT_GENERAL};
        for(int i=0;i<3;++i){ w[i].sType=VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET; w[i].dstSet=ds_jacobi_; w[i].descriptorCount=1; w[i].descriptorType=VK_DESCRIPTOR_TYPE_STORAGE_IMAGE; }
        w[0].dstBinding=0; w[0].pImageInfo=&psrc; w[1].dstBinding=1; w[1].pImageInfo=&divi; w[2].dstBinding=2; w[2].pImageInfo=&pdst;
        vkUpdateDescriptorSets(dev_,3,w,0,nullptr);
    }
    void update_ds_gradient_(){
        // gradient_3d: binding 0 pressure (r32f), 1 velSrc (rgba32f), 2 velDst (rgba32f)
        VkWriteDescriptorSet w[3]{};
        VkDescriptorImageInfo p{.sampler=VK_NULL_HANDLE,.imageView=pA_.view,.imageLayout=VK_IMAGE_LAYOUT_GENERAL};
        VkDescriptorImageInfo vs{.sampler=VK_NULL_HANDLE,.imageView=velA_.view,.imageLayout=VK_IMAGE_LAYOUT_GENERAL};
        VkDescriptorImageInfo vd{.sampler=VK_NULL_HANDLE,.imageView=velB_.view,.imageLayout=VK_IMAGE_LAYOUT_GENERAL};
        for(int i=0;i<3;++i){ w[i].sType=VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET; w[i].dstSet=ds_gradient_; w[i].descriptorCount=1; w[i].descriptorType=VK_DESCRIPTOR_TYPE_STORAGE_IMAGE; }
        w[0].dstBinding=0; w[0].pImageInfo=&p; w[1].dstBinding=1; w[1].pImageInfo=&vs; w[2].dstBinding=2; w[2].pImageInfo=&vd;
        vkUpdateDescriptorSets(dev_,3,w,0,nullptr);
    }
    void update_ds_inject_(){
        // inject_3d: binding 0 velField (rgba32f), 1 denField (r32f)
        VkWriteDescriptorSet w[2]{};
        VkDescriptorImageInfo v{.sampler=VK_NULL_HANDLE,.imageView=velA_.view,.imageLayout=VK_IMAGE_LAYOUT_GENERAL};
        VkDescriptorImageInfo den{.sampler=VK_NULL_HANDLE,.imageView=denA_.view,.imageLayout=VK_IMAGE_LAYOUT_GENERAL};
        for(int i=0;i<2;++i){ w[i].sType=VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET; w[i].dstSet=ds_inject_; w[i].descriptorCount=1; w[i].descriptorType=VK_DESCRIPTOR_TYPE_STORAGE_IMAGE; }
        w[0].dstBinding=0; w[0].pImageInfo=&v; w[1].dstBinding=1; w[1].pImageInfo=&den;
        vkUpdateDescriptorSets(dev_,2,w,0,nullptr);
    }
    void update_ds_render_(const AttachmentView& color){ VkDescriptorImageInfo i0{.sampler=VK_NULL_HANDLE, .imageView=denA_.view, .imageLayout=VK_IMAGE_LAYOUT_GENERAL}; VkDescriptorImageInfo i1{.sampler=VK_NULL_HANDLE, .imageView=color.view, .imageLayout=VK_IMAGE_LAYOUT_GENERAL}; VkWriteDescriptorSet w[2]{{VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET},{VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET}}; w[0].dstSet=ds_render_; w[0].dstBinding=0; w[0].descriptorCount=1; w[0].descriptorType=VK_DESCRIPTOR_TYPE_STORAGE_IMAGE; w[0].pImageInfo=&i0; w[1]=w[0]; w[1].dstBinding=1; w[1].pImageInfo=&i1; vkUpdateDescriptorSets(dev_,2,w,0,nullptr);}

    void create_pipelines_(){
        std::string d(SHADER_OUTPUT_DIR);
        sm_advect_vec_   = make_shader(dev_, load_spv(d+"/advect_vec3_3d.comp.spv"));
        sm_advect_scalar_= make_shader(dev_, load_spv(d+"/advect_scalar_3d.comp.spv"));
        sm_divergence_   = make_shader(dev_, load_spv(d+"/divergence_3d.comp.spv"));
        sm_jacobi_       = make_shader(dev_, load_spv(d+"/jacobi_3d.comp.spv"));
        sm_gradient_     = make_shader(dev_, load_spv(d+"/gradient_3d.comp.spv"));
        sm_inject_       = make_shader(dev_, load_spv(d+"/inject_3d.comp.spv"));
        sm_render_       = make_shader(dev_, load_spv(d+"/render_volume_3d.comp.spv"));
        auto mkdsl = [&](std::vector<VkDescriptorSetLayoutBinding> binds){ VkDescriptorSetLayoutCreateInfo ci{VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO}; ci.bindingCount=(uint32_t)binds.size(); ci.pBindings=binds.data(); VkDescriptorSetLayout l{}; VK_CHECK(vkCreateDescriptorSetLayout(dev_, &ci, nullptr, &l)); return l; };
        // advect_vec3_3d: 2 images (src, dst)
        dsl_advect_vec_    = mkdsl({ {0,VK_DESCRIPTOR_TYPE_STORAGE_IMAGE,1,VK_SHADER_STAGE_COMPUTE_BIT,nullptr}, {1,VK_DESCRIPTOR_TYPE_STORAGE_IMAGE,1,VK_SHADER_STAGE_COMPUTE_BIT,nullptr} });
        // advect_scalar_3d: 3 images (vel, src, dst)
        dsl_advect_scalar_ = mkdsl({ {0,VK_DESCRIPTOR_TYPE_STORAGE_IMAGE,1,VK_SHADER_STAGE_COMPUTE_BIT,nullptr}, {1,VK_DESCRIPTOR_TYPE_STORAGE_IMAGE,1,VK_SHADER_STAGE_COMPUTE_BIT,nullptr}, {2,VK_DESCRIPTOR_TYPE_STORAGE_IMAGE,1,VK_SHADER_STAGE_COMPUTE_BIT,nullptr} });
        // divergence_3d: 2 images (vel -> outDiv)
        dsl_divergence_    = mkdsl({ {0,VK_DESCRIPTOR_TYPE_STORAGE_IMAGE,1,VK_SHADER_STAGE_COMPUTE_BIT,nullptr}, {1,VK_DESCRIPTOR_TYPE_STORAGE_IMAGE,1,VK_SHADER_STAGE_COMPUTE_BIT,nullptr} });
        // jacobi_3d: unchanged (pSrc, div, pDst)
        dsl_jacobi_        = mkdsl({ {0,VK_DESCRIPTOR_TYPE_STORAGE_IMAGE,1,VK_SHADER_STAGE_COMPUTE_BIT,nullptr}, {1,VK_DESCRIPTOR_TYPE_STORAGE_IMAGE,1,VK_SHADER_STAGE_COMPUTE_BIT,nullptr}, {2,VK_DESCRIPTOR_TYPE_STORAGE_IMAGE,1,VK_SHADER_STAGE_COMPUTE_BIT,nullptr} });
        // gradient_3d: 3 images (p, velSrc, velDst)
        dsl_gradient_      = mkdsl({ {0,VK_DESCRIPTOR_TYPE_STORAGE_IMAGE,1,VK_SHADER_STAGE_COMPUTE_BIT,nullptr}, {1,VK_DESCRIPTOR_TYPE_STORAGE_IMAGE,1,VK_SHADER_STAGE_COMPUTE_BIT,nullptr}, {2,VK_DESCRIPTOR_TYPE_STORAGE_IMAGE,1,VK_SHADER_STAGE_COMPUTE_BIT,nullptr} });
        // inject_3d: 2 images (vel, density)
        dsl_inject_        = mkdsl({ {0,VK_DESCRIPTOR_TYPE_STORAGE_IMAGE,1,VK_SHADER_STAGE_COMPUTE_BIT,nullptr}, {1,VK_DESCRIPTOR_TYPE_STORAGE_IMAGE,1,VK_SHADER_STAGE_COMPUTE_BIT,nullptr} });
        // render: unchanged (density, color)
        dsl_render_        = mkdsl({ {0,VK_DESCRIPTOR_TYPE_STORAGE_IMAGE,1,VK_SHADER_STAGE_COMPUTE_BIT,nullptr}, {1,VK_DESCRIPTOR_TYPE_STORAGE_IMAGE,1,VK_SHADER_STAGE_COMPUTE_BIT,nullptr} });
        auto mkpl = [&](VkDescriptorSetLayout dsl, uint32_t pcSize){ VkPushConstantRange pcr{VK_SHADER_STAGE_COMPUTE_BIT, 0, pcSize}; VkPipelineLayoutCreateInfo ci{VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO}; ci.setLayoutCount=1; ci.pSetLayouts=&dsl; ci.pushConstantRangeCount=1; ci.pPushConstantRanges=&pcr; VkPipelineLayout l{}; VK_CHECK(vkCreatePipelineLayout(dev_, &ci, nullptr, &l)); return l; };
        pl_advect_vec_    = mkpl(dsl_advect_vec_,    32);
        pl_advect_scalar_ = mkpl(dsl_advect_scalar_, 32);
        pl_divergence_    = mkpl(dsl_divergence_,    32);
        pl_jacobi_        = mkpl(dsl_jacobi_,        32);
        pl_gradient_      = mkpl(dsl_gradient_,      32);
        pl_inject_        = mkpl(dsl_inject_,        48);
        pl_render_        = mkpl(dsl_render_,        80);
        auto mkp = [&](VkShaderModule sm, VkPipelineLayout pl){ VkComputePipelineCreateInfo ci{VK_STRUCTURE_TYPE_COMPUTE_PIPELINE_CREATE_INFO}; VkPipelineShaderStageCreateInfo st{VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO}; st.stage=VK_SHADER_STAGE_COMPUTE_BIT; st.module=sm; st.pName="main"; ci.stage=st; ci.layout=pl; VkPipeline p{}; VK_CHECK(vkCreateComputePipelines(dev_, VK_NULL_HANDLE, 1, &ci, nullptr, &p)); return p; };
        p_advect_vec_    = mkp(sm_advect_vec_,    pl_advect_vec_);
        p_advect_scalar_ = mkp(sm_advect_scalar_, pl_advect_scalar_);
        p_divergence_    = mkp(sm_divergence_,    pl_divergence_);
        p_jacobi_        = mkp(sm_jacobi_,        pl_jacobi_);
        p_gradient_      = mkp(sm_gradient_,      pl_gradient_);
        p_inject_        = mkp(sm_inject_,        pl_inject_);
        p_render_        = mkp(sm_render_,        pl_render_);
        ds_advect_vec_    = da_->allocate(dev_, dsl_advect_vec_);
        ds_advect_scalar_ = da_->allocate(dev_, dsl_advect_scalar_);
        ds_divergence_    = da_->allocate(dev_, dsl_divergence_);
        ds_jacobi_        = da_->allocate(dev_, dsl_jacobi_);
        ds_gradient_      = da_->allocate(dev_, dsl_gradient_);
        ds_inject_        = da_->allocate(dev_, dsl_inject_);
        ds_render_        = da_->allocate(dev_, dsl_render_);
    }

    void destroy_pipelines_(){
        auto ds=[&](VkPipeline& p){ if(p) vkDestroyPipeline(dev_, p, nullptr); p=VK_NULL_HANDLE; };
        ds(p_advect_vec_); ds(p_advect_scalar_); ds(p_divergence_); ds(p_jacobi_); ds(p_gradient_); ds(p_inject_); ds(p_render_);
        auto dl=[&](VkPipelineLayout& l){ if(l) vkDestroyPipelineLayout(dev_, l, nullptr); l=VK_NULL_HANDLE; };
        dl(pl_advect_vec_); dl(pl_advect_scalar_); dl(pl_divergence_); dl(pl_jacobi_); dl(pl_gradient_); dl(pl_inject_); dl(pl_render_);
        auto dsl=[&](VkDescriptorSetLayout& l){ if(l) vkDestroyDescriptorSetLayout(dev_, l, nullptr); l=VK_NULL_HANDLE; };
        dsl(dsl_advect_vec_); dsl(dsl_advect_scalar_); dsl(dsl_divergence_); dsl(dsl_jacobi_); dsl(dsl_gradient_); dsl(dsl_inject_); dsl(dsl_render_);
        auto sm=[&](VkShaderModule& m){ if(m) vkDestroyShaderModule(dev_, m, nullptr); m=VK_NULL_HANDLE; };
        sm(sm_advect_vec_); sm(sm_advect_scalar_); sm(sm_divergence_); sm(sm_jacobi_); sm(sm_gradient_); sm(sm_inject_); sm(sm_render_);
    }
};

int main(){ try{ VulkanEngine e; e.configure_window(1280, 720, "ex11_stable_fluids_3d"); e.set_renderer(std::make_unique<StableFluids>()); e.init(); e.run(); e.cleanup(); } catch(const std::exception& ex){ std::fprintf(stderr, "Fatal: %s\n", ex.what()); return 1; } return 0; }
