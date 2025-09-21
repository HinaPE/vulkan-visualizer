#include "vk_engine.h"
#include "vv_camera.h"
#include <vulkan/vulkan.h>
#include <imgui.h>
#include <stdexcept>
#include <string>
#include <vector>
#include <fstream>
#include <cstring>

#ifndef VK_CHECK
#define VK_CHECK(x) do{VkResult r=(x); if(r!=VK_SUCCESS) throw std::runtime_error("Vulkan error: "+std::to_string(r)); }while(false)
#endif

static std::vector<char> load_spv(const std::string& p){
    std::ifstream f(p, std::ios::binary | std::ios::ate);
    if (!f) throw std::runtime_error("open "+p);
    size_t s = static_cast<size_t>(f.tellg()); f.seekg(0);
    std::vector<char> d(s); f.read(d.data(), static_cast<std::streamsize>(s)); return d;
}
static VkShaderModule make_shader(VkDevice d, const std::vector<char>& b){
    VkShaderModuleCreateInfo ci{VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO};
    ci.codeSize = static_cast<uint32_t>(b.size()); ci.pCode = reinterpret_cast<const uint32_t*>(b.data());
    VkShaderModule m{}; VK_CHECK(vkCreateShaderModule(d, &ci, nullptr, &m)); return m;
}

// Minimal column-major math helpers (match vv::float4x4 layout)
static vv::float4x4 make_translate(float x, float y, float z){ vv::float4x4 M = vv::make_identity(); M.m[12]=x; M.m[13]=y; M.m[14]=z; return M; }
static vv::float4x4 make_scale(float x, float y, float z){ vv::float4x4 M{}; M.m = {x,0,0,0, 0,y,0,0, 0,0,z,0, 0,0,0,1}; return M; }
static vv::float4x4 mul3(const vv::float4x4& a, const vv::float4x4& b, const vv::float4x4& c){ return vv::mul(vv::mul(a,b), c); }

class AxisVisualizer : public IRenderer {
public:
    void get_capabilities(const EngineContext&, RendererCaps& c) override {
        c = RendererCaps{};
        c.enable_imgui = true;
        c.presentation_mode = PresentationMode::EngineBlit;
        c.color_attachments = { AttachmentRequest{ .name = "color", .format = VK_FORMAT_B8G8R8A8_UNORM } };
        c.presentation_attachment = "color";
        c.depth_attachment = AttachmentRequest{ .name = "depth", .format = c.preferred_depth_format, .usage = VK_IMAGE_USAGE_DEPTH_STENCIL_ATTACHMENT_BIT, .samples = VK_SAMPLE_COUNT_1_BIT, .aspect = VK_IMAGE_ASPECT_DEPTH_BIT, .initial_layout = VK_IMAGE_LAYOUT_UNDEFINED };
        c.uses_depth = VK_TRUE;
    }

    void initialize(const EngineContext& e, const RendererCaps& c, const FrameContext&) override {
        dev_ = e.device; color_fmt_ = c.color_attachments.front().format; depth_fmt_ = c.depth_attachment ? c.depth_attachment->format : VK_FORMAT_D32_SFLOAT;
        std::string dir(SHADER_OUTPUT_DIR);
        // Axis box pipeline
        VkShaderModule v_axis = make_shader(dev_, load_spv(dir + "/axis_box.vert.spv"));
        VkShaderModule f_axis = make_shader(dev_, load_spv(dir + "/axis_color.frag.spv"));
        pipe_axis_ = create_pipe_(v_axis, f_axis);
        vkDestroyShaderModule(dev_, v_axis, nullptr); vkDestroyShaderModule(dev_, f_axis, nullptr);
        // Sphere impostor pipeline
        VkShaderModule v_sph = make_shader(dev_, load_spv(dir + "/sphere_impostor.vert.spv"));
        VkShaderModule f_sph = make_shader(dev_, load_spv(dir + "/sphere_impostor.frag.spv"));
        pipe_sphere_ = create_pipe_(v_sph, f_sph);
        vkDestroyShaderModule(dev_, v_sph, nullptr); vkDestroyShaderModule(dev_, f_sph, nullptr);

        // initial camera and scene bounds
        cam_.set_mode(vv::CameraMode::Orbit);
        vv::CameraState s = cam_.state();
        s.target = {0,0,0}; s.distance = 2.5f; s.pitch_deg = 0.0f; s.yaw_deg = -90.0f; // Front: look along -Z, X right, Y up
        s.znear = 0.01f; s.zfar = 100.0f; cam_.set_state(s);
        cam_.set_scene_bounds(vv::BoundingBox{ .min = {-0.3f,-0.3f,-0.3f}, .max = {1.2f,1.2f,1.2f}, .valid = true });
    }

