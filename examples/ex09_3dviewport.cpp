#include "vk_engine.h"
#include "vv_camera.h"
#include <vulkan/vulkan.h>
#include <imgui.h>
#include <fstream>
#include <vector>
#include <memory>
#include <stdexcept>
#include <string>

#ifndef VK_CHECK
#define VK_CHECK(x) do{VkResult r=(x); if(r!=VK_SUCCESS) throw std::runtime_error("Vulkan error: "+std::to_string(r)); }while(false)
#endif

static std::vector<char> load_spv(const std::string& p)
{
    std::ifstream f(p, std::ios::binary | std::ios::ate);
    if (!f) throw std::runtime_error("open "+p);
    size_t s = (size_t)f.tellg();
    f.seekg(0);
    std::vector<char> d(s);
    f.read(d.data(), s);
    return d;
}

static VkShaderModule make_shader(VkDevice d, const std::vector<char>& b)
{
    VkShaderModuleCreateInfo ci{VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO};
    ci.codeSize = b.size();
    ci.pCode = reinterpret_cast<const uint32_t*>(b.data());
    VkShaderModule m{}; VK_CHECK(vkCreateShaderModule(d, &ci, nullptr, &m));
    return m;
}

class Viewport3DRenderer : public IRenderer {
public:
    void get_capabilities(const EngineContext&, RendererCaps& c) override {
        c = RendererCaps{};
        c.enable_imgui = true;
        c.presentation_mode = PresentationMode::EngineBlit;
        c.color_attachments = { AttachmentRequest{ .name = "color", .format = VK_FORMAT_B8G8R8A8_UNORM } };
        c.presentation_attachment = "color";
        // request depth
        c.depth_attachment = AttachmentRequest{ .name = "depth", .format = c.preferred_depth_format, .usage = VK_IMAGE_USAGE_DEPTH_STENCIL_ATTACHMENT_BIT, .samples = VK_SAMPLE_COUNT_1_BIT, .aspect = VK_IMAGE_ASPECT_DEPTH_BIT, .initial_layout = VK_IMAGE_LAYOUT_UNDEFINED };
        c.uses_depth = VK_TRUE;
    }

