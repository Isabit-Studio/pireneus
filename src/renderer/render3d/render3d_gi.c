// Realtime global illumination, for both pipelines, where the GPU has ray tracing (shaders in
// realtime_gi.slang) : a grid of probes over what's drawn, updated every frame by ray queries, which the
// shading reads in place of the baked lighting.
//
// Per frame : the scene's acceleration structure ; a pass tracing every probe's rays (one texel each) ;
// a pass integrating them into each probe's spherical harmonics, blended over the previous frames'
// (ping-pong targets). The grid stays put while the scene fits in it ; moved, it keeps its history.

#include "renderer/render3d/render3d_internal.h"

#include <SDL3/SDL.h>
#include <stdio.h>

#include "debug.h"

#define GI_TEXTURE_WIDTH 128   // as in render3d_common.slang
#define MAX_PROBES 16384
#define MAX_PER_AXIS 48
#define HISTORY_KEPT 0.92f     // how much of the previous frames stays : smooth, and still quick to react

static GpuPipeline* trace_pipeline;
static GpuPipeline* update_pipeline;

static Texture* ray_target;                // rays across, probes down
static Texture* probe_targets[2][4];       // the current and previous frames' : red, green, blue, state
static int current;
static int history_valid;
static int grid[3];
static int rays;
static float origin[3];
static float spacing;
static float requested_spacing; // the quality's, before choose_grid() made it coarser
static int shift[3];             // how many cells the grid moved this frame
static uint32_t frame_counter;

static GpuRayInstance ray_instances[R3_MAX_DRAWS];

int r3_gi_create(void)
{
    if (!gpu_raytracing_supported())
    {
        return 1; // nothing to create : realtime GI stays off
    }

    trace_pipeline = r3_screen_pipeline("r3_gi_trace_ps", GPU_FORMAT_RGBA16F, GPU_BLEND_NONE);

    GpuPipelineDesc desc;
    SDL_zero(desc);
    desc.vertex_shader = "r3_screen_vs";
    desc.fragment_shader = "r3_gi_update_ps";

    for (int i = 0; i < 4; i++)
    {
        desc.color_formats[i] = GPU_FORMAT_RGBA16F;
    }

    desc.color_count = 4;
    update_pipeline = gpu_create_pipeline(&desc);

    if (!trace_pipeline || !update_pipeline)
    {
        printf("Could not create the realtime GI's pipelines : it stays off.\n");
    }

    return 1;
}

static void destroy_targets(void)
{
    r3_destroy_target(&ray_target);

    for (int f = 0; f < 2; f++)
    {
        for (int i = 0; i < 4; i++)
        {
            r3_destroy_target(&probe_targets[f][i]);
        }
    }

    grid[0] = grid[1] = grid[2] = 0;
    rays = 0;
    history_valid = 0;
}

void r3_gi_destroy(void)
{
    destroy_targets();

    if (trace_pipeline)
    {
        gpu_destroy_pipeline(trace_pipeline);
    }

    if (update_pipeline)
    {
        gpu_destroy_pipeline(update_pipeline);
    }

    trace_pipeline = update_pipeline = NULL;
}

int r3_gi_available(void)
{
    return trace_pipeline && update_pipeline;
}

// What the grid must cover : everything drawn (its boxes).
static void scene_bounds(float lo[3], float hi[3])
{
    lo[0] = lo[1] = lo[2] = 1e30f;
    hi[0] = hi[1] = hi[2] = -1e30f;

    for (int i = 0; i < r3.draw_count; i++)
    {
        const R3Draw* draw = &r3.draws[i];

        if (draw->radius < 0.0f)
        {
            continue;
        }

        for (int a = 0; a < 3; a++)
        {
            lo[a] = SDL_min(lo[a], draw->center[a] - draw->extent[a]);
            hi[a] = SDL_max(hi[a], draw->center[a] + draw->extent[a]);
        }
    }

    if (lo[0] > hi[0])
    {
        for (int a = 0; a < 3; a++)
        {
            lo[a] = -5.0f;
            hi[a] = 5.0f;
        }
    }
}

