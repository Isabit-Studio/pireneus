// The photorealistic renderer on the Blender test scene, under a procedural sky.

#include <stdio.h>

#include <SDL3/SDL.h>

#include "platforms/platforms_window.h"
#include "renderer/renderer.h"
#include "renderer/photoreal/photoreal.h"
#include "model/model.h"
#include "math/math3d.h"
#include "debug.h"
#include "example.h"

#define SCENE_PATH "resources/3D-test/Debug.fbx"
#define ORBIT_SPEED 0.2f
#define ORBIT_DISTANCE 1.05f
#define ORBIT_HEIGHT 0.45f

static Mesh* scene_mesh;
static ModelData model;
static Environment environment;
static Vec3 scene_center;
static float scene_radius;
static Uint64 start_ticks;

static void render(void)
{
    int width = 0;
    int height = 0;
    get_window_size(&width, &height);

    if (width <= 0 || height <= 0)
    {
        return;
    }

    static float orbit_time;
    Uint64 now = SDL_GetTicks();

    orbit_time += (float)(now - start_ticks) / 1000.0f;

    start_ticks = now;

    float angle = orbit_time * ORBIT_SPEED + 0.9f;

    PhotorealCamera camera;
    camera.eye = vec3_add(scene_center, vec3(SDL_cosf(angle) * scene_radius * ORBIT_DISTANCE, scene_radius * ORBIT_HEIGHT, SDL_sinf(angle) * scene_radius * ORBIT_DISTANCE));
    camera.near_plane = scene_radius * 0.01f;
    camera.far_plane = scene_radius * 40.0f;
    camera.view = mat4_look_at(camera.eye, scene_center, vec3(0.0f, 1.0f, 0.0f));
    camera.projection = mat4_perspective(55.0f * SDL_PI_F / 180.0f, (float)width / (float)height, camera.near_plane, camera.far_plane);

    photoreal_begin(&camera);

    Mat4 identity = mat4_identity();
    float bounds[6] = { model.bounds_min[0], model.bounds_min[1], model.bounds_min[2], model.bounds_max[0], model.bounds_max[1], model.bounds_max[2] };

    for (int i = 0; i < model.submesh_count; i++)
    {
        const ModelMaterial* source = &model.materials[model.submeshes[i].material];

        PbrMaterial material;
        photoreal_default_material(&material);
        material.metallic = source->metallic;
        material.roughness = source->roughness;
        SDL_memcpy(material.emissive, source->emissive, sizeof(material.emissive));

        photoreal_add(scene_mesh, model.submeshes[i].first_index, model.submeshes[i].index_count, identity.m, bounds, &material);
    }

    photoreal_render(width, height);
    photoreal_present();
}

static int init(void)
{
    if (!model_load(SCENE_PATH, &model))
    {
        printf("Failed to load the test scene.\n");
        return 0;
    }

    scene_mesh = create_mesh(model.vertices, model.vertex_count, model.indices, model.index_count);

    scene_center = vec3((model.bounds_min[0] + model.bounds_max[0]) * 0.5f,
        (model.bounds_min[1] + model.bounds_max[1]) * 0.5f,
        (model.bounds_min[2] + model.bounds_max[2]) * 0.5f);
    scene_radius = vec3_length(vec3(model.bounds_max[0] - model.bounds_min[0],
        model.bounds_max[1] - model.bounds_min[1],
        model.bounds_max[2] - model.bounds_min[2])) * 0.5f;

    if (!scene_mesh || !photoreal_create())
    {
        destroy_mesh(scene_mesh);
        model_free(&model);
        return 0;
    }

    const float sun[3] = { -0.45f, 0.62f, -0.35f };

    if (!environment_create_procedural(sun, &environment))
    {
        photoreal_destroy();
        destroy_mesh(scene_mesh);
        model_free(&model);
        return 0;
    }

    photoreal_set_environment(&environment, environment.sun_direction, environment.sun_color);

    start_ticks = SDL_GetTicks();

    return 1;
}

static void shutdown_example(void)
{
    photoreal_destroy();
    environment_destroy(&environment);
    destroy_mesh(scene_mesh);
    model_free(&model);
}

const Example example_photoreal = {
    "photoreal", "Photorealistic Rendering",
    "Visibility buffer, physically based shading, image-based lighting, soft shadows, ambient occlusion.",
    init, NULL, render, shutdown_example
};
