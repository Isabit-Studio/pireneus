#include "model/model.h"

#include <SDL3/SDL.h>
#include <stdio.h>
#include <float.h>

#include "ufbx.h"

#define CGLTF_IMPLEMENTATION
#include "cgltf.h"

#include "debug.h"

// Arrays growing as the file is read, then handed over to ModelData.
typedef struct
{
    MeshVertex* vertices;
    int vertex_count;
    int vertex_capacity;
    uint32_t* indices;
    int index_count;
    int index_capacity;
    ModelSubmesh* submeshes;
    int submesh_count;
    ModelMaterial* materials;
    int material_count;
    ModelTexture* textures;
    int texture_count;
    int texture_capacity;
} Builder;

static void builder_reserve(Builder* builder, int extra_vertices, int extra_indices)
{
    if (builder->vertex_count + extra_vertices > builder->vertex_capacity)
    {
        builder->vertex_capacity = SDL_max(builder->vertex_capacity * 2, builder->vertex_count + extra_vertices);
        builder->vertices = SDL_realloc(builder->vertices, sizeof(MeshVertex) * (size_t)builder->vertex_capacity);
    }

    if (builder->index_count + extra_indices > builder->index_capacity)
    {
        builder->index_capacity = SDL_max(builder->index_capacity * 2, builder->index_count + extra_indices);
        builder->indices = SDL_realloc(builder->indices, sizeof(uint32_t) * (size_t)builder->index_capacity);
    }
}

static void default_material(ModelMaterial* material, const char* name)
{
    SDL_zerop(material);
    SDL_snprintf(material->name, sizeof(material->name), "%s", name ? name : "Material");
    material->base_color[0] = material->base_color[1] = material->base_color[2] = material->base_color[3] = 1.0f;
    material->metallic = 0.0f;
    material->roughness = 0.5f;
    material->normal_scale = 1.0f;
    material->occlusion_strength = 1.0f;
    material->base_color_texture = MODEL_NO_TEXTURE;
    material->normal_texture = MODEL_NO_TEXTURE;
    material->metallic_roughness_texture = MODEL_NO_TEXTURE;
    material->emissive_texture = MODEL_NO_TEXTURE;
    material->occlusion_texture = MODEL_NO_TEXTURE;
}

static void directory_of(const char* path, char* out, size_t out_size)
{
    SDL_snprintf(out, out_size, "%s", path);

    char* separator = NULL;

    for (char* c = out; *c; c++)
    {
        if (*c == '/' || *c == '\\')
        {
            separator = c;
        }
    }

    if (separator)
    {
        separator[1] = '\0';
    }
    else
    {
        out[0] = '\0';
    }
}

static int is_absolute(const char* path)
{
    return path[0] == '/' || path[0] == '\\' || (path[0] && path[1] == ':');
}

// Adds an external image, given its path as written in the model file ; reuses an existing entry.
static int add_external_texture(Builder* builder, const char* directory, const char* relative)
{
    if (!relative || !relative[0])
    {
        return MODEL_NO_TEXTURE;
    }

    char path[512];

    if (is_absolute(relative))
    {
        SDL_snprintf(path, sizeof(path), "%s", relative);
    }
    else
    {
        SDL_snprintf(path, sizeof(path), "%s%s", directory, relative);
    }

    for (int i = 0; i < builder->texture_count; i++)
    {
        if (SDL_strcmp(builder->textures[i].path, path) == 0)
        {
            return i;
        }
    }

    if (builder->texture_count == builder->texture_capacity)
    {
        builder->texture_capacity = SDL_max(8, builder->texture_capacity * 2);
        builder->textures = SDL_realloc(builder->textures, sizeof(ModelTexture) * (size_t)builder->texture_capacity);
    }

    ModelTexture* texture = &builder->textures[builder->texture_count];
    SDL_zerop(texture);
    SDL_snprintf(texture->path, sizeof(texture->path), "%s", path);

    return builder->texture_count++;
}

