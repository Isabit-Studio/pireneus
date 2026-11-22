// The photorealistic renderer, on the bindless layer of gpu.h (shaders in photoreal.slang) : cascaded
// shadows, a visibility buffer, GTAO, one full-screen shading pass, bloom and tone mapping.

#include "renderer/photoreal/photoreal.h"

#include <SDL3/SDL.h>
#include <stdio.h>

#include "renderer/gpu.h"
#include "renderer/photoreal/half_float.h"
#include "debug.h"

#define MAX_DRAWS 4095 // the visibility buffer's ID holds 12 bits of draw (all ones means "empty")
#define MAX_CASCADES 4
#define BLOOM_LEVELS 6
#define BRDF_LUT_SIZE 128
#define NO_TEXTURE 0xFFFFFFFFu
#define PI_F 3.14159265f

// Mirrors Frame in photoreal.slang, field for field.
typedef struct
{
    float view_projection[16];
    float inverse_view_projection[16];
    float view[16];
    float inverse_projection[16];
    float camera_position[4];
    float sun_direction[4];
    float sun_color[4];
    float sh[9][4];
    float environment[4];
    uint32_t environment_textures[4];
    float fog_inscatter[4];
    float fog_sun[4];
    float fog_params[4];
    float shadow_view_projection[MAX_CASCADES][16];
    float cascade_far[4];
    uint32_t shadow_textures[4];
    float shadow_texel_world[4];
    float shadow_depth_range[4];
    float shadow_params[4];
    float resolution[4];
} PhotorealFrame;

// Mirrors Instance in photoreal.slang.
typedef struct
{
    float model[12];
    float normal_matrix[12];
    uint32_t geometry[4];
    float base_color[4];
    float emissive[4];
    float surface[4];
    uint32_t textures[4];
    uint32_t textures2[4];
    float uv_transform[4];
} PhotorealInstance;

SDL_COMPILE_TIME_ASSERT(instance_size, sizeof(PhotorealInstance) == 208);

// What every pass pushes (see Push in photoreal.slang).
typedef struct
{
    uint32_t ids[4];
    uint32_t inputs[4];
    float params[4];
    float params2[4];
} PassConstants;

typedef struct
{
    Mesh* mesh;
    int first_index;
    int index_count;
    float center[3];  // world-space bounding sphere
    float radius;     // < 0 : no bounds, never culled
} Draw;

static int created;

static GpuPipeline* shadow_pipeline;
static GpuPipeline* shadow_masked_pipeline; // alpha-masked casters
static GpuPipeline* visibility_pipeline;
static GpuPipeline* ao_pipeline;
static GpuPipeline* ao_blur_pipeline;
static GpuPipeline* shade_pipeline;
static GpuPipeline* bloom_down_pipeline;
static GpuPipeline* bloom_up_pipeline;
static GpuPipeline* tonemap_pipeline;
static GpuPipeline* fxaa_pipeline;

static Texture* brdf_lut;
static Texture* shadow_maps[MAX_CASCADES];
static int shadow_map_size;

static Texture* visibility_target;
static Texture* depth_target;
static Texture* hdr_target;
static Texture* ao_target;
static Texture* ao_blur_target;
static Texture* bloom_targets[BLOOM_LEVELS];
static Texture* ldr_target;
static Texture* final_target;
static int target_width;   // internal resolution
static int target_height;
static int final_width;    // window resolution
static int final_height;
static int bloom_level_count;

static PhotorealQuality quality;
static PhotorealLook look;
static const Environment* environment;
static float sun_direction[3];
static float sun_color[3];

static PhotorealCamera camera;
static PhotorealInstance instances[MAX_DRAWS];
static Draw draws[MAX_DRAWS];
static int draw_count;

void photoreal_default_quality(PhotorealQuality* out_quality)
{
    out_quality->render_scale = 1.0f;
    out_quality->shadow_resolution = 2048;
    out_quality->shadow_cascades = 3;
    out_quality->shadow_samples = 24;
    out_quality->shadow_distance = 120.0f;
    out_quality->ao_enabled = 1;
    out_quality->ao_slices = 3;
    out_quality->ao_steps = 5;
    out_quality->bloom_enabled = 1;
    out_quality->fxaa_enabled = 1;
}

void photoreal_default_look(PhotorealLook* out_look)
{
    out_look->exposure_ev = 0.0f;
    out_look->tonemapper = PHOTOREAL_TONEMAP_ACES;
    out_look->bloom_strength = 0.04f;
    out_look->contrast = 0.15f;
    out_look->saturation = 1.05f;
    out_look->vignette = 0.25f;
    out_look->ao_radius = 0.8f;
    out_look->ao_power = 1.4f;
    out_look->sky_intensity = 1.0f;
    out_look->bounce_strength = 0.12f;
    out_look->sun_softness = 3.0f;
    out_look->fog_density = 0.0f;
    out_look->fog_height_falloff = 0.08f;
    out_look->fog_base_height = 0.0f;
    out_look->fog_color[0] = 0.55f;
    out_look->fog_color[1] = 0.65f;
    out_look->fog_color[2] = 0.8f;
    out_look->fog_sun_color[0] = 1.0f;
    out_look->fog_sun_color[1] = 0.85f;
    out_look->fog_sun_color[2] = 0.6f;
    out_look->fog_sun_power = 8.0f;
}

