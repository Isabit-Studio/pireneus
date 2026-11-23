// The 3D renderer's API and the passes both pipelines share (shadows, ambient occlusion, bloom, tone
// mapping, anti-aliasing) ; forward_plus.c and deferred_plus.c draw the scene itself. Shaders :
// render3d_common.slang (shared lighting), render3d.slang (shared passes), forward_plus.slang and
// deferred_plus.slang.

#include "renderer/render3d/render3d.h"
#include "renderer/render3d/render3d_internal.h"

#include <SDL3/SDL.h>
#include <stdio.h>

#include "renderer/render3d/half_float.h"
#include "debug.h"

#define BLOOM_LEVELS 6
#define BRDF_LUT_SIZE 128
#define PI_F 3.14159265f

R3State r3;

SDL_COMPILE_TIME_ASSERT(instance_size, sizeof(R3Instance) == 240);

static int created;
static Render3DPipeline pipeline = RENDER3D_DEFERRED_PLUS;
static Render3DPipeline targets_pipeline; // whose targets exist

static GpuPipeline* shadow_pipeline;
static GpuPipeline* shadow_masked_pipeline; // alpha-masked casters
static GpuPipeline* ao_pipeline;
static GpuPipeline* ao_blur_pipeline;
static GpuPipeline* bloom_down_pipeline;
static GpuPipeline* bloom_up_pipeline;
static GpuPipeline* tonemap_pipeline;
static GpuPipeline* fxaa_pipeline;
static GpuPipeline* taa_pipeline;
static GpuPipeline* sharpen_pipeline;
static GpuPipeline* volumetrics_pipeline;
static GpuPipeline* volumetrics_composite_pipeline;

static Texture* brdf_lut;
static Texture* shadow_maps[R3_MAX_CASCADES];
static int shadow_map_size;

static Texture* ao_target;
static Texture* ao_blur_target;
static Texture* bloom_targets[BLOOM_LEVELS];
static Texture* ldr_target;
static Texture* sharpen_source;    // with sharpening : the anti-aliased image, before
static Texture* volumetrics_target; // half resolution : scattered light, transmittance
static Uint64 start_ticks;          // the clouds drift with time
static Texture* taa_targets[2];    // this frame's result and the previous one, in turn
static int taa_current;
static int history_valid;
static Mat4 previous_view_projection; // unjittered
static uint32_t frame_number;
static Texture* final_target;
static int final_width;    // window resolution
static int final_height;
static int bloom_level_count;

static const Environment* environment;
static float sun_direction[3];
static float sun_color[3];
static Render3DProbeVolume probe_volume;
static int has_probe_volume;

void render3d_default_quality(Render3DQuality* out_quality)
{
    out_quality->render_scale = 1.0f;
    out_quality->shadow_resolution = 2048;
    out_quality->shadow_cascades = 3;
    out_quality->shadow_samples = 24;
    out_quality->shadow_distance = 120.0f;
    out_quality->local_shadow_atlas = 4096;
    out_quality->local_shadow_lights = 8;
    out_quality->ao_enabled = 1;
    out_quality->ao_slices = 3;
    out_quality->ao_steps = 5;
    out_quality->bloom_enabled = 1;
    out_quality->fxaa_enabled = 1;
    out_quality->msaa_samples = 4;
    out_quality->taa_enabled = 0;
    out_quality->sharpness = 0.0f;
    out_quality->ssr_enabled = 1;
    out_quality->ssr_steps = 24;
    out_quality->volumetrics_enabled = 1;
    out_quality->volumetric_steps = 24;
    out_quality->volumetric_local_lights = 1;
    out_quality->gi_enabled = 0;
    out_quality->gi_rays = 64;
    out_quality->gi_spacing = 1.5f;
    out_quality->gi_lights = 16;
}

void render3d_default_look(Render3DLook* out_look)
{
    out_look->exposure_ev = 0.0f;
    out_look->tonemapper = RENDER3D_TONEMAP_ACES;
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
    out_look->ssr_distance = 20.0f;
    out_look->ssr_thickness = 0.3f;
    out_look->volumetric_distance = 80.0f;
    out_look->volumetric_anisotropy = 0.6f;
    out_look->volumetric_intensity = 1.0f;
    out_look->cloud_coverage = 0.0f;
    out_look->cloud_density = 0.6f;
    out_look->cloud_altitude = 1500.0f;
    out_look->cloud_scale = 900.0f;
    out_look->cloud_wind[0] = 8.0f;
    out_look->cloud_wind[1] = 3.0f;
    out_look->cloud_shadows = 0.6f;
}

void render3d_default_light(Render3DLight* out_light)
{
    SDL_zerop(out_light);
    out_light->type = RENDER3D_LIGHT_POINT;
    out_light->direction[1] = -1.0f;
    out_light->color[0] = out_light->color[1] = out_light->color[2] = 1.0f;
    out_light->intensity = 10.0f;
    out_light->range = 10.0f;
    out_light->spot_angle = 60.0f;
    out_light->spot_softness = 0.3f;
}

void render3d_default_material(PbrMaterial* out_material)
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

GpuPipeline* r3_screen_pipeline(const char* fragment_shader, GpuFormat format, GpuBlend blend)
{
    GpuPipelineDesc desc;
    SDL_zero(desc);
    desc.vertex_shader = "r3_screen_vs";
    desc.fragment_shader = fragment_shader;
    desc.color_formats[0] = format;
    desc.color_count = 1;
    desc.blend = blend;
    return gpu_create_pipeline(&desc);
}

