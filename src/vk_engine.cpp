#include "vk_engine.h"

#ifdef _MSC_VER
#pragma warning(push)
#pragma warning(disable : 4100 4189 4127 4324)
#elif defined(__clang__)
#pragma clang diagnostic push
#pragma clang diagnostic ignored "-Wunused-parameter"
#pragma clang diagnostic ignored "-Wunused-variable"
#pragma clang diagnostic ignored "-Wconstant-conversion"
#pragma clang diagnostic ignored "-Wpadding"
#elif defined(__GNUC__)
#pragma GCC diagnostic push
#pragma GCC diagnostic ignored "-Wunused-parameter"
#pragma GCC diagnostic ignored "-Wunused-variable"
#pragma GCC diagnostic ignored "-Wtype-limits"
#pragma GCC diagnostic ignored "-Wmaybe-uninitialized"
#endif
#define VMA_IMPLEMENTATION
#include "vk_mem_alloc.h"
#if defined(_MSC_VER)
#pragma warning(pop)
#elif defined(__clang__)
#pragma clang diagnostic pop
#elif defined(__GNUC__)
#pragma GCC diagnostic pop
#endif

#include "VkBootstrap.h"
#include "backends/imgui_impl_sdl3.h"
#include "backends/imgui_impl_vulkan.h"
#include <SDL3/SDL_vulkan.h>
#include <array>
#include <imgui.h>
#include <ranges>
#include <chrono>
#include <stdexcept>
#include <algorithm>
#include <filesystem>
#include <sstream>
#include <iomanip>
#include <ctime>

#ifdef VV_ENABLE_SCREENSHOT
#define STB_IMAGE_WRITE_IMPLEMENTATION_DISABLED
#include <stb_image_write.h>
#endif

#ifndef VK_CHECK
#define VK_CHECK(x) do { VkResult _vk_check_res = (x); if (_vk_check_res != VK_SUCCESS) { throw std::runtime_error(std::string("Vulkan error ") + std::to_string(_vk_check_res) + " at " #x); } } while (false)
#endif
#ifndef IF_NOT_NULL_DO
#define IF_NOT_NULL_DO(ptr, stmt) do { if ((ptr) != nullptr) { stmt; } } while (false)
#endif
#ifndef IF_NOT_NULL_DO_AND_SET
#define IF_NOT_NULL_DO_AND_SET(ptr, stmt, val) do { if ((ptr) != nullptr) { stmt; (ptr) = (val); } } while (false)
#endif
#ifndef REQUIRE_TRUE
#define REQUIRE_TRUE(expr, msg) do { if (!(expr)) { throw std::runtime_error(std::string("Check failed: ") + #expr + " | " + (msg)); } } while (false)
#endif

struct VulkanEngine::UiSystem : vv_ui::TabsHost {
    using PanelFn = std::function<void()>;

    bool init(SDL_Window* window, VkInstance instance, VkPhysicalDevice physicalDevice, VkDevice device, VkQueue graphicsQueue, uint32_t graphicsQueueFamily, VkFormat swapchainFormat, uint32_t swapchainImageCount) {
        std::array<VkDescriptorPoolSize, 11> pool_sizes{{
            {VK_DESCRIPTOR_TYPE_SAMPLER, 1000},
            {VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, 1000},
            {VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE, 1000},
            {VK_DESCRIPTOR_TYPE_STORAGE_IMAGE, 1000},
            {VK_DESCRIPTOR_TYPE_UNIFORM_TEXEL_BUFFER, 1000},
            {VK_DESCRIPTOR_TYPE_STORAGE_TEXEL_BUFFER, 1000},
            {VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER, 1000},
            {VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, 1000},
            {VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER_DYNAMIC, 1000},
            {VK_DESCRIPTOR_TYPE_STORAGE_BUFFER_DYNAMIC, 1000},
            {VK_DESCRIPTOR_TYPE_INPUT_ATTACHMENT, 1000},
        }};

        VkDescriptorPoolCreateInfo pool_info{
            .sType         = VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO,
            .pNext         = nullptr,
            .flags         = VK_DESCRIPTOR_POOL_CREATE_FREE_DESCRIPTOR_SET_BIT,
            .maxSets       = 1000u * static_cast<uint32_t>(pool_sizes.size()),
            .poolSizeCount = static_cast<uint32_t>(pool_sizes.size()),
            .pPoolSizes    = pool_sizes.data(),
        };
        VK_CHECK(vkCreateDescriptorPool(device, &pool_info, nullptr, &pool_));

        IMGUI_CHECKVERSION();
        ImGui::CreateContext();

        ImGuiIO& io = ImGui::GetIO();
        io.ConfigFlags |= ImGuiConfigFlags_DockingEnable;
        io.ConfigFlags |= ImGuiConfigFlags_ViewportsEnable;

        ImGui::StyleColorsDark();
        if (io.ConfigFlags & ImGuiConfigFlags_ViewportsEnable) {
            ImGuiStyle& style                 = ImGui::GetStyle();
            style.WindowRounding              = 0.0f;
            style.Colors[ImGuiCol_WindowBg].w = 1.0f;
        }

        if (!ImGui_ImplSDL3_InitForVulkan(window)) {
            ImGui::DestroyContext();
            vkDestroyDescriptorPool(device, pool_, nullptr);
            pool_ = VK_NULL_HANDLE;
            return false;
        }

        ImGui_ImplVulkan_InitInfo init_info{};
        init_info.ApiVersion          = VK_API_VERSION_1_3;
        init_info.Instance            = instance;
        init_info.PhysicalDevice      = physicalDevice;
        init_info.Device              = device;
        init_info.QueueFamily         = graphicsQueueFamily;
        init_info.Queue               = graphicsQueue;
        init_info.DescriptorPool      = pool_;
        init_info.MinImageCount       = swapchainImageCount;
        init_info.ImageCount          = swapchainImageCount;
        init_info.MSAASamples         = VK_SAMPLE_COUNT_1_BIT;
        init_info.Allocator           = nullptr;
        init_info.CheckVkResultFn     = [](VkResult res) { VK_CHECK(res); };
        init_info.UseDynamicRendering = VK_TRUE;

        VkPipelineRenderingCreateInfo rendering_info{
            .sType                   = VK_STRUCTURE_TYPE_PIPELINE_RENDERING_CREATE_INFO,
            .pNext                   = nullptr,
            .viewMask                = 0,
            .colorAttachmentCount    = 1,
            .pColorAttachmentFormats = &swapchainFormat,
            .depthAttachmentFormat   = VK_FORMAT_UNDEFINED,
            .stencilAttachmentFormat = VK_FORMAT_UNDEFINED,
        };
        init_info.PipelineRenderingCreateInfo = rendering_info;

        if (!ImGui_ImplVulkan_Init(&init_info)) {
            ImGui_ImplSDL3_Shutdown();
            ImGui::DestroyContext();
            vkDestroyDescriptorPool(device, pool_, nullptr);
            pool_ = VK_NULL_HANDLE;
            return false;
        }

        color_format_ = swapchainFormat;
        initialized_  = true;
        return true;
    }

    void shutdown(VkDevice device) {
        if (!initialized_) return;
        ImGui_ImplVulkan_Shutdown();
        ImGui_ImplSDL3_Shutdown();
        ImGui::DestroyContext();
        if (pool_ != VK_NULL_HANDLE) {
            vkDestroyDescriptorPool(device, pool_, nullptr);
            pool_ = VK_NULL_HANDLE;
        }
        initialized_ = false;
    }

    void process_event(const SDL_Event* e) const {
        if (!initialized_ || !e) return;
        ImGui_ImplSDL3_ProcessEvent(e);
    }

    void new_frame() const {
        if (!initialized_) return;
        ImGui_ImplVulkan_NewFrame();
        ImGui_ImplSDL3_NewFrame();
        ImGui::NewFrame();
        // Do not build UI here; renderers will register tabs; we will draw them in render_overlay before ImGui::Render().
    }

    // vv_ui::TabsHost
    void add_tab(const char* name, std::function<void()> fn) override {
        if (!name || !*name || !fn) return;
        frame_tabs_.emplace_back(std::string(name), std::move(fn));
    }
    void set_main_window_title(const char* title) override {
        if (title) main_title_ = title;
    }
    void add_overlay(std::function<void()> fn) override {
        if (!fn) return;
        frame_overlays_.push_back(std::move(fn));
    }

    // Allow engine to update backend min image count on swapchain changes
    void set_min_image_count(uint32_t count) {
        if (!initialized_) return;
        ImGui_ImplVulkan_SetMinImageCount(count);
    }

    // Engine-owned persistent tabs
    void add_persistent_tab(const char* name, PanelFn fn) {
        if (!name || !*name || !fn) return;
        persistent_tabs_.emplace_back(std::string(name), std::move(fn));
    }

    void draw_tabs_ui() {
        if (!initialized_) return;
        const ImGuiWindowFlags win_flags = ImGuiWindowFlags_NoDocking;
        if (ImGui::Begin(main_title_.c_str(), nullptr, win_flags)) {
            if (ImGui::BeginTabBar("MainTabs", ImGuiTabBarFlags_Reorderable | ImGuiTabBarFlags_TabListPopupButton | ImGuiTabBarFlags_FittingPolicyScroll)) {
                // Persistent tabs first
                for (auto& [name, fn] : persistent_tabs_) {
                    if (ImGui::BeginTabItem(name.c_str())) { fn(); ImGui::EndTabItem(); }
                }
                // Per-frame tabs from renderer(s)
                for (auto& [name, fn] : frame_tabs_) {
                    if (ImGui::BeginTabItem(name.c_str())) { fn(); ImGui::EndTabItem(); }
                }
                ImGui::EndTabBar();
            }
        }
        ImGui::End();
    }

