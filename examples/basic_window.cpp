#include "vk_abi.h"
#include <vulkan/vulkan.h>
#include <cstdio>
#include <cstdlib>
#include <cstdint>
#include <vector>
#include <string>
#include <fstream>
#include <stdexcept>
#include <cstring>

#ifndef VK_CHECK
#define VK_CHECK(x) do { VkResult _res = (x); if (_res != VK_SUCCESS) { throw std::runtime_error(std::string("Vulkan error ")+ std::to_string(_res) + " at " #x); } } while(false)
#endif

// -----------------------------------------------------------------------------
// Simple triangle renderer state stored in static (for demo) – in real usage
// you would wrap this in a struct and pass as user_data.
// -----------------------------------------------------------------------------
struct TriangleState {
    VkDevice device{VK_NULL_HANDLE};
    VkPipelineLayout layout{VK_NULL_HANDLE};
    VkPipeline pipeline{VK_NULL_HANDLE};
};

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

// -----------------------------------------------------------------------------
// Callback implementations (C ABI)
// -----------------------------------------------------------------------------
static VKVizResult TRI_initialize(const VKVizEngineContext* eng, void* user_data) {
    if (!eng || !user_data) return VKVIZ_ERROR_INVALID_ARGUMENT;
    TriangleState* st = static_cast<TriangleState*>(user_data);
    st->device = eng->device;

    std::string base = std::string(SHADER_OUTPUT_DIR);
    std::string vertPath = base + "/triangle.vert.spv";
    std::string fragPath = base + "/triangle.frag.spv";

    auto vert = load_file(vertPath);
    auto frag = load_file(fragPath);
    VkShaderModule vertMod = create_shader_module(st->device, vert);
    VkShaderModule fragMod = create_shader_module(st->device, frag);

    VkPipelineLayoutCreateInfo plci{ VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO };
    VK_CHECK(vkCreatePipelineLayout(st->device, &plci, nullptr, &st->layout));

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

    VkFormat colorFormat = VK_FORMAT_B8G8R8A8_UNORM; // engine blits offscreen -> swapchain; pipeline uses generic format
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
    gp.layout = st->layout;
    gp.renderPass = VK_NULL_HANDLE;
    VK_CHECK(vkCreateGraphicsPipelines(st->device, VK_NULL_HANDLE, 1, &gp, nullptr, &st->pipeline));

    vkDestroyShaderModule(st->device, vertMod, nullptr);
    vkDestroyShaderModule(st->device, fragMod, nullptr);
    return VKVIZ_SUCCESS;
}

static void TRI_destroy(const VKVizEngineContext* eng, void* user_data) {
    if (!user_data) return; TriangleState* st = static_cast<TriangleState*>(user_data);
    if (st->device) {
        if (st->pipeline) vkDestroyPipeline(st->device, st->pipeline, nullptr);
        if (st->layout) vkDestroyPipelineLayout(st->device, st->layout, nullptr);
    }
    st->pipeline = VK_NULL_HANDLE; st->layout = VK_NULL_HANDLE; st->device = VK_NULL_HANDLE;
}

