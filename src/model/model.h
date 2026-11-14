#ifndef _MODEL_H_
#define _MODEL_H_

#include <stddef.h>
#include <stdint.h>

#include "renderer/renderer.h" // MeshVertex only : this module never calls the renderer

#ifdef __cplusplus
extern "C" {
#endif

// A 3D model file (FBX, OBJ through ufbx ; glTF 2.0, GLB through cgltf) flattened into one static,
// indexed triangle list ready for create_mesh() : every mesh node is triangulated and baked into model
// space (node transforms applied), in the engine's convention (right-handed, +Y up, 1 unit = 1 meter,
// whatever axes/units the file was exported with). Triangles are grouped by material into submeshes.

#define MODEL_NO_TEXTURE (-1)

// An image a material refers to : a file on disk, or the embedded contents of one.
typedef struct
{
    char path[512];   // resolved against the model's folder ; "" if embedded
    void* data;       // embedded image file contents (owned), NULL if external
    size_t size;
} ModelTexture;

typedef struct
{
    char name[64];
    float base_color[4];
    float emissive[3];
    float metallic;
    float roughness;
    float normal_scale;
    float occlusion_strength;
    float alpha_cutoff;        // 0 : opaque
    int base_color_texture;    // index into ModelData::textures, or MODEL_NO_TEXTURE
    int normal_texture;
    int metallic_roughness_texture; // glTF convention : G = roughness, B = metallic
    int emissive_texture;
    int occlusion_texture;     // R = occlusion
} ModelMaterial;

typedef struct
{
    int first_index;
    int index_count;
    int material;              // index into ModelData::materials
} ModelSubmesh;

typedef struct
{
    MeshVertex* vertices;
    uint32_t* indices;
    int vertex_count;
    int index_count;
    ModelSubmesh* submeshes;
    int submesh_count;
    ModelMaterial* materials;
    int material_count;
    ModelTexture* textures;
    int texture_count;
    float bounds_min[3];       // axis-aligned bounding box
    float bounds_max[3];
} ModelData;

// `file_path` is absolute, or relative to the executable. 0 on failure.
int model_load(const char* file_path, ModelData* out_model);
void model_free(ModelData* model);

#ifdef __cplusplus
}
#endif

#endif // _MODEL_H_
