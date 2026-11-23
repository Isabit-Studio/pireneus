// Baked lighting and visibility (bake.h) : a BVH over the static triangles, then path tracing, on every
// core, for the lightmaps' texels and the probes, and visibility rays for occlusion culling.

#include "bake/bake.h"

#include <SDL3/SDL.h>
#include <stdio.h>
#include <stdlib.h>

#include "renderer/render3d/half_float.h"
#include "debug.h"

#define PI_F 3.14159265f
#define LEAF_SIZE 4
#define CHART_PADDING 2       // texels around each chart : bilinear filtering never reads another one
#define BRICK_PROBES 125      // 5 x 5 x 5
#define PROBE_BYTES 64
#define NO_BRICK 0xFFFFFFFFu
#define MAX_CELLS (96 * 96 * 96)
#define CULLING_RAYS 48
#define FILE_MAGIC "PIRBAKE1"

// --- Small vector helpers ------------------------------------------------------------------------

typedef struct
{
    float x, y, z;
} V3;

static V3 v3(float x, float y, float z) { V3 r = { x, y, z }; return r; }
static V3 add(V3 a, V3 b) { return v3(a.x + b.x, a.y + b.y, a.z + b.z); }
static V3 sub(V3 a, V3 b) { return v3(a.x - b.x, a.y - b.y, a.z - b.z); }
static V3 mul(V3 a, float s) { return v3(a.x * s, a.y * s, a.z * s); }
static V3 hadamard(V3 a, V3 b) { return v3(a.x * b.x, a.y * b.y, a.z * b.z); }
static float dot(V3 a, V3 b) { return a.x * b.x + a.y * b.y + a.z * b.z; }
static V3 cross(V3 a, V3 b) { return v3(a.y * b.z - a.z * b.y, a.z * b.x - a.x * b.z, a.x * b.y - a.y * b.x); }
static float length(V3 a) { return SDL_sqrtf(dot(a, a)); }
static V3 normalize(V3 a) { float l = length(a); return l > 1e-12f ? mul(a, 1.0f / l) : v3(0.0f, 1.0f, 0.0f); }
static float component(V3 a, int axis) { return axis == 0 ? a.x : axis == 1 ? a.y : a.z; }

static V3 transform_point(const float m[16], const float p[3])
{
    return v3(m[0] * p[0] + m[1] * p[1] + m[2] * p[2] + m[3],
        m[4] * p[0] + m[5] * p[1] + m[6] * p[2] + m[7],
        m[8] * p[0] + m[9] * p[1] + m[10] * p[2] + m[11]);
}

// PCG32 (O'Neill) : one per thread.
typedef struct
{
    uint64_t state;
} Random;

static uint32_t random_next(Random* r)
{
    uint64_t old = r->state;
    r->state = old * 6364136223846793005ull + 1442695040888963407ull;
    uint32_t shifted = (uint32_t)(((old >> 18u) ^ old) >> 27u);
    uint32_t rotation = (uint32_t)(old >> 59u);
    return (shifted >> rotation) | (shifted << ((32u - rotation) & 31u));
}

static float random_float(Random* r)
{
    return (float)(random_next(r) >> 8) * (1.0f / 16777216.0f);
}

static Random random_seed(uint64_t seed)
{
    Random r = { seed * 0x9E3779B97F4A7C15ull + 1 };
    random_next(&r);
    return r;
}

// An orthonormal basis around n (Duff et al.).
static void basis(V3 n, V3* t, V3* b)
{
    float sign = n.z >= 0.0f ? 1.0f : -1.0f;
    float a = -1.0f / (sign + n.z);
    float c = n.x * n.y * a;
    *t = v3(1.0f + sign * n.x * n.x * a, sign * c, -sign * n.x);
    *b = v3(c, sign + n.y * n.y * a, -n.y);
}

static V3 cosine_sample(V3 n, Random* r)
{
    float u1 = random_float(r);
    float u2 = random_float(r);
    float radius = SDL_sqrtf(u1);
    float phi = 2.0f * PI_F * u2;
    V3 t, b;
    basis(n, &t, &b);
    return normalize(add(add(mul(t, radius * SDL_cosf(phi)), mul(b, radius * SDL_sinf(phi))), mul(n, SDL_sqrtf(SDL_max(0.0f, 1.0f - u1)))));
}

// --- The scene's triangles and their BVH ---------------------------------------------------------

typedef struct
{
    V3 v0, e1, e2;
    V3 normal;          // geometric, unit, from the winding (outside)
    V3 albedo[3];
    V3 emissive;
    int object;
    int occludes;
} Triangle;

typedef struct
{
    float min[3];
    float max[3];
    int first;          // leaf : first triangle ; inner : left child (right = left + 1)
    int count;          // 0 : inner node
} Node;

typedef struct
{
    float t;
    float u, v;
    int triangle;
} Hit;

typedef struct
{
    Triangle* triangles;
    int triangle_count;
    Node* nodes;
    int node_count;
    BakeLight* lights;
    int light_count;
    BakeSettings settings;
} Scene;

static void triangle_bounds(const Triangle* t, float out_min[3], float out_max[3])
{
    V3 a = t->v0;
    V3 b = add(t->v0, t->e1);
    V3 c = add(t->v0, t->e2);

    for (int axis = 0; axis < 3; axis++)
    {
        float pa = component(a, axis), pb = component(b, axis), pc = component(c, axis);
        out_min[axis] = SDL_min(pa, SDL_min(pb, pc));
        out_max[axis] = SDL_max(pa, SDL_max(pb, pc));
    }
}

static float centroid(const Triangle* t, int axis)
{
    return component(t->v0, axis) + (component(t->e1, axis) + component(t->e2, axis)) / 3.0f;
}

static int build_node(Scene* scene, int node_index, int first, int count)
{
    Node* node = &scene->nodes[node_index];

    for (int axis = 0; axis < 3; axis++)
    {
        node->min[axis] = 1e30f;
        node->max[axis] = -1e30f;
    }

    float centroid_min[3] = { 1e30f, 1e30f, 1e30f };
    float centroid_max[3] = { -1e30f, -1e30f, -1e30f };

    for (int i = first; i < first + count; i++)
    {
        float lo[3], hi[3];
        triangle_bounds(&scene->triangles[i], lo, hi);

        for (int axis = 0; axis < 3; axis++)
        {
            node->min[axis] = SDL_min(node->min[axis], lo[axis]);
            node->max[axis] = SDL_max(node->max[axis], hi[axis]);
            float c = centroid(&scene->triangles[i], axis);
            centroid_min[axis] = SDL_min(centroid_min[axis], c);
            centroid_max[axis] = SDL_max(centroid_max[axis], c);
        }
    }

    if (count <= LEAF_SIZE)
    {
        node->first = first;
        node->count = count;
        return 1;
    }

    // Split at the middle of the centroids' widest axis (and at the median if that splits nothing).
    int axis = 0;

    for (int a = 1; a < 3; a++)
    {
        if (centroid_max[a] - centroid_min[a] > centroid_max[axis] - centroid_min[axis])
        {
            axis = a;
        }
    }

    float split = (centroid_min[axis] + centroid_max[axis]) * 0.5f;
    int middle = first;

    for (int i = first; i < first + count; i++)
    {
        if (centroid(&scene->triangles[i], axis) < split)
        {
            Triangle swap = scene->triangles[i];
            scene->triangles[i] = scene->triangles[middle];
            scene->triangles[middle] = swap;
            middle++;
        }
    }

    if (middle == first || middle == first + count)
    {
        middle = first + count / 2;
    }

    int left = scene->node_count;
    scene->node_count += 2;
    node->first = left;
    node->count = 0;

    return build_node(scene, left, first, middle - first) && build_node(scene, left + 1, middle, first + count - middle);
}

static int ray_box(V3 origin, V3 inverse, const Node* node, float max_t, float* out_t)
{
    float t0 = 0.0f;
    float t1 = max_t;

    for (int axis = 0; axis < 3; axis++)
    {
        float o = component(origin, axis);
        float inv = component(inverse, axis);
        float a = (node->min[axis] - o) * inv;
        float b = (node->max[axis] - o) * inv;
        t0 = SDL_max(t0, SDL_min(a, b));
        t1 = SDL_min(t1, SDL_max(a, b));
    }

    *out_t = t0;
    return t0 <= t1;
}

// Möller-Trumbore, both sides.
static int ray_triangle(V3 origin, V3 direction, const Triangle* t, float max_t, Hit* hit)
{
    V3 p = cross(direction, t->e2);
    float det = dot(t->e1, p);

    if (SDL_fabsf(det) < 1e-12f)
    {
        return 0;
    }

    float inv = 1.0f / det;
    V3 s = sub(origin, t->v0);
    float u = dot(s, p) * inv;

    if (u < 0.0f || u > 1.0f)
    {
        return 0;
    }

    V3 q = cross(s, t->e1);
    float v = dot(direction, q) * inv;

    if (v < 0.0f || u + v > 1.0f)
    {
        return 0;
    }

    float distance = dot(t->e2, q) * inv;

    if (distance <= 1e-5f || distance >= max_t)
    {
        return 0;
    }

    hit->t = distance;
    hit->u = u;
    hit->v = v;
    return 1;
}

