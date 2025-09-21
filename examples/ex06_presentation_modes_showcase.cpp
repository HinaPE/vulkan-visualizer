#include "vk_engine.h"
#include <vulkan/vulkan.h>
#include <imgui.h>
#include <fstream>
#include <stdexcept>
#include <string>
#include <vector>
#ifndef VK_CHECK
#define VK_CHECK(x) do{VkResult r=(x);if(r!=VK_SUCCESS)throw std::runtime_error("vk: "+std::to_string(r));}while(false)
#endif
static std::vector<char> rd(const std::string& p)
{
    std::ifstream f(p, std::ios::binary | std::ios::ate);
    if (!f)throw std::runtime_error("open " + p);
    size_t s = (size_t)f.tellg();
    f.seekg(0);
    std::vector<char> d(s);
    f.read(d.data(), s);
    return d;
}

static VkShaderModule sh(VkDevice d, const std::vector<char>& b)
{
    VkShaderModuleCreateInfo ci{VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO};
    ci.codeSize = b.size();
    ci.pCode = reinterpret_cast<const uint32_t*>(b.data());
    VkShaderModule m{};
    VK_CHECK(vkCreateShaderModule(d,&ci,nullptr,&m));
    return m;
}

class R : public IRenderer
{
public:
    void get_capabilities(const EngineContext&, RendererCaps& c) override
    {
        c = RendererCaps{};
        c.enable_imgui = true;
        c.presentation_mode = PresentationMode::RendererComposite;
    }

    void initialize(const EngineContext& e, const RendererCaps&, const FrameContext& init) override
    {
        dev = e.device;
        fmt = init.swapchain_format;
        std::string d(SHADER_OUTPUT_DIR);
        VkShaderModule vs = sh(dev, rd(d + "/triangle.vert.spv")), fs = sh(dev, rd(d + "/triangle.frag.spv"));
        VkPipelineShaderStageCreateInfo st[2]{};
        st[0].sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO;
        st[0].stage = VK_SHADER_STAGE_VERTEX_BIT;
        st[0].module = vs;
        st[0].pName = "main";
        st[1] = st[0];
        st[1].stage = VK_SHADER_STAGE_FRAGMENT_BIT;
        st[1].module = fs;
        VkPipelineLayoutCreateInfo lci{VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO};
        VK_CHECK(vkCreatePipelineLayout(dev,&lci,nullptr,&layout));
        VkPipelineVertexInputStateCreateInfo vi{VK_STRUCTURE_TYPE_PIPELINE_VERTEX_INPUT_STATE_CREATE_INFO};
        VkPipelineInputAssemblyStateCreateInfo ia{VK_STRUCTURE_TYPE_PIPELINE_INPUT_ASSEMBLY_STATE_CREATE_INFO};
        ia.topology = VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST;
        VkPipelineViewportStateCreateInfo vp{VK_STRUCTURE_TYPE_PIPELINE_VIEWPORT_STATE_CREATE_INFO};
        vp.viewportCount = 1;
        vp.scissorCount = 1;
        VkPipelineRasterizationStateCreateInfo rs{VK_STRUCTURE_TYPE_PIPELINE_RASTERIZATION_STATE_CREATE_INFO};
        rs.polygonMode = VK_POLYGON_MODE_FILL;
        rs.cullMode = VK_CULL_MODE_NONE;
        rs.frontFace = VK_FRONT_FACE_COUNTER_CLOCKWISE;
        rs.lineWidth = 1.0f;
        VkPipelineMultisampleStateCreateInfo ms{VK_STRUCTURE_TYPE_PIPELINE_MULTISAMPLE_STATE_CREATE_INFO};
        ms.rasterizationSamples = VK_SAMPLE_COUNT_1_BIT;
        VkPipelineColorBlendAttachmentState ba{};
        ba.colorWriteMask = VK_COLOR_COMPONENT_R_BIT | VK_COLOR_COMPONENT_G_BIT | VK_COLOR_COMPONENT_B_BIT | VK_COLOR_COMPONENT_A_BIT;
        VkPipelineColorBlendStateCreateInfo cb{VK_STRUCTURE_TYPE_PIPELINE_COLOR_BLEND_STATE_CREATE_INFO};
        cb.attachmentCount = 1;
        cb.pAttachments = &ba;
        const VkDynamicState dyns[2] = {VK_DYNAMIC_STATE_VIEWPORT, VK_DYNAMIC_STATE_SCISSOR};
        VkPipelineDynamicStateCreateInfo ds{VK_STRUCTURE_TYPE_PIPELINE_DYNAMIC_STATE_CREATE_INFO};
        ds.dynamicStateCount = 2;
        ds.pDynamicStates = dyns;
        VkPipelineRenderingCreateInfo r{VK_STRUCTURE_TYPE_PIPELINE_RENDERING_CREATE_INFO};
        r.colorAttachmentCount = 1;
        r.pColorAttachmentFormats = &fmt;
        VkGraphicsPipelineCreateInfo pci{VK_STRUCTURE_TYPE_GRAPHICS_PIPELINE_CREATE_INFO};
        pci.pNext = &r;
        pci.stageCount = 2;
        pci.pStages = st;
        pci.pVertexInputState = &vi;
        pci.pInputAssemblyState = &ia;
        pci.pViewportState = &vp;
        pci.pRasterizationState = &rs;
        pci.pMultisampleState = &ms;
        pci.pColorBlendState = &cb;
        pci.pDynamicState = &ds;
        pci.layout = layout;
        VK_CHECK(vkCreateGraphicsPipelines(dev,VK_NULL_HANDLE,1,&pci,nullptr,&pipe));
        vkDestroyShaderModule(dev, vs, nullptr);
        vkDestroyShaderModule(dev, fs, nullptr);
    }

