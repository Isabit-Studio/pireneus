// The 2D renderer (render2d.h). Sprites are sorted back to front, then batched : consecutive sprites
// sharing a texture go out in one draw_vertices() call.

#include "renderer/render2d/render2d.h"

#include <SDL3/SDL.h>
#include <stdlib.h>

#define BATCH_SPRITES 512
#define TARGET_WIDTH_STEP 64 // the pixel-perfect target only grows, by this much : no reallocation per pixel of a resize

typedef struct
{
    float corners[4][3]; // world space : top-left, top-right, bottom-right, bottom-left
    Texture* texture;
    float uv[4];
    float color[4];
    float z;
    int order;           // ties keep the order they were added in
} Sprite;

static Sprite* sprites;
static int sprite_count;
static int sprite_capacity;

static Render2DCamera camera;
static Render2DSettings settings;

static RenderTarget* target;
static int target_capacity_width;
static int target_capacity_height;
static int target_ready;  // the pixel-perfect image of this frame exists
static int target_used[2]; // the part of the target drawn into
static int target_scale;
static float target_origin[2]; // where its top-left corner goes in the window

static Vertex batch[BATCH_SPRITES * 6];
static int batch_count;
static Texture* batch_texture;

void render2d_default_settings(Render2DSettings* out_settings)
{
    SDL_zerop(out_settings);
    out_settings->pixel_perfect = 0;
    out_settings->pixel_height = 216;
    out_settings->background[0] = 0.12f;
    out_settings->background[1] = 0.13f;
    out_settings->background[2] = 0.16f;
    out_settings->background[3] = 1.0f;
}

void render2d_begin(const Render2DCamera* new_camera, const Render2DSettings* new_settings)
{
    camera = *new_camera;
    settings = *new_settings;
    sprite_count = 0;
    target_ready = 0;
}

void render2d_add(const float model[16], const float size[2], Texture* texture, const float uv[4], const float color[4])
{
    if (!texture)
    {
        return;
    }

    if (sprite_count == sprite_capacity)
    {
        int capacity = sprite_capacity ? sprite_capacity * 2 : 256;
        Sprite* grown = SDL_realloc(sprites, sizeof(Sprite) * (size_t)capacity);

        if (!grown)
        {
            return;
        }

        sprites = grown;
        sprite_capacity = capacity;
    }

    Sprite* sprite = &sprites[sprite_count];
    float hw = size[0] * 0.5f;
    float hh = size[1] * 0.5f;
    const float local[4][2] = { { -hw, hh }, { hw, hh }, { hw, -hh }, { -hw, -hh } };

    for (int c = 0; c < 4; c++)
    {
        for (int r = 0; r < 3; r++)
        {
            sprite->corners[c][r] = model[r * 4 + 0] * local[c][0] + model[r * 4 + 1] * local[c][1] + model[r * 4 + 3];
        }
    }

    sprite->texture = texture;
    SDL_memcpy(sprite->uv, uv, sizeof(sprite->uv));
    SDL_memcpy(sprite->color, color, sizeof(sprite->color));
    sprite->z = model[11];
    sprite->order = sprite_count;
    sprite_count++;
}

static int compare_sprites(const void* a, const void* b)
{
    const Sprite* sa = (const Sprite*)a;
    const Sprite* sb = (const Sprite*)b;

    if (sa->z != sb->z)
    {
        return sa->z < sb->z ? -1 : 1;
    }

    return sa->order - sb->order;
}

static void flush(void)
{
    if (batch_count > 0)
    {
        draw_vertices(batch, batch_count, batch_texture);
    }

    batch_count = 0;
}

// One quad, its corners in pixels of the current pass (window or target).
static void push_quad(const float corners[4][2], Texture* texture, const float uv[4], const float color[4])
{
    if (texture != batch_texture || batch_count + 6 > BATCH_SPRITES * 6)
    {
        flush();
        batch_texture = texture;
    }

    const float uvs[4][2] = { { uv[0], uv[1] }, { uv[2], uv[1] }, { uv[2], uv[3] }, { uv[0], uv[3] } };
    static const int order[6] = { 0, 1, 2, 0, 2, 3 };

    for (int i = 0; i < 6; i++)
    {
        Vertex* v = &batch[batch_count++];
        v->x = corners[order[i]][0];
        v->y = corners[order[i]][1];
        v->u = uvs[order[i]][0];
        v->v = uvs[order[i]][1];
        v->r = color[0];
        v->g = color[1];
        v->b = color[2];
        v->a = color[3];
    }
}

static void push_rectangle(float x, float y, float w, float h, Texture* texture, const float uv[4], const float color[4])
{
    const float corners[4][2] = { { x, y }, { x + w, y }, { x + w, y + h }, { x, y + h } };
    push_quad(corners, texture, uv, color);
}

static const float full_uv[4] = { 0.0f, 0.0f, 1.0f, 1.0f };

static int ensure_target(int width, int height)
{
    if (target && width <= target_capacity_width && height == target_capacity_height)
    {
        return 1;
    }

    if (target)
    {
        wait_renderer_idle();
        destroy_render_target(target);
    }

    target_capacity_width = (width + TARGET_WIDTH_STEP - 1) / TARGET_WIDTH_STEP * TARGET_WIDTH_STEP;
    target_capacity_height = height;
    target = create_render_target(target_capacity_width, target_capacity_height);

    return target != NULL;
}

