#include "vk_engine.h"
#include <vulkan/vulkan.h>
#include <cstdio>
#include <cstdlib>
#include <fstream>
#include <memory>
#include <stdexcept>
#include <string>
#include <string_view>
#include <vector>

#ifndef VK_CHECK
#define VK_CHECK(x) do { VkResult _res = (x); if (_res != VK_SUCCESS) throw std::runtime_error("Vulkan error: " + std::to_string(_res)); } while(false)
#endif

namespace {

std::vector<char> load_binary(std::string_view path) {
    std::ifstream f(std::string(path), std::ios::binary | std::ios::ate);
    if (!f) throw std::runtime_error("open fail: " + std::string(path));
    auto sz = f.tellg();
    if (sz <= 0) throw std::runtime_error("empty file: " + std::string(path));
    f.seekg(0);
    std::vector<char> data(static_cast<size_t>(sz));
    f.read(data.data(), sz);
    if (!f) throw std::runtime_error("read fail: " + std::string(path));
    return data;
}

VkShaderModule make_shader(VkDevice dev, const std::vector<char>& bytes) {
    VkShaderModuleCreateInfo ci{VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO};
    ci.codeSize = bytes.size();
    ci.pCode    = reinterpret_cast<const uint32_t*>(bytes.data());
    VkShaderModule mod{}; VK_CHECK(vkCreateShaderModule(dev, &ci, nullptr, &mod)); return mod;
}

class TriangleRenderer final : public IRenderer {
public:
    void query_required_device_caps(RendererCaps& caps) override { caps.allow_async_compute = false; }

    void get_capabilities(const EngineContext&, RendererCaps& caps) override {
        caps = RendererCaps{};
        caps.presentation_mode          = PresentationMode::EngineBlit;
        caps.preferred_swapchain_format = VK_FORMAT_B8G8R8A8_UNORM;
        caps.color_attachments = { AttachmentRequest{ .name = "color", .format = VK_FORMAT_B8G8R8A8_UNORM, .usage = VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT | VK_IMAGE_USAGE_TRANSFER_SRC_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT, .samples = VK_SAMPLE_COUNT_1_BIT, .aspect = VK_IMAGE_ASPECT_COLOR_BIT, .initial_layout = VK_IMAGE_LAYOUT_GENERAL } };
        caps.presentation_attachment = "color";
    }

    void initialize(const EngineContext& eng, const RendererCaps& caps, const FrameContext&) override {
        device_       = eng.device;
        color_format_ = caps.color_attachments.empty() ? VK_FORMAT_B8G8R8A8_UNORM : caps.color_attachments.front().format;
        create_graphics_pipeline();
    }

    void destroy(const EngineContext& eng, const RendererCaps&) override {
        if (pipeline_) vkDestroyPipeline(eng.device, pipeline_, nullptr);
        if (layout_)   vkDestroyPipelineLayout(eng.device, layout_, nullptr);
        pipeline_ = VK_NULL_HANDLE; layout_ = VK_NULL_HANDLE; device_ = VK_NULL_HANDLE;
    }

