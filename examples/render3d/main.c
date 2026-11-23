// The 3D renderer (renderer/render3d) on the Blender test scene, under a procedural sky, with an
// orbiting camera : one example per pipeline, Forward+ and Deferred+. Tab switches between them, to
// compare the two on the same scene ; N switches to night, where 128 colored point lights circle the
// scene, and a spot light and a point light cast shadows. Deferred+ adds realtime GI, where the GPU
// traces rays.

#include <stdio.h>

#include <SDL3/SDL.h>

#include "platforms/platforms_input.h"
#include "platforms/platforms_window.h"
#include "renderer/renderer.h"
#include "renderer/render3d/render3d.h"
#include "model/model.h"
#include "math/math3d.h"
#include "debug.h"
#include "example.h"

#define SCENE_PATH "resources/3D-test/Debug.fbx"
#define ORBIT_SPEED 0.2f
#define ORBIT_DISTANCE 1.05f
#define ORBIT_HEIGHT 0.45f
#define LIGHT_COUNT 128

static Mesh* scene_mesh;
static ModelData model;
static Environment environment;
static Vec3 scene_center;
static float scene_radius;
static Uint64 last_ticks;
static float orbit_time;
static int tab_was_down;
static int n_was_down;
static int night;

// Each pipeline's anti-aliasing : MSAA for Forward+, sharpened TAA for Deferred+ ; night or day.
static void apply_settings(Render3DPipeline pipeline)
{
    Render3DQuality quality;
    Render3DLook look;
    render3d_default_quality(&quality);
    render3d_default_look(&look);

    int deferred = pipeline == RENDER3D_DEFERRED_PLUS;
    quality.gi_enabled = deferred && render3d_realtime_gi_supported();
    quality.taa_enabled = deferred;
    quality.sharpness = deferred ? 0.4f : 0.0f;
    quality.fxaa_enabled = 0;

    // Day : drifting clouds. Night : a light fog, which the lights scatter.
    look.cloud_coverage = night ? 0.0f : 0.5f;

    if (night)
    {
        look.sky_intensity = 0.02f;
        look.exposure_ev = -5.0f;
        look.fog_density = 0.08f / scene_radius;
        look.fog_height_falloff = 0.5f / scene_radius;
        look.fog_base_height = scene_center.y - scene_radius * 0.3f;
        look.volumetric_distance = scene_radius * 4.0f;
    }

    render3d_set_pipeline(pipeline);
    render3d_set_settings(&quality, &look);

    float sun_color[3];

    for (int c = 0; c < 3; c++)
    {
        sun_color[c] = environment.sun_color[c] * (night ? 0.0f : 1.0f);
    }

    render3d_set_environment(&environment, environment.sun_direction, sun_color);
}

static void update(void)
{
    // Tab : the other pipeline.
    int tab_down = input_is_key_down(SCANCODE_TAB);

    if (tab_down && !tab_was_down)
    {
        Render3DPipeline next = render3d_pipeline() == RENDER3D_FORWARD_PLUS ? RENDER3D_DEFERRED_PLUS : RENDER3D_FORWARD_PLUS;
        apply_settings(next);
        printf("%s\n", next == RENDER3D_FORWARD_PLUS ? "Forward+" : "Deferred+");
    }

    tab_was_down = tab_down;

    // N : night, the scene lit by its lights alone.
    int n_down = input_is_key_down(SCANCODE_N);

    if (n_down && !n_was_down)
    {
        night = !night;
        apply_settings(render3d_pipeline());
    }

    n_was_down = n_down;
}

