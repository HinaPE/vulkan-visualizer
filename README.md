# Vulkan Visualizer (v0.5.0)
[![Windows CI](https://github.com/HinaPE/vulkan-visualizer/actions/workflows/windows-build.yml/badge.svg?branch=master)](https://github.com/HinaPE/vulkan-visualizer/actions/workflows/windows-build.yml) 
[![Linux CI](https://github.com/HinaPE/vulkan-visualizer/actions/workflows/linux-build.yml/badge.svg?branch=master)](https://github.com/HinaPE/vulkan-visualizer/actions/workflows/linux-build.yml) 
[![macOS CI](https://github.com/HinaPE/vulkan-visualizer/actions/workflows/macos-build.yml/badge.svg?branch=master)](https://github.com/HinaPE/vulkan-visualizer/actions/workflows/macos-build.yml)

> A lightweight, modular Vulkan 1.3 rendering mini‑engine featuring dynamic rendering, multi‑attachment offscreen pipeline, ImGui HUD, and both high‑level C++ and stable C ABI frontends.

---
## TL;DR
Build a tiny Vulkan prototype fast: plug in a renderer (C++ class or C callback table), describe the attachments you need, record into a command buffer the engine prepares, and optionally let the engine blit an HDR target to the swapchain with an ImGui overlay.

---
## Highlights (Current State)
| Area | Details |
|------|---------|
| Vulkan Core | 1.4.321 (dynamic rendering, synchronization2) via VkBootstrap |
| Windowing | SDL3 |
| Memory | Vulkan Memory Allocator (VMA) |
| Descriptors | Single pooled allocator with ratio config |
| Frames In Flight | 2 (FRAME_OVERLAP) |
| Sync | Timeline semaphore + per-frame binary semaphores |
| Rendering | Fully dynamic (no render pass objects) |
| Offscreen Path | Configurable color attachment list (default HDR R16G16B16A16) + optional depth |
| Presentation Modes | EngineBlit / RendererComposite / DirectToSwapchain |
| ImGui | Docking + multi-viewport backend (optional via caps) |
| Capability Negotiation | Renderer supplies feature + attachment + usage requirements |
| Extensibility | Async compute (experimental flag), custom attachments, stats callbacks |
| Library Type | Static (default) |
| Example | Triangle (basic_window) via C ABI |

---
## What's New in 0.3.0
- Switched default build to a static library (simpler distribution).
- Extended `RendererCaps` for feature negotiation (ray tracing, mesh shader, etc. flags — placeholders for future enablement).
- Attachment negotiation via `RendererCaps.color_attachments` / `depth_attachment` with per‑attachment usage bits.
- Presentation pipeline modes (`PresentationMode`) enabling custom composition or direct swapchain rendering.
- Expanded `IRenderer` interface: compute paths (`record_compute`, `record_async_compute`), composition (`compose`), simulation/update split, events, option channel, asset reload, screenshot request stub, stats accessor.
- Improved swapchain/depth/attachment lifecycle hooks (`on_swapchain_ready` / `on_swapchain_destroy`).

---
## Repository Layout
```
include/
  vk_engine.h          # Public C++ API (engine + renderer interfaces)
  vk_abi.h             # Stable C ABI surface (opaque handle + callbacks)
src/
  vk_engine.cpp        # Engine implementation
examples/
  CMakeLists.txt
  basic_window.cpp     # Triangle example (C ABI)
  shaders/             # GLSL sources (compiled if glslc found)
cmake/                 # Dependency setup helpers
CMakeLists.txt         # Root build script
LICENSE
README.md
```

---
## Architecture Overview
Core responsibilities:
1. VulkanEngine: Instance/device creation, queue discovery, swapchain management, descriptor pool, VMA allocator, attachment creation, frame loop, ImGui lifecycle, synchronization.
2. IRenderer (C++) or callback table (C ABI): Supplies capabilities, initializes resources, records commands (graphics/compute), handles events & UI, provides stats.
3. Frame Loop Steps:
   - Poll SDL events (dispatch to renderer `on_event`).
   - Resize check & potential swapchain re-create.
   - Acquire swapchain image + begin primary command buffer.
   - (Optional) Async compute recording.
   - Renderer simulation/update (`simulate` then `update`).
   - Renderer graphics recording (`record_graphics`).
   - Optional composition (`compose`) if using RendererComposite mode.
   - Engine blit offscreen → swapchain (EngineBlit mode only).
   - ImGui pass (if enabled in caps).
   - Submit + present; advance timeline semaphore.

---
## Renderer Capability Negotiation
`RendererCaps` is populated/adjusted via (in order if implemented):
1. `query_required_device_caps(RendererCaps&)` (static needs before device creation — request features/extensions).
2. Device creation occurs considering caps.
3. `get_capabilities(const EngineContext&, RendererCaps&)` (device-aware refinement: choose formats, attachments, presentation mode, toggle ImGui, etc.).
4. Engine sanitizes, allocates attachments & swapchain.
5. `initialize(eng, caps, initial_frame)` invoked.

Important fields:
- `color_attachments`: vector of `AttachmentRequest` (name, format, usage, samples, initial_layout).
- `depth_attachment`: optional; if set, depth image created with requested format.
- `presentation_attachment`: name of color attachment used as source for presentation (EngineBlit) or composition.
- `presentation_mode`: EngineBlit (engine blits), RendererComposite (renderer composites to swapchain), DirectToSwapchain (renderer writes directly — no offscreen required).
- `enable_imgui`: toggle HUD.
- Feature flags (e.g. `need_ray_tracing_pipeline`) are placeholders; engine will later propagate proper extension chains.

Example minimal customization:
```cpp
void MyRenderer::query_required_device_caps(RendererCaps& caps) {
    caps.color_attachments = {
        AttachmentRequest{ .name = "hdr_color", .format = VK_FORMAT_R16G16B16A16_SFLOAT },
        AttachmentRequest{ .name = "albedo",    .format = VK_FORMAT_R8G8B8A8_UNORM }
    }; 
    caps.depth_attachment = AttachmentRequest{ .name = "depth", .format = VK_FORMAT_D32_SFLOAT, .aspect = VK_IMAGE_ASPECT_DEPTH_BIT };
    caps.presentation_attachment = "hdr_color"; // engine blits this one
}
```

---
## Presentation Modes
| Mode | Flow |
|------|------|
| EngineBlit | Renderer writes offscreen; engine blits chosen attachment to swapchain + overlays ImGui |
| RendererComposite | Renderer performs its own final composite into swapchain; engine skips blit |
| DirectToSwapchain | Renderer records directly into swapchain image (no intermediate HDR) |

---
## C++ API Quick Start
```cpp
#include <vk_engine.h>
class MyRenderer : public IRenderer {
public:
    void query_required_device_caps(RendererCaps& caps) override {
        caps.enable_imgui = true;
        // leave defaults (single HDR color attachment)
    }
    void initialize(const EngineContext& eng, const RendererCaps& caps, const FrameContext& frm) override {
        (void)eng; (void)caps; (void)frm; // create pipelines / descriptors
    }
    void destroy(const EngineContext& eng, const RendererCaps& caps) override {
        (void)eng; (void)caps; // cleanup
    }
    void record_graphics(VkCommandBuffer cmd, const EngineContext& eng, const FrameContext& frm) override {
        (void)eng; // Record against frm.color_attachments[0].image / frm.offscreen_image
        // Perform layout transitions & draws as needed.
    }
};
int main(){
    VulkanEngine engine;
    engine.configure_window(1280, 720, "Visualizer Example");
    engine.set_renderer(std::make_unique<MyRenderer>());
    engine.init();
    engine.run();
    engine.cleanup();
}
```
Key frame data:
- `frm.frame_index`, `frm.image_index`.
- `frm.extent` (current swapchain / attachment dimensions).
- `frm.color_attachments` (vector of negotiated attachments).
- `frm.offscreen_image` (legacy primary color when EngineBlit; mirrors first color attachment for convenience).
- `frm.presentation_mode` (active presentation strategy).

### Events & ImGui
Override `on_event(const SDL_Event&, ...)` to intercept input. Override `on_imgui(...)` to inject custom dockable panels (ImGui context already begun).

### Compute & Async
- `record_compute` executes on primary queue before graphics.
- `record_async_compute` (if returns true) records on a separate command buffer & may signal a semaphore; set `allow_async_compute` in caps to request resources.

### Composition
If using `RendererComposite`, emit your final color output directly into the swapchain image provided in `FrameContext` (perform layout transitions).

---
## C ABI Usage
```c
#include <vk_abi.h>
static VKVizResult init(const VKVizEngineContext* eng, const VKVizRendererCaps* caps, const VKVizFrameContext* frm, void* ud){ (void)eng;(void)caps;(void)frm;(void)ud; return VKVIZ_SUCCESS; }
static void destroy(const VKVizEngineContext* eng, const VKVizRendererCaps* caps, void* ud){ (void)eng;(void)caps;(void)ud; }
static void record(VkCommandBuffer cmd, const VKVizEngineContext* eng, const VKVizFrameContext* frm, void* ud){ (void)eng; (void)ud; /* draw using frm->offscreen_image */ }
int main(){
  VKVizEngineCreateInfo ci = {0}; ci.struct_size = sizeof(ci); ci.window_width=1280; ci.window_height=720; ci.enable_imgui=1;
  VKVizEngine* engine = NULL; VKViz_CreateEngine(&ci, &engine);
  VKVizRendererCallbacks cb = {0}; cb.struct_size = sizeof(cb); cb.initialize = init; cb.destroy = destroy; cb.record_graphics = record;
  VKViz_SetRenderer(engine, &cb, NULL);
  VKViz_Init(engine);
  VKViz_Run(engine);
  VKViz_DestroyEngine(engine);
}
```
Always set `struct_size` for every struct passed into the ABI. Functions return `VKVizResult` codes (see `vk_abi.h`).

---
## Building
### Requirements
| Component | Minimum |
|-----------|---------|
| CMake | 3.26 |
| Compiler | MSVC 19.36+ / Clang 16+ / GCC 12+ |
| Vulkan SDK | 1.3+ |

SDL3, ImGui, VkBootstrap, and VMA are fetched / configured by CMake helper scripts.

### Configure & Build
```bash
cmake -S . -B build -DCMAKE_BUILD_TYPE=Release -DVULKAN_VISUALIZER_BUILD_EXAMPLE=ON
cmake --build build --config Release
```

### Run Example
```bash
# Linux/macOS
./build/examples/basic_window
# Windows
build\\examples\\basic_window.exe
```

If `glslc` is on PATH, GLSL shaders in `examples/shaders` are compiled to SPIR-V (`*.spv`). Otherwise, provide precompiled binaries manually.

---
## Static vs Shared Library
The current default is a static library (`add_library(vulkan_visualizer STATIC ...)`). To experiment with a shared build, adjust the root `CMakeLists.txt` (a future option flag may automate this).

---
## Custom Attachments & Presentation Example
```cpp
void MyRenderer::query_required_device_caps(RendererCaps& caps) {
    caps.presentation_mode = PresentationMode::EngineBlit;
    caps.color_attachments = {
        { .name = "hdr_color", .format = VK_FORMAT_R16G16B16A16_SFLOAT },
        { .name = "normal",   .format = VK_FORMAT_A2B10G10R10_UNORM_PACK32, .usage = VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT | VK_IMAGE_USAGE_STORAGE_BIT }
    };
    caps.depth_attachment = AttachmentRequest{ .name = "depth", .format = VK_FORMAT_D32_SFLOAT, .aspect = VK_IMAGE_ASPECT_DEPTH_BIT };
}
```
Access inside frame:
```cpp
for (auto& att : frm.color_attachments) {
    if (att.name == "normal") { /* use att.image / att.view */ }
}
```

---
## Stats & Options
- Implement `get_stats()` to surface runtime metrics (draw calls, CPU/GPU ms). ImGui HUD will query periodically.
- `set_option_*` / `get_option_*` provide a lightweight, string‑keyed channel for UI controls without hard API changes.

---
## Screenshot / Asset Reload (Stubs)
- `request_screenshot(const char* path)` and `reload_assets` are forward‑looking hooks; provide internal wiring as needed.

---
## Roadmap
- Install & export targets (CMake package config).
- Toggle validation layers & debug messenger exposure.
- Configurable frames in flight.
- Render graph / pass scheduling layer.
- Async screenshot implementation.
- Headless offscreen mode (no SDL window).
- Additional samples: compute, textured quad, composition path, async compute.
- Enhanced statistics & descriptor/memory tracking panels.
- Feature flag activation for ray tracing / mesh shading when available.

---
## Contributing
1. Fork & branch (`feat/my-feature`).
2. Keep commits focused; write clear messages.
3. Test Debug & Release across at least one Windows and one Unix compiler if possible.
4. Open PR with concise problem + solution summary.
5. Style: Modern C++23; keep public headers minimal & stable.

Issues & feature requests: open a GitHub issue (include environment + repro).

---
## Troubleshooting
| Symptom | Cause | Fix |
|---------|-------|-----|
| Size mismatch error | Missing `struct_size` init | Zero struct then set `.struct_size = sizeof(struct)` |
| Blank window | Renderer never writes to presentation attachment | Ensure proper layout transitions + draw calls |
| ImGui absent | `enable_imgui=false` in caps | Set to true in `query_required_device_caps` |
| Stale swapchain | Resize events ignored | Engine auto-flags; ensure not blocking event loop |
| Missing Vulkan symbols | Linkage scope mismatch (should be PUBLIC) | (Root CMake already sets `Vulkan::Vulkan` PUBLIC) |

Logging: Future callback hook planned; currently unexpected exceptions map to generic failures.

Performance tips:
- Keep per-frame allocations minimal (reuse descriptor sets & buffers).
- Batch Vulkan state changes; prefer dynamic rendering subpass-like grouping.
- Avoid heavy CPU stalls in `record_graphics` / `record_compute`.

---
## License
See [LICENSE](./LICENSE). (SPDX identifier recommended if redistributing.)

---
Happy rendering — contributions & feedback welcome!