    void record_graphics(VkCommandBuffer cmd, const EngineContext&, const FrameContext& frm) override {
        if (!pipeline_ || frm.color_attachments.empty()) return;
        const AttachmentView& target = frm.color_attachments.front();

        auto barrier = [&](VkImageLayout oldL, VkImageLayout newL, VkPipelineStageFlags2 srcStage, VkPipelineStageFlags2 dstStage, VkAccessFlags2 srcAccess, VkAccessFlags2 dstAccess){
            VkImageMemoryBarrier2 b{VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER_2};
            b.srcStageMask = srcStage; b.dstStageMask = dstStage; b.srcAccessMask = srcAccess; b.dstAccessMask = dstAccess; b.oldLayout = oldL; b.newLayout = newL; b.image = target.image; b.subresourceRange = {target.aspect,0u,1u,0u,1u};
            VkDependencyInfo di{VK_STRUCTURE_TYPE_DEPENDENCY_INFO}; di.imageMemoryBarrierCount=1; di.pImageMemoryBarriers=&b; vkCmdPipelineBarrier2(cmd,&di);
        };

        barrier(VK_IMAGE_LAYOUT_GENERAL, VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL,
                VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT, VK_PIPELINE_STAGE_2_COLOR_ATTACHMENT_OUTPUT_BIT,
                VK_ACCESS_2_MEMORY_WRITE_BIT, VK_ACCESS_2_COLOR_ATTACHMENT_WRITE_BIT);

        VkClearValue clear{.color={{0.05f,0.07f,0.12f,1.0f}}};
        VkRenderingAttachmentInfo color{VK_STRUCTURE_TYPE_RENDERING_ATTACHMENT_INFO};
        color.imageView = target.view; color.imageLayout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL; color.loadOp = VK_ATTACHMENT_LOAD_OP_CLEAR; color.storeOp = VK_ATTACHMENT_STORE_OP_STORE; color.clearValue = clear;
        VkRenderingInfo ri{VK_STRUCTURE_TYPE_RENDERING_INFO}; ri.renderArea = {{0,0}, frm.extent}; ri.layerCount = 1; ri.colorAttachmentCount = 1; ri.pColorAttachments = &color;

        vkCmdBeginRendering(cmd, &ri);
        vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, pipeline_);
        VkViewport vp{}; vp.width = float(frm.extent.width); vp.height = float(frm.extent.height); vp.minDepth = 0.f; vp.maxDepth = 1.f;
        VkRect2D sc{{0,0}, frm.extent};
        vkCmdSetViewport(cmd,0,1,&vp); vkCmdSetScissor(cmd,0,1,&sc); vkCmdDraw(cmd,3,1,0,0); vkCmdEndRendering(cmd);

        barrier(VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL, VK_IMAGE_LAYOUT_GENERAL,
                VK_PIPELINE_STAGE_2_COLOR_ATTACHMENT_OUTPUT_BIT, VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT,
                VK_ACCESS_2_COLOR_ATTACHMENT_WRITE_BIT, VK_ACCESS_2_MEMORY_READ_BIT | VK_ACCESS_2_MEMORY_WRITE_BIT);
    }

