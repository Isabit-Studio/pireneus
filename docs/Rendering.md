# Rendering

## The RHI

`src/renderer/renderer.h` is the engine's rendering API, the same whatever the backend. It is a
deliberately minimal RHI:

- `renderer_backend.h` defines `RendererBackend`, a struct of function pointers, one per public
  function. Each backend fills it in and exposes it through a single entry point
  (`vulkan_get_backend()`, `dx12_get_backend()`).
- `renderer.c` picks the backend **at runtime**, on the first `create_renderer()`, then forwards every
  call to it. Selection order:
  1. the `PIRENEUS_RENDERER` environment variable (`vulkan` or `dx12`), with no dialog;
  2. otherwise, on Windows, a native dialog. Enter, Escape or a failure pick the compiled-in default
     (the `PIRENEUS_RENDERER` CMake option);
  3. elsewhere, the only backend compiled (Vulkan).
- On Windows, Vulkan and DirectX 12 are compiled into the same binary. UWP only has DX12.

Adding a backend (Metal, DirectX 11, OpenGL...) means filling in a new `RendererBackend`. Neither
`renderer.c` nor the examples need to change.

### Vulkan (`src/renderer/vulkan`)

- **No SDK required.** The headers come from `Vulkan-Headers`, and functions are loaded by **volk**,
  bootstrapped with SDL's `vkGetInstanceProcAddr`.
- **Memory** is managed by **Vulkan Memory Allocator**, compiled in `vulkan_memory_allocator.cpp`.
- **Swapchain**: `*_UNORM`, like DX12. Textures and colors pass through as they are, without being
  sRGB-encoded a second time.
- **Clip space**: the viewport has a negative height (`VK_KHR_maintenance1`), so clip space has the
  same convention (+Y up) as on D3D12.

### DirectX 12 (`src/renderer/dx12`)

- **No third-party dependency.** It only uses the Windows SDK (`d3d12`, `dxgi`, `dxguid`), called
  from C with `COBJMACROS`.
- **DirectX 12 Ultimate**: support (`D3D_FEATURE_LEVEL_12_2`) is checked and logged at startup.
- **Synchronization**: a single fence with strictly increasing values. A value the fence has already
  reached is never signaled again.
- **Descriptors**: texture descriptors live in a shader-visible heap with a free list, because render
  targets may be recreated on every resize.

## 2D

The base primitive is `draw_vertices(vertices, count, texture)`: a triangle list in window pixels,
with UVs and colors, between `begin_frame()` and `end_frame()`. All of the 2D is built on it:

- **`SpriteBatch`** (`sprite_batch.h`), inspired by XNA, MonoGame and FNA: `sprite_batch_begin`,
  `sprite_batch_draw`, `sprite_batch_draw_region`, `sprite_batch_end`. Quads are accumulated and
  flushed when the texture changes.
- **Tiled maps**: `tiled_map_draw_layer()` and `tiled_map_draw()` (`src/tiled`) cull against the
  camera with a one-tile margin, then call back for each visible tile.
- **Dear ImGui** is drawn through the same primitive (see [Dear ImGui](Dear-ImGui)).
- `set_scissor()` restricts the following draws to a rectangle.

Textures are sampled with *nearest* filtering and *clamp* addressing, which suits pixel art. The pixel
to clip space conversion (`ortho.h`) is a plain scale and offset, identical on both backends.

### Render targets

`create_render_target()`, then `begin_render_target_pass()` and `end_render_target_pass()`, draw with
`draw_vertices()` into an offscreen texture.

A pass is a **synchronous** unit of GPU work: it is submitted and waited on. Its texture can be
sampled as soon as the pass ends.

The *Pixel Perfect* example uses one to render the scene 216 pixels tall, then scales it up by a whole
factor.

## Forward 3D

A simple renderer: one directional light, Lambert diffuse plus a hemispheric ambient term, and shadows
from a shadow map (3×3 PCF).

```c
begin_shadow_pass(shadow_map, light_view_projection);  // outside begin_frame()/end_frame()
draw_mesh(mesh, model);
end_shadow_pass();

begin_frame();
begin_scene(&lighting, shadow_map);
draw_mesh(mesh, model);
draw_skybox(panorama, intensity);   // after the meshes: only fills the background
end_scene();                        // back to 2D
end_frame();
```

- `Mesh`: `create_mesh(MeshVertex*, n, uint32_t* indices, n)`. A vertex is 64 bytes (position,
  normal, color, UV, tangent).
- `draw_mesh_colored` replaces the vertex colors with one color.
- `draw_mesh_range` draws a range of indices: one part per material, for editable meshes.
- Matrices are **row-major**, in a right-handed frame (+Y up), with depth in [0, 1] (see
  `src/math/math3d.h`). Shaders receive them as four rows applied with `dot()`, so there is no layout
  ambiguity between SPIR-V and HLSL.
- Per-draw constants fit in 252 bytes (`mesh_constants.h`), under the 256-byte limit of push constants
  and root constants. That block is **full**: any new per-draw data needs another mechanism.

## Bindless GPU layer (`gpu.h`)

The photorealistic renderer doesn't use `draw_vertices`. It uses `gpu.h`, a *bindless* RHI:

- **Resources**: textures and buffers live in a global table, and shaders refer to them by index.
- **Passes**: `gpu_begin_pass`, `gpu_bind_pipeline`, `gpu_push_constants` and `gpu_draw` are recorded
  into the frame's command list.
- **Transient memory**: `gpu_frame_alloc` gives per-frame memory.
- **Ray tracing**: `gpu_raytracing_supported` and `gpu_build_ray_scene`, on DXR 1.1 or
  `VK_KHR_ray_query`.

`gpu_supported()` returns 0 without the requirements:

- on Vulkan: Vulkan 1.3, with dynamic rendering and descriptor indexing;
- on DX12: resource binding tier 2.

## Statistics

`renderer_get_stats()` fills a `RendererStats` for profilers:

| Field | Meaning |
| --- | --- |
| `backend`, `gpu_name` | The backend in use and the GPU's name |
| `gpu_frame_ms` | GPU time of the last measured frame's command list, from timestamp queries; < 0 if unavailable |
| `vram_used`, `vram_budget` | Video memory used by the process and its budget, in bytes; 0 if unknown |
| `draw_calls`, `mesh_draws`, `vertices_2d` | Counts for the last frame |

- **Draw counts** cover everything between two `end_frame()` calls, including the synchronous passes.
- **GPU time** is measured around the frame's command list only, and arrives a frame or two late,
  since the GPU runs behind the CPU.
- **Backends** provide their part through `RendererBackend::get_stats`: timestamp queries (a
  `VkQueryPool` or an `ID3D12QueryHeap`) and DXGI or VMA memory budgets.

Next: [Photorealistic rendering and path tracing](Photorealistic-Rendering).