    void render_overlay(VkCommandBuffer cmd, VkImage swapchainImage, VkImageView swapchainView, VkExtent2D extent, VkImageLayout previousLayout) {
        if (!initialized_) return;

        // Build tabs UI right before Render()
        draw_tabs_ui();
        // Invoke per-frame overlays so they draw on the foreground draw list before Render()
        for (auto& fn : frame_overlays_) { fn(); }

        VkImageMemoryBarrier2 to_color{
            .sType            = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER_2,
            .pNext            = nullptr,
            .srcStageMask     = VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT,
            .srcAccessMask    = VK_ACCESS_2_MEMORY_WRITE_BIT,
            .dstStageMask     = VK_PIPELINE_STAGE_2_COLOR_ATTACHMENT_OUTPUT_BIT,
            .dstAccessMask    = VK_ACCESS_2_COLOR_ATTACHMENT_WRITE_BIT | VK_ACCESS_2_COLOR_ATTACHMENT_READ_BIT,
            .oldLayout        = previousLayout,
            .newLayout        = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL,
            .image            = swapchainImage,
            .subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0u, 1u, 0u, 1u},
        };
        VkDependencyInfo dep_color{
            .sType                    = VK_STRUCTURE_TYPE_DEPENDENCY_INFO,
            .pNext                    = nullptr,
            .dependencyFlags          = 0u,
            .memoryBarrierCount       = 0u,
            .pMemoryBarriers          = nullptr,
            .bufferMemoryBarrierCount = 0u,
            .pBufferMemoryBarriers    = nullptr,
            .imageMemoryBarrierCount  = 1u,
            .pImageMemoryBarriers     = &to_color,
        };
        vkCmdPipelineBarrier2(cmd, &dep_color);

        VkRenderingAttachmentInfo color_attachment{
            .sType              = VK_STRUCTURE_TYPE_RENDERING_ATTACHMENT_INFO,
            .pNext              = nullptr,
            .imageView          = swapchainView,
            .imageLayout        = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL,
            .resolveMode        = VK_RESOLVE_MODE_NONE,
            .resolveImageView   = VK_NULL_HANDLE,
            .resolveImageLayout = VK_IMAGE_LAYOUT_UNDEFINED,
            .loadOp             = VK_ATTACHMENT_LOAD_OP_LOAD,
            .storeOp            = VK_ATTACHMENT_STORE_OP_STORE,
            .clearValue         = {},
        };
        VkRenderingInfo rendering_info{
            .sType                = VK_STRUCTURE_TYPE_RENDERING_INFO,
            .pNext                = nullptr,
            .flags                = 0u,
            .renderArea           = {{0, 0}, extent},
            .layerCount           = 1u,
            .viewMask             = 0u,
            .colorAttachmentCount = 1u,
            .pColorAttachments    = &color_attachment,
            .pDepthAttachment     = nullptr,
            .pStencilAttachment   = nullptr,
        };
        vkCmdBeginRendering(cmd, &rendering_info);

        ImGui::Render();
        ImGui_ImplVulkan_RenderDrawData(ImGui::GetDrawData(), cmd);

        vkCmdEndRendering(cmd);

        ImGuiIO& io = ImGui::GetIO();
        if (io.ConfigFlags & ImGuiConfigFlags_ViewportsEnable) {
            ImGui::UpdatePlatformWindows();
            ImGui::RenderPlatformWindowsDefault();
        }

        VkImageMemoryBarrier2 to_present{
            .sType            = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER_2,
            .pNext            = nullptr,
            .srcStageMask     = VK_PIPELINE_STAGE_2_COLOR_ATTACHMENT_OUTPUT_BIT,
            .srcAccessMask    = VK_ACCESS_2_COLOR_ATTACHMENT_WRITE_BIT,
            .dstStageMask     = VK_PIPELINE_STAGE_2_NONE,
            .dstAccessMask    = 0u,
            .oldLayout        = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL,
            .newLayout        = VK_IMAGE_LAYOUT_PRESENT_SRC_KHR,
            .image            = swapchainImage,
            .subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0u, 1u, 0u, 1u},
        };
        VkDependencyInfo dep_present{
            .sType                    = VK_STRUCTURE_TYPE_DEPENDENCY_INFO,
            .pNext                    = nullptr,
            .dependencyFlags          = 0u,
            .memoryBarrierCount       = 0u,
            .pMemoryBarriers          = nullptr,
            .bufferMemoryBarrierCount = 0u,
            .pBufferMemoryBarriers    = nullptr,
            .imageMemoryBarrierCount  = 1u,
            .pImageMemoryBarriers     = &to_present,
        };
        vkCmdPipelineBarrier2(cmd, &dep_present);

        // Clear per-frame tabs and overlays after rendering
        frame_tabs_.clear();
        frame_overlays_.clear();
    }

    // Legacy methods (unused externally now); kept for internal calls if needed
    void add_panel(PanelFn fn) { add_persistent_tab("Panel", std::move(fn)); }

private:
    VkDescriptorPool pool_{VK_NULL_HANDLE};
    bool initialized_{false};
    VkFormat color_format_{};
    std::string main_title_{"Vulkan Visualizer"};
    std::vector<std::pair<std::string, PanelFn>> persistent_tabs_{};
    std::vector<std::pair<std::string, PanelFn>> frame_tabs_{};
    std::vector<PanelFn> frame_overlays_{};
};

void DescriptorAllocator::init_pool(VkDevice device, uint32_t maxSets, std::span<const PoolSizeRatio> ratios) {
    maxSets = std::max(1u, maxSets);
    std::vector<VkDescriptorPoolSize> sizes; sizes.reserve(ratios.size());
    for (const auto& [type, ratio] : ratios) {
        const uint32_t count = std::max(1u, static_cast<uint32_t>(ratio * static_cast<float>(maxSets)));
        sizes.push_back(VkDescriptorPoolSize{.type = type, .descriptorCount = count});
    }
    const VkDescriptorPoolCreateInfo info{.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO, .pNext = nullptr, .flags = 0u, .maxSets = maxSets, .poolSizeCount = static_cast<uint32_t>(sizes.size()), .pPoolSizes = sizes.data()};
    VK_CHECK(vkCreateDescriptorPool(device, &info, nullptr, &pool));
}
void DescriptorAllocator::clear_descriptors(VkDevice device) const { if (pool) vkResetDescriptorPool(device, pool, 0); }
void DescriptorAllocator::destroy_pool(VkDevice device) const { if (pool) vkDestroyDescriptorPool(device, pool, nullptr); }
VkDescriptorSet DescriptorAllocator::allocate(VkDevice device, VkDescriptorSetLayout layout) const { const VkDescriptorSetAllocateInfo ai{.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO, .pNext = nullptr, .descriptorPool = pool, .descriptorSetCount = 1u, .pSetLayouts = &layout}; VkDescriptorSet ds{}; VK_CHECK(vkAllocateDescriptorSets(device, &ai, &ds)); return ds; }

VulkanEngine::VulkanEngine()  = default;
VulkanEngine::~VulkanEngine() = default;

void VulkanEngine::init() {
    if (!renderer_) throw std::runtime_error("Renderer not set");

    renderer_caps_ = RendererCaps{};
    renderer_->query_required_device_caps(renderer_caps_);

    auto ensure_ext = [&](const char* name) {
        if (std::ranges::find(renderer_caps_.extra_device_extensions, name) == renderer_caps_.extra_device_extensions.end()) renderer_caps_.extra_device_extensions.push_back(name);
    };
    if (renderer_caps_.need_acceleration_structure) { ensure_ext(VK_KHR_ACCELERATION_STRUCTURE_EXTENSION_NAME); ensure_ext(VK_KHR_DEFERRED_HOST_OPERATIONS_EXTENSION_NAME); }
    if (renderer_caps_.need_ray_tracing_pipeline) { ensure_ext(VK_KHR_RAY_TRACING_PIPELINE_EXTENSION_NAME); ensure_ext(VK_KHR_DEFERRED_HOST_OPERATIONS_EXTENSION_NAME); ensure_ext(VK_KHR_ACCELERATION_STRUCTURE_EXTENSION_NAME); }
    if (renderer_caps_.need_ray_query) { ensure_ext(VK_KHR_RAY_QUERY_EXTENSION_NAME); ensure_ext(VK_KHR_ACCELERATION_STRUCTURE_EXTENSION_NAME); }
    if (renderer_caps_.need_mesh_shader) { ensure_ext(VK_EXT_MESH_SHADER_EXTENSION_NAME); }
    if (renderer_caps_.buffer_device_address) { ensure_ext(VK_KHR_BUFFER_DEVICE_ADDRESS_EXTENSION_NAME); }

    create_context(state_.width, state_.height, state_.name.c_str());

#ifdef VV_ENABLE_GPU_TIMESTAMPS
    create_timestamp_pool();
#endif

    EngineContext engPost = make_engine_context();
    renderer_->get_capabilities(engPost, renderer_caps_);
    sanitize_renderer_caps(renderer_caps_);

    create_swapchain(state_.width, state_.height);
    create_renderer_targets(swapchain_.swapchain_extent);

    create_command_buffers();

    create_renderer();

    if (renderer_caps_.enable_imgui) create_imgui();

    if (renderer_) {
        EngineContext ctx = make_engine_context();
        FrameContext frm  = make_frame_context(state_.frame_number, 0u, swapchain_.swapchain_extent);
        frm.dt_sec        = 0.0;
        frm.time_sec      = 0.0;
        frm.swapchain_image      = VK_NULL_HANDLE;
        frm.swapchain_image_view = VK_NULL_HANDLE;
        renderer_->on_swapchain_ready(ctx, frm);
    }

    state_.initialized      = true;
    state_.should_rendering = true;
}

