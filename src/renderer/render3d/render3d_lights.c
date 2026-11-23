// Point and spot lights, for both pipelines.
//
// Clustered shading (Olsson et al.) : the view is cut into 16 x 9 tiles across and 24 exponential slices
// in depth, and the CPU lists which lights reach each of those clusters, every frame, as id Tech 6 does.
// A pixel then only loops over its cluster's lights, so a frame can have thousands of them.
//
// Shadows : one depth atlas of 8 x 8 tiles, a tile per spot light and six per point light (a cube). Only
// the most important lights in view get tiles, and a tile is only drawn again when what it sees changed
// (the light, or a caster within its reach, moved) : a static scene costs no shadow rendering at all.

#include "renderer/render3d/render3d_internal.h"

#include <SDL3/SDL.h>
#include <stdio.h>
#include <stdlib.h>

#define CLUSTERS_X 16
#define CLUSTERS_Y 9
#define CLUSTERS_Z 24
#define CLUSTER_COUNT (CLUSTERS_X * CLUSTERS_Y * CLUSTERS_Z)
#define MAX_LIGHT_INDICES (CLUSTER_COUNT * 96)

#define ATLAS_TILES 8 // per side
#define TILE_COUNT (ATLAS_TILES * ATLAS_TILES)
#define TILE_MARGIN_TEXELS 4.0f // cube faces see a little wider than 90 degrees : the filter stays inside

typedef struct
{
    float min[3];
    float max[3];
} Box;

typedef struct
{
    uint32_t owner;      // the light's id, valid if `used`
    int used;
    int face;
    uint64_t signature;  // what it was drawn from ; 0 : must be drawn
    int drawn_this_frame;
    float reach[4];      // its light's position and range : nothing farther casts into it
    R3ShadowTile data;
} Tile;

static GpuPipeline* shadow_pipeline;
static GpuPipeline* shadow_masked_pipeline;
static GpuPipeline* clear_pipeline;

static Texture* atlas;
static int atlas_size;
static Tile tiles[TILE_COUNT];

// The clusters' view-space boxes, for the projection they were computed with.
static Box cluster_boxes[CLUSTER_COUNT];
static float boxes_projection[16];
static float boxes_near;
static float boxes_far;
static int boxes_valid;

static uint32_t cluster_counts[CLUSTER_COUNT];
static uint32_t cluster_offsets[CLUSTER_COUNT];
static uint32_t pair_cluster[MAX_LIGHT_INDICES];
static uint32_t pair_light[MAX_LIGHT_INDICES];

// The lists as the shaders read them, built here then copied whole : frame memory is write-combined,
// and reading it back (filling the lists in place) cost milliseconds.
static uint32_t cluster_lists[CLUSTER_COUNT * 2];
static uint32_t light_indices[MAX_LIGHT_INDICES];
static int pair_count;

static int light_tile[R3_MAX_LIGHTS]; // first tile of each light this frame, -1 : no shadow
static int tiles_to_draw[TILE_COUNT];
static int tiles_to_draw_count;

int r3_lights_create(void)
{
    GpuPipelineDesc desc;
    SDL_zero(desc);
    desc.vertex_shader = "r3_local_shadow_vs";
    desc.depth_format = GPU_FORMAT_D32;
    desc.depth_test = 1;
    desc.depth_write = 1;
    shadow_pipeline = gpu_create_pipeline(&desc);

    desc.vertex_shader = "r3_local_shadow_masked_vs";
    desc.fragment_shader = "r3_shadow_masked_ps";
    shadow_masked_pipeline = gpu_create_pipeline(&desc);

    SDL_zero(desc);
    desc.vertex_shader = "r3_clear_depth_vs";
    desc.depth_format = GPU_FORMAT_D32;
    desc.depth_test = 1;
    desc.depth_always = 1;
    desc.depth_write = 1;
    clear_pipeline = gpu_create_pipeline(&desc);

    boxes_valid = 0;

    return shadow_pipeline && shadow_masked_pipeline && clear_pipeline;
}

void r3_lights_destroy(void)
{
    r3_destroy_target(&atlas);
    atlas_size = 0;

    GpuPipeline** pipelines[3] = { &shadow_pipeline, &shadow_masked_pipeline, &clear_pipeline };

    for (int i = 0; i < 3; i++)
    {
        if (*pipelines[i])
        {
            gpu_destroy_pipeline(*pipelines[i]);
            *pipelines[i] = NULL;
        }
    }
}

