#include "vk_engine.h"
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
        c.enable_imgui = false; // self-contained
        c.presentation_mode = PresentationMode::EngineBlit;
        c.color_attachments = { AttachmentRequest{ .name = "color", .format = VK_FORMAT_R8G8B8A8_UNORM, .usage = VK_IMAGE_USAGE_STORAGE_BIT | VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT | VK_IMAGE_USAGE_TRANSFER_SRC_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT, .samples = VK_SAMPLE_COUNT_1_BIT, .aspect = VK_IMAGE_ASPECT_COLOR_BIT, .initial_layout = VK_IMAGE_LAYOUT_GENERAL } };
        c.presentation_attachment = "color";
    }

    void initialize(const EngineContext& e, const RendererCaps&, const FrameContext& f0) override {
        eng_ = e; dev_ = e.device; alloc_ = e.allocator; da_ = e.descriptorAllocator;
        create_all(f0.extent);
        create_pipelines_();
    }
    void on_swapchain_ready(const EngineContext& e, const FrameContext& f) override { (void)e; recreate_for_extent_(f.extent); }
    void on_swapchain_destroy(const EngineContext& e) override { (void)e; destroy_images_(); }

    void destroy(const EngineContext& e, const RendererCaps&) override {
        destroy_pipelines_();
        destroy_images_();
        eng_ = {}; dev_ = VK_NULL_HANDLE; alloc_ = nullptr; da_ = nullptr;
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

        // Inject source (velocity + density) near bottom-center of volume
        {
            update_ds_inject_();
            barrier_img(velA_.img, VK_IMAGE_ASPECT_COLOR_BIT, VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT, VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT, VK_ACCESS_2_MEMORY_READ_BIT|VK_ACCESS_2_MEMORY_WRITE_BIT, VK_ACCESS_2_SHADER_READ_BIT|VK_ACCESS_2_SHADER_WRITE_BIT);
            barrier_img(denA_.img, VK_IMAGE_ASPECT_COLOR_BIT, VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT, VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT, VK_ACCESS_2_MEMORY_READ_BIT|VK_ACCESS_2_MEMORY_WRITE_BIT, VK_ACCESS_2_SHADER_READ_BIT|VK_ACCESS_2_SHADER_WRITE_BIT);
            bind_and_push8(p_inject_, pl_inject_, ds_inject_, dt, force, (float)(W*0.5f), (float)(H*0.25f), (float)(D*0.5f), 18.0f, 0.0f, 1.0f);
            uint32_t gx=(W+7)/8, gy=(H+7)/8, gz=(D+7)/8; vkCmdDispatch(cmd,gx,gy,gz);
        }

        // Advect velocity: velA -> velB
        {
            update_ds_advect_vec_(velA_, velB_, diss_vel);
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
            update_ds_jacobi_(pA_, pB_);
            const int iters = 30;
            for(int i=0;i<iters;++i){
                barrier_img(pA_.img, VK_IMAGE_ASPECT_COLOR_BIT, VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT, VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT, VK_ACCESS_2_MEMORY_READ_BIT|VK_ACCESS_2_MEMORY_WRITE_BIT, VK_ACCESS_2_SHADER_READ_BIT);
                barrier_img(div_.img, VK_IMAGE_ASPECT_COLOR_BIT, VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT, VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT, VK_ACCESS_2_MEMORY_READ_BIT|VK_ACCESS_2_MEMORY_WRITE_BIT, VK_ACCESS_2_SHADER_READ_BIT);
                barrier_img(pB_.img, VK_IMAGE_ASPECT_COLOR_BIT, VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT, VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT, 0, VK_ACCESS_2_SHADER_WRITE_BIT);
                bind_and_push8(p_jacobi_, pl_jacobi_, ds_jacobi_, 0,(float)W,(float)H,(float)D,0,0,0,0);
                uint32_t gx=(W+7)/8, gy=(H+7)/8, gz=(D+7)/8; vkCmdDispatch(cmd,gx,gy,gz);
                std::swap(pA_, pB_);
                update_ds_jacobi_(pA_, pB_);
            }
        }

        // Subtract gradient: velA - grad(pA) -> velB, then swap
        {
            update_ds_gradient_(pA_, velA_, velB_);
            barrier_img(pA_.img, VK_IMAGE_ASPECT_COLOR_BIT, VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT, VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT, VK_ACCESS_2_MEMORY_READ_BIT|VK_ACCESS_2_MEMORY_WRITE_BIT, VK_ACCESS_2_SHADER_READ_BIT);
            barrier_img(velA_.img, VK_IMAGE_ASPECT_COLOR_BIT, VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT, VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT, VK_ACCESS_2_MEMORY_READ_BIT|VK_ACCESS_2_MEMORY_WRITE_BIT, VK_ACCESS_2_SHADER_READ_BIT);
            barrier_img(velB_.img, VK_IMAGE_ASPECT_COLOR_BIT, VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT, VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT, 0, VK_ACCESS_2_SHADER_WRITE_BIT);
            bind_and_push8(p_gradient_, pl_gradient_, ds_gradient_, 0,(float)W,(float)H,(float)D,0,0,0,0);
            uint32_t gx=(W+7)/8, gy=(H+7)/8, gz=(D+7)/8; vkCmdDispatch(cmd,gx,gy,gz);
            std::swap(velA_, velB_);
        }

        // Advect density: denA -> denB, using velA
        {
            update_ds_advect_scalar_(velA_, denA_, denB_, diss_den);
            barrier_img(velA_.img, VK_IMAGE_ASPECT_COLOR_BIT, VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT, VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT, VK_ACCESS_2_MEMORY_READ_BIT|VK_ACCESS_2_MEMORY_WRITE_BIT, VK_ACCESS_2_SHADER_READ_BIT);
            barrier_img(denA_.img, VK_IMAGE_ASPECT_COLOR_BIT, VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT, VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT, VK_ACCESS_2_MEMORY_READ_BIT|VK_ACCESS_2_MEMORY_WRITE_BIT, VK_ACCESS_2_SHADER_READ_BIT);
            barrier_img(denB_.img, VK_IMAGE_ASPECT_COLOR_BIT, VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT, VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT, 0, VK_ACCESS_2_SHADER_WRITE_BIT);
            bind_and_push8(p_advect_scalar_, pl_advect_scalar_, ds_advect_scalar_, dt,(float)W,(float)H,(float)D,diss_den,0,0,0);
            uint32_t gx=(W+7)/8, gy=(H+7)/8, gz=(D+7)/8; vkCmdDispatch(cmd,gx,gy,gz);
            std::swap(denA_, denB_);
        }

        // Render: simple raymarch along +Z through density volume
        if (!f.color_attachments.empty()){
            const auto& color = f.color_attachments.front();
            update_ds_render_(color);
            barrier_img(color.image, color.aspect, VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT, VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT, VK_ACCESS_2_COLOR_ATTACHMENT_WRITE_BIT|VK_ACCESS_2_MEMORY_READ_BIT|VK_ACCESS_2_MEMORY_WRITE_BIT, VK_ACCESS_2_SHADER_WRITE_BIT);
            barrier_img(denA_.img, VK_IMAGE_ASPECT_COLOR_BIT, VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT, VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT, VK_ACCESS_2_MEMORY_READ_BIT|VK_ACCESS_2_MEMORY_WRITE_BIT, VK_ACCESS_2_SHADER_READ_BIT);
            vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_COMPUTE, p_render_);
            vkCmdBindDescriptorSets(cmd, VK_PIPELINE_BIND_POINT_COMPUTE, pl_render_, 0, 1, &ds_render_, 0, nullptr);
            struct PC{ float vw,vh, W,H, D, steps; } pc{ (float)f.extent.width, (float)f.extent.height, (float)W, (float)H, (float)D, (float)D };
            vkCmdPushConstants(cmd, pl_render_, VK_SHADER_STAGE_COMPUTE_BIT, 0, sizeof(PC), &pc);
            uint32_t gx=(f.extent.width+15)/16, gy=(f.extent.height+15)/16; vkCmdDispatch(cmd,gx,gy,1);
        }
    }

    void record_graphics(VkCommandBuffer, const EngineContext&, const FrameContext&) override {}