void photoreal_default_material(PbrMaterial* out_material)
{
    SDL_zerop(out_material);
    out_material->base_color[0] = out_material->base_color[1] = out_material->base_color[2] = out_material->base_color[3] = 1.0f;
    out_material->roughness = 0.5f;
    out_material->normal_scale = 1.0f;
    out_material->occlusion_strength = 1.0f;
    out_material->uv_scale[0] = out_material->uv_scale[1] = 1.0f;
}

// The split-sum BRDF lookup table (Karis) : scale and bias applied to F0, by (N.V, roughness).
static Texture* create_brdf_lut(void)
{
    uint16_t* pixels = SDL_malloc(sizeof(uint16_t) * 2 * BRDF_LUT_SIZE * BRDF_LUT_SIZE);
    const uint32_t sample_count = 256;

    for (int y = 0; y < BRDF_LUT_SIZE; y++)
    {
        float roughness = ((float)y + 0.5f) / (float)BRDF_LUT_SIZE;
        float a = roughness * roughness;
        float a2 = a * a;

        for (int x = 0; x < BRDF_LUT_SIZE; x++)
        {
            float n_dot_v = ((float)x + 0.5f) / (float)BRDF_LUT_SIZE;
            float vx = SDL_sqrtf(1.0f - n_dot_v * n_dot_v);
            float vz = n_dot_v;
            float scale = 0.0f;
            float bias = 0.0f;

            for (uint32_t i = 0; i < sample_count; i++)
            {
                uint32_t bits = i;
                bits = (bits << 16u) | (bits >> 16u);
                bits = ((bits & 0x55555555u) << 1u) | ((bits & 0xAAAAAAAAu) >> 1u);
                bits = ((bits & 0x33333333u) << 2u) | ((bits & 0xCCCCCCCCu) >> 2u);
                bits = ((bits & 0x0F0F0F0Fu) << 4u) | ((bits & 0xF0F0F0F0u) >> 4u);
                bits = ((bits & 0x00FF00FFu) << 8u) | ((bits & 0xFF00FF00u) >> 8u);

                float xi0 = (float)i / (float)sample_count;
                float xi1 = (float)bits * 2.3283064365386963e-10f;

                float phi = 2.0f * PI_F * xi0;
                float cos_theta = SDL_sqrtf((1.0f - xi1) / (1.0f + (a2 - 1.0f) * xi1));
                float sin_theta = SDL_sqrtf(SDL_max(0.0f, 1.0f - cos_theta * cos_theta));
                float hx = sin_theta * SDL_cosf(phi);
                float hz = cos_theta;

                float v_dot_h = vx * hx + vz * hz;
                float lz = 2.0f * v_dot_h * hz - vz;
                float n_dot_l = lz;
                float n_dot_h = hz;

                if (n_dot_l > 0.0f && v_dot_h > 0.0f)
                {
                    // Smith GGX, height-correlated, for image-based lighting.
                    float gv = n_dot_l * SDL_sqrtf(n_dot_v * n_dot_v * (1.0f - a2) + a2);
                    float gl = n_dot_v * SDL_sqrtf(n_dot_l * n_dot_l * (1.0f - a2) + a2);
                    float visibility = 0.5f / (gv + gl);
                    float g_vis = 4.0f * visibility * n_dot_l * v_dot_h / n_dot_h;
                    float fc = SDL_powf(1.0f - v_dot_h, 5.0f);

                    scale += (1.0f - fc) * g_vis;
                    bias += fc * g_vis;
                }
            }

            pixels[(y * BRDF_LUT_SIZE + x) * 2 + 0] = float_to_half(scale / (float)sample_count);
            pixels[(y * BRDF_LUT_SIZE + x) * 2 + 1] = float_to_half(bias / (float)sample_count);
        }
    }

    GpuTextureDesc desc;
    SDL_zero(desc);
    desc.width = BRDF_LUT_SIZE;
    desc.height = BRDF_LUT_SIZE;
    desc.mip_count = 1;
    desc.format = GPU_FORMAT_RG16F;
    desc.usage = GPU_USAGE_SAMPLED;
    desc.mips[0] = pixels;

    Texture* texture = gpu_create_texture(&desc);
    SDL_free(pixels);

    return texture;
}

static GpuPipeline* screen_pipeline(const char* fragment_shader, GpuFormat format, GpuBlend blend)
{
    GpuPipelineDesc desc;
    SDL_zero(desc);
    desc.vertex_shader = "pr_screen_vs";
    desc.fragment_shader = fragment_shader;
    desc.color_formats[0] = format;
    desc.color_count = 1;
    desc.blend = blend;
    return gpu_create_pipeline(&desc);
}

