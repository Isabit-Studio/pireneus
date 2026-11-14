// The backend dispatcher : picks the rendering backend once, on the first create_renderer() (the
// PIRENEUS_RENDERER environment variable, else a dialog on Windows, else the only one compiled), then
// forwards every renderer.h and gpu.h call to it. Checks shared by all backends live here too.

#include "renderer/renderer.h"
#include "renderer/renderer_backend.h"
#include "renderer/gpu.h"
#ifndef PIRENEUS_UWP
#include "renderer/vulkan/vulkan_renderer.h"
#endif
#ifdef _WIN32
#include "renderer/dx12/dx12_renderer.h"
#endif

#include <SDL3/SDL.h>
#include <stdio.h>

static const RendererBackend* backend;

#if defined(_WIN32) && !defined(PIRENEUS_UWP)
// Only asked on Windows, the one platform where there's an actual choice to make (DX12 isn't
// compiled anywhere else, see renderer_backend.h/dx12_renderer.h). Uses a native SDL message box
// rather than a terminal prompt, so it also works for GUI-subsystem builds with no console. Enter
// picks the compiled-in default (PIRENEUS_DEFAULT_RENDERER), as does closing the dialog (Escape /
// close button) or any failure to show it ; scripts/automation set PIRENEUS_RENDERER instead.
static const RendererBackend* prompt_backend_choice(void)
{
    enum { CHOICE_VULKAN = 1, CHOICE_DX12 = 2 };

    int default_is_dx12 = SDL_strcmp(PIRENEUS_DEFAULT_RENDERER, "dx12") == 0;
    SDL_MessageBoxButtonFlags default_flags = SDL_MESSAGEBOX_BUTTON_RETURNKEY_DEFAULT | SDL_MESSAGEBOX_BUTTON_ESCAPEKEY_DEFAULT;

    SDL_MessageBoxButtonData buttons[2];
    buttons[0].flags = default_is_dx12 ? 0 : default_flags;
    buttons[0].buttonID = CHOICE_VULKAN;
    buttons[0].text = default_is_dx12 ? "Vulkan" : "Vulkan (default)";
    buttons[1].flags = default_is_dx12 ? default_flags : 0;
    buttons[1].buttonID = CHOICE_DX12;
    buttons[1].text = default_is_dx12 ? "DirectX 12 (default)" : "DirectX 12";

    SDL_MessageBoxData data;
    SDL_zero(data);
    data.flags = SDL_MESSAGEBOX_INFORMATION | SDL_MESSAGEBOX_BUTTONS_LEFT_TO_RIGHT;
    data.window = NULL;
    data.title = "Pireneus";
    data.message = "Select a rendering backend:";
    data.numbuttons = 2;
    data.buttons = buttons;

    int choice = -1;

    if (!SDL_ShowMessageBox(&data, &choice))
    {
        printf("Unable to show the backend selection dialog (%s), using the default backend.\n", SDL_GetError());
    }

    if (choice == CHOICE_VULKAN)
    {
        return vulkan_get_backend();
    }

    if (choice == CHOICE_DX12)
    {
        return dx12_get_backend();
    }

    return default_is_dx12 ? dx12_get_backend() : vulkan_get_backend();
}
#endif

// Picks the backend to use, once, the first time create_renderer() is called. PIRENEUS_RENDERER
// (an environment variable, same name/values as the PIRENEUS_RENDERER CMake variable which only
// sets the compiled-in default via PIRENEUS_DEFAULT_RENDERER) lets scripts/automation skip the
// prompt entirely ; interactive runs without it get a small dialog box instead (Windows only,
// the one platform where more than one backend is ever compiled in). Vulkan is always the eventual
// fallback : it's the one backend guaranteed compiled on every platform this binary could target.
static const RendererBackend* select_backend(void)
{
#ifdef PIRENEUS_UWP
    return dx12_get_backend(); // the only graphics API a UWP app has
#else
    const char* requested = SDL_getenv("PIRENEUS_RENDERER");

    if (requested)
    {
        if (SDL_strcmp(requested, "vulkan") == 0)
        {
            return vulkan_get_backend();
        }

#ifdef _WIN32
        if (SDL_strcmp(requested, "dx12") == 0)
        {
            return dx12_get_backend();
        }
#endif

        printf("Unknown or unavailable PIRENEUS_RENDERER backend '%s', falling back to Vulkan.\n", requested);
        return vulkan_get_backend();
    }

#ifdef _WIN32
    return prompt_backend_choice();
#else
    return vulkan_get_backend();
#endif
#endif // PIRENEUS_UWP
}

