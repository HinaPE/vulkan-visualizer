#ifndef VULKAN_VISUALIZER_VK_ENGINE_H
#define VULKAN_VISUALIZER_VK_ENGINE_H

#include <SDL3/SDL.h>
#include <vulkan/vulkan.h>

#include <cstdint>
#include <functional>
#include <memory>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <vector>

// Minimal UI tabs host interface exposed via EngineContext.services (if ImGui is enabled)
namespace vv_ui {
struct TabsHost {
    virtual ~TabsHost() = default;
    virtual void add_tab(const char* name, std::function<void()> fn) = 0;     // per-frame ephemeral tabs
    virtual void set_main_window_title(const char* title) = 0;                // optional
    // New: per-frame overlay callbacks (drawn after tabs, before ImGui::Render)
    virtual void add_overlay(std::function<void()> fn) = 0;
};
} // namespace vv_ui

// VMA handle fwd decls
struct VmaAllocator_T; using VmaAllocator = VmaAllocator_T*;
struct VmaAllocation_T; using VmaAllocation = VmaAllocation_T*;

#ifdef VV_ENABLE_LOGGING
#include <cstdio>
#define VV_LOG_INFO(fmt, ...)  do { std::fprintf(stderr, "[INFO] " fmt "\n", ##__VA_ARGS__); } while(0)
#define VV_LOG_WARN(fmt, ...)  do { std::fprintf(stderr, "[WARN] " fmt "\n", ##__VA_ARGS__); } while(0)
#define VV_LOG_ERROR(fmt, ...) do { std::fprintf(stderr, "[ERROR] " fmt "\n", ##__VA_ARGS__); } while(0)
#else
#define VV_LOG_INFO(...)  do {} while(0)
#define VV_LOG_WARN(...)  do {} while(0)
#define VV_LOG_ERROR(...) do {} while(0)
#endif

inline constexpr unsigned int FRAME_OVERLAP = 2;

struct DescriptorAllocator {
    struct PoolSizeRatio { VkDescriptorType type; float ratio; };
    VkDescriptorPool pool{};
    void init_pool(VkDevice device, uint32_t maxSets, std::span<const PoolSizeRatio> ratios);
    void clear_descriptors(VkDevice device) const;
    void destroy_pool(VkDevice device) const;
    VkDescriptorSet allocate(VkDevice device, VkDescriptorSetLayout layout) const;
};

enum class PresentationMode : uint8_t { EngineBlit, RendererComposite, DirectToSwapchain };

struct AttachmentRequest {
    std::string name;
    VkFormat format{VK_FORMAT_R16G16B16A16_SFLOAT};
    VkImageUsageFlags usage{VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT | VK_IMAGE_USAGE_STORAGE_BIT | VK_IMAGE_USAGE_TRANSFER_SRC_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT};
    VkSampleCountFlagBits samples{VK_SAMPLE_COUNT_1_BIT};
    VkImageAspectFlags aspect{VK_IMAGE_ASPECT_COLOR_BIT};
    VkImageLayout initial_layout{VK_IMAGE_LAYOUT_GENERAL};
};

struct AttachmentView {
    std::string_view name;
    VkImage image{VK_NULL_HANDLE};
    VkImageView view{VK_NULL_HANDLE};
    VkFormat format{VK_FORMAT_UNDEFINED};
    VkExtent3D extent{};
    VkSampleCountFlagBits samples{VK_SAMPLE_COUNT_1_BIT};
    VkImageUsageFlags usage{0};
    VkImageAspectFlags aspect{VK_IMAGE_ASPECT_COLOR_BIT};
    VkImageLayout current_layout{VK_IMAGE_LAYOUT_UNDEFINED};
};

struct EngineContext {
    VkInstance instance{};
    VkPhysicalDevice physical{};
    VkDevice device{};
    VmaAllocator allocator{};
    DescriptorAllocator* descriptorAllocator{};
    SDL_Window* window{};
    VkQueue graphics_queue{};
    VkQueue compute_queue{};
    VkQueue transfer_queue{};
    VkQueue present_queue{};
    uint32_t graphics_queue_family{};
    uint32_t compute_queue_family{};
    uint32_t transfer_queue_family{};
    uint32_t present_queue_family{};
    void* services{}; // if ImGui enabled: points to vv_ui::TabsHost, otherwise nullptr
};

struct FrameContext {
    uint64_t frame_index{};
    uint32_t image_index{};
    VkExtent2D extent{};
    VkFormat swapchain_format{};
    double dt_sec{};
    double time_sec{};
    VkImage swapchain_image{};
    VkImageView swapchain_image_view{};
    VkImage offscreen_image{};
    VkImageView offscreen_image_view{};
    VkImage depth_image{VK_NULL_HANDLE};
    VkImageView depth_image_view{VK_NULL_HANDLE};
    std::span<const AttachmentView> color_attachments{};
    const AttachmentView* depth_attachment{nullptr};
    PresentationMode presentation_mode{PresentationMode::EngineBlit};
};