static int add_embedded_texture(Builder* builder, const void* data, size_t size)
{
    if (builder->texture_count == builder->texture_capacity)
    {
        builder->texture_capacity = SDL_max(8, builder->texture_capacity * 2);
        builder->textures = SDL_realloc(builder->textures, sizeof(ModelTexture) * (size_t)builder->texture_capacity);
    }

    ModelTexture* texture = &builder->textures[builder->texture_count];
    SDL_zerop(texture);
    texture->data = SDL_malloc(size);
    texture->size = size;
    SDL_memcpy(texture->data, data, size);

    return builder->texture_count++;
}

// Per-vertex tangents from the triangles' UV gradients (Lengyel), once the mesh is indexed.
static void generate_tangents(MeshVertex* vertices, int vertex_count, const uint32_t* indices, int index_count)
{
    float* accumulated = SDL_calloc((size_t)vertex_count * 6, sizeof(float)); // tangent, bitangent

    for (int i = 0; i + 2 < index_count; i += 3)
    {
        const MeshVertex* a = &vertices[indices[i]];
        const MeshVertex* b = &vertices[indices[i + 1]];
        const MeshVertex* c = &vertices[indices[i + 2]];

        float e1[3] = { b->x - a->x, b->y - a->y, b->z - a->z };
        float e2[3] = { c->x - a->x, c->y - a->y, c->z - a->z };
        float du1 = b->u - a->u;
        float dv1 = b->v - a->v;
        float du2 = c->u - a->u;
        float dv2 = c->v - a->v;
        float det = du1 * dv2 - du2 * dv1;
        float r = SDL_fabsf(det) > 1e-12f ? 1.0f / det : 0.0f;

        float t[3] = { (e1[0] * dv2 - e2[0] * dv1) * r, (e1[1] * dv2 - e2[1] * dv1) * r, (e1[2] * dv2 - e2[2] * dv1) * r };
        float bt[3] = { (e2[0] * du1 - e1[0] * du2) * r, (e2[1] * du1 - e1[1] * du2) * r, (e2[2] * du1 - e1[2] * du2) * r };

        for (int k = 0; k < 3; k++)
        {
            float* entry = &accumulated[(size_t)indices[i + k] * 6];

            for (int axis = 0; axis < 3; axis++)
            {
                entry[axis] += t[axis];
                entry[axis + 3] += bt[axis];
            }
        }
    }

    for (int i = 0; i < vertex_count; i++)
    {
        MeshVertex* v = &vertices[i];
        const float* entry = &accumulated[(size_t)i * 6];

        // Gram-Schmidt against the normal ; any perpendicular vector for degenerate UVs.
        float n_dot_t = v->nx * entry[0] + v->ny * entry[1] + v->nz * entry[2];
        float t[3] = { entry[0] - v->nx * n_dot_t, entry[1] - v->ny * n_dot_t, entry[2] - v->nz * n_dot_t };
        float length = SDL_sqrtf(t[0] * t[0] + t[1] * t[1] + t[2] * t[2]);

        if (length < 1e-6f)
        {
            float helper[3] = { SDL_fabsf(v->ny) < 0.99f ? 0.0f : 1.0f, SDL_fabsf(v->ny) < 0.99f ? 1.0f : 0.0f, 0.0f };
            t[0] = helper[1] * v->nz - helper[2] * v->ny;
            t[1] = helper[2] * v->nx - helper[0] * v->nz;
            t[2] = helper[0] * v->ny - helper[1] * v->nx;
            length = SDL_max(1e-6f, SDL_sqrtf(t[0] * t[0] + t[1] * t[1] + t[2] * t[2]));
        }

        v->tx = t[0] / length;
        v->ty = t[1] / length;
        v->tz = t[2] / length;

        // Handedness. Texture V goes down while a normal map's +Y means "up" : the bitangent the shader
        // rebuilds as cross(n, t) * w must point opposite to the accumulated dP/dV.
        float cx = v->ny * v->tz - v->nz * v->ty;
        float cy = v->nz * v->tx - v->nx * v->tz;
        float cz = v->nx * v->ty - v->ny * v->tx;
        v->tw = (cx * entry[3] + cy * entry[4] + cz * entry[5]) < 0.0f ? 1.0f : -1.0f;
    }

    SDL_free(accumulated);
}

