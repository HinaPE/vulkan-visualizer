// ============================================================================
// Vulkan Visualizer - Core Engine Interfaces & Types
// This header defines the public engine context structures, renderer contracts
// and the main VulkanEngine responsible for initialization, frame lifecycle,
// swapchain management, command submission, ImGui integration, and renderer
// delegation.
//
// All types here are intentionally lightweight POD-style where possible to
// simplify construction and inspection (e.g. inside debug UI panels).
// ============================================================================
#ifndef VULKAN_VISUALIZER_VK_ENGINE_H
#define VULKAN_VISUALIZER_VK_ENGINE_H

#include "vk_mem_alloc.h"
#include <SDL3/SDL.h>
#include <functional>
#include <memory>
#include <span>
#include <string>
#include <vector>
#include <vulkan/vulkan.h>

// Number of frames-in-flight (double buffering for CPU frame overlap)
inline constexpr unsigned int FRAME_OVERLAP = 2;

// ----------------------------------------------------------------------------
// DescriptorAllocator
// Simple linear-style descriptor pool allocator with a single VkDescriptorPool.
// Call init_pool() once, then allocate() descriptor sets from it. Use
// clear_descriptors() between frames / recreations if you want to recycle sets.
// ----------------------------------------------------------------------------
struct DescriptorAllocator {
    struct PoolSizeRatio {
        VkDescriptorType type;  // Descriptor type (e.g. VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER)
        float ratio;            // Relative share of the maxSets budget for this type
    };
    VkDescriptorPool pool{};     // Underlying Vulkan descriptor pool

    // Create the descriptor pool. 'maxSets' is the number of descriptor sets
    // expected; each PoolSizeRatio scales descriptor counts proportionally.
    void init_pool(VkDevice device, uint32_t maxSets, std::span<const PoolSizeRatio> ratios);

    // Reset (recycle) all descriptor sets but keep pool memory.
    void clear_descriptors(VkDevice device) const;

    // Destroy the pool (must not be in use by the device).
    void destroy_pool(VkDevice device) const;

    // Allocate a single descriptor set from the pool using the given layout.
    VkDescriptorSet allocate(VkDevice device, VkDescriptorSetLayout layout) const;
};

// ============================================================================
// Engine Context Structures
// These structs are passed to renderer implementations to give them access to
// device resources and per-frame data in a controlled, read-only fashion.
// ============================================================================

// Immutable (during a frame) device-level handles & global systems.
struct EngineContext {
    VkInstance instance{};               // Vulkan instance
    VkPhysicalDevice physical{};         // Chosen physical device
    VkDevice device{};                   // Logical device
    VmaAllocator allocator{};            // VMA allocator for GPU memory
    DescriptorAllocator* descriptorAllocator{}; // Descriptor allocations helper
    SDL_Window* window{};                // SDL window for surface/platform events
    VkQueue graphics_queue{};            // Graphics queue handle
    VkQueue compute_queue{};             // Compute queue handle
    VkQueue transfer_queue{};            // Transfer queue handle
    VkQueue present_queue{};             // Presentation queue (often same as graphics)
    uint32_t graphics_queue_family{};    // Family index for graphics queue
    uint32_t compute_queue_family{};     // Family index for compute queue
    uint32_t transfer_queue_family{};    // Family index for transfer queue
    uint32_t present_queue_family{};     // Family index for present queue
};

// Per-frame dynamic values and swapchain/offscreen image references.
struct FrameContext {
    uint64_t frame_index{};              // Absolute frame counter since engine start
    uint32_t image_index{};              // Current swapchain image index
    VkExtent2D extent{};                 // Output surface extent (pixels)
    VkFormat swapchain_format{};         // Swapchain image format
    double dt_sec{};                     // Delta-time (seconds) for the frame
    double time_sec{};                   // Accumulated time (seconds) since start

    // Active presentation image + view (may be VK_NULL_HANDLE for callbacks
    // fired before acquire)
    VkImage swapchain_image{};
    VkImageView swapchain_image_view{};

    // Offscreen (HDR / intermediate) color target + view
    VkImage offscreen_image{};
    VkImageView offscreen_image_view{};

    // Optional depth buffer resources
    VkImage depth_image{VK_NULL_HANDLE};
    VkImageView depth_image_view{VK_NULL_HANDLE};
};

// Renderer capability flags communicated from renderer to engine/UI.
struct RendererCaps {
    uint32_t api_version{};              // Vulkan API version expected / used
    uint32_t frames_in_flight{FRAME_OVERLAP}; // Renderer requested frames in flight
    VkBool32 dynamic_rendering{VK_TRUE}; // Uses dynamic rendering instead of render passes
    VkBool32 timeline_semaphore{VK_TRUE};
    VkBool32 descriptor_indexing{VK_TRUE};
    VkBool32 buffer_device_address{VK_TRUE};
    VkBool32 uses_depth{VK_FALSE};       // Needs a depth image
    VkBool32 uses_offscreen{VK_TRUE};    // Needs offscreen intermediate render target
};

