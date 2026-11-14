#include "meshedit/edit_mesh.h"

#include <SDL3/SDL.h>
#include <stdio.h>

#define DEG_TO_RAD (SDL_PI_F / 180.0f)
#define PLANE_EPSILON 1e-4f

// --- Small helpers ----------------------------------------------------------------------------------

static void v_copy(float* out, const float* a) { out[0] = a[0]; out[1] = a[1]; out[2] = a[2]; }
static void v_set(float* out, float x, float y, float z) { out[0] = x; out[1] = y; out[2] = z; }
static void v_sub(float* out, const float* a, const float* b) { out[0] = a[0] - b[0]; out[1] = a[1] - b[1]; out[2] = a[2] - b[2]; }
static void v_add(float* out, const float* a, const float* b) { out[0] = a[0] + b[0]; out[1] = a[1] + b[1]; out[2] = a[2] + b[2]; }
static void v_scale(float* out, const float* a, float s) { out[0] = a[0] * s; out[1] = a[1] * s; out[2] = a[2] * s; }
static float v_dot(const float* a, const float* b) { return a[0] * b[0] + a[1] * b[1] + a[2] * b[2]; }
static float v_length(const float* a) { return SDL_sqrtf(v_dot(a, a)); }

static void v_cross(float* out, const float* a, const float* b)
{
    float x = a[1] * b[2] - a[2] * b[1];
    float y = a[2] * b[0] - a[0] * b[2];
    float z = a[0] * b[1] - a[1] * b[0];
    v_set(out, x, y, z);
}

static int v_normalize(float* a)
{
    float length = v_length(a);

    if (length < 1e-12f)
    {
        return 0;
    }

    v_scale(a, a, 1.0f / length);
    return 1;
}

static void* grow(void* data, int* capacity, int needed, size_t element_size)
{
    if (needed <= *capacity)
    {
        return data;
    }

    int new_capacity = SDL_max(needed, SDL_max(16, *capacity * 2));
    data = SDL_realloc(data, (size_t)new_capacity * element_size);
    *capacity = new_capacity;

    return data;
}

static void ensure_vertices(EditMesh* mesh, int count)
{
    if (count <= mesh->vertex_capacity)
    {
        return;
    }

    int capacity = mesh->vertex_capacity;
    mesh->positions = grow(mesh->positions, &capacity, count, sizeof(float) * 3);
    mesh->vertex_selected = SDL_realloc(mesh->vertex_selected, (size_t)capacity);
    mesh->vertex_capacity = capacity;
}

// --- Creation ---------------------------------------------------------------------------------------

EditMesh* edit_mesh_create(void)
{
    return SDL_calloc(1, sizeof(EditMesh));
}

void edit_mesh_destroy(EditMesh* mesh)
{
    if (!mesh)
    {
        return;
    }

    SDL_free(mesh->positions);
    SDL_free(mesh->vertex_selected);
    SDL_free(mesh->loops);
    SDL_free(mesh->faces);
    SDL_free(mesh->edges);
    SDL_free(mesh->materials);
    SDL_free(mesh);
}

static void* duplicate(const void* data, size_t size)
{
    if (!data || size == 0)
    {
        return NULL;
    }

    void* copy = SDL_malloc(size);
    SDL_memcpy(copy, data, size);
    return copy;
}

EditMesh* edit_mesh_clone(const EditMesh* mesh)
{
    EditMesh* copy = edit_mesh_create();

    copy->positions = duplicate(mesh->positions, sizeof(float) * 3 * (size_t)mesh->vertex_count);
    copy->vertex_selected = duplicate(mesh->vertex_selected, (size_t)mesh->vertex_count);
    copy->vertex_count = copy->vertex_capacity = mesh->vertex_count;

    copy->loops = duplicate(mesh->loops, sizeof(int) * (size_t)mesh->loop_count);
    copy->loop_count = copy->loop_capacity = mesh->loop_count;

    copy->faces = duplicate(mesh->faces, sizeof(EditFace) * (size_t)mesh->face_count);
    copy->face_count = copy->face_capacity = mesh->face_count;

    copy->edges = duplicate(mesh->edges, sizeof(EditEdge) * (size_t)mesh->edge_count);
    copy->edge_count = copy->edge_capacity = mesh->edge_count;

    copy->materials = duplicate(mesh->materials, EDIT_MESH_MATERIAL_SIZE * (size_t)mesh->material_count);
    copy->material_count = copy->material_capacity = mesh->material_count;

    copy->revision = mesh->revision;

    return copy;
}

int edit_mesh_add_vertex(EditMesh* mesh, const float position[3])
{
    ensure_vertices(mesh, mesh->vertex_count + 1);

    int index = mesh->vertex_count++;
    v_copy(&mesh->positions[index * 3], position);
    mesh->vertex_selected[index] = 0;

    return index;
}

static int add_vertex_xyz(EditMesh* mesh, float x, float y, float z)
{
    const float p[3] = { x, y, z };
    return edit_mesh_add_vertex(mesh, p);
}

void edit_mesh_default_style(EditFaceStyle* out, int material)
{
    SDL_zerop(out);
    out->material = material;
    out->align = EDIT_UV_WORLD;
    out->uv_scale[0] = out->uv_scale[1] = 1.0f;
}

int edit_mesh_add_face(EditMesh* mesh, const int* vertices, int count, const EditFaceStyle* style)
{
    mesh->loops = grow(mesh->loops, &mesh->loop_capacity, mesh->loop_count + count, sizeof(int));
    mesh->faces = grow(mesh->faces, &mesh->face_capacity, mesh->face_count + 1, sizeof(EditFace));

    EditFace* face = &mesh->faces[mesh->face_count];
    face->first = mesh->loop_count;
    face->count = count;
    face->selected = 0;

    if (style)
    {
        face->style = *style;
    }
    else
    {
        edit_mesh_default_style(&face->style, 0);
    }

    SDL_memcpy(&mesh->loops[mesh->loop_count], vertices, sizeof(int) * (size_t)count);
    mesh->loop_count += count;

    return mesh->face_count++;
}

int edit_mesh_material(EditMesh* mesh, const char* path)
{
    for (int i = 0; i < mesh->material_count; i++)
    {
        if (SDL_strcmp(mesh->materials[i], path) == 0)
        {
            return i;
        }
    }

    mesh->materials = grow(mesh->materials, &mesh->material_capacity, mesh->material_count + 1, EDIT_MESH_MATERIAL_SIZE);
    SDL_snprintf(mesh->materials[mesh->material_count], EDIT_MESH_MATERIAL_SIZE, "%s", path);

    return mesh->material_count++;
}

// --- Polygons : a working copy of the faces for the operations that reshape them ---------------------

typedef struct
{
    int* v;
    int n;
    int capacity;
    EditFaceStyle style;
    int selected;
    int alive;
} Poly;

typedef struct
{
    Poly* items;
    int count;
    int capacity;
} PolyList;

static void poly_reserve(Poly* poly, int count)
{
    poly->v = grow(poly->v, &poly->capacity, count, sizeof(int));
}

static void poly_push(Poly* poly, int vertex)
{
    poly_reserve(poly, poly->n + 1);
    poly->v[poly->n++] = vertex;
}

static void poly_insert(Poly* poly, int at, int vertex)
{
    poly_reserve(poly, poly->n + 1);
    SDL_memmove(&poly->v[at + 1], &poly->v[at], sizeof(int) * (size_t)(poly->n - at));
    poly->v[at] = vertex;
    poly->n++;
}

static Poly* polys_add(PolyList* list, const int* vertices, int count, const EditFaceStyle* style, int selected)
{
    list->items = grow(list->items, &list->capacity, list->count + 1, sizeof(Poly));

    Poly* poly = &list->items[list->count++];
    SDL_zerop(poly);
    poly_reserve(poly, SDL_max(count, 4));

    if (vertices)
    {
        SDL_memcpy(poly->v, vertices, sizeof(int) * (size_t)count);
        poly->n = count;
    }

    poly->style = *style;
    poly->selected = selected;
    poly->alive = 1;

    return poly;
}

static void polys_from_mesh(const EditMesh* mesh, PolyList* out)
{
    SDL_zerop(out);

    for (int f = 0; f < mesh->face_count; f++)
    {
        const EditFace* face = &mesh->faces[f];
        polys_add(out, &mesh->loops[face->first], face->count, &face->style, face->selected);
    }
}

// Replaces the mesh's faces by the list's live ones (and frees the list). Doesn't update the mesh.
static void polys_to_mesh(EditMesh* mesh, PolyList* list)
{
    mesh->loop_count = 0;
    mesh->face_count = 0;

    for (int i = 0; i < list->count; i++)
    {
        Poly* poly = &list->items[i];

        if (poly->alive && poly->n >= 3)
        {
            int f = edit_mesh_add_face(mesh, poly->v, poly->n, &poly->style);
            mesh->faces[f].selected = poly->selected;
        }

        SDL_free(poly->v);
    }

    SDL_free(list->items);
    SDL_zerop(list);
}

// Marks an edge to be selected once the edges are rebuilt (edit_mesh_update() carries the selection of
// the edges array's pairs over to the new edges, whether the pair was already there or not).
static void mark_edge(EditMesh* mesh, int a, int b)
{
    mesh->edges = grow(mesh->edges, &mesh->edge_capacity, mesh->edge_count + 1, sizeof(EditEdge));

    EditEdge* edge = &mesh->edges[mesh->edge_count++];
    edge->a = SDL_min(a, b);
    edge->b = SDL_max(a, b);
    edge->selected = 1;
}

static void deselect_edges(EditMesh* mesh)
{
    for (int e = 0; e < mesh->edge_count; e++)
    {
        mesh->edges[e].selected = 0;
    }
}

// --- Topology ---------------------------------------------------------------------------------------

static int compare_pairs(const void* a, const void* b)
{
    const int* p = a;
    const int* q = b;

    if (p[0] != q[0])
    {
        return p[0] < q[0] ? -1 : 1;
    }

    return p[1] < q[1] ? -1 : (p[1] > q[1] ? 1 : 0);
}

static int find_pair(const int* pairs, int count, int a, int b)
{
    int lo = 0;
    int hi = count - 1;

    while (lo <= hi)
    {
        int mid = (lo + hi) / 2;
        const int* p = &pairs[mid * 2];

        if (p[0] == a && p[1] == b)
        {
            return mid;
        }

        if (p[0] < a || (p[0] == a && p[1] < b))
        {
            lo = mid + 1;
        }
        else
        {
            hi = mid - 1;
        }
    }

    return -1;
}

// Removes repeated consecutive vertices and "spikes" (a, b, a) ; returns the remaining count.
static int clean_polygon(int* v, int n)
{
    int changed = 1;

    while (changed && n >= 3)
    {
        changed = 0;

        for (int i = 0; i < n; i++)
        {
            int next = (i + 1) % n;

            if (v[i] == v[next])
            {
                SDL_memmove(&v[next], &v[next + 1], sizeof(int) * (size_t)(n - next - 1));
                n--;
                changed = 1;
                break;
            }
        }

        if (changed || n < 3)
        {
            continue;
        }

        for (int i = 0; i < n; i++)
        {
            int previous = (i + n - 1) % n;
            int next = (i + 1) % n;

            if (v[previous] == v[next])
            {
                int first = SDL_min(i, next);
                int second = SDL_max(i, next);
                SDL_memmove(&v[second], &v[second + 1], sizeof(int) * (size_t)(n - second - 1));
                n--;
                SDL_memmove(&v[first], &v[first + 1], sizeof(int) * (size_t)(n - first - 1));
                n--;
                changed = 1;
                break;
            }
        }
    }

    return n;
}

void edit_mesh_update(EditMesh* mesh)
{
    // The selected edges, as vertex pairs, to select them again afterwards.
    int selected_count = 0;
    int* selected = SDL_malloc(sizeof(int) * 2 * (size_t)SDL_max(1, mesh->edge_count));

    for (int e = 0; e < mesh->edge_count; e++)
    {
        if (mesh->edges[e].selected)
        {
            selected[selected_count * 2] = mesh->edges[e].a;
            selected[selected_count * 2 + 1] = mesh->edges[e].b;
            selected_count++;
        }
    }

    // Faces, cleaned.
    int* loops = SDL_malloc(sizeof(int) * (size_t)SDL_max(1, mesh->loop_count));
    int loop_count = 0;
    int face_count = 0;

    for (int f = 0; f < mesh->face_count; f++)
    {
        EditFace face = mesh->faces[f];
        int* v = &loops[loop_count];
        SDL_memcpy(v, &mesh->loops[face.first], sizeof(int) * (size_t)face.count);

        int n = clean_polygon(v, face.count);

        if (n < 3)
        {
            continue;
        }

        face.first = loop_count;
        face.count = n;
        mesh->faces[face_count++] = face;
        loop_count += n;
    }

    SDL_free(mesh->loops);
    mesh->loops = loops;
    mesh->loop_count = loop_count;
    mesh->loop_capacity = SDL_max(1, mesh->loop_count);
    mesh->face_count = face_count;

    // Unused vertices go.
    int old_vertex_count = mesh->vertex_count;
    int* remap = SDL_malloc(sizeof(int) * (size_t)SDL_max(1, mesh->vertex_count));

    for (int i = 0; i < mesh->vertex_count; i++)
    {
        remap[i] = -1;
    }

    for (int i = 0; i < mesh->loop_count; i++)
    {
        remap[mesh->loops[i]] = 0;
    }

    int vertex_count = 0;

    for (int i = 0; i < mesh->vertex_count; i++)
    {
        if (remap[i] == 0)
        {
            remap[i] = vertex_count;
            v_copy(&mesh->positions[vertex_count * 3], &mesh->positions[i * 3]);
            mesh->vertex_selected[vertex_count] = mesh->vertex_selected[i];
            vertex_count++;
        }
    }

    mesh->vertex_count = vertex_count;

    for (int i = 0; i < mesh->loop_count; i++)
    {
        mesh->loops[i] = remap[mesh->loops[i]];
    }

    int kept = 0;

    for (int i = 0; i < selected_count; i++)
    {
        int a = selected[i * 2] < 0 || selected[i * 2] >= old_vertex_count ? -1 : remap[selected[i * 2]];
        int b = selected[i * 2 + 1] < 0 || selected[i * 2 + 1] >= old_vertex_count ? -1 : remap[selected[i * 2 + 1]];

        if (a >= 0 && b >= 0)
        {
            selected[kept * 2] = SDL_min(a, b);
            selected[kept * 2 + 1] = SDL_max(a, b);
            kept++;
        }
    }

    selected_count = kept;
    SDL_qsort(selected, (size_t)selected_count, sizeof(int) * 2, compare_pairs);
    SDL_free(remap);

    // Edges.
    int* pairs = SDL_malloc(sizeof(int) * 2 * (size_t)SDL_max(1, mesh->loop_count));
    int pair_count = 0;

    for (int f = 0; f < mesh->face_count; f++)
    {
        const EditFace* face = &mesh->faces[f];

        for (int i = 0; i < face->count; i++)
        {
            int a = mesh->loops[face->first + i];
            int b = mesh->loops[face->first + (i + 1) % face->count];
            pairs[pair_count * 2] = SDL_min(a, b);
            pairs[pair_count * 2 + 1] = SDL_max(a, b);
            pair_count++;
        }
    }

    SDL_qsort(pairs, (size_t)pair_count, sizeof(int) * 2, compare_pairs);

    mesh->edge_count = 0;

    for (int i = 0; i < pair_count; i++)
    {
        if (i > 0 && pairs[i * 2] == pairs[(i - 1) * 2] && pairs[i * 2 + 1] == pairs[(i - 1) * 2 + 1])
        {
            continue;
        }

        mesh->edges = grow(mesh->edges, &mesh->edge_capacity, mesh->edge_count + 1, sizeof(EditEdge));
        EditEdge* edge = &mesh->edges[mesh->edge_count++];
        edge->a = pairs[i * 2];
        edge->b = pairs[i * 2 + 1];
        edge->selected = find_pair(selected, selected_count, edge->a, edge->b) >= 0;
    }

    SDL_free(pairs);
    SDL_free(selected);

    mesh->revision++;
}

