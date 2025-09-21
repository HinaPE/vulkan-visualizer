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

class R : public IRenderer
{
public:
    void get_capabilities(const EngineContext&, RendererCaps& c) override
    {
        c = RendererCaps{};
        c.presentation_mode = PresentationMode::EngineBlit;
        c.enable_imgui = true;
        AttachmentRequest a{
            .name = "comp_out", .format = VK_FORMAT_R8G8B8A8_UNORM, .usage = VK_IMAGE_USAGE_STORAGE_BIT | VK_IMAGE_USAGE_TRANSFER_SRC_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT | VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT,
            .samples = VK_SAMPLE_COUNT_1_BIT, .aspect = VK_IMAGE_ASPECT_COLOR_BIT, .initial_layout = VK_IMAGE_LAYOUT_GENERAL
        };
        c.color_attachments = {a};
        c.presentation_attachment = "comp_out";
    }

    void initialize(const EngineContext& e, const RendererCaps&, const FrameContext&) override
    {
        dev = e.device;
        alloc = e.descriptorAllocator;
        std::string d(SHADER_OUTPUT_DIR);
        auto spv = rd(d + "/comp_noise.comp.spv");
        VkShaderModuleCreateInfo sci{VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO};
        sci.codeSize = spv.size();
        sci.pCode = reinterpret_cast<const uint32_t*>(spv.data());
        VK_CHECK(vkCreateShaderModule(dev,&sci,nullptr,&cs));
        VkDescriptorSetLayoutBinding b{.binding = 0, .descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_IMAGE, .descriptorCount = 1, .stageFlags = VK_SHADER_STAGE_COMPUTE_BIT};
        VkDescriptorSetLayoutCreateInfo dl{VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO};
        dl.bindingCount = 1;
        dl.pBindings = &b;
        VK_CHECK(vkCreateDescriptorSetLayout(dev,&dl,nullptr,&dsl));
        VkPushConstantRange pcr{VK_SHADER_STAGE_COMPUTE_BIT, 0, sizeof(float) * 4};
        VkPipelineLayoutCreateInfo lci{VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO};
        lci.setLayoutCount = 1;
        lci.pSetLayouts = &dsl;
        lci.pushConstantRangeCount = 1;
        lci.pPushConstantRanges = &pcr;
        VK_CHECK(vkCreatePipelineLayout(dev,&lci,nullptr,&layout));
        VkComputePipelineCreateInfo ci{VK_STRUCTURE_TYPE_COMPUTE_PIPELINE_CREATE_INFO};
        VkPipelineShaderStageCreateInfo st{VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO};
        st.stage = VK_SHADER_STAGE_COMPUTE_BIT;
        st.module = cs;
        st.pName = "main";
        ci.stage = st;
        ci.layout = layout;
        VK_CHECK(vkCreateComputePipelines(dev,VK_NULL_HANDLE,1,&ci,nullptr,&pipe));
        ds = alloc->allocate(dev, dsl);
    }

    void destroy(const EngineContext& e, const RendererCaps&) override
    {
        if (pipe)vkDestroyPipeline(e.device, pipe, nullptr);
        if (layout)vkDestroyPipelineLayout(e.device, layout, nullptr);
        if (dsl)vkDestroyDescriptorSetLayout(e.device, dsl, nullptr);
        if (cs)vkDestroyShaderModule(e.device, cs, nullptr);
    }

    void record_compute(VkCommandBuffer cmd, const EngineContext&, const FrameContext& f) override
    {
        if (f.color_attachments.empty())return;
        const auto& t = f.color_attachments.front();
        VkDescriptorImageInfo ii{.sampler = VK_NULL_HANDLE, .imageView = t.view, .imageLayout = VK_IMAGE_LAYOUT_GENERAL};
        VkWriteDescriptorSet w{VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET};
        w.dstSet = ds;
        w.dstBinding = 0;
        w.descriptorCount = 1;
        w.descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_IMAGE;
        w.pImageInfo = &ii;
        vkUpdateDescriptorSets(dev, 1, &w, 0, nullptr);
        VkImageMemoryBarrier2 toGen{
            .sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER_2, .srcStageMask = VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT, .srcAccessMask = VK_ACCESS_2_MEMORY_WRITE_BIT, .dstStageMask = VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT,
            .dstAccessMask = VK_ACCESS_2_SHADER_WRITE_BIT, .oldLayout = VK_IMAGE_LAYOUT_GENERAL, .newLayout = VK_IMAGE_LAYOUT_GENERAL, .image = t.image, .subresourceRange = {t.aspect, 0, 1, 0, 1}
        };
        VkDependencyInfo dep{VK_STRUCTURE_TYPE_DEPENDENCY_INFO};
        dep.imageMemoryBarrierCount = 1;
        dep.pImageMemoryBarriers = &toGen;
        vkCmdPipelineBarrier2(cmd, &dep);
        vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_COMPUTE, pipe);
        vkCmdBindDescriptorSets(cmd, VK_PIPELINE_BIND_POINT_COMPUTE, layout, 0, 1, &ds, 0, nullptr);
        float pc[4]{float(f.time_sec), 0, 0, 0};
        vkCmdPushConstants(cmd, layout, VK_SHADER_STAGE_COMPUTE_BIT, 0, sizeof(pc), pc);
        uint32_t gx = (f.extent.width + 7) / 8, gy = (f.extent.height + 7) / 8;
        vkCmdDispatch(cmd, gx, gy, 1);
    }

    void record_graphics(VkCommandBuffer, const EngineContext&, const FrameContext&) override
    {
    }

    void on_imgui(const EngineContext&, const FrameContext& f) override
    {
        ImGui::Begin("compute_to_image");
        ImGui::Text("ex05_compute_to_image");
        ImGui::Text("Extent %u x %u", f.extent.width, f.extent.height);
        ImGui::End();
    }

private:
    VkDevice dev{};
    DescriptorAllocator* alloc{};
    VkShaderModule cs{};
    VkDescriptorSetLayout dsl{};
    VkPipelineLayout layout{};
    VkPipeline pipe{};
    VkDescriptorSet ds{};
};

int main()
{
    try
    {
        VulkanEngine e;
        e.configure_window(1280, 720, "ex05_compute_to_image");
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