struct RendererCaps {
    uint32_t api_version{};
    uint32_t frames_in_flight{FRAME_OVERLAP};
    VkBool32 dynamic_rendering{VK_TRUE};
    VkBool32 timeline_semaphore{VK_TRUE};
    VkBool32 descriptor_indexing{VK_TRUE};
    VkBool32 buffer_device_address{VK_TRUE};
    VkBool32 uses_depth{VK_FALSE};
    VkBool32 uses_offscreen{VK_TRUE};
    VkSampleCountFlagBits color_samples{VK_SAMPLE_COUNT_1_BIT};
    PresentationMode presentation_mode{PresentationMode::EngineBlit};
    std::string presentation_attachment{"hdr_color"};
    std::vector<AttachmentRequest> color_attachments{AttachmentRequest{.name = "hdr_color"}};
    std::optional<AttachmentRequest> depth_attachment{};
    VkFormat preferred_swapchain_format{VK_FORMAT_B8G8R8A8_UNORM};
    VkFormat preferred_depth_format{VK_FORMAT_D32_SFLOAT};
    VkImageUsageFlags swapchain_usage{VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT};
    VkPresentModeKHR present_mode{VK_PRESENT_MODE_FIFO_KHR};
    bool enable_imgui{true};
    bool allow_async_compute{false};
    bool allow_async_transfer{false};
    bool need_ray_tracing_pipeline{false};
    bool need_acceleration_structure{false};
    bool need_ray_query{false};
    bool need_mesh_shader{false};
    bool need_shader_int64{false};
    bool need_shader_float16{false};
    std::vector<const char*> extra_instance_extensions{};
    std::vector<const char*> extra_device_extensions{};
};

struct RendererStats { uint64_t draw_calls{}; uint64_t dispatches{}; uint64_t triangles{}; double cpu_ms{}; double gpu_ms{}; };

class IRenderer {
public:
    virtual ~IRenderer() = default;
    virtual void query_required_device_caps(RendererCaps& caps) { (void)caps; }
    virtual void get_capabilities(const EngineContext& eng, RendererCaps& caps) { (void)eng; (void)caps; }
    virtual void initialize(const EngineContext& eng, const RendererCaps& caps, const FrameContext& initial_frame) = 0;
    virtual void destroy(const EngineContext& eng, const RendererCaps& caps) = 0;
    virtual void on_swapchain_ready(const EngineContext& eng, const FrameContext& frm) { (void)eng; (void)frm; }
    virtual void on_swapchain_destroy(const EngineContext& eng) { (void)eng; }
    virtual void simulate(const EngineContext& eng, const FrameContext& frm) { (void)eng; (void)frm; }
    virtual void update(const EngineContext& eng, const FrameContext& frm) { (void)eng; (void)frm; }
    virtual void record_compute(VkCommandBuffer, const EngineContext&, const FrameContext&) {}
    virtual bool record_async_compute(VkCommandBuffer, const EngineContext&, const FrameContext&) { return false; }
    virtual void record_graphics(VkCommandBuffer cmd, const EngineContext& eng, const FrameContext& frm) = 0;
    virtual void compose(VkCommandBuffer, const EngineContext&, const FrameContext&) {}
    virtual void on_event(const SDL_Event&, const EngineContext&, const FrameContext*) {}
    virtual void on_imgui(const EngineContext&, const FrameContext&) {}
    virtual void reload_assets(const EngineContext&) {}
    virtual void request_screenshot(const char*) {}
    [[nodiscard]] virtual RendererStats get_stats() const { return {}; }
    virtual void set_option_int(const char*, int) {}
    virtual void set_option_float(const char*, float) {}
    virtual void set_option_str(const char*, const char*) {}
    virtual bool get_option_int(const char*, int&) const { return false; }
    virtual bool get_option_float(const char*, float&) const { return false; }
    virtual bool get_option_str(const char*, const char*&) const { return false; }
};

class VulkanEngine {
public:
    VulkanEngine();
    ~VulkanEngine();
    VulkanEngine(const VulkanEngine&) = delete;
    VulkanEngine& operator=(const VulkanEngine&) = delete;
    VulkanEngine(VulkanEngine&&) noexcept = default;
    VulkanEngine& operator=(VulkanEngine&&) noexcept = default;
    void init();
    void run();
    void cleanup();
    void set_renderer(std::unique_ptr<IRenderer> r);
    void configure_window(uint32_t w, uint32_t h, std::string_view title);
    [[nodiscard]] uint32_t width() const { return state_.width; }
    [[nodiscard]] uint32_t height() const { return state_.height; }
#ifdef VV_ENABLE_HOTRELOAD
    void add_hot_reload_watch_path(const std::string& path);
#endif
#ifdef VV_ENABLE_LOGGING
    void log_line(const std::string& s);
#endif

private:
    void sanitize_renderer_caps(RendererCaps& caps) const;
    void create_context(int window_width, int window_height, const char* app_name);
    void destroy_context();
    [[nodiscard]] EngineContext make_engine_context() const;
    FrameContext make_frame_context(uint64_t frame_index, uint32_t image_index, VkExtent2D extent);
    void blit_offscreen_to_swapchain(VkCommandBuffer cmd, uint32_t imageIndex, VkExtent2D extent);

