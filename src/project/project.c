#include "project/project.h"

#include <SDL3/SDL.h>
#include <stdio.h>

#include "json/json.h"
#include "platforms/platforms_paths.h"
#include "debug.h"

static const char* const asset_folders[] = {
    "Assets", "Assets/Textures", "Assets/Sprites", "Assets/Models", "Assets/Audio", "Assets/Skyboxes", "Assets/Scenes", "Assets/Tilemaps"
};

void project_default_settings(ProjectSettings* s, ProjectTemplate template_kind)
{
    SDL_zerop(s);

    s->gravity_3d[1] = -9.81f;
    s->gravity_2d[1] = -9.81f;
    s->physics_rate = 60;
    s->physics_substeps = 4;
    s->default_friction = 0.6f;
    s->default_restitution = 0.0f;

    SDL_snprintf(s->layer_names[0], sizeof(s->layer_names[0]), "Default");
    SDL_snprintf(s->layer_names[1], sizeof(s->layer_names[1]), "Player");
    SDL_snprintf(s->layer_names[2], sizeof(s->layer_names[2]), "Environment");
    SDL_snprintf(s->layer_names[3], sizeof(s->layer_names[3]), "Props");

    for (int i = 0; i < PROJECT_MAX_LAYERS; i++)
    {
        s->layer_masks[i] = 0xFFFFu;
    }

    s->character_speed = 5.0f;
    s->character_sprint_multiplier = 1.8f;
    s->character_jump_speed = 6.0f;
    s->character_acceleration = 40.0f;
    s->character_air_control = 0.25f;
    s->character_max_slope = 50.0f;
    s->character_mass = 75.0f;

    s->render_path = template_kind == PROJECT_TEMPLATE_2D ? RENDER_PATH_FORWARD : RENDER_PATH_PHOTOREAL;
    photoreal_default_look(&s->look);

    photoreal_default_quality(&s->quality);
    project_apply_quality_preset(s, QUALITY_HIGH);

    s->master_volume = 1.0f;
    s->music_volume = 0.8f;
    s->sound_volume = 1.0f;

    SDL_snprintf(s->startup_scene, sizeof(s->startup_scene), "Assets/Scenes/Main.scene");
}

void project_apply_quality_preset(ProjectSettings* settings, QualityPreset preset)
{
    PhotorealQuality* q = &settings->quality;
    settings->quality_preset = preset;

    switch (preset)
    {
    case QUALITY_LOW:
        q->render_scale = 0.75f;
        q->shadow_resolution = 1024;
        q->shadow_cascades = 2;
        q->shadow_samples = 8;
        q->shadow_distance = 60.0f;
        q->ao_enabled = 0;
        q->ao_slices = 2;
        q->ao_steps = 3;
        q->bloom_enabled = 0;
        q->fxaa_enabled = 1;
        break;

    case QUALITY_MEDIUM:
        q->render_scale = 1.0f;
        q->shadow_resolution = 1024;
        q->shadow_cascades = 3;
        q->shadow_samples = 12;
        q->shadow_distance = 90.0f;
        q->ao_enabled = 1;
        q->ao_slices = 2;
        q->ao_steps = 3;
        q->bloom_enabled = 1;
        q->fxaa_enabled = 1;
        break;

    case QUALITY_HIGH:
        q->render_scale = 1.0f;
        q->shadow_resolution = 2048;
        q->shadow_cascades = 3;
        q->shadow_samples = 24;
        q->shadow_distance = 120.0f;
        q->ao_enabled = 1;
        q->ao_slices = 3;
        q->ao_steps = 5;
        q->bloom_enabled = 1;
        q->fxaa_enabled = 1;
        break;

    case QUALITY_ULTRA:
        q->render_scale = 1.5f;
        q->shadow_resolution = 4096;
        q->shadow_cascades = 4;
        q->shadow_samples = 32;
        q->shadow_distance = 200.0f;
        q->ao_enabled = 1;
        q->ao_slices = 4;
        q->ao_steps = 8;
        q->bloom_enabled = 1;
        q->fxaa_enabled = 1;
        break;

    default:
        break;
    }
}

