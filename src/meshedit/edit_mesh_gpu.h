#ifndef _EDIT_MESH_GPU_H_
#define _EDIT_MESH_GPU_H_

#include <stdint.h>

#include "renderer/renderer.h"

#ifdef __cplusplus
extern "C" {
#endif

// Editable meshes (edit_mesh.h) as GPU meshes : triangulated, flat-shaded (hard edges, the whitebox
// look), grouped into one part per material. Built on first use and rebuilt whenever the mesh changes ;
// call edit_mesh_gpu_end_frame() once per frame, so that what's no longer drawn is released, a few frames
// later, once the GPU can't be using it any more.
//
// Vertex colors are the materials' flat colors (asset_material_color()) ; the 3D renderer ignores them
// (PbrMaterial::ignore_vertex_color) and samples the materials' textures.

typedef struct
{
    int first_index;
    int index_count;
    int material;           // index into the EditMesh's materials
} EditMeshGpuPart;

typedef struct
{
    Mesh* mesh;             // NULL : nothing to draw
    EditMeshGpuPart* parts;
    int part_count;
    float bounds[6];

    // The same geometry on the CPU, for baking (bake/bake.h) : the GPU mesh's vertices and indices, and
    // the face each vertex belongs to (a face is a lightmap chart).
    const MeshVertex* vertices;
    int vertex_count;
    const uint32_t* indices;
    int index_count;
    const int* vertex_faces;
} EditMeshGpu;

const EditMeshGpu* edit_mesh_gpu_get(uint32_t handle); // NULL if the handle is unknown
void edit_mesh_gpu_end_frame(void);
void edit_mesh_gpu_clear(void); // the GPU must be idle

#ifdef __cplusplus
}
#endif

#endif // _EDIT_MESH_GPU_H_
