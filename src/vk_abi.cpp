#include "vk_abi.h"
#include "vk_engine.h"

#ifndef VKVIZ_BUILD_DLL
#define VKVIZ_BUILD_DLL 1
#endif

#include <new>
#include <cstring>
#include <exception>
#include <mutex>
#include <atomic>

// Local helpers to fill ABI structs
static void VKViz_FillEngineContext(const EngineContext& in, VKVizEngineContext& out) {
    std::memset(&out, 0, sizeof(out));
    out.struct_size = sizeof(VKVizEngineContext);
    out.instance = in.instance;
    out.physical_device = in.physical;
    out.device = in.device;
    out.graphics_queue = in.graphics_queue;
    out.graphics_queue_family = in.graphics_queue_family;
    out.window = in.window;
}
static void VKViz_FillFrameContext(const FrameContext& in, VKVizFrameContext& out) {
    std::memset(&out, 0, sizeof(out));
    out.struct_size = sizeof(VKVizFrameContext);
    out.frame_index = in.frame_index;
    out.image_index = in.image_index;
    out.extent = in.extent;
    out.swapchain_format = in.swapchain_format;
    out.time_sec = in.time_sec;
    out.dt_sec = in.dt_sec;
    out.swapchain_image = in.swapchain_image;
    out.swapchain_image_view = in.swapchain_image_view;
    out.offscreen_image = in.offscreen_image;
    out.offscreen_image_view = in.offscreen_image_view;
    out.depth_image = in.depth_image;
    out.depth_image_view = in.depth_image_view;
}

// String for results
static const char* VKViz_ResultString(VKVizResult r) {
    switch (r) {
        case VKVIZ_SUCCESS: return "SUCCESS";
        case VKVIZ_ERROR_UNKNOWN: return "ERROR_UNKNOWN";
        case VKVIZ_ERROR_INVALID_ARGUMENT: return "ERROR_INVALID_ARGUMENT";
        case VKVIZ_ERROR_UNSUPPORTED_VERSION: return "ERROR_UNSUPPORTED_VERSION";
        case VKVIZ_ERROR_ALREADY_INITIALIZED: return "ERROR_ALREADY_INITIALIZED";
        case VKVIZ_ERROR_NOT_INITIALIZED: return "ERROR_NOT_INITIALIZED";
        case VKVIZ_ERROR_OUT_OF_MEMORY: return "ERROR_OUT_OF_MEMORY";
        case VKVIZ_ERROR_VULKAN_INIT_FAILED: return "ERROR_VULKAN_INIT_FAILED";
        case VKVIZ_ERROR_SDL_INIT_FAILED: return "ERROR_SDL_INIT_FAILED";
        case VKVIZ_ERROR_SWAPCHAIN_FAILED: return "ERROR_SWAPCHAIN_FAILED";
        case VKVIZ_ERROR_RENDERER_NOT_SET: return "ERROR_RENDERER_NOT_SET";
        case VKVIZ_ERROR_IMGUI_INIT_FAILED: return "ERROR_IMGUI_INIT_FAILED";
        case VKVIZ_ERROR_RUN_LOOP_ACTIVE: return "ERROR_RUN_LOOP_ACTIVE";
        case VKVIZ_ERROR_STRUCT_SIZE_MISMATCH: return "ERROR_STRUCT_SIZE_MISMATCH";
        case VKVIZ_ERROR_DEVICE_LOST: return "ERROR_DEVICE_LOST";
        default: return "ERROR_UNRECOGNIZED";
    }
}

extern "C" VKVIZ_API VKVizVersion VKViz_GetVersion(void) {
    return VKVizVersion{VKVIZ_VERSION_MAJOR, VKVIZ_VERSION_MINOR, VKVIZ_VERSION_PATCH, VKVIZ_VERSION};
}
extern "C" VKVIZ_API const char* VKViz_ResultToString(VKVizResult result) { return VKViz_ResultString(result); }
extern "C" VKVIZ_API int VKViz_CheckStructSize(uint32_t provided, uint32_t expected) { return provided == expected; }

struct VKVizEngine {
    VulkanEngine* core{nullptr};
    VKVizRendererCallbacks callbacks{};
    void* user_data{nullptr};
    VKVizLogFn log_fn{nullptr};
    void* log_ud{nullptr};
    std::atomic<bool> exit_requested{false};
};