// --- Clusters -----------------------------------------------------------------------------------

static float slice_depth(int slice)
{
    return boxes_near * SDL_powf(boxes_far / boxes_near, (float)slice / (float)CLUSTERS_Z);
}

// The view-space point of the line through NDC (x, y) at view depth `depth` (along -Z).
static Vec3 point_at_depth(const Mat4* inverse_projection, float x, float y, float depth)
{
    float a[4];
    float b[4];
    mat4_transform(*inverse_projection, x, y, 0.0f, 1.0f, a);
    mat4_transform(*inverse_projection, x, y, 1.0f, 1.0f, b);

    Vec3 near_point = vec3(a[0] / a[3], a[1] / a[3], a[2] / a[3]);
    Vec3 far_point = vec3(b[0] / b[3], b[1] / b[3], b[2] / b[3]);
    float span = far_point.z - near_point.z;
    float t = SDL_fabsf(span) > 1e-6f ? (-depth - near_point.z) / span : 0.0f;

    return vec3_add(near_point, vec3_scale(vec3_sub(far_point, near_point), t));
}

// Only when the projection changes : the editor's or a game's camera keeps it for many frames.
static void update_cluster_boxes(void)
{
    const Render3DCamera* camera = &r3.camera;

    if (boxes_valid && boxes_near == camera->near_plane && boxes_far == camera->far_plane &&
        SDL_memcmp(boxes_projection, camera->projection.m, sizeof(boxes_projection)) == 0)
    {
        return;
    }

    SDL_memcpy(boxes_projection, camera->projection.m, sizeof(boxes_projection));
    boxes_near = SDL_max(camera->near_plane, 1e-3f);
    boxes_far = SDL_max(camera->far_plane, boxes_near * 2.0f);
    boxes_valid = 1;

    Mat4 inverse_projection = mat4_inverse(camera->projection);

    for (int z = 0; z < CLUSTERS_Z; z++)
    {
        float d0 = slice_depth(z);
        float d1 = slice_depth(z + 1);

        for (int y = 0; y < CLUSTERS_Y; y++)
        {
            // Tiles go down the screen ; NDC Y goes up.
            float y0 = 1.0f - 2.0f * (float)y / (float)CLUSTERS_Y;
            float y1 = 1.0f - 2.0f * (float)(y + 1) / (float)CLUSTERS_Y;

            for (int x = 0; x < CLUSTERS_X; x++)
            {
                float x0 = -1.0f + 2.0f * (float)x / (float)CLUSTERS_X;
                float x1 = -1.0f + 2.0f * (float)(x + 1) / (float)CLUSTERS_X;
                Box* box = &cluster_boxes[(z * CLUSTERS_Y + y) * CLUSTERS_X + x];

                for (int c = 0; c < 3; c++)
                {
                    box->min[c] = 1e30f;
                    box->max[c] = -1e30f;
                }

                for (int corner = 0; corner < 8; corner++)
                {
                    Vec3 p = point_at_depth(&inverse_projection, (corner & 1) ? x1 : x0, (corner & 2) ? y1 : y0, (corner & 4) ? d1 : d0);
                    const float v[3] = { p.x, p.y, p.z };

                    for (int c = 0; c < 3; c++)
                    {
                        box->min[c] = SDL_min(box->min[c], v[c]);
                        box->max[c] = SDL_max(box->max[c], v[c]);
                    }
                }
            }
        }
    }
}

static int slice_of(float depth)
{
    float s = SDL_logf(SDL_max(depth, boxes_near) / boxes_near) / SDL_logf(boxes_far / boxes_near) * (float)CLUSTERS_Z;
    return SDL_clamp((int)s, 0, CLUSTERS_Z - 1);
}

// The light's bounding sphere : for a spot, the one around its cone rather than its whole range.
static void light_sphere(const Render3DLight* light, float out_center[3], float* out_radius)
{
    if (light->type == RENDER3D_LIGHT_SPOT)
    {
        float half = SDL_clamp(light->spot_angle, 1.0f, 179.0f) * 0.5f * SDL_PI_F / 180.0f;
        float c = SDL_cosf(half);
        float along;
        float radius;

        if (half <= SDL_PI_F * 0.25f)
        {
            radius = light->range / (2.0f * c);
            along = radius;
        }
        else
        {
            radius = light->range * SDL_sinf(half);
            along = light->range * c;
        }

        for (int i = 0; i < 3; i++)
        {
            out_center[i] = light->position[i] + light->direction[i] * along;
        }

        *out_radius = radius;
        return;
    }

    SDL_memcpy(out_center, light->position, sizeof(float) * 3);
    *out_radius = light->range;
}

