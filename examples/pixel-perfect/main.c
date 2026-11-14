#include <stdio.h>

#include <SDL3/SDL.h>

#include "platforms/platforms_window.h"
#include "renderer/renderer.h"
#include "renderer/sprite_batch.h"
#include "tiled/tiled_map.h"
#include "tiled/tiled_map_render.h"
#include "debug.h"
#include "example.h"

// Fixed internal *vertical* resolution the whole scene is drawn at ("virtual pixels") : the
// integer zoom is derived from height alone, and centered with thin black letterbox bars
// (top/bottom only, from the leftover sub-scale remainder) so every virtual pixel lands on an
// exact NxN block of real pixels, whatever the window's actual size, and the pixel art never
// blurs or shimmers while resizing. The *visible width*, in contrast, adapts to the window's
// actual aspect ratio at that same integer scale (see `visible_width`) : a wider window reveals
// more of the map instead of pillarboxing, the way many pixel-art games handle ultrawide/narrow
// windows.
#define VIRTUAL_HEIGHT 216

static TiledMap* map;
static Texture* tileset_texture;
static int tileset_width;
static int tileset_height;
static int tile_width;
static int tile_height;
static float orbit_src_x;
static float orbit_src_y;
static Uint64 start_ticks;

static int pixel_scale;
static int visible_width; // virtual pixels of world visible horizontally this frame ; adapts to the window's aspect ratio
static float pixel_offset_x;
static float pixel_offset_y;

// The whole scene is drawn once, at exact 1:1 virtual-pixel resolution, into this offscreen
// target ; the window pass then draws it as a single scaled quad. This is what makes the
// upscale genuinely pixel-perfect : every sprite/tile is rasterized at its native resolution
// (no per-draw-call scaling arithmetic to accumulate rounding error between them), and the one
// hardware upscale of the finished frame is guaranteed uniform (nearest-neighbour sampling,
// same integer factor everywhere), instead of many independently-scaled draws that could each
// round to a slightly different real-pixel footprint.
static RenderTarget* render_target;
static int render_target_capacity_width; // allocated width, in virtual pixels ; see update_render_target()

// Bounces `x` back and forth across [0, max_value], so the camera visibly pans across the
// whole map over time without needing keyboard input to test the sprite batcher/tilemap.
static float triangle_wave(float x, float max_value)
{
    if (max_value <= 0.0f)
    {
        return 0.0f;
    }

    float period = max_value * 2.0f;
    float phase = SDL_fmodf(x, period);

    if (phase < 0.0f)
    {
        phase += period;
    }

    return phase <= max_value ? phase : period - phase;
}

// tiled_map_draw() callback: draws each visible tile straight to the sprite batch, at native
// 1:1 resolution into the render target (no scaling here — see render_target above).
static void draw_tile(Texture* tileset_texture, int tileset_width, int tileset_height,
    float dest_x, float dest_y, float dest_w, float dest_h,
    float src_x, float src_y, float src_w, float src_h, void* user_data)
{
    (void)user_data;

    sprite_batch_draw_region(tileset_texture, tileset_width, tileset_height,
        dest_x, dest_y, dest_w, dest_h, src_x, src_y, src_w, src_h, color_white());
}

// (Re)creates the offscreen render target only when `visible_width` outgrows its current
// capacity, and only ever grows (quantized to 256-virtual-pixel steps, never shrinks). Only the
// (0,0)-(visible_width,VIRTUAL_HEIGHT) corner of it is ever drawn into or sampled from, so extra
// capacity is otherwise just unused (cleared/black) margin.
//
// Recreating a GPU resource on literally every resize event (matching capacity exactly to
// visible_width) turned out to be too aggressive : dragging the window border fires many resize
// events per second, each one synchronously destroying and recreating an image, RTV, SRV and
// descriptor set (and waiting on the GPU to be idle first) — dozens of times a second. That
// churn was crashing the DX12 backend under a fast drag. Reallocating only occasionally, in
// bigger steps, keeps that (already synchronous, see renderer.h) resource churn rare enough to
// never be a problem in practice, on either backend.
static void update_render_target(void)
{
    int width = SDL_max(1, visible_width);

    if (render_target && width <= render_target_capacity_width)
    {
        return;
    }

    if (render_target)
    {
        // The window pass's previous blit of this target may still be in flight on the GPU
        // (end_frame() doesn't wait for completion) ; wait here before freeing it, same as the
        // engine already does before recreating the swapchain on resize.
        wait_renderer_idle();
        destroy_render_target(render_target);
    }

    int new_capacity = ((width + 255) / 256) * 256;

    render_target = create_render_target(new_capacity, VIRTUAL_HEIGHT);
    render_target_capacity_width = new_capacity;

    DEBUG_LOG("[pixel-perfect] render target capacity: %dx%d (needed %dx%d)\n",
        new_capacity, VIRTUAL_HEIGHT, width, VIRTUAL_HEIGHT);
}

// Recomputes the integer zoom level (from height alone) and the resulting visible width, plus
// the letterbox offset, for the current window size : the largest integer multiple of
// VIRTUAL_HEIGHT that still fits, so every virtual pixel lands exactly on a whole number of real
// pixels (no blurring, no partial-pixel seams). The visible width is then whatever that same
// integer scale reveals of the window's actual width, so it adapts continuously instead of
// being pillarboxed to a fixed value.
static void update_pixel_scale(int window_width, int window_height)
{
    int new_scale = SDL_max(1, window_height / VIRTUAL_HEIGHT);

    if (new_scale != pixel_scale)
    {
        pixel_scale = new_scale;
        DEBUG_LOG("[pixel-perfect] scale: %dx (window %dx%d)\n", pixel_scale, window_width, window_height);
    }

    visible_width = window_width / pixel_scale;

    pixel_offset_x = (float)((window_width - visible_width * pixel_scale) / 2);
    pixel_offset_y = (float)((window_height - VIRTUAL_HEIGHT * pixel_scale) / 2);
}