static void finish(Builder* builder, ModelData* out_model)
{
    generate_tangents(builder->vertices, builder->vertex_count, builder->indices, builder->index_count);

    out_model->vertices = builder->vertices;
    out_model->vertex_count = builder->vertex_count;
    out_model->indices = builder->indices;
    out_model->index_count = builder->index_count;
    out_model->submeshes = builder->submeshes;
    out_model->submesh_count = builder->submesh_count;
    out_model->materials = builder->materials;
    out_model->material_count = builder->material_count;
    out_model->textures = builder->textures;
    out_model->texture_count = builder->texture_count;

    for (int axis = 0; axis < 3; axis++)
    {
        out_model->bounds_min[axis] = FLT_MAX;
        out_model->bounds_max[axis] = -FLT_MAX;
    }

    for (int i = 0; i < builder->vertex_count; i++)
    {
        const float position[3] = { builder->vertices[i].x, builder->vertices[i].y, builder->vertices[i].z };

        for (int axis = 0; axis < 3; axis++)
        {
            out_model->bounds_min[axis] = SDL_min(out_model->bounds_min[axis], position[axis]);
            out_model->bounds_max[axis] = SDL_max(out_model->bounds_max[axis], position[axis]);
        }
    }
}

// --- FBX, OBJ (ufbx) ----------------------------------------------------------------------------

static int fbx_texture(Builder* builder, const char* directory, const ufbx_material_map* map)
{
    const ufbx_texture* texture = map->texture;

    if (!texture)
    {
        return MODEL_NO_TEXTURE;
    }

    if (texture->content.size > 0)
    {
        return add_embedded_texture(builder, texture->content.data, texture->content.size);
    }

    return add_external_texture(builder, directory, texture->relative_filename.length ? texture->relative_filename.data : texture->filename.data);
}

static void fbx_read_material(Builder* builder, const char* directory, const ufbx_material* source, ModelMaterial* material)
{
    default_material(material, source ? source->name.data : "Default");

    if (!source)
    {
        return;
    }

    // ufbx exposes both the PBR view (Blender's Principled BSDF base color, as exported) and the
    // legacy FBX diffuse color ; prefer the former, fall back to the latter.
    const ufbx_material_map* base = source->pbr.base_color.has_value ? &source->pbr.base_color : &source->fbx.diffuse_color;

    if (base->has_value)
    {
        material->base_color[0] = (float)base->value_vec4.x;
        material->base_color[1] = (float)base->value_vec4.y;
        material->base_color[2] = (float)base->value_vec4.z;
        material->base_color[3] = base->value_components >= 4 ? (float)base->value_vec4.w : 1.0f;
    }

    if (source->pbr.roughness.has_value)
    {
        material->roughness = SDL_clamp((float)source->pbr.roughness.value_real, 0.04f, 1.0f);
    }

    if (source->pbr.metalness.has_value)
    {
        material->metallic = SDL_clamp((float)source->pbr.metalness.value_real, 0.0f, 1.0f);
    }

    if (source->pbr.emission_color.has_value)
    {
        float factor = source->pbr.emission_factor.has_value ? (float)source->pbr.emission_factor.value_real : 1.0f;

        for (int i = 0; i < 3; i++)
        {
            material->emissive[i] = (float)source->pbr.emission_color.value_vec4.v[i] * factor;
        }
    }

    material->base_color_texture = fbx_texture(builder, directory, base);
    material->normal_texture = fbx_texture(builder, directory, source->fbx.normal_map.texture ? &source->fbx.normal_map : &source->pbr.normal_map);
    material->emissive_texture = fbx_texture(builder, directory, &source->pbr.emission_color);
    material->occlusion_texture = fbx_texture(builder, directory, &source->pbr.ambient_occlusion);

    // FBX has separate roughness/metalness maps, the engine one packed map : a roughness map alone
    // is used when it's the only one (it reads G ; greyscale maps have it in every channel).
    material->metallic_roughness_texture = fbx_texture(builder, directory, &source->pbr.roughness);

    if (material->metallic_roughness_texture != MODEL_NO_TEXTURE)
    {
        material->roughness = 1.0f;
        material->metallic = 0.0f;
    }
}