// Runtime statistics optionally reported by renderer each frame.
struct RendererStats {
    uint64_t draw_calls{};               // Number of draw calls recorded
    uint64_t dispatches{};               // Number of compute dispatches
    uint64_t triangles{};                // Estimated triangle count
    double cpu_ms{};                     // CPU frame time (ms) (renderer side)
    double gpu_ms{};                     // GPU frame time (ms) (collected via queries)
};

// ============================================================================
// IRenderer - Abstract renderer interface implemented by the application.
// The engine owns an IRenderer instance and calls into it for initialization,
// per-frame logic, command recording and UI integration.
// Implementations may selectively override optional hooks (with default empty
// implementations provided here for convenience).
// ============================================================================
class IRenderer {
public:
    virtual ~IRenderer() = default;

    // Mandatory: allocate persistent GPU resources, pipelines, etc.
    virtual void initialize(const EngineContext& eng) = 0;

    // Mandatory: free all resources created in initialize() / swapchain hooks.
    virtual void destroy(const EngineContext& eng) = 0;

    // Notified after a new / recreated swapchain + related drawables exist.
    virtual void on_swapchain_ready(const EngineContext& eng, const FrameContext& frm) { (void)eng; (void)frm; }

    // Notified just before swapchain images are destroyed.
    virtual void on_swapchain_destroy(const EngineContext& eng) { (void)eng; }

    // Per-frame CPU simulation / logic update (no command buffer).
    virtual void update(const EngineContext& eng, const FrameContext& frm) { (void)eng; (void)frm; }

    // Mandatory: record graphics commands into provided primary command buffer.
    virtual void record_graphics(VkCommandBuffer cmd, const EngineContext& eng, const FrameContext& frm) = 0;

    // Optional: record compute work in same frame (invoked after update()).
    virtual void record_compute(VkCommandBuffer cmd, const EngineContext& eng, const FrameContext& frm) { (void)cmd; (void)eng; (void)frm; }

    // Raw SDL event forward (input, window, etc.)
    virtual void on_event(const SDL_Event& e, const EngineContext& eng, const FrameContext* frm) { (void)e; (void)eng; (void)frm; }

    // Provide additional ImGui panels (called between begin/end frame UI).
    virtual void on_imgui(const EngineContext& eng, const FrameContext& frm) { (void)eng; (void)frm; }

    // Hot-reload assets (e.g. shaders) on external trigger.
    virtual void reload_assets(const EngineContext& eng) { (void)eng; }

    // Async screenshot request path (engine may observe and capture later).
    virtual void request_screenshot(const char* path) { (void)path; }

    // Query static/negotiated renderer capabilities.
    virtual void get_capabilities(RendererCaps& out_caps) const { out_caps = RendererCaps{}; }

    // Runtime statistics retrieval (should be fast / lock free).
    [[nodiscard]] virtual RendererStats get_stats() const { return RendererStats{}; }

    // Simple key/value option interface (tuning, debug toggles, etc.)
    virtual void set_option_int(const char* key, int v) { (void)key; (void)v; }
    virtual void set_option_float(const char* key, float v) { (void)key; (void)v; }
    virtual void set_option_str(const char* key, const char* v) { (void)key; (void)v; }
    virtual bool get_option_int(const char* key, int& v) const { (void)key; (void)v; return false; }
    virtual bool get_option_float(const char* key, float& v) const { (void)key; (void)v; return false; }
    virtual bool get_option_str(const char* key, const char*& v) const { (void)key; (void)v; return false; }
};

// ============================================================================
// VulkanEngine - Orchestrates Vulkan setup, frame loop, swapchain, command
// submission, synchronization, ImGui overlays, and delegates rendering to an
// external IRenderer implementation.
// ============================================================================
class VulkanEngine {
public:
    VulkanEngine();
    ~VulkanEngine();
    VulkanEngine(const VulkanEngine&)                = delete;
    VulkanEngine& operator=(const VulkanEngine&)     = delete;
    VulkanEngine(VulkanEngine&&) noexcept            = default;
    VulkanEngine& operator=(VulkanEngine&&) noexcept = default;

    // Initialize the entire engine (instance, device, swapchain, renderer, UI).
    void init();
    // Run main loop until exit event (blocking call).
    void run();
    // Destroy resources (safe to call multiple times; guards included).
    void cleanup();

    // Provide ownership of renderer implementation before init().
    void set_renderer(std::unique_ptr<IRenderer> r);