    void destroy(const EngineContext& e, const RendererCaps&) override
    {
        if (pipe)vkDestroyPipeline(e.device, pipe, nullptr);
        if (layout)vkDestroyPipelineLayout(e.device, layout, nullptr);
    }

    void record_graphics(VkCommandBuffer, const EngineContext&, const FrameContext&) override
    {
    }

    void compose(VkCommandBuffer cmd, const EngineContext&, const FrameContext& f) override
    {
        if (!pipe || !f.swapchain_image || !f.swapchain_image_view)return;
        VkImageMemoryBarrier2 to_ca{
            .sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER_2, .srcStageMask = VK_PIPELINE_STAGE_2_NONE, .srcAccessMask = 0, .dstStageMask = VK_PIPELINE_STAGE_2_COLOR_ATTACHMENT_OUTPUT_BIT,
            .dstAccessMask = VK_ACCESS_2_COLOR_ATTACHMENT_WRITE_BIT, .oldLayout = VK_IMAGE_LAYOUT_UNDEFINED, .newLayout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL, .image = f.swapchain_image,
            .subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1}
        };
        VkDependencyInfo dep{VK_STRUCTURE_TYPE_DEPENDENCY_INFO};
        dep.imageMemoryBarrierCount = 1;
        dep.pImageMemoryBarriers = &to_ca;
        vkCmdPipelineBarrier2(cmd, &dep);
        VkClearValue cv{.color = {{0.02f, 0.02f, 0.02f, 1}}};
        VkRenderingAttachmentInfo ca{VK_STRUCTURE_TYPE_RENDERING_ATTACHMENT_INFO};
        ca.imageView = f.swapchain_image_view;
        ca.imageLayout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL;
        ca.loadOp = VK_ATTACHMENT_LOAD_OP_CLEAR;
        ca.storeOp = VK_ATTACHMENT_STORE_OP_STORE;
        ca.clearValue = cv;
        VkRenderingInfo ri{VK_STRUCTURE_TYPE_RENDERING_INFO};
        ri.renderArea = {{0, 0}, f.extent};
        ri.layerCount = 1;
        ri.colorAttachmentCount = 1;
        ri.pColorAttachments = &ca;
        vkCmdBeginRendering(cmd, &ri);
        vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, pipe);
        VkViewport vp{};
        vp.width = float(f.extent.width);
        vp.height = float(f.extent.height);
        vp.minDepth = 0;
        vp.maxDepth = 1;
        VkRect2D sc{{0, 0}, f.extent};
        vkCmdSetViewport(cmd, 0, 1, &vp);
        vkCmdSetScissor(cmd, 0, 1, &sc);
        vkCmdDraw(cmd, 3, 1, 0, 0);
        vkCmdEndRendering(cmd);
        VkImageMemoryBarrier2 to_tr{
            .sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER_2, .srcStageMask = VK_PIPELINE_STAGE_2_COLOR_ATTACHMENT_OUTPUT_BIT, .srcAccessMask = VK_ACCESS_2_COLOR_ATTACHMENT_WRITE_BIT, .dstStageMask = VK_PIPELINE_STAGE_2_TRANSFER_BIT,
            .dstAccessMask = VK_ACCESS_2_TRANSFER_WRITE_BIT, .oldLayout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL, .newLayout = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, .image = f.swapchain_image,
            .subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1}
        };
        VkDependencyInfo dep2{VK_STRUCTURE_TYPE_DEPENDENCY_INFO};
        dep2.imageMemoryBarrierCount = 1;
        dep2.pImageMemoryBarriers = &to_tr;
        vkCmdPipelineBarrier2(cmd, &dep2);
    }

    void on_imgui(const EngineContext&, const FrameContext&) override
    {
        ImGui::Begin("presentation_modes");
        ImGui::TextUnformatted("ex06_presentation_modes_showcase");
        ImGui::BulletText("Mode: RendererComposite (custom compose)");
        ImGui::TextUnformatted("Engine overlays ImGui after this pass.");
        ImGui::End();
    }

private:
    VkDevice dev{};
    VkFormat fmt{};
    VkPipelineLayout layout{};
    VkPipeline pipe{};
};

int main()
{
    try
    {
        VulkanEngine e;
        e.configure_window(1280, 720, "ex06_presentation_modes_showcase");
        e.set_renderer(std::make_unique<R>());
        e.init();
        e.run();
        e.cleanup();
    }
    catch (const std::exception& ex)
    {
        fprintf(stderr, "Fatal: %s\n", ex.what());
        return 1;
    }
    return 0;
}