int photoreal_create(void)
{
    if (!gpu_supported())
    {
        printf("This GPU can't run the photorealistic renderer.\n");
        return 0;
    }

    GpuPipelineDesc desc;
    SDL_zero(desc);
    desc.vertex_shader = "pr_shadow_vs";
    desc.depth_format = GPU_FORMAT_D32;
    desc.depth_test = 1;
    desc.depth_write = 1;
    shadow_pipeline = gpu_create_pipeline(&desc);

    desc.vertex_shader = "pr_shadow_masked_vs";
    desc.fragment_shader = "pr_shadow_masked_ps";
    shadow_masked_pipeline = gpu_create_pipeline(&desc);

    SDL_zero(desc);
    desc.vertex_shader = "pr_visibility_vs";
    desc.fragment_shader = "pr_visibility_ps";
    desc.color_formats[0] = GPU_FORMAT_RGBA8;
    desc.color_count = 1;
    desc.depth_format = GPU_FORMAT_D32;
    desc.depth_test = 1;
    desc.depth_write = 1;
    visibility_pipeline = gpu_create_pipeline(&desc);

    ao_pipeline = screen_pipeline("pr_ao_ps", GPU_FORMAT_R8, GPU_BLEND_NONE);
    ao_blur_pipeline = screen_pipeline("pr_ao_blur_ps", GPU_FORMAT_R8, GPU_BLEND_NONE);
    shade_pipeline = screen_pipeline("pr_shade_ps", GPU_FORMAT_RGBA16F, GPU_BLEND_NONE);
    bloom_down_pipeline = screen_pipeline("pr_bloom_down_ps", GPU_FORMAT_RGBA16F, GPU_BLEND_NONE);
    bloom_up_pipeline = screen_pipeline("pr_bloom_up_ps", GPU_FORMAT_RGBA16F, GPU_BLEND_ADD);
    tonemap_pipeline = screen_pipeline("pr_tonemap_ps", GPU_FORMAT_RGBA8, GPU_BLEND_NONE);
    fxaa_pipeline = screen_pipeline("pr_fxaa_ps", GPU_FORMAT_RGBA8, GPU_BLEND_NONE);

    brdf_lut = create_brdf_lut();

    if (!shadow_pipeline || !shadow_masked_pipeline || !visibility_pipeline || !ao_pipeline || !ao_blur_pipeline || !shade_pipeline ||
        !bloom_down_pipeline || !bloom_up_pipeline || !tonemap_pipeline || !fxaa_pipeline || !brdf_lut)
    {
        printf("Could not create the photorealistic renderer's GPU objects.\n");
        created = 1; // what exists gets released
        photoreal_destroy();
        return 0;
    }

    photoreal_default_quality(&quality);
    photoreal_default_look(&look);
    created = 1;

    DEBUG_LOG("[photoreal] renderer created\n");

    return 1;
}

static void destroy_target(Texture** texture)
{
    if (*texture)
    {
        destroy_texture(*texture);
        *texture = NULL;
    }
}

static void destroy_targets(void)
{
    destroy_target(&visibility_target);
    destroy_target(&depth_target);
    destroy_target(&hdr_target);
    destroy_target(&ao_target);
    destroy_target(&ao_blur_target);
    destroy_target(&ldr_target);
    destroy_target(&final_target);

    for (int i = 0; i < BLOOM_LEVELS; i++)
    {
        destroy_target(&bloom_targets[i]);
    }

    target_width = 0;
    target_height = 0;
    final_width = 0;
    final_height = 0;
}

static void destroy_shadow_maps(void)
{
    for (int i = 0; i < MAX_CASCADES; i++)
    {
        destroy_target(&shadow_maps[i]);
    }

    shadow_map_size = 0;
}

void photoreal_destroy(void)
{
    if (!created)
    {
        return;
    }

    wait_renderer_idle();

    destroy_targets();
    destroy_shadow_maps();
    destroy_target(&brdf_lut);

    gpu_destroy_pipeline(shadow_pipeline);
    gpu_destroy_pipeline(shadow_masked_pipeline);
    gpu_destroy_pipeline(visibility_pipeline);
    gpu_destroy_pipeline(ao_pipeline);
    gpu_destroy_pipeline(ao_blur_pipeline);
    gpu_destroy_pipeline(shade_pipeline);
    gpu_destroy_pipeline(bloom_down_pipeline);
    gpu_destroy_pipeline(bloom_up_pipeline);
    gpu_destroy_pipeline(tonemap_pipeline);
    gpu_destroy_pipeline(fxaa_pipeline);

    shadow_pipeline = shadow_masked_pipeline = visibility_pipeline = ao_pipeline = ao_blur_pipeline = shade_pipeline = NULL;
    bloom_down_pipeline = bloom_up_pipeline = tonemap_pipeline = fxaa_pipeline = NULL;

    created = 0;
}

void photoreal_set_settings(const PhotorealQuality* new_quality, const PhotorealLook* new_look)
{
    quality = *new_quality;
    look = *new_look;
    quality.shadow_cascades = SDL_clamp(quality.shadow_cascades, 1, MAX_CASCADES);
    quality.render_scale = SDL_clamp(quality.render_scale, 0.25f, 2.0f);
}

void photoreal_set_environment(const Environment* new_environment, const float direction[3], const float color[3])
{
    environment = new_environment;
    SDL_memcpy(sun_direction, direction, sizeof(sun_direction));
    SDL_memcpy(sun_color, color, sizeof(sun_color));
}

void photoreal_begin(const PhotorealCamera* new_camera)
{
    camera = *new_camera;
    draw_count = 0;
}

static uint32_t texture_index(Texture* texture)
{
    return texture ? gpu_texture_index(texture) : NO_TEXTURE;
}

