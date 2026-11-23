#ifndef _RENDER3D_H_
#define _RENDER3D_H_

// The engine's 3D renderer, written once on top of the GPU layer (renderer/gpu.h), with two pipelines
// that share almost everything : lighting, shadows, sky, ambient occlusion, post-processing.
//
// - Forward+ (RENDER3D_FORWARD_PLUS) : a depth prepass, then each object shaded where it is drawn.
//   The cheapest of the two, meant to rely on baked lighting.
// - Deferred+ (RENDER3D_DEFERRED_PLUS) : a triangle visibility buffer (The Forge), then one
//   full-screen pass shading every pixel exactly once, whatever the overdraw.
//
// Lighting is physically based (GGX, split-sum image-based lighting) ; the outdoors treatment (sun, sky,
// bounce light, height fog, contrast curve, vignette) comes from Inigo Quilez's articles.
//
// Per frame, after begin_frame() and before drawing into the window :
//     render3d_begin(&camera);
//     render3d_add(mesh, ...);        // every visible object
//     render3d_render(width, height); // every pass
//     render3d_present();             // draws the result into the window

#include "renderer/renderer.h"
#include "renderer/gpu.h"
#include "renderer/render3d/environment.h"
#include "math/math3d.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef enum
{
    RENDER3D_FORWARD_PLUS,
    RENDER3D_DEFERRED_PLUS
} Render3DPipeline;

typedef enum
{
    RENDER3D_TONEMAP_ACES,
    RENDER3D_TONEMAP_NEUTRAL,
    RENDER3D_TONEMAP_REINHARD,
    RENDER3D_TONEMAP_NONE
} Render3DTonemapper;

// How much GPU time to spend : project settings' quality presets fill this in.
typedef struct
{
    float render_scale;      // internal resolution, relative to the window
    int shadow_resolution;   // of each cascade
    int shadow_cascades;     // 1 to 4
    int shadow_samples;      // filter taps
    float shadow_distance;   // how far from the camera shadows reach, in meters
    int local_shadow_atlas;  // point and spot lights' shadows : the atlas' size (8 x 8 tiles)
    int local_shadow_lights; // at most this many lights cast shadows at once, the most important ones
    int ao_enabled;          // screen-space ambient occlusion (GTAO)
    int ao_slices;
    int ao_steps;
    int bloom_enabled;
    int fxaa_enabled;
    int msaa_samples;        // Forward+ : 1 (off), 2, 4 or 8
    int taa_enabled;         // temporal anti-aliasing, both pipelines
    float sharpness;         // contrast adaptive sharpening of the final image : 0 = off, to 1
    int ssr_enabled;         // Deferred+ : screen-space reflections
    int ssr_steps;
    int volumetrics_enabled; // light scattered by the fog (shafts, halos), in place of the analytic fog
    int volumetric_steps;
    int volumetric_local_lights; // the point and spot lights scatter too, not only the sun
    int gi_enabled;          // realtime global illumination (ray tracing GPUs) : replaces the baked lighting
    int gi_rays;             // per probe and frame
    float gi_spacing;        // meters between probes (coarser for big scenes)
    int gi_lights;           // how many point and spot lights bounce (the first ones added)
} Render3DQuality;

// The image's look.
typedef struct
{
    float exposure_ev;       // on top of the automatic exposure derived from the environment
    Render3DTonemapper tonemapper;
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
    float ssr_distance;      // meters : how far reflection rays go
    float ssr_thickness;     // meters : how thick surfaces are, for rays passing behind them
    float volumetric_distance;   // meters : how far the volumetric fog is computed
    float volumetric_anisotropy; // 0 = scatters evenly, toward 1 = mostly forward (glow around lights)
    float volumetric_intensity;
    float cloud_coverage;    // procedural clouds : 0 = none, to 1 = overcast
    float cloud_density;
    float cloud_altitude;    // meters
    float cloud_scale;       // meters : the size of their features
    float cloud_wind[2];     // m/s, along X and Z
    float cloud_shadows;     // 0 to 1 : how much they shade the ground
} Render3DLook;

void render3d_default_quality(Render3DQuality* out_quality);
void render3d_default_look(Render3DLook* out_look);

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
    int normal_y_down;               // the normal map's green points down (DirectX's convention) ; else up
    float uv_scale[2];
    float uv_offset[2];
    Texture* base_color_texture;     // NULL : none. sRGB-encoded
    Texture* normal_texture;         // tangent space ; its blue is ignored (rebuilt : two-channel BC5 works)
    Texture* metallic_roughness_texture; // G = roughness, B = metallic
    Texture* emissive_texture;
    Texture* occlusion_texture;
} PbrMaterial;