void edit_mesh_touch(EditMesh* mesh)
{
    mesh->revision++;
}

// Faces around each edge : for edge e, entries offsets[e] .. offsets[e + 1] - 1, each a face and the
// corner where the edge starts in it (from loop[corner] to loop[corner + 1]).
typedef struct
{
    int* offsets;
    int* faces;
    int* corners;
} Adjacency;

static void adjacency_build(const EditMesh* mesh, Adjacency* adjacency)
{
    adjacency->offsets = SDL_calloc((size_t)mesh->edge_count + 1, sizeof(int));
    adjacency->faces = SDL_malloc(sizeof(int) * (size_t)SDL_max(1, mesh->loop_count));
    adjacency->corners = SDL_malloc(sizeof(int) * (size_t)SDL_max(1, mesh->loop_count));

    int* edge_of_loop = SDL_malloc(sizeof(int) * (size_t)SDL_max(1, mesh->loop_count));

    for (int f = 0; f < mesh->face_count; f++)
    {
        const EditFace* face = &mesh->faces[f];

        for (int i = 0; i < face->count; i++)
        {
            int e = edit_mesh_find_edge(mesh, mesh->loops[face->first + i], mesh->loops[face->first + (i + 1) % face->count]);
            edge_of_loop[face->first + i] = e;

            if (e >= 0)
            {
                adjacency->offsets[e + 1]++;
            }
        }
    }

    for (int e = 0; e < mesh->edge_count; e++)
    {
        adjacency->offsets[e + 1] += adjacency->offsets[e];
    }

    int* cursor = SDL_malloc(sizeof(int) * (size_t)SDL_max(1, mesh->edge_count));
    SDL_memcpy(cursor, adjacency->offsets, sizeof(int) * (size_t)mesh->edge_count);

    for (int f = 0; f < mesh->face_count; f++)
    {
        const EditFace* face = &mesh->faces[f];

        for (int i = 0; i < face->count; i++)
        {
            int e = edge_of_loop[face->first + i];

            if (e >= 0)
            {
                adjacency->faces[cursor[e]] = f;
                adjacency->corners[cursor[e]] = i;
                cursor[e]++;
            }
        }
    }

    SDL_free(cursor);
    SDL_free(edge_of_loop);
}

static void adjacency_free(Adjacency* adjacency)
{
    SDL_free(adjacency->offsets);
    SDL_free(adjacency->faces);
    SDL_free(adjacency->corners);
}

// --- Queries ----------------------------------------------------------------------------------------

const float* edit_mesh_position(const EditMesh* mesh, int vertex)
{
    return &mesh->positions[vertex * 3];
}

int edit_mesh_face_vertex(const EditMesh* mesh, int face, int corner)
{
    const EditFace* f = &mesh->faces[face];
    return mesh->loops[f->first + ((corner % f->count) + f->count) % f->count];
}

static void polygon_normal(const EditMesh* mesh, const int* v, int n, float out[3])
{
    v_set(out, 0.0f, 0.0f, 0.0f);

    for (int i = 0; i < n; i++)
    {
        const float* a = edit_mesh_position(mesh, v[i]);
        const float* b = edit_mesh_position(mesh, v[(i + 1) % n]);
        out[0] += (a[1] - b[1]) * (a[2] + b[2]);
        out[1] += (a[2] - b[2]) * (a[0] + b[0]);
        out[2] += (a[0] - b[0]) * (a[1] + b[1]);
    }

    if (!v_normalize(out))
    {
        v_set(out, 0.0f, 1.0f, 0.0f);
    }
}

void edit_mesh_face_normal(const EditMesh* mesh, int face, float out[3])
{
    const EditFace* f = &mesh->faces[face];
    polygon_normal(mesh, &mesh->loops[f->first], f->count, out);
}

void edit_mesh_face_center(const EditMesh* mesh, int face, float out[3])
{
    const EditFace* f = &mesh->faces[face];
    v_set(out, 0.0f, 0.0f, 0.0f);

    for (int i = 0; i < f->count; i++)
    {
        v_add(out, out, edit_mesh_position(mesh, mesh->loops[f->first + i]));
    }

    v_scale(out, out, 1.0f / (float)SDL_max(1, f->count));
}

void edit_mesh_bounds(const EditMesh* mesh, float out[6])
{
    if (mesh->vertex_count == 0)
    {
        SDL_memset(out, 0, sizeof(float) * 6);
        return;
    }

    for (int axis = 0; axis < 3; axis++)
    {
        out[axis] = 1e30f;
        out[axis + 3] = -1e30f;
    }

    for (int i = 0; i < mesh->vertex_count; i++)
    {
        for (int axis = 0; axis < 3; axis++)
        {
            out[axis] = SDL_min(out[axis], mesh->positions[i * 3 + axis]);
            out[axis + 3] = SDL_max(out[axis + 3], mesh->positions[i * 3 + axis]);
        }
    }
}

int edit_mesh_find_edge(const EditMesh* mesh, int a, int b)
{
    int lo_vertex = SDL_min(a, b);
    int hi_vertex = SDL_max(a, b);
    int lo = 0;
    int hi = mesh->edge_count - 1;

    while (lo <= hi)
    {
        int mid = (lo + hi) / 2;
        const EditEdge* edge = &mesh->edges[mid];

        if (edge->a == lo_vertex && edge->b == hi_vertex)
        {
            return mid;
        }

        if (edge->a < lo_vertex || (edge->a == lo_vertex && edge->b < hi_vertex))
        {
            lo = mid + 1;
        }
        else
        {
            hi = mid - 1;
        }
    }

    return -1;
}

int edit_mesh_edge_face_count(const EditMesh* mesh, int edge)
{
    int a = mesh->edges[edge].a;
    int b = mesh->edges[edge].b;
    int count = 0;

    for (int f = 0; f < mesh->face_count; f++)
    {
        const EditFace* face = &mesh->faces[f];

        for (int i = 0; i < face->count; i++)
        {
            int p = mesh->loops[face->first + i];
            int q = mesh->loops[face->first + (i + 1) % face->count];

            if ((p == a && q == b) || (p == b && q == a))
            {
                count++;
            }
        }
    }

    return count;
}

static int dominant_axis(const float n[3])
{
    float x = SDL_fabsf(n[0]);
    float y = SDL_fabsf(n[1]);
    float z = SDL_fabsf(n[2]);

    if (y >= x && y >= z)
    {
        return 1;
    }

    return x >= z ? 0 : 2;
}

static int point_in_face(const EditMesh* mesh, int face, const float normal[3], const float point[3])
{
    const EditFace* f = &mesh->faces[face];
    int axis = dominant_axis(normal);
    int i_axis = axis == 0 ? 1 : 0;
    int j_axis = axis == 2 ? 1 : 2;
    float px = point[i_axis];
    float py = point[j_axis];
    int inside = 0;

    for (int i = 0, j = f->count - 1; i < f->count; j = i++)
    {
        const float* a = edit_mesh_position(mesh, mesh->loops[f->first + i]);
        const float* b = edit_mesh_position(mesh, mesh->loops[f->first + j]);
        float ax = a[i_axis], ay = a[j_axis];
        float bx = b[i_axis], by = b[j_axis];

        if ((ay > py) != (by > py) && px < (bx - ax) * (py - ay) / (by - ay) + ax)
        {
            inside = !inside;
        }
    }

    return inside;
}

int edit_mesh_raycast(const EditMesh* mesh, const float origin[3], const float direction[3], float* inout_t, int* out_face)
{
    int hit = 0;

    for (int f = 0; f < mesh->face_count; f++)
    {
        float normal[3];
        edit_mesh_face_normal(mesh, f, normal);

        float denominator = v_dot(normal, direction);

        if (SDL_fabsf(denominator) < 1e-12f)
        {
            continue;
        }

        const float* p0 = edit_mesh_position(mesh, mesh->loops[mesh->faces[f].first]);
        float t = (v_dot(normal, p0) - v_dot(normal, origin)) / denominator;

        if (t <= 1e-6f || t >= *inout_t)
        {
            continue;
        }

        float point[3];
        v_scale(point, direction, t);
        v_add(point, point, origin);

        if (point_in_face(mesh, f, normal, point))
        {
            *inout_t = t;
            *out_face = f;
            hit = 1;
        }
    }

    return hit;
}

int edit_mesh_triangulate_face(const EditMesh* mesh, int face, int* out)
{
    const EditFace* f = &mesh->faces[face];
    int n = f->count;

    if (n < 3)
    {
        return 0;
    }

    if (n == 3)
    {
        out[0] = 0;
        out[1] = 1;
        out[2] = 2;
        return 1;
    }

    float normal[3];
    edit_mesh_face_normal(mesh, face, normal);

    int axis = dominant_axis(normal);
    int i_axis = axis == 0 ? 1 : 0;
    int j_axis = axis == 2 ? 1 : 2;

    float stack_points[64 * 2];
    int stack_indices[64];
    float* points = n <= 64 ? stack_points : SDL_malloc(sizeof(float) * 2 * (size_t)n);
    int* indices = n <= 64 ? stack_indices : SDL_malloc(sizeof(int) * (size_t)n);

    float lo_x = 1e30f, lo_y = 1e30f, hi_x = -1e30f, hi_y = -1e30f;

    for (int i = 0; i < n; i++)
    {
        const float* p = edit_mesh_position(mesh, mesh->loops[f->first + i]);
        points[i * 2] = p[i_axis];
        points[i * 2 + 1] = p[j_axis];
        lo_x = SDL_min(lo_x, p[i_axis]);
        lo_y = SDL_min(lo_y, p[j_axis]);
        hi_x = SDL_max(hi_x, p[i_axis]);
        hi_y = SDL_max(hi_y, p[j_axis]);
        indices[i] = i;
    }

    // Normalized, so the tolerances below don't depend on the face's size.
    float extent = SDL_max(1e-9f, SDL_max(hi_x - lo_x, hi_y - lo_y));

    for (int i = 0; i < n; i++)
    {
        points[i * 2] = (points[i * 2] - lo_x) / extent;
        points[i * 2 + 1] = (points[i * 2 + 1] - lo_y) / extent;
    }

    float area = 0.0f;

    for (int i = 0; i < n; i++)
    {
        const float* a = &points[i * 2];
        const float* b = &points[((i + 1) % n) * 2];
        area += a[0] * b[1] - b[0] * a[1];
    }

    float orientation = area >= 0.0f ? 1.0f : -1.0f;
    int remaining = n;
    int triangles = 0;

    while (remaining > 3)
    {
        int found = 0;

        for (int k = 0; k < remaining && !found; k++)
        {
            int i0 = indices[(k + remaining - 1) % remaining];
            int i1 = indices[k];
            int i2 = indices[(k + 1) % remaining];
            const float* a = &points[i0 * 2];
            const float* b = &points[i1 * 2];
            const float* c = &points[i2 * 2];

            float cross = ((b[0] - a[0]) * (c[1] - b[1]) - (b[1] - a[1]) * (c[0] - b[0])) * orientation;

            if (cross <= 1e-9f)
            {
                continue; // reflex or flat : not an ear
            }

            int blocked = 0;

            for (int m = 0; m < remaining && !blocked; m++)
            {
                int other = indices[m];

                if (other == i0 || other == i1 || other == i2)
                {
                    continue;
                }

                const float* p = &points[other * 2];
                float d0 = ((b[0] - a[0]) * (p[1] - a[1]) - (b[1] - a[1]) * (p[0] - a[0])) * orientation;
                float d1 = ((c[0] - b[0]) * (p[1] - b[1]) - (c[1] - b[1]) * (p[0] - b[0])) * orientation;
                float d2 = ((a[0] - c[0]) * (p[1] - c[1]) - (a[1] - c[1]) * (p[0] - c[0])) * orientation;
                blocked = d0 > 1e-9f && d1 > 1e-9f && d2 > 1e-9f;
            }

            if (blocked)
            {
                continue;
            }

            out[triangles * 3] = i0;
            out[triangles * 3 + 1] = i1;
            out[triangles * 3 + 2] = i2;
            triangles++;

            SDL_memmove(&indices[k], &indices[k + 1], sizeof(int) * (size_t)(remaining - k - 1));
            remaining--;
            found = 1;
        }

        if (!found)
        {
            break; // degenerate : a fan for what's left
        }
    }

    for (int k = 1; k + 1 < remaining; k++)
    {
        out[triangles * 3] = indices[0];
        out[triangles * 3 + 1] = indices[k];
        out[triangles * 3 + 2] = indices[k + 1];
        triangles++;
    }

    if (points != stack_points)
    {
        SDL_free(points);
    }

    if (indices != stack_indices)
    {
        SDL_free(indices);
    }

    return triangles;
}

