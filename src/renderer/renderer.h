// The engine's rendering API (RHI), the same on every backend : frames, textures, render targets,
// the 2D primitive draw_vertices(), and the forward 3D path (meshes, shadow map, skybox). The calls are
// forwarded by renderer.c to the backend chosen at runtime (Vulkan or DirectX 12, see
// renderer_backend.h) ; the bindless layer used by the photorealistic renderer is in gpu.h.

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

// --- 3D ---------------------------------------------------------------------------------------
//
// Matrices are row-major float[16] transforming column vectors, right-handed, depth in [0, 1] : see
// math/math3d.h, which builds them. A frame with 3D content looks like :
//
//     begin_shadow_pass(shadow_map, light_view_projection);  // outside begin_frame()/end_frame()
//     draw_mesh(mesh, model);                                 // ... every shadow caster
//     end_shadow_pass();
//
//     begin_frame();
//     begin_scene(&lighting, shadow_map);                     // forward pass, depth-tested
//     draw_mesh(mesh, model);                                 // ... every visible mesh
//     end_scene();                                            // back to 2D : draw_vertices(), UI...
//     end_frame();

typedef struct
{
    float x, y, z;       // position
    float nx, ny, nz;    // normal
    float r, g, b, a;    // albedo color
    float u, v;          // texture coordinates
    float tx, ty, tz, tw; // tangent ; w = bitangent sign (the photoreal renderer's normal mapping)
} MeshVertex;

typedef struct Mesh Mesh;           // opaque static GPU geometry (vertices + 32-bit indices)
typedef struct ShadowMap ShadowMap; // opaque square depth texture, rendered from the light's point of view

typedef struct
{
    float view_projection[16];       // camera
    float light_view_projection[16]; // must be the one the shadow map was rendered with
    float light_direction[3];        // world-space direction the light travels in, normalized
    float light_color[3];
    float ambient_color[3];          // ambient light from above (e.g. a skybox's sky irradiance)
    float ground_color[3];           // ambient light from below (ground bounce) ; = ambient_color for a uniform ambient
    float shadow_depth_bias;         // in light clip-space depth units (e.g. 0.001)
    float shadow_normal_offset;      // world units the receiver is pushed along its normal before the lookup
} SceneLighting;

Mesh* create_mesh(const MeshVertex* vertices, int vertex_count, const uint32_t* indices, int index_count); // NULL on failure
void destroy_mesh(Mesh*); // the GPU must be done with it (e.g. after wait_renderer_idle())

ShadowMap* create_shadow_map(int size); // size x size depth texels ; NULL on failure
void destroy_shadow_map(ShadowMap*);

// Renders depth only into `shadow_map` : like begin_render_target_pass(), a self-contained,
// synchronous unit of GPU work (submitted and waited on by end_shadow_pass()), so it must not be
// called between begin_frame()/end_frame(), and the shadow map is ready to sample as soon as
// end_shadow_pass() returns.
int begin_shadow_pass(ShadowMap* shadow_map, const float light_view_projection[16]); // 0 on failure
void end_shadow_pass(void);

// Starts forward-shaded, depth-tested 3D drawing into the window : one directional light,
// Lambert diffuse + ambient, shadows from `shadow_map` (3x3 PCF). Must be called between
// begin_frame()/end_frame(), and matched by end_scene() before any draw_vertices() call.
void begin_scene(const SceneLighting* lighting, ShadowMap* shadow_map);
void end_scene(void);

// Draws `mesh` transformed by `model` (row-major float[16]), in the current shadow pass or scene.
void draw_mesh(Mesh* mesh, const float model[16]);
void draw_mesh_colored(Mesh* mesh, const float model[16], const float color[3]); // one color, in place of the mesh's vertex colors
void draw_mesh_range(Mesh* mesh, const float model[16], int first_index, int index_count); // part of its indices

// Draws `equirectangular` (a 2:1 latitude/longitude panorama : U around the horizon, -Z at its
// center, V from straight up to straight down) as the scene's background, scaled by `intensity`.
// Between begin_scene()/end_scene() ; drawn at the far plane, only where no mesh is, so drawing it
// after the meshes skips every covered pixel. See renderer/photoreal/environment.h to load one.
void draw_skybox(Texture* equirectangular, float intensity);

// Restricts subsequent draw_vertices() calls to the (x, y, width, height) pixel rectangle of the
// current pass (window or render target), e.g. for clipped UI. Every begin_frame() and
// begin_render_target_pass() resets it to the whole pass. Anything batching draws on the CPU side
// (sprite_batch) must be flushed before changing it, since it only affects draws issued afterwards.
void set_scissor(int x, int y, int width, int height);

// --- Statistics (profiling) -------------------------------------------------------------------
//
// What the renderer knows about the last frames, for profilers. The counts cover everything drawn
// between two end_frame() calls, synchronous passes (shadow maps, render targets) included. The GPU
// time is measured with timestamp queries around the frame's command list only (not the synchronous
// passes), a frame or two late, since the GPU runs behind the CPU.
typedef struct
{
    const char* backend;        // "Vulkan", "DirectX 12"
    char gpu_name[128];
    float gpu_frame_ms;         // GPU time of the frame's command list ; < 0 if it can't be measured
    unsigned long long vram_used;   // bytes of video memory this process uses ; 0 if unknown
    unsigned long long vram_budget; // bytes the OS lets it use before trouble ; 0 if unknown
    int draw_calls;             // every draw of the last frame : 2D batches, meshes, sky, GPU layer
    int vertices_2d;            // vertices sent through draw_vertices() (sprites, UI)
    int mesh_draws;             // draw_mesh*() calls
} RendererStats;

void renderer_get_stats(RendererStats* out_stats);

#ifdef __cplusplus
}
#endif

#endif // _RENDERER_H_
