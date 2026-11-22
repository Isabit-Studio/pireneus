#ifndef _PROJECT_H_
#define _PROJECT_H_

#include <stdint.h>

#include "renderer/photoreal/photoreal.h"

#ifdef __cplusplus
extern "C" {
#endif

// A game project : a folder holding a project.pireneus file (these settings, in JSON) and an Assets
// folder, into which the asset browser imports textures, models, sounds...

#define PROJECT_FILE_NAME "project.pireneus"
#define PROJECT_MAX_LAYERS 16
#define PROJECT_PATH_SIZE 512

typedef enum
{
    PROJECT_TEMPLATE_3D,
    PROJECT_TEMPLATE_2D
} ProjectTemplate;

typedef enum
{
    RENDER_PATH_PHOTOREAL,
    RENDER_PATH_FORWARD   // the simple forward renderer, for GPUs that can't run the other, or for speed
} RenderPath;

typedef enum
{
    QUALITY_LOW,
    QUALITY_MEDIUM,
    QUALITY_HIGH,
    QUALITY_ULTRA,
    QUALITY_CUSTOM,
    QUALITY_PRESET_COUNT
} QualityPreset;

typedef struct
{
    // Physics
    float gravity_3d[3];
    float gravity_2d[2];
    int physics_rate;        // steps per second
    int physics_substeps;
    float default_friction;
    float default_restitution;

    // Collision : named layers, and which ones collide with which (a symmetric matrix of bits).
    char layer_names[PROJECT_MAX_LAYERS][32];
    uint16_t layer_masks[PROJECT_MAX_LAYERS];

    // Character controller
    float character_speed;
    float character_sprint_multiplier;
    float character_jump_speed;
    float character_acceleration;
    float character_air_control;   // 0 to 1
    float character_max_slope;     // degrees
    float character_mass;

    // Rendering
    int render_path;               // RenderPath
    PhotorealLook look;

    // Render quality
    int quality_preset;            // QualityPreset
    PhotorealQuality quality;

    // Audio
    float master_volume;
    float music_volume;
    float sound_volume;

    // Application
    char startup_scene[256];       // relative to the project's root
} ProjectSettings;

typedef struct
{
    char name[64];
    char root[PROJECT_PATH_SIZE];  // absolute, with a trailing separator
    int template_kind;             // ProjectTemplate
    ProjectSettings settings;
} Project;

void project_default_settings(ProjectSettings* out_settings, ProjectTemplate template_kind);

// Fills the quality settings of a preset. QUALITY_CUSTOM leaves them as they are.
void project_apply_quality_preset(ProjectSettings* settings, QualityPreset preset);

// Creates `<parent_directory>/<name>/` with its folders and settings. 0 on failure.
int project_create(const char* parent_directory, const char* name, ProjectTemplate template_kind, Project* out_project);

// `path` : the project's folder, or its project.pireneus file. 0 on failure.
int project_open(const char* path, Project* out_project);

int project_save(const Project* project); // 0 on failure

// Recently opened projects, most recent first, kept in the user's preferences folder.
#define PROJECT_MAX_RECENT 12
int project_recent_list(char out_paths[PROJECT_MAX_RECENT][PROJECT_PATH_SIZE]);
void project_recent_add(const char* root);
void project_recent_remove(const char* root);

// Where new projects go by default : a folder in the user's documents.
void project_default_location(char* out_path, int out_size);

#ifdef __cplusplus
}
#endif

#endif // _PROJECT_H_
