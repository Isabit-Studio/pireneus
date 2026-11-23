#ifndef _BAKE_H_
#define _BAKE_H_

// Baked lighting and visibility, computed on the CPU (every core) by ray tracing the static geometry :
//
// - Lightmaps : each lightmapped mesh is unwrapped, one chart per face, the charts packed into pages ;
//   every texel then gathers the direct light of the baked lights, and the indirect light of all of them
//   and of the sky, bouncing around the scene (path tracing). Stored as irradiance / PI.
// - A probe volume : an adaptive grid of irradiance probes (L1 spherical harmonics) for everything without
//   a lightmap. Coarse cells everywhere, a brick of 5 x 5 x 5 finer probes where there's geometry.
// - Occlusion culling : the space cut into cells, and for each the static objects visible from it
//   (a potentially visible set), sampled with rays.
//
// A bake runs on its own thread : bake_start() copies everything it needs, so the scene can change
// meanwhile. The result is saved next to the scene, and read back by bake_load().

#include <stdint.h>

#include "renderer/renderer.h"

#ifdef __cplusplus
extern "C" {
#endif

// A static object : its triangles, in the GPU mesh's own vertex order (lightmap UVs are per vertex).
typedef struct
{
    uint32_t key;            // identifies it from one bake to the next (its name's hash)
    const MeshVertex* vertices; // object space ; their colors are its albedo
    int vertex_count;
    const uint32_t* indices;
    int index_count;
    const int* vertex_charts; // the chart of each vertex (vertices of a chart share a plane) ; NULL : no lightmap
    float model[16];         // row-major, to world space
    float albedo[3];         // multiplies the vertex colors
    float emissive[3];
    int occludes;            // 0 : only lit, never blocks light (glass...)
} BakeMesh;

typedef enum
{
    BAKE_LIGHT_DIRECTIONAL,
    BAKE_LIGHT_POINT,
    BAKE_LIGHT_SPOT
} BakeLightType;

typedef struct
{
    BakeLightType type;
    float position[3];
    float direction[3];      // where it points ; for the directional light, where its light goes
    float color[3];          // times the intensity : the irradiance at 1 m, or the sun's
    float range;
    float spot_angle;        // degrees, full
    float spot_softness;
    int baked;               // 1 : its direct light too is baked ; 0 (mixed) : only its indirect light
} BakeLight;

typedef struct
{
    float texels_per_meter;  // lightmap density
    int page_size;           // lightmap pages, texels per side
    int samples;             // rays per texel
    int bounces;
    float probe_spacing;     // the finest probes' spacing, meters ; 0 : no probe volume
    int probe_rays;
    float culling_cell;      // occlusion culling cells' size, meters ; 0 : none
    float sky_sh[9][3];      // the environment's irradiance / PI (Environment::sh) : what rays escaping see
    float sky_intensity;
} BakeSettings;

void bake_default_settings(BakeSettings* out_settings);

typedef struct BakeJob BakeJob;
typedef struct BakeResult BakeResult;

BakeJob* bake_start(const BakeMesh* meshes, int mesh_count, const BakeLight* lights, int light_count, const BakeSettings* settings);
float bake_progress(const BakeJob* job, const char** out_stage); // 0 to 1, and what it's doing
int bake_done(const BakeJob* job);
void bake_cancel(BakeJob* job);
BakeResult* bake_finish(BakeJob* job); // waits for it ; NULL if cancelled or failed. Frees the job.

int bake_save(const BakeResult* result, const char* path); // absolute ; 0 on failure
BakeResult* bake_load(const char* path);                   // NULL if absent or unreadable
void bake_free(BakeResult* result);

// Lightmaps : RGBA16F pages.
int bake_page_count(const BakeResult* result);
int bake_page_size(const BakeResult* result);
const uint16_t* bake_page_pixels(const BakeResult* result, int page);

// An object's lightmap : its page, and a UV per vertex (in page space). 0 if it has none, or if its mesh
// changed since (a different vertex count).
int bake_object_lightmap(const BakeResult* result, uint32_t key, int vertex_count, int* out_page, const float** out_uvs);

// The probe volume, laid out as render3d.h's Render3DProbeVolume reads it. 0 if there's none.
typedef struct
{
    const void* data;
    uint32_t size;
    float origin[3];
    float cell_size;
    int cells[3];
    uint32_t table_offset;
    uint32_t coarse_offset;
    uint32_t bricks_offset;
} BakeProbeVolume;

int bake_probe_volume(const BakeResult* result, BakeProbeVolume* out_volume);

// Occlusion culling : whether an object is potentially visible from a point. 1 when unknown (no data,
// the point outside the cells, an object baked without culling data).
int bake_object_visible(const BakeResult* result, uint32_t key, const float point[3]);

// Reflection probes : captured by the renderer itself (the editor does it, after the rest), stored here
// as equirectangular RGBA16F panoramas.
typedef struct
{
    uint32_t key;
    float position[3];
    float box_min[3];
    float box_max[3];
    float blend;
    int width;
    int height;
    const uint16_t* pixels;
} BakeReflectionProbe;

void bake_set_reflection_probe(BakeResult* result, const BakeReflectionProbe* probe); // copies ; replaces the key's
int bake_reflection_probe_count(const BakeResult* result);
const BakeReflectionProbe* bake_reflection_probe(const BakeResult* result, int index);
BakeResult* bake_empty(void); // nothing baked yet : for captures alone

uint32_t bake_key(const char* name); // FNV-1a

#ifdef __cplusplus
}
#endif

#endif // _BAKE_H_
