#include "scene/scene.h"

#include <SDL3/SDL.h>
#include <stdio.h>

#include "json/json.h"
#include "meshedit/edit_mesh.h"
#include "debug.h"

#define DEG_TO_RAD (SDL_PI_F / 180.0f)
#define RAD_TO_DEG (180.0f / SDL_PI_F)

void scene_init(Scene* scene)
{
    SDL_zerop(scene);
    scene->selected = -1;
    scene->sun_yaw = 0.8f;
    scene->sun_pitch = 0.75f;
    scene->sun_intensity = 1.0f;
    scene->use_environment_sun = 1;
}

void scene_object_init(SceneObject* object, const char* name)
{
    SDL_zerop(object);
    SDL_snprintf(object->name, sizeof(object->name), "%s", name);
    object->scale[0] = object->scale[1] = object->scale[2] = 1.0f;
    object->sprite_size[0] = object->sprite_size[1] = 1.0f;
    object->base_color[0] = object->base_color[1] = object->base_color[2] = object->base_color[3] = 1.0f;
    object->roughness = 0.5f;
    object->uv_scale[0] = object->uv_scale[1] = 1.0f;
    object->density = 1.0f;
    object->friction = 0.6f;
    object->audio_volume = 1.0f;
}

void scene_rotation_from_euler(const float degrees[3], float out[9])
{
    float cx = SDL_cosf(degrees[0] * DEG_TO_RAD), sx = SDL_sinf(degrees[0] * DEG_TO_RAD);
    float cy = SDL_cosf(degrees[1] * DEG_TO_RAD), sy = SDL_sinf(degrees[1] * DEG_TO_RAD);
    float cz = SDL_cosf(degrees[2] * DEG_TO_RAD), sz = SDL_sinf(degrees[2] * DEG_TO_RAD);

    // R = Rz * Ry * Rx, the convention of ImGuizmo's RecomposeMatrixFromComponents().
    out[0] = cz * cy;
    out[1] = cz * sy * sx - sz * cx;
    out[2] = cz * sy * cx + sz * sx;
    out[3] = sz * cy;
    out[4] = sz * sy * sx + cz * cx;
    out[5] = sz * sy * cx - cz * sx;
    out[6] = -sy;
    out[7] = cy * sx;
    out[8] = cy * cx;
}

void scene_euler_from_rotation(const float r[9], float out_degrees[3])
{
    out_degrees[0] = RAD_TO_DEG * SDL_atan2f(r[7], r[8]);
    out_degrees[1] = RAD_TO_DEG * SDL_atan2f(-r[6], SDL_sqrtf(r[7] * r[7] + r[8] * r[8]));
    out_degrees[2] = RAD_TO_DEG * SDL_atan2f(r[3], r[0]);
}

Mat4 scene_object_matrix(const SceneObject* object)
{
    float r[9];
    scene_rotation_from_euler(object->rotation, r);

    const float* s = object->scale;
    const float* t = object->position;

    Mat4 m = { {
        r[0] * s[0], r[1] * s[1], r[2] * s[2], t[0],
        r[3] * s[0], r[4] * s[1], r[5] * s[2], t[1],
        r[6] * s[0], r[7] * s[1], r[8] * s[2], t[2],
        0.0f, 0.0f, 0.0f, 1.0f
    } };

    return m;
}

// --- Saving -------------------------------------------------------------------------------------

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

    written = SDL_min(written, (int)sizeof(line) - 1);

    if (w->length + (size_t)written + 1 > w->capacity)
    {
        w->capacity = SDL_max(w->capacity * 2, w->length + (size_t)written + 65536);
        w->text = SDL_realloc(w->text, w->capacity);
    }

    SDL_memcpy(w->text + w->length, line, (size_t)written);
    w->length += (size_t)written;
    w->text[w->length] = '\0';
}

// Text of any length, as is.
static void write_raw(Writer* w, const char* text)
{
    size_t length = SDL_strlen(text);

    if (w->length + length + 1 > w->capacity)
    {
        w->capacity = SDL_max(w->capacity * 2, w->length + length + 65536);
        w->text = SDL_realloc(w->text, w->capacity);
    }

    SDL_memcpy(w->text + w->length, text, length);
    w->length += length;
    w->text[w->length] = '\0';
}

// Names are typed by hand : what JSON would need escaped is dropped.
static void write_string(Writer* w, const char* key, const char* value)
{
    char clean[256];
    size_t n = 0;

    for (const char* c = value; *c && n + 1 < sizeof(clean); c++)
    {
        if (*c != '"' && *c != '\\' && (unsigned char)*c >= 0x20)
        {
            clean[n++] = *c;
        }
    }

    clean[n] = '\0';
    write_text(w, "\"%s\": \"%s\"", key, clean);
}

