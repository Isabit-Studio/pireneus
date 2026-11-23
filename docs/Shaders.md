# Shaders (Slang)

Shaders are written once, in [Slang](https://shader-slang.org/), in `src/renderer/shaders/`, and feed
both backends:

| File | Contents |
| --- | --- |
| `sprite.slang` | 2D (`draw_vertices`): one vertex and one fragment shader. |
| `render3d_common.slang` | Included by the 3D shaders: bindless tables, per-frame and per-object data, lights, shadows, sky and clouds, probes, materials and the whole lighting model. |
| `render3d.slang` | The passes both 3D pipelines share: shadows, sky, AO, MSAA resolves, volumetrics, TAA, bloom, tone mapping, FXAA, sharpening. |
| `forward_plus.slang` | Forward+: the prepass and shading pass. |
| `deferred_plus.slang` | Deferred+: the visibility buffer, its shading, screen-space reflections. |
| `realtime_gi.slang` | Realtime GI's probes, with ray queries (`ps_6_5`). |

## Compilation

`cmake/pireneus_shaders.cmake` compiles every entry point twice, once for the whole project, into
`<build>/shaders`:

- **SPIR-V (Vulkan)**: `slangc -target spirv`, directly.
- **DXIL (DX12)**:
  1. `slangc -target hlsl` writes HLSL to `<build>/generated_shaders`;
  2. the Windows SDK's `dxc.exe` compiles that HLSL.

  Slang's own DXIL backend isn't used, because it needs `dxcompiler.dll`, which Slang's release
  archives don't ship.

`pireneus_copy_shaders(<target>)` copies the result next to an executable, on every build (a change to
shaders alone is deployed too). Every entry point depends on every `.slang` file, since they include
each other.

To add an entry point, add a line to `PIRENEUS_SHADER_ENTRY_POINTS`:

```
"<source>,<entry point>,<stage>,<SPIR-V name>,<dxc profile>,<.cso name>"
```

The backends load the files by those output names.

Slang is downloaded as a prebuilt binary, and only on Windows for now. On other platforms, configuring
warns that no shader gets compiled.

## Conventions

- **Constants** go in a `cbuffer` marked `[vk::push_constant]`: push constants on Vulkan, root
  constants (register `b0`) on DX12.
- **Matrices** arrive as four **row** `float4`s, applied with explicit `dot()`s. Their memory layout
  is therefore unambiguous between SPIR-V and HLSL.
- **2D** uses one combined texture (`t0`/`s0`).
- **3D**: bindless textures, multisampled textures and buffers, plus four fixed samplers declared as
  separate variables (`g_sampler_point`, `g_sampler_linear`, `g_sampler_anisotropic`,
  `g_sampler_panorama`). A D3D12 static sampler can't back an array.
- **Shared structures**: the structs shared with C (`Frame`, `Instance`, `Light`... in
  `render3d_internal.h`) mirror them exactly.