void render2d_prepare(void)
{
    if (sprite_count > 1)
    {
        qsort(sprites, (size_t)sprite_count, sizeof(Sprite), compare_sprites);
    }

    float view_width = camera.viewport[2];
    float view_height = camera.viewport[3];

    if (!settings.pixel_perfect || !camera.orthographic || camera.half_height <= 0.0f || view_width < 1.0f || view_height < 1.0f)
    {
        return;
    }

    // The largest whole factor that fits the art's height in the view, and as many columns as cover it.
    // One more pixel on every side, for the sub-pixel camera offset below.
    int height = SDL_clamp(settings.pixel_height, 16, 4096);
    int scale = SDL_max(1, (int)(view_height / (float)height));
    int width = (int)SDL_ceilf(view_width / (float)scale);
    int target_width = width + 2;
    int target_height = height + 2;

    if (!ensure_target(target_width, target_height))
    {
        return;
    }

    // The camera snapped to whole art pixels : every sprite edge then falls on a pixel boundary of the
    // target, and moving the camera never makes the art shimmer.
    float unit = (2.0f * camera.half_height) / (float)height; // world units per art pixel
    float snapped_x = SDL_floorf(camera.center[0] / unit) * unit;
    float snapped_y = SDL_floorf(camera.center[1] / unit) * unit;
    float left = snapped_x - (float)target_width * 0.5f * unit;
    float top = snapped_y + (float)target_height * 0.5f * unit;

    if (!begin_render_target_pass(target))
    {
        return;
    }

    batch_count = 0;
    batch_texture = NULL;

    if (settings.background[3] > 0.0f)
    {
        push_rectangle(0.0f, 0.0f, (float)target_width, (float)target_height, white_texture(), full_uv, settings.background);
    }

    for (int i = 0; i < sprite_count; i++)
    {
        const Sprite* sprite = &sprites[i];
        float corners[4][2];

        for (int c = 0; c < 4; c++)
        {
            corners[c][0] = (sprite->corners[c][0] - left) / unit;
            corners[c][1] = (top - sprite->corners[c][1]) / unit;
        }

        push_quad(corners, sprite->texture, sprite->uv, sprite->color);
    }

    flush();
    end_render_target_pass();

    // The snapping's remainder, in whole screen pixels : the scaled image moves smoothly while each art
    // pixel stays exactly `scale` screen pixels wide.
    float center_x = camera.viewport[0] + view_width * 0.5f;
    float center_y = camera.viewport[1] + view_height * 0.5f;
    float pixel_x = (float)target_width * 0.5f + (camera.center[0] - snapped_x) / unit;
    float pixel_y = (float)target_height * 0.5f - (camera.center[1] - snapped_y) / unit;

    target_origin[0] = SDL_roundf(center_x - pixel_x * (float)scale);
    target_origin[1] = SDL_roundf(center_y - pixel_y * (float)scale);
    target_used[0] = target_width;
    target_used[1] = target_height;
    target_scale = scale;
    target_ready = 1;
}

static void present_pixel_perfect(void)
{
    const float black[4] = { 0.0f, 0.0f, 0.0f, 1.0f };
    const float white[4] = { 1.0f, 1.0f, 1.0f, 1.0f };
    const float uv[4] = { 0.0f, 0.0f, (float)target_used[0] / (float)target_capacity_width, (float)target_used[1] / (float)target_capacity_height };

    // The bars above and below the art, if the view's height isn't a whole multiple of it.
    push_rectangle(camera.viewport[0], camera.viewport[1], camera.viewport[2], camera.viewport[3], white_texture(), full_uv, black);
    push_rectangle(target_origin[0], target_origin[1], (float)(target_used[0] * target_scale), (float)(target_used[1] * target_scale),
        render_target_texture(target), uv, white);
}

static void present_direct(void)
{
    int window_width = 0;
    int window_height = 0;
    get_window_size(&window_width, &window_height);

    if (settings.background[3] > 0.0f)
    {
        push_rectangle(camera.viewport[0], camera.viewport[1], camera.viewport[2], camera.viewport[3], white_texture(), full_uv, settings.background);
    }

    for (int i = 0; i < sprite_count; i++)
    {
        const Sprite* sprite = &sprites[i];
        float corners[4][2];
        int visible = 1;

        for (int c = 0; c < 4; c++)
        {
            float clip[4];
            mat4_transform(camera.view_projection, sprite->corners[c][0], sprite->corners[c][1], sprite->corners[c][2], 1.0f, clip);

            if (clip[3] <= 0.001f)
            {
                visible = 0;
                break;
            }

            corners[c][0] = (clip[0] / clip[3] * 0.5f + 0.5f) * (float)window_width;
            corners[c][1] = (0.5f - clip[1] / clip[3] * 0.5f) * (float)window_height;
        }

        if (visible)
        {
            push_quad(corners, sprite->texture, sprite->uv, sprite->color);
        }
    }
}

void render2d_present(void)
{
    int window_width = 0;
    int window_height = 0;
    get_window_size(&window_width, &window_height);

    batch_count = 0;
    batch_texture = NULL;

    set_scissor((int)camera.viewport[0], (int)camera.viewport[1], (int)camera.viewport[2], (int)camera.viewport[3]);

    if (target_ready)
    {
        present_pixel_perfect();
    }
    else
    {
        present_direct();
    }

    flush();
    set_scissor(0, 0, window_width, window_height);
}

void render2d_shutdown(void)
{
    if (target)
    {
        destroy_render_target(target);
        target = NULL;
    }

    target_capacity_width = 0;
    target_capacity_height = 0;
    target_ready = 0;

    SDL_free(sprites);
    sprites = NULL;
    sprite_count = 0;
    sprite_capacity = 0;
}