static void add_pair(uint32_t cluster, uint32_t light)
{
    if (pair_count < MAX_LIGHT_INDICES)
    {
        pair_cluster[pair_count] = cluster;
        pair_light[pair_count] = light;
        pair_count++;
        cluster_counts[cluster]++;
    }
}

static void assign_light(int index, const Mat4* view_projection)
{
    float world_center[3];
    float radius;
    light_sphere(&r3.lights[index], world_center, &radius);

    if (!r3_sphere_in_frustum(world_center, radius, view_projection))
    {
        return;
    }

    float view_center4[4];
    mat4_transform(r3.camera.view, world_center[0], world_center[1], world_center[2], 1.0f, view_center4);
    const float c[3] = { view_center4[0], view_center4[1], view_center4[2] };
    float depth = -c[2];

    if (depth + radius < boxes_near || depth - radius > boxes_far)
    {
        return;
    }

    int z0 = slice_of(depth - radius);
    int z1 = slice_of(depth + radius);

    // The tiles its box covers on screen ; all of them if it reaches behind the camera.
    int x0 = 0, x1 = CLUSTERS_X - 1, y0 = 0, y1 = CLUSTERS_Y - 1;

    if (depth - radius > boxes_near)
    {
        float lo[2] = { 1e30f, 1e30f };
        float hi[2] = { -1e30f, -1e30f };

        for (int corner = 0; corner < 8; corner++)
        {
            float clip[4];
            mat4_transform(r3.camera.projection, c[0] + ((corner & 1) ? radius : -radius), c[1] + ((corner & 2) ? radius : -radius),
                c[2] + ((corner & 4) ? radius : -radius), 1.0f, clip);
            float w = SDL_max(clip[3], 1e-6f);

            for (int a = 0; a < 2; a++)
            {
                lo[a] = SDL_min(lo[a], clip[a] / w);
                hi[a] = SDL_max(hi[a], clip[a] / w);
            }
        }

        x0 = SDL_clamp((int)SDL_floorf((lo[0] * 0.5f + 0.5f) * CLUSTERS_X), 0, CLUSTERS_X - 1);
        x1 = SDL_clamp((int)SDL_floorf((hi[0] * 0.5f + 0.5f) * CLUSTERS_X), 0, CLUSTERS_X - 1);
        y0 = SDL_clamp((int)SDL_floorf((0.5f - hi[1] * 0.5f) * CLUSTERS_Y), 0, CLUSTERS_Y - 1);
        y1 = SDL_clamp((int)SDL_floorf((0.5f - lo[1] * 0.5f) * CLUSTERS_Y), 0, CLUSTERS_Y - 1);
    }

    float radius2 = radius * radius;

    for (int z = z0; z <= z1; z++)
    {
        for (int y = y0; y <= y1; y++)
        {
            for (int x = x0; x <= x1; x++)
            {
                uint32_t cluster = (uint32_t)((z * CLUSTERS_Y + y) * CLUSTERS_X + x);
                const Box* box = &cluster_boxes[cluster];
                float d2 = 0.0f;

                for (int a = 0; a < 3; a++)
                {
                    float v = c[a] < box->min[a] ? box->min[a] - c[a] : c[a] > box->max[a] ? c[a] - box->max[a] : 0.0f;
                    d2 += v * v;
                }

                if (d2 <= radius2)
                {
                    add_pair(cluster, (uint32_t)index);
                }
            }
        }
    }
}

// --- Shadow tiles -----------------------------------------------------------------------------

static int ensure_atlas(void)
{
    if (atlas && atlas_size == r3.quality.local_shadow_atlas)
    {
        return 1;
    }

    wait_renderer_idle();
    r3_destroy_target(&atlas);

    atlas_size = r3.quality.local_shadow_atlas;
    atlas = r3_create_target(atlas_size, atlas_size, GPU_FORMAT_D32, GPU_USAGE_DEPTH_TARGET);
    SDL_zeroa(tiles); // every tile must be drawn again

    if (!atlas)
    {
        printf("Could not create the local lights' shadow atlas (%d).\n", atlas_size);
        atlas_size = 0;
    }

    return atlas != NULL;
}

