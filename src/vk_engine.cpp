// ============================================================================
// Vulkan Visualizer - vk_engine.cpp
// Implementation of the VulkanEngine: context/device creation, swapchain &
// offscreen targets management, frame loop, command submission, ImGui
// integration, and renderer delegation.
//
// NOTE: All logic preserved; only comments / structure annotations added for
// clarity and onboarding. Public API documented in vk_engine.h.
// ============================================================================
#include "vk_engine.h"

// --- Compiler diagnostics silencing for third‑party code & VMA implementation
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

// --- External dependencies
#include "VkBootstrap.h"
#include "backends/imgui_impl_sdl3.h"
#include "backends/imgui_impl_vulkan.h"
#include <SDL3/SDL_vulkan.h>
#include <array>
#include <imgui.h>
#include <ranges>
#include <chrono>
#include <stdexcept>

// ============================================================================
// Utility Macros (minimal, self‑contained)
// VK_CHECK            : Throws std::runtime_error on non-success VkResult.
// IF_NOT_NULL_DO      : Execute statement if pointer / handle non-null.
// IF_NOT_NULL_DO_AND_SET : Execute statement then overwrite handle with value.
// REQUIRE_TRUE        : Runtime assertion that throws with message.
// ============================================================================
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

// ============================================================================
// Internal: ImGui / UI System Wrapper
// Encapsulates descriptor pool + ImGui backend initialization for SDL3 + Vulkan
// with dynamic rendering. Provides panel registration & overlay rendering.
// ============================================================================
struct VulkanEngine::UiSystem {
    using PanelFn = std::function<void()>;

    // Initialize ImGui context & Vulkan backend resources.
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

        ImGui_ImplVulkan_InitInfo init_info{}; // Standard ImGui Vulkan init structure
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

    // Release all ImGui/Vulkan backend resources.
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

    // Forward SDL events to ImGui.
    void process_event(const SDL_Event* e) const {
        if (!initialized_ || !e) return;
        ImGui_ImplSDL3_ProcessEvent(e);
    }

    // Start a new ImGui frame & invoke registered panels.
    void new_frame() const {
        if (!initialized_) return;
        ImGui_ImplVulkan_NewFrame();
        ImGui_ImplSDL3_NewFrame();
        ImGui::NewFrame();
        for (auto& panel : panels_) {
            panel();
        }
    }

    // Record overlay rendering to the swapchain image (dynamic rendering path).
    void render_overlay(VkCommandBuffer cmd, VkImage swapchainImage, VkImageView swapchainView, VkExtent2D extent, VkImageLayout previousLayout) const {
        if (!initialized_) return;

        // Transition image to COLOR_ATTACHMENT for ImGui draw
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

        // Dynamic rendering begin
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

        // Multi-viewport rendering
        ImGuiIO& io = ImGui::GetIO();
        if (io.ConfigFlags & ImGuiConfigFlags_ViewportsEnable) {
            ImGui::UpdatePlatformWindows();
            ImGui::RenderPlatformWindowsDefault();
        }

        // Transition image for presentation
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
    }

    // Register an ImGui panel callback executed every frame.
    void add_panel(PanelFn fn) { panels_.push_back(std::move(fn)); }

    // Update backend min image count after swapchain recreation.
    void set_min_image_count(uint32_t count) const {
        if (!initialized_) return;
        ImGui_ImplVulkan_SetMinImageCount(count);
    }

private:
    VkDescriptorPool pool_{VK_NULL_HANDLE}; // ImGui descriptor pool
    bool initialized_{false};               // Init flag
    VkFormat color_format_{};               // Cached color format
    std::vector<PanelFn> panels_;           // Registered UI panels
};

// ============================================================================
// DescriptorAllocator Implementation (simple single-pool helper)
// ============================================================================
void DescriptorAllocator::init_pool(VkDevice device, uint32_t maxSets, std::span<const PoolSizeRatio> ratios) {
    maxSets = std::max(1u, maxSets);
    std::vector<VkDescriptorPoolSize> sizes;
    sizes.reserve(ratios.size());
    for (const auto& [type, ratio] : ratios) {
        const uint32_t count = std::max(1u, static_cast<uint32_t>(ratio * static_cast<float>(maxSets)));
        sizes.push_back(VkDescriptorPoolSize{.type = type, .descriptorCount = count});
    }
    const VkDescriptorPoolCreateInfo info{.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO, .pNext = nullptr, .flags = 0u, .maxSets = maxSets, .poolSizeCount = static_cast<uint32_t>(sizes.size()), .pPoolSizes = sizes.data()};
    VK_CHECK(vkCreateDescriptorPool(device, &info, nullptr, &pool));
}
void DescriptorAllocator::clear_descriptors(VkDevice device) const { if (pool) vkResetDescriptorPool(device, pool, 0); }
void DescriptorAllocator::destroy_pool(VkDevice device) const { if (pool) vkDestroyDescriptorPool(device, pool, nullptr); }
VkDescriptorSet DescriptorAllocator::allocate(VkDevice device, VkDescriptorSetLayout layout) const {
    const VkDescriptorSetAllocateInfo ai{.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO, .pNext = nullptr, .descriptorPool = pool, .descriptorSetCount = 1u, .pSetLayouts = &layout};
    VkDescriptorSet ds{}; VK_CHECK(vkAllocateDescriptorSets(device, &ai, &ds)); return ds; }