void render3d_default_material(PbrMaterial* out_material);

typedef struct
{
    Mat4 view;
    Mat4 projection;
    Vec3 eye;
    float near_plane;
    float far_plane;
} Render3DCamera;

typedef enum
{
    RENDER3D_LIGHT_POINT,
    RENDER3D_LIGHT_SPOT
} Render3DLightType;

// A point or spot light ; the sun is the environment's (render3d_set_environment()). There's no set limit
// to how many a frame has : they're sorted into clusters of the view, and each pixel only goes through
// those near it.
typedef struct
{
    Render3DLightType type;
    float position[3];
    float direction[3];      // spot : where it points, normalized
    float color[3];
    float intensity;         // the irradiance it gives at 1 m (on its axis), in the sun's units
    float range;             // meters : nothing is lit farther than this
    float spot_angle;        // spot : the cone's full angle, in degrees
    float spot_softness;     // spot : 0 = a hard edge, 1 = fading all the way from the axis
    int cast_shadows;
    uint32_t id;             // the same from frame to frame (an object's index...) : shadows that didn't change are kept
} Render3DLight;

void render3d_default_light(Render3DLight* out_light);

int render3d_create(void); // after create_renderer() ; 0 if the GPU can't run it
int render3d_realtime_gi_supported(void);
void render3d_destroy(void);

void render3d_set_pipeline(Render3DPipeline pipeline);
Render3DPipeline render3d_pipeline(void);
void render3d_set_settings(const Render3DQuality* quality, const Render3DLook* look);

// The sky and the light. `sun_direction` (toward the sun) and `sun_color` are what light the scene ; an
// HDR environment's own sun (Environment::has_sun) is usually what to pass. May not be NULL.
void render3d_set_environment(const Environment* environment, const float sun_direction[3], const float sun_color[3]);

void render3d_begin(const Render3DCamera* camera);

// Baked lighting (see bake/bake.h) : an object's lightmap, its diffuse light from afar (direct light of
// baked lights, and every light's indirect one), irradiance / PI in RGBA16F.
typedef struct
{
    Texture* texture;        // the atlas page
    GpuBuffer* uvs;          // a float2 per vertex of the mesh, in the object's chart space
    float scale_offset[4];   // that space into the page : xy scale, zw offset
} Render3DLightmap;

typedef struct
{
    const Render3DLightmap* lightmap; // NULL : lit by the probe volume, or the environment
    int hidden_from_view;    // occlusion culling : not drawn from the camera, its shadows still are
} Render3DDrawOptions;

// One object to draw this frame : `index_count` indices of `mesh` from `first_index`, `model` being its
// world matrix (row-major), `bounds` its local bounds (min xyz, max xyz) or NULL to never cull it.
// `options` may be NULL.
void render3d_add(Mesh* mesh, int first_index, int index_count, const float model[16], const float bounds[6], const PbrMaterial* material,
    const Render3DDrawOptions* options);

// The baked probe volume (bake/bake.h's layout, in one GPU buffer) : the diffuse light of everything
// that has no lightmap. NULL : none, the environment's light instead.
typedef struct
{
    GpuBuffer* buffer;
    float origin[3];         // the grid's corner
    float cell_size;         // meters
    int cells[3];
    uint32_t table_offset;   // in the buffer, bytes : a brick index per cell
    uint32_t coarse_offset;  // the coarse probes, (cells + 1) per axis
    uint32_t bricks_offset;  // the bricks, 5 x 5 x 5 probes each
} Render3DProbeVolume;

void render3d_set_probe_volume(const Render3DProbeVolume* volume);

// A reflection probe (environment probe), for this frame : a capture of the scene (see
// environment_create_from_pixels()), reflected by what's inside its box, box-projected.
typedef struct
{
    const Environment* environment;
    float position[3];       // where it was captured
    float box_min[3];        // world space
    float box_max[3];
    float blend;             // meters : the fade inside the box's walls
} Render3DReflectionProbe;

void render3d_add_reflection_probe(const Render3DReflectionProbe* probe);

// The last render3d_render()'s lit image (RGBA16F, its internal resolution), before tone mapping : what a
// reflection probe captures. Valid until the next render ; without TAA, it's the final HDR image.
Texture* render3d_hdr_image(void);
void render3d_add_light(const Render3DLight* light);

void render3d_render(int width, int height);
void render3d_present(void);

#ifdef __cplusplus
}
#endif

#endif // _RENDER3D_H_