// --- Writing ------------------------------------------------------------------------------------

typedef struct
{
    char* text;
    size_t length;
    size_t capacity;
} Writer;

static void write_text(Writer* w, const char* format, ...)
{
    char line[1024];
    va_list args;
    va_start(args, format);
    int written = SDL_vsnprintf(line, sizeof(line), format, args);
    va_end(args);

    if (written < 0)
    {
        return;
    }

    if (written >= (int)sizeof(line))
    {
        written = (int)sizeof(line) - 1;
    }

    if (w->length + (size_t)written + 1 > w->capacity)
    {
        w->capacity = SDL_max(w->capacity * 2, w->length + (size_t)written + 4096);
        w->text = SDL_realloc(w->text, w->capacity);
    }

    SDL_memcpy(w->text + w->length, line, (size_t)written);
    w->length += (size_t)written;
    w->text[w->length] = '\0';
}

// Paths and names : Windows separators and quotes would need escaping in JSON.
static void clean_string(const char* source, char* out, size_t out_size)
{
    size_t n = 0;

    for (const char* c = source; *c && n + 1 < out_size; c++)
    {
        if (*c != '"' && (unsigned char)*c >= 0x20)
        {
            out[n++] = *c == '\\' ? '/' : *c;
        }
    }

    out[n] = '\0';
}

int project_save(const Project* project)
{
    const ProjectSettings* s = &project->settings;
    Writer w = { 0 };
    char name[64];
    char scene[256];

    clean_string(project->name, name, sizeof(name));
    clean_string(s->startup_scene, scene, sizeof(scene));

    write_text(&w, "{\n  \"name\": \"%s\",\n  \"template\": \"%s\",\n", name, project->template_kind == PROJECT_TEMPLATE_2D ? "2d" : "3d");
    write_text(&w, "  \"startup_scene\": \"%s\",\n", scene);

    write_text(&w, "  \"physics\": { \"gravity_3d\": [%g, %g, %g], \"gravity_2d\": [%g, %g], \"rate\": %d, \"substeps\": %d, \"friction\": %g, \"restitution\": %g },\n",
        s->gravity_3d[0], s->gravity_3d[1], s->gravity_3d[2], s->gravity_2d[0], s->gravity_2d[1], s->physics_rate, s->physics_substeps, s->default_friction, s->default_restitution);

    write_text(&w, "  \"collision\": { \"layers\": [");

    for (int i = 0; i < PROJECT_MAX_LAYERS; i++)
    {
        char layer[32];
        clean_string(s->layer_names[i], layer, sizeof(layer));
        write_text(&w, "%s\"%s\"", i ? ", " : "", layer);
    }

    write_text(&w, "], \"masks\": [");

    for (int i = 0; i < PROJECT_MAX_LAYERS; i++)
    {
        write_text(&w, "%s%u", i ? ", " : "", (unsigned)s->layer_masks[i]);
    }

    write_text(&w, "] },\n");

    write_text(&w, "  \"character\": { \"speed\": %g, \"sprint\": %g, \"jump_speed\": %g, \"acceleration\": %g, \"air_control\": %g, \"max_slope\": %g, \"mass\": %g },\n",
        s->character_speed, s->character_sprint_multiplier, s->character_jump_speed, s->character_acceleration, s->character_air_control, s->character_max_slope, s->character_mass);

    const PhotorealLook* l = &s->look;
    write_text(&w, "  \"rendering\": { \"path\": \"%s\", \"exposure_ev\": %g, \"tonemapper\": %d, \"bloom_strength\": %g, \"contrast\": %g, \"saturation\": %g, \"vignette\": %g, ",
        s->render_path == RENDER_PATH_FORWARD ? "forward" : "photoreal", l->exposure_ev, (int)l->tonemapper, l->bloom_strength, l->contrast, l->saturation, l->vignette);
    write_text(&w, "\"ao_radius\": %g, \"ao_power\": %g, \"sky_intensity\": %g, \"bounce_strength\": %g, \"sun_softness\": %g, ",
        l->ao_radius, l->ao_power, l->sky_intensity, l->bounce_strength, l->sun_softness);
    write_text(&w, "\"fog_density\": %g, \"fog_height_falloff\": %g, \"fog_base_height\": %g, \"fog_color\": [%g, %g, %g], \"fog_sun_color\": [%g, %g, %g], \"fog_sun_power\": %g },\n",
        l->fog_density, l->fog_height_falloff, l->fog_base_height, l->fog_color[0], l->fog_color[1], l->fog_color[2],
        l->fog_sun_color[0], l->fog_sun_color[1], l->fog_sun_color[2], l->fog_sun_power);

    const PhotorealQuality* q = &s->quality;
    write_text(&w, "  \"quality\": { \"preset\": %d, \"render_scale\": %g, \"shadow_resolution\": %d, \"shadow_cascades\": %d, \"shadow_samples\": %d, \"shadow_distance\": %g, ",
        s->quality_preset, q->render_scale, q->shadow_resolution, q->shadow_cascades, q->shadow_samples, q->shadow_distance);
    write_text(&w, "\"ao\": %d, \"ao_slices\": %d, \"ao_steps\": %d, \"bloom\": %d, \"fxaa\": %d },\n",
        q->ao_enabled, q->ao_slices, q->ao_steps, q->bloom_enabled, q->fxaa_enabled);

    write_text(&w, "  \"audio\": { \"master\": %g, \"music\": %g, \"sound\": %g }\n}\n", s->master_volume, s->music_volume, s->sound_volume);

    char path[PROJECT_PATH_SIZE + 32];
    SDL_snprintf(path, sizeof(path), "%s%s", project->root, PROJECT_FILE_NAME);

    int saved = SDL_SaveFile(path, w.text, w.length);

    if (!saved)
    {
        printf("Could not save '%s': %s\n", path, SDL_GetError());
    }

    SDL_free(w.text);

    return saved;
}