// Before begin_frame() : the offscreen pass is its own synchronous unit of GPU work.
static void update(void)
{
    int window_width = 0;
    int window_height = 0;
    get_window_size(&window_width, &window_height);

    if (window_width <= 0 || window_height <= 0)
    {
        return;
    }

    update_pixel_scale(window_width, window_height);
    update_render_target();

    if (!render_target)
    {
        return;
    }

    float map_pixel_width = (float)(tiled_map_width(map) * tile_width);
    float map_pixel_height = (float)(tiled_map_height(map) * tile_height);
    float max_camera_x = SDL_max(0.0f, map_pixel_width - (float)visible_width);
    float max_camera_y = SDL_max(0.0f, map_pixel_height - (float)VIRTUAL_HEIGHT);

    float t = (float)(SDL_GetTicks() - start_ticks) / 1000.0f;

    // Floored to a whole virtual pixel : the render target is native, low resolution (e.g. 216
    // px tall), so any sub-pixel position here would leave a partially-covered row of blended
    // pixels at that tile/sprite's edge (from the rasterizer's coverage-based alpha blending) ;
    // nearest-upscaling then blows that thin blended row up by the zoom factor, which is what
    // reads as "blur" across the image. Whole-pixel positions keep every edge a hard cut, so the
    // upscale stays crisp (this is also why pixel-art games visibly move in whole-pixel steps
    // rather than smoothly interpolating sub-pixel positions).
    float camera_x = SDL_floorf(triangle_wave(t * 60.0f, max_camera_x));
    float camera_y = SDL_floorf(triangle_wave(t * 37.0f + 5.0f, max_camera_y));

    // Pass 1: draw the whole scene at native 1:1 resolution into the offscreen render target.
    if (begin_render_target_pass(render_target))
    {
        sprite_batch_begin();

        tiled_map_draw(map, tileset_texture, tileset_width, tileset_height, camera_x, camera_y, visible_width, VIRTUAL_HEIGHT, draw_tile, NULL);

        // An orbiting sprite in virtual-canvas space, to check that a moving/animated sprite
        // also stays pixel-aligned at every integer zoom level.
        float angle = t * 2.0f;
        float orbit_x = SDL_floorf(visible_width / 2.0f + SDL_cosf(angle) * 60.0f - 8.0f);
        float orbit_y = SDL_floorf(VIRTUAL_HEIGHT / 2.0f + SDL_sinf(angle) * 60.0f - 8.0f);

        sprite_batch_draw_region(tileset_texture, tileset_width, tileset_height,
            orbit_x, orbit_y, (float)tile_width, (float)tile_height,
            orbit_src_x, orbit_src_y, (float)tile_width, (float)tile_height, color_white());

        sprite_batch_end();
        end_render_target_pass();
    }
}

// Pass 2: blit the finished, exactly-virtual-resolution frame onto the window as a single
// scaled quad (nearest-neighbour sampling, uniform integer factor : this is the only place
// any upscaling happens).
static void render(void)
{
    if (!render_target)
    {
        return;
    }

    sprite_batch_begin();

    // Source rect is (0,0)-(visible_width,VIRTUAL_HEIGHT), not the full texture : the render
    // target's actual capacity can be wider than what's needed this frame (see update_render_target()).
    sprite_batch_draw_region(render_target_texture(render_target), render_target_capacity_width, VIRTUAL_HEIGHT,
        pixel_offset_x, pixel_offset_y, (float)(visible_width * pixel_scale), (float)(VIRTUAL_HEIGHT * pixel_scale),
        0.0f, 0.0f, (float)visible_width, (float)VIRTUAL_HEIGHT,
        color_white());

    sprite_batch_end();
}

static int init(void)
{
    map = tiled_map_load("resources/2D-test/sample_map.json");

    if (!map)
    {
        printf("Failed to load the test map.\n");
        return 0;
    }

    char tileset_path[512];
    SDL_snprintf(tileset_path, sizeof(tileset_path), "resources/2D-test/%s", tiled_map_tileset_image(map));

    tileset_texture = create_texture(tileset_path, &tileset_width, &tileset_height);

    if (!tileset_texture)
    {
        printf("Failed to load the tileset texture.\n");
        tiled_map_destroy(map);
        return 0;
    }

    tile_width = tiled_map_tile_width(map);
    tile_height = tiled_map_tile_height(map);

    // An arbitrary tile from the same tileset, drawn as an orbiting sprite.
    int orbit_tile_column = 4;
    int orbit_tile_row = 0;
    orbit_src_x = (float)(orbit_tile_column * (tile_width + tiled_map_tileset_spacing(map)));
    orbit_src_y = (float)(orbit_tile_row * (tile_height + tiled_map_tileset_spacing(map)));

    start_ticks = SDL_GetTicks();
    pixel_scale = 0; // forces update_pixel_scale() to log the initial scale on the first frame

    return 1;
}

static void shutdown_example(void)
{
    if (render_target)
    {
        destroy_render_target(render_target);
        render_target = NULL;
        render_target_capacity_width = 0;
    }

    destroy_texture(tileset_texture);
    tiled_map_destroy(map);
}

const Example example_pixel_perfect = {
    "pixel-perfect", "Pixel Perfect",
    "A low fixed resolution upscaled by a whole factor, whatever the window size.",
    init, update, render, shutdown_example
};