static void add_lights(float time)
{
    for (int i = 0; i < LIGHT_COUNT; i++)
    {
        float t = (float)i / (float)LIGHT_COUNT;
        float angle = t * 2.0f * SDL_PI_F + time * (0.3f + 0.2f * (float)(i % 3));
        float radius = scene_radius * (0.25f + 0.45f * (float)((i * 37) % 11) / 10.0f);

        Render3DLight light;
        render3d_default_light(&light);
        light.position[0] = scene_center.x + SDL_cosf(angle) * radius;
        light.position[1] = scene_center.y - scene_radius * 0.15f + scene_radius * 0.05f * (float)(i % 4);
        light.position[2] = scene_center.z + SDL_sinf(angle) * radius;

        // Hues around the color wheel.
        light.color[0] = 0.5f + 0.5f * SDL_cosf(t * 6.2832f);
        light.color[1] = 0.5f + 0.5f * SDL_cosf(t * 6.2832f - 2.094f);
        light.color[2] = 0.5f + 0.5f * SDL_cosf(t * 6.2832f + 2.094f);
        light.intensity = scene_radius * scene_radius * 0.02f;
        light.range = scene_radius * 0.22f;
        light.id = (uint32_t)i;
        render3d_add_light(&light);
    }

    // A spot from above, swinging, and a point light among the objects : both cast shadows.
    Render3DLight spot;
    render3d_default_light(&spot);
    spot.type = RENDER3D_LIGHT_SPOT;
    spot.position[0] = scene_center.x + SDL_cosf(time * 0.5f) * scene_radius * 0.3f;
    spot.position[1] = scene_center.y + scene_radius * 0.6f;
    spot.position[2] = scene_center.z;
    Vec3 aim = vec3_normalize(vec3_sub(scene_center, vec3(spot.position[0], spot.position[1], spot.position[2])));
    spot.direction[0] = aim.x;
    spot.direction[1] = aim.y;
    spot.direction[2] = aim.z;
    spot.color[0] = 1.0f;
    spot.color[1] = 0.85f;
    spot.color[2] = 0.65f;
    spot.intensity = scene_radius * scene_radius * 1.5f;
    spot.range = scene_radius * 2.0f;
    spot.spot_angle = 50.0f;
    spot.cast_shadows = 1;
    spot.id = LIGHT_COUNT;
    render3d_add_light(&spot);

    Render3DLight point;
    render3d_default_light(&point);
    point.position[0] = scene_center.x + scene_radius * 0.05f;
    point.position[1] = scene_center.y + scene_radius * 0.05f;
    point.position[2] = scene_center.z + scene_radius * 0.12f;
    point.color[0] = 0.5f;
    point.color[1] = 0.7f;
    point.color[2] = 1.0f;
    point.intensity = scene_radius * scene_radius * 0.1f;
    point.range = scene_radius;
    point.cast_shadows = 1;
    point.id = LIGHT_COUNT + 1;
    render3d_add_light(&point);
}

static void render(void)
{
    int width = 0;
    int height = 0;
    get_window_size(&width, &height);

    if (width <= 0 || height <= 0)
    {
        return;
    }

    Uint64 now = SDL_GetTicks();
    orbit_time += (float)(now - last_ticks) / 1000.0f;
    last_ticks = now;

    float angle = orbit_time * ORBIT_SPEED + 0.9f;

    Render3DCamera camera;
    camera.eye = vec3_add(scene_center, vec3(SDL_cosf(angle) * scene_radius * ORBIT_DISTANCE, scene_radius * ORBIT_HEIGHT, SDL_sinf(angle) * scene_radius * ORBIT_DISTANCE));
    camera.near_plane = scene_radius * 0.01f;
    camera.far_plane = scene_radius * 40.0f;
    camera.view = mat4_look_at(camera.eye, scene_center, vec3(0.0f, 1.0f, 0.0f));
    camera.projection = mat4_perspective(55.0f * SDL_PI_F / 180.0f, (float)width / (float)height, camera.near_plane, camera.far_plane);

    render3d_begin(&camera);

    Mat4 identity = mat4_identity();
    float bounds[6] = { model.bounds_min[0], model.bounds_min[1], model.bounds_min[2], model.bounds_max[0], model.bounds_max[1], model.bounds_max[2] };

    for (int i = 0; i < model.submesh_count; i++)
    {
        const ModelMaterial* source = &model.materials[model.submeshes[i].material];

        PbrMaterial material;
        render3d_default_material(&material);
        material.metallic = source->metallic;
        material.roughness = source->roughness;
        SDL_memcpy(material.emissive, source->emissive, sizeof(material.emissive));

        render3d_add(scene_mesh, model.submeshes[i].first_index, model.submeshes[i].index_count, identity.m, bounds, &material, NULL);
    }

    if (night)
    {
        add_lights(orbit_time);
    }

    render3d_render(width, height);
    render3d_present();
}

static int init_with(Render3DPipeline pipeline)
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

    if (!scene_mesh || !render3d_create())
    {
        destroy_mesh(scene_mesh);
        model_free(&model);
        return 0;
    }

    const float sun[3] = { -0.45f, 0.62f, -0.35f };

    if (!environment_create_procedural(sun, &environment))
    {
        render3d_destroy();
        destroy_mesh(scene_mesh);
        model_free(&model);
        return 0;
    }

    last_ticks = SDL_GetTicks();
    orbit_time = 0.0f;
    tab_was_down = 0;
    n_was_down = 0;
    night = 0;
    apply_settings(pipeline);

    return 1;
}

static int init_forward_plus(void)
{
    return init_with(RENDER3D_FORWARD_PLUS);
}

static int init_deferred_plus(void)
{
    return init_with(RENDER3D_DEFERRED_PLUS);
}

static void shutdown_example(void)
{
    render3d_destroy();
    environment_destroy(&environment);
    destroy_mesh(scene_mesh);
    model_free(&model);
}

const Example example_forward_plus = {
    "forward-plus", "Forward+ Rendering",
    "Depth prepass, then forward shading : clustered lights, soft shadows, MSAA. Tab : Deferred+, N : night.",
    init_forward_plus, update, render, shutdown_example
};

const Example example_deferred_plus = {
    "deferred-plus", "Deferred+ Rendering",
    "Visibility buffer, one shading pass per pixel : clustered lights, reflections, sharp TAA. Tab : Forward+, N : night.",
    init_deferred_plus, update, render, shutdown_example
};