// Adapter bridging C callbacks to C++ IRenderer
class CAbiRendererAdapter final : public IRenderer {
public:
    explicit CAbiRendererAdapter(VKVizEngine* wrap) : wrapper_(wrap) {}
    void initialize(const EngineContext& eng) override {
        if (!wrapper_->callbacks.initialize) return;
        VKVizEngineContext ce{}; VKViz_FillEngineContext(eng, ce);
        wrapper_->callbacks.initialize(&ce, wrapper_->user_data);
    }
    void destroy(const EngineContext& eng) override {
        if (!wrapper_->callbacks.destroy) return;
        VKVizEngineContext ce{}; VKViz_FillEngineContext(eng, ce);
        wrapper_->callbacks.destroy(&ce, wrapper_->user_data);
    }
    void on_swapchain_ready(const EngineContext& eng, const FrameContext& frm) override {
        if (!wrapper_->callbacks.on_swapchain_ready) return;
        VKVizEngineContext ce{}; VKViz_FillEngineContext(eng, ce);
        VKVizFrameContext fc{}; VKViz_FillFrameContext(frm, fc);
        wrapper_->callbacks.on_swapchain_ready(&ce, &fc, wrapper_->user_data);
    }
    void on_swapchain_destroy(const EngineContext& eng) override {
        if (!wrapper_->callbacks.on_swapchain_destroy) return;
        VKVizEngineContext ce{}; VKViz_FillEngineContext(eng, ce);
        wrapper_->callbacks.on_swapchain_destroy(&ce, wrapper_->user_data);
    }
    void update(const EngineContext& eng, const FrameContext& frm) override {
        if (!wrapper_->callbacks.update) return;
        VKVizEngineContext ce{}; VKViz_FillEngineContext(eng, ce);
        VKVizFrameContext fc{}; VKViz_FillFrameContext(frm, fc);
        wrapper_->callbacks.update(&ce, &fc, wrapper_->user_data);
    }
    void record_graphics(VkCommandBuffer cmd, const EngineContext& eng, const FrameContext& frm) override {
        if (!wrapper_->callbacks.record_graphics) return;
        VKVizEngineContext ce{}; VKViz_FillEngineContext(eng, ce);
        VKVizFrameContext fc{}; VKViz_FillFrameContext(frm, fc);
        wrapper_->callbacks.record_graphics(cmd, &ce, &fc, wrapper_->user_data);
    }
    void on_imgui(const EngineContext& eng, const FrameContext& frm) override {
        if (!wrapper_->callbacks.on_imgui) return;
        VKVizEngineContext ce{}; VKViz_FillEngineContext(eng, ce);
        VKVizFrameContext fc{}; VKViz_FillFrameContext(frm, fc);
        wrapper_->callbacks.on_imgui(&ce, &fc, wrapper_->user_data);
    }
    void on_event(const SDL_Event& e, const EngineContext& eng, const FrameContext* frm) override {
        if (!wrapper_->callbacks.on_event) return;
        VKVizEngineContext ce{}; VKViz_FillEngineContext(eng, ce);
        VKVizFrameContext fc{}; if (frm) VKViz_FillFrameContext(*frm, fc); else fc.struct_size = sizeof(VKVizFrameContext);
        wrapper_->callbacks.on_event(static_cast<const void*>(&e), &ce, frm ? &fc : nullptr, wrapper_->user_data);
    }
private:
    VKVizEngine* wrapper_{nullptr};
};

static VKVizResult translate_exception(const std::exception&) { return VKVIZ_ERROR_UNKNOWN; }

extern "C" VKVIZ_API VKVizResult VKViz_CreateEngine(const VKVizEngineCreateInfo* ci, VKVizEngine** out_engine) {
    if (!out_engine) return VKVIZ_ERROR_INVALID_ARGUMENT;
    *out_engine = nullptr;
    if (!ci || ci->struct_size != sizeof(VKVizEngineCreateInfo)) return VKVIZ_ERROR_STRUCT_SIZE_MISMATCH;
    try {
        VKVizEngine* wrap = new VKVizEngine();
        wrap->log_fn = ci->log_fn;
        wrap->log_ud = ci->log_user_data;
        wrap->core = new VulkanEngine();
        if (ci->app_name) wrap->core->state_.name = ci->app_name;
        if (ci->window_width) wrap->core->state_.width = static_cast<int>(ci->window_width);
        if (ci->window_height) wrap->core->state_.height = static_cast<int>(ci->window_height);
        *out_engine = wrap;
        return VKVIZ_SUCCESS;
    } catch (const std::bad_alloc&) {
        return VKVIZ_ERROR_OUT_OF_MEMORY;
    } catch (const std::exception& ex) {
        return translate_exception(ex);
    }
}

extern "C" VKVIZ_API VKVizResult VKViz_DestroyEngine(VKVizEngine* engine) {
    if (!engine) return VKVIZ_ERROR_INVALID_ARGUMENT;
    try {
        if (engine->core) { engine->core->cleanup(); delete engine->core; engine->core = nullptr; }
        delete engine;
        return VKVIZ_SUCCESS;
    } catch (const std::exception& ex) { return translate_exception(ex); }
}

extern "C" VKVIZ_API VKVizResult VKViz_SetRenderer(VKVizEngine* engine, const VKVizRendererCallbacks* callbacks, void* user_data) {
    if (!engine) return VKVIZ_ERROR_INVALID_ARGUMENT;
    if (engine->core->state_.initialized) return VKVIZ_ERROR_ALREADY_INITIALIZED;
    if (!callbacks || callbacks->struct_size != sizeof(VKVizRendererCallbacks)) return VKVIZ_ERROR_STRUCT_SIZE_MISMATCH;
    engine->callbacks = *callbacks;
    engine->user_data = user_data;
    try {
        engine->core->set_renderer(std::make_unique<CAbiRendererAdapter>(engine));
        return VKVIZ_SUCCESS;
    } catch (const std::exception& ex) { return translate_exception(ex); }
}