// The closest hit, or with `any`, whether something that occludes is closer than max_t.
static int trace(const Scene* scene, V3 origin, V3 direction, float max_t, int any, Hit* out_hit)
{
    if (scene->node_count == 0)
    {
        return 0;
    }

    V3 inverse = v3(1.0f / (SDL_fabsf(direction.x) > 1e-12f ? direction.x : 1e-12f),
        1.0f / (SDL_fabsf(direction.y) > 1e-12f ? direction.y : 1e-12f),
        1.0f / (SDL_fabsf(direction.z) > 1e-12f ? direction.z : 1e-12f));

    int stack[96];
    int top = 0;
    stack[top++] = 0;
    int found = 0;
    Hit best;
    best.t = max_t;
    best.triangle = -1;

    while (top > 0)
    {
        const Node* node = &scene->nodes[stack[--top]];
        float entry;

        if (!ray_box(origin, inverse, node, best.t, &entry))
        {
            continue;
        }

        if (node->count > 0)
        {
            for (int i = node->first; i < node->first + node->count; i++)
            {
                const Triangle* t = &scene->triangles[i];

                if (any && !t->occludes)
                {
                    continue;
                }

                Hit hit;

                if (ray_triangle(origin, direction, t, best.t, &hit))
                {
                    if (any)
                    {
                        return 1;
                    }

                    best = hit;
                    best.triangle = i;
                    found = 1;
                }
            }

            continue;
        }

        // The nearer child is visited first.
        float t_left, t_right;
        int left_hit = ray_box(origin, inverse, &scene->nodes[node->first], best.t, &t_left);
        int right_hit = ray_box(origin, inverse, &scene->nodes[node->first + 1], best.t, &t_right);

        if (top + 2 > (int)SDL_arraysize(stack))
        {
            continue;
        }

        if (left_hit && right_hit)
        {
            stack[top++] = t_left < t_right ? node->first + 1 : node->first;
            stack[top++] = t_left < t_right ? node->first : node->first + 1;
        }
        else if (left_hit)
        {
            stack[top++] = node->first;
        }
        else if (right_hit)
        {
            stack[top++] = node->first + 1;
        }
    }

    if (found && out_hit)
    {
        *out_hit = best;
    }

    return found;
}

// --- Lighting -----------------------------------------------------------------------------------

static V3 sky_radiance(const Scene* scene, V3 n)
{
    const float (*sh)[3] = scene->settings.sky_sh;
    float basis_values[9] = {
        0.282095f, 0.488603f * n.y, 0.488603f * n.z, 0.488603f * n.x,
        1.092548f * n.x * n.y, 1.092548f * n.y * n.z, 0.315392f * (3.0f * n.z * n.z - 1.0f),
        1.092548f * n.x * n.z, 0.546274f * (n.x * n.x - n.y * n.y)
    };

    float c[3] = { 0.0f, 0.0f, 0.0f };

    for (int i = 0; i < 9; i++)
    {
        for (int k = 0; k < 3; k++)
        {
            c[k] += sh[i][k] * basis_values[i];
        }
    }

    float s = scene->settings.sky_intensity;
    return v3(SDL_max(0.0f, c[0]) * s, SDL_max(0.0f, c[1]) * s, SDL_max(0.0f, c[2]) * s);
}

// A light's irradiance at p, at normal incidence, its direction L (toward it), shadowed.
static int light_at(const Scene* scene, const BakeLight* light, V3 p, V3* out_irradiance, V3* out_L)
{
    V3 color = v3(light->color[0], light->color[1], light->color[2]);

    if (light->type == BAKE_LIGHT_DIRECTIONAL)
    {
        V3 L = normalize(mul(v3(light->direction[0], light->direction[1], light->direction[2]), -1.0f));

        if (trace(scene, p, L, 1e30f, 1, NULL))
        {
            return 0;
        }

        *out_irradiance = color;
        *out_L = L;
        return 1;
    }

    V3 to_light = sub(v3(light->position[0], light->position[1], light->position[2]), p);
    float d2 = dot(to_light, to_light);
    float range2 = light->range * light->range;

    if (d2 >= range2)
    {
        return 0;
    }

    float distance = SDL_sqrtf(d2);
    V3 L = mul(to_light, 1.0f / SDL_max(distance, 1e-4f));
    float window = 1.0f - (d2 * d2) / (range2 * range2);
    float attenuation = window * window / SDL_max(d2, 1e-4f);

    if (light->type == BAKE_LIGHT_SPOT)
    {
        float outer = SDL_clamp(light->spot_angle, 1.0f, 179.0f) * 0.5f * PI_F / 180.0f;
        float inner = outer * (1.0f - SDL_clamp(light->spot_softness, 0.0f, 1.0f));
        float cos_outer = SDL_cosf(outer);
        float cos_inner = SDL_max(SDL_cosf(inner), cos_outer + 1e-4f);
        V3 axis = normalize(v3(light->direction[0], light->direction[1], light->direction[2]));
        float cone = SDL_clamp((dot(mul(L, -1.0f), axis) - cos_outer) / (cos_inner - cos_outer), 0.0f, 1.0f);
        attenuation *= cone * cone;
    }

    if (attenuation <= 0.0f || trace(scene, p, L, distance - 1e-3f, 1, NULL))
    {
        return 0;
    }

    *out_irradiance = mul(color, attenuation);
    *out_L = L;
    return 1;
}

// Irradiance / PI from the lights, on a surface : all of them, or only the baked ones.
static V3 direct_light(const Scene* scene, V3 p, V3 n, int baked_only)
{
    V3 total = v3(0.0f, 0.0f, 0.0f);
    V3 origin = add(p, mul(n, 2e-3f));

    for (int i = 0; i < scene->light_count; i++)
    {
        const BakeLight* light = &scene->lights[i];
        V3 irradiance, L;

        if ((baked_only && !light->baked) || !light_at(scene, light, origin, &irradiance, &L))
        {
            continue;
        }

        float n_dot_l = dot(n, L);

        if (n_dot_l > 0.0f)
        {
            total = add(total, mul(irradiance, n_dot_l / PI_F));
        }
    }

    return total;
}

// The radiance coming back along a ray : the sky, or what a diffuse surface sends back, its own direct
// light and, with bounces left, one more sample of its indirect light. `backface` : it hit the inside.
static V3 radiance(const Scene* scene, V3 origin, V3 direction, int bounces, Random* random, int* backface)
{
    Hit hit;

    if (!trace(scene, origin, direction, 1e30f, 0, &hit))
    {
        return sky_radiance(scene, direction);
    }

    const Triangle* t = &scene->triangles[hit.triangle];
    V3 n = t->normal;

    if (dot(n, direction) > 0.0f)
    {
        if (backface)
        {
            *backface = 1;
        }

        return v3(0.0f, 0.0f, 0.0f);
    }

    float w = 1.0f - hit.u - hit.v;
    V3 albedo = add(add(mul(t->albedo[0], w), mul(t->albedo[1], hit.u)), mul(t->albedo[2], hit.v));
    V3 p = add(origin, mul(direction, hit.t));
    V3 light = direct_light(scene, p, n, 0);

    if (bounces > 0)
    {
        V3 next = cosine_sample(n, random);
        light = add(light, radiance(scene, add(p, mul(n, 2e-3f)), next, bounces - 1, random, NULL));
    }

    return add(t->emissive, hadamard(albedo, light));
}

// --- Parallel work ------------------------------------------------------------------------------

typedef void (*WorkFunction)(void* context, int index, Random* random);

typedef struct
{
    WorkFunction function;
    void* context;
    int count;
    SDL_AtomicInt next;
    SDL_AtomicInt done;
    const SDL_AtomicInt* cancel;
} Work;

static int SDLCALL worker(void* data)
{
    Work* work = (Work*)data;
    Random random = random_seed((uint64_t)(uintptr_t)SDL_GetCurrentThreadID());

    for (;;)
    {
        if (SDL_GetAtomicInt((SDL_AtomicInt*)work->cancel))
        {
            break;
        }

        int index = SDL_AddAtomicInt(&work->next, 1);

        if (index >= work->count)
        {
            break;
        }

        random.state ^= (uint64_t)index * 0x9E3779B97F4A7C15ull;
        work->function(work->context, index, &random);
        SDL_AddAtomicInt(&work->done, 1);
    }

    return 0;
}

// --- Jobs -----------------------------------------------------------------------------------------

