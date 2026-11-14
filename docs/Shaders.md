# Shaders (Slang)

Shaders are written once, in [Slang](https://shader-slang.org/), in `src/renderer/shaders/`, and feed
both backends:

| File | Contents |
| --- | --- |
| `sprite.slang` | 2D (`draw_vertices`): one vertex and one fragment shader. |
| `mesh.slang` | Forward 3D: shadow pass, scene pass, sky. |
| `photoreal.slang` | The photorealistic renderer and the path tracer. |

## Compilation

`cmake/pireneus_shaders.cmake` compiles every entry point twice, once for the whole project, into
`<build>/shaders`:

- **SPIR-V (Vulkan)**: `slangc -target spirv`, directly.
- **DXIL (DX12)**:
  1. `slangc -target hlsl` writes HLSL to `<build>/generated_shaders`;
  2. the Windows SDK's `dxc.exe` compiles that HLSL.

  Slang's own DXIL backend isn't used, because it needs `dxcompiler.dll`, which Slang's release
  archives don't ship.

`pireneus_copy_shaders(<target>)` copies the result next to an executable.

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
- **2D and forward 3D** use one combined texture (`t0`/`s0`). In `mesh.slang`, `pass_texture` is the
  shadow map during the scene pass, and the panorama during the sky pass.
- **Photorealistic**: bindless textures and buffers, plus four fixed samplers declared as separate
  variables (`g_sampler_point`, `g_sampler_linear`, `g_sampler_anisotropic`, `g_sampler_panorama`).
  A D3D12 static sampler can't back an array.
- **Shared structures**: the structs shared with C (`mesh_constants.h`, and `Frame` and `Instance` in
  `photoreal.c`) mirror them exactly.