// The world-aligned texture axes of a face pointing along `normal` : u along the closest axis plane's
// horizontal direction (seen from the front), v pointing down.
static void world_axes(const float normal[3], float u[3], float v[3])
{
    int axis = dominant_axis(normal);
    float sign = normal[axis] >= 0.0f ? 1.0f : -1.0f;
    float n_axis[3] = { 0.0f, 0.0f, 0.0f };
    n_axis[axis] = sign;

    if (axis == 0)
    {
        v_set(u, 0.0f, 0.0f, -sign);
    }
    else if (axis == 2)
    {
        v_set(u, sign, 0.0f, 0.0f);
    }
    else
    {
        v_set(u, 1.0f, 0.0f, 0.0f);
    }

    v_cross(v, u, n_axis);
}

void edit_mesh_texture_axes(const EditMesh* mesh, int face, float out_u[4], float out_v[4])
{
    const EditFaceStyle* style = &mesh->faces[face].style;
    float normal[3];
    edit_mesh_face_normal(mesh, face, normal);

    float u[3];
    float v[3];
    world_axes(normal, u, v);

    if (style->align == EDIT_UV_FACE)
    {
        float projected[3];
        v_scale(projected, normal, v_dot(normal, u));
        v_sub(projected, u, projected);

        if (v_normalize(projected))
        {
            v_copy(u, projected);
            v_cross(v, u, normal);
        }
    }

    float angle = style->uv_rotation * DEG_TO_RAD;
    float c = SDL_cosf(angle);
    float s = SDL_sinf(angle);
    float su = SDL_fabsf(style->uv_scale[0]) > 1e-6f ? style->uv_scale[0] : 1.0f;
    float sv = SDL_fabsf(style->uv_scale[1]) > 1e-6f ? style->uv_scale[1] : 1.0f;

    for (int i = 0; i < 3; i++)
    {
        float ru = c * u[i] + s * v[i];
        float rv = -s * u[i] + c * v[i];
        out_u[i] = ru / su;
        out_v[i] = rv / sv;
    }

    out_u[3] = style->uv_offset[0];
    out_v[3] = style->uv_offset[1];
}

int edit_mesh_selection_vertices(const EditMesh* mesh, int element, unsigned char* out)
{
    SDL_memset(out, 0, (size_t)mesh->vertex_count);

    if (element == EDIT_ELEMENT_VERTEX)
    {
        SDL_memcpy(out, mesh->vertex_selected, (size_t)mesh->vertex_count);
    }
    else if (element == EDIT_ELEMENT_EDGE)
    {
        for (int e = 0; e < mesh->edge_count; e++)
        {
            if (mesh->edges[e].selected)
            {
                out[mesh->edges[e].a] = 1;
                out[mesh->edges[e].b] = 1;
            }
        }
    }
    else
    {
        for (int f = 0; f < mesh->face_count; f++)
        {
            if (mesh->faces[f].selected)
            {
                for (int i = 0; i < mesh->faces[f].count; i++)
                {
                    out[mesh->loops[mesh->faces[f].first + i]] = 1;
                }
            }
        }
    }

    int count = 0;

    for (int i = 0; i < mesh->vertex_count; i++)
    {
        count += out[i] != 0;
    }

    return count;
}

int edit_mesh_selection_count(const EditMesh* mesh, int element)
{
    int count = 0;

    if (element == EDIT_ELEMENT_VERTEX)
    {
        for (int i = 0; i < mesh->vertex_count; i++)
        {
            count += mesh->vertex_selected[i] != 0;
        }
    }
    else if (element == EDIT_ELEMENT_EDGE)
    {
        for (int e = 0; e < mesh->edge_count; e++)
        {
            count += mesh->edges[e].selected != 0;
        }
    }
    else
    {
        for (int f = 0; f < mesh->face_count; f++)
        {
            count += mesh->faces[f].selected != 0;
        }
    }

    return count;
}

// --- Primitives -------------------------------------------------------------------------------------

static const char* const shape_names[EDIT_SHAPE_COUNT] = { "Box", "Wedge", "Cylinder", "Cone", "Stairs", "Arch", "Sphere" };

const char* edit_mesh_shape_name(int shape)
{
    return shape >= 0 && shape < EDIT_SHAPE_COUNT ? shape_names[shape] : "?";
}

void edit_mesh_default_shape(EditShapeParams* out, int shape)
{
    SDL_zerop(out);
    out->shape = shape;
    out->sides = shape == EDIT_SHAPE_ARCH ? 8 : shape == EDIT_SHAPE_SPHERE ? 12 : 16;
    out->steps = 8;
    out->thickness = 0.5f;
}

// A prism : a 2D profile (counter-clockwise in its (s, t) coordinates) extruded along e, with s x t = e.
static void make_prism(EditMesh* mesh, const float* profile, int count, int s_axis, int t_axis, int e_axis, float e_min, float e_max, int material)
{
    int base = mesh->vertex_count;

    for (int side = 0; side < 2; side++)
    {
        for (int i = 0; i < count; i++)
        {
            float p[3];
            p[s_axis] = profile[i * 2];
            p[t_axis] = profile[i * 2 + 1];
            p[e_axis] = side == 0 ? e_min : e_max;
            edit_mesh_add_vertex(mesh, p);
        }
    }

    EditFaceStyle style;
    edit_mesh_default_style(&style, material);

    int* cap = SDL_malloc(sizeof(int) * (size_t)count);

    for (int i = 0; i < count; i++)
    {
        cap[i] = base + count + i; // front : as is
    }

    edit_mesh_add_face(mesh, cap, count, &style);

    for (int i = 0; i < count; i++)
    {
        cap[i] = base + count - 1 - i; // back : reversed
    }

    edit_mesh_add_face(mesh, cap, count, &style);
    SDL_free(cap);

    for (int i = 0; i < count; i++)
    {
        int j = (i + 1) % count;
        int quad[4] = { base + i, base + j, base + count + j, base + count + i };
        edit_mesh_add_face(mesh, quad, 4, &style);
    }
}

// Convex shapes : every face made to point away from the center, whatever order it was built in.
static void orient_outward(EditMesh* mesh)
{
    float bounds[6];
    edit_mesh_bounds(mesh, bounds);
    float center[3] = { (bounds[0] + bounds[3]) * 0.5f, (bounds[1] + bounds[4]) * 0.5f, (bounds[2] + bounds[5]) * 0.5f };

    for (int f = 0; f < mesh->face_count; f++)
    {
        float normal[3];
        float face_center[3];
        edit_mesh_face_normal(mesh, f, normal);
        edit_mesh_face_center(mesh, f, face_center);
        v_sub(face_center, face_center, center);

        if (v_dot(normal, face_center) < 0.0f)
        {
            int* v = &mesh->loops[mesh->faces[f].first];
            int n = mesh->faces[f].count;

            for (int i = 0; i < n / 2; i++)
            {
                int swap = v[i];
                v[i] = v[n - 1 - i];
                v[n - 1 - i] = swap;
            }
        }
    }
}

EditMesh* edit_mesh_make_shape(const EditShapeParams* params, const float size[3], const char* material_path)
{
    EditMesh* mesh = edit_mesh_create();
    int material = edit_mesh_material(mesh, material_path);

    float hx = SDL_fabsf(size[0]) * 0.5f;
    float hy = SDL_fabsf(size[1]) * 0.5f;
    float hz = SDL_fabsf(size[2]) * 0.5f;

    EditFaceStyle style;
    edit_mesh_default_style(&style, material);

    switch (params->shape)
    {
    case EDIT_SHAPE_WEDGE:
    {
        // Profile in (y, z), extruded along x (y x z = x) : front bottom, back bottom, back top.
        const float profile[6] = { -hy, hz, -hy, -hz, hy, -hz };
        make_prism(mesh, profile, 3, 1, 2, 0, -hx, hx, material);
        break;
    }

    case EDIT_SHAPE_CYLINDER:
    case EDIT_SHAPE_CONE:
    {
        int sides = SDL_clamp(params->sides, 3, 128);

        if (params->shape == EDIT_SHAPE_CYLINDER)
        {
            // Profile in (z, x), extruded along y (z x x = y).
            float* profile = SDL_malloc(sizeof(float) * 2 * (size_t)sides);

            for (int i = 0; i < sides; i++)
            {
                float a = 2.0f * SDL_PI_F * (float)i / (float)sides;
                profile[i * 2] = SDL_cosf(a) * hz;
                profile[i * 2 + 1] = SDL_sinf(a) * hx;
            }

            make_prism(mesh, profile, sides, 2, 0, 1, -hy, hy, material);
            SDL_free(profile);
        }
        else
        {
            int* base = SDL_malloc(sizeof(int) * (size_t)sides);

            for (int i = 0; i < sides; i++)
            {
                float a = 2.0f * SDL_PI_F * (float)i / (float)sides;
                base[i] = add_vertex_xyz(mesh, SDL_sinf(a) * hx, -hy, SDL_cosf(a) * hz);
            }

            int apex = add_vertex_xyz(mesh, 0.0f, hy, 0.0f);
            edit_mesh_add_face(mesh, base, sides, &style);

            for (int i = 0; i < sides; i++)
            {
                int triangle[3] = { base[i], base[(i + 1) % sides], apex };
                edit_mesh_add_face(mesh, triangle, 3, &style);
            }

            SDL_free(base);
        }

        orient_outward(mesh);
        break;
    }

    case EDIT_SHAPE_STAIRS:
    {
        // Profile in (y, z), extruded along x : the bottom, the back, then down the steps to the front.
        int steps = SDL_clamp(params->steps, 1, 64);
        float rise = 2.0f * hy / (float)steps;
        float run = 2.0f * hz / (float)steps;
        float* profile = SDL_malloc(sizeof(float) * 2 * (size_t)(2 * steps + 3));
        int count = 0;

        profile[count * 2] = -hy; profile[count * 2 + 1] = hz; count++;
        profile[count * 2] = -hy; profile[count * 2 + 1] = -hz; count++;
        profile[count * 2] = hy; profile[count * 2 + 1] = -hz; count++;

        for (int i = 0; i < steps; i++)
        {
            float height = hy - (float)i * rise;
            float end = -hz + (float)(i + 1) * run;
            profile[count * 2] = height; profile[count * 2 + 1] = end; count++;

            if (i + 1 < steps)
            {
                profile[count * 2] = height - rise; profile[count * 2 + 1] = end; count++;
            }
        }

        make_prism(mesh, profile, count, 1, 2, 0, -hx, hx, material);
        SDL_free(profile);
        break;
    }

    case EDIT_SHAPE_ARCH:
    {
        // A half ring in the XY plane, standing on the box's bottom, through Z.
        int sides = SDL_clamp(params->sides, 2, 64);
        float thickness = SDL_clamp(params->thickness, 0.01f, SDL_min(hx, 2.0f * hy) * 0.95f);
        float outer_x = hx, outer_y = 2.0f * hy;
        float inner_x = hx - thickness, inner_y = 2.0f * hy - thickness;
        int base = mesh->vertex_count;

        // Per side (back, front), per step : outer then inner.
        for (int side = 0; side < 2; side++)
        {
            for (int i = 0; i <= sides; i++)
            {
                float a = SDL_PI_F * (float)i / (float)sides;
                float z = side == 0 ? -hz : hz;
                add_vertex_xyz(mesh, SDL_cosf(a) * outer_x, -hy + SDL_sinf(a) * outer_y, z);
                add_vertex_xyz(mesh, SDL_cosf(a) * inner_x, -hy + SDL_sinf(a) * inner_y, z);
            }
        }

        int stride = (sides + 1) * 2;
#define ARCH_OUTER(side, i) (base + (side) * stride + (i) * 2)
#define ARCH_INNER(side, i) (base + (side) * stride + (i) * 2 + 1)

        for (int i = 0; i < sides; i++)
        {
            int front[4] = { ARCH_OUTER(1, i), ARCH_OUTER(1, i + 1), ARCH_INNER(1, i + 1), ARCH_INNER(1, i) };
            int back[4] = { ARCH_INNER(0, i), ARCH_INNER(0, i + 1), ARCH_OUTER(0, i + 1), ARCH_OUTER(0, i) };
            int outer[4] = { ARCH_OUTER(0, i), ARCH_OUTER(0, i + 1), ARCH_OUTER(1, i + 1), ARCH_OUTER(1, i) };
            int inner[4] = { ARCH_INNER(0, i + 1), ARCH_INNER(0, i), ARCH_INNER(1, i), ARCH_INNER(1, i + 1) };
            edit_mesh_add_face(mesh, front, 4, &style);
            edit_mesh_add_face(mesh, back, 4, &style);
            edit_mesh_add_face(mesh, outer, 4, &style);
            edit_mesh_add_face(mesh, inner, 4, &style);
        }

        int start[4] = { ARCH_INNER(0, 0), ARCH_OUTER(0, 0), ARCH_OUTER(1, 0), ARCH_INNER(1, 0) };
        int end[4] = { ARCH_OUTER(0, sides), ARCH_INNER(0, sides), ARCH_INNER(1, sides), ARCH_OUTER(1, sides) };
        edit_mesh_add_face(mesh, start, 4, &style);
        edit_mesh_add_face(mesh, end, 4, &style);
#undef ARCH_OUTER
#undef ARCH_INNER
        break;
    }

    case EDIT_SHAPE_SPHERE:
    {
        int segments = SDL_clamp(params->sides, 4, 64);
        int rings = SDL_max(2, segments / 2);
        int top = add_vertex_xyz(mesh, 0.0f, hy, 0.0f);
        int first = mesh->vertex_count;

        for (int r = 1; r < rings; r++)
        {
            float theta = SDL_PI_F * (float)r / (float)rings;

            for (int s = 0; s < segments; s++)
            {
                float phi = 2.0f * SDL_PI_F * (float)s / (float)segments;
                add_vertex_xyz(mesh, SDL_sinf(theta) * SDL_cosf(phi) * hx, SDL_cosf(theta) * hy, SDL_sinf(theta) * SDL_sinf(phi) * hz);
            }
        }

        int bottom = add_vertex_xyz(mesh, 0.0f, -hy, 0.0f);

        for (int s = 0; s < segments; s++)
        {
            int next = (s + 1) % segments;
            int triangle[3] = { top, first + s, first + next };
            edit_mesh_add_face(mesh, triangle, 3, &style);
        }

        for (int r = 0; r + 1 < rings - 1; r++)
        {
            for (int s = 0; s < segments; s++)
            {
                int next = (s + 1) % segments;
                int quad[4] = { first + r * segments + s, first + (r + 1) * segments + s, first + (r + 1) * segments + next, first + r * segments + next };
                edit_mesh_add_face(mesh, quad, 4, &style);
            }
        }

        int last = first + (rings - 2) * segments;

        for (int s = 0; s < segments; s++)
        {
            int next = (s + 1) % segments;
            int triangle[3] = { bottom, last + next, last + s };
            edit_mesh_add_face(mesh, triangle, 3, &style);
        }

        orient_outward(mesh);
        break;
    }

    default:
    {
        // A box : a rectangle in (x, y), extruded along z.
        const float profile[8] = { -hx, -hy, hx, -hy, hx, hy, -hx, hy };
        make_prism(mesh, profile, 4, 0, 1, 2, -hz, hz, material);
        break;
    }
    }

    edit_mesh_update(mesh);

    return mesh;
}