void VulkanEngine::run() {
    if (!state_.running) state_.running = true;
    if (!state_.should_rendering) state_.should_rendering = true;
    using clock = std::chrono::steady_clock;
    auto t0     = clock::now();
    auto t_prev = t0;
    SDL_Event e{};

    EngineContext eng             = make_engine_context();
    FrameContext last_frm         = make_frame_context(state_.frame_number, 0u, swapchain_.swapchain_extent);
    last_frm.swapchain_image      = VK_NULL_HANDLE;
    last_frm.swapchain_image_view = VK_NULL_HANDLE;

    while (state_.running) {
        while (SDL_PollEvent(&e)) {
            if (renderer_) { renderer_->on_event(e, eng, state_.initialized ? &last_frm : nullptr); }
            if (ui_) { ui_->process_event(&e); }
            switch (e.type) {
            case SDL_EVENT_QUIT:
            case SDL_EVENT_WINDOW_CLOSE_REQUESTED: state_.running = false; break;
            case SDL_EVENT_WINDOW_MINIMIZED: state_.minimized = true; state_.should_rendering = false; break;
            case SDL_EVENT_WINDOW_RESTORED:
            case SDL_EVENT_WINDOW_MAXIMIZED: state_.minimized = false; state_.should_rendering = true; break;
            case SDL_EVENT_WINDOW_FOCUS_GAINED: state_.focused = true; break;
            case SDL_EVENT_WINDOW_FOCUS_LOST: state_.focused = false; break;
            case SDL_EVENT_WINDOW_RESIZED:
            case SDL_EVENT_WINDOW_PIXEL_SIZE_CHANGED: state_.resize_requested = true; break;
#ifdef VV_ENABLE_SCREENSHOT
#if defined(SDL_EVENT_KEY_DOWN)
            case SDL_EVENT_KEY_DOWN:
                if (e.key.keysym.sym == SDLK_PRINTSCREEN) { screenshot_.request = true; screenshot_.path.clear(); }
                break;
#endif
#endif
            default: break;
            }
        }

        auto t_now      = clock::now();
        state_.dt_sec   = std::chrono::duration<double>(t_now - t_prev).count();
        state_.time_sec = std::chrono::duration<double>(t_now - t0).count();
        t_prev          = t_now;

#ifdef VV_ENABLE_HOTRELOAD
        watch_accum_ += state_.dt_sec;
        if (watch_accum_ > 0.5) { poll_file_watches(eng); watch_accum_ = 0.0; }
#endif

        if (!state_.should_rendering) { SDL_WaitEventTimeout(nullptr, 100); continue; }

        if (state_.resize_requested) {
            recreate_swapchain();
            eng                           = make_engine_context();
            last_frm                      = make_frame_context(state_.frame_number, 0u, swapchain_.swapchain_extent);
            last_frm.swapchain_image      = VK_NULL_HANDLE;
            last_frm.swapchain_image_view = VK_NULL_HANDLE;
            continue;
        }

        uint32_t imageIndex = 0; VkCommandBuffer cmd = VK_NULL_HANDLE;
        begin_frame(imageIndex, cmd);
        if (cmd == VK_NULL_HANDLE) {
            if (state_.resize_requested) {
                recreate_swapchain();
                eng                           = make_engine_context();
                last_frm                      = make_frame_context(state_.frame_number, 0u, swapchain_.swapchain_extent);
                last_frm.swapchain_image      = VK_NULL_HANDLE;
                last_frm.swapchain_image_view = VK_NULL_HANDLE;
            }
            continue;
        }

        FrameContext frm = make_frame_context(state_.frame_number, imageIndex, swapchain_.swapchain_extent);
        last_frm         = frm;

        if (renderer_) { renderer_->simulate(eng, frm); }

        FrameData& frData = frames_[state_.frame_number % FRAME_OVERLAP];
        frData.asyncComputeSubmitted = false;
        const bool can_async = renderer_caps_.allow_async_compute && ctx_.compute_queue && ctx_.compute_queue != ctx_.graphics_queue && frData.asyncComputeCommandBuffer != VK_NULL_HANDLE;
        if (can_async && renderer_) {
            VK_CHECK(vkResetCommandBuffer(frData.asyncComputeCommandBuffer, 0));
            VkCommandBufferBeginInfo cbi{.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO, .pNext = nullptr, .flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT, .pInheritanceInfo = nullptr};
            VK_CHECK(vkBeginCommandBuffer(frData.asyncComputeCommandBuffer, &cbi));
            const bool recorded = renderer_->record_async_compute(frData.asyncComputeCommandBuffer, eng, frm);
            if (recorded) {
                VK_CHECK(vkEndCommandBuffer(frData.asyncComputeCommandBuffer));
                VkCommandBufferSubmitInfo cbsi{.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_SUBMIT_INFO, .pNext = nullptr, .commandBuffer = frData.asyncComputeCommandBuffer, .deviceMask = 0};
                VkSemaphoreSubmitInfo signal{.sType = VK_STRUCTURE_TYPE_SEMAPHORE_SUBMIT_INFO, .pNext = nullptr, .semaphore = frData.asyncComputeFinished, .value = 0u, .stageMask = VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT, .deviceIndex = 0};
                VkSubmitInfo2 submit{.sType = VK_STRUCTURE_TYPE_SUBMIT_INFO_2, .pNext = nullptr, .waitSemaphoreInfoCount = 0, .pWaitSemaphoreInfos = nullptr, .commandBufferInfoCount = 1, .pCommandBufferInfos = &cbsi, .signalSemaphoreInfoCount = 1, .pSignalSemaphoreInfos = &signal};
                VK_CHECK(vkQueueSubmit2(ctx_.compute_queue, 1, &submit, VK_NULL_HANDLE));
                frData.asyncComputeSubmitted = true;
            } else {
                VK_CHECK(vkEndCommandBuffer(frData.asyncComputeCommandBuffer));
            }
        }

        if (renderer_) { renderer_->update(eng, frm); }
        if (renderer_) { renderer_->record_compute(cmd, eng, frm); }
        if (renderer_) { renderer_->record_graphics(cmd, eng, frm); }

        switch (renderer_caps_.presentation_mode) {
        case PresentationMode::EngineBlit: blit_offscreen_to_swapchain(cmd, imageIndex, frm.extent); break;
        case PresentationMode::RendererComposite: if (renderer_) renderer_->compose(cmd, eng, frm); break;
        case PresentationMode::DirectToSwapchain: default: break; }

#ifdef VV_ENABLE_SCREENSHOT
        if (screenshot_.request) { queue_swapchain_screenshot(cmd, imageIndex); }
#endif

        if (ui_) {
            ui_->new_frame();
            if (renderer_) { renderer_->on_imgui(eng, frm); }
            ui_->render_overlay(cmd, frm.swapchain_image, frm.swapchain_image_view, frm.extent, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL);
        }

        end_frame(imageIndex, cmd);

#ifdef VV_ENABLE_SCREENSHOT
        if (screenshot_.request) {
            // Clear request only after a copy was actually queued (dq non-empty)
            if (!frData.dq.empty()) {
                vkQueueWaitIdle(ctx_.graphics_queue);
                for (auto& fn : frData.dq) fn();
                frData.dq.clear();
                screenshot_.request = false;
            }
        }
#endif

        state_.frame_number++;
    }
}

void VulkanEngine::cleanup() {
    if (ctx_.device) { vkDeviceWaitIdle(ctx_.device); destroy_imgui(); }
    if (renderer_) { renderer_->on_swapchain_destroy(make_engine_context()); }
    destroy_command_buffers();
    for (auto& f : std::ranges::reverse_view(mdq_)) { f(); }
    mdq_.clear();
    destroy_context();
}

void VulkanEngine::set_renderer(std::unique_ptr<IRenderer> r) { renderer_ = std::move(r); }
void VulkanEngine::configure_window(uint32_t w, uint32_t h, std::string_view title) { state_.width = w; state_.height = h; state_.name = std::string(title); }
void VulkanEngine::sanitize_renderer_caps(RendererCaps& caps) const {
    caps.swapchain_usage |= VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT;

    if (caps.presentation_mode != PresentationMode::DirectToSwapchain && caps.color_attachments.empty()) caps.color_attachments.push_back(AttachmentRequest{.name = "hdr_color"});
    if (caps.presentation_attachment.empty() && !caps.color_attachments.empty()) caps.presentation_attachment = caps.color_attachments.front().name;

    bool found = false; for (const auto& att : caps.color_attachments) { if (att.name == caps.presentation_attachment) { found = true; break; } }
    if (!found && !caps.color_attachments.empty()) caps.presentation_attachment = caps.color_attachments.front().name;

    for (auto& att : caps.color_attachments) {
        if (att.aspect == 0) att.aspect = VK_IMAGE_ASPECT_COLOR_BIT;
        if (att.samples == VK_SAMPLE_COUNT_1_BIT) att.samples = caps.color_samples;
        if (caps.presentation_mode == PresentationMode::EngineBlit && att.name == caps.presentation_attachment) att.usage |= VK_IMAGE_USAGE_TRANSFER_SRC_BIT;
    }

    if (caps.presentation_mode == PresentationMode::EngineBlit) caps.swapchain_usage |= VK_IMAGE_USAGE_TRANSFER_DST_BIT;

    if (caps.uses_depth == VK_TRUE && !caps.depth_attachment.has_value()) {
        caps.depth_attachment = AttachmentRequest{ .name = "depth", .format = caps.preferred_depth_format, .usage = VK_IMAGE_USAGE_DEPTH_STENCIL_ATTACHMENT_BIT | VK_IMAGE_USAGE_SAMPLED_BIT | VK_IMAGE_USAGE_TRANSFER_SRC_BIT, .samples = caps.color_samples, .aspect = VK_IMAGE_ASPECT_DEPTH_BIT, .initial_layout = VK_IMAGE_LAYOUT_UNDEFINED };
    }

    if (caps.depth_attachment) {
        caps.uses_depth = VK_TRUE;
        if (caps.depth_attachment->aspect == 0) caps.depth_attachment->aspect = VK_IMAGE_ASPECT_DEPTH_BIT;
        if (caps.depth_attachment->samples == VK_SAMPLE_COUNT_1_BIT) caps.depth_attachment->samples = caps.color_samples;
    } else {
        caps.uses_depth = VK_FALSE;
    }

    caps.uses_offscreen = caps.color_attachments.empty() ? VK_FALSE : VK_TRUE;
    if (caps.presentation_mode == PresentationMode::DirectToSwapchain) { caps.uses_offscreen = VK_FALSE; caps.presentation_attachment.clear(); }
}