// The grid : over those bounds and a cell more on every side (things can move a little without the grid
// following), at the quality's spacing, coarser if it would be too big.
static void choose_grid(const float bounds_lo[3], const float bounds_hi[3], int out_grid[3], float out_origin[3], float* out_spacing)
{
    float s = SDL_max(0.25f, r3.quality.gi_spacing);
    float lo[3], hi[3];

    for (;;)
    {
        for (int a = 0; a < 3; a++)
        {
            lo[a] = bounds_lo[a] - s;
            hi[a] = bounds_hi[a] + s;
        }

        long long total = 1;
        int fits = 1;

        for (int a = 0; a < 3; a++)
        {
            out_grid[a] = SDL_clamp((int)SDL_ceilf((hi[a] - lo[a]) / s) + 2, 2, MAX_PER_AXIS);
            total *= out_grid[a];
            fits &= (float)(out_grid[a] - 2) * s >= hi[a] - lo[a] - s * 0.01f;
        }

        if (fits && total <= MAX_PROBES)
        {
            break;
        }

        s *= 1.2f;
    }

    // Snapped to the spacing : a grid that moves moves by whole cells, and keeps its history.
    for (int a = 0; a < 3; a++)
    {
        out_origin[a] = SDL_floorf((lo[a] - s * 0.5f) / s) * s;
    }

    *out_spacing = s;
}

// The grid stays where it is while what's drawn still fits in it, with the same settings : a moving
// object doesn't make it slide. When it must move, by whole cells, the probes keep their history
// (shifted). Only a new size starts again from nothing.
static int ensure_targets(void)
{
    float lo[3], hi[3];
    scene_bounds(lo, hi);
    int new_rays = SDL_clamp(r3.quality.gi_rays, 16, 256);
    float wanted_spacing = SDL_max(0.25f, r3.quality.gi_spacing);
    shift[0] = shift[1] = shift[2] = 0;

    if (ray_target && new_rays == rays && wanted_spacing == requested_spacing)
    {
        int inside = 1;

        for (int a = 0; a < 3; a++)
        {
            inside &= lo[a] >= origin[a] && hi[a] <= origin[a] + (float)(grid[a] - 1) * spacing;
        }

        if (inside)
        {
            return 1;
        }
    }

    int new_grid[3];
    float new_origin[3];
    float new_spacing;
    choose_grid(lo, hi, new_grid, new_origin, &new_spacing);

    if (ray_target && new_rays == rays && new_spacing == spacing && new_grid[0] == grid[0] && new_grid[1] == grid[1] && new_grid[2] == grid[2])
    {
        for (int a = 0; a < 3; a++)
        {
            shift[a] = (int)SDL_roundf((new_origin[a] - origin[a]) / spacing);
        }

        SDL_memcpy(origin, new_origin, sizeof(origin));
        requested_spacing = wanted_spacing;
        return 1;
    }

    wait_renderer_idle();
    destroy_targets();

    SDL_memcpy(grid, new_grid, sizeof(grid));
    SDL_memcpy(origin, new_origin, sizeof(origin));
    spacing = new_spacing;
    requested_spacing = wanted_spacing;
    rays = new_rays;

    int probes = grid[0] * grid[1] * grid[2];
    int rows = (probes + GI_TEXTURE_WIDTH - 1) / GI_TEXTURE_WIDTH;
    ray_target = r3_create_target(rays, probes, GPU_FORMAT_RGBA16F, GPU_USAGE_RENDER_TARGET);
    int ok = ray_target != NULL;

    for (int f = 0; f < 2; f++)
    {
        for (int i = 0; i < 4; i++)
        {
            probe_targets[f][i] = r3_create_target(GI_TEXTURE_WIDTH, rows, GPU_FORMAT_RGBA16F, GPU_USAGE_RENDER_TARGET);
            ok &= probe_targets[f][i] != NULL;
        }
    }

    if (!ok)
    {
        destroy_targets();
    }

    DEBUG_LOG("[render3d] realtime GI : %d x %d x %d probes, %.2f m apart, %d rays each%s\n", grid[0], grid[1], grid[2], (double)spacing, rays,
        ok ? "" : " : could not create its targets");

    return ok;
}