static int load_ufbx(const char* path, Builder* builder)
{
    ufbx_load_opts options = { 0 };
    options.target_axes = ufbx_axes_right_handed_y_up;
    options.target_unit_meters = 1.0f;
    options.generate_missing_normals = true;

    ufbx_error error;
    ufbx_scene* scene = ufbx_load_file(path, &options, &error);

    if (!scene)
    {
        printf("Could not load model '%s': %s\n", path, error.description.data);
        return 0;
    }

    char directory[512];
    directory_of(path, directory, sizeof(directory));

    size_t max_face_triangles = 0;
    size_t triangle_count = 0;

    for (size_t i = 0; i < scene->nodes.count; i++)
    {
        const ufbx_mesh* mesh = scene->nodes.data[i]->mesh;

        if (mesh)
        {
            max_face_triangles = SDL_max(max_face_triangles, mesh->max_face_triangles);
            triangle_count += mesh->num_triangles;
        }
    }

    if (triangle_count == 0)
    {
        printf("Model '%s' contains no mesh.\n", path);
        ufbx_free_scene(scene);
        return 0;
    }

    uint32_t* face_indices = SDL_malloc(sizeof(uint32_t) * max_face_triangles * 3);

    // One material per entry of the scene, plus a last one for faces with none.
    builder->material_count = (int)scene->materials.count + 1;
    builder->materials = SDL_malloc(sizeof(ModelMaterial) * (size_t)builder->material_count);
    builder->submeshes = SDL_malloc(sizeof(ModelSubmesh) * (size_t)builder->material_count);

    for (size_t m = 0; m < scene->materials.count; m++)
    {
        fbx_read_material(builder, directory, scene->materials.data[m], &builder->materials[m]);
    }

    fbx_read_material(builder, directory, NULL, &builder->materials[scene->materials.count]);

    // Material by material, so each becomes one contiguous submesh.
    for (size_t m = 0; m <= scene->materials.count; m++)
    {
        const ufbx_material* wanted = m < scene->materials.count ? scene->materials.data[m] : NULL;
        int first_index = builder->index_count;

        for (size_t n = 0; n < scene->nodes.count; n++)
        {
            const ufbx_node* node = scene->nodes.data[n];
            const ufbx_mesh* mesh = node->mesh;

            if (!mesh)
            {
                continue;
            }

            ufbx_matrix normal_matrix = ufbx_matrix_for_normals(&node->geometry_to_world);

            for (size_t f = 0; f < mesh->faces.count; f++)
            {
                const ufbx_material* material = NULL;

                if (mesh->face_material.count > f && mesh->face_material.data[f] < mesh->materials.count)
                {
                    material = mesh->materials.data[mesh->face_material.data[f]];
                }

                if (material != wanted)
                {
                    continue;
                }

                ufbx_face face = mesh->faces.data[f];
                uint32_t face_triangles = ufbx_triangulate_face(face_indices, max_face_triangles * 3, mesh, face);

                builder_reserve(builder, (int)face_triangles * 3, (int)face_triangles * 3);

                for (uint32_t c = 0; c < face_triangles * 3; c++)
                {
                    uint32_t index = face_indices[c];

                    ufbx_vec3 position = ufbx_transform_position(&node->geometry_to_world, ufbx_get_vertex_vec3(&mesh->vertex_position, index));
                    ufbx_vec3 normal = ufbx_transform_direction(&normal_matrix, ufbx_get_vertex_vec3(&mesh->vertex_normal, index));
                    double normal_length = SDL_sqrt(normal.x * normal.x + normal.y * normal.y + normal.z * normal.z);

                    if (normal_length > 0.0)
                    {
                        normal.x /= normal_length;
                        normal.y /= normal_length;
                        normal.z /= normal_length;
                    }

                    MeshVertex* vertex = &builder->vertices[builder->vertex_count];
                    SDL_zerop(vertex);
                    vertex->x = (float)position.x;
                    vertex->y = (float)position.y;
                    vertex->z = (float)position.z;
                    vertex->nx = (float)normal.x;
                    vertex->ny = (float)normal.y;
                    vertex->nz = (float)normal.z;
                    vertex->r = builder->materials[m].base_color[0];
                    vertex->g = builder->materials[m].base_color[1];
                    vertex->b = builder->materials[m].base_color[2];
                    vertex->a = builder->materials[m].base_color[3];

                    if (mesh->vertex_uv.exists)
                    {
                        ufbx_vec2 uv = ufbx_get_vertex_vec2(&mesh->vertex_uv, index);
                        vertex->u = (float)uv.x;
                        vertex->v = 1.0f - (float)uv.y; // FBX V goes up, textures' V goes down
                    }

                    builder->indices[builder->index_count++] = (uint32_t)builder->vertex_count;
                    builder->vertex_count++;
                }
            }
        }

        if (builder->index_count > first_index)
        {
            ModelSubmesh* submesh = &builder->submeshes[builder->submesh_count++];
            submesh->first_index = first_index;
            submesh->index_count = builder->index_count - first_index;
            submesh->material = (int)m;
        }
    }

    SDL_free(face_indices);
    ufbx_free_scene(scene);

    return 1;
}