// --- Reading ------------------------------------------------------------------------------------

static void read_floats(const JsonValue* array, float* out, int count)
{
    for (int i = 0; i < count; i++)
    {
        const JsonValue* value = json_array_get(array, (size_t)i);

        if (value)
        {
            out[i] = (float)json_number(value, out[i]);
        }
    }
}

static float read_float(const JsonValue* object, const char* key, float fallback)
{
    return (float)json_number(json_object_get(object, key), fallback);
}

static int read_int(const JsonValue* object, const char* key, int fallback)
{
    return json_int(json_object_get(object, key), fallback);
}

static void normalize_root(const char* path, char* out, size_t out_size)
{
    char copy[PROJECT_PATH_SIZE];
    SDL_snprintf(copy, sizeof(copy), "%s", path);

    size_t length = SDL_strlen(copy);
    size_t file_length = SDL_strlen(PROJECT_FILE_NAME);

    if (length >= file_length && SDL_strcasecmp(copy + length - file_length, PROJECT_FILE_NAME) == 0)
    {
        copy[length - file_length] = '\0';
        length -= file_length;
    }

    for (char* c = copy; *c; c++)
    {
        if (*c == '\\')
        {
            *c = '/';
        }
    }

    if (length > 0 && copy[length - 1] != '/')
    {
        SDL_snprintf(out, out_size, "%s/", copy);
    }
    else
    {
        SDL_snprintf(out, out_size, "%s", copy);
    }
}