void photoreal_add(Mesh* mesh, int first_index, int index_count, const float model[16], const float bounds[6], const PbrMaterial* material)
{
    if (draw_count == MAX_DRAWS || !mesh || index_count <= 0)
    {
        return;
    }

    PhotorealInstance* instance = &instances[draw_count];
    Draw* draw = &draws[draw_count];

    SDL_memcpy(instance->model, model, sizeof(instance->model));

    // Normals transform by the inverse transpose of the upper 3x3 (cofactors : normalized in the shader).
    float a = model[0], b = model[1], c = model[2];
    float d = model[4], e = model[5], f = model[6];
    float g = model[8], h = model[9], i = model[10];

    const float cofactors[12] = {
        e * i - f * h, f * g - d * i, d * h - e * g, 0.0f,
        c * h - b * i, a * i - c * g, b * g - a * h, 0.0f,
        b * f - c * e, c * d - a * f, a * e - b * d, 0.0f
    };

    float determinant = a * cofactors[0] + b * cofactors[1] + c * cofactors[2];
    float sign = determinant < 0.0f ? -1.0f : 1.0f;

    for (int n = 0; n < 12; n++)
    {
        instance->normal_matrix[n] = cofactors[n] * sign;
    }

    instance->geometry[0] = gpu_mesh_vertices_index(mesh);
    instance->geometry[1] = gpu_mesh_indices_index(mesh);
    instance->geometry[2] = (uint32_t)first_index;
    instance->geometry[3] = (material->alpha_cutoff > 0.0f ? 1u : 0u) | (material->ignore_vertex_color ? 2u : 0u);

    SDL_memcpy(instance->base_color, material->base_color, sizeof(instance->base_color));
    SDL_memcpy(instance->emissive, material->emissive, sizeof(float) * 3);
    instance->emissive[3] = material->metallic;
    instance->surface[0] = material->roughness;
    instance->surface[1] = material->normal_scale;
    instance->surface[2] = material->occlusion_strength;
    instance->surface[3] = material->alpha_cutoff;
    instance->textures[0] = texture_index(material->base_color_texture);
    instance->textures[1] = texture_index(material->normal_texture);
    instance->textures[2] = texture_index(material->metallic_roughness_texture);
    instance->textures[3] = texture_index(material->emissive_texture);
    instance->textures2[0] = texture_index(material->occlusion_texture);
    instance->textures2[1] = instance->textures2[2] = instance->textures2[3] = 0;
    instance->uv_transform[0] = material->uv_scale[0];
    instance->uv_transform[1] = material->uv_scale[1];
    instance->uv_transform[2] = material->uv_offset[0];
    instance->uv_transform[3] = material->uv_offset[1];

    draw->mesh = mesh;
    draw->first_index = first_index;
    draw->index_count = index_count;
    draw->radius = -1.0f;

    if (bounds)
    {
        float cx = (bounds[0] + bounds[3]) * 0.5f;
        float cy = (bounds[1] + bounds[4]) * 0.5f;
        float cz = (bounds[2] + bounds[5]) * 0.5f;
        float ex = (bounds[3] - bounds[0]) * 0.5f;
        float ey = (bounds[4] - bounds[1]) * 0.5f;
        float ez = (bounds[5] - bounds[2]) * 0.5f;

        draw->center[0] = model[0] * cx + model[1] * cy + model[2] * cz + model[3];
        draw->center[1] = model[4] * cx + model[5] * cy + model[6] * cz + model[7];
        draw->center[2] = model[8] * cx + model[9] * cy + model[10] * cz + model[11];

        // The transformed box's half-extents, whose length bounds it in every direction.
        float wx = SDL_fabsf(model[0]) * ex + SDL_fabsf(model[1]) * ey + SDL_fabsf(model[2]) * ez;
        float wy = SDL_fabsf(model[4]) * ex + SDL_fabsf(model[5]) * ey + SDL_fabsf(model[6]) * ez;
        float wz = SDL_fabsf(model[8]) * ex + SDL_fabsf(model[9]) * ey + SDL_fabsf(model[10]) * ez;
        draw->radius = SDL_sqrtf(wx * wx + wy * wy + wz * wz);
    }

    draw_count++;
}

static Texture* create_target(int width, int height, GpuFormat format, unsigned usage)
{
    GpuTextureDesc desc;
    SDL_zero(desc);
    desc.width = SDL_max(1, width);
    desc.height = SDL_max(1, height);
    desc.mip_count = 1;
    desc.format = format;
    desc.usage = GPU_USAGE_SAMPLED | usage;
    return gpu_create_texture(&desc);
}

static int ensure_targets(int width, int height, int window_width, int window_height)
{
    if (width == target_width && height == target_height && window_width == final_width && window_height == final_height)
    {
        return visibility_target != NULL;
    }

    wait_renderer_idle();
    destroy_targets();

    visibility_target = create_target(width, height, GPU_FORMAT_RGBA8, GPU_USAGE_RENDER_TARGET);
    depth_target = create_target(width, height, GPU_FORMAT_D32, GPU_USAGE_DEPTH_TARGET);
    hdr_target = create_target(width, height, GPU_FORMAT_RGBA16F, GPU_USAGE_RENDER_TARGET);
    ao_target = create_target(width, height, GPU_FORMAT_R8, GPU_USAGE_RENDER_TARGET);
    ao_blur_target = create_target(width, height, GPU_FORMAT_R8, GPU_USAGE_RENDER_TARGET);
    ldr_target = create_target(width, height, GPU_FORMAT_RGBA8, GPU_USAGE_RENDER_TARGET);
    final_target = create_target(window_width, window_height, GPU_FORMAT_RGBA8, GPU_USAGE_RENDER_TARGET);

    bloom_level_count = 0;

    for (int i = 0; i < BLOOM_LEVELS; i++)
    {
        int w = width >> (i + 1);
        int h = height >> (i + 1);

        if (w < 4 || h < 4)
        {
            break;
        }

        bloom_targets[i] = create_target(w, h, GPU_FORMAT_RGBA16F, GPU_USAGE_RENDER_TARGET);
        bloom_level_count++;
    }

    target_width = width;
    target_height = height;
    final_width = window_width;
    final_height = window_height;

    return visibility_target && depth_target && hdr_target && ao_target && ao_blur_target && ldr_target && final_target;
}