// FNV-1a, a 32-bit word at a time (everything hashed is made of them).
static uint64_t hash_words(uint64_t hash, const void* data, size_t size)
{
    const uint32_t* words = (const uint32_t*)data;

    for (size_t i = 0; i < size / 4; i++)
    {
        hash = (hash ^ words[i]) * 1099511628211ull;
    }

    return hash;
}

// Whether a draw is close enough to cast into a tile : its sphere against the light's (a frustum alone
// would reach far beyond the light's range). Unbounded draws always are.
static int draw_in_reach(const R3Draw* draw, const float reach[4])
{
    if (draw->radius < 0.0f)
    {
        return 1;
    }

    float dx = draw->center[0] - reach[0];
    float dy = draw->center[1] - reach[1];
    float dz = draw->center[2] - reach[2];
    float limit = reach[3] + draw->radius;

    return dx * dx + dy * dy + dz * dz < limit * limit;
}

static int draw_casts_into(const R3Draw* draw, const float reach[4], const float planes[5][4])
{
    return draw_in_reach(draw, reach) && (draw->radius < 0.0f || r3_sphere_in_planes(planes, draw->center, draw->radius));
}

// A tile's view-projection : the spot's cone, or one face of the point light's cube.
static Mat4 tile_matrix(const Render3DLight* light, int face, float* out_texel_at_1m)
{
    static const float directions[6][3] = { { 1, 0, 0 }, { -1, 0, 0 }, { 0, 1, 0 }, { 0, -1, 0 }, { 0, 0, 1 }, { 0, 0, -1 } };
    static const float ups[6][3] = { { 0, 1, 0 }, { 0, 1, 0 }, { 0, 0, 1 }, { 0, 0, 1 }, { 0, 1, 0 }, { 0, 1, 0 } };

    int tile_size = atlas_size / ATLAS_TILES;
    Vec3 eye = vec3(light->position[0], light->position[1], light->position[2]);
    Vec3 direction;
    Vec3 up;
    float tan_half;

    if (light->type == RENDER3D_LIGHT_SPOT)
    {
        direction = vec3_normalize(vec3(light->direction[0], light->direction[1], light->direction[2]));
        up = SDL_fabsf(direction.y) > 0.99f ? vec3(0.0f, 0.0f, 1.0f) : vec3(0.0f, 1.0f, 0.0f);
        float half = SDL_clamp(light->spot_angle, 1.0f, 170.0f) * 0.5f * SDL_PI_F / 180.0f;
        tan_half = SDL_tanf(half) * (1.0f + 2.0f * TILE_MARGIN_TEXELS / (float)tile_size);
    }
    else
    {
        direction = vec3(directions[face][0], directions[face][1], directions[face][2]);
        up = vec3(ups[face][0], ups[face][1], ups[face][2]);
        tan_half = 1.0f + 2.0f * TILE_MARGIN_TEXELS / (float)tile_size;
    }

    float near_plane = SDL_max(0.02f, light->range * 0.002f);
    Mat4 view = mat4_look_at(eye, vec3_add(eye, direction), up);
    Mat4 projection = mat4_perspective(2.0f * SDL_atanf(tan_half), 1.0f, near_plane, light->range);

    *out_texel_at_1m = 2.0f * tan_half / (float)tile_size;

    return mat4_multiply(projection, view);
}

static float light_importance(const Render3DLight* light)
{
    float dx = light->position[0] - r3.camera.eye.x;
    float dy = light->position[1] - r3.camera.eye.y;
    float dz = light->position[2] - r3.camera.eye.z;
    float distance = SDL_max(SDL_sqrtf(dx * dx + dy * dy + dz * dz) - light->range * 0.5f, 1.0f);

    return light->range * light->intensity / distance;
}

static int compare_importance(const void* a, const void* b)
{
    float ia = light_importance(&r3.lights[*(const int*)a]);
    float ib = light_importance(&r3.lights[*(const int*)b]);
    return ia > ib ? -1 : ia < ib ? 1 : 0;
}