// ============================================================================
// VulkanEngine: Ctors / Dtors
// ============================================================================
VulkanEngine::VulkanEngine()  = default;
VulkanEngine::~VulkanEngine() = default;

// ============================================================================
// VulkanEngine :: init
// Creates the context (instance/device), swapchain, offscreen targets, command
// buffers, renderer & ImGui system. Fires initial swapchain-ready callback.
// ============================================================================
void VulkanEngine::init() {
    create_context(state_.width, state_.height, state_.name.c_str());
    create_swapchain(state_.width, state_.height);
    create_offscreen_drawable(state_.width, state_.height);
    create_command_buffers();
    create_renderer();
    create_imgui();

    if (renderer_) {
        renderer_->get_capabilities(renderer_caps_);
        EngineContext eng        = make_engine_context();
        FrameContext frm         = make_frame_context(state_.frame_number, 0u, swapchain_.swapchain_extent);
        frm.dt_sec               = 0.0;
        frm.time_sec             = 0.0;
        frm.swapchain_image      = VK_NULL_HANDLE;
        frm.swapchain_image_view = VK_NULL_HANDLE;
        renderer_->on_swapchain_ready(eng, frm);
    }

    state_.initialized      = true;
    state_.should_rendering = true; // defer setting running flag until run()
}

// ============================================================================
// VulkanEngine :: run
// Main event & frame loop: processes SDL events, handles resize, acquires image,
// builds frame contexts, records rendering + UI, submits & presents.
// ============================================================================
void VulkanEngine::run() {
    // Ensure running state is enabled when entering the loop
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
        // --- Event Pump ---
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
            default: break;
            }
        }

        // --- Time Update ---
        auto t_now      = clock::now();
        state_.dt_sec   = std::chrono::duration<double>(t_now - t_prev).count();
        state_.time_sec = std::chrono::duration<double>(t_now - t0).count();
        t_prev          = t_now;

        if (!state_.should_rendering) { SDL_WaitEventTimeout(nullptr, 100); continue; }

        // --- Resize Handling ---
        if (state_.resize_requested) {
            recreate_swapchain();
            eng                           = make_engine_context();
            last_frm                      = make_frame_context(state_.frame_number, 0u, swapchain_.swapchain_extent);
            last_frm.swapchain_image      = VK_NULL_HANDLE;
            last_frm.swapchain_image_view = VK_NULL_HANDLE;
            continue;
        }

        // --- Frame Begin ---
        uint32_t imageIndex = 0; VkCommandBuffer cmd = VK_NULL_HANDLE;
        begin_frame(imageIndex, cmd);
        if (cmd == VK_NULL_HANDLE) { // Acquire failed due to outdated swapchain
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

        // --- Renderer Work ---
        if (renderer_) {
            renderer_->update(eng, frm);
            renderer_->record_graphics(cmd, eng, frm);
        }

        // --- Composite Offscreen -> Swapchain ---
        blit_offscreen_to_swapchain(cmd, imageIndex, frm.extent);

        // --- UI Rendering ---
        if (ui_) {
            ui_->new_frame();
            if (renderer_) { renderer_->on_imgui(eng, frm); }
            ui_->render_overlay(cmd, frm.swapchain_image, frm.swapchain_image_view, frm.extent, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL);
        }

        // --- Submit + Present ---
        end_frame(imageIndex, cmd);
        state_.frame_number++;
    }
}

// ============================================================================
// VulkanEngine :: cleanup
// Safe teardown: waits for device idle, destroys UI & renderer, command buffers
// and finally the Vulkan context.
// ============================================================================
void VulkanEngine::cleanup() {
    if (ctx_.device) {
        vkDeviceWaitIdle(ctx_.device);
        destroy_imgui();
    }

    if (renderer_) { renderer_->on_swapchain_destroy(make_engine_context()); }

    destroy_command_buffers();
    for (auto& f : std::ranges::reverse_view(mdq_)) { f(); }
    mdq_.clear();
    destroy_context();
}

// Set the renderer implementation (must be done before init()).
void VulkanEngine::set_renderer(std::unique_ptr<IRenderer> r) { renderer_ = std::move(r); }

