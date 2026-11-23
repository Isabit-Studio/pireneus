# 3D rendering: Forward+ and Deferred+

- Code: `src/renderer/render3d/`.
- Shaders: `src/renderer/shaders/` (see [Shaders](Shaders)).
- Built on the [bindless GPU layer](Rendering#bindless-gpu-layer-gpuh).

The engine has two 3D pipelines. They share almost everything: materials and lighting (one shading
function, `shade_surface()` in `render3d_common.slang`), lights, shadows, the sky and clouds, fog and
volumetrics, probes, lightmaps, occlusion culling, ambient occlusion and the whole post-processing chain.
Only the way they draw the scene differs:

| | Forward+ | Deferred+ |
| --- | --- | --- |
| Geometry | Depth prepass, then each object shaded where it's drawn | Triangle visibility buffer (after The Forge), then one full-screen shading pass |
| Anti-aliasing | MSAA (2x, 4x, 8x), FXAA | TAA with sharpening |
| Reflections | Reflection probes | Screen-space reflections, over the reflection probes |
| Meant for | The cheapest frames, relying on baked lighting | Realtime lighting: realtime GI, reflections |

The other effects work in both: TAA in Forward+, realtime GI in Forward+, MSAA excepted. The quality
presets pick sensible defaults for each pipeline.

## API

It is global state, like the rest of the renderer:

```c
render3d_create();                                    // after create_renderer() ; 0 if the GPU can't run it
render3d_set_pipeline(RENDER3D_FORWARD_PLUS);         // or RENDER3D_DEFERRED_PLUS
render3d_set_settings(&quality, &look);               // Render3DQuality, Render3DLook
render3d_set_environment(&environment, sun_direction, sun_color);
render3d_set_probe_volume(&volume);                   // baked, optional (see Baked lighting)

// Every frame, after begin_frame():
render3d_begin(&camera);
render3d_add(mesh, first_index, index_count, model, bounds, &material, &options);  // each object
render3d_add_light(&light);                           // each point or spot light
render3d_add_reflection_probe(&probe);                // each baked reflection probe
render3d_render(width, height);
render3d_present();                                   // draws the result into the window
```

- `render3d_default_quality`, `render3d_default_look`, `render3d_default_material` and
  `render3d_default_light` give starting values.
- `Render3DDrawOptions` carries an object's lightmap and its occlusion culling state; `NULL` means none.
- The shader structs (`Frame`, `Instance`, `Light`...) mirror those of `render3d_internal.h` exactly: they
  must change together.

## Materials

`PbrMaterial`: base color, metalness, roughness, normal map, emission, occlusion, alpha cutoff.

- **Textures** come from `pbr_texture.h`:
  - image files (PNG, JPEG, TGA, BMP), decoded to RGBA8 with a mip chain;
  - **DDS files** (BC1, BC3, BC5, with their mips), kept compressed on the GPU. Where it can't sample
    them, or for sizes that aren't multiples of 4 (Direct3D's rule), they're decoded on the CPU.
- **Normal maps**: only X and Y are read, Z is rebuilt, so two-channel BC5 maps work.
  `normal_y_down` flips green for DirectX's convention, which DDS files from game pipelines use.
- `pbr_texture_load_info()` also says whether a texture's alpha is a **cutout**: some texels clearly
  transparent, others clearly opaque. Models whose file says nothing about alpha (FBX) get an alpha
  cutoff from it (foliage), while an even alpha (glass) stays opaque.

## A frame

1. **Shadows.**
   - The sun: cascaded shadow maps (1 to 4), soft shadows with contact hardening (PCSS).
   - Point and spot lights: an atlas of 8 x 8 tiles, one per spot light and six per point light. Only
     the most important lights in view get tiles. A tile is only drawn again when its light, or a
     caster within its reach, has changed: a static scene costs no shadow rendering at all.
2. **Realtime GI**, if it's on: see below.
3. **The scene's geometry**: Forward+'s depth prepass, or Deferred+'s visibility buffer.
4. **Ambient occlusion** (GTAO), from the depth, blurred.
5. **Lighting**: Forward+'s shading pass (with its MSAA resolve), or Deferred+'s full-screen pass (then
   its screen-space reflections).
6. **Volumetric fog**, blended over the image.
7. **Post-processing**: TAA, bloom, tone mapping (ACES, Khronos Neutral or Reinhard), S-curve, saturation,
   vignette, FXAA, then contrast adaptive sharpening. Auto exposure is `0.22 / average luminance × 2^EV`.