// What a tile depends on : its matrix, and every caster it sees. Same signature, same depth : not drawn.
static uint64_t tile_signature(const Mat4* matrix, const float reach[4], int* out_has_casters)
{
    uint64_t hash = hash_words(1469598103934665603ull, matrix->m, sizeof(matrix->m));
    *out_has_casters = 0;

    float planes[5][4];
    r3_frustum_planes(matrix, planes);

    for (int i = 0; i < r3.draw_count; i++)
    {
        const R3Draw* draw = &r3.draws[i];

        if (!draw_casts_into(draw, reach, planes))
        {
            continue;
        }

        hash = hash_words(hash, &draw->mesh, sizeof(draw->mesh));
        hash = hash_words(hash, &draw->first_index, sizeof(int) * 2);
        hash = hash_words(hash, r3.instances[i].model, sizeof(r3.instances[i].model));
        hash = hash_words(hash, &r3.instances[i].textures[0], sizeof(uint32_t));
        *out_has_casters = 1;
    }

    return hash | 1u; // never 0, the "must be drawn" mark
}

static void allocate_shadows(const Mat4* view_projection)
{
    static int candidates[R3_MAX_LIGHTS];
    int candidate_count = 0;

    for (int i = 0; i < r3.light_count; i++)
    {
        light_tile[i] = -1;

        const Render3DLight* light = &r3.lights[i];
        float center[3];
        float radius;
        light_sphere(light, center, &radius);

        if (light->cast_shadows && r3_sphere_in_frustum(center, radius, view_projection))
        {
            candidates[candidate_count++] = i;
        }
    }

    qsort(candidates, (size_t)candidate_count, sizeof(int), compare_importance);
    candidate_count = SDL_min(candidate_count, r3.quality.local_shadow_lights);

    int claimed[TILE_COUNT];
    SDL_zeroa(claimed);
    int pending[R3_MAX_LIGHTS];
    int pending_count = 0;

    // First, the lights keeping last frame's tiles.
    for (int c = 0; c < candidate_count; c++)
    {
        int light = candidates[c];
        int faces = r3.lights[light].type == RENDER3D_LIGHT_SPOT ? 1 : 6;
        int found = -1;

        for (int t = 0; t + faces <= TILE_COUNT; t++)
        {
            if (tiles[t].used && tiles[t].owner == r3.lights[light].id && tiles[t].face == 0)
            {
                found = t;

                for (int f = 1; f < faces; f++)
                {
                    if (!tiles[t + f].used || tiles[t + f].owner != tiles[t].owner || tiles[t + f].face != f)
                    {
                        found = -1;
                    }
                }

                break;
            }
        }

        if (found >= 0)
        {
            light_tile[light] = found;

            for (int f = 0; f < faces; f++)
            {
                claimed[found + f] = 1;
            }
        }
        else
        {
            pending[pending_count++] = light;
        }
    }

    // Then the others, in free runs of tiles : a cube needs six in a row.
    for (int p = 0; p < pending_count; p++)
    {
        int light = pending[p];
        int faces = r3.lights[light].type == RENDER3D_LIGHT_SPOT ? 1 : 6;

        for (int t = 0; t + faces <= TILE_COUNT; t++)
        {
            int free = 1;

            for (int f = 0; f < faces && free; f++)
            {
                free = !claimed[t + f];
            }

            if (free)
            {
                light_tile[light] = t;

                for (int f = 0; f < faces; f++)
                {
                    claimed[t + f] = 1;
                    tiles[t + f].used = 1;
                    tiles[t + f].owner = r3.lights[light].id;
                    tiles[t + f].face = f;
                    tiles[t + f].signature = 0;
                }

                break;
            }
        }
    }

    // Tiles nobody claimed are free again.
    for (int t = 0; t < TILE_COUNT; t++)
    {
        if (!claimed[t])
        {
            tiles[t].used = 0;
        }
    }

    // Each claimed tile : its matrix, and whether it must be drawn.
    tiles_to_draw_count = 0;
    int tile_size = atlas_size / ATLAS_TILES;

    for (int i = 0; i < r3.light_count; i++)
    {
        if (light_tile[i] < 0)
        {
            continue;
        }

        int faces = r3.lights[i].type == RENDER3D_LIGHT_SPOT ? 1 : 6;

        for (int f = 0; f < faces; f++)
        {
            int t = light_tile[i] + f;
            float texel_at_1m = 0.0f;
            Mat4 matrix = tile_matrix(&r3.lights[i], f, &texel_at_1m);

            R3ShadowTile* data = &tiles[t].data;
            SDL_memcpy(data->view_projection, matrix.m, sizeof(data->view_projection));
            data->rect[0] = (float)((t % ATLAS_TILES) * tile_size) / (float)atlas_size;
            data->rect[1] = (float)((t / ATLAS_TILES) * tile_size) / (float)atlas_size;
            data->rect[2] = (float)tile_size / (float)atlas_size;
            data->rect[3] = (float)tile_size / (float)atlas_size;
            data->params[0] = texel_at_1m;

            SDL_memcpy(tiles[t].reach, r3.lights[i].position, sizeof(float) * 3);
            tiles[t].reach[3] = r3.lights[i].range;

            int has_casters = 0;
            uint64_t signature = tile_signature(&matrix, tiles[t].reach, &has_casters);

            if (signature != tiles[t].signature)
            {
                tiles[t].signature = signature;
                tiles_to_draw[tiles_to_draw_count++] = t;
            }
        }
    }
}