static int ensure_shadow_maps(void)
{
    if (shadow_map_size == quality.shadow_resolution)
    {
        return shadow_maps[0] != NULL;
    }

    wait_renderer_idle();
    destroy_shadow_maps();

    for (int i = 0; i < MAX_CASCADES; i++)
    {
        shadow_maps[i] = create_target(quality.shadow_resolution, quality.shadow_resolution, GPU_FORMAT_D32, GPU_USAGE_DEPTH_TARGET);
    }

    shadow_map_size = quality.shadow_resolution;

    return shadow_maps[0] != NULL;
}

typedef struct
{
    Mat4 view_projection;
    float far_distance;
    float texel_world;
    float depth_range;
    Vec3 center;
    float radius;
    Vec3 forward;
    float max_depth;
} Cascade;

// Cascade split distances (practical split scheme), each cascade's bounding sphere, and a light-space
// projection around it whose center is snapped to whole texels, so shadows don't shimmer as the camera moves.
static void compute_cascades(Cascade* cascades, int count, const Mat4* inverse_view_projection)
{
    Vec3 to_sun = vec3_normalize(vec3(sun_direction[0], sun_direction[1], sun_direction[2]));
    Vec3 forward = vec3_scale(to_sun, -1.0f);
    Vec3 up_hint = SDL_fabsf(forward.y) > 0.99f ? vec3(0.0f, 0.0f, 1.0f) : vec3(0.0f, 1.0f, 0.0f);
    Vec3 right = vec3_normalize(vec3_cross(forward, up_hint));
    Vec3 up = vec3_cross(right, forward);

    // How far casters extend toward the sun, to keep them all inside each cascade's depth range.
    float caster_top = -1e30f;

    for (int i = 0; i < draw_count; i++)
    {
        float extent = draws[i].radius >= 0.0f ? draws[i].radius : 200.0f;
        Vec3 center = draws[i].radius >= 0.0f ? vec3(draws[i].center[0], draws[i].center[1], draws[i].center[2]) :
            vec3(instances[i].model[3], instances[i].model[7], instances[i].model[11]);
        caster_top = SDL_max(caster_top, vec3_dot(center, to_sun) + extent);
    }

    // Each corner ray of the view frustum, from its near point to its far point.
    Vec3 near_corners[4];
    Vec3 far_corners[4];

    for (int i = 0; i < 4; i++)
    {
        float x = (i & 1) ? 1.0f : -1.0f;
        float y = (i & 2) ? 1.0f : -1.0f;
        float n[4];
        float f[4];
        mat4_transform(*inverse_view_projection, x, y, 0.0f, 1.0f, n);
        mat4_transform(*inverse_view_projection, x, y, 1.0f, 1.0f, f);
        near_corners[i] = vec3(n[0] / n[3], n[1] / n[3], n[2] / n[3]);
        far_corners[i] = vec3(f[0] / f[3], f[1] / f[3], f[2] / f[3]);
    }

    float near_distance = camera.near_plane;
    float far_distance = SDL_min(camera.far_plane, quality.shadow_distance);
    float previous = near_distance;

    for (int c = 0; c < count; c++)
    {
        float t = (float)(c + 1) / (float)count;
        float logarithmic = near_distance * SDL_powf(far_distance / near_distance, t);
        float uniform = near_distance + (far_distance - near_distance) * t;
        float split = logarithmic * 0.75f + uniform * 0.25f;

        float t0 = (previous - near_distance) / (camera.far_plane - near_distance);
        float t1 = (split - near_distance) / (camera.far_plane - near_distance);

        Vec3 corners[8];
        Vec3 center = vec3(0.0f, 0.0f, 0.0f);

        for (int i = 0; i < 4; i++)
        {
            Vec3 ray = vec3_sub(far_corners[i], near_corners[i]);
            corners[i] = vec3_add(near_corners[i], vec3_scale(ray, t0));
            corners[i + 4] = vec3_add(near_corners[i], vec3_scale(ray, t1));
            center = vec3_add(center, vec3_add(corners[i], corners[i + 4]));
        }

        center = vec3_scale(center, 1.0f / 8.0f);

        float radius = 0.0f;

        for (int i = 0; i < 8; i++)
        {
            radius = SDL_max(radius, vec3_length(vec3_sub(corners[i], center)));
        }

        radius = SDL_ceilf(radius * 16.0f) / 16.0f;

        float texel = (2.0f * radius) / (float)quality.shadow_resolution;

        float cx = SDL_floorf(vec3_dot(center, right) / texel) * texel;
        float cy = SDL_floorf(vec3_dot(center, up) / texel) * texel;
        float cz = vec3_dot(center, forward);
        Vec3 snapped = vec3_add(vec3_add(vec3_scale(right, cx), vec3_scale(up, cy)), vec3_scale(forward, cz));

        float back = SDL_max(radius, caster_top + cz); // caster_top is along to_sun, cz along forward
        Vec3 eye = vec3_sub(snapped, vec3_scale(forward, back + 1.0f));
        float depth_range = back + 1.0f + radius;

        Mat4 view = mat4_look_at(eye, snapped, up);
        Mat4 projection = mat4_orthographic(-radius, radius, -radius, radius, 0.0f, depth_range);

        cascades[c].view_projection = mat4_multiply(projection, view);
        cascades[c].far_distance = split;
        cascades[c].texel_world = texel;
        cascades[c].depth_range = depth_range;
        cascades[c].center = snapped;
        cascades[c].radius = radius;
        cascades[c].forward = forward;
        cascades[c].max_depth = depth_range;

        previous = split;
    }
}

