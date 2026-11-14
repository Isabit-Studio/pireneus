#ifndef _MESH_CONSTANTS_H_
#define _MESH_CONSTANTS_H_

// Backend-private : the 3D pipelines' push constants (Vulkan) / root constants (DX12), mirrored
// field-for-field by the MeshConstants cbuffer in shaders/mesh.slang. Matrices are passed as four
// float4 rows (row-major, see math/math3d.h) and multiplied with explicit dot products in the shader.
//
// 252 bytes : fits Vulkan's push constant budget on every desktop GPU (256 bytes on AMD, NVIDIA and
// Intel ; the spec minimum is 128, checked at startup), and DX12's 64-DWORD root signature limit
// together with the pass texture's descriptor table (63 + 1 DWORDs : FULL, nothing more fits). Every
// scalar parameter rides in the `w` component of a float4 rather than in a 16-byte block of its own,
// and the last field is a bare float3, both to stay under that limit.

#include "math/math3d.h"

#include <string.h>

#include "renderer/renderer.h"

typedef struct
{
    float model[16];
    float view_projection[16];       // camera (scene pass) ; unused by the shadow pass
    float light_view_projection[16];
    float light_direction[4];        // w : shadow map texel size (1 / size)
    float light_color[4];            // w : shadow depth bias
    float ambient_color[4];          // w : shadow normal offset
    float ground_color[3];           // ambient from below (the hemisphere ambient's lower color)
} MeshConstants;

static inline void mesh_constants_for_shadow_pass(MeshConstants* constants, const float light_view_projection[16])
{
    memset(constants, 0, sizeof(*constants));
    memcpy(constants->light_view_projection, light_view_projection, sizeof(constants->light_view_projection));
}

static inline void mesh_constants_for_scene(MeshConstants* constants, const SceneLighting* lighting, int shadow_map_size)
{
    memset(constants, 0, sizeof(*constants));
    memcpy(constants->view_projection, lighting->view_projection, sizeof(constants->view_projection));
    memcpy(constants->light_view_projection, lighting->light_view_projection, sizeof(constants->light_view_projection));

    for (int i = 0; i < 3; i++)
    {
        constants->light_direction[i] = lighting->light_direction[i];
        constants->light_color[i] = lighting->light_color[i];
        constants->ambient_color[i] = lighting->ambient_color[i];
        constants->ground_color[i] = lighting->ground_color[i];
    }

    constants->light_direction[3] = shadow_map_size > 0 ? 1.0f / (float)shadow_map_size : 0.0f;
    constants->light_color[3] = lighting->shadow_depth_bias;
    constants->ambient_color[3] = lighting->shadow_normal_offset;
}

// Sky pass : the same block, other meanings (see skyVertexMain in mesh.slang) — model holds the
// INVERSE view-projection (to rebuild each pixel's view ray), light_direction.w the sky intensity.
static inline void mesh_constants_for_sky(MeshConstants* constants, const float view_projection[16], float intensity)
{
    Mat4 matrix;
    memcpy(matrix.m, view_projection, sizeof(matrix.m));
    Mat4 inverse = mat4_inverse(matrix);

    memset(constants, 0, sizeof(*constants));
    memcpy(constants->model, inverse.m, sizeof(constants->model));
    constants->light_direction[3] = intensity;
}

#endif // _MESH_CONSTANTS_H_
