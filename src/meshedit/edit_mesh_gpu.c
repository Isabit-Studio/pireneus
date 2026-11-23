#include "meshedit/edit_mesh_gpu.h"

#include <SDL3/SDL.h>

#include "assets/asset_database.h"
#include "meshedit/edit_mesh.h"

// With two frames in flight, a mesh last drawn three frames ago is no longer read by the GPU.
#define RETIRE_DELAY 3

typedef struct
{
    uint32_t handle;
    const EditMesh* source; // with its revision : what the GPU mesh was built from
    uint32_t revision;
    int last_used;
    EditMeshGpu gpu;
} Entry;

typedef struct
{
    Mesh* mesh;
    int frame;
} Retired;

static Entry* entries;
static int entry_count;
static int entry_capacity;
static Retired* retired;
static int retired_count;
static int retired_capacity;
static int frame_number;

static void free_cpu_data(EditMeshGpu* gpu)
{
    SDL_free(gpu->parts);
    SDL_free((void*)gpu->vertices);
    SDL_free((void*)gpu->indices);
    SDL_free((void*)gpu->vertex_faces);
}

static void retire(Mesh* mesh)
{
    if (!mesh)
    {
        return;
    }

    if (retired_count == retired_capacity)
    {
        retired_capacity = SDL_max(16, retired_capacity * 2);
        retired = SDL_realloc(retired, sizeof(Retired) * (size_t)retired_capacity);
    }

    retired[retired_count].mesh = mesh;
    retired[retired_count].frame = frame_number;
    retired_count++;
}

static void build(Entry* entry, const EditMesh* mesh)
{
    // Corners and triangles : a face of n vertices has n corners (flat shading : nothing's shared
    // between faces) and n - 2 triangles.
    int corner_count = mesh->loop_count;
    int index_capacity = 0;

    for (int f = 0; f < mesh->face_count; f++)
    {
        index_capacity += (mesh->faces[f].count - 2) * 3;
    }

    MeshVertex* vertices = SDL_malloc(sizeof(MeshVertex) * (size_t)SDL_max(1, corner_count));
    uint32_t* indices = SDL_malloc(sizeof(uint32_t) * (size_t)SDL_max(1, index_capacity));
    int* vertex_faces = SDL_malloc(sizeof(int) * (size_t)SDL_max(1, corner_count));
    int* triangles = SDL_malloc(sizeof(int) * 3 * (size_t)SDL_max(1, corner_count));
    int vertex_count = 0;
    int index_count = 0;

    entry->gpu.parts = SDL_malloc(sizeof(EditMeshGpuPart) * (size_t)SDL_max(1, mesh->material_count));
    entry->gpu.part_count = 0;

    // One part per material : its faces' triangles, one after the other.
    for (int material = 0; material < mesh->material_count; material++)
    {
        int first = index_count;
        float color[3];
        asset_material_color(mesh->materials[material], color);

        for (int f = 0; f < mesh->face_count; f++)
        {
            const EditFace* face = &mesh->faces[f];

            if (face->style.material != material)
            {
                continue;
            }

            float normal[3];
            float u[4];
            float v[4];
            edit_mesh_face_normal(mesh, f, normal);
            edit_mesh_texture_axes(mesh, f, u, v);

            // The tangent follows u ; its sign says whether v runs along normal x tangent or against it.
            float tangent[3] = { u[0], u[1], u[2] };
            float length = SDL_sqrtf(tangent[0] * tangent[0] + tangent[1] * tangent[1] + tangent[2] * tangent[2]);

            if (length > 1e-9f)
            {
                tangent[0] /= length;
                tangent[1] /= length;
                tangent[2] /= length;
            }

            float bitangent[3] = {
                normal[1] * tangent[2] - normal[2] * tangent[1],
                normal[2] * tangent[0] - normal[0] * tangent[2],
                normal[0] * tangent[1] - normal[1] * tangent[0]
            };
            float sign = bitangent[0] * v[0] + bitangent[1] * v[1] + bitangent[2] * v[2] < 0.0f ? -1.0f : 1.0f;

            int base = vertex_count;

            for (int i = 0; i < face->count; i++)
            {
                const float* p = edit_mesh_position(mesh, mesh->loops[face->first + i]);
                vertex_faces[vertex_count] = f;
                MeshVertex* out = &vertices[vertex_count++];
                out->x = p[0];
                out->y = p[1];
                out->z = p[2];
                out->nx = normal[0];
                out->ny = normal[1];
                out->nz = normal[2];
                out->r = color[0];
                out->g = color[1];
                out->b = color[2];
                out->a = 1.0f;
                out->u = p[0] * u[0] + p[1] * u[1] + p[2] * u[2] + u[3];
                out->v = p[0] * v[0] + p[1] * v[1] + p[2] * v[2] + v[3];
                out->tx = tangent[0];
                out->ty = tangent[1];
                out->tz = tangent[2];
                out->tw = sign;
            }

            int count = edit_mesh_triangulate_face(mesh, f, triangles);

            for (int t = 0; t < count * 3; t++)
            {
                indices[index_count++] = (uint32_t)(base + triangles[t]);
            }
        }

        if (index_count > first)
        {
            EditMeshGpuPart* part = &entry->gpu.parts[entry->gpu.part_count++];
            part->first_index = first;
            part->index_count = index_count - first;
            part->material = material;
        }
    }

    entry->gpu.mesh = index_count > 0 ? create_mesh(vertices, vertex_count, indices, index_count) : NULL;
    edit_mesh_bounds(mesh, entry->gpu.bounds);

    entry->gpu.vertices = vertices;
    entry->gpu.vertex_count = vertex_count;
    entry->gpu.indices = indices;
    entry->gpu.index_count = index_count;
    entry->gpu.vertex_faces = vertex_faces;

    SDL_free(triangles);
}