// ============================================================================
// Context Creation / Destruction
// create_context: builds instance, selects physical device, creates logical
// device, queues, allocator, descriptor allocator & timeline semaphore.
// ============================================================================
void VulkanEngine::create_context(int window_width, int window_height, const char* app_name) {
    vkb::Instance vkb_inst = vkb::InstanceBuilder().set_app_name(app_name).request_validation_layers(false).use_default_debug_messenger().require_api_version(1, 3, 0).build().value();
    ctx_.instance          = vkb_inst.instance;
    ctx_.debug_messenger   = vkb_inst.debug_messenger;
    REQUIRE_TRUE(SDL_Init(SDL_INIT_VIDEO), std::string("SDL_Init failed: ") + SDL_GetError());
    REQUIRE_TRUE(ctx_.window = SDL_CreateWindow(app_name, window_width, window_height, SDL_WINDOW_VULKAN | SDL_WINDOW_RESIZABLE), std::string("SDL_CreateWindow failed: ") + SDL_GetError());
    REQUIRE_TRUE(SDL_Vulkan_CreateSurface(ctx_.window, ctx_.instance, nullptr, &ctx_.surface), std::string("SDL_Vulkan_CreateSurface failed: ") + SDL_GetError());

    VkPhysicalDeviceVulkan13Features f13{.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_3_FEATURES, .pNext = nullptr, .synchronization2 = VK_TRUE, .dynamicRendering = VK_TRUE};
    VkPhysicalDeviceVulkan12Features f12{.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_2_FEATURES, .pNext = &f13, .descriptorIndexing = VK_TRUE, .bufferDeviceAddress = VK_TRUE};
    vkb::PhysicalDevice phys = vkb::PhysicalDeviceSelector(vkb_inst).set_surface(ctx_.surface).set_minimum_version(1, 3).set_required_features_12(f12).select().value();
    ctx_.physical            = phys.physical_device;

    vkb::DeviceBuilder db(phys);
    vkb::Device vkbDev         = db.build().value();
    ctx_.device                = vkbDev.device;
    ctx_.graphics_queue        = vkbDev.get_queue(vkb::QueueType::graphics).value();
    ctx_.compute_queue         = vkbDev.get_queue(vkb::QueueType::compute).value();
    ctx_.transfer_queue        = vkbDev.get_queue(vkb::QueueType::transfer).value();
    ctx_.present_queue         = ctx_.graphics_queue; // Present uses graphics queue
    ctx_.graphics_queue_family = vkbDev.get_queue_index(vkb::QueueType::graphics).value();
    ctx_.compute_queue_family  = vkbDev.get_queue_index(vkb::QueueType::compute).value();
    ctx_.transfer_queue_family = vkbDev.get_queue_index(vkb::QueueType::transfer).value();
    ctx_.present_queue_family  = ctx_.graphics_queue_family;

    VmaAllocatorCreateInfo ac{};
    ac.flags            = VMA_ALLOCATOR_CREATE_BUFFER_DEVICE_ADDRESS_BIT;
    ac.physicalDevice   = ctx_.physical;
    ac.device           = ctx_.device;
    ac.instance         = ctx_.instance;
    ac.vulkanApiVersion = VK_API_VERSION_1_3;
    VK_CHECK(vmaCreateAllocator(&ac, &ctx_.allocator));
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

// Destroy all device-level resources and SDL components.
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

// Build an EngineContext snapshot for renderer usage.
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
    return eng;
}

// Build a FrameContext for current frame including swapchain/offscreen handles.
FrameContext VulkanEngine::make_frame_context(uint64_t frame_index, uint32_t image_index, VkExtent2D extent) const {
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
    frm.offscreen_image      = swapchain_.drawable_image.image;
    frm.offscreen_image_view = swapchain_.drawable_image.imageView;
    frm.depth_image          = swapchain_.depth_image.image;
    frm.depth_image_view     = swapchain_.depth_image.imageView;
    return frm;
}

