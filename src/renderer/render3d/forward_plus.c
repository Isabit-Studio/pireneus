// Forward+ (shaders in forward_plus.slang) : a depth prepass, then each visible object shaded where it's
// drawn, with depth testing against the prepass rejecting every hidden pixel before its shader runs, and
// the sky behind everything. Both passes run the same vertex shader, so their depths match exactly.
//
// With MSAA, both passes draw into multisampled targets, which shaders then resolve : the closest sample
// of each pixel becomes the depth the screen-space passes read, and the shaded samples are averaged into
// the HDR image in a tone-mapped space (render3d.slang).

#include "renderer/render3d/render3d_internal.h"

#include <SDL3/SDL.h>

typedef struct
{
    GpuPipeline* depth;        // opaque surfaces
    GpuPipeline* depth_masked; // alpha-masked ones
    GpuPipeline* forward;
    GpuPipeline* sky;
} Pipelines;

static Pipelines single;      // created with the renderer
static Pipelines multisampled; // with the targets, for their sample count
static GpuPipeline* resolve_depth_pipeline;
static GpuPipeline* resolve_color_pipeline;

static Texture* msaa_color;
static Texture* msaa_depth;
static Texture* resolved_depth;

static int create_pipelines(Pipelines* out, int samples)
{
    GpuPipelineDesc desc;
    SDL_zero(desc);
    desc.vertex_shader = "fp_forward_vs";
    desc.depth_format = GPU_FORMAT_D32;
    desc.depth_test = 1;
    desc.depth_write = 1;
    desc.samples = samples;
    out->depth = gpu_create_pipeline(&desc);

    desc.fragment_shader = "fp_depth_masked_ps";
    out->depth_masked = gpu_create_pipeline(&desc);

    // The prepass already wrote the depth : this one only tests against it.
    SDL_zero(desc);
    desc.vertex_shader = "fp_forward_vs";
    desc.fragment_shader = "fp_forward_ps";
    desc.color_formats[0] = GPU_FORMAT_RGBA16F;
    desc.color_count = 1;
    desc.depth_format = GPU_FORMAT_D32;
    desc.depth_test = 1;
    desc.samples = samples;
    out->forward = gpu_create_pipeline(&desc);

    desc.vertex_shader = "r3_sky_vs";
    desc.fragment_shader = "r3_sky_ps";
    out->sky = gpu_create_pipeline(&desc);

    return out->depth && out->depth_masked && out->forward && out->sky;
}

static void destroy_pipelines(Pipelines* pipelines)
{
    GpuPipeline** all[4] = { &pipelines->depth, &pipelines->depth_masked, &pipelines->forward, &pipelines->sky };

    for (int i = 0; i < 4; i++)
    {
        if (*all[i])
        {
            gpu_destroy_pipeline(*all[i]);
            *all[i] = NULL;
        }
    }
}

int forward_plus_create(void)
{
    resolve_depth_pipeline = r3_screen_pipeline("r3_resolve_depth_ps", GPU_FORMAT_R32F, GPU_BLEND_NONE);
    resolve_color_pipeline = r3_screen_pipeline("r3_resolve_color_ps", GPU_FORMAT_RGBA16F, GPU_BLEND_NONE);

    return create_pipelines(&single, 1) && resolve_depth_pipeline && resolve_color_pipeline;
}

void forward_plus_destroy(void)
{
    destroy_pipelines(&single);
    destroy_pipelines(&multisampled);

    if (resolve_depth_pipeline)
    {
        gpu_destroy_pipeline(resolve_depth_pipeline);
    }

    if (resolve_color_pipeline)
    {
        gpu_destroy_pipeline(resolve_color_pipeline);
    }

    resolve_depth_pipeline = resolve_color_pipeline = NULL;
}

static Texture* create_multisampled(GpuFormat format, unsigned usage)
{
    GpuTextureDesc desc;
    SDL_zero(desc);
    desc.width = r3.width;
    desc.height = r3.height;
    desc.mip_count = 1;
    desc.format = format;
    desc.usage = GPU_USAGE_SAMPLED | usage;
    desc.samples = r3.msaa_samples;
    return gpu_create_texture(&desc);
}