int scene_save(const Scene* scene, const char* path)
{
    Writer w = { 0 };

    write_text(&w, "{\n  ");
    write_string(&w, "skybox", scene->skybox);
    write_text(&w, ",\n  \"sun\": { \"yaw\": %g, \"pitch\": %g, \"intensity\": %g, \"from_environment\": %d },\n  \"objects\": [\n",
        scene->sun_yaw, scene->sun_pitch, scene->sun_intensity, scene->use_environment_sun);

    for (int i = 0; i < scene->count; i++)
    {
        const SceneObject* o = &scene->objects[i];

        write_text(&w, "    { ");
        write_string(&w, "name", o->name);
        write_text(&w, ", \"position\": [%g, %g, %g], \"rotation\": [%g, %g, %g], \"scale\": [%g, %g, %g]",
            o->position[0], o->position[1], o->position[2], o->rotation[0], o->rotation[1], o->rotation[2], o->scale[0], o->scale[1], o->scale[2]);

        if (o->model[0])
        {
            write_text(&w, ", ");
            write_string(&w, "model", o->model);
        }

        const EditMesh* mesh = o->mesh ? edit_mesh_store_get(o->mesh) : NULL;

        if (mesh)
        {
            char* json = edit_mesh_to_json(mesh);
            write_text(&w, ",\n      \"mesh\": ");
            write_raw(&w, json);
            SDL_free(json);
        }

        if (o->hidden)
        {
            write_text(&w, ", \"hidden\": 1");
        }

        if (o->sprite[0] || o->is_2d)
        {
            write_text(&w, ", ");
            write_string(&w, "sprite", o->sprite);
            write_text(&w, ", \"is_2d\": %d, \"sprite_size\": [%g, %g], \"sprite_region\": [%g, %g, %g, %g], \"tint\": [%g, %g, %g, %g]", o->is_2d, o->sprite_size[0], o->sprite_size[1],
                o->sprite_region[0], o->sprite_region[1], o->sprite_region[2], o->sprite_region[3],
                o->base_color[0], o->base_color[1], o->base_color[2], o->base_color[3]);
        }

        if (o->override_material)
        {
            write_text(&w, ", \"material\": { \"base_color\": [%g, %g, %g, %g], \"emissive\": [%g, %g, %g], \"metallic\": %g, \"roughness\": %g, \"uv_scale\": [%g, %g], ",
                o->base_color[0], o->base_color[1], o->base_color[2], o->base_color[3], o->emissive[0], o->emissive[1], o->emissive[2], o->metallic, o->roughness, o->uv_scale[0], o->uv_scale[1]);
            write_string(&w, "albedo", o->albedo_texture);
            write_text(&w, ", ");
            write_string(&w, "normal", o->normal_texture);
            write_text(&w, ", ");
            write_string(&w, "metallic_roughness", o->metallic_roughness_texture);
            write_text(&w, " }");
        }

        if (o->body_type != SCENE_BODY_NONE || o->is_character)
        {
            write_text(&w, ", \"physics\": { \"body\": %d, \"collider\": %d, \"is_2d\": %d, \"density\": %g, \"friction\": %g, \"restitution\": %g, \"layer\": %d, \"character\": %d }",
                o->body_type, o->collider, o->is_2d, o->density, o->friction, o->restitution, o->layer, o->is_character);
        }

        if (o->audio[0])
        {
            write_text(&w, ", \"audio\": { ");
            write_string(&w, "clip", o->audio);
            write_text(&w, ", \"volume\": %g, \"loop\": %d, \"autoplay\": %d, \"music\": %d, \"spatial\": %d }", o->audio_volume, o->audio_loop, o->audio_autoplay, o->audio_is_music, o->audio_3d);
        }

        write_text(&w, " }%s\n", i + 1 < scene->count ? "," : "");
    }

    write_text(&w, "  ]\n}\n");

    int saved = SDL_SaveFile(path, w.text, w.length);

    if (!saved)
    {
        printf("Could not save scene '%s': %s\n", path, SDL_GetError());
    }

    SDL_free(w.text);

    return saved;
}

// --- Loading ------------------------------------------------------------------------------------

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

static void read_string(const JsonValue* object, const char* key, char* out, size_t out_size)
{
    const char* value = json_string(json_object_get(object, key), NULL);

    if (value)
    {
        SDL_snprintf(out, out_size, "%s", value);
    }
}