// Copy / blit the HDR offscreen color target into the acquired swapchain image.
void VulkanEngine::blit_offscreen_to_swapchain(VkCommandBuffer cmd, uint32_t imageIndex, VkExtent2D extent) {
    VkImage src = swapchain_.drawable_image.image; if (src == VK_NULL_HANDLE) return; if (imageIndex >= swapchain_.swapchain_images.size()) return; VkImage dst = swapchain_.swapchain_images[imageIndex];

    VkImageMemoryBarrier2 barriers[2]{};
    barriers[0].sType            = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER_2;
    barriers[0].srcStageMask     = VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT;
    barriers[0].srcAccessMask    = VK_ACCESS_2_MEMORY_WRITE_BIT;
    barriers[0].dstStageMask     = VK_PIPELINE_STAGE_2_TRANSFER_BIT;
    barriers[0].dstAccessMask    = VK_ACCESS_2_TRANSFER_READ_BIT;
    barriers[0].oldLayout        = VK_IMAGE_LAYOUT_GENERAL;
    barriers[0].newLayout        = VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL;
    barriers[0].image            = src;
    barriers[0].subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0u, 1u, 0u, 1u};

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

    VkImageBlit2 blit{}; blit.sType = VK_STRUCTURE_TYPE_IMAGE_BLIT_2; blit.srcSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0u, 0u, 1u}; blit.dstSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0u, 0u, 1u}; blit.srcOffsets[0] = {0, 0, 0}; blit.srcOffsets[1] = {static_cast<int32_t>(swapchain_.drawable_image.imageExtent.width), static_cast<int32_t>(swapchain_.drawable_image.imageExtent.height), 1}; blit.dstOffsets[0] = {0, 0, 0}; blit.dstOffsets[1] = {static_cast<int32_t>(extent.width), static_cast<int32_t>(extent.height), 1};

    VkBlitImageInfo2 bi{}; bi.sType = VK_STRUCTURE_TYPE_BLIT_IMAGE_INFO_2; bi.srcImage = src; bi.srcImageLayout = VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL; bi.dstImage = dst; bi.dstImageLayout = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL; bi.regionCount = 1u; bi.pRegions = &blit; bi.filter = VK_FILTER_LINEAR; vkCmdBlitImage2(cmd, &bi);
}

// ============================================================================
// Swapchain & Offscreen Resource Management
// ============================================================================
void VulkanEngine::create_swapchain(uint32_t width, uint32_t height) {
    swapchain_.swapchain_image_format = VK_FORMAT_B8G8R8A8_UNORM;
    vkb::Swapchain sc                 = vkb::SwapchainBuilder(ctx_.physical, ctx_.device, ctx_.surface)
                            .set_desired_format(VkSurfaceFormatKHR{swapchain_.swapchain_image_format, VK_COLOR_SPACE_SRGB_NONLINEAR_KHR})
                            .set_desired_present_mode(VK_PRESENT_MODE_FIFO_KHR)
                            .set_desired_extent(width, height)
                            .add_image_usage_flags(VK_IMAGE_USAGE_TRANSFER_DST_BIT | VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT)
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
    destroy_offscreen_drawable();

    int pxw = 0; int pxh = 0; SDL_GetWindowSizeInPixels(ctx_.window, &pxw, &pxh); pxw = std::max(1, pxw); pxh = std::max(1, pxh);
    create_swapchain(static_cast<uint32_t>(pxw), static_cast<uint32_t>(pxh));
    create_offscreen_drawable(static_cast<uint32_t>(pxw), static_cast<uint32_t>(pxh));

    FrameContext frm         = make_frame_context(state_.frame_number, 0u, swapchain_.swapchain_extent);
    frm.swapchain_image      = VK_NULL_HANDLE;
    frm.swapchain_image_view = VK_NULL_HANDLE;

    IF_NOT_NULL_DO(renderer_, renderer_->on_swapchain_ready(make_engine_context(), frm));
    IF_NOT_NULL_DO(ui_, ui_->set_min_image_count(static_cast<uint32_t>(swapchain_.swapchain_images.size())));

    state_.resize_requested = false;
}