extern "C" VKVIZ_API VKVizResult VKViz_Init(VKVizEngine* engine) {
    if (!engine) return VKVIZ_ERROR_INVALID_ARGUMENT;
    if (engine->core->state_.initialized) return VKVIZ_ERROR_ALREADY_INITIALIZED;
    try { engine->core->init(); return VKVIZ_SUCCESS; } catch (const std::exception& ex) { return translate_exception(ex);} }

extern "C" VKVIZ_API VKVizResult VKViz_Run(VKVizEngine* engine) {
    if (!engine) return VKVIZ_ERROR_INVALID_ARGUMENT;
    if (!engine->core->state_.initialized) return VKVIZ_ERROR_NOT_INITIALIZED;
    if (engine->core->state_.running) return VKVIZ_ERROR_RUN_LOOP_ACTIVE;
    try { engine->core->state_.running = true; engine->core->run(); return VKVIZ_SUCCESS; } catch (const std::exception& ex) { return translate_exception(ex);} }

extern "C" VKVIZ_API VKVizResult VKViz_RequestExit(VKVizEngine* engine) {
    if (!engine) return VKVIZ_ERROR_INVALID_ARGUMENT;
    engine->exit_requested = true;
    if (engine->core) engine->core->state_.running = false;
    return VKVIZ_SUCCESS;
}

extern "C" VKVIZ_API VKVizResult VKViz_PollEvents(VKVizEngine*) { return VKVIZ_ERROR_UNSUPPORTED_VERSION; }
extern "C" VKVIZ_API VKVizResult VKViz_RenderFrame(VKVizEngine*) { return VKVIZ_ERROR_UNSUPPORTED_VERSION; }

extern "C" VKVIZ_API VKVizResult VKViz_GetEngineContext(VKVizEngine* engine, VKVizEngineContext* out_ctx) {
    if (!engine || !out_ctx) return VKVIZ_ERROR_INVALID_ARGUMENT;
    if (out_ctx->struct_size != sizeof(VKVizEngineContext)) return VKVIZ_ERROR_STRUCT_SIZE_MISMATCH;
    if (!engine->core->state_.initialized) return VKVIZ_ERROR_NOT_INITIALIZED;
    EngineContext ec = engine->core->export_engine_context();
    VKViz_FillEngineContext(ec, *out_ctx);
    return VKVIZ_SUCCESS;
}

extern "C" VKVIZ_API VKVizResult VKViz_GetLastFrameContext(VKVizEngine* engine, VKVizFrameContext* out_frame) {
    if (!engine || !out_frame) return VKVIZ_ERROR_INVALID_ARGUMENT;
    if (out_frame->struct_size != sizeof(VKVizFrameContext)) return VKVIZ_ERROR_STRUCT_SIZE_MISMATCH;
    if (!engine->core->state_.initialized) return VKVIZ_ERROR_NOT_INITIALIZED;
    FrameContext fc = engine->core->export_frame_context_current();
    VKViz_FillFrameContext(fc, *out_frame);
    return VKVIZ_SUCCESS;
}

extern "C" VKVIZ_API VKVizResult VKViz_GetStats(VKVizEngine* engine, VKVizStats* out_stats) {
    if (!engine || !out_stats) return VKVIZ_ERROR_INVALID_ARGUMENT;
    if (out_stats->struct_size != sizeof(VKVizStats)) return VKVIZ_ERROR_STRUCT_SIZE_MISMATCH;
    if (!engine->core->state_.initialized) return VKVIZ_ERROR_NOT_INITIALIZED;
    std::memset(out_stats, 0, sizeof(*out_stats));
    out_stats->struct_size = sizeof(VKVizStats);
    RendererStats rs = engine->core->export_renderer_stats();
    out_stats->draw_calls = rs.draw_calls;
    out_stats->dispatches = rs.dispatches;
    out_stats->triangles = rs.triangles;
    out_stats->cpu_ms = rs.cpu_ms;
    out_stats->gpu_ms = rs.gpu_ms;
    return VKVIZ_SUCCESS;
}

extern "C" VKVIZ_API VKVizResult VKViz_SetImGuiEnabled(VKVizEngine*, int) { return VKVIZ_SUCCESS; }

extern "C" VKVIZ_API VKVizResult VKViz_DeviceWaitIdle(VKVizEngine* engine) {
    if (!engine) return VKVIZ_ERROR_INVALID_ARGUMENT;
    if (!engine->core->state_.initialized) return VKVIZ_ERROR_NOT_INITIALIZED;
    try { EngineContext ec = engine->core->export_engine_context(); if (ec.device) vkDeviceWaitIdle(ec.device); return VKVIZ_SUCCESS; } catch (const std::exception& ex) { return translate_exception(ex);} }
