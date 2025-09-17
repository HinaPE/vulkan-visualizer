# Vulkan Visualizer
[![Windows CI](https://github.com/HinaPE/vulkan-visualizer/actions/workflows/windows-build.yml/badge.svg?branch=master)](https://github.com/HinaPE/vulkan-visualizer/actions/workflows/windows-build.yml)
> A lightweight, modular Vulkan rendering engine with an ImGui HUD, dynamic rendering pipeline, offscreen HDR path, and both C++ and C (stable) ABI frontends.

## Overview
`vulkan_visualizer` is a compact Vulkan engine focused on:
- Fast iteration with a minimal yet extensible rendering loop.
- Dynamic Rendering (no static render pass objects) targeting Vulkan 1.3 features.
- ImGui docking HUD for real‑time introspection of device, frame, and memory state.
- An offscreen HDR render target (R16G16B16A16) followed by a blit to the swapchain.
- A clear separation between engine orchestration and user renderer logic (`IRenderer`).
- A stable C ABI (via `vk_abi.h`) for language bindings / embedding (Rust, Python FFI, etc.).

---
## Highlights
| Feature | Details |
|---------|---------|
| Vulkan Version | 1.3 (via VkBootstrap) |
| Swapchain | FIFO present mode, dynamic recreation |
| Dynamic Rendering | Yes (no fixed render pass objects) |
| Synchronization | Timeline semaphore + per-frame binaries |
| Memory | Vulkan Memory Allocator (VMA) |
| Descriptors | Simple pooled allocator (`DescriptorAllocator`) |
| ImGui | SDL3 + Vulkan backend, docking + multi-viewport |
| Offscreen Path | R16G16B16A16 color + optional D32 depth |
| C++ API | `VulkanEngine`, `IRenderer` interface |
| C ABI | Stable functions + opaque handle (`vk_abi.h`) |
| Example | Colored triangle (`basic_window`) in both APIs |

---
## Project Structure
```
include/
  vk_engine.h        # C++ engine & interfaces
  vk_abi.h           # Public C ABI surface
src/
  vk_engine.cpp      # Engine implementation
  vk_abi.cpp         # ABI adapter (C -> C++)
examples/
  CMakeLists.txt
  basic_window.cpp   # Triangle example using C ABI
  shaders/
    triangle.vert
    triangle.frag
cmake/
  setup_sdl3.cmake
  setup_imgui.cmake
  setup_vkbootstrap.cmake
  setup_vma.cmake
CMakeLists.txt       # Root build script
README.md
LICENSE
```

---
## Architecture
Top-level responsibilities:
- **VulkanEngine**: Owns instance, device, surface, queues, swapchain, command buffers, timeline semaphore, offscreen targets, and ImGui lifecycle.
- **IRenderer (C++)** / **Callback Table (C ABI)**: User logic entry points (initialize, update, record_graphics, on_swapchain events, on_imgui, etc.).
- **Frame Loop**:
  1. Poll SDL events.
  2. Recreate swapchain if resize flagged.
  3. Acquire swapchain image; begin primary command buffer.
  4. User graphics recording (`record_graphics`).
  5. Engine blits offscreen HDR → swapchain image.
  6. ImGui overlay render.
  7. Submit & present; advance frame counter.
- **Synchronization**: One timeline semaphore governs per-frame GPU completion; wait before reusing resources.

---
## Dependencies
Bundled / fetched via CMake (expected to download or be externally provided):
- **Vulkan SDK** (headers + loader; environment variable `VULKAN_SDK` helpful on Windows).
- **SDL3** (window + input + surface creation).
- **VkBootstrap** (instance / device selection & feature chaining).
- **Vulkan Memory Allocator (VMA)** (buffer & image allocation abstraction).
- **Dear ImGui** (debug UI).

---
## Build Requirements
| Component | Minimum Version (Suggested) |
|-----------|-----------------------------|
| CMake     | 3.26+                       |
| Compiler  | MSVC 19.36+ / Clang 16+ / GCC 12+ |
| Vulkan SDK| 1.3+ (example uses 1.3 dynamic rendering, synchronization2) |
| Python    | (optional, for external tooling, not required to build) |

> NOTE: Ensure your GPU & driver support Vulkan 1.3 features: dynamic rendering & synchronization2.

---
## Quick Start
### Configure & Build (Standard)
```bash
# Configure (Release example)
cmake -S . -B build -DCMAKE_BUILD_TYPE=Release -DVULKAN_VISUALIZER_BUILD_EXAMPLE=ON
# Build
cmake --build build --config Release
```

### Building the Example Triangle
The `basic_window` executable (C ABI) will be built if `VULKAN_VISUALIZER_BUILD_EXAMPLE=ON`.
Run it:
```bash
./build/examples/basic_window   # (Linux/macOS)
# or on Windows
build\examples\basic_window.exe
```

### Runtime DLL Copy on Windows
A post-build step copies transitive runtime DLLs (engine DLL, SDL3, etc.) into the `examples/` output directory using `$<TARGET_RUNTIME_DLLS:basic_window>`.
If you add new executables, replicate the pattern in `examples/CMakeLists.txt`.

---
## CMake Options
| Option | Default | Description |
|--------|---------|-------------|
| `VULKAN_VISUALIZER_BUILD_EXAMPLE` | `OFF` | Build example triangle executable |

(Planned) Additional options (not yet implemented):
- `VULKAN_VISUALIZER_BUILD_SHARED` (toggle shared vs static)
- `VULKAN_VISUALIZER_ENABLE_IMGUI`
- `VULKAN_VISUALIZER_ENABLE_VALIDATION`

---
## Using the C++ API
Minimal skeleton:
```cpp
#include <vk_engine.h>
class MyRenderer : public IRenderer {
  void initialize(const EngineContext& eng) override { /* create pipelines */ }
  void destroy(const EngineContext& eng) override { /* destroy pipelines */ }
  void record_graphics(VkCommandBuffer cmd, const EngineContext& eng, const FrameContext& frm) override {
      // Record Vulkan commands against frm.offscreen_image
  }
};
int main(){
  VulkanEngine engine;
  engine.set_renderer(std::make_unique<MyRenderer>());
  engine.init();
  engine.run();
  engine.cleanup();
}
```
Key frame data fields:
- `frm.extent` – current framebuffer dimensions.
- `frm.offscreen_image` – HDR color image to render into (GENERAL layout expected when you start; engine handles post-blit state transitions for the next frame).

---
## Using the C ABI (vk_abi.h)
The C ABI is a stable, struct-size validated interface for foreign language bindings.
```c
#include <vk_abi.h>
static VKVizResult init(const VKVizEngineContext* eng, void* ud) { return VKVIZ_SUCCESS; }
static void destroy(const VKVizEngineContext* eng, void* ud) {}
static void record(VkCommandBuffer cmd, const VKVizEngineContext* eng, const VKVizFrameContext* frm, void* ud) {
    // Vulkan recording using frm->offscreen_image
}
int main(){
  VKVizEngineCreateInfo ci = {0};
  ci.struct_size = sizeof(ci);
  ci.window_width = 1280; ci.window_height = 720; ci.enable_imgui = 1;
  VKVizEngine* engine = NULL; VKViz_CreateEngine(&ci, &engine);
  VKVizRendererCallbacks cb = {0};
  cb.struct_size = sizeof(cb);
  cb.initialize = init; cb.destroy = destroy; cb.record_graphics = record;
  VKViz_SetRenderer(engine, &cb, NULL);
  VKViz_Init(engine);
  VKViz_Run(engine);
  VKViz_DestroyEngine(engine);
}
```
**Struct Size Contracts**: Always set `struct_size = sizeof(StructType)` before calling into the ABI. The engine rejects mismatched sizes with `VKVIZ_ERROR_STRUCT_SIZE_MISMATCH`.

---
## Implementing a Custom Renderer
### C++ Renderer
Implement `IRenderer::record_graphics` and optionally override `update`, `on_imgui`, etc. Use `EngineContext` queue families or device handles for advanced resource creation.

### C ABI Renderer
Fill out a `VKVizRendererCallbacks` table. Only `record_graphics` is strictly required; others may be NULL.

Lifecycle order (C ABI):
1. `VKViz_CreateEngine`
2. `VKViz_SetRenderer`
3. `VKViz_Init` (triggers renderer `initialize`)
4. `VKViz_Run` (calls `update` + `record_graphics` per frame)
5. `VKViz_DestroyEngine` (calls renderer `destroy`)

---
## Shader Pipeline
Example shaders are GLSL 460 core. CMake attempts to compile them with `glslc` if found. Artifacts land in `examples/shaders/*.spv`.
If `glslc` is not present:
- Provide precompiled SPIR-V manually.
- Or set up the Vulkan SDK so that `glslc` is discoverable in `PATH`.

---
## Offscreen & Presentation Path
Rendering flow:
1. Renderer writes into `offscreen_image` (initially `VK_IMAGE_LAYOUT_GENERAL`).
2. Engine transitions it to `TRANSFER_SRC` then blits → swapchain image (now `TRANSFER_DST`).
3. ImGui overlay loads swapchain image, appends UI.
4. Swapchain image presented.
5. Offscreen image is expected back in `GENERAL` for next frame (renderer returns it so; provided example transitions back).

---
## ImGui Integration
- Dockspace + multi-viewport enabled by default.
- Panels added via `UiSystem::add_panel` (internal); renderer can expose UI by overriding `on_imgui`.
- HUD displays: FPS, frame index, swapchain format/extent, device properties, memory usage, renderer stats, synchronization timeline.

---
## Static vs Shared Library
Currently the root CMake builds a **shared** library (`vulkan_visualizer.dll` / `.so` / `.dylib`).
Reasons you may prefer static:
- Simpler deployment (no runtime dependency).
- Avoids export macro complexity on Windows.
To switch temporarily, edit:
```cmake
add_library(vulkan_visualizer STATIC ${${libname}_SOURCES})
```
(Planned: configurable with `VULKAN_VISUALIZER_BUILD_SHARED` option.)

### Export Macros
When building shared, `VKVIZ_BUILD_DLL` is defined for proper symbol export on Windows in the C ABI.

---
## Planned Install / Packaging
(Not yet implemented in current CMakeLists — suggestions)
```cmake
include(GNUInstallDirs)
install(TARGETS vulkan_visualizer
  RUNTIME DESTINATION ${CMAKE_INSTALL_BINDIR}
  LIBRARY DESTINATION ${CMAKE_INSTALL_LIBDIR}
  ARCHIVE DESTINATION ${CMAKE_INSTALL_LIBDIR}
)
install(FILES include/vk_engine.h include/vk_abi.h DESTINATION ${CMAKE_INSTALL_INCLUDEDIR})
```
Then downstream usage:
```cmake
find_package(VulkanVisualizer REQUIRED)
add_executable(app main.cpp)
target_link_libraries(app PRIVATE VulkanVisualizer::vulkan_visualizer)
```

---
## Roadmap
- [ ] Add install + export targets (CMake package config).
- [ ] Optional validation layer toggle (runtime & build option).
- [ ] Configurable number of frames-in-flight.
- [ ] Render graph / pass abstraction (optional layer on top of dynamic rendering).
- [ ] Async screenshot API (expose in C ABI).
- [ ] Headless mode (no SDL window, offscreen only).
- [ ] Additional examples (compute + graphics, textured quad, IMGUI docking custom panels).
- [ ] Memory / descriptor statistics panel.

---
## Contributing
1. Fork & branch (`feat/my-feature`).
2. Keep commits focused and messages clear.
3. Run builds for Debug & Release (Windows + at least one Unix toolchain if possible).
4. Submit PR with a short problem/solution summary.
5. Style: modern C++23, avoid unnecessary abstractions, keep public headers minimal.

Issues / Feature Requests: Use GitHub Issues template (describe environment + repro steps).

---
## Troubleshooting
| Symptom | Cause | Fix |
|---------|-------|-----|
| `VKVIZ_ERROR_RUN_LOOP_ACTIVE` | Called `VKViz_Run` while `running` already set | Call only once, or ensure previous loop ended |
| `ERROR_STRUCT_SIZE_MISMATCH` | Forgot to set `struct_size` | Initialize struct to zero then set size |
| Blank window / no triangle | Offscreen image never cleared/drawn | Ensure `record_graphics` transitions + draws + transitions back |
| ImGui not visible | Viewports or docking conflicts / minimized window | Restore window, check swapchain extent |
| Validation layer warnings (future) | Optional features missing | Enable layers or adjust feature requests |
| Missing Vulkan symbols when linking example | Library linked PRIVATE instead of PUBLIC | (Fixed: Vulkan::Vulkan now PUBLIC) |
| Windows post-build copy fails | Path quoting or source=dest copy | Simplified `copy_if_different` command |

### Logging
A user-provided logging callback (planned enhancement) can surface internal exceptions; currently most exceptions map to `VKVIZ_ERROR_UNKNOWN`.

### Performance Tips
- Avoid heavy CPU stalls inside `record_graphics`.
- Batch resource creation; reuse descriptor sets where possible.
- Use GPU vendor debug markers if later integrated.

---
## License
See [LICENSE](./LICENSE). (Provide SPDX identifier if appropriate — e.g. MIT, Apache-2.0.)

---
**Happy rendering!** Feel free to propose improvements or request additional bindings.