// Create HDR offscreen color + depth images (single-sampled) used for rendering.
void VulkanEngine::create_offscreen_drawable(uint32_t width, uint32_t height) {
    // Color target (R16G16B16A16_SFLOAT)
    {
        VkImageCreateInfo imgci{.sType = VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO,
            .pNext                     = nullptr,
            .flags                     = 0u,
            .imageType                 = VK_IMAGE_TYPE_2D,
            .format                    = VK_FORMAT_R16G16B16A16_SFLOAT,
            .extent                    = {width, height, 1u},
            .mipLevels                 = 1u,
            .arrayLayers               = 1u,
            .samples                   = VK_SAMPLE_COUNT_1_BIT,
            .tiling                    = VK_IMAGE_TILING_OPTIMAL,
            .usage                     = VK_IMAGE_USAGE_TRANSFER_SRC_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT | VK_IMAGE_USAGE_STORAGE_BIT | VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT,
            .sharingMode               = VK_SHARING_MODE_EXCLUSIVE,
            .queueFamilyIndexCount     = 0u,
            .pQueueFamilyIndices       = nullptr,
            .initialLayout             = VK_IMAGE_LAYOUT_UNDEFINED};
        VmaAllocationCreateInfo ainfo{.flags = 0u, .usage = VMA_MEMORY_USAGE_GPU_ONLY, .requiredFlags = static_cast<VkMemoryPropertyFlags>(VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT), .preferredFlags = 0u, .memoryTypeBits = 0u, .pool = VK_NULL_HANDLE, .pUserData = nullptr, .priority = 1.0f};
        VK_CHECK(vmaCreateImage(ctx_.allocator, &imgci, &ainfo, &swapchain_.drawable_image.image, &swapchain_.drawable_image.allocation, nullptr));
        VkImageViewCreateInfo viewci{.sType = VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO,
            .pNext                          = nullptr,
            .flags                          = 0u,
            .image                          = swapchain_.drawable_image.image,
            .viewType                       = VK_IMAGE_VIEW_TYPE_2D,
            .format                         = VK_FORMAT_R16G16B16A16_SFLOAT,
            .components                     = {VK_COMPONENT_SWIZZLE_IDENTITY, VK_COMPONENT_SWIZZLE_IDENTITY, VK_COMPONENT_SWIZZLE_IDENTITY, VK_COMPONENT_SWIZZLE_IDENTITY},
            .subresourceRange               = {VK_IMAGE_ASPECT_COLOR_BIT, 0u, 1u, 0u, 1u}};
        VK_CHECK(vkCreateImageView(ctx_.device, &viewci, nullptr, &swapchain_.drawable_image.imageView));
        swapchain_.drawable_image.imageFormat = VK_FORMAT_R16G16B16A16_SFLOAT;
        swapchain_.drawable_image.imageExtent = {width, height, 1u};
    }
    // Depth target (D32_SFLOAT)
    {
        VkImageCreateInfo imgci{.sType = VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO,
            .pNext                     = nullptr,
            .flags                     = 0u,
            .imageType                 = VK_IMAGE_TYPE_2D,
            .format                    = VK_FORMAT_D32_SFLOAT,
            .extent                    = {width, height, 1u},
            .mipLevels                 = 1u,
            .arrayLayers               = 1u,
            .samples                   = VK_SAMPLE_COUNT_1_BIT,
            .tiling                    = VK_IMAGE_TILING_OPTIMAL,
            .usage                     = VK_IMAGE_USAGE_DEPTH_STENCIL_ATTACHMENT_BIT,
            .sharingMode               = VK_SHARING_MODE_EXCLUSIVE,
            .queueFamilyIndexCount     = 0u,
            .pQueueFamilyIndices       = nullptr,
            .initialLayout             = VK_IMAGE_LAYOUT_UNDEFINED};
        VmaAllocationCreateInfo ainfo{.flags = 0u, .usage = VMA_MEMORY_USAGE_GPU_ONLY, .requiredFlags = static_cast<VkMemoryPropertyFlags>(VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT), .preferredFlags = 0u, .memoryTypeBits = 0u, .pool = VK_NULL_HANDLE, .pUserData = nullptr, .priority = 1.0f};
        VK_CHECK(vmaCreateImage(ctx_.allocator, &imgci, &ainfo, &swapchain_.depth_image.image, &swapchain_.depth_image.allocation, nullptr));
        VkImageViewCreateInfo viewci{.sType = VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO,
            .pNext                          = nullptr,
            .flags                          = 0u,
            .image                          = swapchain_.depth_image.image,
            .viewType                       = VK_IMAGE_VIEW_TYPE_2D,
            .format                         = VK_FORMAT_D32_SFLOAT,
            .components                     = {VK_COMPONENT_SWIZZLE_IDENTITY, VK_COMPONENT_SWIZZLE_IDENTITY, VK_COMPONENT_SWIZZLE_IDENTITY, VK_COMPONENT_SWIZZLE_IDENTITY},
            .subresourceRange               = {VK_IMAGE_ASPECT_DEPTH_BIT, 0u, 1u, 0u, 1u}};
        VK_CHECK(vkCreateImageView(ctx_.device, &viewci, nullptr, &swapchain_.depth_image.imageView));
        swapchain_.depth_image.imageFormat = VK_FORMAT_D32_SFLOAT;
        swapchain_.depth_image.imageExtent = {width, height, 1u};
    }
    mdq_.emplace_back([&] { destroy_offscreen_drawable(); });
}

// Destroy offscreen images + views.
void VulkanEngine::destroy_offscreen_drawable() {
    IF_NOT_NULL_DO_AND_SET(swapchain_.drawable_image.imageView, vkDestroyImageView(ctx_.device, swapchain_.drawable_image.imageView, nullptr), VK_NULL_HANDLE);
    IF_NOT_NULL_DO_AND_SET(swapchain_.drawable_image.image, vmaDestroyImage(ctx_.allocator, swapchain_.drawable_image.image, swapchain_.drawable_image.allocation), VK_NULL_HANDLE);
    swapchain_.drawable_image = {};
    IF_NOT_NULL_DO_AND_SET(swapchain_.depth_image.imageView, vkDestroyImageView(ctx_.device, swapchain_.depth_image.imageView, nullptr), VK_NULL_HANDLE);
    IF_NOT_NULL_DO_AND_SET(swapchain_.depth_image.image, vmaDestroyImage(ctx_.allocator, swapchain_.depth_image.image, swapchain_.depth_image.allocation), VK_NULL_HANDLE);
    swapchain_.depth_image = {};
}