    void initialize(const EngineContext& e, const RendererCaps& c, const FrameContext&) override {
        dev_ = e.device;
        color_fmt_ = c.color_attachments.front().format;
        depth_fmt_ = c.depth_attachment ? c.depth_attachment->format : VK_FORMAT_D32_SFLOAT;
        std::string dir(SHADER_OUTPUT_DIR);
        VkShaderModule vs = make_shader(dev_, load_spv(dir + "/simple3d.vert.spv"));
        VkShaderModule fs = make_shader(dev_, load_spv(dir + "/simple3d.frag.spv"));

        // Pipeline
        VkPipelineShaderStageCreateInfo st[2]{};
        for (int i = 0; i < 2; ++i) st[i].sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO;
        st[0].stage = VK_SHADER_STAGE_VERTEX_BIT; st[0].module = vs; st[0].pName = "main";
        st[1] = st[0]; st[1].stage = VK_SHADER_STAGE_FRAGMENT_BIT; st[1].module = fs;

        VkPipelineVertexInputStateCreateInfo vi{VK_STRUCTURE_TYPE_PIPELINE_VERTEX_INPUT_STATE_CREATE_INFO};
        VkPipelineInputAssemblyStateCreateInfo ia{VK_STRUCTURE_TYPE_PIPELINE_INPUT_ASSEMBLY_STATE_CREATE_INFO}; ia.topology = VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST;
        VkPipelineViewportStateCreateInfo vp{VK_STRUCTURE_TYPE_PIPELINE_VIEWPORT_STATE_CREATE_INFO}; vp.viewportCount = 1; vp.scissorCount = 1;
        VkPipelineRasterizationStateCreateInfo rs{VK_STRUCTURE_TYPE_PIPELINE_RASTERIZATION_STATE_CREATE_INFO};
        rs.polygonMode = VK_POLYGON_MODE_FILL; rs.cullMode = VK_CULL_MODE_BACK_BIT; rs.frontFace = VK_FRONT_FACE_COUNTER_CLOCKWISE; rs.lineWidth = 1.0f;
        VkPipelineMultisampleStateCreateInfo ms{VK_STRUCTURE_TYPE_PIPELINE_MULTISAMPLE_STATE_CREATE_INFO}; ms.rasterizationSamples = VK_SAMPLE_COUNT_1_BIT;
        VkPipelineDepthStencilStateCreateInfo ds{VK_STRUCTURE_TYPE_PIPELINE_DEPTH_STENCIL_STATE_CREATE_INFO};
        ds.depthTestEnable = VK_TRUE; ds.depthWriteEnable = VK_TRUE; ds.depthCompareOp = VK_COMPARE_OP_LESS;
        VkPipelineColorBlendAttachmentState ba{}; ba.colorWriteMask = 0xF; // no blending
        VkPipelineColorBlendStateCreateInfo cb{VK_STRUCTURE_TYPE_PIPELINE_COLOR_BLEND_STATE_CREATE_INFO}; cb.attachmentCount = 1; cb.pAttachments = &ba;
        const VkDynamicState dyns[2] = { VK_DYNAMIC_STATE_VIEWPORT, VK_DYNAMIC_STATE_SCISSOR };
        VkPipelineDynamicStateCreateInfo dsi{VK_STRUCTURE_TYPE_PIPELINE_DYNAMIC_STATE_CREATE_INFO}; dsi.dynamicStateCount = 2; dsi.pDynamicStates = dyns;

        // push constant range for MVP
        VkPushConstantRange pcr{VK_SHADER_STAGE_VERTEX_BIT, 0, 64};
        VkPipelineLayoutCreateInfo lci{VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO}; lci.pushConstantRangeCount = 1; lci.pPushConstantRanges = &pcr;
        VK_CHECK(vkCreatePipelineLayout(dev_, &lci, nullptr, &layout_));

        VkPipelineRenderingCreateInfo ri{VK_STRUCTURE_TYPE_PIPELINE_RENDERING_CREATE_INFO};
        ri.colorAttachmentCount = 1; ri.pColorAttachmentFormats = &color_fmt_; ri.depthAttachmentFormat = depth_fmt_;
        VkGraphicsPipelineCreateInfo pci{VK_STRUCTURE_TYPE_GRAPHICS_PIPELINE_CREATE_INFO};
        pci.pNext = &ri; pci.stageCount = 2; pci.pStages = st; pci.pVertexInputState = &vi; pci.pInputAssemblyState = &ia; pci.pViewportState = &vp; pci.pRasterizationState = &rs; pci.pMultisampleState = &ms; pci.pDepthStencilState = &ds; pci.pColorBlendState = &cb; pci.pDynamicState = &dsi; pci.layout = layout_;
        VK_CHECK(vkCreateGraphicsPipelines(dev_, VK_NULL_HANDLE, 1, &pci, nullptr, &pipe_));
        vkDestroyShaderModule(dev_, vs, nullptr); vkDestroyShaderModule(dev_, fs, nullptr);

        // initial camera
        cam_.set_mode(vv::CameraMode::Orbit);
        vv::CameraState s = cam_.state(); s.target = {0,0,0}; s.distance = 3.5f; s.pitch_deg = 20.0f; s.yaw_deg = -30.0f; s.znear = 0.01f; s.zfar = 100.0f; cam_.set_state(s);
    }

    void destroy(const EngineContext& e, const RendererCaps&) override {
        if (pipe_) vkDestroyPipeline(e.device, pipe_, nullptr);
        if (layout_) vkDestroyPipelineLayout(e.device, layout_, nullptr);
        pipe_ = VK_NULL_HANDLE; layout_ = VK_NULL_HANDLE; dev_ = VK_NULL_HANDLE;
    }

    void update(const EngineContext&, const FrameContext& f) override {
        cam_.update(f.dt_sec, int(f.extent.width), int(f.extent.height));
    }

    void on_event(const SDL_Event& e, const EngineContext& eng, const FrameContext* f) override { cam_.handle_event(e, &eng, f); }