typedef struct
{
    int page;
    int x, y;
    float position[3];
    float normal[3];
} Texel;

typedef struct
{
    uint32_t key;
    int vertex_count;
    int page;           // -1 : no lightmap
    float* uvs;         // vertex_count * 2
} ObjectEntry;

struct BakeResult
{
    int page_size;
    int page_count;
    uint16_t* pages;    // page_count * page_size^2 * 4 halves

    ObjectEntry* objects;
    int object_count;

    int has_probes;
    BakeProbeVolume probes;
    uint8_t* probe_data;

    int has_culling;
    float culling_origin[3];
    float culling_cell;
    int culling_cells[3];
    int culling_objects;      // bits per cell, in the objects' order
    uint8_t* culling_bits;    // per cell : (culling_objects + 7) / 8 bytes

    BakeReflectionProbe* reflection_probes; // their pixels owned
    int reflection_probe_count;
};

struct BakeJob
{
    Scene scene;
    BakeMesh* meshes;   // copies
    int mesh_count;
    SDL_Thread* thread;
    SDL_AtomicInt cancel;
    SDL_AtomicInt finished;
    const char* volatile stage;
    Work* volatile work;  // the current parallel stage, for the progress
    float stage_base;
    float stage_span;
    BakeResult* result;

    Texel* texels;
    int texel_count;
    float* probe_positions;   // 3 per probe
    int probe_count;
    float* probe_coefficients; // 16 per probe (r, g, b SH, validity)
    float* culling_objects_bounds; // 6 per mesh
};

static void run_parallel(BakeJob* job, WorkFunction function, void* context, int count, const char* stage, float base, float span)
{
    Work work;
    SDL_zero(work);
    work.function = function;
    work.context = context;
    work.count = count;
    work.cancel = &job->cancel;

    job->stage = stage;
    job->stage_base = base;
    job->stage_span = span;
    job->work = &work;

    int thread_count = SDL_clamp(SDL_GetNumLogicalCPUCores() - 1, 1, 64);
    SDL_Thread* threads[64];

    for (int i = 0; i < thread_count; i++)
    {
        threads[i] = SDL_CreateThread(worker, "bake", &work);
    }

    for (int i = 0; i < thread_count; i++)
    {
        if (threads[i])
        {
            SDL_WaitThread(threads[i], NULL);
        }
        else
        {
            worker(&work); // no thread : this one does it
        }
    }

    job->work = NULL;
}

// --- Lightmaps : charts, packing, texels -----------------------------------------------------------

typedef struct
{
    int mesh;
    int chart;
    V3 origin, u, v;     // the chart's plane
    float min[2], max[2]; // in meters, on that plane
    float density;       // texels per meter, this chart's
    int width, height;   // texels, padding included
    int page, x, y;      // placement
    int* triangles;      // first indices of its triangles in the mesh's index array
    int triangle_count;
} Chart;

typedef struct
{
    int page;
    int x, y;            // the next free spot on the current shelf
    int height;          // the current shelf's
} Shelf;

static void chart_size(Chart* chart, float density, int page)
{
    float extent = SDL_max(chart->max[0] - chart->min[0], chart->max[1] - chart->min[1]);
    chart->density = density;

    // A face bigger than a page is baked coarser.
    if (extent * chart->density > (float)(page - 2 * CHART_PADDING - 1))
    {
        chart->density = (float)(page - 2 * CHART_PADDING - 1) / SDL_max(extent, 1e-6f);
    }

    chart->width = (int)SDL_ceilf((chart->max[0] - chart->min[0]) * chart->density) + 1 + 2 * CHART_PADDING;
    chart->height = (int)SDL_ceilf((chart->max[1] - chart->min[1]) * chart->density) + 1 + 2 * CHART_PADDING;
}

// Places the charts on the shelves of the current page ; 0 if they don't all fit.
static int pack_charts(Chart** charts, int count, int page, Shelf* shelf)
{
    for (int c = 0; c < count; c++)
    {
        Chart* chart = charts[c];

        if (shelf->x + chart->width > page)
        {
            shelf->x = 0;
            shelf->y += shelf->height;
            shelf->height = 0;
        }

        if (shelf->y + chart->height > page)
        {
            return 0;
        }

        chart->page = shelf->page;
        chart->x = shelf->x;
        chart->y = shelf->y;
        shelf->x += chart->width;
        shelf->height = SDL_max(shelf->height, chart->height);
    }

    return 1;
}

static int compare_charts(const void* a, const void* b)
{
    const Chart* ca = *(const Chart* const*)a;
    const Chart* cb = *(const Chart* const*)b;
    return cb->height != ca->height ? cb->height - ca->height : cb->width - ca->width;
}

static V3 mesh_position(const BakeMesh* mesh, uint32_t index)
{
    const MeshVertex* vertex = &mesh->vertices[index];
    const float p[3] = { vertex->x, vertex->y, vertex->z };
    return transform_point(mesh->model, p);
}

static void project(const Chart* chart, V3 p, float out[2])
{
    V3 d = sub(p, chart->origin);
    out[0] = dot(d, chart->u);
    out[1] = dot(d, chart->v);
}

// The closest point of a 2D triangle to q, as barycentrics.
static float closest_barycentrics(const float a[2], const float b[2], const float c[2], const float q[2], float out[3])
{
    float best = 1e30f;
    float area = (b[0] - a[0]) * (c[1] - a[1]) - (b[1] - a[1]) * (c[0] - a[0]);

    if (SDL_fabsf(area) > 1e-12f)
    {
        float l1 = ((q[0] - a[0]) * (c[1] - a[1]) - (q[1] - a[1]) * (c[0] - a[0])) / area;
        float l2 = ((b[0] - a[0]) * (q[1] - a[1]) - (b[1] - a[1]) * (q[0] - a[0])) / area;

        if (l1 >= 0.0f && l2 >= 0.0f && l1 + l2 <= 1.0f)
        {
            out[0] = 1.0f - l1 - l2;
            out[1] = l1;
            out[2] = l2;
            return 0.0f;
        }
    }

    // Outside : the closest point of its three edges.
    const float* corners[3] = { a, b, c };

    for (int e = 0; e < 3; e++)
    {
        const float* p0 = corners[e];
        const float* p1 = corners[(e + 1) % 3];
        float dx = p1[0] - p0[0], dy = p1[1] - p0[1];
        float l2 = dx * dx + dy * dy;
        float t = l2 > 1e-12f ? SDL_clamp(((q[0] - p0[0]) * dx + (q[1] - p0[1]) * dy) / l2, 0.0f, 1.0f) : 0.0f;
        float px = p0[0] + dx * t - q[0], py = p0[1] + dy * t - q[1];
        float d2 = px * px + py * py;

        if (d2 < best)
        {
            best = d2;
            out[0] = out[1] = out[2] = 0.0f;
            out[e] = 1.0f - t;
            out[(e + 1) % 3] = t;
        }
    }

    return SDL_sqrtf(best);
}

