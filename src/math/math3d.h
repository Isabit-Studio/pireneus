#ifndef _MATH3D_H_
#define _MATH3D_H_

// Minimal 3D math for the renderer and examples : plain C, header-only, no SIMD.
//
// Conventions (shared by every renderer backend, see renderer.h and mesh.slang) :
// - Mat4 is ROW-major (m[row * 4 + column]) and transforms COLUMN vectors : p' = M * p. Shaders
//   receive matrices as four float4 rows and multiply with explicit dot products (one per row),
//   so the memory layout written here is exactly what they read, on every shader target (no
//   row/column-major packing rules involved — the same reason ortho.h avoids matrices altogether).
// - Right-handed world and view space (the camera looks down -Z, +Y up).
// - Clip-space depth in [0, 1] (D3D convention ; Vulkan uses the same range), and clip +Y up,
//   which the Vulkan backend reconciles with a negative-height viewport, like ortho.h.

#include <math.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef struct
{
    float x, y, z;
} Vec3;

typedef struct
{
    float m[16];
} Mat4;

static inline Vec3 vec3(float x, float y, float z)
{
    Vec3 v = { x, y, z };
    return v;
}

static inline Vec3 vec3_add(Vec3 a, Vec3 b) { return vec3(a.x + b.x, a.y + b.y, a.z + b.z); }
static inline Vec3 vec3_sub(Vec3 a, Vec3 b) { return vec3(a.x - b.x, a.y - b.y, a.z - b.z); }
static inline Vec3 vec3_scale(Vec3 v, float s) { return vec3(v.x * s, v.y * s, v.z * s); }
static inline float vec3_dot(Vec3 a, Vec3 b) { return a.x * b.x + a.y * b.y + a.z * b.z; }
static inline float vec3_length(Vec3 v) { return sqrtf(vec3_dot(v, v)); }

static inline Vec3 vec3_cross(Vec3 a, Vec3 b)
{
    return vec3(a.y * b.z - a.z * b.y, a.z * b.x - a.x * b.z, a.x * b.y - a.y * b.x);
}

static inline Vec3 vec3_normalize(Vec3 v)
{
    float length = vec3_length(v);
    return length > 0.0f ? vec3_scale(v, 1.0f / length) : v;
}

static inline Mat4 mat4_identity(void)
{
    Mat4 r = { { 1, 0, 0, 0,  0, 1, 0, 0,  0, 0, 1, 0,  0, 0, 0, 1 } };
    return r;
}

// a * b : applies b first, then a.
static inline Mat4 mat4_multiply(Mat4 a, Mat4 b)
{
    Mat4 r;

    for (int row = 0; row < 4; row++)
    {
        for (int column = 0; column < 4; column++)
        {
            r.m[row * 4 + column] =
                a.m[row * 4 + 0] * b.m[0 * 4 + column] +
                a.m[row * 4 + 1] * b.m[1 * 4 + column] +
                a.m[row * 4 + 2] * b.m[2 * 4 + column] +
                a.m[row * 4 + 3] * b.m[3 * 4 + column];
        }
    }

    return r;
}

// Also converts between the engine's row-major layout and the column-major (glm/OpenGL) layout
// some libraries expect, e.g. ImGuizmo : same matrix, transposed storage.
static inline Mat4 mat4_transpose(Mat4 a)
{
    Mat4 r;

    for (int row = 0; row < 4; row++)
    {
        for (int column = 0; column < 4; column++)
        {
            r.m[column * 4 + row] = a.m[row * 4 + column];
        }
    }

    return r;
}