    void destroy(const EngineContext& e, const RendererCaps&) override {
        if (pipe_axis_.pipeline) vkDestroyPipeline(e.device, pipe_axis_.pipeline, nullptr);
        if (pipe_axis_.layout)   vkDestroyPipelineLayout(e.device, pipe_axis_.layout, nullptr);
        if (pipe_sphere_.pipeline) vkDestroyPipeline(e.device, pipe_sphere_.pipeline, nullptr);
        if (pipe_sphere_.layout)   vkDestroyPipelineLayout(e.device, pipe_sphere_.layout, nullptr);
        pipe_axis_ = {}; pipe_sphere_ = {}; dev_ = VK_NULL_HANDLE;
    }

    void update(const EngineContext&, const FrameContext& f) override {
        cam_.update(f.dt_sec, static_cast<int>(f.extent.width), static_cast<int>(f.extent.height));
    }

    void on_event(const SDL_Event& e, const EngineContext& eng, const FrameContext* f) override { cam_.handle_event(e, &eng, f); }

    void record_graphics(VkCommandBuffer cmd, const EngineContext&, const FrameContext& f) override {
        if (!pipe_axis_.pipeline || !pipe_sphere_.pipeline || f.color_attachments.empty()) return;
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

        VkViewport vp{}; vp.x=0; vp.y=static_cast<float>(f.extent.height); vp.width=static_cast<float>(f.extent.width); vp.height=-static_cast<float>(f.extent.height); vp.minDepth=0.0f; vp.maxDepth=1.0f; VkRect2D sc{{0,0}, f.extent};
        vkCmdSetViewport(cmd, 0, 1, &vp); vkCmdSetScissor(cmd, 0, 1, &sc);

        // Build transforms
        const vv::float4x4 V = cam_.view_matrix();
        const vv::float4x4 P = cam_.proj_matrix();
        auto mvp_of = [&](const vv::float4x4& M){ return vv::mul(P, vv::mul(V, M)); };

        struct PC { float mvp[16]; float color[4]; } pc{};

        // Draw origin sphere (neutral gray)
        {
            vv::float4x4 M = make_scale(0.2f, 0.2f, 0.2f); vv::float4x4 MVP = mvp_of(M);
            std::memcpy(pc.mvp, MVP.m.data(), sizeof(pc.mvp)); pc.color[0]=0.9f; pc.color[1]=0.9f; pc.color[2]=0.9f; pc.color[3]=1.0f;
            vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, pipe_sphere_.pipeline);
            vkCmdPushConstants(cmd, pipe_sphere_.layout, VK_SHADER_STAGE_VERTEX_BIT, 0, sizeof(PC), &pc);
            vkCmdDraw(cmd, 36, 1, 0, 0);
        }

        // Axis sticks parameters
        const float L = 1.0f; const float T = 0.06f;

