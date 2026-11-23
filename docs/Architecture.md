# Architecture

## Overview

```
pireneus (static library, C)         pireneus_imgui (C++)          pireneus-examples (executable)
  src/platforms   window, input        src/imgui                     examples/launcher      menu
  src/renderer    Vulkan / DX12 RHI    Dear ImGui on the RHI         examples/*             examples
  src/assets      asset database                                     examples/scene-editor  editor
  src/scene       scenes, runtime
  src/physics     Box2D / Box3D
  src/audio       SDL_mixer
  src/meshedit    editable meshes
  ...
```

- **`pireneus`** is a static library holding the whole engine (`src/`). It isn't an executable.
- **`pireneus_imgui`** is a separate library (`src/imgui`). Only the tools that use Dear ImGui link
  it, so the engine itself stays in C.
- **`pireneus-examples`** is the only executable (`examples/`). It holds a Dear ImGui menu
  (`examples/launcher/main.cpp`), the examples and the editor.
  - Each example is an `Example` descriptor (`examples/example.h`): `init`, `update`, `render` and
    `shutdown` around the frame.
  - Each example gets a fresh ImGui context.

## Engine modules

| Module | Role |
| --- | --- |
| `src/platforms` | Platform abstraction. Window and event loop (`platforms_window.h`), keyboard by scancode (`platforms_input.h`), user folders (`platforms_paths.h`). SDL3 (`sdl/`) and UWP (`uwp/`) implementations. |
| `src/renderer` | The RHI (`renderer.h`) and its dispatch to the backends (`renderer.c`, `renderer_backend.h`). The `vulkan/` and `dx12/` backends, the bindless GPU layer (`gpu.h`), the sprite batch, the Slang shaders, the 2D renderer (`render2d/`) and the 3D renderer, Forward+ and Deferred+ (`render3d/`). |
| `src/bake` | Baked lighting: lightmaps, the probe volume, occlusion culling, on the CPU. |
| `src/math` | Header-only 3D math (`math3d.h`): `Vec3`, row-major `Mat4`, projections. |
| `src/json` | Hand-written JSON reader. |
| `src/tiled` | Tiled maps (loading, editing, saving) and culled drawing. |
| `src/model` | 3D model import (ufbx, cgltf), flattened into per-material submeshes. |
| `src/assets` | A project's asset database, caches, built-in assets, HDR and EXR images. |
| `src/project` | Projects: folder, `project.pireneus` settings, recent projects. |
| `src/scene` | Scenes (data, JSON) and playing them (`scene_runtime`). |
| `src/meshedit` | The level editor's editable polygon meshes, and their GPU version. |
| `src/physics` | Box3D and Box2D physics, character controller. |
| `src/audio` | Clips, 2D and 3D voices, buses. |
| `src/imgui` | Dear ImGui integration (`pireneus_imgui` library). |
| `src/debug.h` | `DEBUG_LOG()`, a `printf` that compiles away outside Debug builds. |

## Principles

- **Public headers expose no third-party library.** `renderer.h` and `platforms_window.h` use opaque
  types (`Texture`, `Window`...). Only the implementation `.c` files include SDL, Vulkan or D3D12, and
  they convert at the boundary.
- **Headers callable from C++ have `extern "C"` guards.**
- **Data modules don't depend on rendering.** `tiled_map.c`, `model.c` and `edit_mesh.c` only load or
  process data. Drawing lives elsewhere (`tiled_map_render.c`, `edit_mesh_gpu.c`).
- **Dependencies are kept minimal.**
  - They are fetched with `FetchContent` and built statically, often from their sources alone (see
    `third-parties/CMakeLists.txt`).
  - Small C libraries (ufbx, cgltf, miniz) are preferred over large frameworks.

## A frame

Order of calls in the launcher, for an example with a UI:

```c
handle_events();                 // platform events, also forwarded to ImGui
pireneus_imgui_new_frame();
example->update();               // logic, ImGui panels, synchronous offscreen passes
pireneus_imgui_prepare_frame();  // ImGui::Render() and ImGui's texture updates
if (begin_frame())
{
    example->render();           // drawing into the window
    pireneus_imgui_draw();       // the UI on top
    end_frame();
}
```

The main loop goes through `call_redraw_callback()`. It guards rendering against reentrancy, because
some DXGI calls pump the message queue during a resize.