int scene_load(Scene* scene, const char* path)
{
    char* text = SDL_LoadFile(path, NULL);

    if (!text)
    {
        return 0;
    }

    JsonValue* root = json_parse(text);
    SDL_free(text);

    if (!root)
    {
        printf("Scene '%s' is not valid JSON.\n", path);
        return 0;
    }

    scene_init(scene);

    read_string(root, "skybox", scene->skybox, sizeof(scene->skybox));

    const JsonValue* sun = json_object_get(root, "sun");
    scene->sun_yaw = (float)json_number(json_object_get(sun, "yaw"), scene->sun_yaw);
    scene->sun_pitch = (float)json_number(json_object_get(sun, "pitch"), scene->sun_pitch);
    scene->sun_intensity = (float)json_number(json_object_get(sun, "intensity"), scene->sun_intensity);
    scene->use_environment_sun = json_int(json_object_get(sun, "from_environment"), scene->use_environment_sun);

    const JsonValue* objects = json_object_get(root, "objects");

    for (size_t i = 0; i < json_array_count(objects) && scene->count < SCENE_MAX_OBJECTS; i++)
    {
        const JsonValue* entry = json_array_get(objects, i);
        SceneObject* o = &scene->objects[scene->count++];

        scene_object_init(o, json_string(json_object_get(entry, "name"), "Object"));
        read_floats(json_object_get(entry, "position"), o->position, 3);
        read_floats(json_object_get(entry, "rotation"), o->rotation, 3);
        read_floats(json_object_get(entry, "scale"), o->scale, 3);
        read_string(entry, "model", o->model, sizeof(o->model));
        read_string(entry, "sprite", o->sprite, sizeof(o->sprite));
        read_floats(json_object_get(entry, "sprite_size"), o->sprite_size, 2);
        read_floats(json_object_get(entry, "sprite_region"), o->sprite_region, 4);
        read_floats(json_object_get(entry, "tint"), o->base_color, 4);
        o->is_2d = json_int(json_object_get(entry, "is_2d"), 0);
        o->hidden = json_int(json_object_get(entry, "hidden"), 0);

        const JsonValue* mesh = json_object_get(entry, "mesh");

        if (mesh)
        {
            EditMesh* loaded = edit_mesh_from_json(mesh);
            o->mesh = loaded ? edit_mesh_store_add(loaded) : 0;
        }

        // Scenes from before projects named a built-in primitive directly.
        const char* primitive = json_string(json_object_get(entry, "primitive"), NULL);

        if (primitive)
        {
            SDL_snprintf(o->model, sizeof(o->model), SCENE_BUILTIN_PREFIX "%s", primitive);
        }

        const JsonValue* material = json_object_get(entry, "material");

        if (material)
        {
            o->override_material = 1;
            read_floats(json_object_get(material, "base_color"), o->base_color, 4);
            read_floats(json_object_get(material, "emissive"), o->emissive, 3);
            o->metallic = (float)json_number(json_object_get(material, "metallic"), o->metallic);
            o->roughness = (float)json_number(json_object_get(material, "roughness"), o->roughness);
            read_floats(json_object_get(material, "uv_scale"), o->uv_scale, 2);
            read_string(material, "albedo", o->albedo_texture, sizeof(o->albedo_texture));
            read_string(material, "normal", o->normal_texture, sizeof(o->normal_texture));
            read_string(material, "metallic_roughness", o->metallic_roughness_texture, sizeof(o->metallic_roughness_texture));
        }

        const JsonValue* physics = json_object_get(entry, "physics");

        if (physics)
        {
            o->body_type = json_int(json_object_get(physics, "body"), SCENE_BODY_NONE);
            o->collider = json_int(json_object_get(physics, "collider"), SCENE_COLLIDER_AUTO);
            o->is_2d = json_int(json_object_get(physics, "is_2d"), o->is_2d);
            o->density = (float)json_number(json_object_get(physics, "density"), o->density);
            o->friction = (float)json_number(json_object_get(physics, "friction"), o->friction);
            o->restitution = (float)json_number(json_object_get(physics, "restitution"), o->restitution);
            o->layer = json_int(json_object_get(physics, "layer"), 0);
            o->is_character = json_int(json_object_get(physics, "character"), 0);
        }

        const JsonValue* audio = json_object_get(entry, "audio");

        if (audio)
        {
            read_string(audio, "clip", o->audio, sizeof(o->audio));
            o->audio_volume = (float)json_number(json_object_get(audio, "volume"), o->audio_volume);
            o->audio_loop = json_int(json_object_get(audio, "loop"), 0);
            o->audio_autoplay = json_int(json_object_get(audio, "autoplay"), 0);
            o->audio_is_music = json_int(json_object_get(audio, "music"), 0);
            o->audio_3d = json_int(json_object_get(audio, "spatial"), 0);
        }
    }

    json_destroy(root);

    DEBUG_LOG("[scene] loaded \"%s\" (%d objects)\n", path, scene->count);

    return 1;
}
