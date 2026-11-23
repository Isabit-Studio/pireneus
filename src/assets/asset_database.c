// The project's asset database : a scan of the Assets folder into typed entries, imports (copies),
// and lazy caches of what's loaded from those files (models, textures, thumbnails, environments,
// sounds). Also the engine's built-in assets : the primitives and the level editor's dev materials.

#include "assets/asset_database.h"

#include <SDL3/SDL.h>
#include <SDL3_image/SDL_image.h>
#include <stdio.h>

#include "assets/hdr_image.h"
#include "platforms/platforms_paths.h"
#include "renderer/render3d/pbr_texture.h"
#include "scene/scene.h"
#include "debug.h"

#define MAX_ENTRIES 8192
#define THUMBNAIL_SIZE 128
#define BUILTIN_DIRECTORY "resources/3D-test/Primitives/"

typedef enum
{
    CACHE_MODEL,
    CACHE_MATERIAL_TEXTURE,
    CACHE_MATERIAL_TEXTURE_SRGB,
    CACHE_SPRITE,
    CACHE_THUMBNAIL,
    CACHE_ENVIRONMENT,
    CACHE_AUDIO
} CacheKind;

typedef struct
{
    char path[ASSET_PATH_SIZE];
    CacheKind kind;
    void* data;     // NULL : loading failed, don't retry
    int width;
    int height;
} CacheEntry;

static char project_root[PROJECT_PATH_SIZE];
static AssetEntry* entries;
static int entry_count;
static CacheEntry* cache;
static int cache_count;
static int cache_capacity;

const char* asset_type_name(AssetType type)
{
    static const char* const names[ASSET_TYPE_COUNT] = { "Folder", "Texture", "Sprite", "3D Model", "Audio", "Skybox", "Scene", "Tilemap", "File" };
    return names[type];
}

static const char* extension_of(const char* path)
{
    const char* dot = SDL_strrchr(path, '.');
    return dot ? dot + 1 : "";
}

AssetType asset_type_for_file(const char* path)
{
    const char* e = extension_of(path);

    static const char* const textures[] = { "png", "jpg", "jpeg", "tga", "bmp" };
    static const char* const models[] = { "fbx", "obj", "gltf", "glb" };
    static const char* const audio[] = { "wav", "ogg", "mp3", "flac", "aiff", "aif" };
    static const char* const skyboxes[] = { "exr", "hdr" };

    for (size_t i = 0; i < sizeof(textures) / sizeof(textures[0]); i++)
    {
        if (SDL_strcasecmp(e, textures[i]) == 0)
        {
            return ASSET_TEXTURE;
        }
    }

    for (size_t i = 0; i < sizeof(models) / sizeof(models[0]); i++)
    {
        if (SDL_strcasecmp(e, models[i]) == 0)
        {
            return ASSET_MODEL;
        }
    }

    for (size_t i = 0; i < sizeof(audio) / sizeof(audio[0]); i++)
    {
        if (SDL_strcasecmp(e, audio[i]) == 0)
        {
            return ASSET_AUDIO;
        }
    }

    for (size_t i = 0; i < sizeof(skyboxes) / sizeof(skyboxes[0]); i++)
    {
        if (SDL_strcasecmp(e, skyboxes[i]) == 0)
        {
            return ASSET_SKYBOX;
        }
    }

    if (SDL_strcasecmp(e, "scene") == 0)
    {
        return ASSET_SCENE;
    }

    if (SDL_strcasecmp(e, "json") == 0 || SDL_strcasecmp(e, "tmj") == 0)
    {
        return ASSET_TILEMAP;
    }

    return ASSET_OTHER;
}

const char* asset_default_folder(AssetType type)
{
    switch (type)
    {
    case ASSET_TEXTURE: return "Assets/Textures";
    case ASSET_SPRITE: return "Assets/Sprites";
    case ASSET_MODEL: return "Assets/Models";
    case ASSET_AUDIO: return "Assets/Audio";
    case ASSET_SKYBOX: return "Assets/Skyboxes";
    case ASSET_SCENE: return "Assets/Scenes";
    case ASSET_TILEMAP: return "Assets/Tilemaps";
    default: return "Assets";
    }
}