// --- glTF 2.0 (cgltf) ---------------------------------------------------------------------------

static void percent_decode(char* text)
{
    char* out = text;

    for (const char* in = text; *in; in++)
    {
        if (in[0] == '%' && in[1] && in[2])
        {
            char hex[3] = { in[1], in[2], 0 };
            *out++ = (char)SDL_strtol(hex, NULL, 16);
            in += 2;
        }
        else
        {
            *out++ = *in;
        }
    }

    *out = '\0';
}

static int gltf_texture(Builder* builder, const char* directory, const cgltf_texture_view* view)
{
    const cgltf_texture* texture = view->texture;

    if (!texture || !texture->image)
    {
        return MODEL_NO_TEXTURE;
    }

    const cgltf_image* image = texture->image;

    if (image->buffer_view)
    {
        return add_embedded_texture(builder, (const uint8_t*)image->buffer_view->buffer->data + image->buffer_view->offset, image->buffer_view->size);
    }

    if (image->uri && SDL_strncmp(image->uri, "data:", 5) != 0)
    {
        char relative[512];
        SDL_snprintf(relative, sizeof(relative), "%s", image->uri);
        percent_decode(relative);
        return add_external_texture(builder, directory, relative);
    }

    return MODEL_NO_TEXTURE;
}

static void gltf_read_material(Builder* builder, const char* directory, const cgltf_material* source, ModelMaterial* material)
{
    default_material(material, source && source->name ? source->name : "Default");

    if (!source)
    {
        return;
    }

    if (source->has_pbr_metallic_roughness)
    {
        const cgltf_pbr_metallic_roughness* pbr = &source->pbr_metallic_roughness;

        for (int i = 0; i < 4; i++)
        {
            material->base_color[i] = pbr->base_color_factor[i];
        }

        material->metallic = pbr->metallic_factor;
        material->roughness = pbr->roughness_factor;
        material->base_color_texture = gltf_texture(builder, directory, &pbr->base_color_texture);
        material->metallic_roughness_texture = gltf_texture(builder, directory, &pbr->metallic_roughness_texture);
    }

    for (int i = 0; i < 3; i++)
    {
        material->emissive[i] = source->emissive_factor[i];
    }

    material->normal_scale = source->normal_texture.texture ? source->normal_texture.scale : 1.0f;
    material->occlusion_strength = source->occlusion_texture.texture ? source->occlusion_texture.scale : 1.0f;
    material->normal_texture = gltf_texture(builder, directory, &source->normal_texture);
    material->emissive_texture = gltf_texture(builder, directory, &source->emissive_texture);
    material->occlusion_texture = gltf_texture(builder, directory, &source->occlusion_texture);

    if (source->alpha_mode == cgltf_alpha_mode_mask)
    {
        material->alpha_cutoff = source->alpha_cutoff;
    }
}

