#ifndef _RENDER2D_H_
#define _RENDER2D_H_

// The 2D renderer : sprites (textured quads placed in the world) drawn back to front, as a camera sees
// them. Either straight into the window, or "pixel perfect" : into a low-resolution target of a fixed
// height in pixels, then scaled up by a whole factor, so every pixel of the art stays a crisp square
// of screen pixels whatever the window size. Its width follows the view's aspect ratio : a wider window
// shows more of the world, rather than adding bars on the sides.
//
// Per frame :
//     render2d_begin(&camera, &settings);
//     render2d_add(...);    // every sprite
//     render2d_prepare();   // before begin_frame() : the pixel-perfect offscreen pass
//     begin_frame();
//     render2d_present();   // into the window
//     end_frame();

#include "renderer/renderer.h"
#include "math/math3d.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef struct
{
    int pixel_perfect;
    int pixel_height;       // the low-resolution target's height : how many art pixels the view is tall
    float background[4];    // what's behind the sprites ; alpha 0 : nothing drawn
} Render2DSettings;

typedef struct
{
    Mat4 view_projection;   // world to the whole window's clip space
    int orthographic;       // pixel perfect needs an axis-aligned orthographic camera ; otherwise drawn directly
    float center[2];        // orthographic : the world point (x, y) at the middle of the view
    float half_height;      // orthographic : half the world height the view shows
    float viewport[4];      // where the view is in the window : x, y, width, height in pixels
} Render2DCamera;

void render2d_default_settings(Render2DSettings* out_settings);

void render2d_begin(const Render2DCamera* camera, const Render2DSettings* settings);

// A `size` (world units) quad centered on the origin of `model` (row-major), facing +Z. `uv` : the
// texture's region (u0, v0, u1, v1). Sprites are drawn from the lowest world Z to the highest.
void render2d_add(const float model[16], const float size[2], Texture* texture, const float uv[4], const float color[4]);

void render2d_prepare(void);
void render2d_present(void);

void render2d_shutdown(void); // releases the pixel-perfect target ; the GPU must be done with it

#ifdef __cplusplus
}
#endif

#endif // _RENDER2D_H_