// Before any pass : the grid's targets (they may be recreated, which waits for the GPU), and the frame's
// fields, pointing at the probes this frame writes.
int r3_gi_prepare(R3Frame* frame)
{
    if (!r3_gi_available() || !r3.quality.gi_enabled || r3.draw_count == 0)
    {
        history_valid = 0;
        return 0;
    }

    if (!ensure_targets())
    {
        return 0;
    }

    SDL_memcpy(frame->gi_origin, origin, sizeof(origin));
    frame->gi_origin[3] = spacing;
    frame->gi_grid[0] = (uint32_t)grid[0];
    frame->gi_grid[1] = (uint32_t)grid[1];
    frame->gi_grid[2] = (uint32_t)grid[2];
    frame->gi_grid[3] = (uint32_t)rays;

    int next = current ^ 1;

    for (int i = 0; i < 4; i++)
    {
        frame->gi_textures[i] = gpu_texture_index(probe_targets[next][i]);
    }

    return 1;
}

void r3_gi_update(R3PassConstants* constants)
{
    if (!ray_target)
    {
        return;
    }

    for (int i = 0; i < r3.draw_count; i++)
    {
        ray_instances[i].mesh = r3.draws[i].mesh;
        ray_instances[i].first_index = r3.draws[i].first_index;
        ray_instances[i].index_count = r3.draws[i].index_count;
        SDL_memcpy(ray_instances[i].transform, r3.instances[i].model, sizeof(ray_instances[i].transform));
        ray_instances[i].alpha_tested = 0;
    }

    uint32_t tlas = gpu_build_ray_scene(ray_instances, r3.draw_count);
    int next = current ^ 1;
    frame_counter++;

    // The previous probes : the bounces, and the history.
    SDL_zero(constants->inputs);
    SDL_zero(constants->params);

    for (int i = 0; i < 4; i++)
    {
        constants->inputs[i] = gpu_texture_index(probe_targets[current][i]);
    }

    constants->ids[3] = tlas;
    constants->params[0] = (float)(frame_counter % 4096u);
    constants->params[1] = (float)r3.quality.gi_lights;
    constants->params[2] = history_valid ? 1.0f : 0.0f;

    // Where the history's probes were : shift cells back.
    for (int a = 0; a < 3; a++)
    {
        constants->params2[a] = (float)shift[a];
    }

    if (!history_valid)
    {
        // Nothing to bounce yet : the history reads as dark, valid probes.
        GpuPassDesc clear;
        SDL_zero(clear);

        for (int i = 0; i < 4; i++)
        {
            clear.colors[i].texture = probe_targets[current][i];
            clear.colors[i].load = GPU_LOAD_CLEAR;
            clear.colors[i].clear[0] = i == 3 ? 1.0f : 0.0f;
        }

        clear.color_count = 4;
        gpu_begin_pass(&clear);
        gpu_end_pass();
    }

    r3_fullscreen_pass(ray_target, trace_pipeline, constants);

    uint32_t ray_index = gpu_texture_index(ray_target);
    SDL_memcpy(&constants->params[2], &ray_index, sizeof(ray_index)); // read back with asuint()
    constants->params[1] = history_valid ? HISTORY_KEPT : 0.0f;
    constants->params[3] = 0.0f;

    GpuPassDesc pass;
    SDL_zero(pass);

    for (int i = 0; i < 4; i++)
    {
        pass.colors[i].texture = probe_targets[next][i];
        pass.colors[i].load = GPU_LOAD_DISCARD;
    }

    pass.color_count = 4;

    gpu_begin_pass(&pass);
    gpu_bind_pipeline(update_pipeline);
    gpu_push_constants(constants, sizeof(*constants));
    gpu_draw(3);
    gpu_end_pass();

    current = next;
    history_valid = 1;
    constants->ids[3] = 0;
    SDL_zero(constants->params);
    SDL_zero(constants->params2);
}