int create_renderer(Window* window)
{
    backend = select_backend();
    return backend->create_renderer(window);
}

void destroy_renderer(void)
{
    backend->destroy_renderer();
}

void wait_renderer_idle(void)
{
    backend->wait_renderer_idle();
}

int begin_frame(void)
{
    return backend->begin_frame();
}

void end_frame(void)
{
    backend->end_frame();
}

Texture* create_texture(const char* file_path, int* out_width, int* out_height)
{
    return backend->create_texture(file_path, out_width, out_height);
}

void destroy_texture(Texture* texture)
{
    backend->destroy_texture(texture);
}

Texture* white_texture(void)
{
    return backend->white_texture();
}

void texture_get_size(Texture* texture, int* out_width, int* out_height)
{
    backend->texture_get_size(texture, out_width, out_height);
}

Texture* create_texture_from_pixels(const void* rgba_pixels, int width, int height)
{
    if (!rgba_pixels || width <= 0 || height <= 0)
    {
        return NULL;
    }

    return backend->create_texture_from_pixels(rgba_pixels, width, height);
}

RenderTarget* create_render_target(int width, int height)
{
    return backend->create_render_target(width, height);
}

void destroy_render_target(RenderTarget* target)
{
    backend->destroy_render_target(target);
}

Texture* render_target_texture(RenderTarget* target)
{
    return backend->render_target_texture(target);
}

int begin_render_target_pass(RenderTarget* target)
{
    return backend->begin_render_target_pass(target);
}

void end_render_target_pass(void)
{
    backend->end_render_target_pass();
}

void draw_vertices(const Vertex* vertices, int vertex_count, Texture* texture)
{
    backend->draw_vertices(vertices, vertex_count, texture);
}

Mesh* create_mesh(const MeshVertex* vertices, int vertex_count, const uint32_t* indices, int index_count)
{
    if (!vertices || !indices || vertex_count <= 0 || index_count <= 0)
    {
        return NULL;
    }

    return backend->create_mesh(vertices, vertex_count, indices, index_count);
}

void destroy_mesh(Mesh* mesh)
{
    if (mesh)
    {
        backend->destroy_mesh(mesh);
    }
}

ShadowMap* create_shadow_map(int size)
{
    if (size <= 0)
    {
        return NULL;
    }

    return backend->create_shadow_map(size);
}

void destroy_shadow_map(ShadowMap* shadow_map)
{
    if (shadow_map)
    {
        backend->destroy_shadow_map(shadow_map);
    }
}

int begin_shadow_pass(ShadowMap* shadow_map, const float light_view_projection[16])
{
    if (!shadow_map || !light_view_projection)
    {
        return 0;
    }

    return backend->begin_shadow_pass(shadow_map, light_view_projection);
}

void end_shadow_pass(void)
{
    backend->end_shadow_pass();
}

void begin_scene(const SceneLighting* lighting, ShadowMap* shadow_map)
{
    backend->begin_scene(lighting, shadow_map);
}

void end_scene(void)
{
    backend->end_scene();
}

// A model matrix is affine : its bottom row, always (0, 0, 0, 1), carries the color to the shader
// instead, at no cost in constants (they're full, see mesh_constants.h).
static void draw_mesh_with(Mesh* mesh, const float model[16], float r, float g, float b, float vertex_colors, int first_index, int index_count)
{
    if (mesh && model)
    {
        float packed[16];
        SDL_memcpy(packed, model, sizeof(float) * 12);
        packed[12] = r;
        packed[13] = g;
        packed[14] = b;
        packed[15] = vertex_colors;

        backend->draw_mesh(mesh, packed, first_index, index_count);
    }
}

