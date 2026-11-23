#ifndef _ASSET_DATABASE_H_
#define _ASSET_DATABASE_H_

#include <stdint.h>

#include "audio/audio.h"
#include "model/model.h"
#include "project/project.h"
#include "renderer/render3d/pbr_texture.h"
#include "renderer/render3d/render3d.h"
#include "renderer/renderer.h"

#ifdef __cplusplus
extern "C" {
#endif

// The current project's assets : everything under its Assets folder, classified by type, and a cache of
// what's been loaded from them. Assets are known by their path relative to the project's root, with
// forward slashes (e.g. "Assets/Models/crate.fbx") ; "builtin/cube" names a model built into the engine.

#define ASSET_PATH_SIZE 192

typedef enum
{
    ASSET_FOLDER,
    ASSET_TEXTURE,
    ASSET_SPRITE,
    ASSET_MODEL,
    ASSET_AUDIO,
    ASSET_SKYBOX,
    ASSET_SCENE,
    ASSET_TILEMAP,
    ASSET_OTHER,
    ASSET_TYPE_COUNT
} AssetType;

typedef struct
{
    char path[ASSET_PATH_SIZE];
    char name[96];
    AssetType type;
    uint64_t size;
} AssetEntry;

void asset_database_open(const Project* project);
void asset_database_close(void);  // unloads everything ; the GPU must be idle
void asset_database_refresh(void); // re-scans the folders

int asset_database_count(void);
const AssetEntry* asset_database_get(int index);
const char* asset_type_name(AssetType type);

// What kind of asset a file would be, from its extension.
AssetType asset_type_for_file(const char* path);

// Where a type of asset is imported by default.
const char* asset_default_folder(AssetType type);

// Copies an external file into the project : into `folder` (relative, NULL for the type's default
// folder). `as_type` overrides the type deduced from the file's extension (a JPEG as a skybox or a
// sprite), or ASSET_OTHER to leave it. The new relative path is written to out_path. 0 on failure.
int asset_import(const char* source_path, const char* folder, AssetType as_type, char* out_path, int out_size);

int asset_create_folder(const char* parent, const char* name);
int asset_rename(const char* path, const char* new_name);
int asset_trash(const char* path); // to the system's trash

void asset_absolute_path(const char* path, char* out, int out_size);

// --- Loaded data ----------------------------------------------------------------------------------
// Loaded on first use and kept until the project closes. NULL on failure.

typedef struct
{
    Mesh* mesh;
    ModelData data;
    PbrMaterial* materials;    // one per ModelData::materials
    Texture** textures;        // owned : one per ModelData::textures, NULL if unused
    PbrTextureInfo* texture_info; // one per ModelData::textures : what loading each found
    float bounds[6];           // min xyz, max xyz
} LoadedModel;

const LoadedModel* asset_get_model(const char* path);
Texture* asset_get_material_texture(const char* path, int srgb);
Texture* asset_get_sprite(const char* path, int* out_width, int* out_height);
Texture* asset_get_thumbnail(const char* path);
const Environment* asset_get_environment(const char* path);
AudioClip* asset_get_audio(const char* path);

// Forgets one asset's loaded data, after it was renamed or trashed. The GPU must be idle.
void asset_forget(const char* path);

// --- Built-in materials ---------------------------------------------------------------------------
// The level editor's "dev" materials, Hammer-style : grids in flat colors whose textures the engine
// generates (a 1 m tile, lines every 25 cm), and tool materials, drawn in the editor only.
// Known as ASSET_BUILTIN_MATERIALS + name, e.g. "builtin/materials/dev_grey". asset_get_material_texture()
// and asset_get_thumbnail() accept them like texture files.

#define ASSET_BUILTIN_MATERIALS "builtin/materials/"
#define ASSET_DEFAULT_MATERIAL ASSET_BUILTIN_MATERIALS "dev_grey"

typedef struct
{
    const char* path;
    const char* label;
    float color[3];          // sRGB : what the texture averages to
    int tool;                // not drawn in the game (still collides), like Hammer's nodraw and clip
} BuiltinMaterial;

int asset_builtin_material_count(void);
const BuiltinMaterial* asset_builtin_material(int index);
const BuiltinMaterial* asset_find_builtin_material(const char* path); // NULL if it isn't one

// A surface's flat color, for what can't show textures (the forward renderer, the 2D views).
void asset_material_color(const char* path, float out_color[3]);
int asset_is_tool_material(const char* path);

#ifdef __cplusplus
}
#endif

#endif // _ASSET_DATABASE_H_
