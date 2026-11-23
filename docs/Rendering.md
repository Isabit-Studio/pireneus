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

### The 2D renderer (`render2d`)

`src/renderer/render2d/` draws sprites placed in the world (textured quads) as a camera sees them: the
editor's 2D render path, and a game's.

```c
render2d_begin(&camera, &settings);   // Render2DCamera, Render2DSettings
render2d_add(model, size, texture, uv, color);  // each sprite
render2d_prepare();                   // before begin_frame()
begin_frame();
render2d_present();
```

- Sprites are sorted back to front (by world Z), then batched: consecutive sprites sharing a texture go
  out in one `draw_vertices()` call.
- **Pixel perfect** (`Render2DSettings::pixel_perfect`):
  - the scene is drawn into a render target of a fixed height in pixels (`pixel_height`), then scaled up
    by the largest whole factor that fits;
  - the width follows the view's aspect ratio: a wider view shows more of the world, rather than bars;
  - the camera snaps to whole art pixels in the target, and the remainder moves the scaled image by
    whole screen pixels: the art never shimmers, and the camera still moves smoothly;
  - it needs an orthographic, axis-aligned camera; any other view is drawn directly.

### Render targets

`create_render_target()`, then `begin_render_target_pass()` and `end_render_target_pass()`, draw with
`draw_vertices()` into an offscreen texture.

A pass is a **synchronous** unit of GPU work: it is submitted and waited on. Its texture can be
sampled as soon as the pass ends.

The *Pixel Perfect* example uses one to render the scene 216 pixels tall, then scales it up by a whole
factor.

## Bindless GPU layer (`gpu.h`)

The 3D renderer (`render3d`, see [3D rendering](3D-Rendering)) doesn't use `draw_vertices`. It uses
`gpu.h`, a *bindless* RHI:

- **Resources**: textures and buffers live in a global table, and shaders refer to them by index.
  Multisampled textures have their own table (`g_textures_ms`, `Texture2DMS`).
- **Passes**: `gpu_begin_pass`, `gpu_bind_pipeline`, `gpu_push_constants`, `gpu_set_viewport` (one tile
  of an atlas) and `gpu_draw` are recorded into the frame's command list.
- **Pipelines**: depth test (or `depth_always`), blending (`GPU_BLEND_ALPHA`, `ADD`, `ATTENUATE`),
  sample count.
- **Transient memory**: `gpu_frame_alloc` gives per-frame memory.
- **Readback**: `gpu_read_texture`, outside a frame (captures, baking).
- **Compressed textures**: `GPU_FORMAT_BC1`, `BC3` (and their sRGB variants) and `BC5`, uploaded as
  rows of 4 x 4 blocks (`gpu_format_level_size()`). `gpu_format_supported()` says whether the GPU
  samples them (Vulkan's `textureCompressionBC`; always on Direct3D 12).
- **Frame memory is write-combined**: write it, never read it back. Build anything that needs reading
  while it's filled (lists, counters) in ordinary memory, then copy it over.
- **Ray tracing**: `gpu_raytracing_supported` and `gpu_build_ray_scene`, on DXR 1.1 or
  `VK_KHR_ray_query`. Shaders trace with inline ray queries: realtime GI uses them.

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
| `draw_calls`, `mesh_draws`, `vertices_2d` | Counts for the last frame (`mesh_draws` : `gpu_draw_indexed()` calls) |

- **Draw counts** cover everything between two `end_frame()` calls, including the synchronous passes
  (render targets).
- **GPU time** is measured around the frame's command list only, and arrives a frame or two late,
  since the GPU runs behind the CPU.
- **Backends** provide their part through `RendererBackend::get_stats`: timestamp queries (a
  `VkQueryPool` or an `ID3D12QueryHeap`) and DXGI or VMA memory budgets.

Next: [3D rendering](3D-Rendering).
