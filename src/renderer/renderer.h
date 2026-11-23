// The engine's rendering API (RHI), the same on every backend : frames, textures, render targets,
// the 2D primitive draw_vertices() and static meshes. The calls are forwarded by renderer.c to the
// backend chosen at runtime (Vulkan or DirectX 12, see renderer_backend.h). 3D rendering is built on
// the bindless layer of gpu.h, by renderer/render3d (Forward+ and Deferred+).

#ifndef _RENDERER_H_
#define _RENDERER_H_

#include <stdint.h>

#include "platforms/platforms_window.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef struct Texture Texture; // opaque GPU texture handle
typedef struct RenderTarget RenderTarget; // opaque GPU render target : a texture that can also be drawn into

typedef struct
{
    float x, y;       // position, in pixels (0,0 = top-left of the window, +Y down)
    float u, v;        // texture coordinates (0..1, relative to the bound texture)
    float r, g, b, a;  // vertex tint color (0..1), multiplied with the sampled texel
} Vertex;

int create_renderer(Window*); // returns 0 on failure, 1 on success
void destroy_renderer(void);
void wait_renderer_idle(void);

int begin_frame(void); // acquires the next frame; returns 0 if the frame should be skipped, 1 otherwise
void end_frame(void);  // submits and presents the frame started by begin_frame

Texture* create_texture(const char* file_path, int* out_width, int* out_height); // NULL on failure
void destroy_texture(Texture*);
Texture* white_texture(void); // a built-in 1x1 opaque white texture, for drawing untextured shapes
void texture_get_size(Texture* texture, int* out_width, int* out_height);

// Creates a texture from raw, tightly packed RGBA8 pixels (width * height * 4 bytes, top row first),
// e.g. a font atlas generated at runtime. Must not be called between begin_frame()/end_frame(). NULL
// on failure ; released with destroy_texture() like any other texture.
Texture* create_texture_from_pixels(const void* rgba_pixels, int width, int height);

// A fixed-size offscreen color target, e.g. for rendering a whole scene at a low, fixed internal
// resolution before upscaling it as a single textured quad (pixel-perfect rendering).
RenderTarget* create_render_target(int width, int height); // NULL on failure
void destroy_render_target(RenderTarget*);
Texture* render_target_texture(RenderTarget*); // the target's contents, sampled like any other texture

// Redirects draw_vertices() to `target` instead of the window, clearing it first. Must be matched
// by end_render_target_pass(), and neither may be called between begin_frame()/end_frame() :
// rendering to a target is its own self-contained unit of GPU work, submitted and waited on by
// end_render_target_pass(), so the target's texture is immediately safe to sample afterwards
// (e.g. with a normal draw_vertices()/sprite_batch call inside the next begin_frame()/end_frame()).
int begin_render_target_pass(RenderTarget* target); // returns 0 on failure, 1 on success
void end_render_target_pass(void);

// Draws `vertex_count` vertices (as a triangle list, no index buffer) from `vertices`,
// sampled from `texture`. Must be called between begin_frame/end_frame, or between
// begin_render_target_pass/end_render_target_pass.
void draw_vertices(const Vertex* vertices, int vertex_count, Texture* texture);

// --- Meshes -----------------------------------------------------------------------------------
//
// Static geometry, drawn by renderer/render3d through gpu.h. Matrices are row-major float[16]
// transforming column vectors, right-handed, depth in [0, 1] : see math/math3d.h, which builds them.

typedef struct
{
    float x, y, z;       // position
    float nx, ny, nz;    // normal
    float r, g, b, a;    // albedo color
    float u, v;          // texture coordinates
    float tx, ty, tz, tw; // tangent ; w = bitangent sign, for normal mapping
} MeshVertex;

typedef struct Mesh Mesh; // opaque static GPU geometry (vertices + 32-bit indices)

Mesh* create_mesh(const MeshVertex* vertices, int vertex_count, const uint32_t* indices, int index_count); // NULL on failure
void destroy_mesh(Mesh*); // the GPU must be done with it (e.g. after wait_renderer_idle())

// Restricts subsequent draw_vertices() calls to the (x, y, width, height) pixel rectangle of the
// current pass (window or render target), e.g. for clipped UI. Every begin_frame() and
// begin_render_target_pass() resets it to the whole pass. Anything batching draws on the CPU side
// (sprite_batch) must be flushed before changing it, since it only affects draws issued afterwards.
void set_scissor(int x, int y, int width, int height);

// --- Statistics (profiling) -------------------------------------------------------------------
//
// What the renderer knows about the last frames, for profilers. The counts cover everything drawn
// between two end_frame() calls, synchronous passes (render targets) included. The GPU
// time is measured with timestamp queries around the frame's command list only (not the synchronous
// passes), a frame or two late, since the GPU runs behind the CPU.
typedef struct
{
    const char* backend;        // "Vulkan", "DirectX 12"
    char gpu_name[128];
    float gpu_frame_ms;         // GPU time of the frame's command list ; < 0 if it can't be measured
    unsigned long long vram_used;   // bytes of video memory this process uses ; 0 if unknown
    unsigned long long vram_budget; // bytes the OS lets it use before trouble ; 0 if unknown
    int draw_calls;             // every draw of the last frame : 2D batches and GPU layer draws
    int vertices_2d;            // vertices sent through draw_vertices() (sprites, UI)
    int mesh_draws;             // gpu_draw_indexed() calls : meshes, shadow casters included
} RendererStats;

void renderer_get_stats(RendererStats* out_stats);

#ifdef __cplusplus
}
#endif

#endif // _RENDERER_H_
