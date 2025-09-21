#include "vk_engine.h"
#include <vulkan/vulkan.h>
#include <imgui.h>
#include <fstream>
#include <memory>
#include <stdexcept>
#include <string>
#include <vector>
#include "vv_camera.h"

#ifndef VK_CHECK
#define VK_CHECK(x) do{VkResult r=(x);if(r!=VK_SUCCESS)throw std::runtime_error("Vulkan error: "+std::to_string(r));}while(false)
#endif
static std::vector<char> load_spv(const std::string& p)
{
    std::ifstream f(p, std::ios::binary | std::ios::ate);
    if (!f)throw std::runtime_error("open " + p);
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
    VkShaderModule m{};
    VK_CHECK(vkCreateShaderModule(d,&ci,nullptr,&m));
    return m;
}

class TriangleRenderer : public IRenderer
{
public:
    void get_capabilities(const EngineContext&, RendererCaps& c) override
    {
        c = RendererCaps{};
        c.enable_imgui = true;
        c.presentation_mode = PresentationMode::EngineBlit;
        c.color_attachments = {AttachmentRequest{.name = "color"}};
        c.presentation_attachment = "color";
    }

    void initialize(const EngineContext& e, const RendererCaps& c, const FrameContext&) override
    {
        dev = e.device;
        fmt = c.color_attachments.front().format;
        std::string dir(SHADER_OUTPUT_DIR);
        VkShaderModule vs = make_shader(dev, load_spv(dir + "/triangle.vert.spv")), fs = make_shader(dev, load_spv(dir + "/triangle.frag.spv"));
        VkPipelineShaderStageCreateInfo st[2]{};
        for (int i = 0; i < 2; ++i)st[i].sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO;
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
        // Set camera axes anchor to world origin by default for this example
        cam_.set_axes_anchor(vv::AxesAnchor::WorldOrigin);
        cam_.set_axes_world_length(1.0f);
    }

    void destroy(const EngineContext& e, const RendererCaps&) override
    {
        if (pipe)vkDestroyPipeline(e.device, pipe, nullptr);
        if (layout)vkDestroyPipelineLayout(e.device, layout, nullptr);
    }

    void record_graphics(VkCommandBuffer cmd, const EngineContext&, const FrameContext& f) override
    {
        if (!pipe || f.color_attachments.empty())return;
        const auto& t = f.color_attachments.front();
        auto b = [&](VkImageLayout a, VkImageLayout n, VkPipelineStageFlags2 s, VkPipelineStageFlags2 d, VkAccessFlags2 sa, VkAccessFlags2 da)
        {
            VkImageMemoryBarrier2 m{VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER_2};
            m.srcStageMask = s;
            m.dstStageMask = d;
            m.srcAccessMask = sa;
            m.dstAccessMask = da;
            m.oldLayout = a;
            m.newLayout = n;
            m.image = t.image;
            m.subresourceRange = {t.aspect, 0, 1, 0, 1};
            VkDependencyInfo di{VK_STRUCTURE_TYPE_DEPENDENCY_INFO};
            di.imageMemoryBarrierCount = 1;
            di.pImageMemoryBarriers = &m;
            vkCmdPipelineBarrier2(cmd, &di);
        };
        b(VK_IMAGE_LAYOUT_GENERAL, VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL, VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT, VK_PIPELINE_STAGE_2_COLOR_ATTACHMENT_OUTPUT_BIT, VK_ACCESS_2_MEMORY_WRITE_BIT, VK_ACCESS_2_COLOR_ATTACHMENT_WRITE_BIT);
        VkClearValue cv{.color = {{clear_[0], clear_[1], clear_[2], 1.0f}}};
        VkRenderingAttachmentInfo ca{VK_STRUCTURE_TYPE_RENDERING_ATTACHMENT_INFO};
        ca.imageView = t.view;
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
        b(VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL, VK_IMAGE_LAYOUT_GENERAL, VK_PIPELINE_STAGE_2_COLOR_ATTACHMENT_OUTPUT_BIT, VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT, VK_ACCESS_2_COLOR_ATTACHMENT_WRITE_BIT,
          VK_ACCESS_2_MEMORY_READ_BIT | VK_ACCESS_2_MEMORY_WRITE_BIT);
    }

    void update(const EngineContext&, const FrameContext& f) override {
        cam_.set_axes_anchor(vv::AxesAnchor::WorldOrigin);
        cam_.update(f.dt_sec, int(f.extent.width), int(f.extent.height));
    }
    void on_event(const SDL_Event& e, const EngineContext& eng, const FrameContext* f) override {
        cam_.handle_event(e, &eng, f);
    }
    void on_imgui(const EngineContext&, const FrameContext& f) override
    {
        ImGui::Begin("Controls");
        ImGui::Text("ex01_imgui_minimal_ui");
        ImGui::SliderFloat3("clear", clear_, 0.0f, 1.0f);
        ImGui::Text("Extent %u x %u", f.extent.width, f.extent.height);
        ImGui::Text("FPS %.1f", ImGui::GetIO().Framerate);
        ImGui::Text("Press F12 to screenshot");
        ImGui::End();

        cam_.imgui_panel(nullptr);

        ImGui::Begin("Log");
        ImGui::TextUnformatted("Dock panels freely. This example checks input/DPI and UI plumbing.");
        ImGui::End();
    }

private:
    VkDevice dev{};
    VkPipelineLayout layout{};
    VkPipeline pipe{};
    VkFormat fmt{};
    float clear_[3]{0.05f, 0.07f, 0.12f};
    vv::CameraService cam_{};
};

int main()
{
    try
    {
        VulkanEngine e;
        e.configure_window(1280, 720, "ex01_imgui_minimal_ui");
        e.set_renderer(std::make_unique<TriangleRenderer>());
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