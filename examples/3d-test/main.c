// Simple forward 3D rendering test : the Blender scene in resources/3D-test (FBX), lit by one
// directional light with a 2048x2048 shadow map, seen from a camera orbiting around it.

#include <stdio.h>

#include <SDL3/SDL.h>

#include "platforms/platforms_window.h"
#include "renderer/renderer.h"
#include "model/model.h"
#include "math/math3d.h"
#include "debug.h"
#include "example.h"

#define SHADOW_MAP_SIZE 2048
#define SCENE_PATH "resources/3D-test/Debug.fbx"

#define ORBIT_SPEED 0.35f       // radians per second
#define ORBIT_DISTANCE 1.05f    // in scene radii, from the scene's center
#define ORBIT_HEIGHT 0.5f       // in scene radii, above the scene's center
#define CAMERA_FOV_DEGREES 60.0f

static Mesh* scene_mesh;
static ShadowMap* shadow_map;
static Vec3 scene_center;
static float scene_radius;
static Uint64 start_ticks;

// The directional light's view-projection : an orthographic box around the scene's bounding sphere,
// looking along the light direction, so every shadow caster in the scene fits in the shadow map.
static Mat4 light_view_projection(Vec3 light_direction)
{
    Vec3 eye = vec3_sub(scene_center, vec3_scale(light_direction, scene_radius * 2.0f));

    // Any "up" not parallel to the light direction works for a directional light.
    Vec3 up = SDL_fabsf(light_direction.y) > 0.99f ? vec3(0.0f, 0.0f, 1.0f) : vec3(0.0f, 1.0f, 0.0f);

    Mat4 view = mat4_look_at(eye, scene_center, up);
    Mat4 projection = mat4_orthographic(-scene_radius, scene_radius, -scene_radius, scene_radius,
        scene_radius, scene_radius * 3.0f);

    return mat4_multiply(projection, view);
}

static Vec3 light_direction;
static Mat4 light_matrix;

// Before begin_frame() : the shadow map, from the light's point of view (its own synchronous GPU pass).
static void update(void)
{
    // Light : fixed, coming from above at an angle, so shadows stretch across the scene.
    light_direction = vec3_normalize(vec3(-0.45f, -1.0f, -0.35f));
    light_matrix = light_view_projection(light_direction);

    Mat4 model = mat4_identity(); // the FBX loader already baked the scene into world space

    if (begin_shadow_pass(shadow_map, light_matrix.m))
    {
        draw_mesh(scene_mesh, model.m);
        end_shadow_pass();
    }
}

static void render(void)
{
    int window_width = 0;
    int window_height = 0;
    get_window_size(&window_width, &window_height);

    if (window_width <= 0 || window_height <= 0)
    {
        return;
    }

    float time = (float)(SDL_GetTicks() - start_ticks) / 1000.0f;

    // Camera : orbiting the scene's center.
    float angle = time * ORBIT_SPEED;
    Vec3 eye = vec3_add(scene_center, vec3(SDL_cosf(angle) * scene_radius * ORBIT_DISTANCE,
        scene_radius * ORBIT_HEIGHT, SDL_sinf(angle) * scene_radius * ORBIT_DISTANCE));

    Mat4 view = mat4_look_at(eye, scene_center, vec3(0.0f, 1.0f, 0.0f));
    Mat4 projection = mat4_perspective(CAMERA_FOV_DEGREES * SDL_PI_F / 180.0f,
        (float)window_width / (float)window_height, scene_radius * 0.02f, scene_radius * 6.0f);
    Mat4 view_projection = mat4_multiply(projection, view);

    Mat4 model = mat4_identity(); // the FBX loader already baked the scene into world space

    // The scene, forward-shaded, from the orbiting camera.
    SceneLighting lighting;
    SDL_memcpy(lighting.view_projection, view_projection.m, sizeof(lighting.view_projection));
    SDL_memcpy(lighting.light_view_projection, light_matrix.m, sizeof(lighting.light_view_projection));
    lighting.light_direction[0] = light_direction.x;
    lighting.light_direction[1] = light_direction.y;
    lighting.light_direction[2] = light_direction.z;
    // Light + ambient stay under 1 together, so white surfaces don't saturate (no tone mapping).
    lighting.light_color[0] = 0.80f;
    lighting.light_color[1] = 0.77f;
    lighting.light_color[2] = 0.70f;
    // A flat bluish ambient.
    for (int i = 0; i < 3; i++)
    {
        lighting.ambient_color[i] = i == 2 ? 0.22f : i == 1 ? 0.18f : 0.16f;
        lighting.ground_color[i] = lighting.ambient_color[i];
    }

    // Biases scale with the shadow map's world-space texel size, so they hold for any scene size.
    // The normal offset does most of the work against acne (a 3x3 PCF kernel reaches 1 texel
    // around the lookup, hence well over 1 texel of offset) ; the depth bias only covers the rest.
    float texel_world_size = (2.0f * scene_radius) / (float)SHADOW_MAP_SIZE;
    lighting.shadow_depth_bias = 0.001f;
    lighting.shadow_normal_offset = texel_world_size * 3.0f;

    begin_scene(&lighting, shadow_map);
    draw_mesh(scene_mesh, model.m);
    end_scene();
}

static int init(void)
{
    ModelData model;

    if (!model_load(SCENE_PATH, &model))
    {
        printf("Failed to load the test scene.\n");
        return 0;
    }

    scene_mesh = create_mesh(model.vertices, model.vertex_count, model.indices, model.index_count);
    shadow_map = create_shadow_map(SHADOW_MAP_SIZE);

    scene_center = vec3((model.bounds_min[0] + model.bounds_max[0]) * 0.5f,
        (model.bounds_min[1] + model.bounds_max[1]) * 0.5f,
        (model.bounds_min[2] + model.bounds_max[2]) * 0.5f);
    scene_radius = vec3_length(vec3(model.bounds_max[0] - model.bounds_min[0],
        model.bounds_max[1] - model.bounds_min[1],
        model.bounds_max[2] - model.bounds_min[2])) * 0.5f;

    model_free(&model);

    if (!scene_mesh || !shadow_map || scene_radius <= 0.0f)
    {
        printf("Failed to create the scene's GPU resources.\n");
        destroy_shadow_map(shadow_map);
        destroy_mesh(scene_mesh);
        return 0;
    }

    start_ticks = SDL_GetTicks();

    DEBUG_LOG("[3d-test] scene radius %.2f\n", scene_radius);

    return 1;
}

static void shutdown_example(void)
{
    destroy_shadow_map(shadow_map);
    destroy_mesh(scene_mesh);
}

const Example example_3d_test = {
    "3d-test", "3D Test",
    "Simple forward rendering : one light, a 2048x2048 shadow map, an orbiting camera.",
    init, update, render, shutdown_example
};
