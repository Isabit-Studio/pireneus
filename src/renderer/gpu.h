#ifndef _GPU_H_
#define _GPU_H_

// The engine's low-level GPU layer : what a multi-pass renderer (renderer/render3d) is written against,
// once for every backend. renderer.h stays the simple, high-level API (sprites, render targets, meshes).
//
// Everything is BINDLESS, in the spirit of The Forge's visibility buffer : there is no per-draw
// resource binding at all. Every texture and buffer lives in one global table and is known to shaders
// by an integer index, passed through push constants or stored in other buffers. Shaders declare the
// table once (see the top of shaders/render3d_common.slang) : an unbounded array of textures, one of
// multisampled textures, an unbounded array of byte-address buffers, and four fixed samplers.
//
// Passes are recorded into the frame's own command list, so they must be issued after begin_frame()
// and before anything is drawn into the window (draw_vertices()...) :
//
//     begin_frame();
//     gpu_begin_pass(&pass); gpu_bind_pipeline(p); gpu_push_constants(...); gpu_draw(3); gpu_end_pass();
//     ... more passes ...
//     draw_vertices(...);   // the window pass starts here
//     end_frame();
//
// A texture is readable by shaders between passes ; gpu_begin_pass()/gpu_end_pass() transition its
// attachments for writing and back.

#include <stddef.h>
#include <stdint.h>

#include "renderer/renderer.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef enum
{
    GPU_FORMAT_NONE,
    GPU_FORMAT_RGBA8,      // UNORM ; also holds the visibility buffer's packed triangle IDs, exactly
    GPU_FORMAT_RGBA8_SRGB, // color textures, decoded to linear when sampled
    GPU_FORMAT_RGBA16F,
    GPU_FORMAT_RGBA32F,
    GPU_FORMAT_RG16F,
    GPU_FORMAT_R16F,
    GPU_FORMAT_R32F,
    GPU_FORMAT_R8,
    GPU_FORMAT_D32,
    // Block-compressed (4 x 4 texels per block), sampled only : what DDS files hold. A level's data is
    // its rows of blocks, tightly packed. gpu_format_supported() says whether the GPU samples them.
    GPU_FORMAT_BC1,        // RGB + 1-bit alpha, 8 bytes per block
    GPU_FORMAT_BC1_SRGB,
    GPU_FORMAT_BC3,        // RGBA, 16 bytes per block
    GPU_FORMAT_BC3_SRGB,
    GPU_FORMAT_BC5         // RG (normal maps), 16 bytes per block
} GpuFormat;

enum
{
    GPU_USAGE_SAMPLED = 1,
    GPU_USAGE_RENDER_TARGET = 2,
    GPU_USAGE_DEPTH_TARGET = 4
};

// The fixed samplers, see the top of render3d_common.slang.
enum
{
    GPU_SAMPLER_POINT_CLAMP,
    GPU_SAMPLER_LINEAR_CLAMP,
    GPU_SAMPLER_ANISOTROPIC_WRAP, // material textures
    GPU_SAMPLER_PANORAMA,         // linear, wraps around the horizon, clamps at the poles
    GPU_SAMPLER_COUNT
};

#define GPU_MAX_COLOR_TARGETS 4
#define GPU_MAX_PUSH_CONSTANTS 128 // bytes
#define GPU_MAX_MIPS 16

typedef struct
{
    int width, height;
    int mip_count;                // >= 1
    GpuFormat format;
    unsigned usage;               // GPU_USAGE_*
    int samples;                  // 0 or 1 : single-sampled. Multisampled : shaders read its samples through
                                  // g_textures_ms[gpu_texture_index()] (Texture2DMS), not g_textures
    const void* mips[GPU_MAX_MIPS]; // tightly packed pixels of each level ; mips[0] == NULL : left uninitialized
} GpuTextureDesc;

typedef struct GpuBuffer GpuBuffer;
typedef struct GpuPipeline GpuPipeline;

typedef enum
{
    GPU_BLEND_NONE,
    GPU_BLEND_ALPHA,
    GPU_BLEND_ADD,
    GPU_BLEND_ATTENUATE // destination * source alpha + source : what's behind a medium, then its own light
} GpuBlend;

typedef struct
{
    const char* vertex_shader;   // compiled shader names, as in cmake/pireneus_shaders.cmake, without extension
    const char* fragment_shader; // NULL : depth only
    GpuFormat color_formats[GPU_MAX_COLOR_TARGETS];
    int color_count;
    GpuFormat depth_format;      // GPU_FORMAT_NONE : no depth
    int depth_test;              // LESS_OR_EQUAL against the depth target
    int depth_always;            // with depth_test : ALWAYS instead (clearing part of a depth target)
    int depth_write;
    GpuBlend blend;
    int cull_back;               // 0 : no culling
    float depth_bias_constant;   // shadow passes
    float depth_bias_slope;
    int samples;                 // the attachments' sample count ; 0 or 1 : single-sampled
} GpuPipelineDesc;

