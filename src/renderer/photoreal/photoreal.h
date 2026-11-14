#ifndef _PHOTOREAL_H_
#define _PHOTOREAL_H_

// The photorealistic renderer, written once on top of the GPU layer (renderer/gpu.h).
//
// Inspired by two bodies of work :
// - The Forge (ConfettiFX) : a triangle visibility buffer, bindless resources, physically based
//   materials (GGX, split-sum image-based lighting).
// - Inigo Quilez's articles : the outdoors lighting rig (sun, sky, bounce), analytic height fog with sun
//   in-scattering, and his final image treatment.
//
// Per frame, after begin_frame() and before drawing into the window :
//     photoreal_begin(&camera);
//     photoreal_add(mesh, ...);        // every visible object
//     photoreal_render(width, height); // every pass
//     photoreal_present();             // draws the result into the window

#include "renderer/renderer.h"
#include "renderer/photoreal/environment.h"
#include "math/math3d.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef enum
{
    PHOTOREAL_TONEMAP_ACES,
    PHOTOREAL_TONEMAP_NEUTRAL,
    PHOTOREAL_TONEMAP_REINHARD,
    PHOTOREAL_TONEMAP_NONE
} PhotorealTonemapper;

// How much GPU time to spend : project settings' quality presets fill this in.
typedef struct
{
    float render_scale;      // internal resolution, relative to the window
    int shadow_resolution;   // of each cascade
    int shadow_cascades;     // 1 to 4
    int shadow_samples;      // filter taps
    float shadow_distance;   // how far from the camera shadows reach, in meters
    int ao_enabled;
    int ao_slices;
    int ao_steps;
    int bloom_enabled;
    int fxaa_enabled;
    int path_tracing;           // the reference mode, on GPUs with ray tracing (see photoreal_path_tracing_supported())
    int path_bounces;
    int path_samples_per_frame;
} PhotorealQuality;

// The image's look.
typedef struct
{
    float exposure_ev;       // on top of the automatic exposure derived from the environment
    PhotorealTonemapper tonemapper;
    float bloom_strength;
    float contrast;          // Quilez's S-curve, 0 to 1
    float saturation;        // 1 = unchanged
    float vignette;
    float ao_radius;         // meters
    float ao_power;
    float sky_intensity;
    float bounce_strength;   // Quilez's bounce light
    float sun_softness;      // penumbra width, relative to the real sun's
    float fog_density;       // 0 = off
    float fog_height_falloff;
    float fog_base_height;
    float fog_color[3];
    float fog_sun_color[3];
    float fog_sun_power;
} PhotorealLook;

void photoreal_default_quality(PhotorealQuality* out_quality);
void photoreal_default_look(PhotorealLook* out_look);

typedef struct
{
    float base_color[4];
    float emissive[3];
    float metallic;
    float roughness;
    float normal_scale;
    float occlusion_strength;
    float alpha_cutoff;              // 0 : opaque
    int ignore_vertex_color;         // base_color alone, rather than multiplied by the mesh's vertex colors
    float uv_scale[2];
    float uv_offset[2];
    Texture* base_color_texture;     // NULL : none. sRGB-encoded
    Texture* normal_texture;
    Texture* metallic_roughness_texture; // G = roughness, B = metallic
    Texture* emissive_texture;
    Texture* occlusion_texture;
} PbrMaterial;

void photoreal_default_material(PbrMaterial* out_material);

typedef struct
{
    Mat4 view;
    Mat4 projection;
    Vec3 eye;
    float near_plane;
    float far_plane;
} PhotorealCamera;

int photoreal_create(void); // after create_renderer() ; 0 if the GPU can't run it
int photoreal_path_tracing_supported(void);
int photoreal_accumulated_samples(void); // how converged the path tracer's image is
void photoreal_destroy(void);

void photoreal_set_settings(const PhotorealQuality* quality, const PhotorealLook* look);

// The sky and the light. `sun_direction` (toward the sun) and `sun_color` are what light the scene ; an
// HDR environment's own sun (Environment::has_sun) is usually what to pass. May not be NULL.
void photoreal_set_environment(const Environment* environment, const float sun_direction[3], const float sun_color[3]);

void photoreal_begin(const PhotorealCamera* camera);

// One object to draw this frame : `index_count` indices of `mesh` from `first_index`, `model` being its
// world matrix (row-major), `bounds` its local bounds (min xyz, max xyz) or NULL to never cull it.
void photoreal_add(Mesh* mesh, int first_index, int index_count, const float model[16], const float bounds[6], const PbrMaterial* material);

void photoreal_render(int width, int height);
void photoreal_present(void);

#ifdef __cplusplus
}
#endif

#endif // _PHOTOREAL_H_