static int build_lightmaps(BakeJob* job)
{
    const BakeSettings* settings = &job->scene.settings;
    int page = settings->page_size;
    int chart_capacity = 0;

    for (int m = 0; m < job->mesh_count; m++)
    {
        if (job->meshes[m].vertex_charts)
        {
            chart_capacity += job->meshes[m].index_count / 3;
        }
    }

    Chart* charts = SDL_calloc((size_t)SDL_max(1, chart_capacity), sizeof(Chart));
    int chart_count = 0;

    // One chart per face : its triangles, gathered by the chart of their first vertex.
    for (int m = 0; m < job->mesh_count; m++)
    {
        const BakeMesh* mesh = &job->meshes[m];

        if (!mesh->vertex_charts)
        {
            continue;
        }

        int first_chart = chart_count;

        for (int i = 0; i + 2 < mesh->index_count; i += 3)
        {
            int id = mesh->vertex_charts[mesh->indices[i]];
            Chart* chart = NULL;

            for (int c = first_chart; c < chart_count && !chart; c++)
            {
                if (charts[c].chart == id)
                {
                    chart = &charts[c];
                }
            }

            if (!chart)
            {
                chart = &charts[chart_count++];
                chart->mesh = m;
                chart->chart = id;
                chart->triangles = SDL_malloc(sizeof(int) * (size_t)(mesh->index_count / 3));
            }

            chart->triangles[chart->triangle_count++] = i;
        }
    }

    // Each chart's plane and size.
    for (int c = 0; c < chart_count; c++)
    {
        Chart* chart = &charts[c];
        const BakeMesh* mesh = &job->meshes[chart->mesh];
        V3 normal = v3(0.0f, 0.0f, 0.0f);

        for (int t = 0; t < chart->triangle_count; t++)
        {
            const uint32_t* ix = &mesh->indices[chart->triangles[t]];
            V3 a = mesh_position(mesh, ix[0]), b = mesh_position(mesh, ix[1]), cc = mesh_position(mesh, ix[2]);
            normal = add(normal, cross(sub(b, a), sub(cc, a)));
        }

        normal = normalize(normal);
        basis(normal, &chart->u, &chart->v);
        chart->origin = mesh_position(mesh, mesh->indices[chart->triangles[0]]);
        chart->min[0] = chart->min[1] = 1e30f;
        chart->max[0] = chart->max[1] = -1e30f;

        for (int t = 0; t < chart->triangle_count; t++)
        {
            for (int k = 0; k < 3; k++)
            {
                float q[2];
                project(chart, mesh_position(mesh, mesh->indices[chart->triangles[t] + k]), q);

                for (int a = 0; a < 2; a++)
                {
                    chart->min[a] = SDL_min(chart->min[a], q[a]);
                    chart->max[a] = SDL_max(chart->max[a], q[a]);
                }
            }
        }

        chart_size(chart, settings->texels_per_meter, page);
    }

    // Shelf packing, an object at a time, its charts tallest first : an object's charts all land on one
    // page (its lightmap is one texture). One that doesn't fit an empty page is shrunk until it does.
    Chart** order = SDL_malloc(sizeof(Chart*) * (size_t)SDL_max(1, chart_count));
    Shelf shelf = { 0, 0, 0, 0 };
    int page_count = chart_count > 0 ? 1 : 0;
    int first = 0;

    while (first < chart_count)
    {
        int last = first;

        while (last < chart_count && charts[last].mesh == charts[first].mesh)
        {
            last++;
        }

        int count = last - first;
        float density = settings->texels_per_meter;

        for (int attempt = 0; attempt < 40; attempt++)
        {
            for (int c = 0; c < count; c++)
            {
                order[c] = &charts[first + c];
            }

            qsort(order, (size_t)count, sizeof(Chart*), compare_charts);

            Shelf trial = shelf;

            if (pack_charts(order, count, page, &trial))
            {
                shelf = trial;
                break;
            }

            if (shelf.x != 0 || shelf.y != 0 || shelf.height != 0)
            {
                // A fresh page, and the same density.
                page_count++;
                shelf.page = page_count - 1;
                shelf.x = shelf.y = shelf.height = 0;
                continue;
            }

            density *= 0.8f;

            for (int c = 0; c < count; c++)
            {
                chart_size(&charts[first + c], density, page);
            }
        }

        first = last;
    }

    SDL_free(order);

    BakeResult* result = job->result;
    result->page_size = page;
    result->page_count = page_count;
    result->pages = SDL_calloc((size_t)SDL_max(1, page_count) * (size_t)page * (size_t)page * 4, sizeof(uint16_t));

    // Each vertex's UV in its page.
    for (int m = 0; m < job->mesh_count; m++)
    {
        const BakeMesh* mesh = &job->meshes[m];
        ObjectEntry* entry = &result->objects[m];

        if (!mesh->vertex_charts)
        {
            continue;
        }

        entry->uvs = SDL_calloc((size_t)mesh->vertex_count * 2, sizeof(float));
        entry->page = -1;
    }

    for (int c = 0; c < chart_count; c++)
    {
        const Chart* chart = &charts[c];
        const BakeMesh* mesh = &job->meshes[chart->mesh];
        ObjectEntry* entry = &result->objects[chart->mesh];

        entry->page = chart->page; // the same for all of its charts

        for (int t = 0; t < chart->triangle_count; t++)
        {
            for (int k = 0; k < 3; k++)
            {
                uint32_t index = mesh->indices[chart->triangles[t] + k];
                float q[2];
                project(chart, mesh_position(mesh, index), q);
                entry->uvs[index * 2 + 0] = ((float)(chart->x + CHART_PADDING) + (q[0] - chart->min[0]) * chart->density + 0.5f) / (float)page;
                entry->uvs[index * 2 + 1] = ((float)(chart->y + CHART_PADDING) + (q[1] - chart->min[1]) * chart->density + 0.5f) / (float)page;
            }
        }
    }

    // The texels : each one's closest point on its chart (padding included, so filtering at the edges
    // reads the chart's own light).
    int texel_capacity = 0;

    for (int c = 0; c < chart_count; c++)
    {
        texel_capacity += charts[c].width * charts[c].height;
    }

    job->texels = SDL_malloc(sizeof(Texel) * (size_t)SDL_max(1, texel_capacity));
    job->texel_count = 0;

    for (int c = 0; c < chart_count; c++)
    {
        const Chart* chart = &charts[c];
        const BakeMesh* mesh = &job->meshes[chart->mesh];
        V3 normal = normalize(cross(chart->u, chart->v));

        for (int y = 0; y < chart->height; y++)
        {
            for (int x = 0; x < chart->width; x++)
            {
                float q[2] = {
                    chart->min[0] + ((float)(x - CHART_PADDING)) / chart->density,
                    chart->min[1] + ((float)(y - CHART_PADDING)) / chart->density
                };

                float best = 1e30f;
                V3 position = v3(0.0f, 0.0f, 0.0f);

                for (int t = 0; t < chart->triangle_count; t++)
                {
                    const uint32_t* ix = &mesh->indices[chart->triangles[t]];
                    V3 p[3] = { mesh_position(mesh, ix[0]), mesh_position(mesh, ix[1]), mesh_position(mesh, ix[2]) };
                    float a[2], b[2], cc[2], l[3];
                    project(chart, p[0], a);
                    project(chart, p[1], b);
                    project(chart, p[2], cc);

                    float d = closest_barycentrics(a, b, cc, q, l);

                    if (d < best)
                    {
                        best = d;
                        position = add(add(mul(p[0], l[0]), mul(p[1], l[1])), mul(p[2], l[2]));
                    }
                }

                if (best > (float)(CHART_PADDING + 1) / chart->density)
                {
                    continue;
                }

                Texel* texel = &job->texels[job->texel_count++];
                texel->page = chart->page;
                texel->x = chart->x + x;
                texel->y = chart->y + y;
                texel->position[0] = position.x;
                texel->position[1] = position.y;
                texel->position[2] = position.z;
                texel->normal[0] = normal.x;
                texel->normal[1] = normal.y;
                texel->normal[2] = normal.z;
            }
        }
    }

    for (int c = 0; c < chart_count; c++)
    {
        SDL_free(charts[c].triangles);
    }

    SDL_free(charts);

    return 1;
}

static void bake_texel(void* context, int index, Random* random)
{
    BakeJob* job = (BakeJob*)context;
    const Scene* scene = &job->scene;
    const Texel* texel = &job->texels[index];
    V3 p = v3(texel->position[0], texel->position[1], texel->position[2]);
    V3 n = v3(texel->normal[0], texel->normal[1], texel->normal[2]);
    V3 origin = add(p, mul(n, 2e-3f));

    // Irradiance / PI : the baked lights' direct light, then the mean radiance of cosine-distributed
    // rays (whose pdf, cos / PI, makes their mean exactly irradiance / PI).
    V3 sum = v3(0.0f, 0.0f, 0.0f);
    int samples = scene->settings.samples;

    for (int s = 0; s < samples; s++)
    {
        sum = add(sum, radiance(scene, origin, cosine_sample(n, random), scene->settings.bounces - 1, random, NULL));
    }

    V3 value = add(direct_light(scene, p, n, 1), mul(sum, 1.0f / (float)samples));

    int size = job->result->page_size;
    uint16_t* pixel = &job->result->pages[(((size_t)texel->page * (size_t)size + (size_t)texel->y) * (size_t)size + (size_t)texel->x) * 4];
    pixel[0] = float_to_half(SDL_min(value.x, 60000.0f));
    pixel[1] = float_to_half(SDL_min(value.y, 60000.0f));
    pixel[2] = float_to_half(SDL_min(value.z, 60000.0f));
    pixel[3] = float_to_half(1.0f);
}

// --- The probe volume ---------------------------------------------------------------------------

static void scene_bounds(const Scene* scene, float out_min[3], float out_max[3])
{
    for (int a = 0; a < 3; a++)
    {
        out_min[a] = 1e30f;
        out_max[a] = -1e30f;
    }

    for (int i = 0; i < scene->triangle_count; i++)
    {
        float lo[3], hi[3];
        triangle_bounds(&scene->triangles[i], lo, hi);

        for (int a = 0; a < 3; a++)
        {
            out_min[a] = SDL_min(out_min[a], lo[a]);
            out_max[a] = SDL_max(out_max[a], hi[a]);
        }
    }
}

