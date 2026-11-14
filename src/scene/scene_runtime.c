// Playing a scene : a fixed-step physics loop with interpolation for drawing, the player's input
// driving the character controller, a third person (3D) or side (2D) camera, and audio sources.

#include "scene/scene_runtime.h"

#include <SDL3/SDL.h>

#include "assets/asset_database.h"
#include "audio/audio.h"
#include "physics/physics.h"
#include "debug.h"

#define MAX_STEPS_PER_FRAME 8 // past this, the simulation slows down rather than spiraling
#define CAMERA_DISTANCE 5.0f
#define CAMERA_HEIGHT 0.6f
#define ORTHO_HALF_HEIGHT 6.0f

typedef struct
{
    int object;
    AudioVoice* voice;
} ActiveSound;

static Scene* scene;
static const ProjectSettings* settings;
static PhysicsWorld* physics;
static float accumulator;
static ActiveSound sounds[SCENE_MAX_OBJECTS];
static int sound_count;
static float camera_yaw;
static float camera_pitch;
static Vec3 last_eye;

int scene_runtime_start(Scene* new_scene, const ProjectSettings* new_settings)
{
    scene = new_scene;
    settings = new_settings;
    accumulator = 0.0f;
    sound_count = 0;
    camera_yaw = 0.0f;
    camera_pitch = 0.25f;

    physics = physics_create(scene, settings);

    audio_set_volumes(settings->master_volume, settings->music_volume, settings->sound_volume);

    for (int i = 0; i < scene->count; i++)
    {
        const SceneObject* object = &scene->objects[i];

        if (object->audio[0] && object->audio_autoplay)
        {
            AudioClip* clip = asset_get_audio(object->audio);

            if (clip)
            {
                sounds[sound_count].object = i;
                sounds[sound_count].voice = audio_play(clip, object->audio_is_music ? AUDIO_BUS_MUSIC : AUDIO_BUS_SOUND, object->audio_volume, object->audio_loop);
                sound_count++;
            }
        }
    }

    DEBUG_LOG("[runtime] started\n");

    return physics != NULL;
}

void scene_runtime_stop(void)
{
    audio_stop_all();
    sound_count = 0;

    physics_destroy(physics);
    physics = NULL;
    scene = NULL;

    DEBUG_LOG("[runtime] stopped\n");
}

// The direction the player asks to move in, relative to where the camera looks.
static void character_input(const RuntimeInput* input, CharacterInput* out, int is_2d)
{
    SDL_zerop(out);
    out->jump = input->jump;
    out->sprint = input->sprint;

    if (is_2d)
    {
        out->move[0] = SDL_clamp(input->move_right, -1.0f, 1.0f);
        return;
    }

    float forward_x = -SDL_sinf(camera_yaw);
    float forward_z = -SDL_cosf(camera_yaw);
    float right_x = SDL_cosf(camera_yaw);
    float right_z = -SDL_sinf(camera_yaw);

    float x = forward_x * input->move_forward + right_x * input->move_right;
    float z = forward_z * input->move_forward + right_z * input->move_right;
    float length = SDL_sqrtf(x * x + z * z);

    if (length > 1.0f)
    {
        x /= length;
        z /= length;
    }

    out->move[0] = x;
    out->move[2] = z;
}

void scene_runtime_update(const RuntimeInput* input, float delta_time)
{
    if (!physics)
    {
        return;
    }

    camera_yaw -= input->look_x;
    camera_pitch = SDL_clamp(camera_pitch + input->look_y, -1.2f, 1.2f);

    int character = physics_character_object(physics);
    int is_2d = character >= 0 && scene->objects[character].is_2d;

    CharacterInput movement;
    character_input(input, &movement, is_2d);

    float step = 1.0f / (float)settings->physics_rate;
    accumulator += SDL_min(delta_time, step * MAX_STEPS_PER_FRAME);

    while (accumulator >= step)
    {
        physics_step(physics, &movement, step);
        accumulator -= step;
    }

    physics_update_scene(physics, accumulator / step);

    // 3D sounds, relative to the listener at the camera.
    for (int i = 0; i < sound_count; i++)
    {
        const SceneObject* object = &scene->objects[sounds[i].object];

        if (object->audio_3d)
        {
            Vec3 relative = vec3_sub(vec3(object->position[0], object->position[1], object->position[2]), last_eye);
            float c = SDL_cosf(camera_yaw);
            float s = SDL_sinf(camera_yaw);
            audio_set_voice_position(sounds[i].voice, relative.x * c - relative.z * s, relative.y, relative.x * s + relative.z * c);
        }
    }
}

int scene_runtime_camera(RuntimeCamera* out_camera)
{
    if (!physics)
    {
        return 0;
    }

    int character = physics_character_object(physics);

    if (character < 0)
    {
        return 0;
    }

    const SceneObject* object = &scene->objects[character];
    Vec3 position = vec3(object->position[0], object->position[1], object->position[2]);

    SDL_zerop(out_camera);

    if (object->is_2d)
    {
        out_camera->orthographic = 1;
        out_camera->ortho_half_height = ORTHO_HALF_HEIGHT;
        out_camera->eye = vec3(position.x, position.y, 50.0f);
        out_camera->target = vec3(position.x, position.y, 0.0f);
    }
    else
    {
        Vec3 pivot = vec3_add(position, vec3(0.0f, CAMERA_HEIGHT, 0.0f));
        Vec3 offset = vec3(SDL_cosf(camera_pitch) * SDL_sinf(camera_yaw), SDL_sinf(camera_pitch), SDL_cosf(camera_pitch) * SDL_cosf(camera_yaw));
        out_camera->eye = vec3_add(pivot, vec3_scale(offset, CAMERA_DISTANCE));
        out_camera->target = pivot;
    }

    last_eye = out_camera->eye;

    return 1;
}

int scene_runtime_character_grounded(void)
{
    return physics ? physics_character_grounded(physics) : 0;
}