// The folder decides between what shares a file format : images under Sprites are sprites, under
// Skyboxes panoramas.
static AssetType classify(const char* relative_path)
{
    AssetType type = asset_type_for_file(relative_path);

    if (type == ASSET_TEXTURE)
    {
        if (SDL_strstr(relative_path, "/Sprites/"))
        {
            return ASSET_SPRITE;
        }

        if (SDL_strstr(relative_path, "/Skyboxes/"))
        {
            return ASSET_SKYBOX;
        }
    }

    return type;
}

static SDL_EnumerationResult SDLCALL scan_entry(void* userdata, const char* dirname, const char* fname)
{
    const char* relative_directory = userdata;

    if (fname[0] == '.' || entry_count >= MAX_ENTRIES)
    {
        return SDL_ENUM_CONTINUE;
    }

    char absolute[PROJECT_PATH_SIZE + ASSET_PATH_SIZE];
    SDL_snprintf(absolute, sizeof(absolute), "%s%s", dirname, fname);

    SDL_PathInfo info;

    if (!SDL_GetPathInfo(absolute, &info))
    {
        return SDL_ENUM_CONTINUE;
    }

    AssetEntry* entry = &entries[entry_count++];
    SDL_snprintf(entry->path, sizeof(entry->path), "%s/%s", relative_directory, fname);
    SDL_snprintf(entry->name, sizeof(entry->name), "%s", fname);
    entry->size = info.size;
    entry->type = info.type == SDL_PATHTYPE_DIRECTORY ? ASSET_FOLDER : classify(entry->path);

    if (info.type == SDL_PATHTYPE_DIRECTORY)
    {
        char relative[ASSET_PATH_SIZE];
        SDL_snprintf(relative, sizeof(relative), "%s", entry->path);

        char child[PROJECT_PATH_SIZE + ASSET_PATH_SIZE];
        SDL_snprintf(child, sizeof(child), "%s/", absolute);
        SDL_EnumerateDirectory(child, scan_entry, relative);
    }

    return SDL_ENUM_CONTINUE;
}

static int compare_entries(const void* a, const void* b)
{
    const AssetEntry* x = a;
    const AssetEntry* y = b;

    if ((x->type == ASSET_FOLDER) != (y->type == ASSET_FOLDER))
    {
        return x->type == ASSET_FOLDER ? -1 : 1;
    }

    return SDL_strcasecmp(x->path, y->path);
}

void asset_database_refresh(void)
{
    entry_count = 0;

    if (!project_root[0])
    {
        return;
    }

    char assets[PROJECT_PATH_SIZE + 16];
    SDL_snprintf(assets, sizeof(assets), "%sAssets/", project_root);

    char relative[] = "Assets";
    SDL_EnumerateDirectory(assets, scan_entry, relative);

    SDL_qsort(entries, (size_t)entry_count, sizeof(AssetEntry), compare_entries);
}

void asset_database_open(const Project* project)
{
    asset_database_close();

    SDL_snprintf(project_root, sizeof(project_root), "%s", project->root);
    entries = SDL_malloc(sizeof(AssetEntry) * MAX_ENTRIES);
    asset_database_refresh();

    DEBUG_LOG("[assets] %d entries in \"%s\"\n", entry_count, project_root);
}

static void release(CacheEntry* entry)
{
    if (!entry->data)
    {
        return;
    }

    switch (entry->kind)
    {
    case CACHE_MODEL:
    {
        LoadedModel* model = entry->data;

        for (int i = 0; i < model->data.texture_count; i++)
        {
            if (model->textures[i])
            {
                destroy_texture(model->textures[i]);
            }
        }

        SDL_free(model->textures);
        SDL_free(model->texture_info);
        SDL_free(model->materials);
        destroy_mesh(model->mesh);
        model_free(&model->data);
        SDL_free(model);
        break;
    }

    case CACHE_MATERIAL_TEXTURE:
    case CACHE_MATERIAL_TEXTURE_SRGB:
    case CACHE_SPRITE:
    case CACHE_THUMBNAIL:
        destroy_texture(entry->data);
        break;

    case CACHE_ENVIRONMENT:
        environment_destroy(entry->data);
        SDL_free(entry->data);
        break;

    case CACHE_AUDIO:
        audio_destroy_clip(entry->data);
        break;
    }

    entry->data = NULL;
}