// --- Operations -------------------------------------------------------------------------------------

int edit_mesh_extrude_faces(EditMesh* mesh)
{
    if (edit_mesh_selection_count(mesh, EDIT_ELEMENT_FACE) == 0)
    {
        return 0;
    }

    Adjacency adjacency;
    adjacency_build(mesh, &adjacency);

    int original_vertices = mesh->vertex_count;
    int original_faces = mesh->face_count;
    int* copy_of = SDL_malloc(sizeof(int) * (size_t)SDL_max(1, original_vertices));

    for (int i = 0; i < original_vertices; i++)
    {
        copy_of[i] = -1;
    }

    // The region's border : the edges of selected faces that no other selected face shares.
    typedef struct { int a, b, face; } Border;
    Border* border = SDL_malloc(sizeof(Border) * (size_t)SDL_max(1, mesh->loop_count));
    int border_count = 0;
    int has_neighbors = 0;

    for (int f = 0; f < original_faces; f++)
    {
        const EditFace* face = &mesh->faces[f];

        if (!face->selected)
        {
            continue;
        }

        for (int i = 0; i < face->count; i++)
        {
            int a = mesh->loops[face->first + i];
            int b = mesh->loops[face->first + (i + 1) % face->count];
            int e = edit_mesh_find_edge(mesh, a, b);
            int interior = 0;

            for (int k = e >= 0 ? adjacency.offsets[e] : 0; e >= 0 && k < adjacency.offsets[e + 1]; k++)
            {
                int other = adjacency.faces[k];

                if (other != f)
                {
                    if (mesh->faces[other].selected)
                    {
                        interior = 1;
                    }
                    else
                    {
                        has_neighbors = 1;
                    }
                }
            }

            if (!interior)
            {
                border[border_count].a = a;
                border[border_count].b = b;
                border[border_count].face = f;
                border_count++;
            }
        }
    }

    adjacency_free(&adjacency);

    for (int i = 0; i < original_vertices; i++)
    {
        mesh->vertex_selected[i] = 0;
    }

    for (int f = 0; f < original_faces; f++)
    {
        if (!mesh->faces[f].selected)
        {
            continue;
        }

        for (int i = 0; i < mesh->faces[f].count; i++)
        {
            int v = mesh->loops[mesh->faces[f].first + i];

            if (copy_of[v] < 0)
            {
                float p[3];
                v_copy(p, edit_mesh_position(mesh, v));
                copy_of[v] = edit_mesh_add_vertex(mesh, p);
                mesh->vertex_selected[copy_of[v]] = 1;
            }
        }
    }

    // A lone patch gets a bottom : copies of its faces, reversed, where they were.
    int add_bottom = border_count > 0 && !has_neighbors;

    if (add_bottom)
    {
        for (int f = 0; f < original_faces; f++)
        {
            if (!mesh->faces[f].selected)
            {
                continue;
            }

            int n = mesh->faces[f].count;
            int* reversed = SDL_malloc(sizeof(int) * (size_t)n);

            for (int i = 0; i < n; i++)
            {
                reversed[i] = mesh->loops[mesh->faces[f].first + n - 1 - i];
            }

            EditFaceStyle style = mesh->faces[f].style;
            edit_mesh_add_face(mesh, reversed, n, &style);
            SDL_free(reversed);
        }
    }

    for (int f = 0; f < original_faces; f++)
    {
        if (mesh->faces[f].selected)
        {
            for (int i = 0; i < mesh->faces[f].count; i++)
            {
                int* v = &mesh->loops[mesh->faces[f].first + i];
                *v = copy_of[*v];
            }
        }
    }

    for (int i = 0; i < border_count; i++)
    {
        int quad[4] = { border[i].a, border[i].b, copy_of[border[i].b], copy_of[border[i].a] };
        EditFaceStyle style = mesh->faces[border[i].face].style;
        edit_mesh_add_face(mesh, quad, 4, &style);
    }

    SDL_free(border);
    SDL_free(copy_of);

    edit_mesh_update(mesh);

    return 1;
}

int edit_mesh_extrude_edges(EditMesh* mesh)
{
    Adjacency adjacency;
    adjacency_build(mesh, &adjacency);

    int original_vertices = mesh->vertex_count;
    int* copy_of = SDL_malloc(sizeof(int) * (size_t)SDL_max(1, original_vertices));

    for (int i = 0; i < original_vertices; i++)
    {
        copy_of[i] = -1;
    }

    typedef struct { int a, b, face; } Extension;
    Extension* extensions = SDL_malloc(sizeof(Extension) * (size_t)SDL_max(1, mesh->edge_count));
    int count = 0;

    for (int e = 0; e < mesh->edge_count; e++)
    {
        if (!mesh->edges[e].selected || adjacency.offsets[e + 1] - adjacency.offsets[e] != 1)
        {
            continue;
        }

        int f = adjacency.faces[adjacency.offsets[e]];
        int corner = adjacency.corners[adjacency.offsets[e]];
        extensions[count].a = edit_mesh_face_vertex(mesh, f, corner);
        extensions[count].b = edit_mesh_face_vertex(mesh, f, corner + 1);
        extensions[count].face = f;
        count++;
    }

    adjacency_free(&adjacency);

    if (count == 0)
    {
        SDL_free(extensions);
        SDL_free(copy_of);
        return 0;
    }

    deselect_edges(mesh);

    for (int i = 0; i < original_vertices; i++)
    {
        mesh->vertex_selected[i] = 0;
    }

    for (int i = 0; i < count; i++)
    {
        int ends[2] = { extensions[i].a, extensions[i].b };

        for (int k = 0; k < 2; k++)
        {
            if (copy_of[ends[k]] < 0)
            {
                float p[3];
                v_copy(p, edit_mesh_position(mesh, ends[k]));
                copy_of[ends[k]] = edit_mesh_add_vertex(mesh, p);
                mesh->vertex_selected[copy_of[ends[k]]] = 1;
            }
        }
    }

    for (int i = 0; i < count; i++)
    {
        int a = extensions[i].a;
        int b = extensions[i].b;
        int quad[4] = { b, a, copy_of[a], copy_of[b] };
        EditFaceStyle style = mesh->faces[extensions[i].face].style;
        edit_mesh_add_face(mesh, quad, 4, &style);
        mark_edge(mesh, copy_of[a], copy_of[b]);
    }

    SDL_free(extensions);
    SDL_free(copy_of);

    edit_mesh_update(mesh);

    return count;
}

int edit_mesh_inset_faces(EditMesh* mesh, float distance)
{
    int original_faces = mesh->face_count;
    int count = 0;

    for (int f = 0; f < original_faces; f++)
    {
        if (!mesh->faces[f].selected)
        {
            continue;
        }

        int n = mesh->faces[f].count;
        int* outer = SDL_malloc(sizeof(int) * (size_t)n);
        int* inner = SDL_malloc(sizeof(int) * (size_t)n);
        SDL_memcpy(outer, &mesh->loops[mesh->faces[f].first], sizeof(int) * (size_t)n);

        float normal[3];
        edit_mesh_face_normal(mesh, f, normal);

        for (int i = 0; i < n; i++)
        {
            const float* previous = edit_mesh_position(mesh, outer[(i + n - 1) % n]);
            const float* current = edit_mesh_position(mesh, outer[i]);
            const float* next = edit_mesh_position(mesh, outer[(i + 1) % n]);

            float e1[3], e2[3], in1[3], in2[3], bisector[3];
            v_sub(e1, current, previous);
            v_sub(e2, next, current);
            v_normalize(e1);
            v_normalize(e2);
            v_cross(in1, normal, e1);
            v_cross(in2, normal, e2);
            v_add(bisector, in1, in2);

            float p[3];
            v_copy(p, current);

            if (v_normalize(bisector))
            {
                float miter = v_dot(bisector, in1);
                float length = miter > 0.25f ? distance / miter : distance * 4.0f;
                v_scale(bisector, bisector, length);
                v_add(p, p, bisector);
            }

            inner[i] = edit_mesh_add_vertex(mesh, p);
        }

        EditFaceStyle style = mesh->faces[f].style;

        // The face itself becomes the inner polygon (same vertex count : in place).
        SDL_memcpy(&mesh->loops[mesh->faces[f].first], inner, sizeof(int) * (size_t)n);

        for (int i = 0; i < n; i++)
        {
            int j = (i + 1) % n;
            int quad[4] = { outer[i], outer[j], inner[j], inner[i] };
            edit_mesh_add_face(mesh, quad, 4, &style);
        }

        SDL_free(outer);
        SDL_free(inner);
        count++;
    }

    if (count)
    {
        edit_mesh_update(mesh);
    }

    return count;
}

int edit_mesh_flip_faces(EditMesh* mesh, int selected_only)
{
    int count = 0;

    for (int f = 0; f < mesh->face_count; f++)
    {
        if (selected_only && !mesh->faces[f].selected)
        {
            continue;
        }

        int* v = &mesh->loops[mesh->faces[f].first];
        int n = mesh->faces[f].count;

        for (int i = 0; i < n / 2; i++)
        {
            int swap = v[i];
            v[i] = v[n - 1 - i];
            v[n - 1 - i] = swap;
        }

        count++;
    }

    if (count)
    {
        edit_mesh_update(mesh);
    }

    return count;
}

int edit_mesh_delete_faces(EditMesh* mesh)
{
    PolyList polys;
    polys_from_mesh(mesh, &polys);
    int count = 0;

    for (int i = 0; i < polys.count; i++)
    {
        if (polys.items[i].selected)
        {
            polys.items[i].alive = 0;
            count++;
        }
    }

    polys_to_mesh(mesh, &polys);
    edit_mesh_update(mesh);

    return count;
}

// The poly containing the directed edge a -> b, and where it starts in it ; -1 if none.
static int find_directed(const PolyList* polys, int a, int b, int* out_corner)
{
    for (int i = 0; i < polys->count; i++)
    {
        const Poly* poly = &polys->items[i];

        if (!poly->alive)
        {
            continue;
        }

        for (int k = 0; k < poly->n; k++)
        {
            if (poly->v[k] == a && poly->v[(k + 1) % poly->n] == b)
            {
                *out_corner = k;
                return i;
            }
        }
    }

    return -1;
}

int edit_mesh_dissolve_edges(EditMesh* mesh)
{
    PolyList polys;
    polys_from_mesh(mesh, &polys);
    int count = 0;

    for (int e = 0; e < mesh->edge_count; e++)
    {
        if (!mesh->edges[e].selected)
        {
            continue;
        }

        int a = mesh->edges[e].a;
        int b = mesh->edges[e].b;
        int corner_p = 0;
        int corner_q = 0;
        int p = find_directed(&polys, a, b, &corner_p);
        int q = find_directed(&polys, b, a, &corner_q);

        if (p < 0 || q < 0 || p == q)
        {
            continue;
        }

        Poly* first = &polys.items[p];
        Poly* second = &polys.items[q];

        // first, from b around to a ; then second, strictly between a and b.
        int* merged = SDL_malloc(sizeof(int) * (size_t)(first->n + second->n));
        int n = 0;

        for (int k = 0; k < first->n; k++)
        {
            merged[n++] = first->v[(corner_p + 1 + k) % first->n];
        }

        for (int k = 1; k < second->n - 1; k++)
        {
            merged[n++] = second->v[(corner_q + 1 + k) % second->n];
        }

        first->n = 0;

        for (int k = 0; k < n; k++)
        {
            poly_push(first, merged[k]);
        }

        first->selected = first->selected || second->selected;
        second->alive = 0;
        SDL_free(merged);
        count++;
    }

    deselect_edges(mesh);
    polys_to_mesh(mesh, &polys);
    edit_mesh_update(mesh);

    return count;
}