// Whether a draw's bounding sphere reaches into a cascade's (infinite toward the sun) cylinder.
static int draw_in_cascade(const Draw* draw, const Cascade* cascade)
{
    if (draw->radius < 0.0f)
    {
        return 1;
    }

    Vec3 offset = vec3_sub(vec3(draw->center[0], draw->center[1], draw->center[2]), cascade->center);
    float along = vec3_dot(offset, cascade->forward);
    Vec3 across = vec3_sub(offset, vec3_scale(cascade->forward, along));

    return vec3_length(across) <= cascade->radius * 1.42f + draw->radius && along <= cascade->radius + draw->radius;
}

// Against the view frustum, its six planes extracted from the view-projection (Gribb & Hartmann).
static int draw_in_view(const Draw* draw, const Mat4* view_projection)
{
    if (draw->radius < 0.0f)
    {
        return 1;
    }

    const float* m = view_projection->m;

    float planes[5][4] = {
        { m[12] + m[0], m[13] + m[1], m[14] + m[2], m[15] + m[3] },
        { m[12] - m[0], m[13] - m[1], m[14] - m[2], m[15] - m[3] },
        { m[12] + m[4], m[13] + m[5], m[14] + m[6], m[15] + m[7] },
        { m[12] - m[4], m[13] - m[5], m[14] - m[6], m[15] - m[7] },
        { m[8], m[9], m[10], m[11] } // near : depth in [0, 1]
    };

    for (int i = 0; i < 5; i++)
    {
        float length = SDL_sqrtf(planes[i][0] * planes[i][0] + planes[i][1] * planes[i][1] + planes[i][2] * planes[i][2]);
        float distance = planes[i][0] * draw->center[0] + planes[i][1] * draw->center[1] + planes[i][2] * draw->center[2] + planes[i][3];

        if (distance < -draw->radius * length)
        {
            return 0;
        }
    }

    return 1;
}

static void fullscreen_pass(Texture* target, GpuPipeline* pipeline, const PassConstants* constants)
{
    GpuPassDesc pass;
    SDL_zero(pass);
    pass.colors[0].texture = target;
    pass.colors[0].load = GPU_LOAD_DISCARD;
    pass.color_count = 1;

    gpu_begin_pass(&pass);
    gpu_bind_pipeline(pipeline);
    gpu_push_constants(constants, sizeof(*constants));
    gpu_draw(3);
    gpu_end_pass();
}

