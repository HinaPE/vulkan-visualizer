// ============================================================================
// vk_abi.h  -  Public C ABI for the vulkan_visualizer engine
//
// Goal:
//   Provide a stable, minimal, C-friendly interface (opaque handles, plain
//   structs, function pointers) so that other languages (C, Rust, Python via FFI,
//   etc.) can embed and drive the engine without exposing C++ symbols.
//
// Design Principles:
//   * No STL types or templates in the ABI surface.
//   * Every public struct starts with a uint32_t struct_size field for forward
//     compatibility (callers must set it to sizeof(struct)).
//   * Opaque handles for engine objects (no exposing internal layout).
//   * All functions return a VKVizResult error code (0 = success) unless noted.
//   * Reserved fields / arrays ensure binary layout stability for future growth.
//   * Callers must zero-initialize all structs and fill required fields.
//   * Threading: unless specified, all engine calls should occur on the same
//     thread that created / runs the engine main loop.
//
// Versioning:
//   * Compile-time macros: VKVIZ_VERSION_{MAJOR,MINOR,PATCH}
//   * Runtime query: VKViz_GetVersion()
//   * Binary compatibility: Clients should verify major version matches.
//
// NOTE: This header only declares the ABI. An accompanying implementation
// (e.g. vk_abi.cpp) must adapt callbacks to the internal C++ VulkanEngine and
// IRenderer abstractions.
// ============================================================================
#ifndef VK_VIZ_ABI_H
#define VK_VIZ_ABI_H