// ============================================================================
// Command Buffers / Synchronization
// ============================================================================
void VulkanEngine::create_command_buffers() {
    VkCommandPoolCreateInfo pci{.sType = VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO, .pNext = nullptr, .flags = VK_COMMAND_POOL_CREATE_RESET_COMMAND_BUFFER_BIT, .queueFamilyIndex = ctx_.graphics_queue_family};
    for (auto& fr : frames_) {
        VK_CHECK(vkCreateCommandPool(ctx_.device, &pci, nullptr, &fr.commandPool));
        VkCommandBufferAllocateInfo ai{.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO, .pNext = nullptr, .commandPool = fr.commandPool, .level = VK_COMMAND_BUFFER_LEVEL_PRIMARY, .commandBufferCount = 1u};
        VK_CHECK(vkAllocateCommandBuffers(ctx_.device, &ai, &fr.mainCommandBuffer));
        VkSemaphoreCreateInfo sci{.sType = VK_STRUCTURE_TYPE_SEMAPHORE_CREATE_INFO, .pNext = nullptr, .flags = 0u};
        VK_CHECK(vkCreateSemaphore(ctx_.device, &sci, nullptr, &fr.imageAcquired));
        VK_CHECK(vkCreateSemaphore(ctx_.device, &sci, nullptr, &fr.renderComplete));
    }
    mdq_.emplace_back([&] { destroy_command_buffers(); });
}
void VulkanEngine::destroy_command_buffers() {
    for (auto& [commandPool, mainCommandBuffer, imageAcquired, renderComplete, submitted_timeline_value, dq] : frames_) {
        for (auto& f : std::ranges::reverse_view(dq)) f();
        dq.clear();
        IF_NOT_NULL_DO_AND_SET(imageAcquired, vkDestroySemaphore(ctx_.device, imageAcquired, nullptr), VK_NULL_HANDLE);
        IF_NOT_NULL_DO_AND_SET(renderComplete, vkDestroySemaphore(ctx_.device, renderComplete, nullptr), VK_NULL_HANDLE);
        IF_NOT_NULL_DO_AND_SET(commandPool, vkDestroyCommandPool(ctx_.device, commandPool, nullptr), VK_NULL_HANDLE);
        mainCommandBuffer        = VK_NULL_HANDLE;
        submitted_timeline_value = 0;
    }
}

// Acquire next image, wait for previous frame (timeline), and begin command buffer.
void VulkanEngine::begin_frame(uint32_t& imageIndex, VkCommandBuffer& cmd) {
    FrameData& fr = frames_[state_.frame_number % FRAME_OVERLAP];
    if (fr.submitted_timeline_value > 0) {
        VkSemaphore sem = render_timeline_;
        uint64_t val    = fr.submitted_timeline_value;
        VkSemaphoreWaitInfo wi{.sType = VK_STRUCTURE_TYPE_SEMAPHORE_WAIT_INFO, .pNext = nullptr, .flags = 0u, .semaphoreCount = 1u, .pSemaphores = &sem, .pValues = &val};
        VK_CHECK(vkWaitSemaphores(ctx_.device, &wi, UINT64_MAX));
    }
    const VkResult acq = vkAcquireNextImageKHR(ctx_.device, swapchain_.swapchain, UINT64_MAX, fr.imageAcquired, VK_NULL_HANDLE, &imageIndex);
    if (acq == VK_ERROR_OUT_OF_DATE_KHR || acq == VK_SUBOPTIMAL_KHR) {
        state_.resize_requested = true; cmd = VK_NULL_HANDLE; return; }
    VK_CHECK(acq);
    VK_CHECK(vkResetCommandBuffer(fr.mainCommandBuffer, 0));
    cmd = fr.mainCommandBuffer;
    VkCommandBufferBeginInfo bi{.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO, .pNext = nullptr, .flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT, .pInheritanceInfo = nullptr};
    VK_CHECK(vkBeginCommandBuffer(cmd, &bi));
}

