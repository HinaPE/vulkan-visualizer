#include "vk_engine.h"
#include <vulkan/vulkan.h>
#include <cstdio>
#include <fstream>
#include <vector>
#include <stdexcept>
#include <string>
#include <array>

#ifndef VK_CHECK
#define VK_CHECK(x) do { VkResult _res = (x); if (_res != VK_SUCCESS) throw std::runtime_error(std::string("Vulkan error ")+ std::to_string(_res) + " at " #x); } while(false)
#endif

// Helper: load SPIR-V binary
static std::vector<char> load_file(const std::string& path) {
    std::ifstream f(path, std::ios::binary | std::ios::ate);
    if (!f) throw std::runtime_error("Failed to open file: " + path);
    size_t sz = (size_t)f.tellg();
    f.seekg(0);
    std::vector<char> data(sz);
    f.read(data.data(), sz);
    return data;
}

class TriangleRenderer final : public IRenderer {
public:
    void initialize(const EngineContext& eng) override {
        device_ = eng.device;
        pipeline_layout_ = VK_NULL_HANDLE;

        // Create shader modules
        std::string vertPath = std::string(SHADER_OUTPUT_DIR) + "/triangle.vert.spv";
        std::string fragPath = std::string(SHADER_OUTPUT_DIR) + "/triangle.frag.spv";
        auto vertCode = load_file(vertPath);
        auto fragCode = load_file(fragPath);
        VkShaderModule vertModule = create_shader_module(vertCode);
        VkShaderModule fragModule = create_shader_module(fragCode);

        // Pipeline layout (no descriptors for this simple example)
        VkPipelineLayoutCreateInfo plci{ VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO };
        VK_CHECK(vkCreatePipelineLayout(device_, &plci, nullptr, &pipeline_layout_));

        // Dynamic rendering pipeline (no vertex buffers -> use gl_VertexIndex)
        VkPipelineShaderStageCreateInfo stages[2] = {};
        stages[0].sType  = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO;
        stages[0].stage  = VK_SHADER_STAGE_VERTEX_BIT;
        stages[0].module = vertModule;
        stages[0].pName  = "main";
        stages[1].sType  = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO;
        stages[1].stage  = VK_SHADER_STAGE_FRAGMENT_BIT;
        stages[1].module = fragModule;
        stages[1].pName  = "main";

        VkPipelineVertexInputStateCreateInfo vi{ VK_STRUCTURE_TYPE_PIPELINE_VERTEX_INPUT_STATE_CREATE_INFO };
        VkPipelineInputAssemblyStateCreateInfo ia{ VK_STRUCTURE_TYPE_PIPELINE_INPUT_ASSEMBLY_STATE_CREATE_INFO };
        ia.topology = VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST;

        VkPipelineViewportStateCreateInfo vp{ VK_STRUCTURE_TYPE_PIPELINE_VIEWPORT_STATE_CREATE_INFO };
        vp.viewportCount = 1; vp.scissorCount  = 1;

        VkPipelineRasterizationStateCreateInfo rs{ VK_STRUCTURE_TYPE_PIPELINE_RASTERIZATION_STATE_CREATE_INFO };
        rs.polygonMode = VK_POLYGON_MODE_FILL; rs.cullMode = VK_CULL_MODE_NONE; rs.frontFace = VK_FRONT_FACE_COUNTER_CLOCKWISE; rs.lineWidth = 1.0f;

        VkPipelineMultisampleStateCreateInfo ms{ VK_STRUCTURE_TYPE_PIPELINE_MULTISAMPLE_STATE_CREATE_INFO };
        ms.rasterizationSamples = VK_SAMPLE_COUNT_1_BIT;

        VkPipelineColorBlendAttachmentState cbatt{}; cbatt.colorWriteMask = 0xF;
        VkPipelineColorBlendStateCreateInfo cb{ VK_STRUCTURE_TYPE_PIPELINE_COLOR_BLEND_STATE_CREATE_INFO }; cb.attachmentCount = 1; cb.pAttachments = &cbatt;

        // Only core dynamic states viewport & scissor to avoid needing extra extensions
        std::array<VkDynamicState,2> dynStates{ VK_DYNAMIC_STATE_VIEWPORT, VK_DYNAMIC_STATE_SCISSOR };
        VkPipelineDynamicStateCreateInfo dyn{ VK_STRUCTURE_TYPE_PIPELINE_DYNAMIC_STATE_CREATE_INFO }; dyn.dynamicStateCount = (uint32_t)dynStates.size(); dyn.pDynamicStates = dynStates.data();

        VkFormat colorFormat = VK_FORMAT_B8G8R8A8_UNORM;

        VkPipelineRenderingCreateInfo rendering{ VK_STRUCTURE_TYPE_PIPELINE_RENDERING_CREATE_INFO };
        rendering.colorAttachmentCount = 1; rendering.pColorAttachmentFormats = &colorFormat;

        VkGraphicsPipelineCreateInfo gp{ VK_STRUCTURE_TYPE_GRAPHICS_PIPELINE_CREATE_INFO };
        gp.pNext=&rendering; gp.stageCount=2; gp.pStages=stages; gp.pVertexInputState=&vi; gp.pInputAssemblyState=&ia; gp.pViewportState=&vp; gp.pRasterizationState=&rs; gp.pMultisampleState=&ms; gp.pColorBlendState=&cb; gp.pDepthStencilState=nullptr; gp.pDynamicState=&dyn; gp.layout=pipeline_layout_; gp.renderPass=VK_NULL_HANDLE; gp.subpass=0;
        VK_CHECK(vkCreateGraphicsPipelines(device_, VK_NULL_HANDLE, 1, &gp, nullptr, &pipeline_));

        // Destroy temporary shader modules
        vkDestroyShaderModule(device_, vertModule, nullptr);
        vkDestroyShaderModule(device_, fragModule, nullptr);
    }

