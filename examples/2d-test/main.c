#include <stdio.h>

#include <SDL3/SDL.h>

#include "platforms/platforms_window.h"
#include "renderer/renderer.h"
#include "renderer/sprite_batch.h"
#include "tiled/tiled_map.h"
#include "tiled/tiled_map_render.h"
#include "debug.h"
#include "example.h"

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

// tiled_map_draw() callback: draws each visible tile straight to the sprite batch, in window
// pixel space (dest_* is already what sprite_batch_draw_region() expects).
static void draw_tile(Texture* tileset_texture, int tileset_width, int tileset_height,
    float dest_x, float dest_y, float dest_w, float dest_h,
    float src_x, float src_y, float src_w, float src_h, void* user_data)
{
    (void)user_data;

    sprite_batch_draw_region(tileset_texture, tileset_width, tileset_height,
        dest_x, dest_y, dest_w, dest_h, src_x, src_y, src_w, src_h, color_white());
}

static TiledMap* map;
static Texture* tileset_texture;
static int tileset_width;
static int tileset_height;
static int tile_width;
static int tile_height;
static float orbit_src_x;
static float orbit_src_y;
static Uint64 start_ticks;

static int last_window_width;
static int last_window_height;

static void render(void)
{
    int window_width = 0;
    int window_height = 0;
    get_window_size(&window_width, &window_height);

    if (window_width <= 0 || window_height <= 0)
    {
        return;
    }

    if (window_width != last_window_width || window_height != last_window_height)
    {
        DEBUG_LOG("[2d-test] window size: %dx%d\n", window_width, window_height);
        last_window_width = window_width;
        last_window_height = window_height;
    }

    float map_pixel_width = (float)(tiled_map_width(map) * tile_width);
    float map_pixel_height = (float)(tiled_map_height(map) * tile_height);
    float max_camera_x = SDL_max(0.0f, map_pixel_width - (float)window_width);
    float max_camera_y = SDL_max(0.0f, map_pixel_height - (float)window_height);

    float t = (float)(SDL_GetTicks() - start_ticks) / 1000.0f;
    float camera_x = triangle_wave(t * 60.0f, max_camera_x);
    float camera_y = triangle_wave(t * 37.0f + 5.0f, max_camera_y);

    sprite_batch_begin();

    tiled_map_draw(map, tileset_texture, tileset_width, tileset_height, camera_x, camera_y, window_width, window_height, draw_tile, NULL);

    float angle = t * 2.0f;
    float orbit_x = window_width / 2.0f + SDL_cosf(angle) * 120.0f - 16.0f;
    float orbit_y = window_height / 2.0f + SDL_sinf(angle) * 120.0f - 16.0f;

    sprite_batch_draw_region(tileset_texture, tileset_width, tileset_height,
        orbit_x, orbit_y, 32.0f, 32.0f,
        orbit_src_x, orbit_src_y, (float)tile_width, (float)tile_height, color_white());

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

    // An arbitrary tile from the same tileset, drawn as an orbiting sprite in screen space
    // (independent of the tilemap/camera), to exercise sprite_batch_draw_region() on its own.
    int orbit_tile_column = 4;
    int orbit_tile_row = 0;
    orbit_src_x = (float)(orbit_tile_column * (tile_width + tiled_map_tileset_spacing(map)));
    orbit_src_y = (float)(orbit_tile_row * (tile_height + tiled_map_tileset_spacing(map)));

    start_ticks = SDL_GetTicks();

    return 1;
}

static void shutdown_example(void)
{
    destroy_texture(tileset_texture);
    tiled_map_destroy(map);
}

const Example example_2d_test = {
    "2d-test", "2D Test",
    "A Tiled map panned by a camera, drawn through the sprite batch.",
    init, NULL, render, shutdown_example
};
