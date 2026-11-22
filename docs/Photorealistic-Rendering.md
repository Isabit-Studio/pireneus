# Photorealistic rendering

- Code: `src/renderer/photoreal/`.
- Shaders: `src/renderer/shaders/photoreal.slang`.
- Built on the [bindless GPU layer](Rendering#bindless-gpu-layer-gpuh).

## API

It is global state, like the rest of the renderer:

```c
photoreal_create();                                   // after create_renderer() ; 0 if the GPU can't run it
photoreal_set_settings(&quality, &look);              // PhotorealQuality, PhotorealLook
photoreal_set_environment(&environment, sun_direction, sun_color);

// Every frame, after begin_frame():
photoreal_begin(&camera);
photoreal_add(mesh, first_index, index_count, model, bounds, &material);  // for each object
photoreal_render(width, height);
photoreal_present();                                  // draws the result into the window
```

Helpers:

- `photoreal_default_quality`, `photoreal_default_look` and `photoreal_default_material` give starting
  values.

The shader's `Frame` and `Instance` structs mirror those of `photoreal.c` exactly: they must change
together.

## Real time

1. **Cascaded shadows** (1 to 4). Alpha-masked materials have their own pipeline.
2. **Visibility buffer**, after The Forge: each pixel stores an ID (12 bits of draw, 20 bits of
   triangle).
3. **GTAO** (ambient occlusion), with a bilateral blur.
4. **Full-screen shading**, each pixel exactly once:
   - GGX, and split-sum IBL with multiple-scattering compensation;
   - PCSS soft shadows, with a shadow terminator fix;
   - analytic height fog and approximate bounce light.
5. **Post-processing**: bloom, tone mapping (ACES, Khronos Neutral or Reinhard), S-curve, saturation,
   vignette, FXAA. Auto exposure is `0.22 / average luminance × 2^EV`.

## Environment

`environment.{h,c}` computes everything on the CPU, identically on every backend:

- spherical harmonics of the irradiance;
- a prefiltered GGX mip chain, in equirectangular layout;
- the sun, which comes from one of two sources:
  - found in an HDR panorama, then removed from the image (it becomes the directional light);
  - or placed in a **procedural sky** (Rayleigh and Mie scattering, Nishita's model).

HDR panoramas (`.hdr`, `.exr`, or an 8-bit image) are read by `src/assets/hdr_image.c`. The engine
ships no sky: skies are project assets, and without one the procedural sky is used.

The forward renderer uses it too: `draw_skybox()` for the background, and a hemispheric ambient term
derived from the spherical harmonics.