static void transform_point(const float m[16], const float p[3], float out[3])
{
    // cgltf matrices are column-major.
    for (int r = 0; r < 3; r++)
    {
        out[r] = m[r] * p[0] + m[4 + r] * p[1] + m[8 + r] * p[2] + m[12 + r];
    }
}

static void transform_direction(const float m[16], const float d[3], float out[3])
{
    for (int r = 0; r < 3; r++)
    {
        out[r] = m[r] * d[0] + m[4 + r] * d[1] + m[8 + r] * d[2];
    }
}

static void gltf_add_primitive(Builder* builder, const cgltf_node* node, const cgltf_primitive* primitive, int material_index)
{
    if (primitive->type != cgltf_primitive_type_triangles)
    {
        return;
    }

    const cgltf_accessor* positions = NULL;
    const cgltf_accessor* normals = NULL;
    const cgltf_accessor* uvs = NULL;
    const cgltf_accessor* colors = NULL;

    for (cgltf_size a = 0; a < primitive->attributes_count; a++)
    {
        const cgltf_attribute* attribute = &primitive->attributes[a];

        if (attribute->type == cgltf_attribute_type_position)
        {
            positions = attribute->data;
        }
        else if (attribute->type == cgltf_attribute_type_normal)
        {
            normals = attribute->data;
        }
        else if (attribute->type == cgltf_attribute_type_texcoord && attribute->index == 0)
        {
            uvs = attribute->data;
        }
        else if (attribute->type == cgltf_attribute_type_color && attribute->index == 0)
        {
            colors = attribute->data;
        }
    }

    if (!positions)
    {
        return;
    }

    float world[16];
    cgltf_node_transform_world(node, world);

    // Inverse transpose of the upper 3x3, for normals (cofactors : the scale of the determinant
    // doesn't matter, normals get normalized).
    float normal_matrix[16] = { 0 };
    normal_matrix[0] = world[5] * world[10] - world[9] * world[6];
    normal_matrix[1] = world[8] * world[6] - world[4] * world[10];
    normal_matrix[2] = world[4] * world[9] - world[8] * world[5];
    normal_matrix[4] = world[9] * world[2] - world[1] * world[10];
    normal_matrix[5] = world[0] * world[10] - world[8] * world[2];
    normal_matrix[6] = world[8] * world[1] - world[0] * world[9];
    normal_matrix[8] = world[1] * world[6] - world[5] * world[2];
    normal_matrix[9] = world[4] * world[2] - world[0] * world[6];
    normal_matrix[10] = world[0] * world[5] - world[4] * world[1];

    int first_vertex = builder->vertex_count;
    int vertex_count = (int)positions->count;
    int index_count = primitive->indices ? (int)primitive->indices->count : vertex_count;

    builder_reserve(builder, vertex_count, index_count);

    const ModelMaterial* material = &builder->materials[material_index];

    for (int i = 0; i < vertex_count; i++)
    {
        MeshVertex* vertex = &builder->vertices[first_vertex + i];
        SDL_zerop(vertex);

        float position[3];
        float out[3];
        cgltf_accessor_read_float(positions, (cgltf_size)i, position, 3);
        transform_point(world, position, out);
        vertex->x = out[0];
        vertex->y = out[1];
        vertex->z = out[2];

        if (normals)
        {
            float normal[3];
            cgltf_accessor_read_float(normals, (cgltf_size)i, normal, 3);
            transform_direction(normal_matrix, normal, out);
            float length = SDL_max(1e-8f, SDL_sqrtf(out[0] * out[0] + out[1] * out[1] + out[2] * out[2]));
            vertex->nx = out[0] / length;
            vertex->ny = out[1] / length;
            vertex->nz = out[2] / length;
        }

        if (uvs)
        {
            float uv[2];
            cgltf_accessor_read_float(uvs, (cgltf_size)i, uv, 2);
            vertex->u = uv[0];
            vertex->v = uv[1];
        }

        float color[4] = { 1.0f, 1.0f, 1.0f, 1.0f };

        if (colors)
        {
            cgltf_accessor_read_float(colors, (cgltf_size)i, color, cgltf_num_components(colors->type));
        }

        vertex->r = color[0] * material->base_color[0];
        vertex->g = color[1] * material->base_color[1];
        vertex->b = color[2] * material->base_color[2];
        vertex->a = color[3] * material->base_color[3];
    }

    builder->vertex_count += vertex_count;

    int first_index = builder->index_count;

    for (int i = 0; i < index_count; i++)
    {
        uint32_t index = primitive->indices ? (uint32_t)cgltf_accessor_read_index(primitive->indices, (cgltf_size)i) : (uint32_t)i;
        builder->indices[builder->index_count++] = (uint32_t)first_vertex + index;
    }

    if (!normals)
    {
        // Flat normals : accumulate each triangle's face normal on its vertices.
        for (int i = first_index; i + 2 < builder->index_count; i += 3)
        {
            MeshVertex* a = &builder->vertices[builder->indices[i]];
            MeshVertex* b = &builder->vertices[builder->indices[i + 1]];
            MeshVertex* c = &builder->vertices[builder->indices[i + 2]];
            float e1[3] = { b->x - a->x, b->y - a->y, b->z - a->z };
            float e2[3] = { c->x - a->x, c->y - a->y, c->z - a->z };
            float n[3] = { e1[1] * e2[2] - e1[2] * e2[1], e1[2] * e2[0] - e1[0] * e2[2], e1[0] * e2[1] - e1[1] * e2[0] };
            MeshVertex* corners[3] = { a, b, c };

            for (int k = 0; k < 3; k++)
            {
                corners[k]->nx += n[0];
                corners[k]->ny += n[1];
                corners[k]->nz += n[2];
            }
        }

        for (int i = first_vertex; i < builder->vertex_count; i++)
        {
            MeshVertex* v = &builder->vertices[i];
            float length = SDL_max(1e-8f, SDL_sqrtf(v->nx * v->nx + v->ny * v->ny + v->nz * v->nz));
            v->nx /= length;
            v->ny /= length;
            v->nz /= length;
        }
    }
}