int edit_mesh_dissolve_vertices(EditMesh* mesh)
{
    int count = edit_mesh_selection_count(mesh, EDIT_ELEMENT_VERTEX);

    if (count == 0)
    {
        return 0;
    }

    PolyList polys;
    polys_from_mesh(mesh, &polys);

    for (int i = 0; i < polys.count; i++)
    {
        Poly* poly = &polys.items[i];
        int n = 0;

        for (int k = 0; k < poly->n; k++)
        {
            if (!mesh->vertex_selected[poly->v[k]])
            {
                poly->v[n++] = poly->v[k];
            }
        }

        poly->n = n;
    }

    polys_to_mesh(mesh, &polys);
    edit_mesh_update(mesh);

    return count;
}

// Every vertex replaced by remap[vertex] (itself if it maps to itself).
static void remap_loops(EditMesh* mesh, const int* remap)
{
    for (int i = 0; i < mesh->loop_count; i++)
    {
        mesh->loops[i] = remap[mesh->loops[i]];
    }
}

int edit_mesh_merge_vertices(EditMesh* mesh)
{
    int target = -1;
    int count = 0;
    float center[3] = { 0.0f, 0.0f, 0.0f };

    for (int i = 0; i < mesh->vertex_count; i++)
    {
        if (mesh->vertex_selected[i])
        {
            target = target < 0 ? i : target;
            v_add(center, center, edit_mesh_position(mesh, i));
            count++;
        }
    }

    if (count < 2)
    {
        return 0;
    }

    v_scale(center, center, 1.0f / (float)count);
    v_copy(&mesh->positions[target * 3], center);

    int* remap = SDL_malloc(sizeof(int) * (size_t)mesh->vertex_count);

    for (int i = 0; i < mesh->vertex_count; i++)
    {
        remap[i] = mesh->vertex_selected[i] ? target : i;
    }

    remap_loops(mesh, remap);
    SDL_free(remap);

    edit_mesh_update(mesh);

    return count;
}

static int find_root(int* parent, int i)
{
    while (parent[i] != i)
    {
        parent[i] = parent[parent[i]];
        i = parent[i];
    }

    return i;
}

static void unite(int* parent, int a, int b)
{
    a = find_root(parent, a);
    b = find_root(parent, b);

    if (a != b)
    {
        parent[SDL_max(a, b)] = SDL_min(a, b);
    }
}

// Each group of vertices joined in `parent` (with more than one member) merged into one, at its center.
static int merge_groups(EditMesh* mesh, int* parent)
{
    int n = mesh->vertex_count;
    float* sums = SDL_calloc((size_t)SDL_max(1, n), sizeof(float) * 3);
    int* counts = SDL_calloc((size_t)SDL_max(1, n), sizeof(int));
    int merged = 0;

    for (int i = 0; i < n; i++)
    {
        int root = find_root(parent, i);
        v_add(&sums[root * 3], &sums[root * 3], edit_mesh_position(mesh, i));
        counts[root]++;
    }

    for (int i = 0; i < n; i++)
    {
        if (counts[i] > 1)
        {
            v_scale(&mesh->positions[i * 3], &sums[i * 3], 1.0f / (float)counts[i]);
            merged += counts[i];
        }
    }

    int* remap = SDL_malloc(sizeof(int) * (size_t)SDL_max(1, n));

    for (int i = 0; i < n; i++)
    {
        remap[i] = find_root(parent, i);
    }

    remap_loops(mesh, remap);

    SDL_free(remap);
    SDL_free(sums);
    SDL_free(counts);

    if (merged)
    {
        edit_mesh_update(mesh);
    }

    return merged;
}

int edit_mesh_collapse(EditMesh* mesh, int element)
{
    if (element == EDIT_ELEMENT_VERTEX)
    {
        return edit_mesh_merge_vertices(mesh);
    }

    int* parent = SDL_malloc(sizeof(int) * (size_t)SDL_max(1, mesh->vertex_count));

    for (int i = 0; i < mesh->vertex_count; i++)
    {
        parent[i] = i;
    }

    if (element == EDIT_ELEMENT_EDGE)
    {
        for (int e = 0; e < mesh->edge_count; e++)
        {
            if (mesh->edges[e].selected)
            {
                unite(parent, mesh->edges[e].a, mesh->edges[e].b);
            }
        }
    }
    else
    {
        for (int f = 0; f < mesh->face_count; f++)
        {
            if (mesh->faces[f].selected)
            {
                for (int i = 1; i < mesh->faces[f].count; i++)
                {
                    unite(parent, mesh->loops[mesh->faces[f].first], mesh->loops[mesh->faces[f].first + i]);
                }
            }
        }
    }

    int merged = merge_groups(mesh, parent);
    SDL_free(parent);

    return merged;
}

typedef struct
{
    float x;
    int index;
} SortedVertex;

static int compare_sorted(const void* a, const void* b)
{
    float x = ((const SortedVertex*)a)->x;
    float y = ((const SortedVertex*)b)->x;
    return x < y ? -1 : (x > y ? 1 : 0);
}

int edit_mesh_weld(EditMesh* mesh, float distance, int selected_only)
{
    int n = mesh->vertex_count;
    int* parent = SDL_malloc(sizeof(int) * (size_t)SDL_max(1, n));
    SortedVertex* sorted = SDL_malloc(sizeof(SortedVertex) * (size_t)SDL_max(1, n));
    int sorted_count = 0;

    for (int i = 0; i < n; i++)
    {
        parent[i] = i;

        if (!selected_only || mesh->vertex_selected[i])
        {
            sorted[sorted_count].x = mesh->positions[i * 3];
            sorted[sorted_count].index = i;
            sorted_count++;
        }
    }

    SDL_qsort(sorted, (size_t)sorted_count, sizeof(SortedVertex), compare_sorted);

    float squared = distance * distance;

    for (int i = 0; i < sorted_count; i++)
    {
        for (int j = i + 1; j < sorted_count && sorted[j].x - sorted[i].x <= distance; j++)
        {
            float d[3];
            v_sub(d, edit_mesh_position(mesh, sorted[i].index), edit_mesh_position(mesh, sorted[j].index));

            if (v_dot(d, d) <= squared)
            {
                unite(parent, sorted[i].index, sorted[j].index);
            }
        }
    }

    int merged = merge_groups(mesh, parent);

    SDL_free(sorted);
    SDL_free(parent);

    return merged;
}

// Inserts `vertex` between a and b (either direction) wherever they follow each other.
static void insert_on_edge(PolyList* polys, int a, int b, int vertex)
{
    for (int i = 0; i < polys->count; i++)
    {
        Poly* poly = &polys->items[i];

        for (int k = 0; k < poly->n; k++)
        {
            int p = poly->v[k];
            int q = poly->v[(k + 1) % poly->n];

            if ((p == a && q == b) || (p == b && q == a))
            {
                poly_insert(poly, k + 1, vertex);
                k++;
            }
        }
    }
}

int edit_mesh_split_edges(EditMesh* mesh)
{
    PolyList polys;
    polys_from_mesh(mesh, &polys);

    int edge_count = mesh->edge_count;
    int* ends = SDL_malloc(sizeof(int) * 2 * (size_t)SDL_max(1, edge_count));
    int count = 0;

    for (int e = 0; e < edge_count; e++)
    {
        if (mesh->edges[e].selected)
        {
            ends[count * 2] = mesh->edges[e].a;
            ends[count * 2 + 1] = mesh->edges[e].b;
            count++;
        }
    }

    deselect_edges(mesh);

    for (int i = 0; i < count; i++)
    {
        int a = ends[i * 2];
        int b = ends[i * 2 + 1];
        float middle[3];
        v_add(middle, edit_mesh_position(mesh, a), edit_mesh_position(mesh, b));
        v_scale(middle, middle, 0.5f);

        int m = edit_mesh_add_vertex(mesh, middle);
        insert_on_edge(&polys, a, b, m);
        mark_edge(mesh, a, m);
        mark_edge(mesh, m, b);
    }

    SDL_free(ends);
    polys_to_mesh(mesh, &polys);
    edit_mesh_update(mesh);

    return count;
}

// Splits a poly between two of its corners i < j (not neighbors) : the poly keeps i .. j, a new one gets j .. i.
static void split_poly(PolyList* polys, int index, int i, int j)
{
    Poly* poly = &polys->items[index];
    int n = poly->n;
    int* first = SDL_malloc(sizeof(int) * (size_t)n);
    int* second = SDL_malloc(sizeof(int) * (size_t)n);
    int first_count = 0;
    int second_count = 0;

    for (int k = i; k <= j; k++)
    {
        first[first_count++] = poly->v[k];
    }

    for (int k = j; k != i; k = (k + 1) % n)
    {
        second[second_count++] = poly->v[k];
    }

    second[second_count++] = poly->v[i];

    EditFaceStyle style = poly->style;
    int selected = poly->selected;

    poly->n = 0;

    for (int k = 0; k < first_count; k++)
    {
        poly_push(poly, first[k]);
    }

    polys_add(polys, second, second_count, &style, selected); // may move polys->items : poly isn't used after

    SDL_free(first);
    SDL_free(second);
}

int edit_mesh_connect(EditMesh* mesh, int element)
{
    PolyList polys;
    polys_from_mesh(mesh, &polys);
    int count = 0;

    if (element == EDIT_ELEMENT_VERTEX)
    {
        int original = polys.count;

        for (int p = 0; p < original; p++)
        {
            int corners[2];
            int found = 0;

            for (int k = 0; k < polys.items[p].n; k++)
            {
                if (mesh->vertex_selected[polys.items[p].v[k]])
                {
                    if (found < 2)
                    {
                        corners[found] = k;
                    }

                    found++;
                }
            }

            int n = polys.items[p].n;

            if (found != 2 || corners[1] - corners[0] == 1 || (corners[0] == 0 && corners[1] == n - 1))
            {
                continue;
            }

            mark_edge(mesh, polys.items[p].v[corners[0]], polys.items[p].v[corners[1]]);
            split_poly(&polys, p, corners[0], corners[1]);
            count++;
        }
    }
    else
    {
        // A middle vertex on every selected edge.
        int edge_count = mesh->edge_count;
        int* middle_of = SDL_malloc(sizeof(int) * (size_t)SDL_max(1, edge_count));
        int original = polys.count;
        int* selected_edges_of = SDL_calloc((size_t)SDL_max(1, original), sizeof(int));

        for (int p = 0; p < original; p++)
        {
            for (int k = 0; k < polys.items[p].n; k++)
            {
                int e = edit_mesh_find_edge(mesh, polys.items[p].v[k], polys.items[p].v[(k + 1) % polys.items[p].n]);
                selected_edges_of[p] += e >= 0 && mesh->edges[e].selected;
            }
        }

        for (int e = 0; e < edge_count; e++)
        {
            middle_of[e] = -1;

            if (mesh->edges[e].selected)
            {
                float middle[3];
                v_add(middle, edit_mesh_position(mesh, mesh->edges[e].a), edit_mesh_position(mesh, mesh->edges[e].b));
                v_scale(middle, middle, 0.5f);
                middle_of[e] = edit_mesh_add_vertex(mesh, middle);
                insert_on_edge(&polys, mesh->edges[e].a, mesh->edges[e].b, middle_of[e]);
            }
        }

        unsigned char* is_middle = SDL_calloc((size_t)SDL_max(1, mesh->vertex_count), 1);

        for (int e = 0; e < edge_count; e++)
        {
            if (middle_of[e] >= 0)
            {
                is_middle[middle_of[e]] = 1;
            }
        }

        deselect_edges(mesh);

        for (int p = 0; p < original; p++)
        {
            if (selected_edges_of[p] != 2)
            {
                continue;
            }

            int corners[2];
            int found = 0;

            for (int k = 0; k < polys.items[p].n && found < 2; k++)
            {
                if (is_middle[polys.items[p].v[k]])
                {
                    corners[found++] = k;
                }
            }

            if (found == 2)
            {
                mark_edge(mesh, polys.items[p].v[corners[0]], polys.items[p].v[corners[1]]);
                split_poly(&polys, p, corners[0], corners[1]);
                count++;
            }
        }

        SDL_free(is_middle);
        SDL_free(selected_edges_of);
        SDL_free(middle_of);
    }

    polys_to_mesh(mesh, &polys);
    edit_mesh_update(mesh);

    return count;
}

int edit_mesh_bridge_faces(EditMesh* mesh)
{
    int first = -1;
    int second = -1;

    for (int f = 0; f < mesh->face_count; f++)
    {
        if (mesh->faces[f].selected)
        {
            if (first < 0)
            {
                first = f;
            }
            else if (second < 0)
            {
                second = f;
            }
            else
            {
                return 0; // exactly two
            }
        }
    }

    if (second < 0 || mesh->faces[first].count != mesh->faces[second].count)
    {
        return 0;
    }

    int n = mesh->faces[first].count;
    const int* a = &mesh->loops[mesh->faces[first].first];
    const int* b = &mesh->loops[mesh->faces[second].first];

    // b traversed backwards from the offset that lines it up best with a.
    int best_offset = 0;
    float best_cost = 1e30f;

    for (int offset = 0; offset < n; offset++)
    {
        float cost = 0.0f;

        for (int i = 0; i < n; i++)
        {
            float d[3];
            v_sub(d, edit_mesh_position(mesh, a[i]), edit_mesh_position(mesh, b[((offset - i) % n + n) % n]));
            cost += v_dot(d, d);
        }

        if (cost < best_cost)
        {
            best_cost = cost;
            best_offset = offset;
        }
    }

    int* ring_a = SDL_malloc(sizeof(int) * (size_t)n);
    int* ring_b = SDL_malloc(sizeof(int) * (size_t)n);

    for (int i = 0; i < n; i++)
    {
        ring_a[i] = a[i];
        ring_b[i] = b[((best_offset - i) % n + n) % n];
    }

    EditFaceStyle style = mesh->faces[first].style;

    PolyList polys;
    polys_from_mesh(mesh, &polys);
    polys.items[first].alive = 0;
    polys.items[second].alive = 0;

    for (int i = 0; i < n; i++)
    {
        int j = (i + 1) % n;
        int quad[4] = { ring_a[i], ring_a[j], ring_b[j], ring_b[i] };
        polys_add(&polys, quad, 4, &style, 0);
    }

    SDL_free(ring_a);
    SDL_free(ring_b);

    polys_to_mesh(mesh, &polys);
    edit_mesh_update(mesh);

    return 1;
}