static int build_probes(BakeJob* job)
{
    const Scene* scene = &job->scene;
    float spacing = scene->settings.probe_spacing;
    float lo[3], hi[3];
    scene_bounds(scene, lo, hi);

    if (scene->triangle_count == 0 || spacing <= 0.0f)
    {
        return 1;
    }

    float cell = spacing * 4.0f;
    int cells[3];

    for (;;)
    {
        for (int a = 0; a < 3; a++)
        {
            lo[a] -= 0.0f;
            cells[a] = SDL_max(1, (int)SDL_ceilf((hi[a] - lo[a] + spacing * 2.0f) / cell));
        }

        if ((long long)cells[0] * cells[1] * cells[2] <= MAX_CELLS)
        {
            break;
        }

        cell *= 1.25f; // a huge scene : coarser
    }

    float origin[3] = { lo[0] - spacing, lo[1] - spacing, lo[2] - spacing };
    int cell_count = cells[0] * cells[1] * cells[2];
    uint32_t* table = SDL_malloc(sizeof(uint32_t) * (size_t)cell_count);
    uint8_t* refined = SDL_calloc((size_t)cell_count, 1);

    // Cells holding geometry get a brick.
    for (int i = 0; i < scene->triangle_count; i++)
    {
        float tlo[3], thi[3];
        triangle_bounds(&scene->triangles[i], tlo, thi);
        int c0[3], c1[3];

        for (int a = 0; a < 3; a++)
        {
            c0[a] = SDL_clamp((int)SDL_floorf((tlo[a] - spacing - origin[a]) / cell), 0, cells[a] - 1);
            c1[a] = SDL_clamp((int)SDL_floorf((thi[a] + spacing - origin[a]) / cell), 0, cells[a] - 1);
        }

        for (int z = c0[2]; z <= c1[2]; z++)
        {
            for (int y = c0[1]; y <= c1[1]; y++)
            {
                for (int x = c0[0]; x <= c1[0]; x++)
                {
                    refined[(z * cells[1] + y) * cells[0] + x] = 1;
                }
            }
        }
    }

    int brick_count = 0;

    for (int i = 0; i < cell_count; i++)
    {
        table[i] = refined[i] ? (uint32_t)brick_count++ : NO_BRICK;
    }

    int coarse_count = (cells[0] + 1) * (cells[1] + 1) * (cells[2] + 1);
    job->probe_count = coarse_count + brick_count * BRICK_PROBES;
    job->probe_positions = SDL_malloc(sizeof(float) * 3 * (size_t)job->probe_count);
    job->probe_coefficients = SDL_calloc((size_t)job->probe_count * 16, sizeof(float));

    int p = 0;

    for (int z = 0; z <= cells[2]; z++)
    {
        for (int y = 0; y <= cells[1]; y++)
        {
            for (int x = 0; x <= cells[0]; x++)
            {
                job->probe_positions[p * 3 + 0] = origin[0] + (float)x * cell;
                job->probe_positions[p * 3 + 1] = origin[1] + (float)y * cell;
                job->probe_positions[p * 3 + 2] = origin[2] + (float)z * cell;
                p++;
            }
        }
    }

    for (int c = 0; c < cell_count; c++)
    {
        if (table[c] == NO_BRICK)
        {
            continue;
        }

        int cx = c % cells[0];
        int cy = (c / cells[0]) % cells[1];
        int cz = c / (cells[0] * cells[1]);

        for (int k = 0; k < BRICK_PROBES; k++)
        {
            int bx = k % 5, by = (k / 5) % 5, bz = k / 25;
            int index = coarse_count + (int)table[c] * BRICK_PROBES + k;
            job->probe_positions[index * 3 + 0] = origin[0] + ((float)cx + (float)bx * 0.25f) * cell;
            job->probe_positions[index * 3 + 1] = origin[1] + ((float)cy + (float)by * 0.25f) * cell;
            job->probe_positions[index * 3 + 2] = origin[2] + ((float)cz + (float)bz * 0.25f) * cell;
        }
    }

    // The blob the renderer reads : the table, the coarse probes, the bricks (filled after baking).
    BakeResult* result = job->result;
    uint32_t table_size = (uint32_t)cell_count * 4u;
    uint32_t size = table_size + (uint32_t)job->probe_count * PROBE_BYTES;
    result->probe_data = SDL_calloc(size, 1);
    SDL_memcpy(result->probe_data, table, table_size);

    result->has_probes = 1;
    result->probes.size = size;
    SDL_memcpy(result->probes.origin, origin, sizeof(origin));
    result->probes.cell_size = cell;
    SDL_memcpy(result->probes.cells, cells, sizeof(cells));
    result->probes.table_offset = 0;
    result->probes.coarse_offset = table_size;
    result->probes.bricks_offset = table_size + (uint32_t)coarse_count * PROBE_BYTES;

    SDL_free(table);
    SDL_free(refined);

    return 1;
}

static void bake_probe(void* context, int index, Random* random)
{
    BakeJob* job = (BakeJob*)context;
    const Scene* scene = &job->scene;
    V3 p = v3(job->probe_positions[index * 3], job->probe_positions[index * 3 + 1], job->probe_positions[index * 3 + 2]);
    int rays = scene->settings.probe_rays;
    float coefficients[4][3];
    SDL_zeroa(coefficients);
    int backfaces = 0;
    float rotation = random_float(random) * 2.0f * PI_F;

    // Spherical Fibonacci directions, turned randomly per probe : even coverage, no common pattern.
    for (int i = 0; i < rays; i++)
    {
        float z = 1.0f - (2.0f * (float)i + 1.0f) / (float)rays;
        float r = SDL_sqrtf(SDL_max(0.0f, 1.0f - z * z));
        float phi = (float)i * 2.39996323f + rotation;
        V3 d = v3(r * SDL_cosf(phi), z, r * SDL_sinf(phi));

        int backface = 0;
        V3 L = radiance(scene, p, d, scene->settings.bounces - 1, random, &backface);
        backfaces += backface;

        float basis_values[4] = { 0.282095f, 0.488603f * d.y, 0.488603f * d.z, 0.488603f * d.x };

        for (int k = 0; k < 4; k++)
        {
            coefficients[k][0] += L.x * basis_values[k];
            coefficients[k][1] += L.y * basis_values[k];
            coefficients[k][2] += L.z * basis_values[k];
        }
    }

    float weight = 4.0f * PI_F / (float)rays;

    for (int k = 0; k < 4; k++)
    {
        for (int c = 0; c < 3; c++)
        {
            coefficients[k][c] *= weight;
        }
    }

    // The baked lights' direct light : radiance from one direction, irradiance E there.
    for (int l = 0; l < scene->light_count; l++)
    {
        const BakeLight* light = &scene->lights[l];
        V3 irradiance, Ld;

        if (!light->baked || !light_at(scene, light, p, &irradiance, &Ld))
        {
            continue;
        }

        float basis_values[4] = { 0.282095f, 0.488603f * Ld.y, 0.488603f * Ld.z, 0.488603f * Ld.x };

        for (int k = 0; k < 4; k++)
        {
            coefficients[k][0] += irradiance.x * basis_values[k];
            coefficients[k][1] += irradiance.y * basis_values[k];
            coefficients[k][2] += irradiance.z * basis_values[k];
        }
    }

    // Convolved with the cosine lobe and divided by PI (Ramamoorthi & Hanrahan) : irradiance / PI.
    float* out = &job->probe_coefficients[index * 16];

    for (int c = 0; c < 3; c++)
    {
        out[c * 4 + 0] = coefficients[0][c];
        out[c * 4 + 1] = coefficients[1][c] * (2.0f / 3.0f);
        out[c * 4 + 2] = coefficients[2][c] * (2.0f / 3.0f);
        out[c * 4 + 3] = coefficients[3][c] * (2.0f / 3.0f);
    }

    // Buried in geometry (seeing the inside of walls) : left out of the interpolation.
    out[12] = (float)backfaces < (float)rays * 0.15f ? 1.0f : 0.0f;
}

// --- Occlusion culling --------------------------------------------------------------------------

typedef struct
{
    BakeJob* job;
    int cells[3];
    float origin[3];
    float cell;
    int bytes_per_cell;
    int** object_triangles;   // per mesh : its triangles in the BVH's order
    int* object_triangle_counts;
} Culling;

static Culling culling;

static V3 random_point_on(const Scene* scene, int triangle, Random* random)
{
    const Triangle* t = &scene->triangles[triangle];
    float u = random_float(random), v = random_float(random);

    if (u + v > 1.0f)
    {
        u = 1.0f - u;
        v = 1.0f - v;
    }

    return add(add(t->v0, mul(t->e1, u)), mul(t->e2, v));
}