#ifdef __cplusplus
extern "C" {
#endif

#include <stdint.h>
#include <stddef.h>
#include <vulkan/vulkan.h>

// Forward declare SDL window pointer without including SDL headers to keep ABI lean.
struct SDL_Window;

// ------------------------------------------------------------
// Export Macro (Shared Library)
// Define VKVIZ_BUILD_DLL when building the library itself.
// ------------------------------------------------------------
#if defined(_WIN32) || defined(__CYGWIN__)
  #if defined(VKVIZ_BUILD_DLL)
    #define VKVIZ_API __declspec(dllexport)
  #else
    #define VKVIZ_API __declspec(dllimport)
  #endif
#else
  #if defined(__GNUC__) && __GNUC__ >= 4
    #define VKVIZ_API __attribute__((visibility("default")))
  #else
    #define VKVIZ_API
  #endif
#endif

// ------------------------------------------------------------
// Version Macros & Struct
// ------------------------------------------------------------
#define VKVIZ_VERSION_MAJOR 0
#define VKVIZ_VERSION_MINOR 1
#define VKVIZ_VERSION_PATCH 0
#define VKVIZ_MAKE_VERSION(MAJ,MIN,PAT) ( ((uint32_t)(MAJ) << 22) | ((uint32_t)(MIN) << 12) | (uint32_t)(PAT) )
#define VKVIZ_VERSION VKVIZ_MAKE_VERSION(VKVIZ_VERSION_MAJOR, VKVIZ_VERSION_MINOR, VKVIZ_VERSION_PATCH)

typedef struct VKVizVersion {
    uint32_t major;
    uint32_t minor;
    uint32_t patch;
    uint32_t combined; /* VKVIZ_MAKE_VERSION encoding */
} VKVizVersion;

VKVIZ_API VKVizVersion VKViz_GetVersion(void);

// ------------------------------------------------------------
// Result / Error Codes
// ------------------------------------------------------------
typedef enum VKVizResult {
    VKVIZ_SUCCESS = 0,
    VKVIZ_ERROR_UNKNOWN              = -1,
    VKVIZ_ERROR_INVALID_ARGUMENT     = -2,
    VKVIZ_ERROR_UNSUPPORTED_VERSION  = -3,
    VKVIZ_ERROR_ALREADY_INITIALIZED  = -4,
    VKVIZ_ERROR_NOT_INITIALIZED      = -5,
    VKVIZ_ERROR_OUT_OF_MEMORY        = -6,
    VKVIZ_ERROR_VULKAN_INIT_FAILED   = -7,
    VKVIZ_ERROR_SDL_INIT_FAILED      = -8,
    VKVIZ_ERROR_SWAPCHAIN_FAILED     = -9,
    VKVIZ_ERROR_RENDERER_NOT_SET     = -10,
    VKVIZ_ERROR_IMGUI_INIT_FAILED    = -11,
    VKVIZ_ERROR_RUN_LOOP_ACTIVE      = -12,
    VKVIZ_ERROR_STRUCT_SIZE_MISMATCH = -13,
    VKVIZ_ERROR_DEVICE_LOST          = -14
} VKVizResult;

VKVIZ_API const char* VKViz_ResultToString(VKVizResult result);

// ------------------------------------------------------------
// Logging
// ------------------------------------------------------------
typedef enum VKVizLogLevel {
    VKVIZ_LOG_INFO  = 0,
    VKVIZ_LOG_WARN  = 1,
    VKVIZ_LOG_ERROR = 2,
    VKVIZ_LOG_DEBUG = 3
} VKVizLogLevel;

typedef void (*VKVizLogFn)(VKVizLogLevel level, const char* message, void* user_data);

// ------------------------------------------------------------
// Opaque Handles
// ------------------------------------------------------------
typedef struct VKVizEngine VKVizEngine;              /* Engine core */
typedef struct VKVizRendererAdapter VKVizRendererAdapter; /* Internal renderer bridge */

// ------------------------------------------------------------
// Engine Create Info
// ------------------------------------------------------------
typedef struct VKVizEngineCreateInfo {
    uint32_t struct_size;       /* Must be set to sizeof(VKVizEngineCreateInfo) */
    const char* app_name;       /* Optional (copied) */
    uint32_t window_width;      /* >=1 */
    uint32_t window_height;     /* >=1 */
    uint32_t enable_imgui;      /* 0/1 */
    uint32_t enable_validation; /* 0/1 (may be ignored on release builds) */
    uint32_t api_version;       /* Vulkan desired (0 => default) */
    VKVizLogFn log_fn;          /* Optional logging callback */
    void* log_user_data;        /* User data passed to log_fn */
    uint32_t flags;             /* Reserved (must be 0) */
    uint32_t reserved_u32[8];
    void*    reserved_ptr[4];
} VKVizEngineCreateInfo;

// ------------------------------------------------------------
// Engine Context Snapshot (lightweight, read-only)
// ------------------------------------------------------------
typedef struct VKVizEngineContext {
    uint32_t struct_size;    /* sizeof(VKVizEngineContext) */
    VkInstance instance;
    VkPhysicalDevice physical_device;
    VkDevice device;
    VkQueue graphics_queue;
    uint32_t graphics_queue_family;
    struct SDL_Window* window; /* Borrowed pointer */
    uint32_t reserved_u32[8];
    void*    reserved_ptr[8];
} VKVizEngineContext;

// ------------------------------------------------------------
// Frame Context Snapshot
// ------------------------------------------------------------
typedef struct VKVizFrameContext {
    uint32_t struct_size; /* sizeof(VKVizFrameContext) */
    uint64_t frame_index;
    uint32_t image_index;
    VkExtent2D extent;
    VkFormat swapchain_format;
    double time_sec;
    double dt_sec;
    VkImage swapchain_image;
    VkImageView swapchain_image_view;
    VkImage offscreen_image;
    VkImageView offscreen_image_view;
    VkImage depth_image;
    VkImageView depth_image_view;
    uint32_t reserved_u32[6];
    void*    reserved_ptr[8];
} VKVizFrameContext;

// ------------------------------------------------------------
// Renderer Callback Table
//   All function pointers optional (nullable) except record_graphics which
//   should be provided for visible rendering.
// ------------------------------------------------------------
typedef struct VKVizRendererCallbacks {
    uint32_t struct_size; /* sizeof(VKVizRendererCallbacks) */
    VKVizResult (*initialize)(const VKVizEngineContext* eng, void* user_data);
    void        (*destroy)(const VKVizEngineContext* eng, void* user_data);
    void        (*on_swapchain_ready)(const VKVizEngineContext* eng, const VKVizFrameContext* frame, void* user_data);
    void        (*on_swapchain_destroy)(const VKVizEngineContext* eng, void* user_data);
    void        (*update)(const VKVizEngineContext* eng, const VKVizFrameContext* frame, void* user_data);
    void        (*record_graphics)(VkCommandBuffer cmd, const VKVizEngineContext* eng, const VKVizFrameContext* frame, void* user_data);
    void        (*on_imgui)(const VKVizEngineContext* eng, const VKVizFrameContext* frame, void* user_data);
    void        (*on_event)(const void* sdl_event, const VKVizEngineContext* eng, const VKVizFrameContext* last_frame, void* user_data);
    void* reserved_fp[8];
} VKVizRendererCallbacks;

// ------------------------------------------------------------
// Stats
// ------------------------------------------------------------
typedef struct VKVizStats {
    uint32_t struct_size; /* sizeof(VKVizStats) */
    uint64_t draw_calls;
    uint64_t dispatches;
    uint64_t triangles;
    double cpu_ms;
    double gpu_ms;
    uint32_t reserved_u32[8];
    void*    reserved_ptr[4];
} VKVizStats;

// ------------------------------------------------------------
// Lifecycle API
// ------------------------------------------------------------
VKVIZ_API VKVizResult VKViz_CreateEngine(const VKVizEngineCreateInfo* ci, VKVizEngine** out_engine);
VKVIZ_API VKVizResult VKViz_DestroyEngine(VKVizEngine* engine);
VKVIZ_API VKVizResult VKViz_SetRenderer(VKVizEngine* engine, const VKVizRendererCallbacks* callbacks, void* user_data);
VKVIZ_API VKVizResult VKViz_Init(VKVizEngine* engine);
VKVIZ_API VKVizResult VKViz_Run(VKVizEngine* engine);          /* Blocking loop */
VKVIZ_API VKVizResult VKViz_RequestExit(VKVizEngine* engine);  /* Signal to exit */

// Manual / step mode (only if not inside VKViz_Run)
VKVIZ_API VKVizResult VKViz_PollEvents(VKVizEngine* engine);
VKVIZ_API VKVizResult VKViz_RenderFrame(VKVizEngine* engine);

// Snapshots
VKVIZ_API VKVizResult VKViz_GetEngineContext(VKVizEngine* engine, VKVizEngineContext* out_ctx);
VKVIZ_API VKVizResult VKViz_GetLastFrameContext(VKVizEngine* engine, VKVizFrameContext* out_frame);

// Stats
VKVIZ_API VKVizResult VKViz_GetStats(VKVizEngine* engine, VKVizStats* out_stats);

// ImGui runtime toggle (no-op if disabled at creation)
VKVIZ_API VKVizResult VKViz_SetImGuiEnabled(VKVizEngine* engine, int enabled);

// GPU sync (slow path)
VKVIZ_API VKVizResult VKViz_DeviceWaitIdle(VKVizEngine* engine);

// ------------------------------------------------------------
// Utility / Validation Helpers
// ------------------------------------------------------------
VKVIZ_API int VKViz_CheckStructSize(uint32_t provided, uint32_t expected); /* returns 1 if ok */

// ------------------------------------------------------------
// ABI Usage Notes:
//   1. Always set struct_size for every struct you pass in.
//   2. Zero-initialize structs (e.g. memset to 0) before filling fields.
//   3. You may pass NULL for optional callback pointers you do not implement.
//   4. record_graphics should be inexpensive and avoid heavy CPU stalls.
//   5. The engine owns the main loop timing; do not busy-wait inside callbacks.
// ------------------------------------------------------------

#ifdef __cplusplus
} // extern "C"
#endif

#endif // VK_VIZ_ABI_H