int edit_mesh_fill_holes(EditMesh* mesh, const unsigned char* vertex_filter)
{
    Adjacency adjacency;
    adjacency_build(mesh, &adjacency);

    // A hole's face runs along the border edges backwards : next[b] = a for each border edge a -> b.
    int* next = SDL_malloc(sizeof(int) * (size_t)SDL_max(1, mesh->vertex_count));
    int* style_face = SDL_malloc(sizeof(int) * (size_t)SDL_max(1, mesh->vertex_count));

    for (int i = 0; i < mesh->vertex_count; i++)
    {
        next[i] = -1;
    }

    for (int e = 0; e < mesh->edge_count; e++)
    {
        if (adjacency.offsets[e + 1] - adjacency.offsets[e] != 1)
        {
            continue;
        }

        int f = adjacency.faces[adjacency.offsets[e]];
        int corner = adjacency.corners[adjacency.offsets[e]];
        int a = edit_mesh_face_vertex(mesh, f, corner);
        int b = edit_mesh_face_vertex(mesh, f, corner + 1);
        next[b] = a;
        style_face[b] = f;
    }

    adjacency_free(&adjacency);

    unsigned char* visited = SDL_calloc((size_t)SDL_max(1, mesh->vertex_count), 1);
    int* cycle = SDL_malloc(sizeof(int) * (size_t)SDL_max(1, mesh->vertex_count));
    int count = 0;
    int vertex_count = mesh->vertex_count;

    for (int start = 0; start < vertex_count; start++)
    {
        if (next[start] < 0 || visited[start])
        {
            continue;
        }

        int n = 0;
        int current = start;
        int closed = 0;
        int wanted = vertex_filter == NULL;

        while (current >= 0 && !visited[current] && n < vertex_count)
        {
            visited[current] = 1;
            cycle[n++] = current;
            wanted |= vertex_filter && vertex_filter[current];
            current = next[current];

            if (current == start)
            {
                closed = 1;
                break;
            }
        }

        if (closed && n >= 3 && wanted)
        {
            EditFaceStyle style = mesh->faces[style_face[start]].style;
            style.align = EDIT_UV_WORLD;
            style.uv_offset[0] = style.uv_offset[1] = 0.0f;
            style.uv_rotation = 0.0f;
            edit_mesh_add_face(mesh, cycle, n, &style);
            count++;
        }
    }

    SDL_free(cycle);
    SDL_free(visited);
    SDL_free(style_face);
    SDL_free(next);

    if (count)
    {
        edit_mesh_update(mesh);
    }

    return count;
}

int edit_mesh_thicken(EditMesh* mesh, float thickness)
{
    if (mesh->face_count == 0 || thickness == 0.0f)
    {
        return 0;
    }

    int vertex_count = mesh->vertex_count;
    int face_count = mesh->face_count;

    // Built now : stays valid for the original faces while the new ones are added.
    Adjacency adjacency;
    adjacency_build(mesh, &adjacency);

    // Each vertex moved inward so that every face around it ends up `thickness` behind itself : the
    // least-squares point of those offset planes (lightly pulled toward the averaged normal's offset,
    // for vertices with fewer than three distinct planes).
    float* normals = SDL_malloc(sizeof(float) * 3 * (size_t)face_count);
    double* systems = SDL_calloc((size_t)vertex_count, sizeof(double) * 12); // 3x3 matrix + right side
    float* average = SDL_calloc((size_t)vertex_count, sizeof(float) * 3);

    for (int f = 0; f < face_count; f++)
    {
        edit_mesh_face_normal(mesh, f, &normals[f * 3]);
    }

    for (int f = 0; f < face_count; f++)
    {
        const float* n = &normals[f * 3];

        for (int i = 0; i < mesh->faces[f].count; i++)
        {
            int v = mesh->loops[mesh->faces[f].first + i];
            const float* p = edit_mesh_position(mesh, v);
            double target = (double)v_dot(n, p) - (double)thickness;
            double* s = &systems[v * 12];

            for (int r = 0; r < 3; r++)
            {
                for (int c = 0; c < 3; c++)
                {
                    s[r * 3 + c] += (double)n[r] * (double)n[c];
                }

                s[9 + r] += (double)n[r] * target;
            }

            v_add(&average[v * 3], &average[v * 3], n);
        }
    }

    int* inner = SDL_malloc(sizeof(int) * (size_t)vertex_count);

    for (int v = 0; v < vertex_count; v++)
    {
        float fallback[3];
        float n[3];
        v_copy(n, &average[v * 3]);
        v_normalize(n);
        v_scale(fallback, n, -thickness);
        v_add(fallback, fallback, edit_mesh_position(mesh, v));

        double* s = &systems[v * 12];
        double m[9];
        double rhs[3];
        double det = 0.0;

        // Exact where three planes or more meet (a corner) ; elsewhere, the missing directions are
        // pulled toward the fallback.
        for (int attempt = 0; attempt < 2; attempt++)
        {
            double lambda = attempt == 0 ? 0.0 : 1e-3;

            for (int r = 0; r < 3; r++)
            {
                for (int c = 0; c < 3; c++)
                {
                    m[r * 3 + c] = s[r * 3 + c] + (r == c ? lambda : 0.0);
                }

                rhs[r] = s[9 + r] + lambda * (double)fallback[r];
            }

            det = m[0] * (m[4] * m[8] - m[5] * m[7]) - m[1] * (m[3] * m[8] - m[5] * m[6]) + m[2] * (m[3] * m[7] - m[4] * m[6]);

            if (SDL_fabs(det) > 1e-4)
            {
                break;
            }
        }

        // Cramer's rule.
        float p[3];
        v_copy(p, fallback);

        if (SDL_fabs(det) > 1e-12)
        {
            double x = (rhs[0] * (m[4] * m[8] - m[5] * m[7]) - m[1] * (rhs[1] * m[8] - m[5] * rhs[2]) + m[2] * (rhs[1] * m[7] - m[4] * rhs[2])) / det;
            double y = (m[0] * (rhs[1] * m[8] - m[5] * rhs[2]) - rhs[0] * (m[3] * m[8] - m[5] * m[6]) + m[2] * (m[3] * rhs[2] - rhs[1] * m[6])) / det;
            double z = (m[0] * (m[4] * rhs[2] - rhs[1] * m[7]) - m[1] * (m[3] * rhs[2] - rhs[1] * m[6]) + rhs[0] * (m[3] * m[7] - m[4] * m[6])) / det;
            v_set(p, (float)x, (float)y, (float)z);
        }

        inner[v] = edit_mesh_add_vertex(mesh, p);
    }

    // The inner copy, inside out.
    for (int f = 0; f < face_count; f++)
    {
        int n = mesh->faces[f].count;
        int* reversed = SDL_malloc(sizeof(int) * (size_t)n);

        for (int i = 0; i < n; i++)
        {
            reversed[i] = inner[mesh->loops[mesh->faces[f].first + n - 1 - i]];
        }

        EditFaceStyle style = mesh->faces[f].style;
        edit_mesh_add_face(mesh, reversed, n, &style);
        SDL_free(reversed);
    }

    // Open borders get a side, joining both surfaces.
    for (int f = 0; f < face_count; f++)
    {
        for (int i = 0; i < mesh->faces[f].count; i++)
        {
            int a = edit_mesh_face_vertex(mesh, f, i);
            int b = edit_mesh_face_vertex(mesh, f, i + 1);
            int e = edit_mesh_find_edge(mesh, a, b);

            if (e >= 0 && adjacency.offsets[e + 1] - adjacency.offsets[e] == 1)
            {
                int quad[4] = { b, a, inner[a], inner[b] };
                EditFaceStyle style = mesh->faces[f].style;
                edit_mesh_add_face(mesh, quad, 4, &style);
            }
        }
    }

    adjacency_free(&adjacency);
    SDL_free(inner);
    SDL_free(average);
    SDL_free(systems);
    SDL_free(normals);

    edit_mesh_update(mesh);

    return 1;
}

int edit_mesh_snap_to_grid(EditMesh* mesh, const float m[16], float grid, const unsigned char* vertex_filter)
{
    if (grid <= 0.0f)
    {
        return 0;
    }

    // World = M * local ; back with the inverse of M's 3x3 part (affine).
    float a = m[0], b = m[1], c = m[2], d = m[4], e = m[5], f = m[6], g = m[8], h = m[9], k = m[10];
    float det = a * (e * k - f * h) - b * (d * k - f * g) + c * (d * h - e * g);

    if (SDL_fabsf(det) < 1e-12f)
    {
        return 0;
    }

    float inv[9] = {
        (e * k - f * h) / det, (c * h - b * k) / det, (b * f - c * e) / det,
        (f * g - d * k) / det, (a * k - c * g) / det, (c * d - a * f) / det,
        (d * h - e * g) / det, (b * g - a * h) / det, (a * e - b * d) / det
    };

    int count = 0;

    for (int v = 0; v < mesh->vertex_count; v++)
    {
        if (vertex_filter && !vertex_filter[v])
        {
            continue;
        }

        float* p = &mesh->positions[v * 3];
        float world[3];

        for (int r = 0; r < 3; r++)
        {
            world[r] = m[r * 4] * p[0] + m[r * 4 + 1] * p[1] + m[r * 4 + 2] * p[2] + m[r * 4 + 3];
            world[r] = SDL_roundf(world[r] / grid) * grid - m[r * 4 + 3];
        }

        for (int r = 0; r < 3; r++)
        {
            p[r] = inv[r * 3] * world[0] + inv[r * 3 + 1] * world[1] + inv[r * 3 + 2] * world[2];
        }

        count++;
    }

    if (count)
    {
        edit_mesh_weld(mesh, 1e-5f, 0);
        edit_mesh_touch(mesh);
    }

    return count;
}

// --- Selection --------------------------------------------------------------------------------------

void edit_mesh_select_all(EditMesh* mesh, int element, int selected)
{
    if (element == EDIT_ELEMENT_VERTEX)
    {
        SDL_memset(mesh->vertex_selected, selected ? 1 : 0, (size_t)mesh->vertex_count);
    }
    else if (element == EDIT_ELEMENT_EDGE)
    {
        for (int e = 0; e < mesh->edge_count; e++)
        {
            mesh->edges[e].selected = selected;
        }
    }
    else
    {
        for (int f = 0; f < mesh->face_count; f++)
        {
            mesh->faces[f].selected = selected;
        }
    }
}

void edit_mesh_invert_selection(EditMesh* mesh, int element)
{
    if (element == EDIT_ELEMENT_VERTEX)
    {
        for (int i = 0; i < mesh->vertex_count; i++)
        {
            mesh->vertex_selected[i] = !mesh->vertex_selected[i];
        }
    }
    else if (element == EDIT_ELEMENT_EDGE)
    {
        for (int e = 0; e < mesh->edge_count; e++)
        {
            mesh->edges[e].selected = !mesh->edges[e].selected;
        }
    }
    else
    {
        for (int f = 0; f < mesh->face_count; f++)
        {
            mesh->faces[f].selected = !mesh->faces[f].selected;
        }
    }
}

// The edges at a vertex : up to `max`, returns how many.
static int edges_at(const EditMesh* mesh, int vertex, int* out, int max)
{
    int count = 0;

    for (int e = 0; e < mesh->edge_count; e++)
    {
        if (mesh->edges[e].a == vertex || mesh->edges[e].b == vertex)
        {
            if (count < max)
            {
                out[count] = e;
            }

            count++;
        }
    }

    return count;
}

static int edge_in_face(const EditMesh* mesh, int edge, int face)
{
    const EditFace* f = &mesh->faces[face];

    for (int i = 0; i < f->count; i++)
    {
        int a = mesh->loops[f->first + i];
        int b = mesh->loops[f->first + (i + 1) % f->count];

        if (edit_mesh_find_edge(mesh, a, b) == edge)
        {
            return 1;
        }
    }

    return 0;
}

int edit_mesh_select_loop(EditMesh* mesh)
{
    Adjacency adjacency;
    adjacency_build(mesh, &adjacency);

    unsigned char* selected = SDL_malloc((size_t)SDL_max(1, mesh->edge_count));

    for (int e = 0; e < mesh->edge_count; e++)
    {
        selected[e] = (unsigned char)mesh->edges[e].selected;
    }

    int added = 0;

    for (int start = 0; start < mesh->edge_count; start++)
    {
        if (!mesh->edges[start].selected)
        {
            continue;
        }

        // Both directions from the starting edge.
        for (int direction = 0; direction < 2; direction++)
        {
            int edge = start;
            int vertex = direction == 0 ? mesh->edges[start].b : mesh->edges[start].a;

            for (int guard = 0; guard < mesh->edge_count; guard++)
            {
                int around[8];
                int valence = edges_at(mesh, vertex, around, 8);
                int faces_of_edge = adjacency.offsets[edge + 1] - adjacency.offsets[edge];
                int next = -1;

                if (faces_of_edge == 2 && valence == 4)
                {
                    // The edge across the vertex : sharing no face with this one.
                    for (int k = 0; k < 4 && next < 0; k++)
                    {
                        int candidate = around[k];
                        int shares = 0;

                        for (int m = adjacency.offsets[edge]; m < adjacency.offsets[edge + 1]; m++)
                        {
                            shares |= edge_in_face(mesh, candidate, adjacency.faces[m]);
                        }

                        if (candidate != edge && !shares)
                        {
                            next = candidate;
                        }
                    }
                }
                else if (faces_of_edge == 1)
                {
                    // Along the border : the other border edge at this vertex.
                    for (int k = 0; k < SDL_min(valence, 8) && next < 0; k++)
                    {
                        int candidate = around[k];

                        if (candidate != edge && adjacency.offsets[candidate + 1] - adjacency.offsets[candidate] == 1)
                        {
                            next = candidate;
                        }
                    }
                }

                if (next < 0 || selected[next])
                {
                    break;
                }

                selected[next] = 1;
                added++;
                edge = next;
                vertex = mesh->edges[next].a == vertex ? mesh->edges[next].b : mesh->edges[next].a;
            }
        }
    }

    for (int e = 0; e < mesh->edge_count; e++)
    {
        mesh->edges[e].selected = selected[e];
    }

    SDL_free(selected);
    adjacency_free(&adjacency);

    return added;
}