static void bake_culling_cell(void* context, int index, Random* random)
{
    (void)context;
    BakeJob* job = culling.job;
    const Scene* scene = &job->scene;
    uint8_t* bits = &job->result->culling_bits[(size_t)index * (size_t)culling.bytes_per_cell];

    int cx = index % culling.cells[0];
    int cy = (index / culling.cells[0]) % culling.cells[1];
    int cz = index / (culling.cells[0] * culling.cells[1]);
    float lo[3] = { culling.origin[0] + (float)cx * culling.cell, culling.origin[1] + (float)cy * culling.cell, culling.origin[2] + (float)cz * culling.cell };

    for (int m = 0; m < job->mesh_count; m++)
    {
        const float* b = &job->culling_objects_bounds[m * 6];
        int near_cell = 1;

        // Overlapping (or touching) the cell : visible, no question.
        for (int a = 0; a < 3; a++)
        {
            near_cell &= b[a] <= lo[a] + culling.cell * 1.5f && b[a + 3] >= lo[a] - culling.cell * 0.5f;
        }

        int visible = near_cell || culling.object_triangle_counts[m] == 0;

        for (int r = 0; r < CULLING_RAYS && !visible; r++)
        {
            V3 from = v3(lo[0] + random_float(random) * culling.cell, lo[1] + random_float(random) * culling.cell, lo[2] + random_float(random) * culling.cell);
            int triangle = culling.object_triangles[m][random_next(random) % (uint32_t)culling.object_triangle_counts[m]];
            V3 to = random_point_on(scene, triangle, random);
            V3 d = sub(to, from);
            float distance = length(d);

            if (distance < 1e-4f)
            {
                visible = 1;
                break;
            }

            d = mul(d, 1.0f / distance);
            Hit hit;

            if (!trace(scene, from, d, distance + 1e-3f, 0, &hit) || scene->triangles[hit.triangle].object == m || hit.t >= distance - 1e-3f)
            {
                visible = 1;
            }
        }

        if (visible)
        {
            bits[m >> 3] |= (uint8_t)(1u << (m & 7));
        }
    }
}

static int build_culling(BakeJob* job)
{
    const Scene* scene = &job->scene;
    float cell = job->scene.settings.culling_cell;
    float lo[3], hi[3];
    scene_bounds(scene, lo, hi);

    if (cell <= 0.0f || scene->triangle_count == 0)
    {
        return 1;
    }

    SDL_zero(culling);
    culling.job = job;

    for (;;)
    {
        for (int a = 0; a < 3; a++)
        {
            culling.cells[a] = SDL_max(1, (int)SDL_ceilf((hi[a] - lo[a]) / cell));
        }

        if ((long long)culling.cells[0] * culling.cells[1] * culling.cells[2] <= 32768)
        {
            break;
        }

        cell *= 1.25f;
    }

    SDL_memcpy(culling.origin, lo, sizeof(lo));
    culling.cell = cell;
    culling.bytes_per_cell = (job->mesh_count + 7) / 8;

    // Each object's triangles, to aim the rays at.
    culling.object_triangles = SDL_calloc((size_t)job->mesh_count, sizeof(int*));
    culling.object_triangle_counts = SDL_calloc((size_t)job->mesh_count, sizeof(int));

    for (int i = 0; i < scene->triangle_count; i++)
    {
        culling.object_triangle_counts[scene->triangles[i].object]++;
    }

    for (int m = 0; m < job->mesh_count; m++)
    {
        culling.object_triangles[m] = SDL_malloc(sizeof(int) * (size_t)SDL_max(1, culling.object_triangle_counts[m]));
        culling.object_triangle_counts[m] = 0;
    }

    for (int i = 0; i < scene->triangle_count; i++)
    {
        int m = scene->triangles[i].object;
        culling.object_triangles[m][culling.object_triangle_counts[m]++] = i;
    }

    int cell_count = culling.cells[0] * culling.cells[1] * culling.cells[2];
    BakeResult* result = job->result;
    result->culling_bits = SDL_calloc((size_t)cell_count * (size_t)culling.bytes_per_cell, 1);
    result->has_culling = 1;
    SDL_memcpy(result->culling_origin, culling.origin, sizeof(culling.origin));
    result->culling_cell = cell;
    SDL_memcpy(result->culling_cells, culling.cells, sizeof(culling.cells));
    result->culling_objects = job->mesh_count;

    run_parallel(job, bake_culling_cell, NULL, cell_count, "Occlusion culling", 0.9f, 0.1f);

    for (int m = 0; m < job->mesh_count; m++)
    {
        SDL_free(culling.object_triangles[m]);
    }

    SDL_free(culling.object_triangles);
    SDL_free(culling.object_triangle_counts);

    return 1;
}

// --- The job ------------------------------------------------------------------------------------

void bake_default_settings(BakeSettings* out_settings)
{
    SDL_zerop(out_settings);
    out_settings->texels_per_meter = 8.0f;
    out_settings->page_size = 1024;
    out_settings->samples = 128;
    out_settings->bounces = 2;
    out_settings->probe_spacing = 1.0f;
    out_settings->probe_rays = 192;
    out_settings->culling_cell = 4.0f;
    out_settings->sky_intensity = 1.0f;
}

uint32_t bake_key(const char* name)
{
    uint32_t hash = 2166136261u;

    for (const char* c = name; *c; c++)
    {
        hash = (hash ^ (uint8_t)*c) * 16777619u;
    }

    return hash;
}

static int build_scene(BakeJob* job)
{
    Scene* scene = &job->scene;
    int capacity = 0;

    for (int m = 0; m < job->mesh_count; m++)
    {
        capacity += job->meshes[m].index_count / 3;
    }

    scene->triangles = SDL_malloc(sizeof(Triangle) * (size_t)SDL_max(1, capacity));
    scene->triangle_count = 0;

    for (int m = 0; m < job->mesh_count; m++)
    {
        const BakeMesh* mesh = &job->meshes[m];
        V3 tint = v3(mesh->albedo[0], mesh->albedo[1], mesh->albedo[2]);

        for (int i = 0; i + 2 < mesh->index_count; i += 3)
        {
            V3 a = mesh_position(mesh, mesh->indices[i]);
            V3 b = mesh_position(mesh, mesh->indices[i + 1]);
            V3 c = mesh_position(mesh, mesh->indices[i + 2]);
            V3 n = cross(sub(b, a), sub(c, a));

            if (dot(n, n) < 1e-16f)
            {
                continue;
            }

            Triangle* t = &scene->triangles[scene->triangle_count++];
            t->v0 = a;
            t->e1 = sub(b, a);
            t->e2 = sub(c, a);
            t->normal = normalize(n);

            for (int k = 0; k < 3; k++)
            {
                const MeshVertex* vertex = &mesh->vertices[mesh->indices[i + k]];
                V3 color = v3(SDL_clamp(vertex->r, 0.0f, 1.0f), SDL_clamp(vertex->g, 0.0f, 1.0f), SDL_clamp(vertex->b, 0.0f, 1.0f));
                t->albedo[k] = mul(hadamard(color, tint), 0.98f); // nothing reflects everything
            }

            t->emissive = v3(mesh->emissive[0], mesh->emissive[1], mesh->emissive[2]);
            t->object = m;
            t->occludes = mesh->occludes;
        }
    }

    scene->nodes = SDL_malloc(sizeof(Node) * (size_t)SDL_max(1, scene->triangle_count * 2));
    scene->node_count = scene->triangle_count > 0 ? 1 : 0;

    return scene->triangle_count == 0 || build_node(scene, 0, 0, scene->triangle_count);
}

static int SDLCALL bake_thread(void* data)
{
    BakeJob* job = (BakeJob*)data;
    Uint64 start = SDL_GetTicks();

    job->stage = "Preparing";

    if (build_scene(job) && build_lightmaps(job) && build_probes(job))
    {
        run_parallel(job, bake_texel, job, job->texel_count, "Lightmaps", 0.0f, 0.6f);
        run_parallel(job, bake_probe, job, job->probe_count, "Probes", 0.6f, 0.3f);

        // The probes, into their blob.
        if (job->result->has_probes)
        {
            uint8_t* probes = job->result->probe_data + job->result->probes.coarse_offset;

            for (int p = 0; p < job->probe_count; p++)
            {
                SDL_memcpy(probes + (size_t)p * PROBE_BYTES, &job->probe_coefficients[p * 16], PROBE_BYTES);
            }
        }

        // Every object's bounds, for occlusion culling.
        job->culling_objects_bounds = SDL_malloc(sizeof(float) * 6 * (size_t)SDL_max(1, job->mesh_count));

        for (int m = 0; m < job->mesh_count; m++)
        {
            float* b = &job->culling_objects_bounds[m * 6];
            b[0] = b[1] = b[2] = 1e30f;
            b[3] = b[4] = b[5] = -1e30f;

            for (int v = 0; v < job->meshes[m].vertex_count; v++)
            {
                V3 p = mesh_position(&job->meshes[m], (uint32_t)v);
                b[0] = SDL_min(b[0], p.x); b[1] = SDL_min(b[1], p.y); b[2] = SDL_min(b[2], p.z);
                b[3] = SDL_max(b[3], p.x); b[4] = SDL_max(b[4], p.y); b[5] = SDL_max(b[5], p.z);
            }
        }

        build_culling(job);
    }

    job->stage = SDL_GetAtomicInt(&job->cancel) ? "Cancelled" : "Done";
    DEBUG_LOG("[bake] %d texels, %d probes, %d triangles : %.1f s\n", job->texel_count, job->probe_count, job->scene.triangle_count,
        (double)(SDL_GetTicks() - start) / 1000.0);

    SDL_SetAtomicInt(&job->finished, 1);
    return 0;
}