int render3d_create(void)
{
    if (!gpu_supported())
    {
        printf("This GPU can't run the 3D renderer.\n");
        return 0;
    }

    GpuPipelineDesc desc;
    SDL_zero(desc);
    desc.vertex_shader = "r3_shadow_vs";
    desc.depth_format = GPU_FORMAT_D32;
    desc.depth_test = 1;
    desc.depth_write = 1;
    shadow_pipeline = gpu_create_pipeline(&desc);

    desc.vertex_shader = "r3_shadow_masked_vs";
    desc.fragment_shader = "r3_shadow_masked_ps";
    shadow_masked_pipeline = gpu_create_pipeline(&desc);

    ao_pipeline = r3_screen_pipeline("r3_ao_ps", GPU_FORMAT_R8, GPU_BLEND_NONE);
    ao_blur_pipeline = r3_screen_pipeline("r3_ao_blur_ps", GPU_FORMAT_R8, GPU_BLEND_NONE);
    bloom_down_pipeline = r3_screen_pipeline("r3_bloom_down_ps", GPU_FORMAT_RGBA16F, GPU_BLEND_NONE);
    bloom_up_pipeline = r3_screen_pipeline("r3_bloom_up_ps", GPU_FORMAT_RGBA16F, GPU_BLEND_ADD);
    tonemap_pipeline = r3_screen_pipeline("r3_tonemap_ps", GPU_FORMAT_RGBA8, GPU_BLEND_NONE);
    fxaa_pipeline = r3_screen_pipeline("r3_fxaa_ps", GPU_FORMAT_RGBA8, GPU_BLEND_NONE);
    taa_pipeline = r3_screen_pipeline("r3_taa_ps", GPU_FORMAT_RGBA16F, GPU_BLEND_NONE);
    sharpen_pipeline = r3_screen_pipeline("r3_sharpen_ps", GPU_FORMAT_RGBA8, GPU_BLEND_NONE);
    volumetrics_pipeline = r3_screen_pipeline("r3_volumetrics_ps", GPU_FORMAT_RGBA16F, GPU_BLEND_NONE);
    volumetrics_composite_pipeline = r3_screen_pipeline("r3_volumetrics_composite_ps", GPU_FORMAT_RGBA16F, GPU_BLEND_ATTENUATE);
    start_ticks = SDL_GetTicks();

    brdf_lut = create_brdf_lut();

    created = 1; // from here, render3d_destroy() releases whatever exists

    if (!shadow_pipeline || !shadow_masked_pipeline || !ao_pipeline || !ao_blur_pipeline || !bloom_down_pipeline ||
        !bloom_up_pipeline || !tonemap_pipeline || !fxaa_pipeline || !taa_pipeline || !sharpen_pipeline || !volumetrics_pipeline ||
        !volumetrics_composite_pipeline || !brdf_lut ||
        !forward_plus_create() || !deferred_plus_create() ||
        !r3_lights_create() || !r3_gi_create())
    {
        printf("Could not create the 3D renderer's GPU objects.\n");
        render3d_destroy();
        return 0;
    }

    render3d_default_quality(&r3.quality);
    render3d_default_look(&r3.look);

    DEBUG_LOG("[render3d] renderer created\n");

    return 1;
}

Texture* r3_create_target(int width, int height, GpuFormat format, unsigned usage)
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

void r3_destroy_target(Texture** texture)
{
    if (*texture)
    {
        destroy_texture(*texture);
        *texture = NULL;
    }
}

static void destroy_targets(void)
{
    forward_plus_destroy_targets();
    deferred_plus_destroy_targets();

    r3_destroy_target(&r3.depth_target);
    r3_destroy_target(&r3.hdr_target);
    r3.scene_depth = NULL;
    r3_destroy_target(&ao_target);
    r3_destroy_target(&ao_blur_target);
    r3_destroy_target(&ldr_target);
    r3_destroy_target(&sharpen_source);
    r3_destroy_target(&volumetrics_target);
    r3_destroy_target(&taa_targets[0]);
    r3_destroy_target(&taa_targets[1]);
    r3_destroy_target(&final_target);
    history_valid = 0;

    for (int i = 0; i < BLOOM_LEVELS; i++)
    {
        r3_destroy_target(&bloom_targets[i]);
    }

    r3.width = 0;
    r3.height = 0;
    final_width = 0;
    final_height = 0;
}

static void destroy_shadow_maps(void)
{
    for (int i = 0; i < R3_MAX_CASCADES; i++)
    {
        r3_destroy_target(&shadow_maps[i]);
    }

    shadow_map_size = 0;
}

static void destroy_pipeline(GpuPipeline** p)
{
    if (*p)
    {
        gpu_destroy_pipeline(*p);
        *p = NULL;
    }
}