void VulkanEngine::create_context(int window_width, int window_height, const char* app_name) {
    vkb::InstanceBuilder ib; ib.set_app_name(app_name).request_validation_layers(false).use_default_debug_messenger().require_api_version(1, 3, 0);
    for (const char* ext : renderer_caps_.extra_instance_extensions) ib.enable_extension(ext);
    vkb::Instance vkb_inst = ib.build().value();
    ctx_.instance          = vkb_inst.instance;
    ctx_.debug_messenger   = vkb_inst.debug_messenger;
    int sdl_init_rc = SDL_Init(SDL_INIT_VIDEO);
    REQUIRE_TRUE(sdl_init_rc, std::string("SDL_Init failed: ") + SDL_GetError());
    ctx_.window = SDL_CreateWindow(app_name, window_width, window_height, SDL_WINDOW_VULKAN | SDL_WINDOW_RESIZABLE);
    REQUIRE_TRUE(ctx_.window != nullptr, std::string("SDL_CreateWindow failed: ") + SDL_GetError());
    REQUIRE_TRUE(SDL_Vulkan_CreateSurface(ctx_.window, ctx_.instance, nullptr, &ctx_.surface), std::string("SDL_Vulkan_CreateSurface failed: ") + SDL_GetError());

    VkPhysicalDeviceVulkan13Features f13{.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_3_FEATURES, .pNext = nullptr, .synchronization2 = VK_TRUE, .dynamicRendering = VK_TRUE};
    VkPhysicalDeviceVulkan12Features f12{.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_2_FEATURES, .pNext = &f13, .descriptorIndexing = VK_TRUE, .bufferDeviceAddress = renderer_caps_.buffer_device_address ? VK_TRUE : VK_FALSE};

    vkb::PhysicalDeviceSelector selector(vkb_inst);
    selector.set_surface(ctx_.surface).set_minimum_version(1, 3).set_required_features_12(f12);
    for (const char* ext : renderer_caps_.extra_device_extensions) selector.add_required_extension(ext);
    vkb::PhysicalDevice phys = selector.select().value();
    ctx_.physical            = phys.physical_device;

    vkb::DeviceBuilder db(phys);
    vkb::Device vkbDev         = db.build().value();
    ctx_.device                = vkbDev.device;
    ctx_.graphics_queue        = vkbDev.get_queue(vkb::QueueType::graphics).value();
    ctx_.compute_queue         = vkbDev.get_queue(vkb::QueueType::compute).value();
    ctx_.transfer_queue        = vkbDev.get_queue(vkb::QueueType::transfer).value();
    ctx_.present_queue         = ctx_.graphics_queue;
    ctx_.graphics_queue_family = vkbDev.get_queue_index(vkb::QueueType::graphics).value();
    ctx_.compute_queue_family  = vkbDev.get_queue_index(vkb::QueueType::compute).value();
    ctx_.transfer_queue_family = vkbDev.get_queue_index(vkb::QueueType::transfer).value();
    ctx_.present_queue_family  = ctx_.graphics_queue_family;

    VmaAllocatorCreateInfo ac{}; ac.flags = VMA_ALLOCATOR_CREATE_BUFFER_DEVICE_ADDRESS_BIT; ac.physicalDevice = ctx_.physical; ac.device = ctx_.device; ac.instance = ctx_.instance; ac.vulkanApiVersion = VK_API_VERSION_1_3; VK_CHECK(vmaCreateAllocator(&ac, &ctx_.allocator));
    mdq_.emplace_back([&] { vmaDestroyAllocator(ctx_.allocator); });

    std::vector<DescriptorAllocator::PoolSizeRatio> sizes = {{VK_DESCRIPTOR_TYPE_STORAGE_IMAGE, 2.0f}, {VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, 4.0f}, {VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, 4.0f}, {VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER, 4.0f}};
    ctx_.descriptor_allocator.init_pool(ctx_.device, 128, sizes);
    mdq_.emplace_back([&] { ctx_.descriptor_allocator.destroy_pool(ctx_.device); });

    VkSemaphoreTypeCreateInfo type_ci{.sType = VK_STRUCTURE_TYPE_SEMAPHORE_TYPE_CREATE_INFO, .pNext = nullptr, .semaphoreType = VK_SEMAPHORE_TYPE_TIMELINE, .initialValue = 0};
    VkSemaphoreCreateInfo sem_ci{.sType = VK_STRUCTURE_TYPE_SEMAPHORE_CREATE_INFO, .pNext = &type_ci, .flags = 0u};
    VK_CHECK(vkCreateSemaphore(ctx_.device, &sem_ci, nullptr, &render_timeline_));
    mdq_.emplace_back([&] { vkDestroySemaphore(ctx_.device, render_timeline_, nullptr); });
    timeline_value_ = 0;
}

void VulkanEngine::destroy_context() {
    for (auto& f : std::ranges::reverse_view(mdq_)) f();
    mdq_.clear();
    IF_NOT_NULL_DO_AND_SET(ctx_.device, vkDestroyDevice(ctx_.device, nullptr), nullptr);
    IF_NOT_NULL_DO_AND_SET(ctx_.surface, vkDestroySurfaceKHR(ctx_.instance, ctx_.surface, nullptr), nullptr);
    IF_NOT_NULL_DO_AND_SET(ctx_.window, SDL_DestroyWindow(ctx_.window), nullptr);
    IF_NOT_NULL_DO_AND_SET(
        ctx_.debug_messenger,
        {
            auto f = reinterpret_cast<PFN_vkDestroyDebugUtilsMessengerEXT>(vkGetInstanceProcAddr(ctx_.instance, "vkDestroyDebugUtilsMessengerEXT"));
            if (ctx_.instance && f) f(ctx_.instance, ctx_.debug_messenger, nullptr);
        },
        nullptr);
    IF_NOT_NULL_DO_AND_SET(ctx_.instance, vkDestroyInstance(ctx_.instance, nullptr), nullptr);
    SDL_Quit();
}

EngineContext VulkanEngine::make_engine_context() const {
    EngineContext eng{};
    eng.instance              = ctx_.instance;
    eng.physical              = ctx_.physical;
    eng.device                = ctx_.device;
    eng.allocator             = ctx_.allocator;
    eng.descriptorAllocator   = const_cast<DescriptorAllocator*>(&ctx_.descriptor_allocator);
    eng.window                = ctx_.window;
    eng.graphics_queue        = ctx_.graphics_queue;
    eng.compute_queue         = ctx_.compute_queue;
    eng.transfer_queue        = ctx_.transfer_queue;
    eng.present_queue         = ctx_.present_queue;
    eng.graphics_queue_family = ctx_.graphics_queue_family;
    eng.compute_queue_family  = ctx_.compute_queue_family;
    eng.transfer_queue_family = ctx_.transfer_queue_family;
    eng.present_queue_family  = ctx_.present_queue_family;
    eng.services              = ui_ ? static_cast<vv_ui::TabsHost*>(ui_.get()) : nullptr;
    return eng;
}

FrameContext VulkanEngine::make_frame_context(uint64_t frame_index, uint32_t image_index, VkExtent2D extent) {
    FrameContext frm{};
    frm.frame_index      = frame_index;
    frm.image_index      = image_index;
    frm.extent           = extent;
    frm.swapchain_format = swapchain_.swapchain_image_format;
    frm.dt_sec           = state_.dt_sec;
    frm.time_sec         = state_.time_sec;
    if (image_index < swapchain_.swapchain_images.size()) {
        frm.swapchain_image      = swapchain_.swapchain_images[image_index];
        frm.swapchain_image_view = swapchain_.swapchain_image_views[image_index];
    }

    frame_attachment_views_.clear();
    frame_attachment_views_.reserve(swapchain_.color_attachments.size());
    for (const auto& att : swapchain_.color_attachments) {
        frame_attachment_views_.push_back(AttachmentView{
            .name           = att.name,
            .image          = att.image.image,
            .view           = att.image.imageView,
            .format         = att.image.imageFormat,
            .extent         = att.image.imageExtent,
            .samples        = att.samples,
            .usage          = att.usage,
            .aspect         = att.aspect,
            .current_layout = att.initial_layout});
    }
    frm.color_attachments = frame_attachment_views_;
    if (!frame_attachment_views_.empty()) { frm.offscreen_image = frame_attachment_views_.front().image; frm.offscreen_image_view = frame_attachment_views_.front().view; }
    else { frm.offscreen_image = VK_NULL_HANDLE; frm.offscreen_image_view = VK_NULL_HANDLE; }

    if (swapchain_.depth_attachment) {
        depth_attachment_view_ = AttachmentView{
            .name           = swapchain_.depth_attachment->name,
            .image          = swapchain_.depth_attachment->image.image,
            .view           = swapchain_.depth_attachment->image.imageView,
            .format         = swapchain_.depth_attachment->image.imageFormat,
            .extent         = swapchain_.depth_attachment->image.imageExtent,
            .samples        = swapchain_.depth_attachment->samples,
            .usage          = swapchain_.depth_attachment->usage,
            .aspect         = swapchain_.depth_attachment->aspect,
            .current_layout = swapchain_.depth_attachment->initial_layout};
        frm.depth_attachment = &depth_attachment_view_;
        frm.depth_image      = depth_attachment_view_.image;
        frm.depth_image_view = depth_attachment_view_.view;
    } else {
        frm.depth_attachment = nullptr;
        frm.depth_image      = VK_NULL_HANDLE;
        frm.depth_image_view = VK_NULL_HANDLE;
    }

    frm.presentation_mode = renderer_caps_.presentation_mode;
    return frm;
}