    void reload_assets(const EngineContext& eng) override {
        if (!eng.device) return;
        if (pipeline_) vkDestroyPipeline(eng.device, pipeline_, nullptr);
        if (layout_)   vkDestroyPipelineLayout(eng.device, layout_, nullptr);
        pipeline_ = VK_NULL_HANDLE; layout_ = VK_NULL_HANDLE;
        device_ = eng.device;
        try { create_graphics_pipeline(); } catch(...) {}
    }

private:
    void create_graphics_pipeline() {
        const std::string dir = std::string(SHADER_OUTPUT_DIR);
        VkShaderModule vert = make_shader(device_, load_binary(dir + "/triangle.vert.spv"));
        VkShaderModule frag = make_shader(device_, load_binary(dir + "/triangle.frag.spv"));

        VkPipelineShaderStageCreateInfo stages[2]{};
        stages[0].sType  = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO;
        stages[0].stage  = VK_SHADER_STAGE_VERTEX_BIT;
        stages[0].module = vert;
        stages[0].pName  = "main";
        stages[1].sType  = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO;
        stages[1].stage  = VK_SHADER_STAGE_FRAGMENT_BIT;
        stages[1].module = frag;
        stages[1].pName  = "main";

        VkPipelineLayoutCreateInfo layout_ci{VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO};
        VK_CHECK(vkCreatePipelineLayout(device_, &layout_ci, nullptr, &layout_));

        VkPipelineVertexInputStateCreateInfo vi{VK_STRUCTURE_TYPE_PIPELINE_VERTEX_INPUT_STATE_CREATE_INFO};
        VkPipelineInputAssemblyStateCreateInfo ia{VK_STRUCTURE_TYPE_PIPELINE_INPUT_ASSEMBLY_STATE_CREATE_INFO};
        ia.topology = VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST;
        VkPipelineViewportStateCreateInfo vp{VK_STRUCTURE_TYPE_PIPELINE_VIEWPORT_STATE_CREATE_INFO};
        vp.viewportCount = 1; vp.scissorCount = 1;
        VkPipelineRasterizationStateCreateInfo rs{VK_STRUCTURE_TYPE_PIPELINE_RASTERIZATION_STATE_CREATE_INFO};
        rs.polygonMode = VK_POLYGON_MODE_FILL; rs.cullMode = VK_CULL_MODE_NONE; rs.frontFace = VK_FRONT_FACE_COUNTER_CLOCKWISE; rs.lineWidth = 1.0f;
        VkPipelineMultisampleStateCreateInfo ms{VK_STRUCTURE_TYPE_PIPELINE_MULTISAMPLE_STATE_CREATE_INFO};
        ms.rasterizationSamples = VK_SAMPLE_COUNT_1_BIT;
        VkPipelineColorBlendAttachmentState ba{}; ba.colorWriteMask = VK_COLOR_COMPONENT_R_BIT|VK_COLOR_COMPONENT_G_BIT|VK_COLOR_COMPONENT_B_BIT|VK_COLOR_COMPONENT_A_BIT;
        VkPipelineColorBlendStateCreateInfo cb{VK_STRUCTURE_TYPE_PIPELINE_COLOR_BLEND_STATE_CREATE_INFO}; cb.attachmentCount = 1; cb.pAttachments = &ba;
        const VkDynamicState dyn_states[2] = {VK_DYNAMIC_STATE_VIEWPORT, VK_DYNAMIC_STATE_SCISSOR};
        VkPipelineDynamicStateCreateInfo dyn{VK_STRUCTURE_TYPE_PIPELINE_DYNAMIC_STATE_CREATE_INFO}; dyn.dynamicStateCount = 2; dyn.pDynamicStates = dyn_states;
        VkPipelineRenderingCreateInfo rendering{VK_STRUCTURE_TYPE_PIPELINE_RENDERING_CREATE_INFO};
        rendering.colorAttachmentCount = 1; rendering.pColorAttachmentFormats = &color_format_;

        VkGraphicsPipelineCreateInfo pci{VK_STRUCTURE_TYPE_GRAPHICS_PIPELINE_CREATE_INFO};
        pci.pNext               = &rendering;
        pci.stageCount          = 2;
        pci.pStages             = stages;
        pci.pVertexInputState   = &vi;
        pci.pInputAssemblyState = &ia;
        pci.pViewportState      = &vp;
        pci.pRasterizationState = &rs;
        pci.pMultisampleState   = &ms;
        pci.pColorBlendState    = &cb;
        pci.pDynamicState       = &dyn;
        pci.layout              = layout_;
        VK_CHECK(vkCreateGraphicsPipelines(device_, VK_NULL_HANDLE, 1, &pci, nullptr, &pipeline_));

        vkDestroyShaderModule(device_, vert, nullptr);
        vkDestroyShaderModule(device_, frag, nullptr);
    }

private:
    VkDevice device_{VK_NULL_HANDLE};
    VkPipelineLayout layout_{VK_NULL_HANDLE};
    VkPipeline pipeline_{VK_NULL_HANDLE};
    VkFormat color_format_{VK_FORMAT_B8G8R8A8_UNORM};
};

} // namespace

int main() {
    try {
        VulkanEngine engine;
        engine.configure_window(1280, 720, "VulkanVisualizer Triangle");
        engine.set_renderer(std::make_unique<TriangleRenderer>());
#ifdef VV_ENABLE_HOTRELOAD
        engine.add_hot_reload_watch_path(std::string(SHADER_SOURCE_DIR));
        engine.add_hot_reload_watch_path(std::string(SHADER_OUTPUT_DIR));
#endif
        engine.init();
        engine.run();
        engine.cleanup();
    } catch (const std::exception& e) {
        std::fprintf(stderr, "Fatal: %s\n", e.what());
        return EXIT_FAILURE;
    }
    return EXIT_SUCCESS;
}