int project_open(const char* path, Project* out_project)
{
    SDL_zerop(out_project);
    normalize_root(path, out_project->root, sizeof(out_project->root));

    char file[PROJECT_PATH_SIZE + 32];
    SDL_snprintf(file, sizeof(file), "%s%s", out_project->root, PROJECT_FILE_NAME);

    char* text = SDL_LoadFile(file, NULL);

    if (!text)
    {
        printf("Could not read '%s'.\n", file);
        return 0;
    }

    JsonValue* root = json_parse(text);
    SDL_free(text);

    if (!root)
    {
        printf("'%s' is not valid JSON.\n", file);
        return 0;
    }

    SDL_snprintf(out_project->name, sizeof(out_project->name), "%s", json_string(json_object_get(root, "name"), "Untitled"));
    out_project->template_kind = SDL_strcmp(json_string(json_object_get(root, "template"), "3d"), "2d") == 0 ? PROJECT_TEMPLATE_2D : PROJECT_TEMPLATE_3D;

    ProjectSettings* s = &out_project->settings;
    project_default_settings(s, (ProjectTemplate)out_project->template_kind);

    SDL_snprintf(s->startup_scene, sizeof(s->startup_scene), "%s", json_string(json_object_get(root, "startup_scene"), s->startup_scene));

    const JsonValue* physics = json_object_get(root, "physics");
    read_floats(json_object_get(physics, "gravity_3d"), s->gravity_3d, 3);
    read_floats(json_object_get(physics, "gravity_2d"), s->gravity_2d, 2);
    s->physics_rate = SDL_clamp(read_int(physics, "rate", s->physics_rate), 10, 480);
    s->physics_substeps = SDL_clamp(read_int(physics, "substeps", s->physics_substeps), 1, 16);
    s->default_friction = read_float(physics, "friction", s->default_friction);
    s->default_restitution = read_float(physics, "restitution", s->default_restitution);

    const JsonValue* collision = json_object_get(root, "collision");
    const JsonValue* layers = json_object_get(collision, "layers");
    const JsonValue* masks = json_object_get(collision, "masks");

    for (int i = 0; i < PROJECT_MAX_LAYERS; i++)
    {
        const JsonValue* layer = json_array_get(layers, (size_t)i);

        if (layer)
        {
            SDL_snprintf(s->layer_names[i], sizeof(s->layer_names[i]), "%s", json_string(layer, ""));
        }

        const JsonValue* mask = json_array_get(masks, (size_t)i);

        if (mask)
        {
            s->layer_masks[i] = (uint16_t)json_int(mask, 0xFFFF);
        }
    }

    const JsonValue* character = json_object_get(root, "character");
    s->character_speed = read_float(character, "speed", s->character_speed);
    s->character_sprint_multiplier = read_float(character, "sprint", s->character_sprint_multiplier);
    s->character_jump_speed = read_float(character, "jump_speed", s->character_jump_speed);
    s->character_acceleration = read_float(character, "acceleration", s->character_acceleration);
    s->character_air_control = read_float(character, "air_control", s->character_air_control);
    s->character_max_slope = read_float(character, "max_slope", s->character_max_slope);
    s->character_mass = read_float(character, "mass", s->character_mass);

    const JsonValue* rendering = json_object_get(root, "rendering");
    PhotorealLook* l = &s->look;
    const char* render_path = json_string(json_object_get(rendering, "path"), "");

    if (render_path[0] != '\0') // absent : the template's default stays
    {
        // Anything else, the former "path_traced" included, is the photorealistic renderer.
        s->render_path = SDL_strcmp(render_path, "forward") == 0 ? RENDER_PATH_FORWARD : RENDER_PATH_PHOTOREAL;
    }

    l->exposure_ev = read_float(rendering, "exposure_ev", l->exposure_ev);
    l->tonemapper = (PhotorealTonemapper)read_int(rendering, "tonemapper", (int)l->tonemapper);
    l->bloom_strength = read_float(rendering, "bloom_strength", l->bloom_strength);
    l->contrast = read_float(rendering, "contrast", l->contrast);
    l->saturation = read_float(rendering, "saturation", l->saturation);
    l->vignette = read_float(rendering, "vignette", l->vignette);
    l->ao_radius = read_float(rendering, "ao_radius", l->ao_radius);
    l->ao_power = read_float(rendering, "ao_power", l->ao_power);
    l->sky_intensity = read_float(rendering, "sky_intensity", l->sky_intensity);
    l->bounce_strength = read_float(rendering, "bounce_strength", l->bounce_strength);
    l->sun_softness = read_float(rendering, "sun_softness", l->sun_softness);
    l->fog_density = read_float(rendering, "fog_density", l->fog_density);
    l->fog_height_falloff = read_float(rendering, "fog_height_falloff", l->fog_height_falloff);
    l->fog_base_height = read_float(rendering, "fog_base_height", l->fog_base_height);
    read_floats(json_object_get(rendering, "fog_color"), l->fog_color, 3);
    read_floats(json_object_get(rendering, "fog_sun_color"), l->fog_sun_color, 3);
    l->fog_sun_power = read_float(rendering, "fog_sun_power", l->fog_sun_power);

    const JsonValue* quality = json_object_get(root, "quality");
    PhotorealQuality* q = &s->quality;
    s->quality_preset = SDL_clamp(read_int(quality, "preset", s->quality_preset), 0, QUALITY_PRESET_COUNT - 1);
    q->render_scale = read_float(quality, "render_scale", q->render_scale);
    q->shadow_resolution = SDL_clamp(read_int(quality, "shadow_resolution", q->shadow_resolution), 256, 8192);
    q->shadow_cascades = SDL_clamp(read_int(quality, "shadow_cascades", q->shadow_cascades), 1, 4);
    q->shadow_samples = SDL_clamp(read_int(quality, "shadow_samples", q->shadow_samples), 1, 64);
    q->shadow_distance = read_float(quality, "shadow_distance", q->shadow_distance);
    q->ao_enabled = read_int(quality, "ao", q->ao_enabled);
    q->ao_slices = SDL_clamp(read_int(quality, "ao_slices", q->ao_slices), 1, 8);
    q->ao_steps = SDL_clamp(read_int(quality, "ao_steps", q->ao_steps), 1, 16);
    q->bloom_enabled = read_int(quality, "bloom", q->bloom_enabled);
    q->fxaa_enabled = read_int(quality, "fxaa", q->fxaa_enabled);

    const JsonValue* audio = json_object_get(root, "audio");
    s->master_volume = read_float(audio, "master", s->master_volume);
    s->music_volume = read_float(audio, "music", s->music_volume);
    s->sound_volume = read_float(audio, "sound", s->sound_volume);

    json_destroy(root);

    project_recent_add(out_project->root);

    DEBUG_LOG("[project] opened \"%s\" (%s)\n", out_project->name, out_project->root);

    return 1;
}