    struct DeviceContext {
        VkInstance instance{};
        VkDebugUtilsMessengerEXT debug_messenger{};
        SDL_Window* window{nullptr};
        VkSurfaceKHR surface{};
        VkPhysicalDevice physical{};
        VkDevice device{};
        VkQueue graphics_queue{};
        VkQueue compute_queue{};
        VkQueue transfer_queue{};
        VkQueue present_queue{};
        uint32_t graphics_queue_family{};
        uint32_t compute_queue_family{};
        uint32_t transfer_queue_family{};
        uint32_t present_queue_family{};
        VmaAllocator allocator{};
        DescriptorAllocator descriptor_allocator;
    } ctx_{};

    void create_swapchain(uint32_t width, uint32_t height);
    void destroy_swapchain();
    void recreate_swapchain();
    void create_renderer_targets(VkExtent2D extent);
    void destroy_renderer_targets();

    struct AllocatedImage { VkImage image{}; VkImageView imageView{}; VmaAllocation allocation{}; VkExtent3D imageExtent{}; VkFormat imageFormat{}; };
    struct AttachmentResource { std::string name; VkImageUsageFlags usage{}; VkImageAspectFlags aspect{}; VkSampleCountFlagBits samples{VK_SAMPLE_COUNT_1_BIT}; VkImageLayout initial_layout{VK_IMAGE_LAYOUT_GENERAL}; AllocatedImage image; };
    struct SwapchainSystem { VkSwapchainKHR swapchain{}; VkFormat swapchain_image_format{}; VkExtent2D swapchain_extent{}; std::vector<VkImage> swapchain_images; std::vector<VkImageView> swapchain_image_views; std::vector<AttachmentResource> color_attachments; std::optional<AttachmentResource> depth_attachment; } swapchain_{};

    std::vector<AttachmentView> frame_attachment_views_;
    AttachmentView depth_attachment_view_{};
    int presentation_attachment_index_{0};

    void create_command_buffers();
    void destroy_command_buffers();
    void begin_frame(uint32_t& imageIndex, VkCommandBuffer& cmd);
    void end_frame(uint32_t imageIndex, VkCommandBuffer cmd);

    struct FrameData {
        VkCommandPool commandPool{};
        VkCommandBuffer mainCommandBuffer{};
        VkSemaphore imageAcquired{};
        VkSemaphore renderComplete{};
        uint64_t submitted_timeline_value{0};
        std::vector<std::function<void()>> dq;
        VkCommandBuffer asyncComputeCommandBuffer{};
        VkSemaphore asyncComputeFinished{};
        bool asyncComputeSubmitted{false};
        VkCommandPool computeCommandPool{};
    } frames_[FRAME_OVERLAP]{};

    VkSemaphore render_timeline_{};
    uint64_t timeline_value_{0};

    void create_renderer();
    void destroy_renderer();
    std::unique_ptr<IRenderer> renderer_;
    RendererCaps renderer_caps_{};

    struct UiSystem; // implemented privately; exposed via vv_ui::TabsHost through EngineContext.services
    void create_imgui();
    void destroy_imgui();
    std::unique_ptr<UiSystem> ui_;
    VkFormat imgui_format_{VK_FORMAT_UNDEFINED};
#ifdef VV_ENABLE_LOGGING
    std::vector<std::string> log_lines_;
#endif
    std::vector<std::function<void()>> mdq_;

#ifdef VV_ENABLE_GPU_TIMESTAMPS
    VkQueryPool ts_query_pool_{};
    double ts_period_ns_{1.0};
    double last_gpu_ms_{0.0};
    void create_timestamp_pool();
    void destroy_timestamp_pool();
#endif

#ifdef VV_ENABLE_SCREENSHOT
    struct PendingScreenshot { bool request{false}; std::string path; } screenshot_{};
    void queue_swapchain_screenshot(VkCommandBuffer cmd, uint32_t imageIndex);
#endif

#ifdef VV_ENABLE_HOTRELOAD
    struct WatchItem { std::string path; uint64_t stamp{}; };
    std::vector<WatchItem> watch_list_;
    double watch_accum_{0.0};
    void poll_file_watches(const EngineContext& eng);
#endif

#ifdef VV_ENABLE_TONEMAP
    bool use_srgb_swapchain_{false};
    bool tonemap_enabled_{false};
#endif

    struct EngineState { uint32_t width{1280}; uint32_t height{720}; std::string name{"Vulkan Visualizer"}; bool running{false}; bool initialized{false}; bool should_rendering{false}; bool resize_requested{false}; bool minimized{false}; bool focused{true}; uint64_t frame_number{0}; double time_sec{0.0}; double dt_sec{0.0}; } state_;
};

#endif // VULKAN_VISUALIZER_VK_ENGINE_H