void render3d_destroy(void)
{
    if (!created)
    {
        return;
    }

    wait_renderer_idle();

    destroy_targets();
    destroy_shadow_maps();
    r3_destroy_target(&brdf_lut);

    forward_plus_destroy();
    deferred_plus_destroy();
    r3_lights_destroy();
    r3_gi_destroy();

    destroy_pipeline(&shadow_pipeline);
    destroy_pipeline(&shadow_masked_pipeline);
    destroy_pipeline(&ao_pipeline);
    destroy_pipeline(&ao_blur_pipeline);
    destroy_pipeline(&bloom_down_pipeline);
    destroy_pipeline(&bloom_up_pipeline);
    destroy_pipeline(&tonemap_pipeline);
    destroy_pipeline(&fxaa_pipeline);
    destroy_pipeline(&taa_pipeline);
    destroy_pipeline(&sharpen_pipeline);
    destroy_pipeline(&volumetrics_pipeline);
    destroy_pipeline(&volumetrics_composite_pipeline);

    created = 0;
}

void render3d_set_pipeline(Render3DPipeline new_pipeline)
{
    pipeline = new_pipeline;
}

Render3DPipeline render3d_pipeline(void)
{
    return pipeline;
}

void render3d_set_settings(const Render3DQuality* new_quality, const Render3DLook* new_look)
{
    r3.quality = *new_quality;
    r3.look = *new_look;
    r3.quality.shadow_cascades = SDL_clamp(r3.quality.shadow_cascades, 1, R3_MAX_CASCADES);
    r3.quality.render_scale = SDL_clamp(r3.quality.render_scale, 0.25f, 2.0f);
    r3.quality.local_shadow_atlas = SDL_clamp(r3.quality.local_shadow_atlas, 1024, 8192);
    r3.quality.local_shadow_lights = SDL_clamp(r3.quality.local_shadow_lights, 0, 32);
    r3.quality.msaa_samples = r3.quality.msaa_samples >= 8 ? 8 : r3.quality.msaa_samples >= 4 ? 4 : r3.quality.msaa_samples >= 2 ? 2 : 1;
    r3.quality.sharpness = SDL_clamp(r3.quality.sharpness, 0.0f, 1.0f);
    r3.quality.ssr_steps = SDL_clamp(r3.quality.ssr_steps, 4, 128);
    r3.quality.volumetric_steps = SDL_clamp(r3.quality.volumetric_steps, 4, 128);

    if (!r3.quality.taa_enabled)
    {
        history_valid = 0;
    }
}

void render3d_set_environment(const Environment* new_environment, const float direction[3], const float color[3])
{
    environment = new_environment;
    SDL_memcpy(sun_direction, direction, sizeof(sun_direction));
    SDL_memcpy(sun_color, color, sizeof(sun_color));
}

void render3d_begin(const Render3DCamera* new_camera)
{
    r3.camera = *new_camera;
    r3.draw_count = 0;
    r3.light_count = 0;
    r3.reflection_probe_count = 0;
}

void render3d_add_reflection_probe(const Render3DReflectionProbe* probe)
{
    if (r3.reflection_probe_count < R3_MAX_REFLECTION_PROBES && probe->environment && probe->environment->prefiltered)
    {
        r3.reflection_probes[r3.reflection_probe_count++] = *probe;
    }
}

Texture* render3d_hdr_image(void)
{
    return r3.hdr_target;
}

int render3d_realtime_gi_supported(void)
{
    return created && r3_gi_available();
}

void render3d_add_light(const Render3DLight* light)
{
    if (r3.light_count < R3_MAX_LIGHTS && light->range > 0.0f && light->intensity > 0.0f)
    {
        r3.lights[r3.light_count++] = *light;
    }
}

static uint32_t texture_index(Texture* texture)
{
    return texture ? gpu_texture_index(texture) : R3_NO_TEXTURE;
}

void render3d_set_probe_volume(const Render3DProbeVolume* volume)
{
    has_probe_volume = volume && volume->buffer && volume->cell_size > 0.0f;

    if (has_probe_volume)
    {
        probe_volume = *volume;
    }
}

void render3d_add(Mesh* mesh, int first_index, int index_count, const float model[16], const float bounds[6], const PbrMaterial* material,
    const Render3DDrawOptions* options)
{
    if (r3.draw_count == R3_MAX_DRAWS || !mesh || index_count <= 0)
    {
        return;
    }

    R3Instance* instance = &r3.instances[r3.draw_count];
    R3Draw* draw = &r3.draws[r3.draw_count];

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
    instance->geometry[3] = (material->alpha_cutoff > 0.0f ? 1u : 0u) | (material->ignore_vertex_color ? 2u : 0u) | (material->normal_y_down ? 4u : 0u);

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

    const Render3DLightmap* lightmap = options ? options->lightmap : NULL;
    SDL_zeroa(instance->lightmap);

    if (lightmap && lightmap->texture && lightmap->uvs)
    {
        instance->lightmap[0] = gpu_buffer_index(lightmap->uvs);
        instance->lightmap[1] = gpu_texture_index(lightmap->texture);
        instance->lightmap[2] = 1;
        SDL_memcpy(instance->lightmap_transform, lightmap->scale_offset, sizeof(instance->lightmap_transform));
    }

    draw->mesh = mesh;
    draw->hidden = options ? options->hidden_from_view : 0;
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
        draw->extent[0] = wx;
        draw->extent[1] = wy;
        draw->extent[2] = wz;
    }

    r3.draw_count++;
}

int r3_is_masked(int draw)
{
    return (r3.instances[draw].geometry[3] & 1u) != 0 && r3.instances[draw].textures[0] != R3_NO_TEXTURE;
}

