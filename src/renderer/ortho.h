#ifndef _ORTHO_H_
#define _ORTHO_H_

// Maps window-pixel coordinates (0,0 = top-left, +Y down) to clip space, as a plain
// per-axis scale + offset rather than a 4x4 matrix: an orthographic 2D camera never
// needs rotation or shear, and this sidesteps any row-major/column-major mismatch
// between the GLSL and HLSL shaders (both just do `position * scale + offset`).
//
// The result targets the same "clip +Y is up" convention D3D uses natively; the Vulkan
// backend reconciles this with Vulkan's default "clip +Y is down" by using a negative
// viewport height (see vulkan_renderer.c), so this transform is identical on every backend.
typedef struct
{
    float scale_x, scale_y;
    float offset_x, offset_y;
} ScreenTransform;

static inline ScreenTransform screen_transform_for_size(float width, float height)
{
    ScreenTransform transform = {
        2.0f / width, -2.0f / height,
        -1.0f, 1.0f
    };

    return transform;
}

#endif // _ORTHO_H_