## Lights

Point and spot lights (`Render3DLight`) have no set limit: they're sorted on the CPU into clusters
(16 x 9 tiles across the view, 24 exponential slices in depth, as id Tech 6 does). A pixel only goes
through the lights of its cluster.

- Intensity is the irradiance at 1 m, in the sun's units; light fades to nothing at its range.
- A spot's cone fades from its outer angle to its inner one (`spot_softness`).
- `id` must stay the same from frame to frame: shadow tiles are kept by it.

## Forward+

- The prepass and the shading pass run the **same vertex shader**. The same program on the same
  vertices lands on exactly the same depth, which the shading pass's depth test relies on.
- **MSAA**:
  - Varyings are interpolated at the centroid, so silhouette pixels aren't shaded outside their triangle.
  - Shaders resolve the samples:
    - the screen-space passes get the closest depth of each pixel;
    - the HDR image is averaged in a tone-mapped space (Karis), so one bright sample on an edge doesn't
      bring the aliasing back.

## Deferred+

- **Visibility buffer**: each pixel stores an ID (12 bits of draw, 20 bits of triangle). The shading pass
  fetches the triangle, recomputes its barycentrics and derivatives, and shades each pixel once.
- **Screen-space reflections**:
  - one ray per pixel, drawn from the surface's GGX lobe;
  - marched in world space against the depth, then refined by bisection;
  - they replace the environment's reflection where they find a hit;
  - rough surfaces keep the environment's (or the reflection probes').

## Shared effects

- **TAA**: Halton jitter, a Catmull-Rom history, YCoCg variance clipping, and weights against
  brightness. Sampling patterns (shadows, AO, reflections) change every frame with TAA, and TAA averages
  them away.
- **Sharpening**: contrast adaptive sharpening, after AMD FidelityFX CAS.
- **Clouds**:
  - a procedural layer at a fixed altitude (fractal noise, drifting with the wind);
  - lit by looking toward the sun through the layer;
  - its shadows darken the ground.
- **Volumetric fog**:
  - at half resolution, a ray marched through the height fog's medium;
  - the sun (its shadow cascades and the clouds' shadows) and, optionally, the point and spot lights
    scatter in it;
  - upsampled with depth-aware weights;
  - it replaces the analytic fog.
- **Reflection probes** (`Render3DReflectionProbe`):
  - baked captures, each with a box;
  - reflections are box-projected (parallax-corrected);
  - the two strongest blend, and fall back to the sky.
- **Diffuse light from afar**, in this order:
  1. realtime GI;
  2. the object's lightmap;
  3. the baked probe volume;
  4. the environment's spherical harmonics.

## Realtime GI

Available where the GPU traces rays (DXR 1.1, `VK_KHR_ray_query`), after DDGI:

- a grid of probes over the scene;
- every frame, each probe traces rays in a new random rotation. Each ray brings back the sun, the sky,
  emission and the probes' own light at its hit: infinite bounces, a frame at a time;
- the rays are integrated into L1 spherical harmonics, blended over the previous frames';
- interpolation weighs each probe by how much it faces the point (against leaking through walls).

Against flickering:

- the rays' sky leaves out the sun's disk: the sun is already a light, and a ray finding its tiny disk now
  and then made a probe flash;
- what one ray brings back is limited to a few times its probe's average so far, so a small bright thing
  only shows once enough rays have seen it;
- the grid only moves when the scene no longer fits in it (with a cell of margin), by whole cells, and its
  probes keep their history. Only a grid of a new size starts again from nothing.

Realtime GI takes over from the baked lighting. Quality settings: rays per probe, spacing, and how many
lights bounce.

## Environment

`environment.{h,c}` computes everything on the CPU, identically on every backend:

- spherical harmonics of the irradiance;
- a prefiltered GGX mip chain, in equirectangular layout;
- the sun, which comes from one of two sources:
  - found in an HDR panorama, then removed from the image (it becomes the directional light);
  - or placed in a **procedural sky** (Rayleigh and Mie scattering, Nishita's model).

`environment_create_from_pixels()` builds the same from a panorama in memory: what reflection probes
are.

HDR panoramas (`.hdr`, `.exr`, or an 8-bit image) are read by `src/assets/hdr_image.c`. The engine
ships no sky: skies are project assets, and without one the procedural sky is used.

Next: [Baked lighting](Baked-Lighting).