static int ensure_targets(int width, int height, int window_width, int window_height)
{
    int msaa = pipeline == RENDER3D_FORWARD_PLUS ? r3.quality.msaa_samples : 1;
    int ssr = pipeline == RENDER3D_DEFERRED_PLUS && r3.quality.ssr_enabled;

    if (width == r3.width && height == r3.height && window_width == final_width && window_height == final_height && targets_pipeline == pipeline &&
        msaa == r3.msaa_samples && ssr == r3.ssr)
    {
        return r3.hdr_target != NULL;
    }

    wait_renderer_idle();
    destroy_targets();

    r3.width = width;
    r3.height = height;
    final_width = window_width;
    final_height = window_height;
    targets_pipeline = pipeline;
    r3.msaa_samples = msaa;
    r3.ssr = ssr;

    r3.depth_target = r3_create_target(width, height, GPU_FORMAT_D32, GPU_USAGE_DEPTH_TARGET);
    r3.scene_depth = r3.depth_target;
    r3.hdr_target = r3_create_target(width, height, GPU_FORMAT_RGBA16F, GPU_USAGE_RENDER_TARGET);
    ao_target = r3_create_target(width, height, GPU_FORMAT_R8, GPU_USAGE_RENDER_TARGET);
    ao_blur_target = r3_create_target(width, height, GPU_FORMAT_R8, GPU_USAGE_RENDER_TARGET);
    ldr_target = r3_create_target(width, height, GPU_FORMAT_RGBA8, GPU_USAGE_RENDER_TARGET);
    sharpen_source = r3_create_target(width, height, GPU_FORMAT_RGBA8, GPU_USAGE_RENDER_TARGET);
    volumetrics_target = r3_create_target((width + 1) / 2, (height + 1) / 2, GPU_FORMAT_RGBA16F, GPU_USAGE_RENDER_TARGET);
    taa_targets[0] = r3_create_target(width, height, GPU_FORMAT_RGBA16F, GPU_USAGE_RENDER_TARGET);
    taa_targets[1] = r3_create_target(width, height, GPU_FORMAT_RGBA16F, GPU_USAGE_RENDER_TARGET);
    final_target = r3_create_target(window_width, window_height, GPU_FORMAT_RGBA8, GPU_USAGE_RENDER_TARGET);

    bloom_level_count = 0;

    for (int i = 0; i < BLOOM_LEVELS; i++)
    {
        int w = width >> (i + 1);
        int h = height >> (i + 1);

        if (w < 4 || h < 4)
        {
            break;
        }

        bloom_targets[i] = r3_create_target(w, h, GPU_FORMAT_RGBA16F, GPU_USAGE_RENDER_TARGET);
        bloom_level_count++;
    }

    int pipeline_targets = pipeline == RENDER3D_FORWARD_PLUS ? forward_plus_create_targets() : deferred_plus_create_targets();

    if (!r3.depth_target || !r3.hdr_target || !ao_target || !ao_blur_target || !ldr_target || !sharpen_source || !volumetrics_target || !taa_targets[0] ||
        !taa_targets[1] || !final_target || !pipeline_targets)
    {
        destroy_targets();
        return 0;
    }

    return 1;
}