// General 4x4 inverse (cofactor expansion) ; returns the identity if `a` isn't invertible. The
// formula is layout-agnostic : inverse(transpose(a)) == transpose(inverse(a)).
static inline Mat4 mat4_inverse(Mat4 a)
{
    const float* m = a.m;
    Mat4 r;
    float* inv = r.m;

    inv[0] = m[5] * m[10] * m[15] - m[5] * m[11] * m[14] - m[9] * m[6] * m[15] + m[9] * m[7] * m[14] + m[13] * m[6] * m[11] - m[13] * m[7] * m[10];
    inv[4] = -m[4] * m[10] * m[15] + m[4] * m[11] * m[14] + m[8] * m[6] * m[15] - m[8] * m[7] * m[14] - m[12] * m[6] * m[11] + m[12] * m[7] * m[10];
    inv[8] = m[4] * m[9] * m[15] - m[4] * m[11] * m[13] - m[8] * m[5] * m[15] + m[8] * m[7] * m[13] + m[12] * m[5] * m[11] - m[12] * m[7] * m[9];
    inv[12] = -m[4] * m[9] * m[14] + m[4] * m[10] * m[13] + m[8] * m[5] * m[14] - m[8] * m[6] * m[13] - m[12] * m[5] * m[10] + m[12] * m[6] * m[9];
    inv[1] = -m[1] * m[10] * m[15] + m[1] * m[11] * m[14] + m[9] * m[2] * m[15] - m[9] * m[3] * m[14] - m[13] * m[2] * m[11] + m[13] * m[3] * m[10];
    inv[5] = m[0] * m[10] * m[15] - m[0] * m[11] * m[14] - m[8] * m[2] * m[15] + m[8] * m[3] * m[14] + m[12] * m[2] * m[11] - m[12] * m[3] * m[10];
    inv[9] = -m[0] * m[9] * m[15] + m[0] * m[11] * m[13] + m[8] * m[1] * m[15] - m[8] * m[3] * m[13] - m[12] * m[1] * m[11] + m[12] * m[3] * m[9];
    inv[13] = m[0] * m[9] * m[14] - m[0] * m[10] * m[13] - m[8] * m[1] * m[14] + m[8] * m[2] * m[13] + m[12] * m[1] * m[10] - m[12] * m[2] * m[9];
    inv[2] = m[1] * m[6] * m[15] - m[1] * m[7] * m[14] - m[5] * m[2] * m[15] + m[5] * m[3] * m[14] + m[13] * m[2] * m[7] - m[13] * m[3] * m[6];
    inv[6] = -m[0] * m[6] * m[15] + m[0] * m[7] * m[14] + m[4] * m[2] * m[15] - m[4] * m[3] * m[14] - m[12] * m[2] * m[7] + m[12] * m[3] * m[6];
    inv[10] = m[0] * m[5] * m[15] - m[0] * m[7] * m[13] - m[4] * m[1] * m[15] + m[4] * m[3] * m[13] + m[12] * m[1] * m[7] - m[12] * m[3] * m[5];
    inv[14] = -m[0] * m[5] * m[14] + m[0] * m[6] * m[13] + m[4] * m[1] * m[14] - m[4] * m[2] * m[13] - m[12] * m[1] * m[6] + m[12] * m[2] * m[5];
    inv[3] = -m[1] * m[6] * m[11] + m[1] * m[7] * m[10] + m[5] * m[2] * m[11] - m[5] * m[3] * m[10] - m[9] * m[2] * m[7] + m[9] * m[3] * m[6];
    inv[7] = m[0] * m[6] * m[11] - m[0] * m[7] * m[10] - m[4] * m[2] * m[11] + m[4] * m[3] * m[10] + m[8] * m[2] * m[7] - m[8] * m[3] * m[6];
    inv[11] = -m[0] * m[5] * m[11] + m[0] * m[7] * m[9] + m[4] * m[1] * m[11] - m[4] * m[3] * m[9] - m[8] * m[1] * m[7] + m[8] * m[3] * m[5];
    inv[15] = m[0] * m[5] * m[10] - m[0] * m[6] * m[9] - m[4] * m[1] * m[10] + m[4] * m[2] * m[9] + m[8] * m[1] * m[6] - m[8] * m[2] * m[5];

    float determinant = m[0] * inv[0] + m[1] * inv[4] + m[2] * inv[8] + m[3] * inv[12];

    if (determinant == 0.0f)
    {
        return mat4_identity();
    }

    for (int i = 0; i < 16; i++)
    {
        inv[i] /= determinant;
    }

    return r;
}

// M * (x, y, z, w), the result written to out[4].
static inline void mat4_transform(Mat4 a, float x, float y, float z, float w, float out[4])
{
    for (int row = 0; row < 4; row++)
    {
        out[row] = a.m[row * 4 + 0] * x + a.m[row * 4 + 1] * y + a.m[row * 4 + 2] * z + a.m[row * 4 + 3] * w;
    }
}

// Right-handed view matrix : the camera at `eye`, looking at `target`.
static inline Mat4 mat4_look_at(Vec3 eye, Vec3 target, Vec3 up)
{
    Vec3 f = vec3_normalize(vec3_sub(target, eye));
    Vec3 s = vec3_normalize(vec3_cross(f, up));
    Vec3 u = vec3_cross(s, f);

    Mat4 r = { {
         s.x,  s.y,  s.z, -vec3_dot(s, eye),
         u.x,  u.y,  u.z, -vec3_dot(u, eye),
        -f.x, -f.y, -f.z,  vec3_dot(f, eye),
         0.0f, 0.0f, 0.0f, 1.0f
    } };

    return r;
}

// Right-handed perspective projection, depth mapped to [0, 1] (near -> 0, far -> 1).
static inline Mat4 mat4_perspective(float fov_y_radians, float aspect, float near_z, float far_z)
{
    float f = 1.0f / tanf(fov_y_radians * 0.5f);

    Mat4 r = { {
        f / aspect, 0.0f, 0.0f, 0.0f,
        0.0f, f, 0.0f, 0.0f,
        0.0f, 0.0f, far_z / (near_z - far_z), near_z * far_z / (near_z - far_z),
        0.0f, 0.0f, -1.0f, 0.0f
    } };

    return r;
}

// Right-handed orthographic projection, depth mapped to [0, 1] (near -> 0, far -> 1).
static inline Mat4 mat4_orthographic(float left, float right, float bottom, float top, float near_z, float far_z)
{
    Mat4 r = { {
        2.0f / (right - left), 0.0f, 0.0f, -(right + left) / (right - left),
        0.0f, 2.0f / (top - bottom), 0.0f, -(top + bottom) / (top - bottom),
        0.0f, 0.0f, 1.0f / (near_z - far_z), near_z / (near_z - far_z),
        0.0f, 0.0f, 0.0f, 1.0f
    } };

    return r;
}

#ifdef __cplusplus
}
#endif

#endif // _MATH3D_H_