int edit_mesh_select_ring(EditMesh* mesh)
{
    Adjacency adjacency;
    adjacency_build(mesh, &adjacency);

    unsigned char* selected = SDL_malloc((size_t)SDL_max(1, mesh->edge_count));

    for (int e = 0; e < mesh->edge_count; e++)
    {
        selected[e] = (unsigned char)mesh->edges[e].selected;
    }

    int added = 0;

    for (int start = 0; start < mesh->edge_count; start++)
    {
        if (!mesh->edges[start].selected)
        {
            continue;
        }

        for (int side = 0; side < 2; side++)
        {
            int edge = start;
            int came_from = -1;

            if (adjacency.offsets[edge + 1] - adjacency.offsets[edge] <= side)
            {
                continue;
            }

            int face = adjacency.faces[adjacency.offsets[edge] + side];

            for (int guard = 0; guard < mesh->face_count; guard++)
            {
                if (mesh->faces[face].count != 4 || face == came_from)
                {
                    break;
                }

                // The opposite edge of the quad.
                int corner = -1;

                for (int k = adjacency.offsets[edge]; k < adjacency.offsets[edge + 1]; k++)
                {
                    if (adjacency.faces[k] == face)
                    {
                        corner = adjacency.corners[k];
                    }
                }

                if (corner < 0)
                {
                    break;
                }

                int opposite = edit_mesh_find_edge(mesh, edit_mesh_face_vertex(mesh, face, corner + 2), edit_mesh_face_vertex(mesh, face, corner + 3));

                if (opposite < 0 || selected[opposite])
                {
                    break;
                }

                selected[opposite] = 1;
                added++;

                // Across it, into the next face.
                int next_face = -1;

                for (int k = adjacency.offsets[opposite]; k < adjacency.offsets[opposite + 1]; k++)
                {
                    if (adjacency.faces[k] != face)
                    {
                        next_face = adjacency.faces[k];
                    }
                }

                if (next_face < 0)
                {
                    break;
                }

                came_from = face;
                edge = opposite;
                face = next_face;
            }
        }
    }

    for (int e = 0; e < mesh->edge_count; e++)
    {
        mesh->edges[e].selected = selected[e];
    }

    SDL_free(selected);
    adjacency_free(&adjacency);

    return added;
}

int edit_mesh_grow_selection(EditMesh* mesh, int element)
{
    unsigned char* touched = SDL_malloc((size_t)SDL_max(1, mesh->vertex_count));
    edit_mesh_selection_vertices(mesh, element, touched);
    int added = 0;

    if (element == EDIT_ELEMENT_VERTEX)
    {
        for (int e = 0; e < mesh->edge_count; e++)
        {
            int a = mesh->edges[e].a;
            int b = mesh->edges[e].b;

            if (touched[a] && !mesh->vertex_selected[b]) { mesh->vertex_selected[b] = 1; added++; }
            if (touched[b] && !mesh->vertex_selected[a]) { mesh->vertex_selected[a] = 1; added++; }
        }
    }
    else if (element == EDIT_ELEMENT_EDGE)
    {
        for (int e = 0; e < mesh->edge_count; e++)
        {
            if (!mesh->edges[e].selected && (touched[mesh->edges[e].a] || touched[mesh->edges[e].b]))
            {
                mesh->edges[e].selected = 1;
                added++;
            }
        }
    }
    else
    {
        for (int f = 0; f < mesh->face_count; f++)
        {
            if (mesh->faces[f].selected)
            {
                continue;
            }

            for (int i = 0; i < mesh->faces[f].count; i++)
            {
                if (touched[mesh->loops[mesh->faces[f].first + i]])
                {
                    mesh->faces[f].selected = 1;
                    added++;
                    break;
                }
            }
        }
    }

    SDL_free(touched);
    return added;
}

int edit_mesh_shrink_selection(EditMesh* mesh, int element)
{
    // What touches anything unselected leaves the selection.
    unsigned char* outside = SDL_calloc((size_t)SDL_max(1, mesh->vertex_count), 1);
    int removed = 0;

    if (element == EDIT_ELEMENT_FACE)
    {
        for (int f = 0; f < mesh->face_count; f++)
        {
            if (!mesh->faces[f].selected)
            {
                for (int i = 0; i < mesh->faces[f].count; i++)
                {
                    outside[mesh->loops[mesh->faces[f].first + i]] = 1;
                }
            }
        }

        for (int f = 0; f < mesh->face_count; f++)
        {
            if (!mesh->faces[f].selected)
            {
                continue;
            }

            for (int i = 0; i < mesh->faces[f].count; i++)
            {
                if (outside[mesh->loops[mesh->faces[f].first + i]])
                {
                    mesh->faces[f].selected = 0;
                    removed++;
                    break;
                }
            }
        }
    }
    else if (element == EDIT_ELEMENT_VERTEX)
    {
        for (int e = 0; e < mesh->edge_count; e++)
        {
            int a = mesh->edges[e].a;
            int b = mesh->edges[e].b;

            if (!mesh->vertex_selected[a]) outside[b] = 1;
            if (!mesh->vertex_selected[b]) outside[a] = 1;
        }

        for (int i = 0; i < mesh->vertex_count; i++)
        {
            if (mesh->vertex_selected[i] && outside[i])
            {
                mesh->vertex_selected[i] = 0;
                removed++;
            }
        }
    }
    else
    {
        for (int e = 0; e < mesh->edge_count; e++)
        {
            if (!mesh->edges[e].selected)
            {
                outside[mesh->edges[e].a] = 1;
                outside[mesh->edges[e].b] = 1;
            }
        }

        for (int e = 0; e < mesh->edge_count; e++)
        {
            if (mesh->edges[e].selected && (outside[mesh->edges[e].a] || outside[mesh->edges[e].b]))
            {
                mesh->edges[e].selected = 0;
                removed++;
            }
        }
    }

    SDL_free(outside);
    return removed;
}

// --- Texture ----------------------------------------------------------------------------------------

void edit_mesh_fit_texture(EditMesh* mesh, int face)
{
    EditFaceStyle* style = &mesh->faces[face].style;
    style->uv_scale[0] = style->uv_scale[1] = 1.0f;
    style->uv_offset[0] = style->uv_offset[1] = 0.0f;

    float u[4];
    float v[4];
    edit_mesh_texture_axes(mesh, face, u, v);

    float lo_u = 1e30f, hi_u = -1e30f, lo_v = 1e30f, hi_v = -1e30f;

    for (int i = 0; i < mesh->faces[face].count; i++)
    {
        const float* p = edit_mesh_position(mesh, edit_mesh_face_vertex(mesh, face, i));
        float pu = v_dot(p, u);
        float pv = v_dot(p, v);
        lo_u = SDL_min(lo_u, pu);
        hi_u = SDL_max(hi_u, pu);
        lo_v = SDL_min(lo_v, pv);
        hi_v = SDL_max(hi_v, pv);
    }

    style->uv_scale[0] = SDL_max(1e-4f, hi_u - lo_u);
    style->uv_scale[1] = SDL_max(1e-4f, hi_v - lo_v);
    style->uv_offset[0] = -lo_u / style->uv_scale[0];
    style->uv_offset[1] = -lo_v / style->uv_scale[1];

    edit_mesh_touch(mesh);
}

// --- Clipping and combining -------------------------------------------------------------------------

typedef struct
{
    EditMesh* mesh;
    int* map;            // source vertex -> this mesh's, -1 until used
    int* map_cut;        // cut point (by source edge) -> this mesh's
} ClipSide;

static int clip_vertex(ClipSide* side, const EditMesh* source, int vertex)
{
    if (side->map[vertex] < 0)
    {
        side->map[vertex] = edit_mesh_add_vertex(side->mesh, edit_mesh_position(source, vertex));
    }

    return side->map[vertex];
}

static int clip_cut(ClipSide* side, int edge, const float* point)
{
    if (side->map_cut[edge] < 0)
    {
        side->map_cut[edge] = edit_mesh_add_vertex(side->mesh, point);
    }

    return side->map_cut[edge];
}

// Fills the holes whose vertices all lie on the plane : the cut's cap.
static void cap_clipped(EditMesh* mesh, const float plane[4])
{
    unsigned char* off_plane = SDL_malloc((size_t)SDL_max(1, mesh->vertex_count));

    for (int v = 0; v < mesh->vertex_count; v++)
    {
        off_plane[v] = SDL_fabsf(v_dot(plane, edit_mesh_position(mesh, v)) - plane[3]) > PLANE_EPSILON * 4.0f;
    }

    // As edit_mesh_fill_holes(), but only along border edges lying in the plane.
    Adjacency adjacency;
    adjacency_build(mesh, &adjacency);

    int* next = SDL_malloc(sizeof(int) * (size_t)SDL_max(1, mesh->vertex_count));
    int* style_face = SDL_malloc(sizeof(int) * (size_t)SDL_max(1, mesh->vertex_count));

    for (int i = 0; i < mesh->vertex_count; i++)
    {
        next[i] = -1;
    }

    for (int e = 0; e < mesh->edge_count; e++)
    {
        if (adjacency.offsets[e + 1] - adjacency.offsets[e] != 1)
        {
            continue;
        }

        int f = adjacency.faces[adjacency.offsets[e]];
        int corner = adjacency.corners[adjacency.offsets[e]];
        int a = edit_mesh_face_vertex(mesh, f, corner);
        int b = edit_mesh_face_vertex(mesh, f, corner + 1);

        if (!off_plane[a] && !off_plane[b])
        {
            next[b] = a;
            style_face[b] = f;
        }
    }

    adjacency_free(&adjacency);

    unsigned char* visited = SDL_calloc((size_t)SDL_max(1, mesh->vertex_count), 1);
    int* cycle = SDL_malloc(sizeof(int) * (size_t)SDL_max(1, mesh->vertex_count));
    int vertex_count = mesh->vertex_count;

    for (int start = 0; start < vertex_count; start++)
    {
        if (next[start] < 0 || visited[start])
        {
            continue;
        }

        int n = 0;
        int current = start;
        int closed = 0;

        while (current >= 0 && !visited[current] && n < vertex_count)
        {
            visited[current] = 1;
            cycle[n++] = current;
            current = next[current];

            if (current == start)
            {
                closed = 1;
                break;
            }
        }

        if (closed && n >= 3)
        {
            EditFaceStyle style = mesh->faces[style_face[start]].style;
            style.align = EDIT_UV_WORLD;
            style.uv_offset[0] = style.uv_offset[1] = 0.0f;
            style.uv_rotation = 0.0f;
            edit_mesh_add_face(mesh, cycle, n, &style);
        }
    }

    SDL_free(cycle);
    SDL_free(visited);
    SDL_free(style_face);
    SDL_free(next);
    SDL_free(off_plane);
}

void edit_mesh_clip(const EditMesh* mesh, const float plane[4], EditMesh** out_front, EditMesh** out_back)
{
    *out_front = NULL;
    *out_back = NULL;

    int n = mesh->vertex_count;
    float* distance = SDL_malloc(sizeof(float) * (size_t)SDL_max(1, n));
    int* side_of = SDL_malloc(sizeof(int) * (size_t)SDL_max(1, n));

    for (int v = 0; v < n; v++)
    {
        distance[v] = v_dot(plane, edit_mesh_position(mesh, v)) - plane[3];
        side_of[v] = distance[v] > PLANE_EPSILON ? 1 : (distance[v] < -PLANE_EPSILON ? -1 : 0);
    }

    ClipSide sides[2];

    for (int s = 0; s < 2; s++)
    {
        sides[s].mesh = edit_mesh_create();
        sides[s].map = SDL_malloc(sizeof(int) * (size_t)SDL_max(1, n));
        sides[s].map_cut = SDL_malloc(sizeof(int) * (size_t)SDL_max(1, mesh->edge_count));

        for (int v = 0; v < n; v++)
        {
            sides[s].map[v] = -1;
        }

        for (int e = 0; e < mesh->edge_count; e++)
        {
            sides[s].map_cut[e] = -1;
        }

        for (int m = 0; m < mesh->material_count; m++)
        {
            edit_mesh_material(sides[s].mesh, mesh->materials[m]);
        }
    }

    int* front = SDL_malloc(sizeof(int) * (size_t)SDL_max(1, mesh->loop_count * 2));
    int* back = SDL_malloc(sizeof(int) * (size_t)SDL_max(1, mesh->loop_count * 2));

    for (int f = 0; f < mesh->face_count; f++)
    {
        const EditFace* face = &mesh->faces[f];
        int front_count = 0;
        int back_count = 0;
        int any_front = 0;
        int any_back = 0;

        for (int i = 0; i < face->count; i++)
        {
            int v = mesh->loops[face->first + i];
            any_front |= side_of[v] > 0;
            any_back |= side_of[v] < 0;
        }

        if (!any_front && !any_back)
        {
            // On the plane : it closes the part its back faces.
            float normal[3];
            edit_mesh_face_normal(mesh, f, normal);
            ClipSide* side = &sides[v_dot(normal, plane) > 0.0f ? 1 : 0];
            int* loop = front;

            for (int i = 0; i < face->count; i++)
            {
                loop[i] = clip_vertex(side, mesh, mesh->loops[face->first + i]);
            }

            int added = edit_mesh_add_face(side->mesh, loop, face->count, &face->style);
            side->mesh->faces[added].selected = face->selected;
            continue;
        }

        for (int i = 0; i < face->count; i++)
        {
            int current = mesh->loops[face->first + i];
            int next = mesh->loops[face->first + (i + 1) % face->count];

            if (side_of[current] >= 0)
            {
                front[front_count++] = clip_vertex(&sides[0], mesh, current);
            }

            if (side_of[current] <= 0)
            {
                back[back_count++] = clip_vertex(&sides[1], mesh, current);
            }

            if ((side_of[current] > 0 && side_of[next] < 0) || (side_of[current] < 0 && side_of[next] > 0))
            {
                float t = distance[current] / (distance[current] - distance[next]);
                float point[3];
                v_sub(point, edit_mesh_position(mesh, next), edit_mesh_position(mesh, current));
                v_scale(point, point, t);
                v_add(point, point, edit_mesh_position(mesh, current));

                int edge = edit_mesh_find_edge(mesh, current, next);
                front[front_count++] = clip_cut(&sides[0], edge, point);
                back[back_count++] = clip_cut(&sides[1], edge, point);
            }
        }

        if (any_front && front_count >= 3)
        {
            int added = edit_mesh_add_face(sides[0].mesh, front, front_count, &face->style);
            sides[0].mesh->faces[added].selected = face->selected;
        }

        if (any_back && back_count >= 3)
        {
            int added = edit_mesh_add_face(sides[1].mesh, back, back_count, &face->style);
            sides[1].mesh->faces[added].selected = face->selected;
        }
    }

    SDL_free(front);
    SDL_free(back);

    for (int s = 0; s < 2; s++)
    {
        EditMesh* part = sides[s].mesh;
        edit_mesh_update(part);

        if (part->face_count > 0)
        {
            cap_clipped(part, plane);
            edit_mesh_update(part);
        }

        SDL_free(sides[s].map);
        SDL_free(sides[s].map_cut);

        if (part->face_count == 0)
        {
            edit_mesh_destroy(part);
            part = NULL;
        }

        if (s == 0)
        {
            *out_front = part;
        }
        else
        {
            *out_back = part;
        }
    }

    SDL_free(side_of);
    SDL_free(distance);
}

