// 2D sprites, after XNA / MonoGame / FNA's SpriteBatch : quads accumulated on the CPU and drawn with
// draw_vertices() in as few calls as possible (a flush happens on a texture change, a full buffer, or
// sprite_batch_end()). Positions are in window pixels.

#ifndef _SPRITE_BATCH_H_
#define _SPRITE_BATCH_H_

#include "renderer/renderer.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef struct
{
    float r, g, b, a;
} Color;

static inline Color color_rgba(float r, float g, float b, float a)
{
    Color color = { r, g, b, a };
    return color;
}

static inline Color color_white(void)
{
    return color_rgba(1.0f, 1.0f, 1.0f, 1.0f);
}

void sprite_batch_begin(void);

// Draws the full `texture`, stretched to (x, y, width, height) in pixels, tinted by `color`.
void sprite_batch_draw(Texture* texture, float x, float y, float width, float height, Color color);

// Draws the (src_x, src_y, src_width, src_height) pixel sub-rectangle of `texture` (whose full
// size is texture_width x texture_height, as returned by create_texture), stretched to
// (x, y, width, height) on screen, tinted by `color`. Used for tilesets/atlases.
void sprite_batch_draw_region(Texture* texture, int texture_width, int texture_height,
    float x, float y, float width, float height,
    float src_x, float src_y, float src_width, float src_height, Color color);

void sprite_batch_end(void);

#ifdef __cplusplus
}
#endif

#endif // _SPRITE_BATCH_H_
