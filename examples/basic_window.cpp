#include "vk_engine.h"
#include <vulkan/vulkan.h>
#include <cstdio>
#include <cstdlib>
#include <cstdint>
#include <vector>
#include <string>
#include <fstream>
#include <stdexcept>
#include <cstring>
#include <memory>

#ifndef VK_CHECK
#define VK_CHECK(x) do { VkResult _res = (x); if (_res != VK_SUCCESS) { throw std::runtime_error(std::string("Vulkan error ")+ std::to_string(_res) + " at " #x); } } while(false)
#endif

// ----------------------------------------------------------------------------
// 工具函数：加载 SPIR-V 文件 & 创建着色器模块
// ----------------------------------------------------------------------------
static std::vector<char> load_file(const std::string& path) {
    std::ifstream f(path, std::ios::binary | std::ios::ate);
    if (!f) throw std::runtime_error("Failed to open file: " + path);
    size_t sz = (size_t)f.tellg();
    f.seekg(0);
    std::vector<char> data(sz);
    f.read(data.data(), sz);
    return data;
}

static VkShaderModule create_shader_module(VkDevice device, const std::vector<char>& code) {
    VkShaderModuleCreateInfo ci{ VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO };
    ci.codeSize = code.size();
    ci.pCode = reinterpret_cast<const uint32_t*>(code.data());
    VkShaderModule mod{};
    VK_CHECK(vkCreateShaderModule(device, &ci, nullptr, &mod));
    return mod;
}

// ----------------------------------------------------------------------------
// 示例渲染器：绘制一个三角形到 engine 提供的 offscreen image
// ----------------------------------------------------------------------------
class TriangleRenderer final : public IRenderer {
public:
    void initialize(const EngineContext& eng) override {
        device_ = eng.device;

        std::string base = std::string(SHADER_OUTPUT_DIR);
        auto vert = load_file(base + "/triangle.vert.spv");
        auto frag = load_file(base + "/triangle.frag.spv");
        VkShaderModule vertMod = create_shader_module(device_, vert);
        VkShaderModule fragMod = create_shader_module(device_, frag);

        VkPipelineLayoutCreateInfo plci{ VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO };
        VK_CHECK(vkCreatePipelineLayout(device_, &plci, nullptr, &layout_));

        VkPipelineShaderStageCreateInfo stages[2]{};
        stages[0].sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO;
        stages[0].stage = VK_SHADER_STAGE_VERTEX_BIT;
        stages[0].module = vertMod;
        stages[0].pName = "main";
        stages[1].sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO;
        stages[1].stage = VK_SHADER_STAGE_FRAGMENT_BIT;
        stages[1].module = fragMod;
        stages[1].pName = "main";

        VkPipelineVertexInputStateCreateInfo vi{ VK_STRUCTURE_TYPE_PIPELINE_VERTEX_INPUT_STATE_CREATE_INFO };
        VkPipelineInputAssemblyStateCreateInfo ia{ VK_STRUCTURE_TYPE_PIPELINE_INPUT_ASSEMBLY_STATE_CREATE_INFO };
        ia.topology = VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST;

        VkPipelineViewportStateCreateInfo vp{ VK_STRUCTURE_TYPE_PIPELINE_VIEWPORT_STATE_CREATE_INFO };
        vp.viewportCount = 1; vp.scissorCount = 1;

        VkPipelineRasterizationStateCreateInfo rs{ VK_STRUCTURE_TYPE_PIPELINE_RASTERIZATION_STATE_CREATE_INFO };
        rs.polygonMode = VK_POLYGON_MODE_FILL; rs.cullMode = VK_CULL_MODE_NONE; rs.frontFace = VK_FRONT_FACE_COUNTER_CLOCKWISE; rs.lineWidth = 1.0f;

        VkPipelineMultisampleStateCreateInfo ms{ VK_STRUCTURE_TYPE_PIPELINE_MULTISAMPLE_STATE_CREATE_INFO }; ms.rasterizationSamples = VK_SAMPLE_COUNT_1_BIT;
        VkPipelineColorBlendAttachmentState cbatt{}; cbatt.colorWriteMask = 0xF;
        VkPipelineColorBlendStateCreateInfo cb{ VK_STRUCTURE_TYPE_PIPELINE_COLOR_BLEND_STATE_CREATE_INFO }; cb.attachmentCount = 1; cb.pAttachments = &cbatt;

        VkDynamicState dynStatesArr[2]{ VK_DYNAMIC_STATE_VIEWPORT, VK_DYNAMIC_STATE_SCISSOR };
        VkPipelineDynamicStateCreateInfo dyn{ VK_STRUCTURE_TYPE_PIPELINE_DYNAMIC_STATE_CREATE_INFO };
        dyn.dynamicStateCount = 2; dyn.pDynamicStates = dynStatesArr;

        // 这里约定 engine 的 offscreen color 采用常见 UNORM (示例简化)，若未来支持 HDR 可调
        VkFormat colorFormat = VK_FORMAT_B8G8R8A8_UNORM;
        VkPipelineRenderingCreateInfo rendering{ VK_STRUCTURE_TYPE_PIPELINE_RENDERING_CREATE_INFO };
        rendering.colorAttachmentCount = 1; rendering.pColorAttachmentFormats = &colorFormat;

        VkGraphicsPipelineCreateInfo gp{ VK_STRUCTURE_TYPE_GRAPHICS_PIPELINE_CREATE_INFO };
        gp.pNext = &rendering;
        gp.stageCount = 2; gp.pStages = stages;
        gp.pVertexInputState = &vi;
        gp.pInputAssemblyState = &ia;
        gp.pViewportState = &vp;
        gp.pRasterizationState = &rs;
        gp.pMultisampleState = &ms;
        gp.pColorBlendState = &cb;
        gp.pDynamicState = &dyn;
        gp.layout = layout_;
        gp.renderPass = VK_NULL_HANDLE; // 动态渲染
        VK_CHECK(vkCreateGraphicsPipelines(device_, VK_NULL_HANDLE, 1, &gp, nullptr, &pipeline_));

        vkDestroyShaderModule(device_, vertMod, nullptr);
        vkDestroyShaderModule(device_, fragMod, nullptr);
    }