// --- Per frame --------------------------------------------------------------------------------

int r3_lights_prepare(R3Frame* frame, const Mat4* view_projection)
{
    update_cluster_boxes();

    // Shadows first : the atlas may have to be (re)created, which waits for the GPU.
    tiles_to_draw_count = 0;

    if (r3.quality.local_shadow_lights > 0 && r3.light_count > 0)
    {
        if (!ensure_atlas())
        {
            return 0;
        }

        allocate_shadows(view_projection);
    }
    else
    {
        for (int i = 0; i < r3.light_count; i++)
        {
            light_tile[i] = -1;
        }
    }

    // Clusters : every (cluster, light) pair, then sorted by cluster.
    SDL_zeroa(cluster_counts);
    pair_count = 0;

    for (int i = 0; i < r3.light_count; i++)
    {
        assign_light(i, view_projection);
    }

    uint32_t offset = 0;

    for (int c = 0; c < CLUSTER_COUNT; c++)
    {
        cluster_offsets[c] = offset;
        offset += cluster_counts[c];
    }

    GpuFrameAllocation lights_memory = gpu_frame_alloc(sizeof(R3Light) * (size_t)SDL_max(1, r3.light_count));
    GpuFrameAllocation clusters_memory = gpu_frame_alloc(sizeof(uint32_t) * 2 * CLUSTER_COUNT);
    GpuFrameAllocation indices_memory = gpu_frame_alloc(sizeof(uint32_t) * (size_t)SDL_max(1, pair_count));
    GpuFrameAllocation tiles_memory = gpu_frame_alloc(sizeof(R3ShadowTile) * TILE_COUNT);

    if (!lights_memory.mapped || !clusters_memory.mapped || !indices_memory.mapped || !tiles_memory.mapped)
    {
        printf("Out of frame memory for the lights.\n");
        return 0;
    }

    for (int c = 0; c < CLUSTER_COUNT; c++)
    {
        cluster_lists[c * 2 + 0] = cluster_offsets[c];
        cluster_lists[c * 2 + 1] = 0;
    }

    for (int p = 0; p < pair_count; p++)
    {
        uint32_t c = pair_cluster[p];
        light_indices[cluster_lists[c * 2 + 0] + cluster_lists[c * 2 + 1]] = pair_light[p];
        cluster_lists[c * 2 + 1]++;
    }

    SDL_memcpy(clusters_memory.mapped, cluster_lists, sizeof(uint32_t) * 2 * CLUSTER_COUNT);
    SDL_memcpy(indices_memory.mapped, light_indices, sizeof(uint32_t) * (size_t)pair_count);

    R3Light* lights = lights_memory.mapped;

    for (int i = 0; i < r3.light_count; i++)
    {
        const Render3DLight* source = &r3.lights[i];
        R3Light built; // written whole, never read back from frame memory
        R3Light* light = &built;
        float intensity = source->intensity;

        light->position_range[0] = source->position[0];
        light->position_range[1] = source->position[1];
        light->position_range[2] = source->position[2];
        light->position_range[3] = source->range;
        light->color[0] = source->color[0] * intensity;
        light->color[1] = source->color[1] * intensity;
        light->color[2] = source->color[2] * intensity;
        light->color[3] = 1.0f / (source->range * source->range);

        Vec3 direction = vec3_normalize(vec3(source->direction[0], source->direction[1], source->direction[2]));
        light->direction_type[0] = direction.x;
        light->direction_type[1] = direction.y;
        light->direction_type[2] = direction.z;
        light->direction_type[3] = source->type == RENDER3D_LIGHT_SPOT ? 1.0f : 0.0f;

        // The cone's edge (Frostbite) : saturate(cos * scale + offset), from the outer angle to the inner one.
        float outer = SDL_clamp(source->spot_angle, 1.0f, 179.0f) * 0.5f * SDL_PI_F / 180.0f;
        float inner = outer * (1.0f - SDL_clamp(source->spot_softness, 0.0f, 1.0f));
        float cos_outer = SDL_cosf(outer);
        float cos_inner = SDL_max(SDL_cosf(inner), cos_outer + 1e-4f);
        light->spot_shadow[0] = 1.0f / (cos_inner - cos_outer);
        light->spot_shadow[1] = -cos_outer * light->spot_shadow[0];
        light->spot_shadow[2] = (float)light_tile[i];
        light->spot_shadow[3] = 0.0f;
        lights[i] = built;
    }

    R3ShadowTile* tile_data = tiles_memory.mapped;

    for (int t = 0; t < TILE_COUNT; t++)
    {
        tile_data[t] = tiles[t].data;
    }

    frame->cluster_params[0] = (float)CLUSTERS_X / (float)r3.width;
    frame->cluster_params[1] = (float)CLUSTERS_Y / (float)r3.height;
    frame->cluster_params[2] = (float)CLUSTERS_Z / SDL_logf(boxes_far / boxes_near);
    frame->cluster_params[3] = -SDL_logf(boxes_near) * frame->cluster_params[2];
    frame->cluster_grid[0] = CLUSTERS_X;
    frame->cluster_grid[1] = CLUSTERS_Y;
    frame->cluster_grid[2] = CLUSTERS_Z;
    frame->cluster_grid[3] = (uint32_t)r3.light_count;

    // Every list lives in the same frame buffer as the frame data itself : g_buffers[ids.x].
    frame->light_data[0] = lights_memory.offset;
    frame->light_data[1] = clusters_memory.offset;
    frame->light_data[2] = indices_memory.offset;
    frame->light_data[3] = tiles_memory.offset;
    frame->local_shadows[0] = atlas ? gpu_texture_index(atlas) : 0;
    frame->local_shadows[1] = (uint32_t)SDL_clamp(r3.quality.shadow_samples / 2, 4, 16);

    return 1;
}