        // +X Red (Houdini convention)
        {
            vv::float4x4 M = mul3(make_translate(L*0.5f, 0, 0), make_scale(L, T, T), vv::make_identity());
            vv::float4x4 MVP = mvp_of(M);
            std::memcpy(pc.mvp, MVP.m.data(), sizeof(pc.mvp)); pc.color[0]=1.0f; pc.color[1]=0.0f; pc.color[2]=0.0f; pc.color[3]=1.0f;
            vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, pipe_axis_.pipeline);
            vkCmdPushConstants(cmd, pipe_axis_.layout, VK_SHADER_STAGE_VERTEX_BIT, 0, sizeof(PC), &pc);
            vkCmdDraw(cmd, 36, 1, 0, 0);
        }
        // +Y Green
        {
            vv::float4x4 M = mul3(make_translate(0, L*0.5f, 0), make_scale(T, L, T), vv::make_identity());
            vv::float4x4 MVP = mvp_of(M);
            std::memcpy(pc.mvp, MVP.m.data(), sizeof(pc.mvp)); pc.color[0]=0.0f; pc.color[1]=1.0f; pc.color[2]=0.0f; pc.color[3]=1.0f;
            vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, pipe_axis_.pipeline);
            vkCmdPushConstants(cmd, pipe_axis_.layout, VK_SHADER_STAGE_VERTEX_BIT, 0, sizeof(PC), &pc);
            vkCmdDraw(cmd, 36, 1, 0, 0);
        }
        // +Z Blue
        {
            vv::float4x4 M = mul3(make_translate(0, 0, L*0.5f), make_scale(T, T, L), vv::make_identity());
            vv::float4x4 MVP = mvp_of(M);
            std::memcpy(pc.mvp, MVP.m.data(), sizeof(pc.mvp)); pc.color[0]=0.0f; pc.color[1]=0.0f; pc.color[2]=1.0f; pc.color[3]=1.0f;
            vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, pipe_axis_.pipeline);
            vkCmdPushConstants(cmd, pipe_axis_.layout, VK_SHADER_STAGE_VERTEX_BIT, 0, sizeof(PC), &pc);
            vkCmdDraw(cmd, 36, 1, 0, 0);
        }

        vkCmdEndRendering(cmd);

        // Back to GENERAL for engine blit
        barrier_img(color.image, color.aspect, VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL, VK_IMAGE_LAYOUT_GENERAL, VK_PIPELINE_STAGE_2_COLOR_ATTACHMENT_OUTPUT_BIT, VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT, VK_ACCESS_2_COLOR_ATTACHMENT_WRITE_BIT, VK_ACCESS_2_MEMORY_READ_BIT | VK_ACCESS_2_MEMORY_WRITE_BIT);
    }

    void on_imgui(const EngineContext& eng, const FrameContext& f) override {
        auto* host = static_cast<vv_ui::TabsHost*>(eng.services);
        if (host) {
            host->add_overlay([this]{ cam_.imgui_draw_mini_axis_gizmo(); });
        }

        if (!host) return;
        host->add_tab("Coordinate Check", [this,&f]{
            ImGui::Text("Houdini-style camera: Hold Space/Alt + LMB orbit, MMB pan, RMB dolly.");
            ImGui::Text("Axes: +X=Red, +Y=Green, +Z=Blue. Origin: gray sphere.");
            if (ImGui::Button("Front (look -Z)")) { auto s = cam_.state(); s.mode=vv::CameraMode::Orbit; s.yaw_deg=-90.0f; s.pitch_deg=0.0f; cam_.set_state(s); }
            ImGui::SameLine(); if (ImGui::Button("Right (look -X)")) { auto s = cam_.state(); s.mode=vv::CameraMode::Orbit; s.yaw_deg=180.0f; s.pitch_deg=0.0f; cam_.set_state(s); }
            ImGui::SameLine(); if (ImGui::Button("Top (look -Y)")) { auto s = cam_.state(); s.mode=vv::CameraMode::Orbit; s.yaw_deg=-90.0f; s.pitch_deg=89.5f; cam_.set_state(s); }
            ImGui::Separator();
            // 2D overlay labels at axis endpoints
            const vv::float4x4 V = cam_.view_matrix();
            const vv::float4x4 P = cam_.proj_matrix();
            auto draw = ImGui::GetForegroundDrawList();
            auto draw_label = [&](const vv::float3& p, const char* txt, unsigned int col){
                float sx, sy; if (vv::project_to_screen(p, V, P, (int)f.extent.width, (int)f.extent.height, sx, sy)) {
                    draw->AddText(ImVec2(sx+4.0f, sy), col, txt);
                }
            };
            // endpoints a bit past tips for clarity
            draw_label({1.15f, 0.0f, 0.0f}, "+X", IM_COL32(255,80,80,255));
            draw_label({0.0f, 1.15f, 0.0f}, "+Y", IM_COL32(80,255,80,255));
            draw_label({0.0f, 0.0f, 1.15f}, "+Z", IM_COL32(80,120,255,255));
        });
        host->add_tab("Camera", [this]{ cam_.imgui_panel_contents(); });
    }

