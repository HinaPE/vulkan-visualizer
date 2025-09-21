# Vulkan Visualizer (v0.7.0)

[![Windows CI](https://github.com/HinaPE/vulkan-visualizer/actions/workflows/windows-build.yml/badge.svg?branch=master)](https://github.com/HinaPE/vulkan-visualizer/actions/workflows/windows-build.yml)
[![Linux CI](https://github.com/HinaPE/vulkan-visualizer/actions/workflows/linux-build.yml/badge.svg?branch=master)](https://github.com/HinaPE/vulkan-visualizer/actions/workflows/linux-build.yml)
[![macOS CI](https://github.com/HinaPE/vulkan-visualizer/actions/workflows/macos-build.yml/badge.svg?branch=master)](https://github.com/HinaPE/vulkan-visualizer/actions/workflows/macos-build.yml)

Lightweight, modular Vulkan 1.3 mini‑engine with dynamic rendering, offscreen pipelines, ImGui HUD, and clean C++23 API.

---

## Table of Contents
- [Highlights](#highlights)
- [What’s New (v0.7.0)](#whats-new-v070)
- [Repository Layout](#repository-layout)
- [Build](#build)
- [Quick Start (C++ API)](#quick-start-c-api)
- [ImGui UI, Tabs & Overlays](#imgui-ui-tabs--overlays)
- [Mini Axis Gizmo](#mini-axis-gizmo)
- [Frame Context & Presentation Modes](#frame-context--presentation-modes)
- [Examples](#examples)
- [Architecture](#architecture)
- [Contributing](#contributing)
- [License](#license)

---

## Highlights
- Vulkan Core: 1.3+ via VkBootstrap (dynamic rendering, synchronization2)
- Windowing: SDL3
- Memory: Vulkan Memory Allocator (VMA)
- Descriptors: Single pooled allocator with ratio configuration
- Frames In Flight: 2 (config constant)
- Sync: Timeline semaphore + per‑frame binary semaphores
- Rendering: Fully dynamic (no render pass objects)
- Offscreen Path: Configurable color attachments (default HDR R16G16B16A16) + optional depth
- Presentation: EngineBlit / RendererComposite / DirectToSwapchain
- ImGui: Docking + multi‑viewport; Tabs host + per‑frame overlays (HUD)
- Utilities: Screenshot (PNG), GPU timestamps, basic hot‑reload hook
- Language: Modern C++23, STL‑style API & naming

---

## What’s New (v0.7.0)
- C++23 standard across the project; code style normalized and headers trimmed.
- ImGui overlay system (per‑frame overlays) added to the Tabs host to render persistent HUD on top of all windows.
- Mini Axis Gizmo (top‑right) following camera orientation, anchored to the ImGui main viewport.
- Minor cleanup and structure pass (headers, function order consistency, and redundant includes removal).

> Upgrading from 0.6.x: `CMakeLists.txt` now reports version 0.7.0; ImGui overlay callbacks are available via `vv_ui::TabsHost::add_overlay`.

---

## Repository Layout
```
include/
  vk_engine.h          # Engine API (context, renderer interface, UI TabsHost)
  vv_camera.h          # Camera service + math helpers
src/
  vk_engine.cpp        # Engine implementation (swapchain, attachments, frame loop, ImGui)
  vv_camera.cpp        # Camera implementation (orbit/fly, IO, mini gizmo)
examples/
  CMakeLists.txt
  ex09_3dviewport.cpp  # 3D viewport sample (camera + pipeline)
  ex10_coordinate.cpp  # Axis visualization sample (camera + axis rendering)
  shaders/             # Example GLSL (precompiled if glslc available)
cmake/
  setup_*.cmake        # Third‑party setup helpers (SDL3, ImGui, VkBootstrap, VMA, stb)
CMakeLists.txt         # Root build
LICENSE
README.md
```

---

## Build

### Requirements
| Component | Minimum |
|-----------|---------|
| CMake     | 3.26    |
| Compiler  | MSVC 19.36+ / Clang 16+ / GCC 12+ |
| Vulkan SDK| 1.3+    |

SDL3, ImGui, VkBootstrap, VMA, and stb are integrated by the helper CMake scripts.

### Configure & Build
```bash
cmake -S . -B build -DCMAKE_BUILD_TYPE=Release -DVULKAN_VISUALIZER_BUILD_EXAMPLE=ON
cmake --build build --config Release
```

### Run an Example
```bash
# Linux/macOS
./build/examples/ex09_3dviewport
# Windows
build\examples\ex09_3dviewport.exe
```

If `glslc` is available, `examples/shaders` will be compiled to SPIR‑V (`*.spv`).

---

## Quick Start (C++ API)
```cpp
#include <vk_engine.h>

class MyRenderer : public IRenderer {
public:
    void query_required_device_caps(RendererCaps& caps) override {
        caps.enable_imgui = true; // enable HUD
        // leave default HDR color attachment / EngineBlit
    }
    void initialize(const EngineContext& eng, const RendererCaps& caps, const FrameContext& frm) override {
        (void)eng; (void)caps; (void)frm; // create pipelines / descriptors
    }
    void destroy(const EngineContext& eng, const RendererCaps& caps) override {
        (void)eng; (void)caps; // cleanup
    }
    void record_graphics(VkCommandBuffer cmd, const EngineContext& eng, const FrameContext& frm) override {
        (void)eng; (void)cmd; (void)frm; // issue layout transitions and draws against frm.color_attachments
    }
};

int main() {
    VulkanEngine engine;
    engine.configure_window(1280, 720, "Visualizer Example");
    engine.set_renderer(std::make_unique<MyRenderer>());
    engine.init();
    engine.run();
    engine.cleanup();
}
```

---

## ImGui UI, Tabs & Overlays
If `RendererCaps::enable_imgui = true`, the engine creates a dockable ImGui window and exposes a minimal TabsHost through `EngineContext.services`:

```cpp
auto* tabs = static_cast<vv_ui::TabsHost*>(eng.services);
if (tabs) {
    tabs->add_tab("My Panel", []{ /* ImGui widgets */ });
    tabs->add_overlay([]{ /* Always-on-top HUD (foreground draw list) */ });
}
```

- `add_tab(name, fn)`: per‑frame ephemeral tab.
- `add_overlay(fn)`: per‑frame overlay; executed before `ImGui::Render()` and drawn on the main viewport foreground. Works nicely with multi‑viewport/docking.

---

## Mini Axis Gizmo
The camera service provides a ready‑to‑use axis gizmo anchored at the top‑right of the ImGui main viewport:

```cpp
// Each frame (e.g., inside on_imgui):
auto* tabs = static_cast<vv_ui::TabsHost*>(eng.services);
if (tabs) tabs->add_overlay([&]{ camera.imgui_draw_mini_axis_gizmo(/*margin*/12, /*size*/96, /*thickness*/2.0f); });
```

- Colors: +X (red), +Y (green), +Z (blue).
- Back‑facing axes are dimmed; front‑facing axes drawn on top.
- Uses the camera’s view matrix rotation only; independent of Vulkan canvas origin.

---

## Frame Context & Presentation Modes
`FrameContext` provides per‑frame information:
- Swapchain extent, image/view, time/dt, color/depth attachment views.
- Presentation mode:
  - EngineBlit: engine blits your chosen attachment to the swapchain; overlays ImGui.
  - RendererComposite: renderer composites directly into swapchain; engine skips blit.
  - DirectToSwapchain: renderer records directly into swapchain.

Choose resources in `RendererCaps` via `color_attachments`, `depth_attachment`, and `presentation_attachment`.

---

## Examples
- `ex09_3dviewport`: Minimal dynamic rendering pipeline + camera (orbit/fly), HUD tabs.
- `ex10_coordinate`: Axis visualization with camera and a HUD overlay.

Enable building examples with `-DVULKAN_VISUALIZER_BUILD_EXAMPLE=ON`.

---

## Architecture
1. VulkanEngine
   - Instance/device creation, queues, swapchain
   - Attachment allocation via VMA
   - Descriptor pool (ratio‑based)
   - Frame loop: events → update → record → present
   - Optional: GPU timestamps, screenshot, hot‑reload
   - ImGui lifecycle and rendering
2. IRenderer (user‑provided)
   - Capability negotiation (`query_required_device_caps`, `get_capabilities`)
   - Resource init/destroy; record graphics/compute; optional async compute
   - UI: `on_imgui` to register tabs/overlays
3. Frame flow
   - Poll SDL events → resize handling
   - Acquire swapchain image + begin command buffer
   - Optional async compute → update → record graphics
   - EngineBlit or custom composition
   - ImGui overlays → submit → present

---

## Contributing
- Keep changes focused and documented; prefer modern C++23 idioms.
- Test on at least one Windows and one Unix compiler when possible.
- Style: STL‑like naming, minimal headers in public interfaces, avoid macros unless necessary.
- Open PRs with a concise summary; include build steps and screenshots for UI changes.

---

## License
See [LICENSE](./LICENSE).