static void TRI_record(VkCommandBuffer cmd, const VKVizEngineContext* eng, const VKVizFrameContext* frm, void* user_data) {
    if (!cmd || !frm || !user_data) return;
    TriangleState* st = static_cast<TriangleState*>(user_data);
    if (!st->pipeline) return;

    // Transition offscreen image to color attachment
    VkImageMemoryBarrier2 toColor{ VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER_2 };
    toColor.srcStageMask = VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT;
    toColor.srcAccessMask = VK_ACCESS_2_MEMORY_WRITE_BIT;
    toColor.dstStageMask = VK_PIPELINE_STAGE_2_COLOR_ATTACHMENT_OUTPUT_BIT;
    toColor.dstAccessMask = VK_ACCESS_2_COLOR_ATTACHMENT_WRITE_BIT;
    toColor.oldLayout = VK_IMAGE_LAYOUT_GENERAL;
    toColor.newLayout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL;
    toColor.image = frm->offscreen_image;
    toColor.subresourceRange = { VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1 };
    VkDependencyInfo dep{ VK_STRUCTURE_TYPE_DEPENDENCY_INFO }; dep.imageMemoryBarrierCount = 1; dep.pImageMemoryBarriers = &toColor;
    vkCmdPipelineBarrier2(cmd, &dep);

    VkClearValue clear{ {{0.05f, 0.07f, 0.12f, 1.0f}} };
    VkRenderingAttachmentInfo att{ VK_STRUCTURE_TYPE_RENDERING_ATTACHMENT_INFO };
    att.imageView = frm->offscreen_image_view;
    att.imageLayout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL;
    att.loadOp = VK_ATTACHMENT_LOAD_OP_CLEAR;
    att.storeOp = VK_ATTACHMENT_STORE_OP_STORE;
    att.clearValue = clear;

    VkRenderingInfo ri{ VK_STRUCTURE_TYPE_RENDERING_INFO };
    ri.renderArea = { {0,0}, frm->extent };
    ri.layerCount = 1;
    ri.colorAttachmentCount = 1;
    ri.pColorAttachments = &att;
    vkCmdBeginRendering(cmd, &ri);

    vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, st->pipeline);
    VkViewport vp{}; vp.x = 0; vp.y = 0; vp.width = (float)frm->extent.width; vp.height = (float)frm->extent.height; vp.minDepth=0.f; vp.maxDepth=1.f;
    VkRect2D sc{ {0,0}, frm->extent };
    vkCmdSetViewport(cmd, 0, 1, &vp);
    vkCmdSetScissor(cmd, 0, 1, &sc);
    vkCmdDraw(cmd, 3, 1, 0, 0);
    vkCmdEndRendering(cmd);

    // Transition back to general for engine blit
    VkImageMemoryBarrier2 toGeneral{ VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER_2 };
    toGeneral.srcStageMask = VK_PIPELINE_STAGE_2_COLOR_ATTACHMENT_OUTPUT_BIT;
    toGeneral.srcAccessMask = VK_ACCESS_2_COLOR_ATTACHMENT_WRITE_BIT;
    toGeneral.dstStageMask = VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT;
    toGeneral.dstAccessMask = VK_ACCESS_2_MEMORY_READ_BIT | VK_ACCESS_2_MEMORY_WRITE_BIT;
    toGeneral.oldLayout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL;
    toGeneral.newLayout = VK_IMAGE_LAYOUT_GENERAL;
    toGeneral.image = frm->offscreen_image;
    toGeneral.subresourceRange = { VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1 };
    VkDependencyInfo dep2{ VK_STRUCTURE_TYPE_DEPENDENCY_INFO }; dep2.imageMemoryBarrierCount = 1; dep2.pImageMemoryBarriers = &toGeneral;
    vkCmdPipelineBarrier2(cmd, &dep2);
}

int main() {
    TriangleState tri{};

    VKVizEngineCreateInfo ci{};
    ci.struct_size = sizeof(ci);
    ci.app_name = "VKViz Triangle (C ABI)";
    ci.window_width = 1280;
    ci.window_height = 720;
    ci.enable_imgui = 1;

    VKVizEngine* engine = nullptr;
    VKVizResult r = VKViz_CreateEngine(&ci, &engine);
    if (r != VKVIZ_SUCCESS) { std::fprintf(stderr, "CreateEngine failed: %s\n", VKViz_ResultToString(r)); return 1; }

    VKVizRendererCallbacks cbs{};
    cbs.struct_size = sizeof(cbs);
    cbs.initialize = &TRI_initialize;
    cbs.destroy = &TRI_destroy;
    cbs.record_graphics = &TRI_record; // mandatory for drawing

    r = VKViz_SetRenderer(engine, &cbs, &tri);
    if (r != VKVIZ_SUCCESS) { std::fprintf(stderr, "SetRenderer failed: %s\n", VKViz_ResultToString(r)); VKViz_DestroyEngine(engine); return 1; }

    r = VKViz_Init(engine);
    if (r != VKVIZ_SUCCESS) { std::fprintf(stderr, "Init failed: %s\n", VKViz_ResultToString(r)); VKViz_DestroyEngine(engine); return 1; }

    r = VKViz_Run(engine);
    if (r != VKVIZ_SUCCESS) { std::fprintf(stderr, "Run failed: %s\n", VKViz_ResultToString(r)); }

    VKViz_DestroyEngine(engine);
    return 0;
}