    void destroy(const EngineContext& eng) override {
        if (pipeline_)        vkDestroyPipeline(eng.device, pipeline_, nullptr);
        if (pipeline_layout_) vkDestroyPipelineLayout(eng.device, pipeline_layout_, nullptr);
        pipeline_ = VK_NULL_HANDLE;
        pipeline_layout_ = VK_NULL_HANDLE;
    }

    void get_capabilities(RendererCaps& caps) const override {
        caps.uses_offscreen = VK_TRUE; // We blit from offscreen to swapchain already handled by engine
        caps.uses_depth = VK_FALSE;
    }

    void record_graphics(VkCommandBuffer cmd, const EngineContext& eng, const FrameContext& frm) override {
        // Transition offscreen image to general (already expected general) & begin rendering
        // We assume engine created offscreen with GENERAL layout after renderer writes.

        VkClearValue clearColor{ { {0.05f, 0.07f, 0.12f, 1.0f} } };

        // Transition to COLOR_ATTACHMENT_OPTIMAL for dynamic rendering
        VkImageMemoryBarrier2 toColor{ VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER_2 };
        toColor.srcStageMask = VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT;
        toColor.srcAccessMask = VK_ACCESS_2_MEMORY_WRITE_BIT;
        toColor.dstStageMask = VK_PIPELINE_STAGE_2_COLOR_ATTACHMENT_OUTPUT_BIT;
        toColor.dstAccessMask = VK_ACCESS_2_COLOR_ATTACHMENT_WRITE_BIT;
        toColor.oldLayout = VK_IMAGE_LAYOUT_GENERAL;
        toColor.newLayout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL;
        toColor.image = frm.offscreen_image;
        toColor.subresourceRange = { VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1 };
        VkDependencyInfo dep{ VK_STRUCTURE_TYPE_DEPENDENCY_INFO };
        dep.imageMemoryBarrierCount = 1; dep.pImageMemoryBarriers = &toColor;
        vkCmdPipelineBarrier2(cmd, &dep);

        VkRenderingAttachmentInfo colorAtt{ VK_STRUCTURE_TYPE_RENDERING_ATTACHMENT_INFO };
        colorAtt.imageView = frm.offscreen_image_view;
        colorAtt.imageLayout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL;
        colorAtt.loadOp = VK_ATTACHMENT_LOAD_OP_CLEAR;
        colorAtt.storeOp = VK_ATTACHMENT_STORE_OP_STORE;
        colorAtt.clearValue = clearColor;

        VkRenderingInfo ri{ VK_STRUCTURE_TYPE_RENDERING_INFO };
        ri.renderArea = { {0,0}, frm.extent };
        ri.layerCount = 1;
        ri.colorAttachmentCount = 1;
        ri.pColorAttachments = &colorAtt;
        vkCmdBeginRendering(cmd, &ri);

        vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, pipeline_);

        VkViewport vp{}; vp.x=0; vp.y=0; vp.width=(float)frm.extent.width; vp.height=(float)frm.extent.height; vp.minDepth=0.f; vp.maxDepth=1.f;
        VkRect2D sc{ {0,0}, frm.extent };
        vkCmdSetViewport(cmd, 0, 1, &vp);
        vkCmdSetScissor(cmd, 0, 1, &sc);

        vkCmdDraw(cmd, 3, 1, 0, 0);

        vkCmdEndRendering(cmd);

        // Transition back to GENERAL for engine blit pass
        VkImageMemoryBarrier2 toGeneral{ VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER_2 };
        toGeneral.srcStageMask = VK_PIPELINE_STAGE_2_COLOR_ATTACHMENT_OUTPUT_BIT;
        toGeneral.srcAccessMask = VK_ACCESS_2_COLOR_ATTACHMENT_WRITE_BIT;
        toGeneral.dstStageMask = VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT;
        toGeneral.dstAccessMask = VK_ACCESS_2_MEMORY_READ_BIT | VK_ACCESS_2_MEMORY_WRITE_BIT;
        toGeneral.oldLayout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL;
        toGeneral.newLayout = VK_IMAGE_LAYOUT_GENERAL;
        toGeneral.image = frm.offscreen_image;
        toGeneral.subresourceRange = { VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1 };
        VkDependencyInfo dep2{ VK_STRUCTURE_TYPE_DEPENDENCY_INFO };
        dep2.imageMemoryBarrierCount = 1; dep2.pImageMemoryBarriers = &toGeneral;
        vkCmdPipelineBarrier2(cmd, &dep2);
    }

private:
    VkShaderModule create_shader_module(const std::vector<char>& code) {
        VkShaderModuleCreateInfo ci{ VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO };
        ci.codeSize = code.size(); ci.pCode = reinterpret_cast<const uint32_t*>(code.data());
        VkShaderModule mod{}; VK_CHECK(vkCreateShaderModule(device_, &ci, nullptr, &mod)); return mod; }

    VkDevice device_{};
    VkPipelineLayout pipeline_layout_{VK_NULL_HANDLE};
    VkPipeline pipeline_{VK_NULL_HANDLE};
};

int main() {
    try {
        VulkanEngine engine;
        engine.set_renderer(std::make_unique<TriangleRenderer>());
        engine.init();
        engine.run();
        engine.cleanup();
    } catch (const std::exception& e) {
        std::fprintf(stderr, "Fatal error: %s\n", e.what());
        return 1;
    }
    return 0;
}