void VulkanEngine::blit_offscreen_to_swapchain(VkCommandBuffer cmd, uint32_t imageIndex, VkExtent2D extent) {
    if (renderer_caps_.presentation_mode != PresentationMode::EngineBlit) return;
    if (imageIndex >= swapchain_.swapchain_images.size()) return;
    if (presentation_attachment_index_ < 0 || presentation_attachment_index_ >= static_cast<int>(swapchain_.color_attachments.size())) return;

    const auto& srcAtt = swapchain_.color_attachments[static_cast<size_t>(presentation_attachment_index_)];
    VkImage src        = srcAtt.image.image; if (src == VK_NULL_HANDLE) return; VkImage dst = swapchain_.swapchain_images[imageIndex];

    VkImageMemoryBarrier2 barriers[2]{};
    barriers[0].sType            = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER_2;
    barriers[0].srcStageMask     = VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT;
    barriers[0].srcAccessMask    = VK_ACCESS_2_MEMORY_WRITE_BIT;
    barriers[0].dstStageMask     = VK_PIPELINE_STAGE_2_TRANSFER_BIT;
    barriers[0].dstAccessMask    = VK_ACCESS_2_TRANSFER_READ_BIT;
    barriers[0].oldLayout        = VK_IMAGE_LAYOUT_GENERAL;
    barriers[0].newLayout        = VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL;
    barriers[0].image            = src;
    barriers[0].subresourceRange = {srcAtt.aspect, 0u, 1u, 0u, 1u};

    barriers[1].sType            = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER_2;
    barriers[1].srcStageMask     = VK_PIPELINE_STAGE_2_NONE;
    barriers[1].srcAccessMask    = 0u;
    barriers[1].dstStageMask     = VK_PIPELINE_STAGE_2_TRANSFER_BIT;
    barriers[1].dstAccessMask    = VK_ACCESS_2_TRANSFER_WRITE_BIT;
    barriers[1].oldLayout        = VK_IMAGE_LAYOUT_UNDEFINED;
    barriers[1].newLayout        = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL;
    barriers[1].image            = dst;
    barriers[1].subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0u, 1u, 0u, 1u};

    VkDependencyInfo dep{}; dep.sType = VK_STRUCTURE_TYPE_DEPENDENCY_INFO; dep.imageMemoryBarrierCount = 2u; dep.pImageMemoryBarriers = barriers; vkCmdPipelineBarrier2(cmd, &dep);

    VkImageBlit2 blit{}; blit.sType = VK_STRUCTURE_TYPE_IMAGE_BLIT_2; blit.srcSubresource = {srcAtt.aspect, 0, 0, 1}; blit.dstSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1};
    blit.srcOffsets[0] = {0, 0, 0}; blit.srcOffsets[1] = {static_cast<int32_t>(srcAtt.image.imageExtent.width), static_cast<int32_t>(srcAtt.image.imageExtent.height), 1};
    blit.dstOffsets[0] = {0, 0, 0}; blit.dstOffsets[1] = {static_cast<int32_t>(extent.width), static_cast<int32_t>(extent.height), 1};

    VkBlitImageInfo2 bi{}; bi.sType = VK_STRUCTURE_TYPE_BLIT_IMAGE_INFO_2; bi.srcImage = src; bi.srcImageLayout = VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL; bi.dstImage = dst; bi.dstImageLayout = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL; bi.regionCount = 1u; bi.pRegions = &blit; bi.filter = VK_FILTER_LINEAR; vkCmdBlitImage2(cmd, &bi);
}

void VulkanEngine::create_swapchain(uint32_t width, uint32_t height) {
    swapchain_.swapchain_image_format = renderer_caps_.preferred_swapchain_format;
#ifdef VV_ENABLE_TONEMAP
    if (tonemap_enabled_) { swapchain_.swapchain_image_format = VK_FORMAT_B8G8R8A8_SRGB; use_srgb_swapchain_ = true; }
    else { use_srgb_swapchain_ = false; }
#endif
    VkSurfaceFormatKHR surface_fmt{swapchain_.swapchain_image_format, VK_COLOR_SPACE_SRGB_NONLINEAR_KHR};

    vkb::Swapchain sc = vkb::SwapchainBuilder(ctx_.physical, ctx_.device, ctx_.surface)
                            .set_desired_format(surface_fmt)
                            .set_desired_present_mode(renderer_caps_.present_mode)
                            .set_desired_extent(width, height)
                            .add_image_usage_flags(renderer_caps_.swapchain_usage)
                            .build()
                            .value();

    swapchain_.swapchain             = sc.swapchain;
    swapchain_.swapchain_extent      = sc.extent;
    swapchain_.swapchain_images      = sc.get_images().value();
    swapchain_.swapchain_image_views = sc.get_image_views().value();
    mdq_.emplace_back([&] { destroy_swapchain(); });
}
void VulkanEngine::destroy_swapchain() {
    for (auto v : swapchain_.swapchain_image_views) IF_NOT_NULL_DO_AND_SET(v, vkDestroyImageView(ctx_.device, v, nullptr), VK_NULL_HANDLE);
    swapchain_.swapchain_image_views.clear();
    swapchain_.swapchain_images.clear();
    IF_NOT_NULL_DO_AND_SET(swapchain_.swapchain, vkDestroySwapchainKHR(ctx_.device, swapchain_.swapchain, nullptr), VK_NULL_HANDLE);
}
void VulkanEngine::recreate_swapchain() {
    if (!ctx_.device) return;

    if (renderer_) { renderer_->on_swapchain_destroy(make_engine_context()); }

    vkDeviceWaitIdle(ctx_.device);
    destroy_swapchain();
    destroy_renderer_targets();

    int pxw = 0; int pxh = 0; SDL_GetWindowSizeInPixels(ctx_.window, &pxw, &pxh); pxw = std::max(1, pxw); pxh = std::max(1, pxh);

    create_swapchain(static_cast<uint32_t>(pxw), static_cast<uint32_t>(pxh));
    create_renderer_targets(swapchain_.swapchain_extent);

    FrameContext frm = make_frame_context(state_.frame_number, 0u, swapchain_.swapchain_extent);
    frm.swapchain_image      = VK_NULL_HANDLE;
    frm.swapchain_image_view = VK_NULL_HANDLE;

    IF_NOT_NULL_DO(renderer_, renderer_->on_swapchain_ready(make_engine_context(), frm));

    if (ui_) {
        if (imgui_format_ != swapchain_.swapchain_image_format) {
            ui_->shutdown(ctx_.device);
            ui_.reset();
            create_imgui();
        } else {
            ui_->set_min_image_count(static_cast<uint32_t>(swapchain_.swapchain_images.size()));
        }
    }

    state_.resize_requested = false;
}

void VulkanEngine::create_renderer_targets(VkExtent2D extent) {
    destroy_renderer_targets();

    const uint32_t width  = std::max(1u, extent.width);
    const uint32_t height = std::max(1u, extent.height);

    swapchain_.color_attachments.clear();
    swapchain_.color_attachments.reserve(renderer_caps_.color_attachments.size());

    auto create_image = [&](const AttachmentRequest& req, AttachmentResource& out) {
        VkImageCreateInfo imgci{.sType = VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO,
            .pNext                     = nullptr,
            .flags                     = 0u,
            .imageType                 = VK_IMAGE_TYPE_2D,
            .format                    = req.format,
            .extent                    = {width, height, 1u},
            .mipLevels                 = 1u,
            .arrayLayers               = 1u,
            .samples                   = req.samples,
            .tiling                    = VK_IMAGE_TILING_OPTIMAL,
            .usage                     = req.usage,
            .sharingMode               = VK_SHARING_MODE_EXCLUSIVE,
            .queueFamilyIndexCount     = 0u,
            .pQueueFamilyIndices       = nullptr,
            .initialLayout             = req.initial_layout};

        VmaAllocationCreateInfo ainfo{.flags = 0u,
            .usage                       = VMA_MEMORY_USAGE_GPU_ONLY,
            .requiredFlags               = static_cast<VkMemoryPropertyFlags>(VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT),
            .preferredFlags              = 0u,
            .memoryTypeBits              = 0u,
            .pool                        = VK_NULL_HANDLE,
            .pUserData                   = nullptr,
            .priority                    = 1.0f};

        VK_CHECK(vmaCreateImage(ctx_.allocator, &imgci, &ainfo, &out.image.image, &out.image.allocation, nullptr));

        VkImageViewCreateInfo viewci{.sType = VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO,
            .pNext                          = nullptr,
            .flags                          = 0u,
            .image                          = out.image.image,
            .viewType                       = VK_IMAGE_VIEW_TYPE_2D,
            .format                         = req.format,
            .components                     = {VK_COMPONENT_SWIZZLE_IDENTITY, VK_COMPONENT_SWIZZLE_IDENTITY, VK_COMPONENT_SWIZZLE_IDENTITY, VK_COMPONENT_SWIZZLE_IDENTITY},
            .subresourceRange               = {req.aspect, 0u, 1u, 0u, 1u}};
        VK_CHECK(vkCreateImageView(ctx_.device, &viewci, nullptr, &out.image.imageView));

        out.image.imageFormat = req.format;
        out.image.imageExtent = {width, height, 1u};
        out.usage             = req.usage;
        out.aspect            = req.aspect;
        out.samples           = req.samples;
        out.initial_layout    = req.initial_layout;
    };

    for (const auto& req : renderer_caps_.color_attachments) {
        AttachmentResource res{}; res.name = req.name; create_image(req, res); swapchain_.color_attachments.push_back(std::move(res));
    }

    if (renderer_caps_.depth_attachment) {
        AttachmentResource depth{}; depth.name = renderer_caps_.depth_attachment->name.empty() ? "depth" : renderer_caps_.depth_attachment->name; create_image(*renderer_caps_.depth_attachment, depth); swapchain_.depth_attachment = std::move(depth);
    } else { swapchain_.depth_attachment.reset(); }

    presentation_attachment_index_ = -1;
    for (size_t i = 0; i < swapchain_.color_attachments.size(); ++i) { if (swapchain_.color_attachments[i].name == renderer_caps_.presentation_attachment) { presentation_attachment_index_ = static_cast<int>(i); break; } }
    if (presentation_attachment_index_ == -1 && !swapchain_.color_attachments.empty()) presentation_attachment_index_ = 0;

    mdq_.emplace_back([&] { destroy_renderer_targets(); });
}