private:
    EngineContext eng_{}; VkDevice dev_{VK_NULL_HANDLE}; VmaAllocator alloc_{}; DescriptorAllocator* da_{};

    // sim resources
    uint32_t sim_w_{0}, sim_h_{0}, sim_d_{0}; bool images_ready_{false}; bool images_initialized_{false}; bool clear_pressure_{true};
    Image3D velA_{}, velB_{}; // rgba32f (xyz used)
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
        // pick sim grid ~ half resolution in X/Y, and a reasonable depth
        sim_w_ = std::max(64u, e.width / 2u);
        sim_h_ = std::max(64u, e.height/ 2u);
        sim_d_ = std::max(64u, std::min(128u, e.height/2u));
        create_image3D_(sim_w_, sim_h_, sim_d_, VK_FORMAT_R32G32B32A32_SFLOAT, velA_);
        create_image3D_(sim_w_, sim_h_, sim_d_, VK_FORMAT_R32G32B32A32_SFLOAT, velB_);
        create_image3D_(sim_w_, sim_h_, sim_d_, VK_FORMAT_R32_SFLOAT, denA_);
        create_image3D_(sim_w_, sim_h_, sim_d_, VK_FORMAT_R32_SFLOAT, denB_);
        create_image3D_(sim_w_, sim_h_, sim_d_, VK_FORMAT_R32_SFLOAT, pA_);
        create_image3D_(sim_w_, sim_h_, sim_d_, VK_FORMAT_R32_SFLOAT, pB_);
        create_image3D_(sim_w_, sim_h_, sim_d_, VK_FORMAT_R32_SFLOAT, div_);
        images_ready_ = true; images_initialized_ = false; clear_pressure_ = true;
    }

    void destroy_images_(){
        auto di=[&](Image3D& t){ if (!t.img) return; if (t.view) vkDestroyImageView(dev_, t.view, nullptr); vmaDestroyImage(alloc_, t.img, t.alloc); t = {}; };
        di(velA_); di(velB_); di(denA_); di(denB_); di(pA_); di(pB_); di(div_);
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

    // Descriptor updates per pass (image3D)
    void update_ds_advect_vec_(const Image3D& src, const Image3D& dst, float /*diss*/){ (void)0; VkDescriptorImageInfo i0{.sampler=VK_NULL_HANDLE, .imageView=src.view, .imageLayout=VK_IMAGE_LAYOUT_GENERAL}; VkDescriptorImageInfo i1{.sampler=VK_NULL_HANDLE, .imageView=dst.view, .imageLayout=VK_IMAGE_LAYOUT_GENERAL}; VkWriteDescriptorSet w[2]{{VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET},{VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET}}; w[0].dstSet=ds_advect_vec_; w[0].dstBinding=0; w[0].descriptorCount=1; w[0].descriptorType=VK_DESCRIPTOR_TYPE_STORAGE_IMAGE; w[0].pImageInfo=&i0; w[1]=w[0]; w[1].dstBinding=1; w[1].pImageInfo=&i1; vkUpdateDescriptorSets(dev_,2,w,0,nullptr); }
    void update_ds_advect_scalar_(const Image3D& vel, const Image3D& src, const Image3D& dst, float /*diss*/){ VkDescriptorImageInfo i0{.sampler=VK_NULL_HANDLE, .imageView=vel.view, .imageLayout=VK_IMAGE_LAYOUT_GENERAL}; VkDescriptorImageInfo i1{.sampler=VK_NULL_HANDLE, .imageView=src.view, .imageLayout=VK_IMAGE_LAYOUT_GENERAL}; VkDescriptorImageInfo i2{.sampler=VK_NULL_HANDLE, .imageView=dst.view, .imageLayout=VK_IMAGE_LAYOUT_GENERAL}; VkWriteDescriptorSet w[3]{{VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET},{VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET},{VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET}}; w[0].dstSet=ds_advect_scalar_; w[0].dstBinding=0; w[0].descriptorCount=1; w[0].descriptorType=VK_DESCRIPTOR_TYPE_STORAGE_IMAGE; w[0].pImageInfo=&i0; w[1]=w[0]; w[1].dstBinding=1; w[1].pImageInfo=&i1; w[2]=w[0]; w[2].dstBinding=2; w[2].pImageInfo=&i2; vkUpdateDescriptorSets(dev_,3,w,0,nullptr);}
    void update_ds_divergence_(){ VkDescriptorImageInfo i0{.sampler=VK_NULL_HANDLE, .imageView=velA_.view, .imageLayout=VK_IMAGE_LAYOUT_GENERAL}; VkDescriptorImageInfo i1{.sampler=VK_NULL_HANDLE, .imageView=div_.view, .imageLayout=VK_IMAGE_LAYOUT_GENERAL}; VkWriteDescriptorSet w[2]{{VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET},{VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET}}; w[0].dstSet=ds_divergence_; w[0].dstBinding=0; w[0].descriptorCount=1; w[0].descriptorType=VK_DESCRIPTOR_TYPE_STORAGE_IMAGE; w[0].pImageInfo=&i0; w[1]=w[0]; w[1].dstBinding=1; w[1].pImageInfo=&i1; vkUpdateDescriptorSets(dev_,2,w,0,nullptr);}
    void update_ds_jacobi_(const Image3D& pSrc, const Image3D& pDst){ VkDescriptorImageInfo i0{.sampler=VK_NULL_HANDLE, .imageView=pSrc.view, .imageLayout=VK_IMAGE_LAYOUT_GENERAL}; VkDescriptorImageInfo i1{.sampler=VK_NULL_HANDLE, .imageView=div_.view, .imageLayout=VK_IMAGE_LAYOUT_GENERAL}; VkDescriptorImageInfo i2{.sampler=VK_NULL_HANDLE, .imageView=pDst.view, .imageLayout=VK_IMAGE_LAYOUT_GENERAL}; VkWriteDescriptorSet w[3]{{VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET},{VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET},{VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET}}; w[0].dstSet=ds_jacobi_; w[0].dstBinding=0; w[0].descriptorCount=1; w[0].descriptorType=VK_DESCRIPTOR_TYPE_STORAGE_IMAGE; w[0].pImageInfo=&i0; w[1]=w[0]; w[1].dstBinding=1; w[1].pImageInfo=&i1; w[2]=w[0]; w[2].dstBinding=2; w[2].pImageInfo=&i2; vkUpdateDescriptorSets(dev_,3,w,0,nullptr);}
    void update_ds_gradient_(const Image3D& p, const Image3D& velSrc, const Image3D& velDst){ VkDescriptorImageInfo i0{.sampler=VK_NULL_HANDLE, .imageView=p.view, .imageLayout=VK_IMAGE_LAYOUT_GENERAL}; VkDescriptorImageInfo i1{.sampler=VK_NULL_HANDLE, .imageView=velSrc.view, .imageLayout=VK_IMAGE_LAYOUT_GENERAL}; VkDescriptorImageInfo i2{.sampler=VK_NULL_HANDLE, .imageView=velDst.view, .imageLayout=VK_IMAGE_LAYOUT_GENERAL}; VkWriteDescriptorSet w[3]{{VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET},{VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET},{VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET}}; w[0].dstSet=ds_gradient_; w[0].dstBinding=0; w[0].descriptorCount=1; w[0].descriptorType=VK_DESCRIPTOR_TYPE_STORAGE_IMAGE; w[0].pImageInfo=&i0; w[1]=w[0]; w[1].dstBinding=1; w[1].pImageInfo=&i1; w[2]=w[0]; w[2].dstBinding=2; w[2].pImageInfo=&i2; vkUpdateDescriptorSets(dev_,3,w,0,nullptr);}
    void update_ds_inject_(){ VkDescriptorImageInfo i0{.sampler=VK_NULL_HANDLE, .imageView=velA_.view, .imageLayout=VK_IMAGE_LAYOUT_GENERAL}; VkDescriptorImageInfo i1{.sampler=VK_NULL_HANDLE, .imageView=denA_.view, .imageLayout=VK_IMAGE_LAYOUT_GENERAL}; VkWriteDescriptorSet w[2]{{VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET},{VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET}}; w[0].dstSet=ds_inject_; w[0].dstBinding=0; w[0].descriptorCount=1; w[0].descriptorType=VK_DESCRIPTOR_TYPE_STORAGE_IMAGE; w[0].pImageInfo=&i0; w[1]=w[0]; w[1].dstBinding=1; w[1].pImageInfo=&i1; vkUpdateDescriptorSets(dev_,2,w,0,nullptr);}
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
        // Create layouts
        auto mkdsl = [&](std::vector<VkDescriptorSetLayoutBinding> binds){ VkDescriptorSetLayoutCreateInfo ci{VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO}; ci.bindingCount=(uint32_t)binds.size(); ci.pBindings=binds.data(); VkDescriptorSetLayout l{}; VK_CHECK(vkCreateDescriptorSetLayout(dev_, &ci, nullptr, &l)); return l; };
        dsl_advect_vec_    = mkdsl({ {0, VK_DESCRIPTOR_TYPE_STORAGE_IMAGE, 1, VK_SHADER_STAGE_COMPUTE_BIT, nullptr}, {1, VK_DESCRIPTOR_TYPE_STORAGE_IMAGE, 1, VK_SHADER_STAGE_COMPUTE_BIT, nullptr} });
        dsl_advect_scalar_ = mkdsl({ {0, VK_DESCRIPTOR_TYPE_STORAGE_IMAGE, 1, VK_SHADER_STAGE_COMPUTE_BIT, nullptr}, {1, VK_DESCRIPTOR_TYPE_STORAGE_IMAGE, 1, VK_SHADER_STAGE_COMPUTE_BIT, nullptr}, {2, VK_DESCRIPTOR_TYPE_STORAGE_IMAGE, 1, VK_SHADER_STAGE_COMPUTE_BIT, nullptr} });
        dsl_divergence_    = mkdsl({ {0, VK_DESCRIPTOR_TYPE_STORAGE_IMAGE, 1, VK_SHADER_STAGE_COMPUTE_BIT, nullptr}, {1, VK_DESCRIPTOR_TYPE_STORAGE_IMAGE, 1, VK_SHADER_STAGE_COMPUTE_BIT, nullptr} });
        dsl_jacobi_        = mkdsl({ {0, VK_DESCRIPTOR_TYPE_STORAGE_IMAGE, 1, VK_SHADER_STAGE_COMPUTE_BIT, nullptr}, {1, VK_DESCRIPTOR_TYPE_STORAGE_IMAGE, 1, VK_SHADER_STAGE_COMPUTE_BIT, nullptr}, {2, VK_DESCRIPTOR_TYPE_STORAGE_IMAGE, 1, VK_SHADER_STAGE_COMPUTE_BIT, nullptr} });
        dsl_gradient_      = mkdsl({ {0, VK_DESCRIPTOR_TYPE_STORAGE_IMAGE, 1, VK_SHADER_STAGE_COMPUTE_BIT, nullptr}, {1, VK_DESCRIPTOR_TYPE_STORAGE_IMAGE, 1, VK_SHADER_STAGE_COMPUTE_BIT, nullptr}, {2, VK_DESCRIPTOR_TYPE_STORAGE_IMAGE, 1, VK_SHADER_STAGE_COMPUTE_BIT, nullptr} });
        dsl_inject_        = mkdsl({ {0, VK_DESCRIPTOR_TYPE_STORAGE_IMAGE, 1, VK_SHADER_STAGE_COMPUTE_BIT, nullptr}, {1, VK_DESCRIPTOR_TYPE_STORAGE_IMAGE, 1, VK_SHADER_STAGE_COMPUTE_BIT, nullptr} });
        dsl_render_        = mkdsl({ {0, VK_DESCRIPTOR_TYPE_STORAGE_IMAGE, 1, VK_SHADER_STAGE_COMPUTE_BIT, nullptr}, {1, VK_DESCRIPTOR_TYPE_STORAGE_IMAGE, 1, VK_SHADER_STAGE_COMPUTE_BIT, nullptr} });
        // Pipeline layouts with push constants 32 bytes (we use up to 32)
        auto mkpl = [&](VkDescriptorSetLayout dsl, uint32_t pcSize){ VkPushConstantRange pcr{VK_SHADER_STAGE_COMPUTE_BIT, 0, pcSize}; VkPipelineLayoutCreateInfo ci{VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO}; ci.setLayoutCount=1; ci.pSetLayouts=&dsl; ci.pushConstantRangeCount=1; ci.pPushConstantRanges=&pcr; VkPipelineLayout l{}; VK_CHECK(vkCreatePipelineLayout(dev_, &ci, nullptr, &l)); return l; };
        pl_advect_vec_    = mkpl(dsl_advect_vec_,    32);
        pl_advect_scalar_ = mkpl(dsl_advect_scalar_, 32);
        pl_divergence_    = mkpl(dsl_divergence_,    32);
        pl_jacobi_        = mkpl(dsl_jacobi_,        32);
        pl_gradient_      = mkpl(dsl_gradient_,      32);
        pl_inject_        = mkpl(dsl_inject_,        32);
        pl_render_        = mkpl(dsl_render_,        24);
        // Pipelines
        auto mkp = [&](VkShaderModule sm, VkPipelineLayout pl){ VkComputePipelineCreateInfo ci{VK_STRUCTURE_TYPE_COMPUTE_PIPELINE_CREATE_INFO}; VkPipelineShaderStageCreateInfo st{VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO}; st.stage=VK_SHADER_STAGE_COMPUTE_BIT; st.module=sm; st.pName="main"; ci.stage=st; ci.layout=pl; VkPipeline p{}; VK_CHECK(vkCreateComputePipelines(dev_, VK_NULL_HANDLE, 1, &ci, nullptr, &p)); return p; };
        p_advect_vec_    = mkp(sm_advect_vec_,    pl_advect_vec_);
        p_advect_scalar_ = mkp(sm_advect_scalar_, pl_advect_scalar_);
        p_divergence_    = mkp(sm_divergence_,    pl_divergence_);
        p_jacobi_        = mkp(sm_jacobi_,        pl_jacobi_);
        p_gradient_      = mkp(sm_gradient_,      pl_gradient_);
        p_inject_        = mkp(sm_inject_,        pl_inject_);
        p_render_        = mkp(sm_render_,        pl_render_);
        // Allocate descriptor sets
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