void asset_database_close(void)
{
    for (int i = 0; i < cache_count; i++)
    {
        release(&cache[i]);
    }

    SDL_free(cache);
    cache = NULL;
    cache_count = 0;
    cache_capacity = 0;

    SDL_free(entries);
    entries = NULL;
    entry_count = 0;
    project_root[0] = '\0';
}

int asset_database_count(void)
{
    return entry_count;
}

const AssetEntry* asset_database_get(int index)
{
    return &entries[index];
}

void asset_absolute_path(const char* path, char* out, int out_size)
{
    if (SDL_strncmp(path, SCENE_BUILTIN_PREFIX, SDL_strlen(SCENE_BUILTIN_PREFIX)) == 0)
    {
        const char* base_path = SDL_GetBasePath();
        SDL_snprintf(out, (size_t)out_size, "%s" BUILTIN_DIRECTORY "%s.fbx", base_path ? base_path : "", path + SDL_strlen(SCENE_BUILTIN_PREFIX));
        return;
    }

    SDL_snprintf(out, (size_t)out_size, "%s%s", project_root, path);
}

static const char* file_name_of(const char* path)
{
    const char* name = path;

    for (const char* c = path; *c; c++)
    {
        if (*c == '/' || *c == '\\')
        {
            name = c + 1;
        }
    }

    return name;
}

int asset_import(const char* source_path, const char* folder, AssetType as_type, char* out_path, int out_size)
{
    AssetType type = as_type != ASSET_OTHER ? as_type : asset_type_for_file(source_path);
    const char* destination_folder = folder ? folder : asset_default_folder(type);
    const char* name = file_name_of(source_path);

    char directory[PROJECT_PATH_SIZE + ASSET_PATH_SIZE];
    SDL_snprintf(directory, sizeof(directory), "%s%s", project_root, destination_folder);
    SDL_CreateDirectory(directory);

    // A free name : "crate.png", "crate 2.png", "crate 3.png"...
    char relative[ASSET_PATH_SIZE];
    char absolute[PROJECT_PATH_SIZE + ASSET_PATH_SIZE];
    const char* dot = SDL_strrchr(name, '.');
    int stem_length = dot ? (int)(dot - name) : (int)SDL_strlen(name);

    for (int number = 1; ; number++)
    {
        if (number == 1)
        {
            SDL_snprintf(relative, sizeof(relative), "%s/%s", destination_folder, name);
        }
        else
        {
            SDL_snprintf(relative, sizeof(relative), "%s/%.*s %d%s", destination_folder, stem_length, name, number, dot ? dot : "");
        }

        SDL_snprintf(absolute, sizeof(absolute), "%s%s", project_root, relative);

        if (!SDL_GetPathInfo(absolute, NULL))
        {
            break;
        }
    }

    if (!SDL_CopyFile(source_path, absolute))
    {
        printf("Could not import '%s': %s\n", source_path, SDL_GetError());
        return 0;
    }

    if (out_path)
    {
        SDL_snprintf(out_path, (size_t)out_size, "%s", relative);
    }

    asset_database_refresh();

    DEBUG_LOG("[assets] imported \"%s\" as \"%s\"\n", source_path, relative);

    return 1;
}

int asset_create_folder(const char* parent, const char* name)
{
    char path[PROJECT_PATH_SIZE + ASSET_PATH_SIZE];
    SDL_snprintf(path, sizeof(path), "%s%s/%s", project_root, parent, name);

    int created = SDL_CreateDirectory(path);
    asset_database_refresh();

    return created;
}

int asset_rename(const char* path, const char* new_name)
{
    char source[PROJECT_PATH_SIZE + ASSET_PATH_SIZE];
    char destination[PROJECT_PATH_SIZE + ASSET_PATH_SIZE];
    SDL_snprintf(source, sizeof(source), "%s%s", project_root, path);
    SDL_snprintf(destination, sizeof(destination), "%s%.*s%s", project_root, (int)(file_name_of(path) - path), path, new_name);

    if (SDL_GetPathInfo(destination, NULL))
    {
        return 0;
    }

    int renamed = SDL_RenamePath(source, destination);
    asset_database_refresh();

    return renamed;
}

int asset_trash(const char* path)
{
    char absolute[PROJECT_PATH_SIZE + ASSET_PATH_SIZE];
    SDL_snprintf(absolute, sizeof(absolute), "%s%s", project_root, path);

    int trashed = platform_move_to_trash(absolute);
    asset_database_refresh();

    return trashed;
}