    void record_graphics(VkCommandBuffer cmd, const EngineContext&, const FrameContext& f) override {
        if (!pipe_ || f.color_attachments.empty()) return;
        const auto& color = f.color_attachments.front();
        const auto* depth = f.depth_attachment;

        auto barrier_img = [&](VkImage img, VkImageAspectFlags aspect, VkImageLayout oldL, VkImageLayout newL, VkPipelineStageFlags2 src, VkPipelineStageFlags2 dst, VkAccessFlags2 sa, VkAccessFlags2 da){
            VkImageMemoryBarrier2 b{VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER_2}; b.srcStageMask=src; b.dstStageMask=dst; b.srcAccessMask=sa; b.dstAccessMask=da; b.oldLayout=oldL; b.newLayout=newL; b.image=img; b.subresourceRange={aspect,0,1,0,1};
            VkDependencyInfo di{VK_STRUCTURE_TYPE_DEPENDENCY_INFO}; di.imageMemoryBarrierCount=1; di.pImageMemoryBarriers=&b; vkCmdPipelineBarrier2(cmd, &di);
        };
        barrier_img(color.image, color.aspect, VK_IMAGE_LAYOUT_GENERAL, VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL, VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT, VK_PIPELINE_STAGE_2_COLOR_ATTACHMENT_OUTPUT_BIT, VK_ACCESS_2_MEMORY_WRITE_BIT, VK_ACCESS_2_COLOR_ATTACHMENT_WRITE_BIT);
        if (depth) barrier_img(depth->image, depth->aspect, VK_IMAGE_LAYOUT_UNDEFINED, VK_IMAGE_LAYOUT_DEPTH_ATTACHMENT_OPTIMAL, VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT, VK_PIPELINE_STAGE_2_EARLY_FRAGMENT_TESTS_BIT, 0, VK_ACCESS_2_DEPTH_STENCIL_ATTACHMENT_WRITE_BIT);

        VkClearValue clear_color{.color = {{0.06f, 0.07f, 0.09f, 1.0f}}};
        VkClearValue clear_depth{.depthStencil = {1.0f, 0}};
        VkRenderingAttachmentInfo ca{VK_STRUCTURE_TYPE_RENDERING_ATTACHMENT_INFO}; ca.imageView=color.view; ca.imageLayout=VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL; ca.loadOp=VK_ATTACHMENT_LOAD_OP_CLEAR; ca.storeOp=VK_ATTACHMENT_STORE_OP_STORE; ca.clearValue = clear_color;
        VkRenderingAttachmentInfo da{}; if (depth) { da.sType=VK_STRUCTURE_TYPE_RENDERING_ATTACHMENT_INFO; da.imageView=depth->view; da.imageLayout=VK_IMAGE_LAYOUT_DEPTH_ATTACHMENT_OPTIMAL; da.loadOp=VK_ATTACHMENT_LOAD_OP_CLEAR; da.storeOp=VK_ATTACHMENT_STORE_OP_STORE; da.clearValue=clear_depth; }
        VkRenderingInfo ri{VK_STRUCTURE_TYPE_RENDERING_INFO}; ri.renderArea={{0,0}, f.extent}; ri.layerCount=1; ri.colorAttachmentCount=1; ri.pColorAttachments=&ca; ri.pDepthAttachment = depth? &da : nullptr;
        vkCmdBeginRendering(cmd, &ri);
        vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, pipe_);
        VkViewport vp{}; vp.x=0; vp.y=0; vp.width=float(f.extent.width); vp.height=float(f.extent.height); vp.minDepth=0.0f; vp.maxDepth=1.0f; VkRect2D sc{{0,0}, f.extent};
        vkCmdSetViewport(cmd, 0, 1, &vp); vkCmdSetScissor(cmd, 0, 1, &sc);

        // push MVP
        const vv::float4x4 V = cam_.view_matrix();
        const vv::float4x4 P = cam_.proj_matrix();
        // column-major mul: M (I) * V * P
        vv::float4x4 MVP = vv::mul(P, V); // P*V
        vkCmdPushConstants(cmd, layout_, VK_SHADER_STAGE_VERTEX_BIT, 0, 64, MVP.m.data());
        vkCmdDraw(cmd, 36, 1, 0, 0);
        vkCmdEndRendering(cmd);

        // back to GENERAL for engine blit
        barrier_img(color.image, color.aspect, VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL, VK_IMAGE_LAYOUT_GENERAL, VK_PIPELINE_STAGE_2_COLOR_ATTACHMENT_OUTPUT_BIT, VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT, VK_ACCESS_2_COLOR_ATTACHMENT_WRITE_BIT, VK_ACCESS_2_MEMORY_READ_BIT | VK_ACCESS_2_MEMORY_WRITE_BIT);
    }

    void on_imgui(const EngineContext&, const FrameContext& f) override {
        if (ImGui::Begin("3D Viewport")) {
            ImGui::Text("Use RMB to rotate, MMB/Ctrl+RMB to pan, wheel to zoom.");
            ImGui::Text("WASD/QE to move in Fly mode (toggle in Camera panel).");
            ImGui::Separator();
        }
        ImGui::End();
        cam_.imgui_panel(nullptr);
        cam_.imgui_draw_overlay(int(f.extent.width), int(f.extent.height));
    }

private:
    VkDevice dev_{VK_NULL_HANDLE};
    VkPipelineLayout layout_{VK_NULL_HANDLE};
    VkPipeline pipe_{VK_NULL_HANDLE};
    VkFormat color_fmt_{VK_FORMAT_B8G8R8A8_UNORM};
    VkFormat depth_fmt_{VK_FORMAT_D32_SFLOAT};
    vv::CameraService cam_{};
};

int main(){
    try{
        VulkanEngine e; e.configure_window(1280, 720, "ex09_3dviewport");
        e.set_renderer(std::make_unique<Viewport3DRenderer>());
        e.init(); e.run(); e.cleanup();
    } catch(const std::exception& ex) {
        std::fprintf(stderr, "Fatal: %s\n", ex.what()); return 1;
    }
    return 0;
}

