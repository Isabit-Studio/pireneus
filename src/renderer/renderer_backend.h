#ifndef _RENDERER_BACKEND_H_
#define _RENDERER_BACKEND_H_

#include "renderer/renderer.h"
#include "renderer/gpu.h"

// One function pointer per renderer.h entry point : the explicit, compiler-checked contract every
// backend (vulkan_renderer.c, dx12_renderer.c, and any future one) must fill in entirely to expose
// itself via <backend>_get_backend(). renderer.c holds the currently selected instance of this and
// forwards every renderer.h call to it, which is what lets multiple backends coexist in the same
// binary and be chosen at runtime instead of at CMake-configure time.
typedef struct
{
    int (*create_renderer)(Window*);
    void (*destroy_renderer)(void);
    void (*wait_renderer_idle)(void);

    int (*begin_frame)(void);
    void (*end_frame)(void);

    Texture* (*create_texture)(const char*, int*, int*);
    void (*destroy_texture)(Texture*);
    Texture* (*white_texture)(void);
    void (*texture_get_size)(Texture*, int*, int*);
    Texture* (*create_texture_from_pixels)(const void*, int, int);

    RenderTarget* (*create_render_target)(int, int);
    void (*destroy_render_target)(RenderTarget*);
    Texture* (*render_target_texture)(RenderTarget*);

    int (*begin_render_target_pass)(RenderTarget*);
    void (*end_render_target_pass)(void);

    void (*draw_vertices)(const Vertex*, int, Texture*);
    void (*set_scissor)(int, int, int, int); // already clamped to x, y, width, height >= 0 by renderer.c

    Mesh* (*create_mesh)(const MeshVertex*, int, const uint32_t*, int);
    void (*destroy_mesh)(Mesh*);

    // gpu.h
    int (*gpu_supported)(void);
    Texture* (*gpu_create_texture)(const GpuTextureDesc*);
    uint32_t (*gpu_texture_index)(Texture*);
    int (*gpu_read_texture)(Texture*, void*, size_t);
    GpuBuffer* (*gpu_create_buffer)(const void*, size_t);
    void (*gpu_destroy_buffer)(GpuBuffer*);
    uint32_t (*gpu_buffer_index)(GpuBuffer*);
    uint32_t (*gpu_mesh_vertices_index)(Mesh*);
    uint32_t (*gpu_mesh_indices_index)(Mesh*);
    GpuFrameAllocation (*gpu_frame_alloc)(size_t);
    GpuPipeline* (*gpu_create_pipeline)(const GpuPipelineDesc*);
    void (*gpu_destroy_pipeline)(GpuPipeline*);
    void (*gpu_begin_pass)(const GpuPassDesc*);
    void (*gpu_end_pass)(void);
    void (*gpu_bind_pipeline)(GpuPipeline*);
    void (*gpu_set_viewport)(int, int, int, int);
    void (*gpu_push_constants)(const void*, int);
    void (*gpu_draw)(int);
    void (*gpu_draw_indexed)(Mesh*, int, int);
    int (*gpu_raytracing_supported)(void);
    int (*gpu_format_supported)(GpuFormat format);
    uint32_t (*gpu_build_ray_scene)(const GpuRayInstance*, int);

    // Statistics : fills only what the backend knows (backend, gpu_name, gpu_frame_ms, vram_*).
    void (*get_stats)(RendererStats*);
} RendererBackend;

#endif // _RENDERER_BACKEND_H_