void VulkanEngine::destroy_renderer_targets() {
    for (auto& att : swapchain_.color_attachments) {
        IF_NOT_NULL_DO_AND_SET(att.image.imageView, vkDestroyImageView(ctx_.device, att.image.imageView, nullptr), VK_NULL_HANDLE);
        IF_NOT_NULL_DO_AND_SET(att.image.image, vmaDestroyImage(ctx_.allocator, att.image.image, att.image.allocation), VK_NULL_HANDLE);
        att.image = {};
    }
    swapchain_.color_attachments.clear();

    if (swapchain_.depth_attachment) {
        IF_NOT_NULL_DO_AND_SET(swapchain_.depth_attachment->image.imageView, vkDestroyImageView(ctx_.device, swapchain_.depth_attachment->image.imageView, nullptr), VK_NULL_HANDLE);
        IF_NOT_NULL_DO_AND_SET(swapchain_.depth_attachment->image.image, vmaDestroyImage(ctx_.allocator, swapchain_.depth_attachment->image.image, swapchain_.depth_attachment->image.allocation), VK_NULL_HANDLE);
        swapchain_.depth_attachment.reset();
    }

    frame_attachment_views_.clear();
    depth_attachment_view_ = {};
    presentation_attachment_index_ = -1;
}

void VulkanEngine::create_command_buffers() {
    VkCommandPoolCreateInfo pci{.sType = VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO, .pNext = nullptr, .flags = VK_COMMAND_POOL_CREATE_RESET_COMMAND_BUFFER_BIT, .queueFamilyIndex = ctx_.graphics_queue_family};
    for (auto& fr : frames_) {
        VK_CHECK(vkCreateCommandPool(ctx_.device, &pci, nullptr, &fr.commandPool));
        VkCommandBufferAllocateInfo ai{.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO, .pNext = nullptr, .commandPool = fr.commandPool, .level = VK_COMMAND_BUFFER_LEVEL_PRIMARY, .commandBufferCount = 1u};
        VK_CHECK(vkAllocateCommandBuffers(ctx_.device, &ai, &fr.mainCommandBuffer));
        VkSemaphoreCreateInfo sci{.sType = VK_STRUCTURE_TYPE_SEMAPHORE_CREATE_INFO, .pNext = nullptr, .flags = 0u};
        VK_CHECK(vkCreateSemaphore(ctx_.device, &sci, nullptr, &fr.imageAcquired));
        VK_CHECK(vkCreateSemaphore(ctx_.device, &sci, nullptr, &fr.renderComplete));

        if (renderer_caps_.allow_async_compute && ctx_.compute_queue && ctx_.compute_queue != ctx_.graphics_queue) {
            VkCommandPoolCreateInfo cpool{.sType = VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO, .pNext = nullptr, .flags = VK_COMMAND_POOL_CREATE_RESET_COMMAND_BUFFER_BIT, .queueFamilyIndex = ctx_.compute_queue_family};
            VK_CHECK(vkCreateCommandPool(ctx_.device, &cpool, nullptr, &fr.computeCommandPool));
            VkCommandBufferAllocateInfo cai{.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO, .pNext = nullptr, .commandPool = fr.computeCommandPool, .level = VK_COMMAND_BUFFER_LEVEL_PRIMARY, .commandBufferCount = 1u};
            VK_CHECK(vkAllocateCommandBuffers(ctx_.device, &cai, &fr.asyncComputeCommandBuffer));
            VkSemaphoreCreateInfo sci2{.sType = VK_STRUCTURE_TYPE_SEMAPHORE_CREATE_INFO, .pNext = nullptr, .flags = 0u};
            VK_CHECK(vkCreateSemaphore(ctx_.device, &sci2, nullptr, &fr.asyncComputeFinished));
        }
    }
    mdq_.emplace_back([&] { destroy_command_buffers(); });
}
void VulkanEngine::destroy_command_buffers() {
    for (auto& fr : frames_) {
        for (auto& f : std::ranges::reverse_view(fr.dq)) f();
        fr.dq.clear();
        IF_NOT_NULL_DO_AND_SET(fr.imageAcquired, vkDestroySemaphore(ctx_.device, fr.imageAcquired, nullptr), VK_NULL_HANDLE);
        IF_NOT_NULL_DO_AND_SET(fr.renderComplete, vkDestroySemaphore(ctx_.device, fr.renderComplete, nullptr), VK_NULL_HANDLE);
        IF_NOT_NULL_DO_AND_SET(fr.commandPool, vkDestroyCommandPool(ctx_.device, fr.commandPool, nullptr), VK_NULL_HANDLE);
        IF_NOT_NULL_DO_AND_SET(fr.asyncComputeFinished, vkDestroySemaphore(ctx_.device, fr.asyncComputeFinished, nullptr), VK_NULL_HANDLE);
        IF_NOT_NULL_DO_AND_SET(fr.computeCommandPool, vkDestroyCommandPool(ctx_.device, fr.computeCommandPool, nullptr), VK_NULL_HANDLE);
        fr.mainCommandBuffer         = VK_NULL_HANDLE;
        fr.asyncComputeCommandBuffer = VK_NULL_HANDLE;
        fr.submitted_timeline_value  = 0;
    }
}

void VulkanEngine::begin_frame(uint32_t& imageIndex, VkCommandBuffer& cmd) {
    FrameData& fr = frames_[state_.frame_number % FRAME_OVERLAP];
    if (fr.submitted_timeline_value > 0) {
        VkSemaphore sem = render_timeline_;
        uint64_t val    = fr.submitted_timeline_value;
        VkSemaphoreWaitInfo wi{.sType = VK_STRUCTURE_TYPE_SEMAPHORE_WAIT_INFO, .pNext = nullptr, .flags = 0u, .semaphoreCount = 1u, .pSemaphores = &sem, .pValues = &val};
        VK_CHECK(vkWaitSemaphores(ctx_.device, &wi, UINT64_MAX));
#ifdef VV_ENABLE_GPU_TIMESTAMPS
        if (ts_query_pool_) {
            const uint32_t base = static_cast<uint32_t>((state_.frame_number % FRAME_OVERLAP) * 2);
            uint64_t ticks[2]{};
            VkResult qres = vkGetQueryPoolResults(ctx_.device, ts_query_pool_, base, 2, sizeof(ticks), ticks, sizeof(uint64_t), VK_QUERY_RESULT_64_BIT);
            if (qres == VK_SUCCESS && ticks[1] > ticks[0]) last_gpu_ms_ = (static_cast<double>(ticks[1] - ticks[0]) * ts_period_ns_) / 1.0e6;
        }
#endif
    }
    const VkResult acq = vkAcquireNextImageKHR(ctx_.device, swapchain_.swapchain, UINT64_MAX, fr.imageAcquired, VK_NULL_HANDLE, &imageIndex);
    if (acq == VK_ERROR_OUT_OF_DATE_KHR || acq == VK_SUBOPTIMAL_KHR) { state_.resize_requested = true; cmd = VK_NULL_HANDLE; return; }
    VK_CHECK(acq);
    VK_CHECK(vkResetCommandBuffer(fr.mainCommandBuffer, 0));
    cmd = fr.mainCommandBuffer;
    VkCommandBufferBeginInfo bi{.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO, .pNext = nullptr, .flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT, .pInheritanceInfo = nullptr};
    VK_CHECK(vkBeginCommandBuffer(cmd, &bi));
#ifdef VV_ENABLE_GPU_TIMESTAMPS
    if (ts_query_pool_) {
        const uint32_t base = static_cast<uint32_t>((state_.frame_number % FRAME_OVERLAP) * 2);
        vkCmdResetQueryPool(cmd, ts_query_pool_, base, 2);
        vkCmdWriteTimestamp2(cmd, VK_PIPELINE_STAGE_2_TOP_OF_PIPE_BIT, ts_query_pool_, base);
    }
#endif
}