int project_create(const char* parent_directory, const char* name, ProjectTemplate template_kind, Project* out_project)
{
    SDL_zerop(out_project);

    if (!name[0] || !parent_directory[0])
    {
        return 0;
    }

    for (const char* c = name; *c; c++)
    {
        if (SDL_strchr("<>:\"/\\|?*", *c) || (unsigned char)*c < 0x20)
        {
            printf("'%s' can't be used as a folder name.\n", name);
            return 0;
        }
    }

    char parent[PROJECT_PATH_SIZE];
    normalize_root(parent_directory, parent, sizeof(parent));

    SDL_snprintf(out_project->root, sizeof(out_project->root), "%s%s/", parent, name);
    SDL_snprintf(out_project->name, sizeof(out_project->name), "%s", name);
    out_project->template_kind = template_kind;
    project_default_settings(&out_project->settings, template_kind);

    char file[PROJECT_PATH_SIZE + 32];
    SDL_snprintf(file, sizeof(file), "%s%s", out_project->root, PROJECT_FILE_NAME);

    if (SDL_GetPathInfo(file, NULL))
    {
        printf("A project already exists in '%s'.\n", out_project->root);
        return 0;
    }

    if (!SDL_CreateDirectory(out_project->root))
    {
        printf("Could not create '%s': %s\n", out_project->root, SDL_GetError());
        return 0;
    }

    for (size_t i = 0; i < sizeof(asset_folders) / sizeof(asset_folders[0]); i++)
    {
        char folder[PROJECT_PATH_SIZE + 32];
        SDL_snprintf(folder, sizeof(folder), "%s%s", out_project->root, asset_folders[i]);
        SDL_CreateDirectory(folder);
    }

    if (!project_save(out_project))
    {
        return 0;
    }

    project_recent_add(out_project->root);

    DEBUG_LOG("[project] created \"%s\" (%s)\n", name, out_project->root);

    return 1;
}