BakeJob* bake_start(const BakeMesh* meshes, int mesh_count, const BakeLight* lights, int light_count, const BakeSettings* settings)
{
    BakeJob* job = SDL_calloc(1, sizeof(BakeJob));
    job->scene.settings = *settings;
    job->scene.settings.samples = SDL_max(1, settings->samples);
    job->scene.settings.bounces = SDL_clamp(settings->bounces, 1, 8);
    job->scene.settings.probe_rays = SDL_max(16, settings->probe_rays);
    job->scene.settings.page_size = SDL_clamp(settings->page_size, 128, 8192);
    job->scene.settings.texels_per_meter = SDL_max(0.05f, settings->texels_per_meter);

    // Copies : the scene may change while the bake runs.
    job->meshes = SDL_calloc((size_t)SDL_max(1, mesh_count), sizeof(BakeMesh));
    job->mesh_count = mesh_count;

    for (int m = 0; m < mesh_count; m++)
    {
        BakeMesh* copy = &job->meshes[m];
        *copy = meshes[m];
        copy->vertices = SDL_malloc(sizeof(MeshVertex) * (size_t)SDL_max(1, meshes[m].vertex_count));
        SDL_memcpy((void*)copy->vertices, meshes[m].vertices, sizeof(MeshVertex) * (size_t)meshes[m].vertex_count);
        copy->indices = SDL_malloc(sizeof(uint32_t) * (size_t)SDL_max(1, meshes[m].index_count));
        SDL_memcpy((void*)copy->indices, meshes[m].indices, sizeof(uint32_t) * (size_t)meshes[m].index_count);

        if (meshes[m].vertex_charts)
        {
            copy->vertex_charts = SDL_malloc(sizeof(int) * (size_t)SDL_max(1, meshes[m].vertex_count));
            SDL_memcpy((void*)copy->vertex_charts, meshes[m].vertex_charts, sizeof(int) * (size_t)meshes[m].vertex_count);
        }
    }

    job->scene.lights = SDL_malloc(sizeof(BakeLight) * (size_t)SDL_max(1, light_count));
    SDL_memcpy(job->scene.lights, lights, sizeof(BakeLight) * (size_t)light_count);
    job->scene.light_count = light_count;

    job->result = SDL_calloc(1, sizeof(BakeResult));
    job->result->objects = SDL_calloc((size_t)SDL_max(1, mesh_count), sizeof(ObjectEntry));
    job->result->object_count = mesh_count;

    for (int m = 0; m < mesh_count; m++)
    {
        job->result->objects[m].key = meshes[m].key;
        job->result->objects[m].vertex_count = meshes[m].vertex_count;
        job->result->objects[m].page = -1;
    }

    job->stage = "Starting";
    job->thread = SDL_CreateThread(bake_thread, "bake", job);

    if (!job->thread)
    {
        bake_thread(job);
    }

    return job;
}

float bake_progress(const BakeJob* job, const char** out_stage)
{
    if (out_stage)
    {
        *out_stage = job->stage;
    }

    if (SDL_GetAtomicInt((SDL_AtomicInt*)&job->finished))
    {
        return 1.0f;
    }

    Work* work = job->work;
    float within = work && work->count > 0 ? (float)SDL_GetAtomicInt(&work->done) / (float)work->count : 0.0f;
    return SDL_clamp(job->stage_base + job->stage_span * within, 0.0f, 1.0f);
}

int bake_done(const BakeJob* job)
{
    return SDL_GetAtomicInt((SDL_AtomicInt*)&job->finished);
}

void bake_cancel(BakeJob* job)
{
    SDL_SetAtomicInt(&job->cancel, 1);
}

BakeResult* bake_finish(BakeJob* job)
{
    if (job->thread)
    {
        SDL_WaitThread(job->thread, NULL);
    }

    BakeResult* result = SDL_GetAtomicInt(&job->cancel) ? NULL : job->result;

    if (!result)
    {
        bake_free(job->result);
    }

    for (int m = 0; m < job->mesh_count; m++)
    {
        SDL_free((void*)job->meshes[m].vertices);
        SDL_free((void*)job->meshes[m].indices);
        SDL_free((void*)job->meshes[m].vertex_charts);
    }

    SDL_free(job->meshes);
    SDL_free(job->scene.lights);
    SDL_free(job->scene.triangles);
    SDL_free(job->scene.nodes);
    SDL_free(job->texels);
    SDL_free(job->probe_positions);
    SDL_free(job->probe_coefficients);
    SDL_free(job->culling_objects_bounds);
    SDL_free(job);

    return result;
}

// --- Files ----------------------------------------------------------------------------------------

typedef struct
{
    uint8_t* data;
    size_t size;
    size_t capacity;
    size_t read;
    int failed;
} Stream;

static void put(Stream* s, const void* data, size_t size)
{
    if (s->size + size > s->capacity)
    {
        s->capacity = SDL_max(s->capacity * 2, s->size + size + 4096);
        s->data = SDL_realloc(s->data, s->capacity);
    }

    SDL_memcpy(s->data + s->size, data, size);
    s->size += size;
}

static void put_u32(Stream* s, uint32_t value)
{
    put(s, &value, sizeof(value));
}

static void get(Stream* s, void* out, size_t size)
{
    if (s->failed || s->read + size > s->size)
    {
        s->failed = 1;
        SDL_memset(out, 0, size);
        return;
    }

    SDL_memcpy(out, s->data + s->read, size);
    s->read += size;
}

static uint32_t get_u32(Stream* s)
{
    uint32_t value = 0;
    get(s, &value, sizeof(value));
    return value;
}

int bake_save(const BakeResult* result, const char* path)
{
    Stream s;
    SDL_zero(s);
    put(&s, FILE_MAGIC, 8);

    put_u32(&s, (uint32_t)result->page_size);
    put_u32(&s, (uint32_t)result->page_count);
    put(&s, result->pages, (size_t)result->page_count * (size_t)result->page_size * (size_t)result->page_size * 4 * sizeof(uint16_t));

    put_u32(&s, (uint32_t)result->object_count);

    for (int i = 0; i < result->object_count; i++)
    {
        const ObjectEntry* entry = &result->objects[i];
        put_u32(&s, entry->key);
        put_u32(&s, (uint32_t)entry->vertex_count);
        put_u32(&s, (uint32_t)(entry->uvs ? entry->page : -1));

        if (entry->uvs)
        {
            put(&s, entry->uvs, sizeof(float) * 2 * (size_t)entry->vertex_count);
        }
    }

    put_u32(&s, (uint32_t)result->has_probes);

    if (result->has_probes)
    {
        put(&s, &result->probes, sizeof(result->probes));
        put(&s, result->probe_data, result->probes.size);
    }

    put_u32(&s, (uint32_t)result->has_culling);

    if (result->has_culling)
    {
        put(&s, result->culling_origin, sizeof(result->culling_origin));
        put(&s, &result->culling_cell, sizeof(result->culling_cell));
        put(&s, result->culling_cells, sizeof(result->culling_cells));
        put_u32(&s, (uint32_t)result->culling_objects);
        put(&s, result->culling_bits, (size_t)result->culling_cells[0] * (size_t)result->culling_cells[1] * (size_t)result->culling_cells[2] *
            (size_t)((result->culling_objects + 7) / 8));
    }

    put_u32(&s, (uint32_t)result->reflection_probe_count);

    for (int i = 0; i < result->reflection_probe_count; i++)
    {
        const BakeReflectionProbe* probe = &result->reflection_probes[i];
        put_u32(&s, probe->key);
        put(&s, probe->position, sizeof(probe->position));
        put(&s, probe->box_min, sizeof(probe->box_min));
        put(&s, probe->box_max, sizeof(probe->box_max));
        put(&s, &probe->blend, sizeof(probe->blend));
        put_u32(&s, (uint32_t)probe->width);
        put_u32(&s, (uint32_t)probe->height);
        put(&s, probe->pixels, (size_t)probe->width * (size_t)probe->height * 4 * sizeof(uint16_t));
    }

    int saved = SDL_SaveFile(path, s.data, s.size);
    SDL_free(s.data);

    if (!saved)
    {
        printf("Could not save the baked lighting '%s': %s\n", path, SDL_GetError());
    }

    return saved;
}