void edit_mesh_append(EditMesh* to, const EditMesh* from, const float m[16])
{
    int base = to->vertex_count;

    for (int v = 0; v < from->vertex_count; v++)
    {
        const float* p = edit_mesh_position(from, v);
        float q[3];

        for (int r = 0; r < 3; r++)
        {
            q[r] = m[r * 4] * p[0] + m[r * 4 + 1] * p[1] + m[r * 4 + 2] * p[2] + m[r * 4 + 3];
        }

        int index = edit_mesh_add_vertex(to, q);
        to->vertex_selected[index] = from->vertex_selected[v];
    }

    float det = m[0] * (m[5] * m[10] - m[6] * m[9]) - m[1] * (m[4] * m[10] - m[6] * m[8]) + m[2] * (m[4] * m[9] - m[5] * m[8]);
    int mirrored = det < 0.0f;

    for (int f = 0; f < from->face_count; f++)
    {
        const EditFace* face = &from->faces[f];
        int* loop = SDL_malloc(sizeof(int) * (size_t)face->count);

        for (int i = 0; i < face->count; i++)
        {
            int k = mirrored ? face->count - 1 - i : i;
            loop[i] = base + from->loops[face->first + k];
        }

        EditFaceStyle style = face->style;
        style.material = edit_mesh_material(to, from->materials[SDL_clamp(face->style.material, 0, SDL_max(0, from->material_count - 1))]);
        int added = edit_mesh_add_face(to, loop, face->count, &style);
        to->faces[added].selected = face->selected;
        SDL_free(loop);
    }

    edit_mesh_update(to);
}

EditMesh* edit_mesh_detach_faces(EditMesh* mesh)
{
    if (edit_mesh_selection_count(mesh, EDIT_ELEMENT_FACE) == 0)
    {
        return NULL;
    }

    EditMesh* detached = edit_mesh_create();
    int* map = SDL_malloc(sizeof(int) * (size_t)SDL_max(1, mesh->vertex_count));

    for (int v = 0; v < mesh->vertex_count; v++)
    {
        map[v] = -1;
    }

    for (int m = 0; m < mesh->material_count; m++)
    {
        edit_mesh_material(detached, mesh->materials[m]);
    }

    for (int f = 0; f < mesh->face_count; f++)
    {
        const EditFace* face = &mesh->faces[f];

        if (!face->selected)
        {
            continue;
        }

        int* loop = SDL_malloc(sizeof(int) * (size_t)face->count);

        for (int i = 0; i < face->count; i++)
        {
            int v = mesh->loops[face->first + i];

            if (map[v] < 0)
            {
                map[v] = edit_mesh_add_vertex(detached, edit_mesh_position(mesh, v));
            }

            loop[i] = map[v];
        }

        int added = edit_mesh_add_face(detached, loop, face->count, &face->style);
        detached->faces[added].selected = 1;
        SDL_free(loop);
    }

    SDL_free(map);

    edit_mesh_delete_faces(mesh);
    edit_mesh_update(detached);

    return detached;
}

void edit_mesh_translate(EditMesh* mesh, const float offset[3])
{
    for (int v = 0; v < mesh->vertex_count; v++)
    {
        v_sub(&mesh->positions[v * 3], &mesh->positions[v * 3], offset);
    }

    edit_mesh_touch(mesh);
}

// --- Saving -----------------------------------------------------------------------------------------

typedef struct
{
    char* text;
    size_t length;
    size_t capacity;
} Text;

static void text_append(Text* t, const char* format, ...)
{
    char buffer[512];
    va_list args;
    va_start(args, format);
    int written = SDL_vsnprintf(buffer, sizeof(buffer), format, args);
    va_end(args);

    if (written < 0)
    {
        return;
    }

    written = SDL_min(written, (int)sizeof(buffer) - 1);

    if (t->length + (size_t)written + 1 > t->capacity)
    {
        t->capacity = SDL_max(t->capacity * 2, t->length + (size_t)written + 4096);
        t->text = SDL_realloc(t->text, t->capacity);
    }

    SDL_memcpy(t->text + t->length, buffer, (size_t)written);
    t->length += (size_t)written;
    t->text[t->length] = '\0';
}

char* edit_mesh_to_json(const EditMesh* mesh)
{
    Text t = { 0 };

    text_append(&t, "{ \"materials\": [");

    for (int m = 0; m < mesh->material_count; m++)
    {
        char clean[EDIT_MESH_MATERIAL_SIZE];
        size_t n = 0;

        for (const char* c = mesh->materials[m]; *c && n + 1 < sizeof(clean); c++)
        {
            if (*c != '"' && *c != '\\' && (unsigned char)*c >= 0x20)
            {
                clean[n++] = *c;
            }
        }

        clean[n] = '\0';
        text_append(&t, "%s\"%s\"", m ? ", " : "", clean);
    }

    text_append(&t, "],\n        \"vertices\": [");

    for (int v = 0; v < mesh->vertex_count; v++)
    {
        const float* p = edit_mesh_position(mesh, v);
        text_append(&t, "%s%.7g, %.7g, %.7g", v ? ", " : "", p[0], p[1], p[2]);
    }

    // A face : material, alignment, scale u v, offset u v, rotation, then its vertices.
    text_append(&t, "],\n        \"faces\": [");

    for (int f = 0; f < mesh->face_count; f++)
    {
        const EditFace* face = &mesh->faces[f];
        const EditFaceStyle* s = &face->style;
        text_append(&t, "%s[%d, %d, %g, %g, %g, %g, %g", f ? ", " : "", s->material, s->align,
            s->uv_scale[0], s->uv_scale[1], s->uv_offset[0], s->uv_offset[1], s->uv_rotation);

        for (int i = 0; i < face->count; i++)
        {
            text_append(&t, ", %d", mesh->loops[face->first + i]);
        }

        text_append(&t, "]");
    }

    text_append(&t, "] }");

    return t.text;
}

#define FACE_HEADER 7

EditMesh* edit_mesh_from_json(const JsonValue* value)
{
    const JsonValue* vertices = json_object_get(value, "vertices");
    const JsonValue* faces = json_object_get(value, "faces");

    if (!vertices || !faces)
    {
        return NULL;
    }

    EditMesh* mesh = edit_mesh_create();
    const JsonValue* materials = json_object_get(value, "materials");

    for (size_t m = 0; m < json_array_count(materials); m++)
    {
        edit_mesh_material(mesh, json_string(json_array_get(materials, m), ""));
    }

    if (mesh->material_count == 0)
    {
        edit_mesh_material(mesh, "builtin/materials/dev_grey");
    }

    size_t coordinates = json_array_count(vertices);

    for (size_t i = 0; i + 2 < coordinates; i += 3)
    {
        float p[3] = {
            (float)json_number(json_array_get(vertices, i), 0.0),
            (float)json_number(json_array_get(vertices, i + 1), 0.0),
            (float)json_number(json_array_get(vertices, i + 2), 0.0)
        };
        edit_mesh_add_vertex(mesh, p);
    }

    for (size_t f = 0; f < json_array_count(faces); f++)
    {
        const JsonValue* face = json_array_get(faces, f);
        int count = (int)json_array_count(face) - FACE_HEADER;

        if (count < 3)
        {
            continue;
        }

        EditFaceStyle style;
        edit_mesh_default_style(&style, 0);
        style.material = SDL_clamp(json_int(json_array_get(face, 0), 0), 0, mesh->material_count - 1);
        style.align = json_int(json_array_get(face, 1), EDIT_UV_WORLD);
        style.uv_scale[0] = (float)json_number(json_array_get(face, 2), 1.0);
        style.uv_scale[1] = (float)json_number(json_array_get(face, 3), 1.0);
        style.uv_offset[0] = (float)json_number(json_array_get(face, 4), 0.0);
        style.uv_offset[1] = (float)json_number(json_array_get(face, 5), 0.0);
        style.uv_rotation = (float)json_number(json_array_get(face, 6), 0.0);

        int* loop = SDL_malloc(sizeof(int) * (size_t)count);
        int valid = 1;

        for (int i = 0; i < count; i++)
        {
            loop[i] = json_int(json_array_get(face, (size_t)(FACE_HEADER + i)), -1);
            valid &= loop[i] >= 0 && loop[i] < mesh->vertex_count;
        }

        if (valid)
        {
            edit_mesh_add_face(mesh, loop, count, &style);
        }

        SDL_free(loop);
    }

    edit_mesh_update(mesh);

    return mesh;
}

// --- Store ------------------------------------------------------------------------------------------

typedef struct
{
    uint32_t handle;
    EditMesh* mesh;
    int frozen;
} StoreEntry;

static StoreEntry* store;
static int store_count;
static int store_capacity;
static uint32_t next_handle = 1;

// Entries stay sorted by handle : handles only grow, and removal keeps the order.
static StoreEntry* store_find(uint32_t handle)
{
    int lo = 0;
    int hi = store_count - 1;

    while (lo <= hi)
    {
        int mid = (lo + hi) / 2;

        if (store[mid].handle == handle)
        {
            return &store[mid];
        }

        if (store[mid].handle < handle)
        {
            lo = mid + 1;
        }
        else
        {
            hi = mid - 1;
        }
    }

    return NULL;
}

uint32_t edit_mesh_store_add(EditMesh* mesh)
{
    store = grow(store, &store_capacity, store_count + 1, sizeof(StoreEntry));

    StoreEntry* entry = &store[store_count++];
    entry->handle = next_handle++;
    entry->mesh = mesh;
    entry->frozen = 0;

    return entry->handle;
}

EditMesh* edit_mesh_store_get(uint32_t handle)
{
    StoreEntry* entry = handle ? store_find(handle) : NULL;
    return entry ? entry->mesh : NULL;
}

int edit_mesh_store_frozen(uint32_t handle)
{
    StoreEntry* entry = handle ? store_find(handle) : NULL;
    return entry ? entry->frozen : 0;
}

void edit_mesh_store_freeze_all(void)
{
    for (int i = 0; i < store_count; i++)
    {
        store[i].frozen = 1;
    }
}

uint32_t edit_mesh_store_copy(uint32_t handle)
{
    EditMesh* mesh = edit_mesh_store_get(handle);
    return mesh ? edit_mesh_store_add(edit_mesh_clone(mesh)) : 0;
}

int edit_mesh_store_count(void)
{
    return store_count;
}

static int compare_handles(const void* a, const void* b)
{
    uint32_t x = *(const uint32_t*)a;
    uint32_t y = *(const uint32_t*)b;
    return x < y ? -1 : (x > y ? 1 : 0);
}

void edit_mesh_store_collect(const uint32_t* live, int live_count)
{
    uint32_t* sorted = SDL_malloc(sizeof(uint32_t) * (size_t)SDL_max(1, live_count));
    SDL_memcpy(sorted, live, sizeof(uint32_t) * (size_t)live_count);
    SDL_qsort(sorted, (size_t)live_count, sizeof(uint32_t), compare_handles);

    int kept = 0;
    int cursor = 0;

    for (int i = 0; i < store_count; i++)
    {
        while (cursor < live_count && sorted[cursor] < store[i].handle)
        {
            cursor++;
        }

        if (cursor < live_count && sorted[cursor] == store[i].handle)
        {
            store[kept++] = store[i];
        }
        else
        {
            edit_mesh_destroy(store[i].mesh);
        }
    }

    store_count = kept;
    SDL_free(sorted);
}

void edit_mesh_store_clear(void)
{
    for (int i = 0; i < store_count; i++)
    {
        edit_mesh_destroy(store[i].mesh);
    }

    SDL_free(store);
    store = NULL;
    store_count = 0;
    store_capacity = 0;
}