    void destroy(const EngineContext& eng) override {
        if (pipeline_) { vkDestroyPipeline(eng.device, pipeline_, nullptr); pipeline_ = VK_NULL_HANDLE; }
        if (layout_)   { vkDestroyPipelineLayout(eng.device, layout_, nullptr); layout_ = VK_NULL_HANDLE; }
        device_ = VK_NULL_HANDLE;
    }

    void record_graphics(VkCommandBuffer cmd, const EngineContext&, const FrameContext& frm) override {
        if (!pipeline_) return;

        // offscreen image: GENERAL -> COLOR_ATTACHMENT
        VkImageMemoryBarrier2 toColor{ VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER_2 };
        toColor.srcStageMask = VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT;
        toColor.srcAccessMask = VK_ACCESS_2_MEMORY_WRITE_BIT;
        toColor.dstStageMask = VK_PIPELINE_STAGE_2_COLOR_ATTACHMENT_OUTPUT_BIT;
        toColor.dstAccessMask = VK_ACCESS_2_COLOR_ATTACHMENT_WRITE_BIT;
        toColor.oldLayout = VK_IMAGE_LAYOUT_GENERAL;
        toColor.newLayout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL;
        toColor.image = frm.offscreen_image;
        toColor.subresourceRange = { VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1 };
        VkDependencyInfo dep{ VK_STRUCTURE_TYPE_DEPENDENCY_INFO }; dep.imageMemoryBarrierCount = 1; dep.pImageMemoryBarriers = &toColor;
        vkCmdPipelineBarrier2(cmd, &dep);

        VkClearValue clear{ {{0.05f, 0.07f, 0.12f, 1.0f}} };
        VkRenderingAttachmentInfo att{ VK_STRUCTURE_TYPE_RENDERING_ATTACHMENT_INFO };
        att.imageView = frm.offscreen_image_view;
        att.imageLayout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL;
        att.loadOp = VK_ATTACHMENT_LOAD_OP_CLEAR;
        att.storeOp = VK_ATTACHMENT_STORE_OP_STORE;
        att.clearValue = clear;

        VkRenderingInfo ri{ VK_STRUCTURE_TYPE_RENDERING_INFO };
        ri.renderArea = { {0,0}, frm.extent };
        ri.layerCount = 1;
        ri.colorAttachmentCount = 1;
        ri.pColorAttachments = &att;
        vkCmdBeginRendering(cmd, &ri);

        vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, pipeline_);
        VkViewport vp{}; vp.x = 0; vp.y = 0; vp.width = (float)frm.extent.width; vp.height = (float)frm.extent.height; vp.minDepth=0.f; vp.maxDepth=1.f;
        VkRect2D sc{ {0,0}, frm.extent };
        vkCmdSetViewport(cmd, 0, 1, &vp);
        vkCmdSetScissor(cmd, 0, 1, &sc);
        vkCmdDraw(cmd, 3, 1, 0, 0);
        vkCmdEndRendering(cmd);

        // COLOR_ATTACHMENT -> GENERAL 供 engine 后续 blit
        VkImageMemoryBarrier2 toGeneral{ VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER_2 };
        toGeneral.srcStageMask = VK_PIPELINE_STAGE_2_COLOR_ATTACHMENT_OUTPUT_BIT;
        toGeneral.srcAccessMask = VK_ACCESS_2_COLOR_ATTACHMENT_WRITE_BIT;
        toGeneral.dstStageMask = VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT;
        toGeneral.dstAccessMask = VK_ACCESS_2_MEMORY_READ_BIT | VK_ACCESS_2_MEMORY_WRITE_BIT;
        toGeneral.oldLayout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL;
        toGeneral.newLayout = VK_IMAGE_LAYOUT_GENERAL;
        toGeneral.image = frm.offscreen_image;
        toGeneral.subresourceRange = { VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1 };
        VkDependencyInfo dep2{ VK_STRUCTURE_TYPE_DEPENDENCY_INFO }; dep2.imageMemoryBarrierCount = 1; dep2.pImageMemoryBarriers = &toGeneral;
        vkCmdPipelineBarrier2(cmd, &dep2);
    }

    void get_capabilities(RendererCaps& caps) const override {
        caps = RendererCaps{}; // 使用默认 (dynamic rendering + offscreen)
    }

private:
    VkDevice device_{VK_NULL_HANDLE};
    VkPipelineLayout layout_{VK_NULL_HANDLE};
    VkPipeline pipeline_{VK_NULL_HANDLE};
};

int main() {
    try {
        VulkanEngine engine;
        engine.state_.name = "VulkanVisualizer Triangle (C++ IRenderer)";
        engine.state_.width = 1280;
        engine.state_.height = 720;
        engine.set_renderer(std::make_unique<TriangleRenderer>());
        engine.init();
        engine.run();
        engine.cleanup();
    } catch (const std::exception& e) {
        std::fprintf(stderr, "Fatal error: %s\n", e.what());
        return EXIT_FAILURE;
    }
    return EXIT_SUCCESS;
}