// End recording, submit (with timeline + binary semaphores) and present image.
void VulkanEngine::end_frame(uint32_t imageIndex, VkCommandBuffer cmd) {
    VK_CHECK(vkEndCommandBuffer(cmd));
    FrameData& fr = frames_[state_.frame_number % FRAME_OVERLAP];
    VkCommandBufferSubmitInfo cbsi{.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_SUBMIT_INFO, .pNext = nullptr, .commandBuffer = cmd, .deviceMask = 0u};
    VkSemaphoreSubmitInfo waitInfo{.sType = VK_STRUCTURE_TYPE_SEMAPHORE_SUBMIT_INFO, .pNext = nullptr, .semaphore = fr.imageAcquired, .value = 0u, .stageMask = VK_PIPELINE_STAGE_2_COLOR_ATTACHMENT_OUTPUT_BIT, .deviceIndex = 0u};
    timeline_value_++;
    uint64_t timeline_to_signal = timeline_value_;
    VkSemaphoreSubmitInfo signalInfos[2]{VkSemaphoreSubmitInfo{.sType = VK_STRUCTURE_TYPE_SEMAPHORE_SUBMIT_INFO, .pNext = nullptr, .semaphore = fr.renderComplete, .value = 0u, .stageMask = VK_PIPELINE_STAGE_2_ALL_GRAPHICS_BIT, .deviceIndex = 0u},
        VkSemaphoreSubmitInfo{.sType = VK_STRUCTURE_TYPE_SEMAPHORE_SUBMIT_INFO, .pNext = nullptr, .semaphore = render_timeline_, .value = timeline_to_signal, .stageMask = VK_PIPELINE_STAGE_2_ALL_GRAPHICS_BIT, .deviceIndex = 0u}};
    VkSubmitInfo2 si{.sType = VK_STRUCTURE_TYPE_SUBMIT_INFO_2, .pNext = nullptr, .waitSemaphoreInfoCount = 1u, .pWaitSemaphoreInfos = &waitInfo, .commandBufferInfoCount = 1u, .pCommandBufferInfos = &cbsi, .signalSemaphoreInfoCount = 2u, .pSignalSemaphoreInfos = signalInfos};
    VK_CHECK(vkQueueSubmit2(ctx_.graphics_queue, 1, &si, VK_NULL_HANDLE));
    fr.submitted_timeline_value = timeline_to_signal;
    VkPresentInfoKHR pi{.sType = VK_STRUCTURE_TYPE_PRESENT_INFO_KHR, .pNext = nullptr, .waitSemaphoreCount = 1u, .pWaitSemaphores = &fr.renderComplete, .swapchainCount = 1u, .pSwapchains = &swapchain_.swapchain, .pImageIndices = &imageIndex, .pResults = nullptr};
    VkResult pres = vkQueuePresentKHR(ctx_.graphics_queue, &pi);
    if (pres == VK_ERROR_OUT_OF_DATE_KHR || pres == VK_SUBOPTIMAL_KHR) { state_.resize_requested = true; return; }
    VK_CHECK(pres);
}

// ============================================================================
// Renderer Integration
// ============================================================================
void VulkanEngine::create_renderer() {
    if (!renderer_) { throw std::runtime_error("Renderer not set"); }
    EngineContext eng{};
    eng.instance              = ctx_.instance;
    eng.physical              = ctx_.physical;
    eng.device                = ctx_.device;
    eng.allocator             = ctx_.allocator;
    eng.descriptorAllocator   = &ctx_.descriptor_allocator;
    eng.window                = ctx_.window;
    eng.graphics_queue        = ctx_.graphics_queue;
    eng.compute_queue         = ctx_.compute_queue;
    eng.transfer_queue        = ctx_.transfer_queue;
    eng.present_queue         = ctx_.present_queue;
    eng.graphics_queue_family = ctx_.graphics_queue_family;
    eng.compute_queue_family  = ctx_.compute_queue_family;
    eng.transfer_queue_family = ctx_.transfer_queue_family;
    eng.present_queue_family  = ctx_.present_queue_family;
    renderer_->initialize(eng);
    mdq_.emplace_back([&] { destroy_renderer(); });
}
void VulkanEngine::destroy_renderer() {
    IF_NOT_NULL_DO_AND_SET(
        renderer_,
        {
            EngineContext eng{};
            eng.instance              = ctx_.instance;
            eng.physical              = ctx_.physical;
            eng.device                = ctx_.device;
            eng.allocator             = ctx_.allocator;
            eng.descriptorAllocator   = &ctx_.descriptor_allocator;
            eng.window                = ctx_.window;
            eng.graphics_queue        = ctx_.graphics_queue;
            eng.compute_queue         = ctx_.compute_queue;
            eng.transfer_queue        = ctx_.transfer_queue;
            eng.present_queue         = ctx_.present_queue;
            eng.graphics_queue_family = ctx_.graphics_queue_family;
            eng.compute_queue_family  = ctx_.compute_queue_family;
            eng.transfer_queue_family = ctx_.transfer_queue_family;
            eng.present_queue_family  = ctx_.present_queue_family;
            renderer_->destroy(eng);
            renderer_.reset();
        },
        nullptr);
}

