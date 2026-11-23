#ifndef _RENDER3D_INTERNAL_H_
#define _RENDER3D_INTERNAL_H_

// What the 3D renderer's files share : render3d.c (the API, and the passes both pipelines run),
// forward_plus.c and deferred_plus.c. Not part of the public API.

#include <stdint.h>

#include "renderer/gpu.h"
#include "renderer/render3d/render3d.h"

#define R3_MAX_DRAWS 4095 // Deferred+'s visibility buffer ID holds 12 bits of draw (all ones means "empty")
#define R3_MAX_CASCADES 4
#define R3_NO_TEXTURE 0xFFFFFFFFu
#define R3_MAX_LIGHTS 16384
#define R3_MAX_REFLECTION_PROBES 64

// Mirrors Frame in render3d_common.slang, field for field.
typedef struct
{
    float view_projection[16];
    float inverse_view_projection[16];
    float view[16];
    float inverse_projection[16];
    float camera_position[4];
    float sun_direction[4];
    float sun_color[4];
    float sh[9][4];
    float environment[4];
    uint32_t environment_textures[4];
    float fog_inscatter[4];
    float fog_sun[4];
    float fog_params[4];
    float shadow_view_projection[R3_MAX_CASCADES][16];
    float cascade_far[4];
    uint32_t shadow_textures[4];
    float shadow_texel_world[4];
    float shadow_depth_range[4];
    float shadow_params[4];
    float resolution[4];
    float cluster_params[4];
    uint32_t cluster_grid[4];
    uint32_t light_data[4];
    uint32_t local_shadows[4];
    float reprojection[16];
    float jitter[4];
    float clouds[4];
    float clouds_motion[4];
    float volumetrics[4];
    float probe_origin[4];
    uint32_t probe_grid[4];
    uint32_t probe_layout[4];
    uint32_t reflection_probes[4];
    float gi_origin[4];
    uint32_t gi_grid[4];
    uint32_t gi_textures[4];
} R3Frame;

// Mirrors ReflectionProbe in render3d_common.slang.
typedef struct
{
    float position[4];
    float box_min[4];
    float box_max[4];
} R3ReflectionProbe;

// Mirrors Light in render3d_common.slang.
typedef struct
{
    float position_range[4];
    float color[4];
    float direction_type[4];
    float spot_shadow[4];
} R3Light;

// Mirrors ShadowTile.
typedef struct
{
    float view_projection[16];
    float rect[4];
    float params[4];
} R3ShadowTile;

// Mirrors Instance in render3d_common.slang.
typedef struct
{
    float model[12];
    float normal_matrix[12];
    uint32_t geometry[4];
    float base_color[4];
    float emissive[4];
    float surface[4];
    uint32_t textures[4];
    uint32_t textures2[4];
    float uv_transform[4];
    uint32_t lightmap[4];
    float lightmap_transform[4];
} R3Instance;

// What every pass pushes (see Push in render3d_common.slang).
typedef struct
{
    uint32_t ids[4];    // x : frame buffer ; y : frame data offset ; z : instances offset ; w : draw index
    uint32_t inputs[4]; // the pass's textures
    float params[4];
    float params2[4];
} R3PassConstants;

typedef struct
{
    Mesh* mesh;
    int first_index;
    int index_count;
    float center[3];  // world-space bounding sphere
    float radius;     // < 0 : no bounds, never culled
    float extent[3];  // world-space box : center +- extent
    int hidden;       // occlusion culling : not from the camera
} R3Draw;

// The frame being rendered, as both pipelines see it.
typedef struct
{
    Render3DQuality quality;
    Render3DLook look;
    Render3DCamera camera;

    R3Instance instances[R3_MAX_DRAWS];
    R3Draw draws[R3_MAX_DRAWS];
    uint8_t in_view[R3_MAX_DRAWS]; // inside the camera's frustum this frame
    int draw_count;

    Render3DLight lights[R3_MAX_LIGHTS];
    int light_count;

    Render3DReflectionProbe reflection_probes[R3_MAX_REFLECTION_PROBES];
    int reflection_probe_count;

    int width;        // internal resolution
    int height;

    // Shared targets, at the internal resolution : the scene's depth (written by each pipeline's
    // first pass) and its lit HDR image (written by its second).
    Texture* depth_target;
    Texture* hdr_target;

    // What shaders read the scene's depth from : depth_target, or with MSAA its samples resolved.
    Texture* scene_depth;
    int msaa_samples;    // the targets', Forward+ only
    int ssr;             // the targets', Deferred+ only
} R3State;

extern R3State r3;

Texture* r3_create_target(int width, int height, GpuFormat format, unsigned usage);
void r3_destroy_target(Texture** texture);
GpuPipeline* r3_screen_pipeline(const char* fragment_shader, GpuFormat format, GpuBlend blend);
void r3_fullscreen_pass(Texture* target, GpuPipeline* pipeline, const R3PassConstants* constants);
int r3_is_masked(int draw); // alpha-tested, with a texture to cut holes with
int r3_sphere_in_frustum(const float center[3], float radius, const Mat4* view_projection);
// The same, for many spheres : the planes (left, right, bottom, top, near ; normalized) computed once.
void r3_frustum_planes(const Mat4* view_projection, float planes[5][4]);
int r3_sphere_in_planes(const float planes[5][4], const float center[3], float radius);

// Point and spot lights (render3d_lights.c) : clustering, and their shadows' atlas. r3_lights_prepare()
// runs before any pass is recorded (it may recreate the atlas) and fills the frame's light fields ;
// r3_lights_render_shadows() records the tiles that changed.
int r3_lights_create(void);
void r3_lights_destroy(void);
int r3_lights_prepare(R3Frame* frame, const Mat4* view_projection);
void r3_lights_render_shadows(R3PassConstants* constants);

// Realtime GI (render3d_gi.c) : r3_gi_prepare() before any pass (1 if it runs this frame), then
// r3_gi_update() records its passes, before the pipelines'.
int r3_gi_create(void);
void r3_gi_destroy(void);
int r3_gi_available(void);
int r3_gi_prepare(R3Frame* frame);
void r3_gi_update(R3PassConstants* constants);

// Each pipeline : its GPU objects, its own targets, then two stages of a frame. `geometry` writes
// r3.scene_depth (the shared ambient occlusion runs on it next), `lighting` writes r3.hdr_target.
int forward_plus_create(void);
void forward_plus_destroy(void);
int forward_plus_create_targets(void);
void forward_plus_destroy_targets(void);
void forward_plus_geometry(R3PassConstants* constants);
void forward_plus_lighting(R3PassConstants* constants);

int deferred_plus_create(void);
void deferred_plus_destroy(void);
int deferred_plus_create_targets(void);
void deferred_plus_destroy_targets(void);
void deferred_plus_geometry(R3PassConstants* constants);
void deferred_plus_lighting(R3PassConstants* constants);

#endif // _RENDER3D_INTERNAL_H_