const EditMeshGpu* edit_mesh_gpu_get(uint32_t handle)
{
    const EditMesh* mesh = edit_mesh_store_get(handle);

    if (!mesh)
    {
        return NULL;
    }

    Entry* entry = NULL;

    for (int i = 0; i < entry_count && !entry; i++)
    {
        if (entries[i].handle == handle)
        {
            entry = &entries[i];
        }
    }

    if (entry && entry->source == mesh && entry->revision == mesh->revision)
    {
        entry->last_used = frame_number;
        return &entry->gpu;
    }

    if (!entry)
    {
        if (entry_count == entry_capacity)
        {
            entry_capacity = SDL_max(64, entry_capacity * 2);
            entries = SDL_realloc(entries, sizeof(Entry) * (size_t)entry_capacity);
        }

        entry = &entries[entry_count++];
        SDL_zerop(entry);
        entry->handle = handle;
    }
    else
    {
        retire(entry->gpu.mesh);
        free_cpu_data(&entry->gpu);
        SDL_zero(entry->gpu);
    }

    entry->source = mesh;
    entry->revision = mesh->revision;
    entry->last_used = frame_number;
    build(entry, mesh);

    return &entry->gpu;
}

void edit_mesh_gpu_end_frame(void)
{
    frame_number++;

    int kept = 0;

    for (int i = 0; i < entry_count; i++)
    {
        if (frame_number - entries[i].last_used > RETIRE_DELAY)
        {
            retire(entries[i].gpu.mesh);
            free_cpu_data(&entries[i].gpu);
        }
        else
        {
            entries[kept++] = entries[i];
        }
    }

    entry_count = kept;
    kept = 0;

    for (int i = 0; i < retired_count; i++)
    {
        if (frame_number - retired[i].frame > RETIRE_DELAY)
        {
            destroy_mesh(retired[i].mesh);
        }
        else
        {
            retired[kept++] = retired[i];
        }
    }

    retired_count = kept;
}

void edit_mesh_gpu_clear(void)
{
    for (int i = 0; i < entry_count; i++)
    {
        if (entries[i].gpu.mesh)
        {
            destroy_mesh(entries[i].gpu.mesh);
        }

        free_cpu_data(&entries[i].gpu);
    }

    for (int i = 0; i < retired_count; i++)
    {
        destroy_mesh(retired[i].mesh);
    }

    SDL_free(entries);
    SDL_free(retired);
    entries = NULL;
    retired = NULL;
    entry_count = entry_capacity = 0;
    retired_count = retired_capacity = 0;
}