void asset_forget(const char* path)
{
    for (int i = 0; i < cache_count; i++)
    {
        if (SDL_strcmp(cache[i].path, path) == 0)
        {
            release(&cache[i]);
            cache[i] = cache[--cache_count];
            i--;
        }
    }
}

static CacheEntry* find_cached(const char* path, CacheKind kind)
{
    for (int i = 0; i < cache_count; i++)
    {
        if (cache[i].kind == kind && SDL_strcmp(cache[i].path, path) == 0)
        {
            return &cache[i];
        }
    }

    return NULL;
}

static CacheEntry* add_cached(const char* path, CacheKind kind)
{
    if (cache_count == cache_capacity)
    {
        cache_capacity = SDL_max(64, cache_capacity * 2);
        cache = SDL_realloc(cache, sizeof(CacheEntry) * (size_t)cache_capacity);
    }

    CacheEntry* entry = &cache[cache_count++];
    SDL_zerop(entry);
    SDL_snprintf(entry->path, sizeof(entry->path), "%s", path);
    entry->kind = kind;

    return entry;
}

// One of a model's images, as a GPU texture : `srgb` is decided by which material slot uses it first.
static Texture* model_texture(LoadedModel* model, int index, int srgb)
{
    if (index == MODEL_NO_TEXTURE)
    {
        return NULL;
    }

    if (!model->textures[index])
    {
        const ModelTexture* source = &model->data.textures[index];
        model->textures[index] = source->data ? pbr_texture_load_memory(source->data, source->size, srgb) :
            pbr_texture_load_info(source->path, srgb, &model->texture_info[index]);
    }

    return model->textures[index];
}

const LoadedModel* asset_get_model(const char* path)
{
    CacheEntry* cached = find_cached(path, CACHE_MODEL);

    if (cached)
    {
        return cached->data;
    }

    char absolute[PROJECT_PATH_SIZE + ASSET_PATH_SIZE];
    asset_absolute_path(path, absolute, sizeof(absolute));

    LoadedModel* model = SDL_calloc(1, sizeof(LoadedModel));
    cached = add_cached(path, CACHE_MODEL);
    Uint64 start = SDL_GetTicks();

    if (!model_load(absolute, &model->data))
    {
        SDL_free(model);
        return NULL;
    }

    model->mesh = create_mesh(model->data.vertices, model->data.vertex_count, model->data.indices, model->data.index_count);

    if (!model->mesh)
    {
        model_free(&model->data);
        SDL_free(model);
        return NULL;
    }

    for (int axis = 0; axis < 3; axis++)
    {
        model->bounds[axis] = model->data.bounds_min[axis];
        model->bounds[axis + 3] = model->data.bounds_max[axis];
    }

    model->textures = SDL_calloc((size_t)SDL_max(1, model->data.texture_count), sizeof(Texture*));
    model->texture_info = SDL_calloc((size_t)SDL_max(1, model->data.texture_count), sizeof(PbrTextureInfo));
    model->materials = SDL_malloc(sizeof(PbrMaterial) * (size_t)SDL_max(1, model->data.material_count));

    for (int i = 0; i < model->data.material_count; i++)
    {
        const ModelMaterial* source = &model->data.materials[i];
        PbrMaterial* material = &model->materials[i];

        render3d_default_material(material);
        material->metallic = source->metallic;
        material->roughness = source->roughness;
        material->normal_scale = source->normal_scale;
        material->occlusion_strength = source->occlusion_strength;
        material->alpha_cutoff = source->alpha_cutoff;
        SDL_memcpy(material->emissive, source->emissive, sizeof(material->emissive));

        // The base color factor is already in the vertices' colors.
        material->base_color_texture = model_texture(model, source->base_color_texture, 1);
        material->emissive_texture = model_texture(model, source->emissive_texture, 1);
        material->normal_texture = model_texture(model, source->normal_texture, 0);
        material->metallic_roughness_texture = model_texture(model, source->metallic_roughness_texture, 0);
        material->occlusion_texture = model_texture(model, source->occlusion_texture, 0);

        if (source->normal_texture != MODEL_NO_TEXTURE)
        {
            material->normal_y_down = model->texture_info[source->normal_texture].normal_y_down;
        }

        // Foliage and the like : a cutout, when the file can't say and the base color's alpha does.
        if (source->alpha_from_texture && material->alpha_cutoff <= 0.0f && source->base_color_texture != MODEL_NO_TEXTURE &&
            model->texture_info[source->base_color_texture].alpha_mask)
        {
            material->alpha_cutoff = 0.5f;
        }
    }

    cached->data = model;

    DEBUG_LOG("[assets] model \"%s\" ready in %llu ms (%d submeshes, %d textures)\n", path, (unsigned long long)(SDL_GetTicks() - start),
        model->data.submesh_count, model->data.texture_count);

    return model;
}

