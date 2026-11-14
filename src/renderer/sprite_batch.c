#include <stddef.h>

#include "renderer/sprite_batch.h"
#include "debug.h"

#define SPRITE_BATCH_MAX_QUADS 4096
#define SPRITE_BATCH_MAX_VERTICES (SPRITE_BATCH_MAX_QUADS * 6)

static Vertex vertices[SPRITE_BATCH_MAX_VERTICES];
static int vertex_count;
static Texture* current_texture;

static void flush(void)
{
    if (vertex_count > 0 && current_texture)
    {
        draw_vertices(vertices, vertex_count, current_texture);
    }

    vertex_count = 0;
}

void sprite_batch_begin(void)
{
    vertex_count = 0;
    current_texture = NULL;
}

static void push_quad(Texture* texture, float x, float y, float width, float height,
    float u0, float v0, float u1, float v1, Color color)
{
    if (vertex_count + 6 > SPRITE_BATCH_MAX_VERTICES)
    {
        DEBUG_LOG("[sprite_batch] batch full, auto-flushing mid-frame (%d quads)\n", vertex_count / 6);
    }

    if (texture != current_texture || vertex_count + 6 > SPRITE_BATCH_MAX_VERTICES)
    {
        flush();
        current_texture = texture;
    }

    Vertex top_left = { x, y, u0, v0, color.r, color.g, color.b, color.a };
    Vertex top_right = { x + width, y, u1, v0, color.r, color.g, color.b, color.a };
    Vertex bottom_left = { x, y + height, u0, v1, color.r, color.g, color.b, color.a };
    Vertex bottom_right = { x + width, y + height, u1, v1, color.r, color.g, color.b, color.a };

    vertices[vertex_count++] = top_left;
    vertices[vertex_count++] = top_right;
    vertices[vertex_count++] = bottom_right;
    vertices[vertex_count++] = top_left;
    vertices[vertex_count++] = bottom_right;
    vertices[vertex_count++] = bottom_left;
}

void sprite_batch_draw(Texture* texture, float x, float y, float width, float height, Color color)
{
    push_quad(texture, x, y, width, height, 0.0f, 0.0f, 1.0f, 1.0f, color);
}

void sprite_batch_draw_region(Texture* texture, int texture_width, int texture_height,
    float x, float y, float width, float height,
    float src_x, float src_y, float src_width, float src_height, Color color)
{
    float u0 = src_x / (float)texture_width;
    float v0 = src_y / (float)texture_height;
    float u1 = (src_x + src_width) / (float)texture_width;
    float v1 = (src_y + src_height) / (float)texture_height;

    push_quad(texture, x, y, width, height, u0, v0, u1, v1, color);
}

void sprite_batch_end(void)
{
    flush();
    current_texture = NULL;
}