// ============================================================================
// ImGui Integration (engine-level convenience wrappers)
// ============================================================================
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

    ImGuiStyle& style      = ImGui::GetStyle();
    style.WindowRounding   = 0.0f;
    style.WindowBorderSize = 0.0f;
    style.FrameRounding    = 4.0f;
    style.GrabRounding     = 4.0f;

    VkPhysicalDeviceProperties props{}; vkGetPhysicalDeviceProperties(ctx_.physical, &props);
    VkPhysicalDeviceMemoryProperties memProps{}; vkGetPhysicalDeviceMemoryProperties(ctx_.physical, &memProps);

    // Dockspace panel
    ui_->add_panel([] {
        ImGuiDockNodeFlags flags = ImGuiDockNodeFlags_PassthruCentralNode | ImGuiDockNodeFlags_AutoHideTabBar;
        static ImGuiID dockspace_id = 0; dockspace_id = ImGui::DockSpaceOverViewport(dockspace_id, nullptr, flags, nullptr);
    });

    // HUD panel (debug / stats)
    ui_->add_panel([this, props, memProps] {
        ImGuiViewport* vp = ImGui::GetMainViewport();
        ImVec2 pad(12.0f, 12.0f);

        ImGui::SetNextWindowViewport(vp->ID);
        ImGui::SetNextWindowPos(ImVec2(vp->WorkPos.x + pad.x, vp->WorkPos.y + pad.y), ImGuiCond_Always);
        ImGui::SetNextWindowBgAlpha(0.32f);

        ImGuiWindowFlags flags = ImGuiWindowFlags_NoDecoration | ImGuiWindowFlags_NoDocking | ImGuiWindowFlags_NoMove | ImGuiWindowFlags_NoSavedSettings | ImGuiWindowFlags_NoNav | ImGuiWindowFlags_NoFocusOnAppearing | ImGuiWindowFlags_AlwaysAutoResize;

        if (ImGui::Begin("HUD##top-left", nullptr, flags)) {
            const ImGuiIO& io = ImGui::GetIO();
            const float fps   = io.Framerate;
            const float ms    = fps > 0.f ? 1000.f / fps : 0.f;

            ImGui::Text("FPS: %.1f (%.2f ms)", fps, ms);
            ImGui::SeparatorText("Frame");
            ImGui::Text("Frame#:  %llu", static_cast<unsigned long long>(state_.frame_number));
            ImGui::Text("Time:    %.3f s", state_.time_sec);
            ImGui::Text("dt:      %.3f ms", state_.dt_sec * 1000.0);

            ImGui::SeparatorText("Swapchain");
            ImGui::Text("Extent:  %u x %u", swapchain_.swapchain_extent.width, swapchain_.swapchain_extent.height);
            ImGui::Text("Images:  %zu", swapchain_.swapchain_images.size());
            ImGui::Text("Format:  0x%08X", static_cast<uint32_t>(swapchain_.swapchain_image_format));

            ImGui::SeparatorText("Offscreen");
            ImGui::Text("Color:   0x%08X", static_cast<uint32_t>(swapchain_.drawable_image.imageFormat));
            ImGui::Text("Depth:   0x%08X", static_cast<uint32_t>(swapchain_.depth_image.imageFormat));

            ImGui::SeparatorText("Window");
            int lw = 0, lh = 0, pw = 0, ph = 0; SDL_GetWindowSize(ctx_.window, &lw, &lh); SDL_GetWindowSizeInPixels(ctx_.window, &pw, &ph);
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
            } else {
                ImGui::TextUnformatted("(no renderer)");
            }

            ImGui::SeparatorText("Sync");
            ImGui::Text("Timeline value: %llu", static_cast<unsigned long long>(timeline_value_));

            ImGui::SeparatorText("Memory (VMA)");
            std::vector<VmaBudget> budgets(memProps.memoryHeapCount);
            vmaGetHeapBudgets(ctx_.allocator, budgets.data());
            uint64_t totalBudget = 0, totalUsage = 0; for (uint32_t i = 0; i < memProps.memoryHeapCount; ++i) { totalBudget += budgets[i].budget; totalUsage += budgets[i].usage; }
            auto fmtMB = [](uint64_t bytes) { return double(bytes) / (1024.0 * 1024.0); };
            ImGui::Text("Usage:  %.1f MB / %.1f MB", fmtMB(totalUsage), fmtMB(totalBudget));
        }
        ImGui::End();
    });
}

// Destroy ImGui backend & context.
void VulkanEngine::destroy_imgui() { if (ui_) { ui_->shutdown(ctx_.device); ui_.reset(); } }