// --- Built-in materials ---------------------------------------------------------------------------

typedef enum
{
    PATTERN_GRID,
    PATTERN_NODRAW,
    PATTERN_CLIP
} MaterialPattern;

typedef struct
{
    BuiltinMaterial info;
    int pattern;
} BuiltinMaterialDefinition;

static const BuiltinMaterialDefinition builtin_materials[] = {
    { { ASSET_BUILTIN_MATERIALS "dev_grey", "Dev Grey", { 0.56f, 0.56f, 0.56f }, 0 }, PATTERN_GRID },
    { { ASSET_BUILTIN_MATERIALS "dev_light", "Dev Light", { 0.80f, 0.79f, 0.76f }, 0 }, PATTERN_GRID },
    { { ASSET_BUILTIN_MATERIALS "dev_dark", "Dev Dark", { 0.27f, 0.28f, 0.30f }, 0 }, PATTERN_GRID },
    { { ASSET_BUILTIN_MATERIALS "dev_orange", "Dev Orange", { 0.92f, 0.47f, 0.13f }, 0 }, PATTERN_GRID },
    { { ASSET_BUILTIN_MATERIALS "dev_blue", "Dev Blue", { 0.22f, 0.44f, 0.84f }, 0 }, PATTERN_GRID },
    { { ASSET_BUILTIN_MATERIALS "dev_green", "Dev Green", { 0.33f, 0.62f, 0.29f }, 0 }, PATTERN_GRID },
    { { ASSET_BUILTIN_MATERIALS "dev_red", "Dev Red", { 0.76f, 0.20f, 0.17f }, 0 }, PATTERN_GRID },
    { { ASSET_BUILTIN_MATERIALS "dev_yellow", "Dev Yellow", { 0.90f, 0.78f, 0.22f }, 0 }, PATTERN_GRID },
    { { ASSET_BUILTIN_MATERIALS "tool_nodraw", "Nodraw", { 0.85f, 0.80f, 0.30f }, 1 }, PATTERN_NODRAW },
    { { ASSET_BUILTIN_MATERIALS "tool_clip", "Clip", { 0.62f, 0.32f, 0.78f }, 1 }, PATTERN_CLIP }
};

#define BUILTIN_MATERIAL_COUNT ((int)(sizeof(builtin_materials) / sizeof(builtin_materials[0])))
#define DEV_TEXTURE_SIZE 256

int asset_builtin_material_count(void)
{
    return BUILTIN_MATERIAL_COUNT;
}

const BuiltinMaterial* asset_builtin_material(int index)
{
    return index >= 0 && index < BUILTIN_MATERIAL_COUNT ? &builtin_materials[index].info : NULL;
}

static const BuiltinMaterialDefinition* find_builtin_definition(const char* path)
{
    if (SDL_strncmp(path, ASSET_BUILTIN_MATERIALS, SDL_strlen(ASSET_BUILTIN_MATERIALS)) != 0)
    {
        return NULL;
    }

    for (int i = 0; i < BUILTIN_MATERIAL_COUNT; i++)
    {
        if (SDL_strcmp(builtin_materials[i].info.path, path) == 0)
        {
            return &builtin_materials[i];
        }
    }

    return NULL;
}

const BuiltinMaterial* asset_find_builtin_material(const char* path)
{
    const BuiltinMaterialDefinition* definition = find_builtin_definition(path);
    return definition ? &definition->info : NULL;
}

void asset_material_color(const char* path, float out[3])
{
    const BuiltinMaterial* material = asset_find_builtin_material(path);

    for (int c = 0; c < 3; c++)
    {
        out[c] = material ? material->color[c] : 0.8f;
    }
}

int asset_is_tool_material(const char* path)
{
    const BuiltinMaterial* material = asset_find_builtin_material(path);
    return material && material->tool;
}