void r3_lights_render_shadows(R3PassConstants* constants)
{
    if (tiles_to_draw_count == 0 || !atlas)
    {
        return;
    }

    GpuPassDesc pass;
    SDL_zero(pass);
    pass.depth = atlas;
    pass.depth_load = GPU_LOAD_KEEP;

    gpu_begin_pass(&pass);

    int tile_size = atlas_size / ATLAS_TILES;

    for (int n = 0; n < tiles_to_draw_count; n++)
    {
        int t = tiles_to_draw[n];
        Mat4 matrix;
        SDL_memcpy(matrix.m, tiles[t].data.view_projection, sizeof(matrix.m));

        gpu_set_viewport((t % ATLAS_TILES) * tile_size, (t / ATLAS_TILES) * tile_size, tile_size, tile_size);

        float planes[5][4];
        r3_frustum_planes(&matrix, planes);

        gpu_bind_pipeline(clear_pipeline);
        gpu_push_constants(constants, sizeof(*constants));
        gpu_draw(3);

        constants->inputs[0] = (uint32_t)t;

        for (int masked = 0; masked <= 1; masked++)
        {
            gpu_bind_pipeline(masked ? shadow_masked_pipeline : shadow_pipeline);

            for (int i = 0; i < r3.draw_count; i++)
            {
                const R3Draw* draw = &r3.draws[i];

                if (r3_is_masked(i) == masked && draw_casts_into(draw, tiles[t].reach, planes))
                {
                    constants->ids[3] = (uint32_t)i;
                    gpu_push_constants(constants, sizeof(*constants));
                    gpu_draw_indexed(draw->mesh, draw->first_index, draw->index_count);
                }
            }
        }
    }

    gpu_end_pass();
}