static int ensure_shadow_maps(void)
{
    if (shadow_map_size == r3.quality.shadow_resolution)
    {
        return shadow_maps[0] != NULL;
    }

    wait_renderer_idle();
    destroy_shadow_maps();

    for (int i = 0; i < R3_MAX_CASCADES; i++)
    {
        shadow_maps[i] = r3_create_target(r3.quality.shadow_resolution, r3.quality.shadow_resolution, GPU_FORMAT_D32, GPU_USAGE_DEPTH_TARGET);
    }

    shadow_map_size = r3.quality.shadow_resolution;

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

    for (int i = 0; i < r3.draw_count; i++)
    {
        const R3Draw* draw = &r3.draws[i];
        float extent = draw->radius >= 0.0f ? draw->radius : 200.0f;
        Vec3 center = draw->radius >= 0.0f ? vec3(draw->center[0], draw->center[1], draw->center[2]) :
            vec3(r3.instances[i].model[3], r3.instances[i].model[7], r3.instances[i].model[11]);
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

    float near_distance = r3.camera.near_plane;
    float far_distance = SDL_min(r3.camera.far_plane, r3.quality.shadow_distance);
    float previous = near_distance;

    for (int c = 0; c < count; c++)
    {
        float t = (float)(c + 1) / (float)count;
        float logarithmic = near_distance * SDL_powf(far_distance / near_distance, t);
        float uniform = near_distance + (far_distance - near_distance) * t;
        float split = logarithmic * 0.75f + uniform * 0.25f;

        float t0 = (previous - near_distance) / (r3.camera.far_plane - near_distance);
        float t1 = (split - near_distance) / (r3.camera.far_plane - near_distance);

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

        float texel = (2.0f * radius) / (float)r3.quality.shadow_resolution;

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

        previous = split;
    }
}

// Whether a draw's bounding sphere reaches into a cascade's (infinite toward the sun) cylinder.
static int draw_in_cascade(const R3Draw* draw, const Cascade* cascade)
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

// Against a frustum, its planes extracted from the view-projection (Gribb & Hartmann).
void r3_frustum_planes(const Mat4* view_projection, float planes[5][4])
{
    const float* m = view_projection->m;

    const float raw[5][4] = {
        { m[12] + m[0], m[13] + m[1], m[14] + m[2], m[15] + m[3] },
        { m[12] - m[0], m[13] - m[1], m[14] - m[2], m[15] - m[3] },
        { m[12] + m[4], m[13] + m[5], m[14] + m[6], m[15] + m[7] },
        { m[12] - m[4], m[13] - m[5], m[14] - m[6], m[15] - m[7] },
        { m[8], m[9], m[10], m[11] } // near : depth in [0, 1]
    };

    for (int i = 0; i < 5; i++)
    {
        float length = SDL_sqrtf(raw[i][0] * raw[i][0] + raw[i][1] * raw[i][1] + raw[i][2] * raw[i][2]);
        float scale = length > 0.0f ? 1.0f / length : 0.0f;

        for (int c = 0; c < 4; c++)
        {
            planes[i][c] = raw[i][c] * scale;
        }
    }
}

int r3_sphere_in_planes(const float planes[5][4], const float center[3], float radius)
{
    for (int i = 0; i < 5; i++)
    {
        if (planes[i][0] * center[0] + planes[i][1] * center[1] + planes[i][2] * center[2] + planes[i][3] < -radius)
        {
            return 0;
        }
    }

    return 1;
}

int r3_sphere_in_frustum(const float center[3], float radius, const Mat4* view_projection)
{
    float planes[5][4];
    r3_frustum_planes(view_projection, planes);
    return r3_sphere_in_planes(planes, center, radius);
}

static int draw_in_view(const R3Draw* draw, const Mat4* view_projection)
{
    return draw->radius < 0.0f || r3_sphere_in_frustum(draw->center, draw->radius, view_projection);
}

void r3_fullscreen_pass(Texture* target, GpuPipeline* p, const R3PassConstants* constants)
{
    GpuPassDesc pass;
    SDL_zero(pass);
    pass.colors[0].texture = target;
    pass.colors[0].load = GPU_LOAD_DISCARD;
    pass.color_count = 1;

    gpu_begin_pass(&pass);
    gpu_bind_pipeline(p);
    gpu_push_constants(constants, sizeof(*constants));
    gpu_draw(3);
    gpu_end_pass();
}

// The volumetric pass runs if there's a medium to light : it then replaces the analytic fog.
static int volumetrics_active(void)
{
    return r3.quality.volumetrics_enabled && r3.look.fog_density > 0.0f;
}

static void fill_frame(R3Frame* frame, const Mat4* view_projection, const Mat4* inverse_view_projection, const Mat4* projection, const Cascade* cascades)
{
    Mat4 inverse_projection = mat4_inverse(*projection);
    const Render3DQuality* quality = &r3.quality;
    const Render3DLook* look = &r3.look;

    SDL_zerop(frame);
    SDL_memcpy(frame->view_projection, view_projection->m, sizeof(frame->view_projection));
    SDL_memcpy(frame->inverse_view_projection, inverse_view_projection->m, sizeof(frame->inverse_view_projection));
    SDL_memcpy(frame->view, r3.camera.view.m, sizeof(frame->view));
    SDL_memcpy(frame->inverse_projection, inverse_projection.m, sizeof(frame->inverse_projection));

    frame->camera_position[0] = r3.camera.eye.x;
    frame->camera_position[1] = r3.camera.eye.y;
    frame->camera_position[2] = r3.camera.eye.z;

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

    frame->environment[0] = look->sky_intensity;
    frame->environment[1] = (float)environment->prefiltered_mips;
    frame->environment[2] = look->bounce_strength;

    frame->environment_textures[0] = gpu_texture_index(environment->panorama);
    frame->environment_textures[1] = gpu_texture_index(environment->prefiltered);
    frame->environment_textures[2] = gpu_texture_index(brdf_lut);
    frame->environment_textures[3] = quality->ao_enabled ? gpu_texture_index(ao_blur_target) : R3_NO_TEXTURE; // see render_ambient_occlusion()

    for (int c = 0; c < 3; c++)
    {
        frame->fog_inscatter[c] = look->fog_color[c];
        frame->fog_sun[c] = look->fog_sun_color[c];
    }

    frame->fog_inscatter[3] = look->fog_density;
    frame->fog_sun[3] = look->fog_height_falloff;
    frame->fog_params[0] = look->fog_base_height;
    frame->fog_params[1] = look->fog_sun_power;

    for (int c = 0; c < quality->shadow_cascades; c++)
    {
        SDL_memcpy(frame->shadow_view_projection[c], cascades[c].view_projection.m, sizeof(frame->shadow_view_projection[c]));
        frame->cascade_far[c] = cascades[c].far_distance;
        frame->shadow_textures[c] = gpu_texture_index(shadow_maps[c]);
        frame->shadow_texel_world[c] = cascades[c].texel_world;
        frame->shadow_depth_range[c] = cascades[c].depth_range;
    }

    frame->shadow_params[0] = 3.0f;
    frame->shadow_params[1] = (float)quality->shadow_samples;
    frame->shadow_params[2] = (float)quality->shadow_cascades;
    frame->shadow_params[3] = look->sun_softness;

    frame->resolution[0] = (float)r3.width;
    frame->resolution[1] = (float)r3.height;
    frame->resolution[2] = 1.0f / (float)r3.width;
    frame->resolution[3] = 1.0f / (float)r3.height;
    frame->jitter[2] = r3.quality.taa_enabled ? (float)(frame_number % 1024u) : 0.0f;

    frame->clouds[0] = SDL_clamp(look->cloud_coverage, 0.0f, 1.0f);
    frame->clouds[1] = SDL_max(0.0f, look->cloud_density);
    frame->clouds[2] = look->cloud_altitude;
    frame->clouds[3] = 1.0f / SDL_max(look->cloud_scale, 1.0f);
    frame->clouds_motion[0] = look->cloud_wind[0];
    frame->clouds_motion[1] = look->cloud_wind[1];
    frame->clouds_motion[2] = SDL_clamp(look->cloud_shadows, 0.0f, 1.0f);
    frame->clouds_motion[3] = (float)((SDL_GetTicks() - start_ticks) % 3600000u) / 1000.0f; // an hour, then over again

    if (has_probe_volume)
    {
        SDL_memcpy(frame->probe_origin, probe_volume.origin, sizeof(float) * 3);
        frame->probe_origin[3] = probe_volume.cell_size;
        frame->probe_grid[0] = (uint32_t)probe_volume.cells[0];
        frame->probe_grid[1] = (uint32_t)probe_volume.cells[1];
        frame->probe_grid[2] = (uint32_t)probe_volume.cells[2];
        frame->probe_grid[3] = gpu_buffer_index(probe_volume.buffer);
        frame->probe_layout[0] = probe_volume.table_offset;
        frame->probe_layout[1] = probe_volume.coarse_offset;
        frame->probe_layout[2] = probe_volume.bricks_offset;
    }

    frame->volumetrics[0] = volumetrics_active() ? 1.0f : 0.0f;
    frame->volumetrics[1] = SDL_clamp(look->volumetric_anisotropy, -0.95f, 0.95f);
    frame->volumetrics[2] = SDL_max(0.0f, look->volumetric_intensity);
}

// The Halton sequence's base-b radical inverse : well spread sub-pixel offsets, frame after frame.
static float halton(uint32_t index, uint32_t base)
{
    float result = 0.0f;
    float f = 1.0f;

    while (index > 0)
    {
        f /= (float)base;
        result += f * (float)(index % base);
        index /= base;
    }

    return result;
}

static void render_shadows(R3PassConstants* constants, const Cascade* cascades)
{
    for (int c = 0; c < r3.quality.shadow_cascades; c++)
    {
        GpuPassDesc pass;
        SDL_zero(pass);
        pass.depth = shadow_maps[c];
        pass.depth_load = GPU_LOAD_CLEAR;
        pass.depth_clear = 1.0f;

        gpu_begin_pass(&pass);

        constants->inputs[0] = (uint32_t)c;

        // Opaque casters, then alpha-masked ones : each pipeline is bound once.
        for (int masked = 0; masked <= 1; masked++)
        {
            gpu_bind_pipeline(masked ? shadow_masked_pipeline : shadow_pipeline);

            for (int i = 0; i < r3.draw_count; i++)
            {
                if (r3_is_masked(i) == masked && draw_in_cascade(&r3.draws[i], &cascades[c]))
                {
                    constants->ids[3] = (uint32_t)i;
                    gpu_push_constants(constants, sizeof(*constants));
                    gpu_draw_indexed(r3.draws[i].mesh, r3.draws[i].first_index, r3.draws[i].index_count);
                }
            }
        }

        gpu_end_pass();
    }
}

// GTAO from the depth the pipeline's first pass wrote, then blurred both ways ; the result ends up in
// ao_blur_target, where the frame data points.
static void render_ambient_occlusion(R3PassConstants* constants)
{
    SDL_zero(constants->inputs);
    constants->inputs[0] = gpu_texture_index(r3.scene_depth);
    constants->params[0] = r3.look.ao_radius;
    constants->params[1] = (float)r3.quality.ao_slices;
    constants->params[2] = (float)r3.quality.ao_steps;
    constants->params[3] = r3.look.ao_power;
    r3_fullscreen_pass(ao_blur_target, ao_pipeline, constants);

    constants->inputs[0] = gpu_texture_index(ao_blur_target);
    constants->inputs[1] = gpu_texture_index(r3.scene_depth);
    constants->params[0] = 1.0f;
    constants->params[1] = 0.0f;
    r3_fullscreen_pass(ao_target, ao_blur_pipeline, constants);

    constants->inputs[0] = gpu_texture_index(ao_target);
    constants->params[0] = 0.0f;
    constants->params[1] = 1.0f;
    r3_fullscreen_pass(ao_blur_target, ao_blur_pipeline, constants);
}

// The fog's medium at half resolution, then blended over the lit image (attenuated, then lit).
static void render_volumetrics(R3PassConstants* constants)
{
    SDL_zero(constants->inputs);
    SDL_zero(constants->params);
    constants->inputs[0] = gpu_texture_index(r3.scene_depth);
    constants->params[0] = (float)r3.quality.volumetric_steps;
    constants->params[1] = SDL_max(1.0f, r3.look.volumetric_distance);
    constants->params[2] = r3.quality.volumetric_local_lights ? 1.0f : 0.0f;
    constants->params[3] = 0.5f;
    r3_fullscreen_pass(volumetrics_target, volumetrics_pipeline, constants);

    GpuPassDesc pass;
    SDL_zero(pass);
    pass.colors[0].texture = r3.hdr_target;
    pass.colors[0].load = GPU_LOAD_KEEP;
    pass.color_count = 1;

    SDL_zero(constants->params);
    constants->inputs[0] = gpu_texture_index(volumetrics_target);
    constants->inputs[1] = gpu_texture_index(r3.scene_depth);

    gpu_begin_pass(&pass);
    gpu_bind_pipeline(volumetrics_composite_pipeline);
    gpu_push_constants(constants, sizeof(*constants));
    gpu_draw(3);
    gpu_end_pass();
}

// Bloom (a chain of downsamples, then back up, each level adding to the next), tone mapping, then
// anti-aliasing at the window's resolution.
static void render_post(R3PassConstants* constants)
{
    Texture* lit_image = r3.hdr_target;
    int bloom = r3.quality.bloom_enabled && bloom_level_count > 0;

    if (r3.quality.taa_enabled)
    {
        Texture* history = taa_targets[taa_current];
        taa_current ^= 1;

        SDL_zero(constants->inputs);
        SDL_zero(constants->params);
        constants->inputs[0] = gpu_texture_index(r3.hdr_target);
        constants->inputs[1] = gpu_texture_index(history);
        constants->inputs[2] = gpu_texture_index(r3.scene_depth);
        constants->params[0] = history_valid ? 1.0f : 0.0f;
        constants->params[1] = 0.1f;
        r3_fullscreen_pass(taa_targets[taa_current], taa_pipeline, constants);

        lit_image = taa_targets[taa_current];
        history_valid = 1;
    }

    if (bloom)
    {
        Texture* source = lit_image;
        int source_width = r3.width;
        int source_height = r3.height;

        for (int i = 0; i < bloom_level_count; i++)
        {
            SDL_zero(constants->inputs);
            constants->inputs[0] = gpu_texture_index(source);
            constants->params[0] = 1.0f / (float)source_width;
            constants->params[1] = 1.0f / (float)source_height;
            constants->params[2] = i == 0 ? 64.0f : 60000.0f;
            r3_fullscreen_pass(bloom_targets[i], bloom_down_pipeline, constants);

            source = bloom_targets[i];
            source_width = r3.width >> (i + 1);
            source_height = r3.height >> (i + 1);
        }

        for (int i = bloom_level_count - 1; i > 0; i--)
        {
            GpuPassDesc pass;
            SDL_zero(pass);
            pass.colors[0].texture = bloom_targets[i - 1];
            pass.colors[0].load = GPU_LOAD_KEEP;
            pass.color_count = 1;

            SDL_zero(constants->inputs);
            constants->inputs[0] = gpu_texture_index(bloom_targets[i]);
            constants->params[0] = 1.0f / (float)(r3.width >> (i + 1));
            constants->params[1] = 1.0f / (float)(r3.height >> (i + 1));

            gpu_begin_pass(&pass);
            gpu_bind_pipeline(bloom_up_pipeline);
            gpu_push_constants(constants, sizeof(*constants));
            gpu_draw(3);
            gpu_end_pass();
        }
    }

    SDL_zero(constants->inputs);
    constants->inputs[0] = gpu_texture_index(lit_image);
    constants->inputs[1] = bloom ? gpu_texture_index(bloom_targets[0]) : R3_NO_TEXTURE;

    // Automatic exposure : middle grey for the environment's average brightness.
    float average = SDL_max(environment->average_luminance * r3.look.sky_intensity, 1e-3f);
    constants->params[0] = (0.22f / average) * SDL_powf(2.0f, r3.look.exposure_ev);
    constants->params[1] = r3.look.bloom_strength;
    constants->params[2] = (float)r3.look.tonemapper;
    constants->params[3] = r3.look.contrast;
    constants->params2[0] = r3.look.saturation;
    constants->params2[1] = r3.look.vignette;
    r3_fullscreen_pass(ldr_target, tonemap_pipeline, constants);

    int sharpen = r3.quality.sharpness > 0.0f;

    SDL_zero(constants->inputs);
    constants->inputs[0] = gpu_texture_index(ldr_target);
    constants->params[0] = 1.0f / (float)r3.width;
    constants->params[1] = 1.0f / (float)r3.height;
    constants->params[2] = r3.quality.fxaa_enabled ? 1.0f : 0.0f;
    r3_fullscreen_pass(sharpen ? sharpen_source : final_target, fxaa_pipeline, constants);

    if (sharpen)
    {
        constants->inputs[0] = gpu_texture_index(sharpen_source);
        constants->params[2] = r3.quality.sharpness;
        r3_fullscreen_pass(final_target, sharpen_pipeline, constants);
    }
}

void render3d_render(int width, int height)
{
    if (!created || !environment || width <= 0 || height <= 0)
    {
        return;
    }

    int internal_width = SDL_max(1, (int)((float)width * r3.quality.render_scale));
    int internal_height = SDL_max(1, (int)((float)height * r3.quality.render_scale));

    // Before anything is recorded : these may wait for the GPU and recreate resources.
    if (!ensure_targets(internal_width, internal_height, width, height) || !ensure_shadow_maps())
    {
        return;
    }

    GpuFrameAllocation frame_memory = gpu_frame_alloc(sizeof(R3Frame));
    GpuFrameAllocation instance_memory = gpu_frame_alloc(sizeof(R3Instance) * (size_t)SDL_max(1, r3.draw_count));

    if (!frame_memory.mapped || !instance_memory.mapped)
    {
        printf("Out of frame memory : skipping the 3D passes.\n");
        return;
    }

    Mat4 view_projection = mat4_multiply(r3.camera.projection, r3.camera.view);
    Mat4 inverse_view_projection = mat4_inverse(view_projection);

    Cascade cascades[R3_MAX_CASCADES];
    compute_cascades(cascades, r3.quality.shadow_cascades, &inverse_view_projection);

    // With TAA, the camera moves by a sub-pixel offset every frame : a translation in clip space, the
    // same for perspective and orthographic projections.
    frame_number++;
    Mat4 projection = r3.camera.projection;
    float jitter[2] = { 0.0f, 0.0f };

    if (r3.quality.taa_enabled)
    {
        uint32_t index = (frame_number % 8u) + 1u;
        jitter[0] = (halton(index, 2) - 0.5f) * 2.0f / (float)r3.width;
        jitter[1] = (halton(index, 3) - 0.5f) * 2.0f / (float)r3.height;

        Mat4 offset = mat4_identity();
        offset.m[3] = jitter[0];
        offset.m[7] = jitter[1];
        projection = mat4_multiply(offset, projection);
    }

    Mat4 jittered_view_projection = mat4_multiply(projection, r3.camera.view);
    Mat4 inverse_jittered = mat4_inverse(jittered_view_projection);

    R3Frame* frame = frame_memory.mapped;
    fill_frame(frame, &jittered_view_projection, &inverse_jittered, &projection, cascades);

    Mat4 reprojection = mat4_multiply(previous_view_projection, inverse_view_projection);
    SDL_memcpy(frame->reprojection, reprojection.m, sizeof(frame->reprojection));
    frame->jitter[0] = jitter[0];
    frame->jitter[1] = jitter[1];
    previous_view_projection = view_projection;
    SDL_memcpy(instance_memory.mapped, r3.instances, sizeof(R3Instance) * (size_t)r3.draw_count);

    if (!r3_lights_prepare(frame, &view_projection))
    {
        return;
    }

    int gi = r3_gi_prepare(frame);

    // The reflection probes, smallest box first : a room's own probe wins over the hall's around it.
    if (r3.reflection_probe_count > 0)
    {
        GpuFrameAllocation probes_memory = gpu_frame_alloc(sizeof(R3ReflectionProbe) * (size_t)r3.reflection_probe_count);

        if (probes_memory.mapped)
        {
            int order[R3_MAX_REFLECTION_PROBES];
            float volumes[R3_MAX_REFLECTION_PROBES];

            for (int i = 0; i < r3.reflection_probe_count; i++)
            {
                const Render3DReflectionProbe* p = &r3.reflection_probes[i];
                volumes[i] = (p->box_max[0] - p->box_min[0]) * (p->box_max[1] - p->box_min[1]) * (p->box_max[2] - p->box_min[2]);
                order[i] = i;

                for (int j = i; j > 0 && volumes[order[j]] < volumes[order[j - 1]]; j--)
                {
                    int swap = order[j];
                    order[j] = order[j - 1];
                    order[j - 1] = swap;
                }
            }

            R3ReflectionProbe* out = probes_memory.mapped;

            for (int i = 0; i < r3.reflection_probe_count; i++)
            {
                const Render3DReflectionProbe* p = &r3.reflection_probes[order[i]];
                uint32_t texture = gpu_texture_index(p->environment->prefiltered);

                SDL_memcpy(out[i].position, p->position, sizeof(float) * 3);
                out[i].position[3] = SDL_max(p->blend, 0.01f);
                SDL_memcpy(out[i].box_min, p->box_min, sizeof(float) * 3);
                SDL_memcpy(&out[i].box_min[3], &texture, sizeof(texture)); // read back with asuint()
                SDL_memcpy(out[i].box_max, p->box_max, sizeof(float) * 3);
                out[i].box_max[3] = (float)p->environment->prefiltered_mips;
            }

            frame->reflection_probes[0] = (uint32_t)r3.reflection_probe_count;
            frame->reflection_probes[1] = probes_memory.offset;
        }
    }

    for (int i = 0; i < r3.draw_count; i++)
    {
        r3.in_view[i] = (uint8_t)(!r3.draws[i].hidden && draw_in_view(&r3.draws[i], &view_projection));
    }

    R3PassConstants constants;
    SDL_zero(constants);
    constants.ids[0] = frame_memory.buffer_index;
    constants.ids[1] = frame_memory.offset;
    constants.ids[2] = instance_memory.offset;

    render_shadows(&constants, cascades);
    r3_lights_render_shadows(&constants);

    if (gi)
    {
        r3_gi_update(&constants);
    }

    if (pipeline == RENDER3D_FORWARD_PLUS)
    {
        forward_plus_geometry(&constants);
    }
    else
    {
        deferred_plus_geometry(&constants);
    }

    if (r3.quality.ao_enabled)
    {
        render_ambient_occlusion(&constants);
    }

    SDL_zero(constants.inputs);
    SDL_zero(constants.params);
    SDL_zero(constants.params2);

    if (pipeline == RENDER3D_FORWARD_PLUS)
    {
        forward_plus_lighting(&constants);
    }
    else
    {
        deferred_plus_lighting(&constants);
    }

    if (volumetrics_active())
    {
        render_volumetrics(&constants);
    }

    render_post(&constants);
}

void render3d_present(void)
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