static uint8_t shade(float value, float factor)
{
    return (uint8_t)SDL_clamp((int)(value * factor * 255.0f + 0.5f), 0, 255);
}

// A dev texture : one meter, with lines every 25 cm and fainter ones every 6.25 cm, a darker border.
static uint8_t* builtin_pixels(const BuiltinMaterialDefinition* definition, int size)
{
    uint8_t* pixels = SDL_malloc((size_t)size * (size_t)size * 4);
    const float* color = definition->info.color;

    for (int y = 0; y < size; y++)
    {
        for (int x = 0; x < size; x++)
        {
            float factor = 1.0f;
            int quarter = size / 4;
            int sixteenth = SDL_max(1, size / 16);
            int border = SDL_max(1, size / 64);

            if (x < border || y < border || x >= size - border || y >= size - border)
            {
                factor = 0.68f;
            }
            else if (x % quarter < border || y % quarter < border)
            {
                factor = 0.82f;
            }
            else if (x % sixteenth == 0 || y % sixteenth == 0)
            {
                factor = 0.93f;
            }

            if (definition->pattern == PATTERN_NODRAW)
            {
                // Two diagonals across the tile.
                int d1 = SDL_abs(x - y);
                int d2 = SDL_abs(x + y - (size - 1));

                if (d1 < border * 2 || d2 < border * 2)
                {
                    factor = 0.45f;
                }
            }
            else if (definition->pattern == PATTERN_CLIP)
            {
                if (((x + y) / (size / 8)) % 2 == 0)
                {
                    factor *= 0.78f;
                }
            }

            uint8_t* p = &pixels[((size_t)y * (size_t)size + (size_t)x) * 4];
            p[0] = shade(color[0], factor);
            p[1] = shade(color[1], factor);
            p[2] = shade(color[2], factor);
            p[3] = 255;
        }
    }

    return pixels;
}

Texture* asset_get_material_texture(const char* path, int srgb)
{
    CacheKind kind = srgb ? CACHE_MATERIAL_TEXTURE_SRGB : CACHE_MATERIAL_TEXTURE;
    CacheEntry* cached = find_cached(path, kind);

    if (cached)
    {
        return cached->data;
    }

    const BuiltinMaterialDefinition* builtin = find_builtin_definition(path);

    if (builtin)
    {
        uint8_t* pixels = builtin_pixels(builtin, DEV_TEXTURE_SIZE);
        cached = add_cached(path, kind);
        cached->data = pbr_texture_from_pixels(pixels, DEV_TEXTURE_SIZE, DEV_TEXTURE_SIZE, 1);
        SDL_free(pixels);
        return cached->data;
    }

    char absolute[PROJECT_PATH_SIZE + ASSET_PATH_SIZE];
    asset_absolute_path(path, absolute, sizeof(absolute));

    cached = add_cached(path, kind);
    cached->data = pbr_texture_load(absolute, srgb);

    return cached->data;
}

static Texture* texture_from_surface(SDL_Surface* surface, int* out_width, int* out_height)
{
    SDL_Surface* rgba = SDL_ConvertSurface(surface, SDL_PIXELFORMAT_RGBA32);

    if (!rgba)
    {
        return NULL;
    }

    uint8_t* tight = SDL_malloc((size_t)rgba->w * (size_t)rgba->h * 4);

    for (int y = 0; y < rgba->h; y++)
    {
        SDL_memcpy(tight + (size_t)y * (size_t)rgba->w * 4, (const uint8_t*)rgba->pixels + (size_t)y * (size_t)rgba->pitch, (size_t)rgba->w * 4);
    }

    Texture* texture = create_texture_from_pixels(tight, rgba->w, rgba->h);

    if (out_width)
    {
        *out_width = rgba->w;
    }

    if (out_height)
    {
        *out_height = rgba->h;
    }

    SDL_free(tight);
    SDL_DestroySurface(rgba);

    return texture;
}

Texture* asset_get_sprite(const char* path, int* out_width, int* out_height)
{
    CacheEntry* cached = find_cached(path, CACHE_SPRITE);

    if (!cached)
    {
        char absolute[PROJECT_PATH_SIZE + ASSET_PATH_SIZE];
        asset_absolute_path(path, absolute, sizeof(absolute));

        cached = add_cached(path, CACHE_SPRITE);

        SDL_Surface* surface = IMG_Load(absolute);

        if (surface)
        {
            cached->data = texture_from_surface(surface, &cached->width, &cached->height);
            SDL_DestroySurface(surface);
        }
    }

    if (out_width)
    {
        *out_width = cached->width;
    }

    if (out_height)
    {
        *out_height = cached->height;
    }

    return cached->data;
}