void photoreal_render(int width, int height)
{
    if (!created || !environment || width <= 0 || height <= 0)
    {
        return;
    }

    int internal_width = SDL_max(1, (int)((float)width * quality.render_scale));
    int internal_height = SDL_max(1, (int)((float)height * quality.render_scale));

    // Before anything is recorded : these may wait for the GPU and recreate resources.
    if (!ensure_targets(internal_width, internal_height, width, height) || !ensure_shadow_maps())
    {
        return;
    }

    GpuFrameAllocation frame_memory = gpu_frame_alloc(sizeof(PhotorealFrame));
    GpuFrameAllocation instance_memory = gpu_frame_alloc(sizeof(PhotorealInstance) * (size_t)SDL_max(1, draw_count));

    if (!frame_memory.mapped || !instance_memory.mapped)
    {
        printf("Out of frame memory : skipping the photorealistic pass.\n");
        return;
    }

    Mat4 view_projection = mat4_multiply(camera.projection, camera.view);
    Mat4 inverse_view_projection = mat4_inverse(view_projection);
    Mat4 inverse_projection = mat4_inverse(camera.projection);

    Cascade cascades[MAX_CASCADES];
    compute_cascades(cascades, quality.shadow_cascades, &inverse_view_projection);

    PhotorealFrame* frame = frame_memory.mapped;
    SDL_zerop(frame);
    SDL_memcpy(frame->view_projection, view_projection.m, sizeof(frame->view_projection));
    SDL_memcpy(frame->inverse_view_projection, inverse_view_projection.m, sizeof(frame->inverse_view_projection));
    SDL_memcpy(frame->view, camera.view.m, sizeof(frame->view));
    SDL_memcpy(frame->inverse_projection, inverse_projection.m, sizeof(frame->inverse_projection));

    frame->camera_position[0] = camera.eye.x;
    frame->camera_position[1] = camera.eye.y;
    frame->camera_position[2] = camera.eye.z;

    Vec3 to_sun = vec3_normalize(vec3(sun_direction[0], sun_direction[1], sun_direction[2]));
    frame->sun_direction[0] = to_sun.x;
    frame->sun_direction[1] = to_sun.y;
    frame->sun_direction[2] = to_sun.z;
    frame->sun_direction[3] = 0.00465f; // the real sun : 0.2665 degrees of angular radius

    for (int c = 0; c < 3; c++)
    {
        frame->sun_color[c] = sun_color[c];
    }

    // The disk's radiance : its irradiance spread over its solid angle, scaled down (it would saturate anyway).
    frame->sun_color[3] = environment->show_sun_disk ? 500.0f : 0.0f;

    for (int i = 0; i < 9; i++)
    {
        for (int c = 0; c < 3; c++)
        {
            frame->sh[i][c] = environment->sh[i][c];
        }
    }

    frame->environment[0] = look.sky_intensity;
    frame->environment[1] = (float)environment->prefiltered_mips;
    frame->environment[2] = look.bounce_strength;

    frame->environment_textures[0] = gpu_texture_index(environment->panorama);
    frame->environment_textures[1] = gpu_texture_index(environment->prefiltered);
    frame->environment_textures[2] = gpu_texture_index(brdf_lut);
    frame->environment_textures[3] = quality.ao_enabled ? gpu_texture_index(ao_target) : NO_TEXTURE;

    for (int c = 0; c < 3; c++)
    {
        frame->fog_inscatter[c] = look.fog_color[c];
        frame->fog_sun[c] = look.fog_sun_color[c];
    }

    frame->fog_inscatter[3] = look.fog_density;
    frame->fog_sun[3] = look.fog_height_falloff;
    frame->fog_params[0] = look.fog_base_height;
    frame->fog_params[1] = look.fog_sun_power;

    for (int c = 0; c < quality.shadow_cascades; c++)
    {
        SDL_memcpy(frame->shadow_view_projection[c], cascades[c].view_projection.m, sizeof(frame->shadow_view_projection[c]));
        frame->cascade_far[c] = cascades[c].far_distance;
        frame->shadow_textures[c] = gpu_texture_index(shadow_maps[c]);
        frame->shadow_texel_world[c] = cascades[c].texel_world;
        frame->shadow_depth_range[c] = cascades[c].depth_range;
    }

    frame->shadow_params[0] = 3.0f;
    frame->shadow_params[1] = (float)quality.shadow_samples;
    frame->shadow_params[2] = (float)quality.shadow_cascades;
    frame->shadow_params[3] = look.sun_softness;

    frame->resolution[0] = (float)internal_width;
    frame->resolution[1] = (float)internal_height;
    frame->resolution[2] = 1.0f / (float)internal_width;
    frame->resolution[3] = 1.0f / (float)internal_height;

    SDL_memcpy(instance_memory.mapped, instances, sizeof(PhotorealInstance) * (size_t)draw_count);

    PassConstants constants;
    SDL_zero(constants);
    constants.ids[0] = frame_memory.buffer_index;
    constants.ids[1] = frame_memory.offset;
    constants.ids[2] = instance_memory.offset;

    Texture* lit_image = hdr_target;

    // 1. Shadow cascades.
    for (int c = 0; c < quality.shadow_cascades; c++)
    {
        GpuPassDesc pass;
        SDL_zero(pass);
        pass.depth = shadow_maps[c];
        pass.depth_load = GPU_LOAD_CLEAR;
        pass.depth_clear = 1.0f;

        gpu_begin_pass(&pass);

        constants.inputs[0] = (uint32_t)c;

        // Opaque casters, then alpha-masked ones : each pipeline is bound once.
        for (int masked = 0; masked <= 1; masked++)
        {
            gpu_bind_pipeline(masked ? shadow_masked_pipeline : shadow_pipeline);

            for (int i = 0; i < draw_count; i++)
            {
                int is_masked = (instances[i].geometry[3] & 1u) != 0 && instances[i].textures[0] != NO_TEXTURE;

                if (is_masked == masked && draw_in_cascade(&draws[i], &cascades[c]))
                {
                    constants.ids[3] = (uint32_t)i;
                    gpu_push_constants(&constants, sizeof(constants));
                    gpu_draw_indexed(draws[i].mesh, draws[i].first_index, draws[i].index_count);
                }
            }
        }

        gpu_end_pass();
    }

    // 2. Visibility buffer : which triangle covers each pixel.
    GpuPassDesc pass;
    SDL_zero(pass);
    pass.colors[0].texture = visibility_target;
    pass.colors[0].load = GPU_LOAD_CLEAR;
    pass.colors[0].clear[0] = pass.colors[0].clear[1] = pass.colors[0].clear[2] = pass.colors[0].clear[3] = 1.0f;
    pass.color_count = 1;
    pass.depth = depth_target;
    pass.depth_load = GPU_LOAD_CLEAR;
    pass.depth_clear = 1.0f;

    gpu_begin_pass(&pass);
    gpu_bind_pipeline(visibility_pipeline);

    for (int i = 0; i < draw_count; i++)
    {
        if (draw_in_view(&draws[i], &view_projection))
        {
            constants.ids[3] = (uint32_t)i;
            gpu_push_constants(&constants, sizeof(constants));
            gpu_draw_indexed(draws[i].mesh, draws[i].first_index, draws[i].index_count);
        }
    }

    gpu_end_pass();

    // 3. Ambient occlusion, from depth, blurred.
    if (quality.ao_enabled)
    {
        SDL_zero(constants.inputs);
        constants.inputs[0] = gpu_texture_index(depth_target);
        constants.params[0] = look.ao_radius;
        constants.params[1] = (float)quality.ao_slices;
        constants.params[2] = (float)quality.ao_steps;
        constants.params[3] = look.ao_power;
        fullscreen_pass(ao_blur_target, ao_pipeline, &constants);

        constants.inputs[0] = gpu_texture_index(ao_blur_target);
        constants.inputs[1] = gpu_texture_index(depth_target);
        constants.params[0] = 1.0f;
        constants.params[1] = 0.0f;
        fullscreen_pass(ao_target, ao_blur_pipeline, &constants);

        constants.inputs[0] = gpu_texture_index(ao_target);
        constants.params[0] = 0.0f;
        constants.params[1] = 1.0f;
        fullscreen_pass(ao_blur_target, ao_blur_pipeline, &constants);

        frame->environment_textures[3] = gpu_texture_index(ao_blur_target);
    }

    // 4. Shading : each pixel exactly once.
    SDL_zero(constants.inputs);
    constants.inputs[0] = gpu_texture_index(visibility_target);
    fullscreen_pass(hdr_target, shade_pipeline, &constants);

    // 5. Bloom : a chain of downsamples, then back up, each level adding to the next.
    if (quality.bloom_enabled && bloom_level_count > 0)
    {
        Texture* source = lit_image;
        int source_width = internal_width;
        int source_height = internal_height;

        for (int i = 0; i < bloom_level_count; i++)
        {
            SDL_zero(constants.inputs);
            constants.inputs[0] = gpu_texture_index(source);
            constants.params[0] = 1.0f / (float)source_width;
            constants.params[1] = 1.0f / (float)source_height;
            constants.params[2] = i == 0 ? 64.0f : 60000.0f;
            fullscreen_pass(bloom_targets[i], bloom_down_pipeline, &constants);

            source = bloom_targets[i];
            source_width = internal_width >> (i + 1);
            source_height = internal_height >> (i + 1);
        }

        for (int i = bloom_level_count - 1; i > 0; i--)
        {
            GpuPassDesc pass;
            SDL_zero(pass);
            pass.colors[0].texture = bloom_targets[i - 1];
            pass.colors[0].load = GPU_LOAD_KEEP;
            pass.color_count = 1;

            SDL_zero(constants.inputs);
            constants.inputs[0] = gpu_texture_index(bloom_targets[i]);
            constants.params[0] = 1.0f / (float)(internal_width >> (i + 1));
            constants.params[1] = 1.0f / (float)(internal_height >> (i + 1));

            gpu_begin_pass(&pass);
            gpu_bind_pipeline(bloom_up_pipeline);
            gpu_push_constants(&constants, sizeof(constants));
            gpu_draw(3);
            gpu_end_pass();
        }
    }

    // 6. Tone mapping, then anti-aliasing at the window's resolution.
    SDL_zero(constants.inputs);
    constants.inputs[0] = gpu_texture_index(lit_image);
    constants.inputs[1] = quality.bloom_enabled && bloom_level_count > 0 ? gpu_texture_index(bloom_targets[0]) : NO_TEXTURE;

    // Automatic exposure : middle grey for the environment's average brightness.
    float average = SDL_max(environment->average_luminance * look.sky_intensity, 1e-3f);
    constants.params[0] = (0.22f / average) * SDL_powf(2.0f, look.exposure_ev);
    constants.params[1] = look.bloom_strength;
    constants.params[2] = (float)look.tonemapper;
    constants.params[3] = look.contrast;
    constants.params2[0] = look.saturation;
    constants.params2[1] = look.vignette;
    fullscreen_pass(ldr_target, tonemap_pipeline, &constants);

    SDL_zero(constants.inputs);
    constants.inputs[0] = gpu_texture_index(ldr_target);
    constants.params[0] = 1.0f / (float)internal_width;
    constants.params[1] = 1.0f / (float)internal_height;
    constants.params[2] = quality.fxaa_enabled ? 1.0f : 0.0f;
    fullscreen_pass(final_target, fxaa_pipeline, &constants);
}

void photoreal_present(void)
{
    if (!final_target)
    {
        return;
    }

    float w = (float)final_width;
    float h = (float)final_height;

    const Vertex vertices[6] = {
        { 0.0f, 0.0f, 0.0f, 0.0f, 1.0f, 1.0f, 1.0f, 1.0f },
        { w, 0.0f, 1.0f, 0.0f, 1.0f, 1.0f, 1.0f, 1.0f },
        { w, h, 1.0f, 1.0f, 1.0f, 1.0f, 1.0f, 1.0f },
        { 0.0f, 0.0f, 0.0f, 0.0f, 1.0f, 1.0f, 1.0f, 1.0f },
        { w, h, 1.0f, 1.0f, 1.0f, 1.0f, 1.0f, 1.0f },
        { 0.0f, h, 0.0f, 1.0f, 1.0f, 1.0f, 1.0f, 1.0f }
    };

    draw_vertices(vertices, 6, final_target);
}