static int load_gltf(const char* path, Builder* builder)
{
    cgltf_options options;
    SDL_zero(options);

    cgltf_data* data = NULL;
    cgltf_result result = cgltf_parse_file(&options, path, &data);

    if (result != cgltf_result_success)
    {
        printf("Could not parse glTF '%s' (error %d).\n", path, (int)result);
        return 0;
    }

    result = cgltf_load_buffers(&options, data, path);

    if (result != cgltf_result_success)
    {
        printf("Could not load the buffers of glTF '%s' (error %d).\n", path, (int)result);
        cgltf_free(data);
        return 0;
    }

    char directory[512];
    directory_of(path, directory, sizeof(directory));

    builder->material_count = (int)data->materials_count + 1;
    builder->materials = SDL_malloc(sizeof(ModelMaterial) * (size_t)builder->material_count);
    builder->submeshes = SDL_malloc(sizeof(ModelSubmesh) * (size_t)builder->material_count);

    for (cgltf_size m = 0; m < data->materials_count; m++)
    {
        gltf_read_material(builder, directory, &data->materials[m], &builder->materials[m]);
    }

    gltf_read_material(builder, directory, NULL, &builder->materials[data->materials_count]);

    for (cgltf_size m = 0; m <= data->materials_count; m++)
    {
        const cgltf_material* wanted = m < data->materials_count ? &data->materials[m] : NULL;
        int first_index = builder->index_count;

        for (cgltf_size n = 0; n < data->nodes_count; n++)
        {
            const cgltf_node* node = &data->nodes[n];

            if (!node->mesh)
            {
                continue;
            }

            for (cgltf_size p = 0; p < node->mesh->primitives_count; p++)
            {
                if (node->mesh->primitives[p].material == wanted)
                {
                    gltf_add_primitive(builder, node, &node->mesh->primitives[p], (int)m);
                }
            }
        }

        if (builder->index_count > first_index)
        {
            ModelSubmesh* submesh = &builder->submeshes[builder->submesh_count++];
            submesh->first_index = first_index;
            submesh->index_count = builder->index_count - first_index;
            submesh->material = (int)m;
        }
    }

    cgltf_free(data);

    if (builder->index_count == 0)
    {
        printf("Model '%s' contains no mesh.\n", path);
        return 0;
    }

    return 1;
}