void VulkanEngine::end_frame(uint32_t imageIndex, VkCommandBuffer cmd) {
#ifdef VV_ENABLE_GPU_TIMESTAMPS
    if (ts_query_pool_) {
        const uint32_t base = static_cast<uint32_t>((state_.frame_number % FRAME_OVERLAP) * 2);
        vkCmdWriteTimestamp2(cmd, VK_PIPELINE_STAGE_2_BOTTOM_OF_PIPE_BIT, ts_query_pool_, base + 1);
    }
#endif
    VK_CHECK(vkEndCommandBuffer(cmd));
    FrameData& fr = frames_[state_.frame_number % FRAME_OVERLAP];

    VkCommandBufferSubmitInfo cbsi{.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_SUBMIT_INFO, .pNext = nullptr, .commandBuffer = cmd, .deviceMask = 0u};

    VkSemaphoreSubmitInfo waitInfos[2]{}; uint32_t waitCount = 0;
    waitInfos[waitCount] = VkSemaphoreSubmitInfo{.sType = VK_STRUCTURE_TYPE_SEMAPHORE_SUBMIT_INFO, .pNext = nullptr, .semaphore = fr.imageAcquired, .value = 0u, .stageMask = VK_PIPELINE_STAGE_2_COLOR_ATTACHMENT_OUTPUT_BIT, .deviceIndex = 0u};
    waitCount++;
    if (fr.asyncComputeSubmitted) { waitInfos[waitCount] = VkSemaphoreSubmitInfo{.sType = VK_STRUCTURE_TYPE_SEMAPHORE_SUBMIT_INFO, .pNext = nullptr, .semaphore = fr.asyncComputeFinished, .value = 0u, .stageMask = VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT, .deviceIndex = 0u}; waitCount++; }

    timeline_value_++;
    uint64_t timeline_to_signal = timeline_value_;
    VkSemaphoreSubmitInfo signalInfos[2]{
        VkSemaphoreSubmitInfo{.sType = VK_STRUCTURE_TYPE_SEMAPHORE_SUBMIT_INFO, .pNext = nullptr, .semaphore = fr.renderComplete, .value = 0u, .stageMask = VK_PIPELINE_STAGE_2_ALL_GRAPHICS_BIT, .deviceIndex = 0u},
        VkSemaphoreSubmitInfo{.sType = VK_STRUCTURE_TYPE_SEMAPHORE_SUBMIT_INFO, .pNext = nullptr, .semaphore = render_timeline_, .value = timeline_to_signal, .stageMask = VK_PIPELINE_STAGE_2_ALL_GRAPHICS_BIT, .deviceIndex = 0u}};

    VkSubmitInfo2 si{.sType = VK_STRUCTURE_TYPE_SUBMIT_INFO_2, .pNext = nullptr, .waitSemaphoreInfoCount = waitCount, .pWaitSemaphoreInfos = waitInfos, .commandBufferInfoCount = 1, .pCommandBufferInfos = &cbsi, .signalSemaphoreInfoCount = 2u, .pSignalSemaphoreInfos = signalInfos};
    VK_CHECK(vkQueueSubmit2(ctx_.graphics_queue, 1, &si, VK_NULL_HANDLE));
    fr.submitted_timeline_value = timeline_to_signal;

    VkPresentInfoKHR pi{.sType = VK_STRUCTURE_TYPE_PRESENT_INFO_KHR, .pNext = nullptr, .waitSemaphoreCount = 1u, .pWaitSemaphores = &fr.renderComplete, .swapchainCount = 1u, .pSwapchains = &swapchain_.swapchain, .pImageIndices = &imageIndex, .pResults = nullptr};
    VkResult pres = vkQueuePresentKHR(ctx_.graphics_queue, &pi);
    if (pres == VK_ERROR_OUT_OF_DATE_KHR || pres == VK_SUBOPTIMAL_KHR) { state_.resize_requested = true; return; }
    VK_CHECK(pres);
}

void VulkanEngine::create_renderer() {
    if (!renderer_) throw std::runtime_error("Renderer not set");
    EngineContext eng = make_engine_context();
    FrameContext initial = make_frame_context(state_.frame_number, 0u, swapchain_.swapchain_extent);
    initial.swapchain_image      = VK_NULL_HANDLE;
    initial.swapchain_image_view = VK_NULL_HANDLE;
    renderer_->initialize(eng, renderer_caps_, initial);
    mdq_.emplace_back([&] { destroy_renderer(); });
}
void VulkanEngine::destroy_renderer() {
    if (!renderer_) return;
    EngineContext eng = make_engine_context();
    renderer_->destroy(eng, renderer_caps_);
    renderer_.reset();
}

void VulkanEngine::create_imgui() {
    ui_ = std::make_unique<UiSystem>();
    try {
        if (!ui_->init(ctx_.window, ctx_.instance, ctx_.physical, ctx_.device, ctx_.graphics_queue, ctx_.graphics_queue_family, swapchain_.swapchain_image_format, static_cast<uint32_t>(swapchain_.swapchain_images.size()))) {
            throw std::runtime_error("ImGui initialization failed");
        }
    } catch (const std::exception& ex) {
        if (ui_) { ui_->shutdown(ctx_.device); ui_.reset(); }
        throw std::runtime_error(std::string("ImGui initialization failed: ") + ex.what());
    }

    imgui_format_ = swapchain_.swapchain_image_format;
    ui_->set_main_window_title(state_.name.c_str());

    ImGuiStyle& style      = ImGui::GetStyle();
    style.WindowRounding   = 0.0f;
    style.WindowBorderSize = 0.0f;
    style.FrameRounding    = 4.0f;
    style.GrabRounding     = 4.0f;

    VkPhysicalDeviceProperties props{}; vkGetPhysicalDeviceProperties(ctx_.physical, &props);
    VkPhysicalDeviceMemoryProperties memProps{}; vkGetPhysicalDeviceMemoryProperties(ctx_.physical, &memProps);

    // Engine persistent tabs: Stats
    ui_->add_persistent_tab("Stats", [this, props, memProps] {
        const ImGuiIO& io = ImGui::GetIO();
        const float fps   = io.Framerate; const float ms = fps > 0.f ? 1000.f / fps : 0.f;
        ImGui::Text("FPS: %.1f (%.2f ms)", fps, ms);
        ImGui::SeparatorText("Frame");
        ImGui::Text("Frame#:  %llu", static_cast<unsigned long long>(state_.frame_number));
        ImGui::Text("Time:    %.3f s", state_.time_sec);
        ImGui::Text("dt:      %.3f ms", state_.dt_sec * 1000.0);
#ifdef VV_ENABLE_GPU_TIMESTAMPS
        ImGui::Text("GPU:     %.3f ms (engine)", last_gpu_ms_);
#endif
        ImGui::SeparatorText("Swapchain");
        ImGui::Text("Extent:  %u x %u", swapchain_.swapchain_extent.width, swapchain_.swapchain_extent.height);
        ImGui::Text("Images:  %zu", swapchain_.swapchain_images.size());
        ImGui::Text("Format:  0x%08X", static_cast<uint32_t>(swapchain_.swapchain_image_format));
        ImGui::SeparatorText("Attachments");
        if (swapchain_.color_attachments.empty()) { ImGui::TextUnformatted("Color: (none)"); }
        else { for (const auto& att : swapchain_.color_attachments) ImGui::Text("%s: 0x%08X", att.name.c_str(), static_cast<uint32_t>(att.image.imageFormat)); }
        if (swapchain_.depth_attachment) ImGui::Text("Depth %s: 0x%08X", swapchain_.depth_attachment->name.c_str(), static_cast<uint32_t>(swapchain_.depth_attachment->image.imageFormat));
        ImGui::SeparatorText("Window");
        int lw=0, lh=0, pw=0, ph=0; SDL_GetWindowSize(ctx_.window, &lw, &lh); SDL_GetWindowSizeInPixels(ctx_.window, &pw, &ph);
        ImGui::Text("Logical: %d x %d", lw, lh);
        ImGui::Text("Pixels : %d x %d", pw, ph);
        ImGui::Text("Focused: %s", state_.focused ? "Yes" : "No");
        ImGui::Text("Minimized: %s", state_.minimized ? "Yes" : "No");
        ImGui::Text("Scale:   %.2f,%.2f", io.DisplayFramebufferScale.x, io.DisplayFramebufferScale.y);
        ImGui::SeparatorText("Device");
        ImGui::TextUnformatted(props.deviceName);
        ImGui::Text("VendorID: 0x%04X  DeviceID: 0x%04X", props.vendorID, props.deviceID);
        ImGui::Text("API:  %u.%u.%u", VK_API_VERSION_MAJOR(props.apiVersion), VK_API_VERSION_MINOR(props.apiVersion), VK_API_VERSION_PATCH(props.apiVersion));
        ImGui::Text("Drv:  0x%08X", props.driverVersion);
        ImGui::SeparatorText("Queues");
        ImGui::Text("GFX qfam: %u", ctx_.graphics_queue_family);
        ImGui::Text("CMP qfam: %u", ctx_.compute_queue_family);
        ImGui::Text("XFR qfam: %u", ctx_.transfer_queue_family);
        ImGui::Text("PRS qfam: %u", ctx_.present_queue_family);
        ImGui::SeparatorText("Renderer");
        if (renderer_) {
            const RendererStats st = renderer_->get_stats();
            ImGui::Text("Draws:   %llu", static_cast<unsigned long long>(st.draw_calls));
            ImGui::Text("Disp:    %llu", static_cast<unsigned long long>(st.dispatches));
            ImGui::Text("Tris:    %llu", static_cast<unsigned long long>(st.triangles));
            ImGui::Text("CPU:     %.3f ms", st.cpu_ms);
            ImGui::Text("GPU:     %.3f ms", st.gpu_ms);
            ImGui::SeparatorText("Caps");
            ImGui::Text("FramesInFlight: %u", renderer_caps_.frames_in_flight);
            ImGui::Text("DynamicRendering: %s", renderer_caps_.dynamic_rendering ? "Yes" : "No");
            ImGui::Text("TimelineSemaphore: %s", renderer_caps_.timeline_semaphore ? "Yes" : "No");
            ImGui::Text("DescriptorIndexing: %s", renderer_caps_.descriptor_indexing ? "Yes" : "No");
            ImGui::Text("BufferDeviceAddress: %s", renderer_caps_.buffer_device_address ? "Yes" : "No");
            ImGui::Text("UsesDepth: %s", renderer_caps_.uses_depth ? "Yes" : "No");
            ImGui::Text("UsesOffscreen: %s", renderer_caps_.uses_offscreen ? "Yes" : "No");
        } else ImGui::TextUnformatted("(no renderer)");
        ImGui::SeparatorText("Sync");
        ImGui::Text("Timeline value: %llu", static_cast<unsigned long long>(timeline_value_));
        ImGui::SeparatorText("Memory (VMA)");
        std::vector<VmaBudget> budgets(memProps.memoryHeapCount); vmaGetHeapBudgets(ctx_.allocator, budgets.data());
        uint64_t totalBudget=0, totalUsage=0; for (uint32_t i=0;i<memProps.memoryHeapCount;++i){ totalBudget+=budgets[i].budget; totalUsage+=budgets[i].usage; }
        auto fmtMB = [](uint64_t bytes){ return static_cast<double>(bytes)/(1024.0*1024.0); };
        ImGui::Text("Usage:  %.1f MB / %.1f MB", fmtMB(totalUsage), fmtMB(totalBudget));
    });

#ifdef VV_ENABLE_SCREENSHOT
    // Controls tab (screenshot & tonemap toggle)
    ui_->add_persistent_tab("Controls", [this] {
#ifdef VV_ENABLE_TONEMAP
        ImGui::Checkbox("Use sRGB Swapchain (Gamma)", &tonemap_enabled_);
        ImGui::SameLine(); if (ImGui::Button("Apply")) { state_.resize_requested = true; }
#endif
        if (ImGui::Button("Screenshot (PrtSc)")) { screenshot_.request = true; screenshot_.path.clear(); }
    });
#endif

#ifdef VV_ENABLE_LOGGING
    ui_->add_persistent_tab("Log", [this] {
        for (const auto& line : log_lines_) ImGui::TextUnformatted(line.c_str());
    });
    log_line("Engine initialized");
#endif
}

