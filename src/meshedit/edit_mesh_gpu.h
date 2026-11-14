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
// Vertex colors are the materials' flat colors (asset_material_color()), for the forward renderer ;
// the photorealistic one ignores them and samples the materials' textures.

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
} EditMeshGpu;

const EditMeshGpu* edit_mesh_gpu_get(uint32_t handle); // NULL if the handle is unknown
void edit_mesh_gpu_end_frame(void);
void edit_mesh_gpu_clear(void); // the GPU must be idle

#ifdef __cplusplus
}
#endif

#endif // _EDIT_MESH_GPU_H_