// --- Recent projects ----------------------------------------------------------------------------

static void recent_file_path(char* out, size_t out_size)
{
    char directory[PROJECT_PATH_SIZE];
    platform_user_data_path(directory, sizeof(directory));
    SDL_snprintf(out, out_size, "%srecent_projects.json", directory);
}

int project_recent_list(char out_paths[PROJECT_MAX_RECENT][PROJECT_PATH_SIZE])
{
    char file[PROJECT_PATH_SIZE + 32];
    recent_file_path(file, sizeof(file));

    char* text = SDL_LoadFile(file, NULL);

    if (!text)
    {
        return 0;
    }

    JsonValue* root = json_parse(text);
    SDL_free(text);

    int count = 0;

    for (size_t i = 0; root && i < json_array_count(root) && count < PROJECT_MAX_RECENT; i++)
    {
        const char* path = json_string(json_array_get(root, i), NULL);

        if (path)
        {
            SDL_snprintf(out_paths[count++], PROJECT_PATH_SIZE, "%s", path);
        }
    }

    json_destroy(root);

    return count;
}

static void recent_save(char paths[PROJECT_MAX_RECENT][PROJECT_PATH_SIZE], int count)
{
    Writer w = { 0 };

    write_text(&w, "[\n");

    for (int i = 0; i < count; i++)
    {
        char path[PROJECT_PATH_SIZE];
        clean_string(paths[i], path, sizeof(path));
        write_text(&w, "  \"%s\"%s\n", path, i + 1 < count ? "," : "");
    }

    write_text(&w, "]\n");

    char file[PROJECT_PATH_SIZE + 32];
    recent_file_path(file, sizeof(file));
    SDL_SaveFile(file, w.text, w.length);
    SDL_free(w.text);
}

void project_recent_add(const char* root)
{
    static char paths[PROJECT_MAX_RECENT + 1][PROJECT_PATH_SIZE];
    int count = project_recent_list(paths + 1);

    SDL_snprintf(paths[0], PROJECT_PATH_SIZE, "%s", root);

    int kept = 1;

    for (int i = 1; i <= count && kept < PROJECT_MAX_RECENT; i++)
    {
        if (SDL_strcmp(paths[i], root) != 0)
        {
            if (kept != i)
            {
                SDL_memcpy(paths[kept], paths[i], PROJECT_PATH_SIZE);
            }

            kept++;
        }
    }

    recent_save(paths, kept);
}

void project_recent_remove(const char* root)
{
    static char paths[PROJECT_MAX_RECENT][PROJECT_PATH_SIZE];
    int count = project_recent_list(paths);
    int kept = 0;

    for (int i = 0; i < count; i++)
    {
        if (SDL_strcmp(paths[i], root) != 0)
        {
            if (kept != i)
            {
                SDL_memcpy(paths[kept], paths[i], PROJECT_PATH_SIZE);
            }

            kept++;
        }
    }

    recent_save(paths, kept);
}

void project_default_location(char* out_path, int out_size)
{
    platform_documents_path(out_path, (size_t)out_size);
}