void VulkanEngine::destroy_imgui() { if (ui_) { ui_->shutdown(ctx_.device); ui_.reset(); imgui_format_ = VK_FORMAT_UNDEFINED; } }

#ifdef VV_ENABLE_GPU_TIMESTAMPS
void VulkanEngine::create_timestamp_pool() {
    VkPhysicalDeviceProperties props{}; vkGetPhysicalDeviceProperties(ctx_.physical, &props);
    ts_period_ns_ = static_cast<double>(props.limits.timestampPeriod);
    VkQueryPoolCreateInfo qci{.sType = VK_STRUCTURE_TYPE_QUERY_POOL_CREATE_INFO, .pNext = nullptr, .flags = 0u, .queryType = VK_QUERY_TYPE_TIMESTAMP, .queryCount = FRAME_OVERLAP * 2, .pipelineStatistics = 0u};
    VK_CHECK(vkCreateQueryPool(ctx_.device, &qci, nullptr, &ts_query_pool_));
    mdq_.emplace_back([&] { destroy_timestamp_pool(); });
}
void VulkanEngine::destroy_timestamp_pool() { IF_NOT_NULL_DO_AND_SET(ts_query_pool_, vkDestroyQueryPool(ctx_.device, ts_query_pool_, nullptr), VK_NULL_HANDLE); }
#endif

#ifdef VV_ENABLE_SCREENSHOT
static std::string default_screenshot_name() {
    auto now = std::chrono::system_clock::now();
    std::time_t t = std::chrono::system_clock::to_time_t(now);
    std::tm tm{};
#ifdef _WIN32
    localtime_s(&tm, &t);
#else
    localtime_r(&t, &tm);
#endif
    std::ostringstream oss; oss << "screenshot_" << std::put_time(&tm, "%Y%m%d_%H%M%S") << ".png";
    return oss.str();
}

void VulkanEngine::queue_swapchain_screenshot(VkCommandBuffer cmd, uint32_t imageIndex) {
    if (imageIndex >= swapchain_.swapchain_images.size()) return;
    const uint32_t w = swapchain_.swapchain_extent.width; const uint32_t h = swapchain_.swapchain_extent.height; VkImage img = swapchain_.swapchain_images[imageIndex];

    VkDeviceSize sz = static_cast<VkDeviceSize>(w) * static_cast<VkDeviceSize>(h) * 4u;
    VkBuffer buffer = VK_NULL_HANDLE; VmaAllocation alloc{}; VmaAllocationInfo aout{};
    VkBufferCreateInfo bci{.sType = VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO, .pNext = nullptr, .flags = 0u, .size = sz, .usage = VK_BUFFER_USAGE_TRANSFER_DST_BIT, .sharingMode = VK_SHARING_MODE_EXCLUSIVE, .queueFamilyIndexCount = 0u, .pQueueFamilyIndices = nullptr};
    VmaAllocationCreateInfo ainfo{.flags = VMA_ALLOCATION_CREATE_HOST_ACCESS_RANDOM_BIT | VMA_ALLOCATION_CREATE_MAPPED_BIT, .usage = VMA_MEMORY_USAGE_AUTO, .requiredFlags = VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT};
    VK_CHECK(vmaCreateBuffer(ctx_.allocator, &bci, &ainfo, &buffer, &alloc, &aout));

    VkImageMemoryBarrier2 to_src{.sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER_2, .pNext = nullptr, .srcStageMask = VK_PIPELINE_STAGE_2_TRANSFER_BIT, .srcAccessMask = VK_ACCESS_2_TRANSFER_WRITE_BIT, .dstStageMask = VK_PIPELINE_STAGE_2_TRANSFER_BIT, .dstAccessMask = VK_ACCESS_2_TRANSFER_READ_BIT, .oldLayout = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, .newLayout = VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, .image = img, .subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1}};
    VkDependencyInfo dep_to_src{.sType = VK_STRUCTURE_TYPE_DEPENDENCY_INFO, .pNext = nullptr, .dependencyFlags = 0u, .memoryBarrierCount = 0u, .pMemoryBarriers = nullptr, .bufferMemoryBarrierCount = 0u, .pBufferMemoryBarriers = nullptr, .imageMemoryBarrierCount = 1u, .pImageMemoryBarriers = &to_src};
    vkCmdPipelineBarrier2(cmd, &dep_to_src);

    VkBufferImageCopy region{.bufferOffset = 0, .bufferRowLength = 0, .bufferImageHeight = 0, .imageSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1}, .imageOffset = {0, 0, 0}, .imageExtent = {w, h, 1}};
    vkCmdCopyImageToBuffer(cmd, img, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, buffer, 1, &region);

    VkImageMemoryBarrier2 back_dst{.sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER_2, .pNext = nullptr, .srcStageMask = VK_PIPELINE_STAGE_2_TRANSFER_BIT, .srcAccessMask = VK_ACCESS_2_TRANSFER_READ_BIT, .dstStageMask = VK_PIPELINE_STAGE_2_TRANSFER_BIT, .dstAccessMask = VK_ACCESS_2_TRANSFER_WRITE_BIT, .oldLayout = VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, .newLayout = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, .image = img, .subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1}};
    VkDependencyInfo dep_back{.sType = VK_STRUCTURE_TYPE_DEPENDENCY_INFO, .pNext = nullptr, .dependencyFlags = 0u, .memoryBarrierCount = 0u, .pMemoryBarriers = nullptr, .bufferMemoryBarrierCount = 0u, .pBufferMemoryBarriers = nullptr, .imageMemoryBarrierCount = 1u, .pImageMemoryBarriers = &back_dst};
    vkCmdPipelineBarrier2(cmd, &dep_back);

    FrameData& fr = frames_[state_.frame_number % FRAME_OVERLAP];
    const std::string outPath = screenshot_.path.empty() ? default_screenshot_name() : screenshot_.path;
#ifdef VV_ENABLE_LOGGING
    log_line(std::string("Queued screenshot: ") + outPath);
#endif
    fr.dq.emplace_back([this, buffer, alloc, outPath, w, h] {
        void* p = nullptr; vmaMapMemory(ctx_.allocator, alloc, &p);
        const uint8_t* bgra = static_cast<const uint8_t*>(p);
        std::vector<uint8_t> rgba; rgba.resize(static_cast<size_t>(w) * static_cast<size_t>(h) * 4ull);
        for (size_t i = 0, n = static_cast<size_t>(w) * static_cast<size_t>(h); i < n; ++i) {
            rgba[i * 4 + 0] = bgra[i * 4 + 2];
            rgba[i * 4 + 1] = bgra[i * 4 + 1];
            rgba[i * 4 + 2] = bgra[i * 4 + 0];
            rgba[i * 4 + 3] = bgra[i * 4 + 3];
        }
        stbi_write_png(outPath.c_str(), static_cast<int>(w), static_cast<int>(h), 4, rgba.data(), static_cast<int>(w * 4));
        vmaUnmapMemory(ctx_.allocator, alloc);
        vmaDestroyBuffer(ctx_.allocator, buffer, alloc);
#ifdef VV_ENABLE_LOGGING
        log_line(std::string("Saved screenshot: ") + outPath);
#endif
    });
}
#endif

#ifdef VV_ENABLE_HOTRELOAD
void VulkanEngine::add_hot_reload_watch_path(const std::string& path) {
    if (path.empty()) return;
    std::error_code ec{}; std::filesystem::path p = std::filesystem::absolute(path, ec); if (ec) p = std::filesystem::path(path);
    uint64_t latest = 0;
    for (auto it = std::filesystem::recursive_directory_iterator(p, std::filesystem::directory_options::skip_permission_denied, ec);
         it != std::filesystem::recursive_directory_iterator(); ++it) {
        if (ec) break; if (!it->is_regular_file()) continue; auto ft = it->last_write_time(ec); if (ec) continue; latest = std::max<uint64_t>(latest, static_cast<uint64_t>(ft.time_since_epoch().count()));
    }
    watch_list_.push_back(WatchItem{p.string(), latest});
}

void VulkanEngine::poll_file_watches(const EngineContext& eng) {
    bool changed = false;
    for (auto& w : watch_list_) {
        std::error_code ec{}; uint64_t latest = w.stamp;
        for (auto it = std::filesystem::recursive_directory_iterator(w.path, std::filesystem::directory_options::skip_permission_denied, ec);
             it != std::filesystem::recursive_directory_iterator(); ++it) {
            if (ec) break; if (!it->is_regular_file()) continue; auto ft = it->last_write_time(ec); if (ec) continue; latest = std::max<uint64_t>(latest, static_cast<uint64_t>(ft.time_since_epoch().count()));
        }
        if (latest > w.stamp) { w.stamp = latest; changed = true; }
    }
    if (changed && renderer_) { renderer_->reload_assets(eng); }
}
#endif

#ifdef VV_ENABLE_LOGGING
void VulkanEngine::log_line(const std::string& s) { log_lines_.push_back(s); if (log_lines_.size() > 2000) log_lines_.erase(log_lines_.begin(), log_lines_.begin() + (log_lines_.size() - 2000)); }
#endif