typedef enum
{
    GPU_LOAD_CLEAR,
    GPU_LOAD_KEEP,    // previous contents preserved
    GPU_LOAD_DISCARD  // contents undefined : every pixel gets overwritten
} GpuLoad;

typedef struct
{
    Texture* texture;
    GpuLoad load;
    float clear[4];
} GpuColorAttachment;

typedef struct
{
    GpuColorAttachment colors[GPU_MAX_COLOR_TARGETS];
    int color_count;
    Texture* depth;   // NULL : none
    GpuLoad depth_load;
    float depth_clear;
} GpuPassDesc;

// A block of this frame's upload memory, valid until the frame ends. A shader reads it through
// g_buffers[buffer_index], at byte offset `offset`.
typedef struct
{
    void* mapped;
    uint32_t buffer_index;
    uint32_t offset;
} GpuFrameAllocation;

int gpu_supported(void); // 0 if the GPU or driver lacks what this layer needs (bindless descriptors...)
int gpu_format_supported(GpuFormat format); // sampled textures of this format (BC needs a desktop GPU)

// What a level of a texture of this format, `width` x `height`, takes in bytes (blocks for BC formats).
static inline size_t gpu_format_level_size(GpuFormat format, int width, int height)
{
    switch (format)
    {
    case GPU_FORMAT_BC1:
    case GPU_FORMAT_BC1_SRGB: return (size_t)((width + 3) / 4) * (size_t)((height + 3) / 4) * 8;
    case GPU_FORMAT_BC3:
    case GPU_FORMAT_BC3_SRGB:
    case GPU_FORMAT_BC5: return (size_t)((width + 3) / 4) * (size_t)((height + 3) / 4) * 16;
    case GPU_FORMAT_RGBA16F: return (size_t)width * (size_t)height * 8;
    case GPU_FORMAT_RGBA32F: return (size_t)width * (size_t)height * 16;
    case GPU_FORMAT_R16F: return (size_t)width * (size_t)height * 2;
    case GPU_FORMAT_R8: return (size_t)width * (size_t)height;
    default: return (size_t)width * (size_t)height * 4;
    }
}

static inline int gpu_format_is_compressed(GpuFormat format)
{
    return format >= GPU_FORMAT_BC1 && format <= GPU_FORMAT_BC5;
}

Texture* gpu_create_texture(const GpuTextureDesc* desc); // NULL on failure ; destroy_texture() releases it
uint32_t gpu_texture_index(Texture* texture);

// Copies a single-sampled color texture's first level back to the CPU : tightly packed rows, top first,
// its format's pixels. Waits for the GPU : outside begin_frame()/end_frame() only (baking, captures).
int gpu_read_texture(Texture* texture, void* out_pixels, size_t size); // 0 on failure

// Static, read-only data in device memory.
GpuBuffer* gpu_create_buffer(const void* data, size_t size);
void gpu_destroy_buffer(GpuBuffer* buffer); // the GPU must be done with it
uint32_t gpu_buffer_index(GpuBuffer* buffer);

// A mesh's vertices (MeshVertex array) and 32-bit indices, as bindless buffers.
uint32_t gpu_mesh_vertices_index(Mesh* mesh);
uint32_t gpu_mesh_indices_index(Mesh* mesh);

GpuFrameAllocation gpu_frame_alloc(size_t size); // mapped == NULL if the frame's memory is exhausted

GpuPipeline* gpu_create_pipeline(const GpuPipelineDesc* desc); // NULL on failure
void gpu_destroy_pipeline(GpuPipeline* pipeline);

void gpu_begin_pass(const GpuPassDesc* pass);
void gpu_end_pass(void);
void gpu_bind_pipeline(GpuPipeline* pipeline);

// Restricts the current pass's drawing to a rectangle of its attachments, in pixels from their top-left
// corner : clip space then maps onto it (one tile of an atlas). gpu_begin_pass() resets it to the whole.
void gpu_set_viewport(int x, int y, int width, int height);
void gpu_push_constants(const void* data, int size); // size <= GPU_MAX_PUSH_CONSTANTS, multiple of 4
void gpu_draw(int vertex_count);                     // no vertex buffer : shaders use SV_VertexID
void gpu_draw_indexed(Mesh* mesh, int first_index, int index_count); // SV_VertexID = the index's value

// Hardware ray tracing (DXR 1.1 / VK_KHR_ray_query), where the GPU has it : shaders trace against the
// scene given here through ray queries, from any pass.
typedef struct
{
    Mesh* mesh;
    int first_index;
    int index_count;
    float transform[12]; // 3 rows of a row-major matrix
    int alpha_tested;    // the shader decides, per candidate hit, whether this surface stops the ray
} GpuRayInstance;

int gpu_raytracing_supported(void);

// Records this frame's scene, before the passes that trace against it ; a shader finds instance i under
// InstanceID i. Returns which entry of g_tlas[] holds it.
uint32_t gpu_build_ray_scene(const GpuRayInstance* instances, int count);

#ifdef __cplusplus
}
#endif

#endif // _GPU_H_