private:
    struct Pipeline { VkPipeline pipeline{}; VkPipelineLayout layout{}; };
    VkDevice dev_{VK_NULL_HANDLE};
    VkFormat color_fmt_{VK_FORMAT_B8G8R8A8_UNORM};
    VkFormat depth_fmt_{VK_FORMAT_D32_SFLOAT};
    Pipeline pipe_axis_{};
    Pipeline pipe_sphere_{};
    vv::CameraService cam_{};

    Pipeline create_pipe_(VkShaderModule vs, VkShaderModule fs){
        // Common state
        VkPipelineShaderStageCreateInfo st[2]{}; for(int i=0;i<2;++i) st[i].sType=VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO;
        st[0].stage = VK_SHADER_STAGE_VERTEX_BIT; st[0].module = vs; st[0].pName = "main";
        st[1] = st[0]; st[1].stage = VK_SHADER_STAGE_FRAGMENT_BIT; st[1].module = fs;
        VkPipelineVertexInputStateCreateInfo vi{VK_STRUCTURE_TYPE_PIPELINE_VERTEX_INPUT_STATE_CREATE_INFO};
        VkPipelineInputAssemblyStateCreateInfo ia{VK_STRUCTURE_TYPE_PIPELINE_INPUT_ASSEMBLY_STATE_CREATE_INFO}; ia.topology = VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST;
        VkPipelineViewportStateCreateInfo vp{VK_STRUCTURE_TYPE_PIPELINE_VIEWPORT_STATE_CREATE_INFO}; vp.viewportCount = 1; vp.scissorCount = 1;
        VkPipelineRasterizationStateCreateInfo rs{VK_STRUCTURE_TYPE_PIPELINE_RASTERIZATION_STATE_CREATE_INFO}; rs.polygonMode = VK_POLYGON_MODE_FILL; rs.cullMode=VK_CULL_MODE_BACK_BIT; rs.frontFace=VK_FRONT_FACE_COUNTER_CLOCKWISE; rs.lineWidth=1.0f;
        VkPipelineMultisampleStateCreateInfo ms{VK_STRUCTURE_TYPE_PIPELINE_MULTISAMPLE_STATE_CREATE_INFO}; ms.rasterizationSamples = VK_SAMPLE_COUNT_1_BIT;
        VkPipelineDepthStencilStateCreateInfo ds{VK_STRUCTURE_TYPE_PIPELINE_DEPTH_STENCIL_STATE_CREATE_INFO}; ds.depthTestEnable=VK_TRUE; ds.depthWriteEnable=VK_TRUE; ds.depthCompareOp=VK_COMPARE_OP_LESS;
        VkPipelineColorBlendAttachmentState ba{}; ba.colorWriteMask = 0xF; VkPipelineColorBlendStateCreateInfo cb{VK_STRUCTURE_TYPE_PIPELINE_COLOR_BLEND_STATE_CREATE_INFO}; cb.attachmentCount=1; cb.pAttachments=&ba;
        const VkDynamicState dyns[2] = { VK_DYNAMIC_STATE_VIEWPORT, VK_DYNAMIC_STATE_SCISSOR }; VkPipelineDynamicStateCreateInfo dsi{VK_STRUCTURE_TYPE_PIPELINE_DYNAMIC_STATE_CREATE_INFO}; dsi.dynamicStateCount=2; dsi.pDynamicStates=dyns;
        // Push constants: mat4 + vec4
        VkPushConstantRange pcr{VK_SHADER_STAGE_VERTEX_BIT, 0, 80};
        VkPipelineLayoutCreateInfo lci{VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO}; lci.pushConstantRangeCount=1; lci.pPushConstantRanges=&pcr;
        Pipeline out{}; VK_CHECK(vkCreatePipelineLayout(dev_, &lci, nullptr, &out.layout));
        VkPipelineRenderingCreateInfo ri{VK_STRUCTURE_TYPE_PIPELINE_RENDERING_CREATE_INFO}; ri.colorAttachmentCount=1; ri.pColorAttachmentFormats=&color_fmt_; ri.depthAttachmentFormat=depth_fmt_;
        VkGraphicsPipelineCreateInfo pci{VK_STRUCTURE_TYPE_GRAPHICS_PIPELINE_CREATE_INFO};
        pci.pNext=&ri; pci.stageCount=2; pci.pStages=st; pci.pVertexInputState=&vi; pci.pInputAssemblyState=&ia; pci.pViewportState=&vp; pci.pRasterizationState=&rs; pci.pMultisampleState=&ms; pci.pDepthStencilState=&ds; pci.pColorBlendState=&cb; pci.pDynamicState=&dsi; pci.layout=out.layout;
        VK_CHECK(vkCreateGraphicsPipelines(dev_, VK_NULL_HANDLE, 1, &pci, nullptr, &out.pipeline));
        return out;
    }
};

int main(){
    try{
        VulkanEngine e; e.configure_window(1280, 720, "ex10_coordinate");
        e.set_renderer(std::make_unique<AxisVisualizer>());
        e.init(); e.run(); e.cleanup();
    } catch(const std::exception& ex){ std::fprintf(stderr, "Fatal: %s\n", ex.what()); return 1; }
    return 0;
}