BakeResult* bake_load(const char* path)
{
    Stream s;
    SDL_zero(s);
    s.data = SDL_LoadFile(path, &s.size);

    if (!s.data)
    {
        return NULL;
    }

    char magic[8];
    get(&s, magic, 8);

    if (s.failed || SDL_memcmp(magic, FILE_MAGIC, 8) != 0)
    {
        SDL_free(s.data);
        return NULL;
    }

    BakeResult* result = SDL_calloc(1, sizeof(BakeResult));
    result->page_size = (int)get_u32(&s);
    result->page_count = (int)get_u32(&s);

    size_t page_bytes = (size_t)result->page_count * (size_t)result->page_size * (size_t)result->page_size * 4 * sizeof(uint16_t);

    if (result->page_size <= 0 || result->page_size > 8192 || result->page_count < 0 || result->page_count > 256)
    {
        s.failed = 1;
    }
    else
    {
        result->pages = SDL_malloc(SDL_max(page_bytes, 1));
        get(&s, result->pages, page_bytes);
    }

    result->object_count = s.failed ? 0 : (int)get_u32(&s);

    if (result->object_count < 0 || result->object_count > 1 << 20)
    {
        s.failed = 1;
        result->object_count = 0;
    }

    result->objects = SDL_calloc((size_t)SDL_max(1, result->object_count), sizeof(ObjectEntry));

    for (int i = 0; i < result->object_count && !s.failed; i++)
    {
        ObjectEntry* entry = &result->objects[i];
        entry->key = get_u32(&s);
        entry->vertex_count = (int)get_u32(&s);
        entry->page = (int)get_u32(&s);

        if (entry->page >= 0 && entry->vertex_count > 0 && entry->vertex_count < (1 << 24))
        {
            entry->uvs = SDL_malloc(sizeof(float) * 2 * (size_t)entry->vertex_count);
            get(&s, entry->uvs, sizeof(float) * 2 * (size_t)entry->vertex_count);
        }
    }

    result->has_probes = s.failed ? 0 : (int)get_u32(&s);

    if (result->has_probes)
    {
        get(&s, &result->probes, sizeof(result->probes));

        if (result->probes.size > (1u << 30))
        {
            s.failed = 1;
        }
        else
        {
            result->probe_data = SDL_malloc(SDL_max(result->probes.size, 1u));
            get(&s, result->probe_data, result->probes.size);
        }
    }

    result->has_culling = s.failed ? 0 : (int)get_u32(&s);

    if (result->has_culling)
    {
        get(&s, result->culling_origin, sizeof(result->culling_origin));
        get(&s, &result->culling_cell, sizeof(result->culling_cell));
        get(&s, result->culling_cells, sizeof(result->culling_cells));
        result->culling_objects = (int)get_u32(&s);
        size_t bytes = (size_t)result->culling_cells[0] * (size_t)result->culling_cells[1] * (size_t)result->culling_cells[2] *
            (size_t)((result->culling_objects + 7) / 8);

        if (bytes > (1u << 28))
        {
            s.failed = 1;
        }
        else
        {
            result->culling_bits = SDL_malloc(SDL_max(bytes, 1));
            get(&s, result->culling_bits, bytes);
        }
    }

    // Reflection probes : files from before they existed simply end here.
    if (!s.failed && s.read < s.size)
    {
        int count = (int)get_u32(&s);

        if (count < 0 || count > 1024)
        {
            s.failed = 1;
        }

        result->reflection_probes = SDL_calloc((size_t)SDL_max(1, count), sizeof(BakeReflectionProbe));

        for (int i = 0; i < count && !s.failed; i++)
        {
            BakeReflectionProbe* probe = &result->reflection_probes[i];
            probe->key = get_u32(&s);
            get(&s, probe->position, sizeof(probe->position));
            get(&s, probe->box_min, sizeof(probe->box_min));
            get(&s, probe->box_max, sizeof(probe->box_max));
            get(&s, &probe->blend, sizeof(probe->blend));
            probe->width = (int)get_u32(&s);
            probe->height = (int)get_u32(&s);

            if (probe->width <= 0 || probe->height <= 0 || probe->width > 8192 || probe->height > 8192)
            {
                s.failed = 1;
                break;
            }

            size_t bytes = (size_t)probe->width * (size_t)probe->height * 4 * sizeof(uint16_t);
            uint16_t* pixels = SDL_malloc(bytes);
            get(&s, pixels, bytes);
            probe->pixels = pixels;
            result->reflection_probe_count = i + 1;
        }
    }

    SDL_free(s.data);

    if (s.failed)
    {
        printf("The baked lighting '%s' is damaged : ignored.\n", path);
        bake_free(result);
        return NULL;
    }

    if (result->has_probes)
    {
        result->probes.data = result->probe_data;
    }

    return result;
}

void bake_free(BakeResult* result)
{
    if (!result)
    {
        return;
    }

    for (int i = 0; i < result->object_count; i++)
    {
        SDL_free(result->objects[i].uvs);
    }

    for (int i = 0; i < result->reflection_probe_count; i++)
    {
        SDL_free((void*)result->reflection_probes[i].pixels);
    }

    SDL_free(result->reflection_probes);
    SDL_free(result->objects);
    SDL_free(result->pages);
    SDL_free(result->probe_data);
    SDL_free(result->culling_bits);
    SDL_free(result);
}

BakeResult* bake_empty(void)
{
    BakeResult* result = SDL_calloc(1, sizeof(BakeResult));
    result->page_size = 256;
    result->objects = SDL_calloc(1, sizeof(ObjectEntry));
    result->pages = SDL_calloc(1, sizeof(uint16_t));
    return result;
}

void bake_set_reflection_probe(BakeResult* result, const BakeReflectionProbe* probe)
{
    int index = -1;

    for (int i = 0; i < result->reflection_probe_count; i++)
    {
        if (result->reflection_probes[i].key == probe->key)
        {
            index = i;
            SDL_free((void*)result->reflection_probes[i].pixels);
        }
    }

    if (index < 0)
    {
        result->reflection_probes = SDL_realloc(result->reflection_probes, sizeof(BakeReflectionProbe) * (size_t)(result->reflection_probe_count + 1));
        index = result->reflection_probe_count++;
    }

    size_t bytes = (size_t)probe->width * (size_t)probe->height * 4 * sizeof(uint16_t);
    uint16_t* pixels = SDL_malloc(bytes);
    SDL_memcpy(pixels, probe->pixels, bytes);
    result->reflection_probes[index] = *probe;
    result->reflection_probes[index].pixels = pixels;
}

int bake_reflection_probe_count(const BakeResult* result)
{
    return result->reflection_probe_count;
}

const BakeReflectionProbe* bake_reflection_probe(const BakeResult* result, int index)
{
    return &result->reflection_probes[index];
}

int bake_page_count(const BakeResult* result)
{
    return result->page_count;
}

int bake_page_size(const BakeResult* result)
{
    return result->page_size;
}

const uint16_t* bake_page_pixels(const BakeResult* result, int page)
{
    return &result->pages[(size_t)page * (size_t)result->page_size * (size_t)result->page_size * 4];
}

static int find_object(const BakeResult* result, uint32_t key)
{
    for (int i = 0; i < result->object_count; i++)
    {
        if (result->objects[i].key == key)
        {
            return i;
        }
    }

    return -1;
}

int bake_object_lightmap(const BakeResult* result, uint32_t key, int vertex_count, int* out_page, const float** out_uvs)
{
    int i = find_object(result, key);

    if (i < 0 || !result->objects[i].uvs || result->objects[i].vertex_count != vertex_count || result->objects[i].page < 0)
    {
        return 0;
    }

    *out_page = result->objects[i].page;
    *out_uvs = result->objects[i].uvs;
    return 1;
}

int bake_probe_volume(const BakeResult* result, BakeProbeVolume* out_volume)
{
    if (!result->has_probes)
    {
        return 0;
    }

    *out_volume = result->probes;
    out_volume->data = result->probe_data;
    return 1;
}

int bake_object_visible(const BakeResult* result, uint32_t key, const float point[3])
{
    if (!result->has_culling)
    {
        return 1;
    }

    int cell[3];

    for (int a = 0; a < 3; a++)
    {
        cell[a] = (int)SDL_floorf((point[a] - result->culling_origin[a]) / result->culling_cell);

        if (cell[a] < 0 || cell[a] >= result->culling_cells[a])
        {
            return 1;
        }
    }

    int object = find_object(result, key);

    if (object < 0 || object >= result->culling_objects)
    {
        return 1;
    }

    size_t index = ((size_t)cell[2] * (size_t)result->culling_cells[1] + (size_t)cell[1]) * (size_t)result->culling_cells[0] + (size_t)cell[0];
    const uint8_t* bits = &result->culling_bits[index * (size_t)((result->culling_objects + 7) / 8)];

    return (bits[object >> 3] >> (object & 7)) & 1;
}