int forward_plus_create_targets(void)
{
    if (r3.msaa_samples <= 1)
    {
        return 1; // the shared depth and HDR targets are all it draws into
    }

    msaa_color = create_multisampled(GPU_FORMAT_RGBA16F, GPU_USAGE_RENDER_TARGET);
    msaa_depth = create_multisampled(GPU_FORMAT_D32, GPU_USAGE_DEPTH_TARGET);
    resolved_depth = r3_create_target(r3.width, r3.height, GPU_FORMAT_R32F, GPU_USAGE_RENDER_TARGET);
    r3.scene_depth = resolved_depth;

    return msaa_color && msaa_depth && resolved_depth && create_pipelines(&multisampled, r3.msaa_samples);
}

void forward_plus_destroy_targets(void)
{
    r3_destroy_target(&msaa_color);
    r3_destroy_target(&msaa_depth);
    r3_destroy_target(&resolved_depth);
    destroy_pipelines(&multisampled);
}

// Opaque surfaces first, then alpha-masked ones : each pipeline is bound once.
static void draw_visible(R3PassConstants* constants, GpuPipeline* opaque, GpuPipeline* masked)
{
    for (int pass = 0; pass < 2; pass++)
    {
        gpu_bind_pipeline(pass == 0 ? opaque : masked);

        for (int i = 0; i < r3.draw_count; i++)
        {
            if (r3.in_view[i] && r3_is_masked(i) == pass)
            {
                constants->ids[3] = (uint32_t)i;
                gpu_push_constants(constants, sizeof(*constants));
                gpu_draw_indexed(r3.draws[i].mesh, r3.draws[i].first_index, r3.draws[i].index_count);
            }
        }
    }
}

void forward_plus_geometry(R3PassConstants* constants)
{
    int msaa = r3.msaa_samples > 1;
    const Pipelines* pipelines = msaa ? &multisampled : &single;

    GpuPassDesc pass;
    SDL_zero(pass);
    pass.depth = msaa ? msaa_depth : r3.depth_target;
    pass.depth_load = GPU_LOAD_CLEAR;
    pass.depth_clear = 1.0f;

    gpu_begin_pass(&pass);
    draw_visible(constants, pipelines->depth, pipelines->depth_masked);
    gpu_end_pass();

    if (msaa)
    {
        SDL_zero(constants->inputs);
        constants->inputs[0] = gpu_texture_index(msaa_depth);
        constants->params[0] = (float)r3.msaa_samples;
        r3_fullscreen_pass(resolved_depth, resolve_depth_pipeline, constants);
    }
}

void forward_plus_lighting(R3PassConstants* constants)
{
    int msaa = r3.msaa_samples > 1;
    const Pipelines* pipelines = msaa ? &multisampled : &single;

    // Every pixel is either covered by a surface or left at the far plane for the sky : nothing to clear.
    GpuPassDesc pass;
    SDL_zero(pass);
    pass.colors[0].texture = msaa ? msaa_color : r3.hdr_target;
    pass.colors[0].load = GPU_LOAD_DISCARD;
    pass.color_count = 1;
    pass.depth = msaa ? msaa_depth : r3.depth_target;
    pass.depth_load = GPU_LOAD_KEEP;

    gpu_begin_pass(&pass);
    draw_visible(constants, pipelines->forward, pipelines->forward);

    gpu_bind_pipeline(pipelines->sky);
    gpu_push_constants(constants, sizeof(*constants));
    gpu_draw(3);

    gpu_end_pass();

    if (msaa)
    {
        SDL_zero(constants->inputs);
        constants->inputs[0] = gpu_texture_index(msaa_color);
        constants->params[0] = (float)r3.msaa_samples;
        r3_fullscreen_pass(r3.hdr_target, resolve_color_pipeline, constants);
    }
}