void draw_mesh(Mesh* mesh, const float model[16])
{
    draw_mesh_with(mesh, model, 1.0f, 1.0f, 1.0f, 1.0f, 0, -1);
}

void draw_mesh_colored(Mesh* mesh, const float model[16], const float color[3])
{
    draw_mesh_with(mesh, model, color[0], color[1], color[2], 0.0f, 0, -1);
}

void draw_mesh_range(Mesh* mesh, const float model[16], int first_index, int index_count)
{
    if (index_count > 0)
    {
        draw_mesh_with(mesh, model, 1.0f, 1.0f, 1.0f, 1.0f, first_index, index_count);
    }
}

void draw_skybox(Texture* equirectangular, float intensity)
{
    if (equirectangular)
    {
        backend->draw_skybox(equirectangular, intensity);
    }
}

int gpu_supported(void)
{
    return backend->gpu_supported();
}

Texture* gpu_create_texture(const GpuTextureDesc* desc)
{
    if (!desc || desc->width <= 0 || desc->height <= 0 || desc->mip_count < 1 || desc->mip_count > GPU_MAX_MIPS)
    {
        return NULL;
    }

    return backend->gpu_create_texture(desc);
}

uint32_t gpu_texture_index(Texture* texture)
{
    return backend->gpu_texture_index(texture);
}

GpuBuffer* gpu_create_buffer(const void* data, size_t size)
{
    return data && size > 0 ? backend->gpu_create_buffer(data, size) : NULL;
}

void gpu_destroy_buffer(GpuBuffer* buffer)
{
    if (buffer)
    {
        backend->gpu_destroy_buffer(buffer);
    }
}

uint32_t gpu_buffer_index(GpuBuffer* buffer)
{
    return backend->gpu_buffer_index(buffer);
}

uint32_t gpu_mesh_vertices_index(Mesh* mesh)
{
    return backend->gpu_mesh_vertices_index(mesh);
}

uint32_t gpu_mesh_indices_index(Mesh* mesh)
{
    return backend->gpu_mesh_indices_index(mesh);
}

GpuFrameAllocation gpu_frame_alloc(size_t size)
{
    return backend->gpu_frame_alloc(size);
}

GpuPipeline* gpu_create_pipeline(const GpuPipelineDesc* desc)
{
    return backend->gpu_create_pipeline(desc);
}

void gpu_destroy_pipeline(GpuPipeline* pipeline)
{
    if (pipeline)
    {
        backend->gpu_destroy_pipeline(pipeline);
    }
}

void gpu_begin_pass(const GpuPassDesc* pass)
{
    backend->gpu_begin_pass(pass);
}

void gpu_end_pass(void)
{
    backend->gpu_end_pass();
}

void gpu_bind_pipeline(GpuPipeline* pipeline)
{
    backend->gpu_bind_pipeline(pipeline);
}

void gpu_push_constants(const void* data, int size)
{
    backend->gpu_push_constants(data, size);
}

void gpu_draw(int vertex_count)
{
    backend->gpu_draw(vertex_count);
}

int gpu_raytracing_supported(void)
{
    return backend->gpu_raytracing_supported();
}

uint32_t gpu_build_ray_scene(const GpuRayInstance* instances, int count)
{
    return backend->gpu_build_ray_scene(instances, count);
}

void gpu_draw_indexed(Mesh* mesh, int first_index, int index_count)
{
    if (mesh && index_count > 0)
    {
        backend->gpu_draw_indexed(mesh, first_index, index_count);
    }
}

// Clamped once here for every backend : Vulkan requires a non-negative scissor offset, and an
// off-screen/inverted rectangle simply becomes an empty one (which clips everything).
void set_scissor(int x, int y, int width, int height)
{
    if (x < 0)
    {
        width += x;
        x = 0;
    }

    if (y < 0)
    {
        height += y;
        y = 0;
    }

    backend->set_scissor(x, y, SDL_max(0, width), SDL_max(0, height));
}
