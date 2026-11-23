// Deferred+ : The Forge's triangle visibility buffer (shaders in deferred_plus.slang). The first pass
// writes which triangle covers each pixel, and its depth ; one full-screen pass then shades every pixel
// exactly once, from that triangle's vertices.
//
// With screen-space reflections, the shading pass also writes each pixel's normal, roughness and
// specular terms ; the reflections are traced from them, then composited over the environment's.

#include "renderer/render3d/render3d_internal.h"

#include <SDL3/SDL.h>

static GpuPipeline* visibility_pipeline;
static GpuPipeline* shade_pipeline;
static GpuPipeline* shade_surface_pipeline; // with the reflections' targets
static GpuPipeline* reflections_pipeline;
static GpuPipeline* composite_pipeline;

static Texture* visibility_target; // RGBA8 : 12 bits of draw, 20 bits of triangle
static Texture* lit_target;        // with reflections : the lit image before them
static Texture* surface_target;    // normal, roughness, specular occlusion
static Texture* specular_target;   // the reflection's weight
static Texture* reflections_target;

int deferred_plus_create(void)
{
    GpuPipelineDesc desc;
    SDL_zero(desc);
    desc.vertex_shader = "r3_geometry_vs";
    desc.fragment_shader = "dp_visibility_ps";
    desc.color_formats[0] = GPU_FORMAT_RGBA8;
    desc.color_count = 1;
    desc.depth_format = GPU_FORMAT_D32;
    desc.depth_test = 1;
    desc.depth_write = 1;
    visibility_pipeline = gpu_create_pipeline(&desc);

    shade_pipeline = r3_screen_pipeline("dp_shade_ps", GPU_FORMAT_RGBA16F, GPU_BLEND_NONE);

    SDL_zero(desc);
    desc.vertex_shader = "r3_screen_vs";
    desc.fragment_shader = "dp_shade_surface_ps";
    desc.color_formats[0] = GPU_FORMAT_RGBA16F;
    desc.color_formats[1] = GPU_FORMAT_RGBA16F;
    desc.color_formats[2] = GPU_FORMAT_RGBA8;
    desc.color_count = 3;
    shade_surface_pipeline = gpu_create_pipeline(&desc);

    reflections_pipeline = r3_screen_pipeline("dp_reflections_ps", GPU_FORMAT_RGBA16F, GPU_BLEND_NONE);
    composite_pipeline = r3_screen_pipeline("dp_composite_ps", GPU_FORMAT_RGBA16F, GPU_BLEND_NONE);

    return visibility_pipeline && shade_pipeline && shade_surface_pipeline && reflections_pipeline && composite_pipeline;
}

void deferred_plus_destroy(void)
{
    GpuPipeline** pipelines[5] = { &visibility_pipeline, &shade_pipeline, &shade_surface_pipeline, &reflections_pipeline, &composite_pipeline };

    for (int i = 0; i < 5; i++)
    {
        if (*pipelines[i])
        {
            gpu_destroy_pipeline(*pipelines[i]);
            *pipelines[i] = NULL;
        }
    }
}

int deferred_plus_create_targets(void)
{
    visibility_target = r3_create_target(r3.width, r3.height, GPU_FORMAT_RGBA8, GPU_USAGE_RENDER_TARGET);

    if (!r3.ssr)
    {
        return visibility_target != NULL;
    }

    lit_target = r3_create_target(r3.width, r3.height, GPU_FORMAT_RGBA16F, GPU_USAGE_RENDER_TARGET);
    surface_target = r3_create_target(r3.width, r3.height, GPU_FORMAT_RGBA16F, GPU_USAGE_RENDER_TARGET);
    specular_target = r3_create_target(r3.width, r3.height, GPU_FORMAT_RGBA8, GPU_USAGE_RENDER_TARGET);
    reflections_target = r3_create_target(r3.width, r3.height, GPU_FORMAT_RGBA16F, GPU_USAGE_RENDER_TARGET);

    return visibility_target && lit_target && surface_target && specular_target && reflections_target;
}

void deferred_plus_destroy_targets(void)
{
    r3_destroy_target(&visibility_target);
    r3_destroy_target(&lit_target);
    r3_destroy_target(&surface_target);
    r3_destroy_target(&specular_target);
    r3_destroy_target(&reflections_target);
}

void deferred_plus_geometry(R3PassConstants* constants)
{
    GpuPassDesc pass;
    SDL_zero(pass);
    pass.colors[0].texture = visibility_target;
    pass.colors[0].load = GPU_LOAD_CLEAR;
    pass.colors[0].clear[0] = pass.colors[0].clear[1] = pass.colors[0].clear[2] = pass.colors[0].clear[3] = 1.0f;
    pass.color_count = 1;
    pass.depth = r3.depth_target;
    pass.depth_load = GPU_LOAD_CLEAR;
    pass.depth_clear = 1.0f;

    gpu_begin_pass(&pass);
    gpu_bind_pipeline(visibility_pipeline);

    for (int i = 0; i < r3.draw_count; i++)
    {
        if (r3.in_view[i])
        {
            constants->ids[3] = (uint32_t)i;
            gpu_push_constants(constants, sizeof(*constants));
            gpu_draw_indexed(r3.draws[i].mesh, r3.draws[i].first_index, r3.draws[i].index_count);
        }
    }

    gpu_end_pass();
}

void deferred_plus_lighting(R3PassConstants* constants)
{
    constants->inputs[0] = gpu_texture_index(visibility_target);

    if (!r3.ssr)
    {
        r3_fullscreen_pass(r3.hdr_target, shade_pipeline, constants);
        return;
    }

    GpuPassDesc pass;
    SDL_zero(pass);
    pass.colors[0].texture = lit_target;
    pass.colors[1].texture = surface_target;
    pass.colors[2].texture = specular_target;
    pass.colors[0].load = pass.colors[1].load = pass.colors[2].load = GPU_LOAD_DISCARD;
    pass.color_count = 3;

    gpu_begin_pass(&pass);
    gpu_bind_pipeline(shade_surface_pipeline);
    gpu_push_constants(constants, sizeof(*constants));
    gpu_draw(3);
    gpu_end_pass();

    SDL_zero(constants->inputs);
    SDL_zero(constants->params);
    constants->inputs[0] = gpu_texture_index(surface_target);
    constants->inputs[1] = gpu_texture_index(r3.scene_depth);
    constants->inputs[2] = gpu_texture_index(lit_target);
    constants->params[0] = (float)r3.quality.ssr_steps;
    constants->params[1] = r3.look.ssr_distance;
    constants->params[2] = r3.look.ssr_thickness;
    r3_fullscreen_pass(reflections_target, reflections_pipeline, constants);

    uint32_t depth_index = gpu_texture_index(r3.scene_depth);

    SDL_zero(constants->params);
    constants->inputs[0] = gpu_texture_index(lit_target);
    constants->inputs[1] = gpu_texture_index(reflections_target);
    constants->inputs[2] = gpu_texture_index(surface_target);
    constants->inputs[3] = gpu_texture_index(specular_target);
    SDL_memcpy(&constants->params[0], &depth_index, sizeof(depth_index)); // read back with asuint()
    r3_fullscreen_pass(r3.hdr_target, composite_pipeline, constants);
}