// HDR panoramas are tone mapped for their preview.
static Texture* hdr_thumbnail(const char* absolute)
{
    HdrImage image;

    if (!hdr_image_load(absolute, &image))
    {
        return NULL;
    }

    int width = THUMBNAIL_SIZE;
    int height = THUMBNAIL_SIZE / 2;
    uint8_t* pixels = SDL_malloc((size_t)width * (size_t)height * 4);

    for (int y = 0; y < height; y++)
    {
        for (int x = 0; x < width; x++)
        {
            const float* p = &image.pixels[((size_t)(y * image.height / height) * (size_t)image.width + (size_t)(x * image.width / width)) * 3];
            uint8_t* o = &pixels[((size_t)y * (size_t)width + (size_t)x) * 4];

            for (int c = 0; c < 3; c++)
            {
                float mapped = p[c] / (1.0f + p[c]);
                o[c] = (uint8_t)(SDL_powf(mapped, 1.0f / 2.2f) * 255.0f);
            }

            o[3] = 255;
        }
    }

    Texture* texture = create_texture_from_pixels(pixels, width, height);

    SDL_free(pixels);
    hdr_image_free(&image);

    return texture;
}

Texture* asset_get_thumbnail(const char* path)
{
    CacheEntry* cached = find_cached(path, CACHE_THUMBNAIL);

    if (cached)
    {
        return cached->data;
    }

    const BuiltinMaterialDefinition* builtin = find_builtin_definition(path);

    if (builtin)
    {
        uint8_t* pixels = builtin_pixels(builtin, 64);
        cached = add_cached(path, CACHE_THUMBNAIL);
        cached->data = create_texture_from_pixels(pixels, 64, 64);
        SDL_free(pixels);
        return cached->data;
    }

    char absolute[PROJECT_PATH_SIZE + ASSET_PATH_SIZE];
    asset_absolute_path(path, absolute, sizeof(absolute));

    cached = add_cached(path, CACHE_THUMBNAIL);

    const char* e = extension_of(path);

    if (SDL_strcasecmp(e, "exr") == 0 || SDL_strcasecmp(e, "hdr") == 0)
    {
        cached->data = hdr_thumbnail(absolute);
        return cached->data;
    }

    SDL_Surface* surface = IMG_Load(absolute);

    if (!surface)
    {
        return NULL;
    }

    float scale = (float)THUMBNAIL_SIZE / (float)SDL_max(surface->w, surface->h);

    if (scale < 1.0f)
    {
        SDL_Surface* small = SDL_ScaleSurface(surface, SDL_max(1, (int)((float)surface->w * scale)), SDL_max(1, (int)((float)surface->h * scale)), SDL_SCALEMODE_LINEAR);

        if (small)
        {
            SDL_DestroySurface(surface);
            surface = small;
        }
    }

    cached->data = texture_from_surface(surface, NULL, NULL);
    SDL_DestroySurface(surface);

    return cached->data;
}

const Environment* asset_get_environment(const char* path)
{
    CacheEntry* cached = find_cached(path, CACHE_ENVIRONMENT);

    if (cached)
    {
        return cached->data;
    }

    char absolute[PROJECT_PATH_SIZE + ASSET_PATH_SIZE];
    asset_absolute_path(path, absolute, sizeof(absolute));

    cached = add_cached(path, CACHE_ENVIRONMENT);

    Environment* environment = SDL_malloc(sizeof(Environment));

    if (!environment_load(absolute, environment))
    {
        SDL_free(environment);
        return NULL;
    }

    cached->data = environment;

    return environment;
}

AudioClip* asset_get_audio(const char* path)
{
    CacheEntry* cached = find_cached(path, CACHE_AUDIO);

    if (cached)
    {
        return cached->data;
    }

    char absolute[PROJECT_PATH_SIZE + ASSET_PATH_SIZE];
    asset_absolute_path(path, absolute, sizeof(absolute));

    cached = add_cached(path, CACHE_AUDIO);
    cached->data = audio_load(absolute, 0);

    return cached->data;
}