    // Mutable engine-wide state (lightweight). Public for debug readability.
    struct {
        std::string name = "Vulkan Engine"; // Window / application title
        int width{1700};                    // Initial window width (logical units)
        int height{800};                    // Initial window height (logical units)
        bool initialized{false};            // Becomes true after init()
        bool running{false};                // Main loop active flag
        bool should_rendering{false};       // Skip rendering when minimized / unfocused
        bool resize_requested{false};       // Swapchain recreation flag
        bool focused{true};                 // Window focus state
        bool minimized{false};              // Window minimized state
        uint64_t frame_number{0};           // Absolute frame counter
        double time_sec{0.0};               // Total accumulated time
        double dt_sec{0.0};                 // Time elapsed since previous frame
    } state_;

private: // --- Context creation & destruction ---
    void create_context(int window_width, int window_height, const char* app_name);
    void destroy_context();
    EngineContext make_engine_context() const;                       // Build EngineContext each frame
    FrameContext make_frame_context(uint64_t frame_index, uint32_t image_index, VkExtent2D extent) const; // Build FrameContext snapshot
    void blit_offscreen_to_swapchain(VkCommandBuffer cmd, uint32_t imageIndex, VkExtent2D extent);        // Copy HDR/offscreen to swapchain

    struct DeviceContext {
        VkInstance instance{};                 // Vulkan instance
        VkDebugUtilsMessengerEXT debug_messenger{}; // Debug messenger (if enabled)
        SDL_Window* window{nullptr};           // SDL window handle
        VkSurfaceKHR surface{};                // Presentation surface
        VkPhysicalDevice physical{};           // Chosen physical device
        VkDevice device{};                     // Logical device
        VkQueue graphics_queue{};              // Graphics queue
        VkQueue compute_queue{};               // Compute queue
        VkQueue transfer_queue{};              // Transfer queue
        VkQueue present_queue{};               // Present queue
        uint32_t graphics_queue_family{};      // Graphics queue family index
        uint32_t compute_queue_family{};       // Compute queue family index
        uint32_t transfer_queue_family{};      // Transfer queue family index
        uint32_t present_queue_family{};       // Present queue family index
        VmaAllocator allocator{};              // VMA allocator
        DescriptorAllocator descriptor_allocator; // Descriptor allocator
    } ctx_{};                                  // Live device context

private: // --- Swapchain & Offscreen Targets ---
    void create_swapchain(uint32_t width, uint32_t height);
    void destroy_swapchain();
    void recreate_swapchain();                // Handle window resize / pixel density changes

    void create_offscreen_drawable(uint32_t width, uint32_t height); // Create intermediate color/depth
    void destroy_offscreen_drawable();

    struct AllocatedImage {
        VkImage image{};                      // Vulkan image handle
        VkImageView imageView{};              // Associated view
        VmaAllocation allocation{};           // VMA allocation handle
        VkExtent3D imageExtent{};             // Dimensions
        VkFormat imageFormat{};               // Format
    };

    struct SwapchainSystem {
        VkSwapchainKHR swapchain{};           // Swapchain handle
        VkFormat swapchain_image_format{};    // Image format
        VkExtent2D swapchain_extent{};        // Current extent
        std::vector<VkImage> swapchain_images;        // Images
        std::vector<VkImageView> swapchain_image_views; // Image views
        AllocatedImage drawable_image;        // Offscreen color (HDR) target
        AllocatedImage depth_image{};         // Depth target
    } swapchain_{};                           // Aggregated swapchain state

private: // --- Command Submission & Synchronization ---
    void create_command_buffers();
    void destroy_command_buffers();
    void begin_frame(uint32_t& imageIndex, VkCommandBuffer& cmd); // Acquire image + begin cmd buffer
    void end_frame(uint32_t imageIndex, VkCommandBuffer cmd);     // Submit + present

    struct FrameData {
        VkCommandPool commandPool{};          // Per-frame command pool
        VkCommandBuffer mainCommandBuffer{};  // Primary command buffer
        VkSemaphore imageAcquired{};          // Signals when swapchain image is ready
        VkSemaphore renderComplete{};         // Signals when rendering finished
        uint64_t submitted_timeline_value{0}; // Last timeline value signaled for this frame
        std::vector<std::move_only_function<void()>> dq; // Deferred destruction queue (frame-scoped)
    } frames_[FRAME_OVERLAP]{};               // Ring buffer of per-frame resources

    VkSemaphore render_timeline_{};           // Timeline semaphore for frame completion
    uint64_t timeline_value_{0};              // Global timeline value counter

private: // --- Renderer Integration ---
    void create_renderer();
    void destroy_renderer();
    std::unique_ptr<IRenderer> renderer_;     // Active renderer implementation
    RendererCaps renderer_caps_{};            // Cached renderer capabilities

private: // --- ImGui Integration ---
    struct UiSystem;                          // Forward-declared internal UI system
    void create_imgui();
    void destroy_imgui();
    std::unique_ptr<UiSystem> ui_;            // ImGui system object
    std::vector<std::move_only_function<void()>> mdq_; // Master destruction queue (engine lifetime)
};

#endif // VULKAN_VISUALIZER_VK_ENGINE_H