static int has_extension(const char* path, const char* extension)
{
    size_t path_length = SDL_strlen(path);
    size_t extension_length = SDL_strlen(extension);

    return path_length >= extension_length && SDL_strcasecmp(path + path_length - extension_length, extension) == 0;
}

int model_load(const char* file_path, ModelData* out_model)
{
    SDL_zerop(out_model);

    char path[512];

    if (is_absolute(file_path))
    {
        SDL_snprintf(path, sizeof(path), "%s", file_path);
    }
    else
    {
        const char* base_path = SDL_GetBasePath();
        SDL_snprintf(path, sizeof(path), "%s%s", base_path ? base_path : "", file_path);
    }

    Builder builder;
    SDL_zero(builder);

    int loaded = (has_extension(path, ".gltf") || has_extension(path, ".glb")) ? load_gltf(path, &builder) : load_ufbx(path, &builder);

    if (!loaded)
    {
        ModelData partial;
        SDL_zero(partial);
        partial.textures = builder.textures;
        partial.texture_count = builder.texture_count;
        partial.vertices = builder.vertices;
        partial.indices = builder.indices;
        partial.submeshes = builder.submeshes;
        partial.materials = builder.materials;
        model_free(&partial);
        return 0;
    }

    finish(&builder, out_model);

    DEBUG_LOG("[model] loaded \"%s\": %d triangles, %d materials, %d textures, bounds (%.2f, %.2f, %.2f) - (%.2f, %.2f, %.2f)\n", path,
        out_model->index_count / 3, out_model->material_count, out_model->texture_count,
        out_model->bounds_min[0], out_model->bounds_min[1], out_model->bounds_min[2],
        out_model->bounds_max[0], out_model->bounds_max[1], out_model->bounds_max[2]);

    return 1;
}

void model_free(ModelData* model)
{
    for (int i = 0; i < model->texture_count; i++)
    {
        SDL_free(model->textures[i].data);
    }

    SDL_free(model->textures);
    SDL_free(model->materials);
    SDL_free(model->submeshes);
    SDL_free(model->vertices);
    SDL_free(model->indices);
    SDL_zerop(model);
}
