#ifndef _EDIT_MESH_H_
#define _EDIT_MESH_H_

#include <stdint.h>

#include "json/json.h"

#ifdef __cplusplus
extern "C" {
#endif

// Editable polygon meshes : the level editor's building blocks, for whiteboxing / greyboxing in the
// manner of Valve's Hammer (Source 2). Vertices are shared by polygonal faces of any size (convex or
// not), each face with its own material and texture mapping. Edges aren't stored by the faces : they're
// derived from them by edit_mesh_update(), which every change to the topology must end with.
//
// Positions are in the owning object's local space. Faces wind counter-clockwise seen from the side their
// normal points to (the outside of a solid). Selection flags (vertices, edges, faces) live in the mesh,
// so that copies keep them ; they aren't saved.
//
// Agnostic of any rendering, like tiled_map.c : see edit_mesh_gpu.h for drawing one.

#define EDIT_MESH_MATERIAL_SIZE 192

typedef enum
{
    EDIT_ELEMENT_VERTEX,
    EDIT_ELEMENT_EDGE,
    EDIT_ELEMENT_FACE
} EditElement;

// How a face's texture coordinates come from its vertices' positions : a planar projection.
typedef enum
{
    EDIT_UV_WORLD,  // onto the axis plane closest to the face (textures line up across faces and meshes)
    EDIT_UV_FACE    // onto the face's own plane (no stretching on slopes)
} EditUvAlign;

typedef struct
{
    int material;           // index into EditMesh::materials
    int align;              // EditUvAlign
    float uv_scale[2];      // meters per texture repeat, along u and v
    float uv_offset[2];     // in texture repeats
    float uv_rotation;      // degrees
} EditFaceStyle;

typedef struct
{
    int first;              // into EditMesh::loops
    int count;              // >= 3
    EditFaceStyle style;
    int selected;
} EditFace;

typedef struct
{
    int a, b;               // vertex indices, a < b
    int selected;
} EditEdge;

typedef struct
{
    float* positions;       // xyz per vertex
    unsigned char* vertex_selected;
    int vertex_count;
    int vertex_capacity;

    int* loops;             // every face's vertex indices, one after the other
    int loop_count;
    int loop_capacity;

    EditFace* faces;
    int face_count;
    int face_capacity;

    EditEdge* edges;        // derived : sorted by (a, b), see edit_mesh_update()
    int edge_count;
    int edge_capacity;

    char (*materials)[EDIT_MESH_MATERIAL_SIZE]; // asset paths, e.g. "builtin/materials/dev_grey"
    int material_count;
    int material_capacity;

    uint32_t revision;      // changes with every modification : what's derived from the mesh is rebuilt
} EditMesh;

EditMesh* edit_mesh_create(void);
EditMesh* edit_mesh_clone(const EditMesh* mesh);
void edit_mesh_destroy(EditMesh* mesh);

// --- Building ---------------------------------------------------------------------------------------

int edit_mesh_add_vertex(EditMesh* mesh, const float position[3]);
int edit_mesh_add_face(EditMesh* mesh, const int* vertices, int count, const EditFaceStyle* style);
int edit_mesh_material(EditMesh* mesh, const char* path); // its index, added if needed
void edit_mesh_default_style(EditFaceStyle* out_style, int material);

// After changing the topology : drops degenerate faces and unused vertices, rebuilds the edges (keeping
// their selection), and bumps the revision.
void edit_mesh_update(EditMesh* mesh);

// After moving vertices or restyling faces, without changing the topology.
void edit_mesh_touch(EditMesh* mesh);

// --- Queries ----------------------------------------------------------------------------------------

const float* edit_mesh_position(const EditMesh* mesh, int vertex);
int edit_mesh_face_vertex(const EditMesh* mesh, int face, int corner); // corner wraps around
void edit_mesh_face_normal(const EditMesh* mesh, int face, float out_normal[3]); // unit (Newell's method)
void edit_mesh_face_center(const EditMesh* mesh, int face, float out_center[3]);
void edit_mesh_bounds(const EditMesh* mesh, float out_bounds[6]); // min xyz, max xyz
int edit_mesh_find_edge(const EditMesh* mesh, int a, int b);     // -1 if there's none
int edit_mesh_edge_face_count(const EditMesh* mesh, int edge);     // 1 : on a boundary (an open side)

// The closest face hit by a ray (direction not necessarily normalized), both sides : 1 if one is, with
// `*inout_t` lowered to the hit's ray parameter. Only hits closer than its initial value count.
int edit_mesh_raycast(const EditMesh* mesh, const float origin[3], const float direction[3], float* inout_t, int* out_face);

// A face as triangles : (count - 2) * 3 corner indices (0 .. count - 1), ear clipping (concave faces
// work). Returns the triangle count.
int edit_mesh_triangulate_face(const EditMesh* mesh, int face, int* out_corners);

// The face's texture projection : uv = (dot(p, u.xyz) + u.w, dot(p, v.xyz) + v.w).
void edit_mesh_texture_axes(const EditMesh* mesh, int face, float out_u[4], float out_v[4]);

// Which vertices the selection of one kind of element touches : flags per vertex, returns their count.
int edit_mesh_selection_vertices(const EditMesh* mesh, int element, unsigned char* out_flags);
int edit_mesh_selection_count(const EditMesh* mesh, int element);

// --- Primitives -------------------------------------------------------------------------------------

typedef enum
{
    EDIT_SHAPE_BOX,
    EDIT_SHAPE_WEDGE,       // a ramp, rising toward -Z
    EDIT_SHAPE_CYLINDER,
    EDIT_SHAPE_CONE,
    EDIT_SHAPE_STAIRS,      // rising toward -Z
    EDIT_SHAPE_ARCH,        // in the XY plane, through Z
    EDIT_SHAPE_SPHERE,
    EDIT_SHAPE_COUNT
} EditShape;

typedef struct
{
    int shape;              // EditShape
    int sides;              // cylinder, cone, arch, sphere
    int steps;              // stairs
    float thickness;        // arch : of its ring, in meters
} EditShapeParams;

void edit_mesh_default_shape(EditShapeParams* out_params, int shape);
const char* edit_mesh_shape_name(int shape);

// A shape filling a box of `size`, centered on the origin, all its faces of `material`.
EditMesh* edit_mesh_make_shape(const EditShapeParams* params, const float size[3], const char* material);

// --- Operations -------------------------------------------------------------------------------------
// They work on the selection and return how many elements they changed (0 : nothing to do). Each ends
// with edit_mesh_update().

// Extrudes the selected faces as one region (sides along its border) ; they stay selected, on new
// vertices, ready to be moved. A lone patch (no neighbors) gets a bottom too, making a closed solid.
int edit_mesh_extrude_faces(EditMesh* mesh);

// Extends the selected border edges (edges of a single face) into new faces ; the new outer edges end
// up selected, ready to be moved.
int edit_mesh_extrude_edges(EditMesh* mesh);

int edit_mesh_inset_faces(EditMesh* mesh, float distance); // each face shrunk inward, a ring of quads around
int edit_mesh_flip_faces(EditMesh* mesh, int selected_only);
int edit_mesh_delete_faces(EditMesh* mesh);
int edit_mesh_dissolve_edges(EditMesh* mesh);    // merges the two faces of each selected edge
int edit_mesh_dissolve_vertices(EditMesh* mesh); // removes the selected vertices from their faces
int edit_mesh_merge_vertices(EditMesh* mesh);    // the selected vertices become one, at their center
int edit_mesh_collapse(EditMesh* mesh, int element); // each connected group of selected edges / faces to its center
int edit_mesh_weld(EditMesh* mesh, float distance, int selected_only); // vertices closer than distance
int edit_mesh_split_edges(EditMesh* mesh);       // at their middle
int edit_mesh_connect(EditMesh* mesh, int element); // splits faces : between 2 selected vertices, or the middles of 2 selected edges
int edit_mesh_bridge_faces(EditMesh* mesh);      // two selected faces of as many vertices, joined by a tunnel
int edit_mesh_fill_holes(EditMesh* mesh, const unsigned char* vertex_filter); // NULL : every hole
int edit_mesh_thicken(EditMesh* mesh, float thickness); // an inner copy, inside out : a closed mesh becomes hollow
int edit_mesh_snap_to_grid(EditMesh* mesh, const float matrix[16], float grid, const unsigned char* vertex_filter);

// Selection.
void edit_mesh_select_all(EditMesh* mesh, int element, int selected);
void edit_mesh_invert_selection(EditMesh* mesh, int element);
int edit_mesh_select_loop(EditMesh* mesh); // edges : continues the selected edges across quads' vertices
int edit_mesh_select_ring(EditMesh* mesh); // edges : the opposite edges of quads, across them
int edit_mesh_grow_selection(EditMesh* mesh, int element);
int edit_mesh_shrink_selection(EditMesh* mesh, int element);

// Texture mapping : the texture fits the face exactly once.
void edit_mesh_fit_texture(EditMesh* mesh, int face);

// Cuts the mesh along a plane (n.xyz with n unit, n . x = d) into what's in front and what's behind,
// each closed again where the plane cut it. Either result may be NULL (nothing on that side).
void edit_mesh_clip(const EditMesh* mesh, const float plane[4], EditMesh** out_front, EditMesh** out_back);

// Adds `from`'s faces to `to`, transformed by `matrix` (row-major, from's space into to's).
void edit_mesh_append(EditMesh* to, const EditMesh* from, const float matrix[16]);

// Moves the selected faces into a new mesh (same space). NULL if none is selected.
EditMesh* edit_mesh_detach_faces(EditMesh* mesh);

// Moves every vertex by -offset : with the owning object moved by +offset, nothing moves in the world.
void edit_mesh_translate(EditMesh* mesh, const float offset[3]);

// --- Saving -----------------------------------------------------------------------------------------

char* edit_mesh_to_json(const EditMesh* mesh); // SDL_free() it
EditMesh* edit_mesh_from_json(const JsonValue* value); // NULL if it isn't a mesh

// --- Store ------------------------------------------------------------------------------------------
// Scenes refer to their meshes by handle (SceneObject::mesh), which keeps a whole scene copyable with `=`
// for undo and the play mode's snapshot. The copies then share their meshes, so a mesh that may be shared
// must not change : it's frozen, and editing goes through a copy (copy on write, see the editor).

uint32_t edit_mesh_store_add(EditMesh* mesh); // takes ownership ; returns its handle (never 0)
EditMesh* edit_mesh_store_get(uint32_t handle); // NULL if unknown
int edit_mesh_store_frozen(uint32_t handle);
void edit_mesh_store_freeze_all(void);
uint32_t edit_mesh_store_copy(uint32_t handle); // a new, unfrozen copy ; 0 if unknown
int edit_mesh_store_count(void);

// Frees every mesh whose handle isn't in `live` (in any order, duplicates allowed).
void edit_mesh_store_collect(const uint32_t* live, int live_count);
void edit_mesh_store_clear(void);

#ifdef __cplusplus
}
#endif

#endif // _EDIT_MESH_H_
