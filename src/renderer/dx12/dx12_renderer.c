#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <windows.h>

// C (not C++) vtable-struct bindings for the D3D12/DXGI COM interfaces, so we can call
// e.g. ID3D12Device_CreateCommandQueue(device, ...) instead of device->lpVtbl->CreateCommandQueue(...).
#define COBJMACROS
#include <d3d12.h>
#include <dxgi1_6.h>

#include <SDL3/SDL.h>
#include <SDL3_image/SDL_image.h>

#include <stdio.h>
#include <stdint.h>
#include <limits.h>
#include <stddef.h>

#include "renderer/renderer.h"
#include "renderer/ortho.h"
#include "renderer/mesh_constants.h"
#include "renderer/dx12/dx12_renderer.h"
#include "debug.h"

// Forward declaration : called internally (resize_if_needed, upload_texture_pixels,
// end_render_target_pass) before its own definition further down this file.
void dx12_wait_renderer_idle(void);

#define FRAME_COUNT 2
#define VERTEX_BUFFER_SIZE (4 * 1024 * 1024)
#define MAX_TEXTURES 8192 // the shader-visible heap : textures and buffers alike (the bindless table, see gpu.h)
#define GPU_FRAME_BUFFER_SIZE (8 * 1024 * 1024)
#define SHADER_READ_STATE (D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE | D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE)

struct Texture
{
    ID3D12Resource* resource;
    UINT srv_index;
    UINT width, height;
    int is_depth;
    ID3D12DescriptorHeap* target_heap;            // gpu_create_texture() targets : a dedicated 1-entry RTV/DSV heap
    D3D12_CPU_DESCRIPTOR_HANDLE target_handle;
};

struct GpuBuffer
{
    ID3D12Resource* resource;
    UINT srv_index;
};

struct GpuPipeline
{
    ID3D12PipelineState* state;
};

struct RenderTarget
{
    Texture texture; // same shape as a regular texture, so it can be sampled the same way
    ID3D12DescriptorHeap* rtv_heap; // dedicated 1-entry heap
    D3D12_CPU_DESCRIPTOR_HANDLE rtv_handle;
    int width, height;
};

// A bottom-level acceleration structure : one range of a mesh's triangles.
typedef struct
{
    UINT first_index;
    UINT index_count;
    ID3D12Resource* buffer;
} Blas;

struct Mesh
{
    // Default heap : the photoreal renderer's shading pass reads vertices for every pixel.
    ID3D12Resource* vertex_buffer;
    ID3D12Resource* index_buffer;
    D3D12_VERTEX_BUFFER_VIEW vertex_view;
    D3D12_INDEX_BUFFER_VIEW index_view;
    UINT vertices_srv;
    UINT indices_srv;
    UINT index_count;
    UINT vertex_count;
    Blas* blas;
    int blas_count;
};

struct ShadowMap
{
    Texture texture; // R32_TYPELESS resource : DSV (D32_FLOAT) while rendered, SRV (R32_FLOAT) while sampled
    ID3D12DescriptorHeap* dsv_heap; // dedicated 1-entry heap
    D3D12_CPU_DESCRIPTOR_HANDLE dsv_handle;
    int size;
};

#ifdef PIRENEUS_UWP
static IUnknown* core_window; // the CoreWindow, as its COM interface
#else
static SDL_Window* window;
static HWND hwnd;
#endif

static ID3D12Device* device;
static ID3D12CommandQueue* command_queue;
static IDXGISwapChain3* swapchain;
static UINT swapchain_width;
static UINT swapchain_height;

static ID3D12DescriptorHeap* rtv_heap;
static UINT rtv_descriptor_size;
static ID3D12Resource* render_targets[FRAME_COUNT];

static ID3D12DescriptorHeap* srv_heap;
static UINT srv_descriptor_size;
static UINT next_srv_index;
// Indices freed by destroy_texture()/destroy_render_target(), reused before growing next_srv_index :
// without this, a render target recreated on every resize (see pixel-perfect's update_render_target())
// would permanently burn through the srv_heap's MAX_TEXTURES slots within a single resize drag.
static UINT free_srv_indices[MAX_TEXTURES];
static UINT free_srv_index_count;

static ID3D12CommandAllocator* command_allocators[FRAME_COUNT];
static ID3D12GraphicsCommandList* command_list;

// Whichever command list draw_vertices() currently records into: `command_list` (the window
// pass) or `render_target_command_list` (a render-target pass).
static ID3D12GraphicsCommandList* active_command_list;

static ID3D12RootSignature* root_signature;
static ID3D12PipelineState* pipeline_state;

// --- 3D (see begin_scene / begin_shadow_pass) ---
static ID3D12Resource* depth_buffer; // the window pass's depth buffer, recreated with the swapchain
static ID3D12DescriptorHeap* dsv_heap;
static D3D12_CPU_DESCRIPTOR_HANDLE dsv_handle;
static ID3D12RootSignature* mesh_root_signature; // 60 root constants (MeshConstants) + the shadow map's SRV table
static ID3D12PipelineState* mesh_pipeline_state;
static ID3D12PipelineState* shadow_pipeline_state;
static ID3D12PipelineState* sky_pipeline_state; // full-screen triangle at the far plane, no input layout
static Texture* scene_pass_texture; // what begin_scene() bound (shadow map), restored after draw_skybox()
static ShadowMap* active_shadow_map; // during begin_shadow_pass()/end_shadow_pass()
static MeshConstants mesh_constants; // the current pass's constants ; draw_mesh() only changes `model`

static ID3D12Resource* vertex_buffers[FRAME_COUNT];
static void* vertex_buffer_mapped[FRAME_COUNT];
static UINT vertex_write_offset; // in Vertex units, within the current frame's buffer

// Render-target passes are fully synchronous (submitted and waited on before returning, see
// end_render_target_pass), so a single, non-double-buffered vertex buffer is enough : by the time
// it's reused next frame, the GPU has long finished reading it.
static ID3D12CommandAllocator* render_target_allocator;
static ID3D12GraphicsCommandList* render_target_command_list;
static ID3D12Resource* render_target_vertex_buffer;
static void* render_target_vertex_buffer_mapped;
static UINT render_target_vertex_write_offset;
static int rendering_to_target;
static RenderTarget* active_render_target;

// A single fence, signaled with one strictly increasing counter : next_fence_value is always greater
// than every value signaled so far, so a wait on any recorded value can never be satisfied early by
// an older (or repeated) signal. frame_fence_values[i] is the value signaled right after the last
// submission that used command_allocators[i] (0 = never used, trivially complete), i.e. what must be
// reached before that allocator can be reset again.
static ID3D12Fence* fence;
static UINT64 next_fence_value = 1;
static UINT64 frame_fence_values[FRAME_COUNT];
static HANDLE fence_event;

static UINT frame_index;

static Texture built_in_white_texture;

// --- Bindless GPU layer (see gpu.h) ---
static ID3D12RootSignature* gpu_root_signature;
static ID3D12Resource* gpu_frame_buffers[FRAME_COUNT];
static void* gpu_frame_mapped[FRAME_COUNT];
static UINT gpu_frame_srv[FRAME_COUNT];
static UINT gpu_frame_offset;

// Hardware ray tracing : one top-level structure per frame in flight, of a fixed capacity.
#define MAX_RAY_INSTANCES 4096

static ID3D12Device5* rt_device;  // NULL without DXR 1.1
static ID3D12Resource* tlas_buffers[FRAME_COUNT];
static ID3D12Resource* tlas_instances[FRAME_COUNT];
static void* tlas_instances_mapped[FRAME_COUNT];
static ID3D12Resource* tlas_scratch;
static UINT tlas_srv_base;        // FRAME_COUNT consecutive descriptors

// The attachments of the current gpu_begin_pass(), transitioned back by gpu_end_pass().
static Texture* gpu_pass_colors[GPU_MAX_COLOR_TARGETS];
static int gpu_pass_color_count;
static Texture* gpu_pass_depth;

#ifndef PIRENEUS_UWP
static int get_hwnd(void)
{
    SDL_PropertiesID props = SDL_GetWindowProperties(window);
    hwnd = (HWND)SDL_GetPointerProperty(props, SDL_PROP_WINDOW_WIN32_HWND_POINTER, NULL);

    if (!hwnd)
    {
        printf("Could not get the native Win32 window handle from SDL: %s\n", SDL_GetError());
        return 0;
    }

    return 1;
}
#endif

static int create_device(void)
{
    UINT factory_flags = 0;

#ifndef NDEBUG
    // Free debugging: unlike the Vulkan validation layers, the D3D12 debug layer ships with
    // the Windows SDK, so enabling it costs no extra dependency. It silently no-ops if the
    // optional "Graphics Tools" Windows feature isn't installed.
    ID3D12Debug* debug_controller = NULL;

    if (SUCCEEDED(D3D12GetDebugInterface(&IID_ID3D12Debug, (void**)&debug_controller)))
    {
        ID3D12Debug_EnableDebugLayer(debug_controller);
        ID3D12Debug_Release(debug_controller);
        factory_flags |= DXGI_CREATE_FACTORY_DEBUG;
    }
#endif

    IDXGIFactory6* factory = NULL;

    if (FAILED(CreateDXGIFactory2(factory_flags, &IID_IDXGIFactory6, (void**)&factory)))
    {
        printf("Could not create the DXGI factory.\n");
        return 0;
    }

    IDXGIAdapter1* adapter = NULL;
    DXGI_ADAPTER_DESC1 chosen_adapter_desc = { 0 };

    for (UINT i = 0; SUCCEEDED(IDXGIFactory6_EnumAdapterByGpuPreference(factory, i,
        DXGI_GPU_PREFERENCE_HIGH_PERFORMANCE, &IID_IDXGIAdapter1, (void**)&adapter)); i++)
    {
        DXGI_ADAPTER_DESC1 desc;
        IDXGIAdapter1_GetDesc1(adapter, &desc);

        if (!(desc.Flags & DXGI_ADAPTER_FLAG_SOFTWARE) &&
            SUCCEEDED(D3D12CreateDevice((IUnknown*)adapter, D3D_FEATURE_LEVEL_11_0, &IID_ID3D12Device, (void**)&device)))
        {
            chosen_adapter_desc = desc;
            IDXGIAdapter1_Release(adapter);
            adapter = NULL;
            break;
        }

        IDXGIAdapter1_Release(adapter);
        adapter = NULL;
    }

    IDXGIFactory6_Release(factory);

    if (!device)
    {
        printf("No Direct3D 12 capable GPU found.\n");
        return 0;
    }

    DEBUG_LOG("[dx12] using adapter: %ls\n", chosen_adapter_desc.Description);

    // Feature level 12_2 is the official shorthand for "meets every DirectX 12 Ultimate
    // requirement" (raytracing tier 1.1, mesh shaders, sampler feedback, variable rate
    // shading tier 2): checking it is simpler and more future-proof than querying each
    // D3D12_FEATURE_DATA_D3D12_OPTIONS* tier by hand.
    static const D3D_FEATURE_LEVEL candidate_levels[] = {
        D3D_FEATURE_LEVEL_11_0, D3D_FEATURE_LEVEL_11_1,
        D3D_FEATURE_LEVEL_12_0, D3D_FEATURE_LEVEL_12_1, D3D_FEATURE_LEVEL_12_2
    };

    D3D12_FEATURE_DATA_FEATURE_LEVELS feature_levels = {
        .NumFeatureLevels = ARRAYSIZE(candidate_levels),
        .pFeatureLevelsRequested = candidate_levels
    };

    if (SUCCEEDED(ID3D12Device_CheckFeatureSupport(device, D3D12_FEATURE_FEATURE_LEVELS, &feature_levels, sizeof(feature_levels))))
    {
        if (feature_levels.MaxSupportedFeatureLevel >= D3D_FEATURE_LEVEL_12_2)
        {
            printf("DirectX 12 Ultimate is supported by this GPU/driver.\n");
        }
        else
        {
            printf("This GPU/driver does not meet the DirectX 12 Ultimate feature bar (running plain DirectX 12).\n");
        }
    }

    return 1;
}

static int create_command_queue(void)
{
    D3D12_COMMAND_QUEUE_DESC desc = {
        .Type = D3D12_COMMAND_LIST_TYPE_DIRECT
    };

    if (FAILED(ID3D12Device_CreateCommandQueue(device, &desc, &IID_ID3D12CommandQueue, (void**)&command_queue)))
    {
        printf("Could not create the Direct3D 12 command queue.\n");
        return 0;
    }

    return 1;
}

static int create_swapchain(void)
{
    int width = 0;
    int height = 0;
    get_window_size(&width, &height);
    swapchain_width = (UINT)width;
    swapchain_height = (UINT)height;

    IDXGIFactory6* factory = NULL;

    if (FAILED(CreateDXGIFactory2(0, &IID_IDXGIFactory6, (void**)&factory)))
    {
        printf("Could not create the DXGI factory for the swapchain.\n");
        return 0;
    }

    DXGI_SWAP_CHAIN_DESC1 desc = {
        .Width = swapchain_width,
        .Height = swapchain_height,
        .Format = DXGI_FORMAT_R8G8B8A8_UNORM,
        .SampleDesc.Count = 1,
        .BufferUsage = DXGI_USAGE_RENDER_TARGET_OUTPUT,
        .BufferCount = FRAME_COUNT,
        .SwapEffect = DXGI_SWAP_EFFECT_FLIP_DISCARD
    };

    IDXGISwapChain1* swapchain1 = NULL;

#ifdef PIRENEUS_UWP
    desc.Scaling = DXGI_SCALING_STRETCH;

    if (FAILED(IDXGIFactory6_CreateSwapChainForCoreWindow(factory, (IUnknown*)command_queue, core_window, &desc, NULL, &swapchain1)))
    {
        printf("Could not create the DXGI swapchain.\n");
        IDXGIFactory6_Release(factory);
        return 0;
    }
#else
    if (FAILED(IDXGIFactory6_CreateSwapChainForHwnd(factory, (IUnknown*)command_queue, hwnd, &desc, NULL, NULL, &swapchain1)))
    {
        printf("Could not create the DXGI swapchain.\n");
        IDXGIFactory6_Release(factory);
        return 0;
    }

    IDXGIFactory6_MakeWindowAssociation(factory, hwnd, DXGI_MWA_NO_ALT_ENTER);
#endif

    IDXGIFactory6_Release(factory);

    if (FAILED(IDXGISwapChain1_QueryInterface(swapchain1, &IID_IDXGISwapChain3, (void**)&swapchain)))
    {
        printf("Could not query the IDXGISwapChain3 interface.\n");
        IDXGISwapChain1_Release(swapchain1);
        return 0;
    }

    IDXGISwapChain1_Release(swapchain1);

    frame_index = IDXGISwapChain3_GetCurrentBackBufferIndex(swapchain);

    return 1;
}

static int create_render_targets(void)
{
    if (!rtv_heap)
    {
        D3D12_DESCRIPTOR_HEAP_DESC heap_desc = {
            .Type = D3D12_DESCRIPTOR_HEAP_TYPE_RTV,
            .NumDescriptors = FRAME_COUNT
        };

        if (FAILED(ID3D12Device_CreateDescriptorHeap(device, &heap_desc, &IID_ID3D12DescriptorHeap, (void**)&rtv_heap)))
        {
            printf("Could not create the RTV descriptor heap.\n");
            return 0;
        }

        rtv_descriptor_size = ID3D12Device_GetDescriptorHandleIncrementSize(device, D3D12_DESCRIPTOR_HEAP_TYPE_RTV);
    }

    D3D12_CPU_DESCRIPTOR_HANDLE rtv_handle;
    ID3D12DescriptorHeap_GetCPUDescriptorHandleForHeapStart(rtv_heap, &rtv_handle);

    for (UINT i = 0; i < FRAME_COUNT; i++)
    {
        if (FAILED(IDXGISwapChain3_GetBuffer(swapchain, i, &IID_ID3D12Resource, (void**)&render_targets[i])))
        {
            printf("Could not get swapchain buffer %u.\n", i);
            return 0;
        }

        ID3D12Device_CreateRenderTargetView(device, render_targets[i], NULL, rtv_handle);
        rtv_handle.ptr += rtv_descriptor_size;
    }

    return 1;
}

static int create_srv_heap(void)
{
    D3D12_DESCRIPTOR_HEAP_DESC heap_desc = {
        .Type = D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV,
        .NumDescriptors = MAX_TEXTURES,
        .Flags = D3D12_DESCRIPTOR_HEAP_FLAG_SHADER_VISIBLE
    };

    if (FAILED(ID3D12Device_CreateDescriptorHeap(device, &heap_desc, &IID_ID3D12DescriptorHeap, (void**)&srv_heap)))
    {
        printf("Could not create the SRV descriptor heap.\n");
        return 0;
    }

    srv_descriptor_size = ID3D12Device_GetDescriptorHandleIncrementSize(device, D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV);

    return 1;
}

static D3D12_CPU_DESCRIPTOR_HANDLE srv_cpu_handle(UINT index)
{
    D3D12_CPU_DESCRIPTOR_HANDLE handle;
    ID3D12DescriptorHeap_GetCPUDescriptorHandleForHeapStart(srv_heap, &handle);
    handle.ptr += (SIZE_T)index * srv_descriptor_size;
    return handle;
}

static D3D12_GPU_DESCRIPTOR_HANDLE srv_gpu_handle(UINT index)
{
    D3D12_GPU_DESCRIPTOR_HANDLE handle;
    ID3D12DescriptorHeap_GetGPUDescriptorHandleForHeapStart(srv_heap, &handle);
    handle.ptr += (UINT64)index * srv_descriptor_size;
    return handle;
}

static UINT allocate_srv_index(void)
{
    if (free_srv_index_count > 0)
    {
        return free_srv_indices[--free_srv_index_count];
    }

    if (next_srv_index >= MAX_TEXTURES)
    {
        printf("Direct3D 12 SRV heap is full (%u textures/render targets already live).\n", MAX_TEXTURES);
        return next_srv_index; // still returns an (invalid) index rather than crash-checking every caller
    }

    return next_srv_index++;
}

static void release_srv_index(UINT index)
{
    free_srv_indices[free_srv_index_count++] = index;
}

static int create_commands(void)
{
    for (UINT i = 0; i < FRAME_COUNT; i++)
    {
        if (FAILED(ID3D12Device_CreateCommandAllocator(device, D3D12_COMMAND_LIST_TYPE_DIRECT,
            &IID_ID3D12CommandAllocator, (void**)&command_allocators[i])))
        {
            printf("Could not create a Direct3D 12 command allocator.\n");
            return 0;
        }
    }

    if (FAILED(ID3D12Device_CreateCommandList(device, 0, D3D12_COMMAND_LIST_TYPE_DIRECT,
        command_allocators[frame_index], NULL, &IID_ID3D12GraphicsCommandList, (void**)&command_list)))
    {
        printf("Could not create the Direct3D 12 command list.\n");
        return 0;
    }

    if (FAILED(ID3D12GraphicsCommandList_Close(command_list)))
    {
        printf("Could not close the initial Direct3D 12 command list.\n");
        return 0;
    }

    return 1;
}

// Both root signatures share one shape : root constants at b0 (parameter 0), one SRV table at t0
// (parameter 1, pixel shader) and a static point/clamp sampler at s0 — the layout Slang generates
// for sprite.slang and mesh.slang alike.
static int create_root_signature_with(UINT constant_count, D3D12_SHADER_VISIBILITY constants_visibility, ID3D12RootSignature** out_signature)
{
    D3D12_ROOT_PARAMETER parameters[2] = { 0 };

    parameters[0].ParameterType = D3D12_ROOT_PARAMETER_TYPE_32BIT_CONSTANTS;
    parameters[0].Constants.ShaderRegister = 0;
    parameters[0].Constants.Num32BitValues = constant_count;
    parameters[0].ShaderVisibility = constants_visibility;

    D3D12_DESCRIPTOR_RANGE srv_range = {
        .RangeType = D3D12_DESCRIPTOR_RANGE_TYPE_SRV,
        .NumDescriptors = 1,
        .BaseShaderRegister = 0,
        .OffsetInDescriptorsFromTableStart = D3D12_DESCRIPTOR_RANGE_OFFSET_APPEND
    };

    parameters[1].ParameterType = D3D12_ROOT_PARAMETER_TYPE_DESCRIPTOR_TABLE;
    parameters[1].DescriptorTable.NumDescriptorRanges = 1;
    parameters[1].DescriptorTable.pDescriptorRanges = &srv_range;
    parameters[1].ShaderVisibility = D3D12_SHADER_VISIBILITY_PIXEL;

    D3D12_STATIC_SAMPLER_DESC sampler = {
        // Point/nearest filtering: the tileset is a pixel-art atlas, and linear filtering
        // would bleed adjacent tiles into each other at their edges. Shadow maps are sampled
        // point-wise too (mesh.slang does its own depth comparison and PCF).
        .Filter = D3D12_FILTER_MIN_MAG_MIP_POINT,
        .AddressU = D3D12_TEXTURE_ADDRESS_MODE_CLAMP,
        .AddressV = D3D12_TEXTURE_ADDRESS_MODE_CLAMP,
        .AddressW = D3D12_TEXTURE_ADDRESS_MODE_CLAMP,
        .ComparisonFunc = D3D12_COMPARISON_FUNC_ALWAYS,
        .ShaderRegister = 0,
        .ShaderVisibility = D3D12_SHADER_VISIBILITY_PIXEL
    };

    D3D12_ROOT_SIGNATURE_DESC desc = {
        .NumParameters = 2,
        .pParameters = parameters,
        .NumStaticSamplers = 1,
        .pStaticSamplers = &sampler,
        .Flags = D3D12_ROOT_SIGNATURE_FLAG_ALLOW_INPUT_ASSEMBLER_INPUT_LAYOUT
    };

    ID3DBlob* signature_blob = NULL;
    ID3DBlob* error_blob = NULL;

    if (FAILED(D3D12SerializeRootSignature(&desc, D3D_ROOT_SIGNATURE_VERSION_1, &signature_blob, &error_blob)))
    {
        if (error_blob)
        {
            printf("Could not serialize a root signature: %s\n", (const char*)ID3D10Blob_GetBufferPointer(error_blob));
            ID3D10Blob_Release(error_blob);
        }
        else
        {
            printf("Could not serialize a root signature.\n");
        }

        return 0;
    }

    HRESULT hr = ID3D12Device_CreateRootSignature(device, 0,
        ID3D10Blob_GetBufferPointer(signature_blob), ID3D10Blob_GetBufferSize(signature_blob),
        &IID_ID3D12RootSignature, (void**)out_signature);

    ID3D10Blob_Release(signature_blob);

    if (FAILED(hr))
    {
        printf("Could not create a root signature.\n");
        return 0;
    }

    return 1;
}

// The bindless layout (gpu.h) : push constants, then two unbounded tables over the whole heap (one per
// resource kind, at t0 of their own register space, see photoreal.slang), and the fixed samplers.
static int create_gpu_root_signature(void)
{
    D3D12_DESCRIPTOR_RANGE texture_range = { D3D12_DESCRIPTOR_RANGE_TYPE_SRV, UINT_MAX, 0, 1, 0 };
    D3D12_DESCRIPTOR_RANGE buffer_range = { D3D12_DESCRIPTOR_RANGE_TYPE_SRV, UINT_MAX, 0, 2, 0 };
    D3D12_DESCRIPTOR_RANGE tlas_range = { D3D12_DESCRIPTOR_RANGE_TYPE_SRV, FRAME_COUNT, 0, 3, 0 };

    D3D12_ROOT_PARAMETER parameters[4] = { 0 };

    parameters[0].ParameterType = D3D12_ROOT_PARAMETER_TYPE_32BIT_CONSTANTS;
    parameters[0].Constants.Num32BitValues = GPU_MAX_PUSH_CONSTANTS / 4;
    parameters[0].ShaderVisibility = D3D12_SHADER_VISIBILITY_ALL;

    parameters[1].ParameterType = D3D12_ROOT_PARAMETER_TYPE_DESCRIPTOR_TABLE;
    parameters[1].DescriptorTable.NumDescriptorRanges = 1;
    parameters[1].DescriptorTable.pDescriptorRanges = &texture_range;
    parameters[1].ShaderVisibility = D3D12_SHADER_VISIBILITY_ALL;

    parameters[2].ParameterType = D3D12_ROOT_PARAMETER_TYPE_DESCRIPTOR_TABLE;
    parameters[2].DescriptorTable.NumDescriptorRanges = 1;
    parameters[2].DescriptorTable.pDescriptorRanges = &buffer_range;
    parameters[2].ShaderVisibility = D3D12_SHADER_VISIBILITY_ALL;

    parameters[3].ParameterType = D3D12_ROOT_PARAMETER_TYPE_DESCRIPTOR_TABLE;
    parameters[3].DescriptorTable.NumDescriptorRanges = 1;
    parameters[3].DescriptorTable.pDescriptorRanges = &tlas_range;
    parameters[3].ShaderVisibility = D3D12_SHADER_VISIBILITY_ALL;

    D3D12_STATIC_SAMPLER_DESC samplers[GPU_SAMPLER_COUNT];

    for (int i = 0; i < GPU_SAMPLER_COUNT; i++)
    {
        D3D12_STATIC_SAMPLER_DESC sampler = {
            .Filter = D3D12_FILTER_MIN_MAG_MIP_LINEAR,
            .AddressU = D3D12_TEXTURE_ADDRESS_MODE_CLAMP,
            .AddressV = D3D12_TEXTURE_ADDRESS_MODE_CLAMP,
            .AddressW = D3D12_TEXTURE_ADDRESS_MODE_CLAMP,
            .MaxAnisotropy = 1,
            .ComparisonFunc = D3D12_COMPARISON_FUNC_ALWAYS,
            .MaxLOD = D3D12_FLOAT32_MAX,
            .ShaderRegister = (UINT)i,
            .RegisterSpace = 1,
            .ShaderVisibility = D3D12_SHADER_VISIBILITY_ALL
        };

        samplers[i] = sampler;
    }

    samplers[GPU_SAMPLER_POINT_CLAMP].Filter = D3D12_FILTER_MIN_MAG_MIP_POINT;
    samplers[GPU_SAMPLER_ANISOTROPIC_WRAP].Filter = D3D12_FILTER_ANISOTROPIC;
    samplers[GPU_SAMPLER_ANISOTROPIC_WRAP].MaxAnisotropy = 16;
    samplers[GPU_SAMPLER_ANISOTROPIC_WRAP].AddressU = D3D12_TEXTURE_ADDRESS_MODE_WRAP;
    samplers[GPU_SAMPLER_ANISOTROPIC_WRAP].AddressV = D3D12_TEXTURE_ADDRESS_MODE_WRAP;
    samplers[GPU_SAMPLER_ANISOTROPIC_WRAP].AddressW = D3D12_TEXTURE_ADDRESS_MODE_WRAP;
    samplers[GPU_SAMPLER_PANORAMA].AddressU = D3D12_TEXTURE_ADDRESS_MODE_WRAP;

    D3D12_ROOT_SIGNATURE_DESC desc = {
        .NumParameters = 4,
        .pParameters = parameters,
        .NumStaticSamplers = GPU_SAMPLER_COUNT,
        .pStaticSamplers = samplers
    };

    ID3DBlob* signature_blob = NULL;
    ID3DBlob* error_blob = NULL;

    if (FAILED(D3D12SerializeRootSignature(&desc, D3D_ROOT_SIGNATURE_VERSION_1, &signature_blob, &error_blob)))
    {
        printf("Could not serialize the bindless root signature: %s\n", error_blob ? (const char*)ID3D10Blob_GetBufferPointer(error_blob) : "");

        if (error_blob)
        {
            ID3D10Blob_Release(error_blob);
        }

        return 0;
    }

    HRESULT hr = ID3D12Device_CreateRootSignature(device, 0, ID3D10Blob_GetBufferPointer(signature_blob),
        ID3D10Blob_GetBufferSize(signature_blob), &IID_ID3D12RootSignature, (void**)&gpu_root_signature);

    ID3D10Blob_Release(signature_blob);

    if (FAILED(hr))
    {
        printf("Could not create the bindless root signature.\n");
        return 0;
    }

    return 1;
}

static int create_root_signature(void)
{
    return create_root_signature_with(sizeof(ScreenTransform) / sizeof(float), D3D12_SHADER_VISIBILITY_VERTEX, &root_signature) &&
        create_root_signature_with(sizeof(MeshConstants) / sizeof(float), D3D12_SHADER_VISIBILITY_ALL, &mesh_root_signature) &&
        create_gpu_root_signature();
}

static uint8_t* load_shader(const char* file_name, size_t* out_size)
{
    const char* base_path = SDL_GetBasePath();

    char path[512];
    SDL_snprintf(path, sizeof(path), "%sshaders/%s.cso", base_path ? base_path : "", file_name);

    return SDL_LoadFile(path, out_size);
}

static int create_pipeline_state(void)
{
    size_t vs_size = 0;
    size_t ps_size = 0;

    uint8_t* vs_code = load_shader("sprite_vs", &vs_size);
    uint8_t* ps_code = load_shader("sprite_ps", &ps_size);

    if (!vs_code || !ps_code)
    {
        printf("Could not load the sprite shaders.\n");
        SDL_free(vs_code);
        SDL_free(ps_code);
        return 0;
    }

    D3D12_INPUT_ELEMENT_DESC input_elements[3] = {
        { "POSITION", 0, DXGI_FORMAT_R32G32_FLOAT, 0, (UINT)offsetof(Vertex, x), D3D12_INPUT_CLASSIFICATION_PER_VERTEX_DATA, 0 },
        { "TEXCOORD", 0, DXGI_FORMAT_R32G32_FLOAT, 0, (UINT)offsetof(Vertex, u), D3D12_INPUT_CLASSIFICATION_PER_VERTEX_DATA, 0 },
        { "COLOR", 0, DXGI_FORMAT_R32G32B32A32_FLOAT, 0, (UINT)offsetof(Vertex, r), D3D12_INPUT_CLASSIFICATION_PER_VERTEX_DATA, 0 }
    };

    D3D12_GRAPHICS_PIPELINE_STATE_DESC desc = { 0 };
    desc.pRootSignature = root_signature;
    desc.VS.pShaderBytecode = vs_code;
    desc.VS.BytecodeLength = vs_size;
    desc.PS.pShaderBytecode = ps_code;
    desc.PS.BytecodeLength = ps_size;
    desc.InputLayout.pInputElementDescs = input_elements;
    desc.InputLayout.NumElements = 3;
    // 2D sprites are always axis-aligned quads facing the camera: no backface culling needed.
    desc.RasterizerState.FillMode = D3D12_FILL_MODE_SOLID;
    desc.RasterizerState.CullMode = D3D12_CULL_MODE_NONE;
    desc.RasterizerState.DepthClipEnable = TRUE;
    desc.BlendState.RenderTarget[0].BlendEnable = TRUE;
    desc.BlendState.RenderTarget[0].SrcBlend = D3D12_BLEND_SRC_ALPHA;
    desc.BlendState.RenderTarget[0].DestBlend = D3D12_BLEND_INV_SRC_ALPHA;
    desc.BlendState.RenderTarget[0].BlendOp = D3D12_BLEND_OP_ADD;
    desc.BlendState.RenderTarget[0].SrcBlendAlpha = D3D12_BLEND_ONE;
    desc.BlendState.RenderTarget[0].DestBlendAlpha = D3D12_BLEND_INV_SRC_ALPHA;
    desc.BlendState.RenderTarget[0].BlendOpAlpha = D3D12_BLEND_OP_ADD;
    desc.BlendState.RenderTarget[0].RenderTargetWriteMask = D3D12_COLOR_WRITE_ENABLE_ALL;
    desc.SampleMask = UINT_MAX;
    desc.PrimitiveTopologyType = D3D12_PRIMITIVE_TOPOLOGY_TYPE_TRIANGLE;
    desc.NumRenderTargets = 1;
    desc.RTVFormats[0] = DXGI_FORMAT_R8G8B8A8_UNORM;
    desc.SampleDesc.Count = 1;

    HRESULT hr = ID3D12Device_CreateGraphicsPipelineState(device, &desc, &IID_ID3D12PipelineState, (void**)&pipeline_state);

    SDL_free(vs_code);
    SDL_free(ps_code);

    if (FAILED(hr))
    {
        printf("Could not create the Direct3D 12 graphics pipeline state.\n");
        return 0;
    }

    return 1;
}

static int create_mesh_pipeline_states(void)
{
    size_t vs_size = 0;
    size_t ps_size = 0;
    size_t shadow_vs_size = 0;

    uint8_t* vs_code = load_shader("mesh_vs", &vs_size);
    uint8_t* ps_code = load_shader("mesh_ps", &ps_size);
    uint8_t* shadow_vs_code = load_shader("mesh_shadow_vs", &shadow_vs_size);

    if (!vs_code || !ps_code || !shadow_vs_code)
    {
        printf("Could not load the mesh shaders.\n");
        SDL_free(vs_code);
        SDL_free(ps_code);
        SDL_free(shadow_vs_code);
        return 0;
    }

    D3D12_INPUT_ELEMENT_DESC input_elements[3] = {
        { "POSITION", 0, DXGI_FORMAT_R32G32B32_FLOAT, 0, (UINT)offsetof(MeshVertex, x), D3D12_INPUT_CLASSIFICATION_PER_VERTEX_DATA, 0 },
        { "NORMAL", 0, DXGI_FORMAT_R32G32B32_FLOAT, 0, (UINT)offsetof(MeshVertex, nx), D3D12_INPUT_CLASSIFICATION_PER_VERTEX_DATA, 0 },
        { "COLOR", 0, DXGI_FORMAT_R32G32B32A32_FLOAT, 0, (UINT)offsetof(MeshVertex, r), D3D12_INPUT_CLASSIFICATION_PER_VERTEX_DATA, 0 }
    };

    D3D12_GRAPHICS_PIPELINE_STATE_DESC desc = { 0 };
    desc.pRootSignature = mesh_root_signature;
    desc.VS.pShaderBytecode = vs_code;
    desc.VS.BytecodeLength = vs_size;
    desc.PS.pShaderBytecode = ps_code;
    desc.PS.BytecodeLength = ps_size;
    desc.InputLayout.pInputElementDescs = input_elements;
    desc.InputLayout.NumElements = 3;
    // No culling : imported scenes may have open or single-sided geometry (shadow casters included).
    desc.RasterizerState.FillMode = D3D12_FILL_MODE_SOLID;
    desc.RasterizerState.CullMode = D3D12_CULL_MODE_NONE;
    desc.RasterizerState.DepthClipEnable = TRUE;
    desc.BlendState.RenderTarget[0].RenderTargetWriteMask = D3D12_COLOR_WRITE_ENABLE_ALL;
    desc.DepthStencilState.DepthEnable = TRUE;
    desc.DepthStencilState.DepthWriteMask = D3D12_DEPTH_WRITE_MASK_ALL;
    desc.DepthStencilState.DepthFunc = D3D12_COMPARISON_FUNC_LESS;
    desc.DSVFormat = DXGI_FORMAT_D32_FLOAT;
    desc.SampleMask = UINT_MAX;
    desc.PrimitiveTopologyType = D3D12_PRIMITIVE_TOPOLOGY_TYPE_TRIANGLE;
    desc.NumRenderTargets = 1;
    desc.RTVFormats[0] = DXGI_FORMAT_R8G8B8A8_UNORM;
    desc.SampleDesc.Count = 1;

    HRESULT hr = ID3D12Device_CreateGraphicsPipelineState(device, &desc, &IID_ID3D12PipelineState, (void**)&mesh_pipeline_state);

    if (SUCCEEDED(hr))
    {
        // Depth only : no pixel shader, no render target.
        desc.VS.pShaderBytecode = shadow_vs_code;
        desc.VS.BytecodeLength = shadow_vs_size;
        desc.PS.pShaderBytecode = NULL;
        desc.PS.BytecodeLength = 0;
        desc.NumRenderTargets = 0;
        desc.RTVFormats[0] = DXGI_FORMAT_UNKNOWN;

        hr = ID3D12Device_CreateGraphicsPipelineState(device, &desc, &IID_ID3D12PipelineState, (void**)&shadow_pipeline_state);
    }

    SDL_free(vs_code);
    SDL_free(ps_code);
    SDL_free(shadow_vs_code);

    // Sky : vertices generated from the vertex index (no input layout), depth tested at the far
    // plane (LESS_EQUAL against the cleared 1.0) but never written.
    size_t sky_vs_size = 0;
    size_t sky_ps_size = 0;
    uint8_t* sky_vs_code = load_shader("sky_vs", &sky_vs_size);
    uint8_t* sky_ps_code = load_shader("sky_ps", &sky_ps_size);

    if (SUCCEEDED(hr) && sky_vs_code && sky_ps_code)
    {
        desc.VS.pShaderBytecode = sky_vs_code;
        desc.VS.BytecodeLength = sky_vs_size;
        desc.PS.pShaderBytecode = sky_ps_code;
        desc.PS.BytecodeLength = sky_ps_size;
        desc.InputLayout.pInputElementDescs = NULL;
        desc.InputLayout.NumElements = 0;
        desc.NumRenderTargets = 1;
        desc.RTVFormats[0] = DXGI_FORMAT_R8G8B8A8_UNORM;
        desc.DepthStencilState.DepthWriteMask = D3D12_DEPTH_WRITE_MASK_ZERO;
        desc.DepthStencilState.DepthFunc = D3D12_COMPARISON_FUNC_LESS_EQUAL;

        hr = ID3D12Device_CreateGraphicsPipelineState(device, &desc, &IID_ID3D12PipelineState, (void**)&sky_pipeline_state);
    }
    else if (SUCCEEDED(hr))
    {
        hr = E_FAIL; // sky shaders missing
    }

    SDL_free(sky_vs_code);
    SDL_free(sky_ps_code);

    if (FAILED(hr))
    {
        printf("Could not create the Direct3D 12 mesh pipeline states.\n");
        return 0;
    }

    return 1;
}

static int create_depth_buffer(void)
{
    if (!dsv_heap)
    {
        D3D12_DESCRIPTOR_HEAP_DESC heap_desc = { .Type = D3D12_DESCRIPTOR_HEAP_TYPE_DSV, .NumDescriptors = 1 };

        if (FAILED(ID3D12Device_CreateDescriptorHeap(device, &heap_desc, &IID_ID3D12DescriptorHeap, (void**)&dsv_heap)))
        {
            printf("Could not create the DSV descriptor heap.\n");
            return 0;
        }

        ID3D12DescriptorHeap_GetCPUDescriptorHandleForHeapStart(dsv_heap, &dsv_handle);
    }

    D3D12_HEAP_PROPERTIES default_heap = { .Type = D3D12_HEAP_TYPE_DEFAULT };

    D3D12_RESOURCE_DESC desc = {
        .Dimension = D3D12_RESOURCE_DIMENSION_TEXTURE2D,
        .Width = swapchain_width,
        .Height = swapchain_height,
        .DepthOrArraySize = 1,
        .MipLevels = 1,
        .Format = DXGI_FORMAT_D32_FLOAT,
        .SampleDesc.Count = 1,
        .Layout = D3D12_TEXTURE_LAYOUT_UNKNOWN,
        .Flags = D3D12_RESOURCE_FLAG_ALLOW_DEPTH_STENCIL | D3D12_RESOURCE_FLAG_DENY_SHADER_RESOURCE
    };

    D3D12_CLEAR_VALUE clear_value = { .Format = DXGI_FORMAT_D32_FLOAT, .DepthStencil = { 1.0f, 0 } };

    if (FAILED(ID3D12Device_CreateCommittedResource(device, &default_heap, D3D12_HEAP_FLAG_NONE,
        &desc, D3D12_RESOURCE_STATE_DEPTH_WRITE, &clear_value, &IID_ID3D12Resource, (void**)&depth_buffer)))
    {
        printf("Could not create the Direct3D 12 depth buffer.\n");
        return 0;
    }

    ID3D12Device_CreateDepthStencilView(device, depth_buffer, NULL, dsv_handle);

    return 1;
}

static void release_depth_buffer(void)
{
    if (depth_buffer)
    {
        ID3D12Resource_Release(depth_buffer);
        depth_buffer = NULL;
    }
}

static D3D12_CPU_DESCRIPTOR_HANDLE current_rtv_handle(void)
{
    D3D12_CPU_DESCRIPTOR_HANDLE rtv_handle;
    ID3D12DescriptorHeap_GetCPUDescriptorHandleForHeapStart(rtv_heap, &rtv_handle);
    rtv_handle.ptr += (SIZE_T)frame_index * rtv_descriptor_size;
    return rtv_handle;
}

static int create_fence(void)
{
    if (FAILED(ID3D12Device_CreateFence(device, 0, D3D12_FENCE_FLAG_NONE, &IID_ID3D12Fence, (void**)&fence)))
    {
        printf("Could not create the Direct3D 12 fence.\n");
        return 0;
    }

    fence_event = CreateEventW(NULL, FALSE, FALSE, NULL);

    if (!fence_event)
    {
        printf("Could not create the fence event.\n");
        return 0;
    }

    return 1;
}

static void wait_for_fence_value(UINT64 value)
{
    if (ID3D12Fence_GetCompletedValue(fence) < value)
    {
        ID3D12Fence_SetEventOnCompletion(fence, value, fence_event);
        WaitForSingleObject(fence_event, INFINITE);
    }
}

// Signals the fence with a fresh value once everything submitted so far has executed, and returns
// that value (0 on failure, which any wait treats as already complete).
static UINT64 signal_fence(void)
{
    UINT64 value = next_fence_value;

    if (FAILED(ID3D12CommandQueue_Signal(command_queue, fence, value)))
    {
        return 0;
    }

    next_fence_value++;

    return value;
}

static int move_to_next_frame(void)
{
    UINT64 submitted_value = signal_fence();

    if (submitted_value == 0)
    {
        return 0;
    }

    frame_fence_values[frame_index] = submitted_value;

    // The next back buffer's allocator is reset by the next record_commands() : wait until the GPU
    // is done with the last frame that used it.
    frame_index = IDXGISwapChain3_GetCurrentBackBufferIndex(swapchain);
    wait_for_fence_value(frame_fence_values[frame_index]);

    return 1;
}

static int create_vertex_buffers(void)
{
    D3D12_HEAP_PROPERTIES heap_properties = { .Type = D3D12_HEAP_TYPE_UPLOAD };

    D3D12_RESOURCE_DESC resource_desc = {
        .Dimension = D3D12_RESOURCE_DIMENSION_BUFFER,
        .Width = VERTEX_BUFFER_SIZE,
        .Height = 1,
        .DepthOrArraySize = 1,
        .MipLevels = 1,
        .Format = DXGI_FORMAT_UNKNOWN,
        .SampleDesc.Count = 1,
        .Layout = D3D12_TEXTURE_LAYOUT_ROW_MAJOR
    };

    D3D12_RANGE no_read_range = { 0, 0 };

    for (UINT i = 0; i < FRAME_COUNT; i++)
    {
        if (FAILED(ID3D12Device_CreateCommittedResource(device, &heap_properties, D3D12_HEAP_FLAG_NONE,
            &resource_desc, D3D12_RESOURCE_STATE_GENERIC_READ, NULL, &IID_ID3D12Resource, (void**)&vertex_buffers[i])))
        {
            printf("Could not create a Direct3D 12 vertex buffer.\n");
            return 0;
        }

        if (FAILED(ID3D12Resource_Map(vertex_buffers[i], 0, &no_read_range, &vertex_buffer_mapped[i])))
        {
            printf("Could not map a Direct3D 12 vertex buffer.\n");
            return 0;
        }
    }

    if (FAILED(ID3D12Device_CreateCommittedResource(device, &heap_properties, D3D12_HEAP_FLAG_NONE,
        &resource_desc, D3D12_RESOURCE_STATE_GENERIC_READ, NULL, &IID_ID3D12Resource, (void**)&render_target_vertex_buffer)))
    {
        printf("Could not create the Direct3D 12 render-target vertex buffer.\n");
        return 0;
    }

    if (FAILED(ID3D12Resource_Map(render_target_vertex_buffer, 0, &no_read_range, &render_target_vertex_buffer_mapped)))
    {
        printf("Could not map the Direct3D 12 render-target vertex buffer.\n");
        return 0;
    }

    return 1;
}

static void release_render_targets(void)
{
    for (UINT i = 0; i < FRAME_COUNT; i++)
    {
        if (render_targets[i])
        {
            ID3D12Resource_Release(render_targets[i]);
            render_targets[i] = NULL;
        }
    }
}

// Minimum time between actual IDXGISwapChain3_ResizeBuffers calls. A fast interactive resize
// drag can fire far more WM_SIZE events than there are frames to render (each one synchronously
// draining the GPU and reallocating every swapchain buffer/RTV) ; that churn rate is what was
// crashing the DX12 backend under a fast drag (Vulkan's simpler recreate-swapchain path doesn't
// show the same fragility). Throttling to roughly one actual resize per frame keeps that churn
// bounded without limiting how often the window otherwise redraws : between resizes, begin_frame()
// just renders at the still-current swapchain size (briefly stale by at most one throttle window,
// imperceptible during a drag, and it always catches up to the final size once the drag settles).
#define SWAPCHAIN_RESIZE_MIN_INTERVAL_MS 16

static int resize_if_needed(void)
{
    int width = 0;
    int height = 0;
    get_window_size(&width, &height);

    while (width == 0 || height == 0)
    {
        get_window_size(&width, &height);
#ifndef PIRENEUS_UWP
        SDL_PumpEvents();
#endif
        SDL_Delay(10);
    }

    if ((UINT)width == swapchain_width && (UINT)height == swapchain_height)
    {
        return 1;
    }

    static Uint64 last_resize_ticks;
    Uint64 now = SDL_GetTicks();

    if (now - last_resize_ticks < SWAPCHAIN_RESIZE_MIN_INTERVAL_MS)
    {
        return 1; // too soon ; render this frame at the still-current size and try again next frame
    }

    last_resize_ticks = now;

    dx12_wait_renderer_idle();

    release_render_targets();
    release_depth_buffer();

    if (FAILED(IDXGISwapChain3_ResizeBuffers(swapchain, FRAME_COUNT, (UINT)width, (UINT)height, DXGI_FORMAT_R8G8B8A8_UNORM, 0)))
    {
        printf("Could not resize the Direct3D 12 swapchain buffers.\n");
        return 0;
    }

    swapchain_width = (UINT)width;
    swapchain_height = (UINT)height;
    frame_index = IDXGISwapChain3_GetCurrentBackBufferIndex(swapchain);

    DEBUG_LOG("[dx12] swapchain resized: %ux%u\n", swapchain_width, swapchain_height);

    return create_render_targets() && create_depth_buffer();
}

static ID3D12CommandAllocator* upload_allocator;
static ID3D12GraphicsCommandList* upload_command_list;

static int create_upload_commands(void)
{
    if (FAILED(ID3D12Device_CreateCommandAllocator(device, D3D12_COMMAND_LIST_TYPE_DIRECT,
        &IID_ID3D12CommandAllocator, (void**)&upload_allocator)))
    {
        printf("Could not create the Direct3D 12 upload command allocator.\n");
        return 0;
    }

    if (FAILED(ID3D12Device_CreateCommandList(device, 0, D3D12_COMMAND_LIST_TYPE_DIRECT,
        upload_allocator, NULL, &IID_ID3D12GraphicsCommandList, (void**)&upload_command_list)))
    {
        printf("Could not create the Direct3D 12 upload command list.\n");
        return 0;
    }

    ID3D12GraphicsCommandList_Close(upload_command_list);

    return 1;
}

static int create_render_target_commands(void)
{
    if (FAILED(ID3D12Device_CreateCommandAllocator(device, D3D12_COMMAND_LIST_TYPE_DIRECT,
        &IID_ID3D12CommandAllocator, (void**)&render_target_allocator)))
    {
        printf("Could not create the Direct3D 12 render-target command allocator.\n");
        return 0;
    }

    if (FAILED(ID3D12Device_CreateCommandList(device, 0, D3D12_COMMAND_LIST_TYPE_DIRECT,
        render_target_allocator, NULL, &IID_ID3D12GraphicsCommandList, (void**)&render_target_command_list)))
    {
        printf("Could not create the Direct3D 12 render-target command list.\n");
        return 0;
    }

    ID3D12GraphicsCommandList_Close(render_target_command_list);

    return 1;
}

static int upload_texture_pixels(Texture* texture, const uint8_t* pixels, UINT width, UINT height)
{
    D3D12_HEAP_PROPERTIES default_heap = { .Type = D3D12_HEAP_TYPE_DEFAULT };

    D3D12_RESOURCE_DESC texture_desc = {
        .Dimension = D3D12_RESOURCE_DIMENSION_TEXTURE2D,
        .Width = width,
        .Height = height,
        .DepthOrArraySize = 1,
        .MipLevels = 1,
        .Format = DXGI_FORMAT_R8G8B8A8_UNORM,
        .SampleDesc.Count = 1,
        .Layout = D3D12_TEXTURE_LAYOUT_UNKNOWN
    };

    if (FAILED(ID3D12Device_CreateCommittedResource(device, &default_heap, D3D12_HEAP_FLAG_NONE,
        &texture_desc, D3D12_RESOURCE_STATE_COPY_DEST, NULL, &IID_ID3D12Resource, (void**)&texture->resource)))
    {
        printf("Could not create a Direct3D 12 texture resource.\n");
        return 0;
    }

    // CopyTextureRegion requires the source buffer's row pitch to be a multiple of
    // D3D12_TEXTURE_DATA_PITCH_ALIGNMENT (256 bytes), which is usually wider than the tight
    // width*4 rows the pixel data arrives in, hence the row-by-row copy into a padded buffer.
    UINT row_pitch = (width * 4 + D3D12_TEXTURE_DATA_PITCH_ALIGNMENT - 1) & ~(D3D12_TEXTURE_DATA_PITCH_ALIGNMENT - 1);
    UINT64 upload_size = (UINT64)row_pitch * height;

    D3D12_HEAP_PROPERTIES upload_heap = { .Type = D3D12_HEAP_TYPE_UPLOAD };

    D3D12_RESOURCE_DESC upload_desc = {
        .Dimension = D3D12_RESOURCE_DIMENSION_BUFFER,
        .Width = upload_size,
        .Height = 1,
        .DepthOrArraySize = 1,
        .MipLevels = 1,
        .Format = DXGI_FORMAT_UNKNOWN,
        .SampleDesc.Count = 1,
        .Layout = D3D12_TEXTURE_LAYOUT_ROW_MAJOR
    };

    ID3D12Resource* upload_buffer = NULL;

    if (FAILED(ID3D12Device_CreateCommittedResource(device, &upload_heap, D3D12_HEAP_FLAG_NONE,
        &upload_desc, D3D12_RESOURCE_STATE_GENERIC_READ, NULL, &IID_ID3D12Resource, (void**)&upload_buffer)))
    {
        printf("Could not create a Direct3D 12 texture upload buffer.\n");
        ID3D12Resource_Release(texture->resource);
        return 0;
    }

    D3D12_RANGE no_read_range = { 0, 0 };
    uint8_t* mapped = NULL;
    ID3D12Resource_Map(upload_buffer, 0, &no_read_range, (void**)&mapped);

    for (UINT row = 0; row < height; row++)
    {
        SDL_memcpy(mapped + (size_t)row * row_pitch, pixels + (size_t)row * width * 4, width * 4);
    }

    ID3D12Resource_Unmap(upload_buffer, 0, NULL);

    ID3D12CommandAllocator_Reset(upload_allocator);
    ID3D12GraphicsCommandList_Reset(upload_command_list, upload_allocator, NULL);

    D3D12_TEXTURE_COPY_LOCATION destination = {
        .pResource = texture->resource,
        .Type = D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX,
        .SubresourceIndex = 0
    };

    D3D12_TEXTURE_COPY_LOCATION source = {
        .pResource = upload_buffer,
        .Type = D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT,
        .PlacedFootprint = {
            .Offset = 0,
            .Footprint = {
                .Format = DXGI_FORMAT_R8G8B8A8_UNORM,
                .Width = width,
                .Height = height,
                .Depth = 1,
                .RowPitch = row_pitch
            }
        }
    };

    ID3D12GraphicsCommandList_CopyTextureRegion(upload_command_list, &destination, 0, 0, 0, &source, NULL);

    D3D12_RESOURCE_BARRIER barrier = { 0 };
    barrier.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
    barrier.Transition.pResource = texture->resource;
    barrier.Transition.StateBefore = D3D12_RESOURCE_STATE_COPY_DEST;
    barrier.Transition.StateAfter = D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE;
    barrier.Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
    ID3D12GraphicsCommandList_ResourceBarrier(upload_command_list, 1, &barrier);

    ID3D12GraphicsCommandList_Close(upload_command_list);

    ID3D12CommandList* lists[] = { (ID3D12CommandList*)upload_command_list };
    ID3D12CommandQueue_ExecuteCommandLists(command_queue, 1, lists);

    dx12_wait_renderer_idle();

    ID3D12Resource_Release(upload_buffer);

    D3D12_SHADER_RESOURCE_VIEW_DESC srv_desc = {
        .Format = DXGI_FORMAT_R8G8B8A8_UNORM,
        .ViewDimension = D3D12_SRV_DIMENSION_TEXTURE2D,
        .Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING,
        .Texture2D.MipLevels = 1
    };

    texture->srv_index = allocate_srv_index();
    ID3D12Device_CreateShaderResourceView(device, texture->resource, &srv_desc, srv_cpu_handle(texture->srv_index));

    texture->width = width;
    texture->height = height;

    return 1;
}

static int create_white_texture(void)
{
    uint8_t pixel[4] = { 255, 255, 255, 255 };
    return upload_texture_pixels(&built_in_white_texture, pixel, 1, 1);
}

static int record_commands(void)
{
    if (FAILED(ID3D12CommandAllocator_Reset(command_allocators[frame_index])))
    {
        return 0;
    }

    if (FAILED(ID3D12GraphicsCommandList_Reset(command_list, command_allocators[frame_index], pipeline_state)))
    {
        return 0;
    }

    active_command_list = command_list;
    gpu_frame_offset = 0;

    ID3D12DescriptorHeap* heaps[] = { srv_heap };
    ID3D12GraphicsCommandList_SetDescriptorHeaps(command_list, 1, heaps);

    ID3D12GraphicsCommandList_SetGraphicsRootSignature(command_list, root_signature);
    ID3D12GraphicsCommandList_IASetPrimitiveTopology(command_list, D3D_PRIMITIVE_TOPOLOGY_TRIANGLELIST);

    ScreenTransform transform = screen_transform_for_size((float)swapchain_width, (float)swapchain_height);
    ID3D12GraphicsCommandList_SetGraphicsRoot32BitConstants(command_list, 0,
        sizeof(transform) / sizeof(float), &transform, 0);

    D3D12_VIEWPORT viewport = {
        .Width = (float)swapchain_width,
        .Height = (float)swapchain_height,
        .MinDepth = 0.0f,
        .MaxDepth = 1.0f
    };
    ID3D12GraphicsCommandList_RSSetViewports(command_list, 1, &viewport);

    D3D12_RECT scissor_rect = { 0, 0, (LONG)swapchain_width, (LONG)swapchain_height };
    ID3D12GraphicsCommandList_RSSetScissorRects(command_list, 1, &scissor_rect);

    D3D12_RESOURCE_BARRIER barrier = { 0 };
    barrier.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
    barrier.Transition.pResource = render_targets[frame_index];
    barrier.Transition.StateBefore = D3D12_RESOURCE_STATE_PRESENT;
    barrier.Transition.StateAfter = D3D12_RESOURCE_STATE_RENDER_TARGET;
    barrier.Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
    ID3D12GraphicsCommandList_ResourceBarrier(command_list, 1, &barrier);

    D3D12_CPU_DESCRIPTOR_HANDLE rtv_handle;
    ID3D12DescriptorHeap_GetCPUDescriptorHandleForHeapStart(rtv_heap, &rtv_handle);
    rtv_handle.ptr += (SIZE_T)frame_index * rtv_descriptor_size;

    ID3D12GraphicsCommandList_OMSetRenderTargets(command_list, 1, &rtv_handle, FALSE, NULL);

    static const float clear_color[4] = { 0.0f, 0.0f, 0.0f, 1.0f };
    ID3D12GraphicsCommandList_ClearRenderTargetView(command_list, rtv_handle, clear_color, 0, NULL);

    vertex_write_offset = 0;

    return 1;
}

// A raw (ByteAddressBuffer) view of `resource` in the bindless table.
static UINT create_raw_buffer_srv(ID3D12Resource* resource, UINT64 size)
{
    D3D12_SHADER_RESOURCE_VIEW_DESC srv_desc = {
        .Format = DXGI_FORMAT_R32_TYPELESS,
        .ViewDimension = D3D12_SRV_DIMENSION_BUFFER,
        .Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING,
        .Buffer = { .FirstElement = 0, .NumElements = (UINT)(size / 4), .Flags = D3D12_BUFFER_SRV_FLAG_RAW }
    };

    UINT index = allocate_srv_index();
    ID3D12Device_CreateShaderResourceView(device, resource, &srv_desc, srv_cpu_handle(index));

    return index;
}

static int create_gpu_frame_buffers(void)
{
    D3D12_HEAP_PROPERTIES upload_heap = { .Type = D3D12_HEAP_TYPE_UPLOAD };

    D3D12_RESOURCE_DESC desc = {
        .Dimension = D3D12_RESOURCE_DIMENSION_BUFFER,
        .Width = GPU_FRAME_BUFFER_SIZE,
        .Height = 1,
        .DepthOrArraySize = 1,
        .MipLevels = 1,
        .Format = DXGI_FORMAT_UNKNOWN,
        .SampleDesc.Count = 1,
        .Layout = D3D12_TEXTURE_LAYOUT_ROW_MAJOR
    };

    D3D12_RANGE no_read_range = { 0, 0 };

    for (UINT i = 0; i < FRAME_COUNT; i++)
    {
        if (FAILED(ID3D12Device_CreateCommittedResource(device, &upload_heap, D3D12_HEAP_FLAG_NONE,
            &desc, D3D12_RESOURCE_STATE_GENERIC_READ, NULL, &IID_ID3D12Resource, (void**)&gpu_frame_buffers[i])) ||
            FAILED(ID3D12Resource_Map(gpu_frame_buffers[i], 0, &no_read_range, &gpu_frame_mapped[i])))
        {
            printf("Could not create a Direct3D 12 frame buffer.\n");
            return 0;
        }

        gpu_frame_srv[i] = create_raw_buffer_srv(gpu_frame_buffers[i], GPU_FRAME_BUFFER_SIZE);
    }

    return 1;
}

// A buffer the GPU writes, in the default heap.
static ID3D12Resource* create_uav_buffer(UINT64 size, D3D12_RESOURCE_STATES state)
{
    D3D12_HEAP_PROPERTIES default_heap = { .Type = D3D12_HEAP_TYPE_DEFAULT };

    D3D12_RESOURCE_DESC desc = {
        .Dimension = D3D12_RESOURCE_DIMENSION_BUFFER,
        .Width = size,
        .Height = 1,
        .DepthOrArraySize = 1,
        .MipLevels = 1,
        .Format = DXGI_FORMAT_UNKNOWN,
        .SampleDesc.Count = 1,
        .Layout = D3D12_TEXTURE_LAYOUT_ROW_MAJOR,
        .Flags = D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS
    };

    ID3D12Resource* buffer = NULL;

    if (FAILED(ID3D12Device_CreateCommittedResource(device, &default_heap, D3D12_HEAP_FLAG_NONE, &desc, state, NULL, &IID_ID3D12Resource, (void**)&buffer)))
    {
        return NULL;
    }

    return buffer;
}

static void create_ray_tracing_resources(void)
{
    D3D12_FEATURE_DATA_D3D12_OPTIONS5 options = { 0 };

    if (FAILED(ID3D12Device_CheckFeatureSupport(device, D3D12_FEATURE_D3D12_OPTIONS5, &options, sizeof(options))) ||
        options.RaytracingTier < D3D12_RAYTRACING_TIER_1_1 ||
        FAILED(ID3D12Device_QueryInterface(device, &IID_ID3D12Device5, (void**)&rt_device)))
    {
        return;
    }

    D3D12_BUILD_RAYTRACING_ACCELERATION_STRUCTURE_INPUTS inputs = {
        .Type = D3D12_RAYTRACING_ACCELERATION_STRUCTURE_TYPE_TOP_LEVEL,
        .Flags = D3D12_RAYTRACING_ACCELERATION_STRUCTURE_BUILD_FLAG_PREFER_FAST_BUILD,
        .NumDescs = MAX_RAY_INSTANCES,
        .DescsLayout = D3D12_ELEMENTS_LAYOUT_ARRAY
    };

    D3D12_RAYTRACING_ACCELERATION_STRUCTURE_PREBUILD_INFO info = { 0 };
    ID3D12Device5_GetRaytracingAccelerationStructurePrebuildInfo(rt_device, &inputs, &info);

    tlas_scratch = create_uav_buffer(info.ScratchDataSizeInBytes, D3D12_RESOURCE_STATE_COMMON);

    D3D12_HEAP_PROPERTIES upload_heap = { .Type = D3D12_HEAP_TYPE_UPLOAD };

    D3D12_RESOURCE_DESC instances_desc = {
        .Dimension = D3D12_RESOURCE_DIMENSION_BUFFER,
        .Width = sizeof(D3D12_RAYTRACING_INSTANCE_DESC) * MAX_RAY_INSTANCES,
        .Height = 1,
        .DepthOrArraySize = 1,
        .MipLevels = 1,
        .Format = DXGI_FORMAT_UNKNOWN,
        .SampleDesc.Count = 1,
        .Layout = D3D12_TEXTURE_LAYOUT_ROW_MAJOR
    };

    D3D12_RANGE no_read_range = { 0, 0 };

    for (UINT i = 0; i < FRAME_COUNT; i++)
    {
        tlas_buffers[i] = create_uav_buffer(info.ResultDataMaxSizeInBytes, D3D12_RESOURCE_STATE_RAYTRACING_ACCELERATION_STRUCTURE);

        if (!tlas_buffers[i] || FAILED(ID3D12Device_CreateCommittedResource(device, &upload_heap, D3D12_HEAP_FLAG_NONE, &instances_desc,
            D3D12_RESOURCE_STATE_GENERIC_READ, NULL, &IID_ID3D12Resource, (void**)&tlas_instances[i])) ||
            FAILED(ID3D12Resource_Map(tlas_instances[i], 0, &no_read_range, &tlas_instances_mapped[i])))
        {
            printf("Could not create the Direct3D 12 ray tracing resources : ray tracing is unavailable.\n");
            ID3D12Device5_Release(rt_device);
            rt_device = NULL;
            return;
        }

        D3D12_SHADER_RESOURCE_VIEW_DESC srv_desc = {
            .Format = DXGI_FORMAT_UNKNOWN,
            .ViewDimension = D3D12_SRV_DIMENSION_RAYTRACING_ACCELERATION_STRUCTURE,
            .Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING,
            .RaytracingAccelerationStructure.Location = ID3D12Resource_GetGPUVirtualAddress(tlas_buffers[i])
        };

        // Consecutive in the heap : the root signature's table covers them both.
        UINT index = allocate_srv_index();

        if (i == 0)
        {
            tlas_srv_base = index;
        }
        else if (index != tlas_srv_base + i)
        {
            printf("The Direct3D 12 acceleration structures' descriptors aren't contiguous : ray tracing is unavailable.\n");
            ID3D12Device5_Release(rt_device);
            rt_device = NULL;
            return;
        }

        ID3D12Device_CreateShaderResourceView(device, NULL, &srv_desc, srv_cpu_handle(index));
    }
}

int dx12_create_renderer(Window* target_window)
{
#ifdef PIRENEUS_UWP
    core_window = (IUnknown*)target_window;
#else
    window = (SDL_Window*)target_window;

    if (!get_hwnd())
    {
        return 0;
    }
#endif

    if (!create_device())
    {
        return 0;
    }

    if (!create_command_queue())
    {
        return 0;
    }

    if (!create_swapchain())
    {
        return 0;
    }

    if (!create_render_targets())
    {
        return 0;
    }

    if (!create_depth_buffer())
    {
        return 0;
    }

    if (!create_srv_heap())
    {
        return 0;
    }

    if (!create_commands())
    {
        return 0;
    }

    if (!create_upload_commands())
    {
        return 0;
    }

    if (!create_render_target_commands())
    {
        return 0;
    }

    if (!create_root_signature())
    {
        return 0;
    }

    if (!create_pipeline_state())
    {
        return 0;
    }

    if (!create_mesh_pipeline_states())
    {
        return 0;
    }

    if (!create_vertex_buffers())
    {
        return 0;
    }

    if (!create_fence())
    {
        return 0;
    }

    if (!create_white_texture())
    {
        return 0;
    }

    if (!create_gpu_frame_buffers())
    {
        return 0;
    }

    create_ray_tracing_resources();

    DEBUG_LOG("[dx12] renderer ready (swapchain %ux%u)\n", swapchain_width, swapchain_height);

    return 1;
}

Texture* dx12_create_texture(const char* file_path, int* out_width, int* out_height)
{
    const char* base_path = SDL_GetBasePath();

    char path[512];
    SDL_snprintf(path, sizeof(path), "%s%s", base_path ? base_path : "", file_path);

    SDL_Surface* surface = IMG_Load(path);

    if (!surface)
    {
        printf("Could not load texture '%s': %s\n", path, SDL_GetError());
        return NULL;
    }

    SDL_Surface* converted = SDL_ConvertSurface(surface, SDL_PIXELFORMAT_RGBA32);
    SDL_DestroySurface(surface);

    if (!converted)
    {
        printf("Could not convert texture '%s' to RGBA32: %s\n", path, SDL_GetError());
        return NULL;
    }

    Texture* texture = SDL_calloc(1, sizeof(Texture));

    if (!upload_texture_pixels(texture, (const uint8_t*)converted->pixels, (UINT)converted->w, (UINT)converted->h))
    {
        SDL_free(texture);
        SDL_DestroySurface(converted);
        return NULL;
    }

    if (out_width)
    {
        *out_width = converted->w;
    }

    if (out_height)
    {
        *out_height = converted->h;
    }

    SDL_DestroySurface(converted);

    DEBUG_LOG("[dx12] texture loaded: \"%s\" (%dx%d)\n", file_path, out_width ? *out_width : -1, out_height ? *out_height : -1);

    return texture;
}

void dx12_destroy_texture(Texture* texture)
{
    if (!texture || texture == &built_in_white_texture)
    {
        return;
    }

    release_srv_index(texture->srv_index);

    if (texture->target_heap)
    {
        ID3D12DescriptorHeap_Release(texture->target_heap);
    }

    ID3D12Resource_Release(texture->resource);
    SDL_free(texture);

    DEBUG_LOG("[dx12] texture destroyed\n");
}

Texture* dx12_create_texture_from_pixels(const void* rgba_pixels, int width, int height)
{
    Texture* texture = SDL_calloc(1, sizeof(Texture));

    if (!upload_texture_pixels(texture, (const uint8_t*)rgba_pixels, (UINT)width, (UINT)height))
    {
        SDL_free(texture);
        return NULL;
    }

    DEBUG_LOG("[dx12] texture created from pixels (%dx%d)\n", width, height);

    return texture;
}

void dx12_texture_get_size(Texture* texture, int* out_width, int* out_height)
{
    *out_width = (int)texture->width;
    *out_height = (int)texture->height;
}

Texture* dx12_white_texture(void)
{
    return &built_in_white_texture;
}

RenderTarget* dx12_create_render_target(int width, int height)
{
    RenderTarget* target = SDL_calloc(1, sizeof(RenderTarget));
    target->width = width;
    target->height = height;

    D3D12_HEAP_PROPERTIES default_heap = { .Type = D3D12_HEAP_TYPE_DEFAULT };

    D3D12_RESOURCE_DESC desc = {
        .Dimension = D3D12_RESOURCE_DIMENSION_TEXTURE2D,
        .Width = (UINT64)width,
        .Height = (UINT)height,
        .DepthOrArraySize = 1,
        .MipLevels = 1,
        .Format = DXGI_FORMAT_R8G8B8A8_UNORM,
        .SampleDesc.Count = 1,
        .Layout = D3D12_TEXTURE_LAYOUT_UNKNOWN,
        .Flags = D3D12_RESOURCE_FLAG_ALLOW_RENDER_TARGET
    };

    D3D12_CLEAR_VALUE clear_value = {
        .Format = DXGI_FORMAT_R8G8B8A8_UNORM,
        .Color = { 0.0f, 0.0f, 0.0f, 1.0f }
    };

    // Created directly in PIXEL_SHADER_RESOURCE : begin_render_target_pass()/end_render_target_pass()
    // always barrier from/to that state, treating "readable like any other texture" as the target's
    // resting state between passes.
    if (FAILED(ID3D12Device_CreateCommittedResource(device, &default_heap, D3D12_HEAP_FLAG_NONE,
        &desc, D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE, &clear_value, &IID_ID3D12Resource, (void**)&target->texture.resource)))
    {
        printf("Could not create a Direct3D 12 render target resource.\n");
        SDL_free(target);
        return NULL;
    }

    D3D12_DESCRIPTOR_HEAP_DESC rtv_heap_desc = { .Type = D3D12_DESCRIPTOR_HEAP_TYPE_RTV, .NumDescriptors = 1 };

    if (FAILED(ID3D12Device_CreateDescriptorHeap(device, &rtv_heap_desc, &IID_ID3D12DescriptorHeap, (void**)&target->rtv_heap)))
    {
        printf("Could not create a Direct3D 12 render target RTV heap.\n");
        ID3D12Resource_Release(target->texture.resource);
        SDL_free(target);
        return NULL;
    }

    ID3D12DescriptorHeap_GetCPUDescriptorHandleForHeapStart(target->rtv_heap, &target->rtv_handle);
    ID3D12Device_CreateRenderTargetView(device, target->texture.resource, NULL, target->rtv_handle);

    D3D12_SHADER_RESOURCE_VIEW_DESC srv_desc = {
        .Format = DXGI_FORMAT_R8G8B8A8_UNORM,
        .ViewDimension = D3D12_SRV_DIMENSION_TEXTURE2D,
        .Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING,
        .Texture2D.MipLevels = 1
    };

    target->texture.srv_index = allocate_srv_index();
    ID3D12Device_CreateShaderResourceView(device, target->texture.resource, &srv_desc, srv_cpu_handle(target->texture.srv_index));

    DEBUG_LOG("[dx12] render target created: %dx%d\n", width, height);

    return target;
}

void dx12_destroy_render_target(RenderTarget* target)
{
    if (!target)
    {
        return;
    }

    release_srv_index(target->texture.srv_index);
    ID3D12DescriptorHeap_Release(target->rtv_heap);
    ID3D12Resource_Release(target->texture.resource);
    SDL_free(target);

    DEBUG_LOG("[dx12] render target destroyed\n");
}

Texture* dx12_render_target_texture(RenderTarget* target)
{
    return &target->texture;
}

int dx12_begin_render_target_pass(RenderTarget* target)
{
    if (!target)
    {
        return 0;
    }

    if (FAILED(ID3D12CommandAllocator_Reset(render_target_allocator)))
    {
        return 0;
    }

    if (FAILED(ID3D12GraphicsCommandList_Reset(render_target_command_list, render_target_allocator, pipeline_state)))
    {
        return 0;
    }

    active_command_list = render_target_command_list;
    rendering_to_target = 1;
    render_target_vertex_write_offset = 0;
    active_render_target = target;

    ID3D12DescriptorHeap* heaps[] = { srv_heap };
    ID3D12GraphicsCommandList_SetDescriptorHeaps(active_command_list, 1, heaps);
    ID3D12GraphicsCommandList_SetGraphicsRootSignature(active_command_list, root_signature);
    ID3D12GraphicsCommandList_IASetPrimitiveTopology(active_command_list, D3D_PRIMITIVE_TOPOLOGY_TRIANGLELIST);

    ScreenTransform transform = screen_transform_for_size((float)target->width, (float)target->height);
    ID3D12GraphicsCommandList_SetGraphicsRoot32BitConstants(active_command_list, 0,
        sizeof(transform) / sizeof(float), &transform, 0);

    D3D12_VIEWPORT viewport = {
        .Width = (float)target->width,
        .Height = (float)target->height,
        .MinDepth = 0.0f,
        .MaxDepth = 1.0f
    };
    ID3D12GraphicsCommandList_RSSetViewports(active_command_list, 1, &viewport);

    D3D12_RECT scissor_rect = { 0, 0, (LONG)target->width, (LONG)target->height };
    ID3D12GraphicsCommandList_RSSetScissorRects(active_command_list, 1, &scissor_rect);

    D3D12_RESOURCE_BARRIER barrier = { 0 };
    barrier.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
    barrier.Transition.pResource = target->texture.resource;
    barrier.Transition.StateBefore = D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE;
    barrier.Transition.StateAfter = D3D12_RESOURCE_STATE_RENDER_TARGET;
    barrier.Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
    ID3D12GraphicsCommandList_ResourceBarrier(active_command_list, 1, &barrier);

    ID3D12GraphicsCommandList_OMSetRenderTargets(active_command_list, 1, &target->rtv_handle, FALSE, NULL);

    static const float clear_color[4] = { 0.0f, 0.0f, 0.0f, 1.0f };
    ID3D12GraphicsCommandList_ClearRenderTargetView(active_command_list, target->rtv_handle, clear_color, 0, NULL);

    return 1;
}

void dx12_end_render_target_pass(void)
{
    D3D12_RESOURCE_BARRIER barrier = { 0 };
    barrier.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
    barrier.Transition.pResource = active_render_target->texture.resource;
    barrier.Transition.StateBefore = D3D12_RESOURCE_STATE_RENDER_TARGET;
    barrier.Transition.StateAfter = D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE;
    barrier.Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
    ID3D12GraphicsCommandList_ResourceBarrier(active_command_list, 1, &barrier);

    ID3D12GraphicsCommandList_Close(active_command_list);

    ID3D12CommandList* lists[] = { (ID3D12CommandList*)active_command_list };
    ID3D12CommandQueue_ExecuteCommandLists(command_queue, 1, lists);

    dx12_wait_renderer_idle();

    rendering_to_target = 0;
    active_render_target = NULL;
}

int dx12_begin_frame(void)
{
    if (!resize_if_needed())
    {
        return 0;
    }

    if (!record_commands())
    {
        printf("Could not record the Direct3D 12 command list.\n");
        return 0;
    }

    return 1;
}

void dx12_draw_vertices(const Vertex* vertices, int vertex_count, Texture* texture)
{
    if (vertex_count <= 0 || !texture)
    {
        return;
    }

    ID3D12Resource* vertex_buffer = rendering_to_target ? render_target_vertex_buffer : vertex_buffers[frame_index];
    void* mapped = rendering_to_target ? render_target_vertex_buffer_mapped : vertex_buffer_mapped[frame_index];
    UINT* write_offset = rendering_to_target ? &render_target_vertex_write_offset : &vertex_write_offset;

    if (*write_offset + (UINT)vertex_count > VERTEX_BUFFER_SIZE / sizeof(Vertex))
    {
        printf("Direct3D 12 vertex buffer is full this frame, dropping a draw call.\n");
        return;
    }

    Vertex* destination = (Vertex*)mapped + *write_offset;
    SDL_memcpy(destination, vertices, sizeof(Vertex) * (size_t)vertex_count);

    D3D12_VERTEX_BUFFER_VIEW buffer_view = {
        .BufferLocation = ID3D12Resource_GetGPUVirtualAddress(vertex_buffer) + (UINT64)(*write_offset) * sizeof(Vertex),
        .SizeInBytes = (UINT)(sizeof(Vertex) * (size_t)vertex_count),
        .StrideInBytes = sizeof(Vertex)
    };
    ID3D12GraphicsCommandList_IASetVertexBuffers(active_command_list, 0, 1, &buffer_view);

    ID3D12GraphicsCommandList_SetGraphicsRootDescriptorTable(active_command_list, 1, srv_gpu_handle(texture->srv_index));

    ID3D12GraphicsCommandList_DrawInstanced(active_command_list, (UINT)vertex_count, 1, 0, 0);

    *write_offset += (UINT)vertex_count;
}

void dx12_set_scissor(int x, int y, int width, int height)
{
    D3D12_RECT scissor_rect = { (LONG)x, (LONG)y, (LONG)(x + width), (LONG)(y + height) };
    ID3D12GraphicsCommandList_RSSetScissorRects(active_command_list, 1, &scissor_rect);
}

void dx12_end_frame(void)
{
    D3D12_RESOURCE_BARRIER barrier = { 0 };
    barrier.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
    barrier.Transition.pResource = render_targets[frame_index];
    barrier.Transition.StateBefore = D3D12_RESOURCE_STATE_RENDER_TARGET;
    barrier.Transition.StateAfter = D3D12_RESOURCE_STATE_PRESENT;
    barrier.Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
    ID3D12GraphicsCommandList_ResourceBarrier(command_list, 1, &barrier);

    if (FAILED(ID3D12GraphicsCommandList_Close(command_list)))
    {
        return;
    }

    ID3D12CommandList* command_lists[] = { (ID3D12CommandList*)command_list };
    ID3D12CommandQueue_ExecuteCommandLists(command_queue, 1, command_lists);

    if (FAILED(IDXGISwapChain3_Present(swapchain, 1, 0)))
    {
        printf("Could not present the Direct3D 12 swapchain.\n");
        return;
    }

    if (!move_to_next_frame())
    {
        printf("Could not synchronize the Direct3D 12 frame.\n");
    }
}

void dx12_wait_renderer_idle(void)
{
    if (!command_queue || !fence)
    {
        return;
    }

    // Always a fresh fence value (see signal_fence()), never one already reached : re-signaling an old
    // value would let a later wait return while the GPU still runs that frame, and a command allocator
    // still in use would be reset (device removed).
    wait_for_fence_value(signal_fence());
}

static void print_debug_messages(void);

void dx12_destroy_renderer(void)
{
    print_debug_messages();

    dx12_wait_renderer_idle();

    ID3D12Resource_Release(built_in_white_texture.resource);

    if (fence_event)
    {
        CloseHandle(fence_event);
    }

    if (fence)
    {
        ID3D12Fence_Release(fence);
    }

    for (UINT i = 0; i < FRAME_COUNT; i++)
    {
        if (vertex_buffers[i])
        {
            ID3D12Resource_Unmap(vertex_buffers[i], 0, NULL);
            ID3D12Resource_Release(vertex_buffers[i]);
        }
    }

    if (render_target_vertex_buffer)
    {
        ID3D12Resource_Unmap(render_target_vertex_buffer, 0, NULL);
        ID3D12Resource_Release(render_target_vertex_buffer);
    }

    if (render_target_command_list)
    {
        ID3D12GraphicsCommandList_Release(render_target_command_list);
    }

    if (render_target_allocator)
    {
        ID3D12CommandAllocator_Release(render_target_allocator);
    }

    if (pipeline_state)
    {
        ID3D12PipelineState_Release(pipeline_state);
    }

    if (mesh_pipeline_state)
    {
        ID3D12PipelineState_Release(mesh_pipeline_state);
    }

    if (shadow_pipeline_state)
    {
        ID3D12PipelineState_Release(shadow_pipeline_state);
    }

    if (sky_pipeline_state)
    {
        ID3D12PipelineState_Release(sky_pipeline_state);
    }

    if (mesh_root_signature)
    {
        ID3D12RootSignature_Release(mesh_root_signature);
    }

    if (gpu_root_signature)
    {
        ID3D12RootSignature_Release(gpu_root_signature);
    }

    for (UINT i = 0; i < FRAME_COUNT; i++)
    {
        if (tlas_buffers[i])
        {
            ID3D12Resource_Release(tlas_buffers[i]);
        }

        if (tlas_instances[i])
        {
            ID3D12Resource_Unmap(tlas_instances[i], 0, NULL);
            ID3D12Resource_Release(tlas_instances[i]);
        }
    }

    if (tlas_scratch)
    {
        ID3D12Resource_Release(tlas_scratch);
    }

    if (rt_device)
    {
        ID3D12Device5_Release(rt_device);
    }

    for (UINT i = 0; i < FRAME_COUNT; i++)
    {
        if (gpu_frame_buffers[i])
        {
            ID3D12Resource_Unmap(gpu_frame_buffers[i], 0, NULL);
            ID3D12Resource_Release(gpu_frame_buffers[i]);
        }
    }

    release_depth_buffer();

    if (dsv_heap)
    {
        ID3D12DescriptorHeap_Release(dsv_heap);
    }

    if (root_signature)
    {
        ID3D12RootSignature_Release(root_signature);
    }

    if (upload_command_list)
    {
        ID3D12GraphicsCommandList_Release(upload_command_list);
    }

    if (upload_allocator)
    {
        ID3D12CommandAllocator_Release(upload_allocator);
    }

    if (command_list)
    {
        ID3D12GraphicsCommandList_Release(command_list);
    }

    for (UINT i = 0; i < FRAME_COUNT; i++)
    {
        if (command_allocators[i])
        {
            ID3D12CommandAllocator_Release(command_allocators[i]);
        }
    }

    if (srv_heap)
    {
        ID3D12DescriptorHeap_Release(srv_heap);
    }

    release_render_targets();

    if (rtv_heap)
    {
        ID3D12DescriptorHeap_Release(rtv_heap);
    }

    if (swapchain)
    {
        IDXGISwapChain3_Release(swapchain);
    }

    if (command_queue)
    {
        ID3D12CommandQueue_Release(command_queue);
    }

    if (device)
    {
        ID3D12Device_Release(device);
    }
}

// --- 3D ---------------------------------------------------------------------------------------

static ID3D12Resource* create_filled_upload_buffer(const void* data, UINT64 size);

// Static data in the default heap, copied through an upload buffer, left readable by every stage.
static ID3D12Resource* create_device_buffer(const void* data, UINT64 size)
{
    ID3D12Resource* upload = create_filled_upload_buffer(data, size);

    if (!upload)
    {
        return NULL;
    }

    D3D12_HEAP_PROPERTIES default_heap = { .Type = D3D12_HEAP_TYPE_DEFAULT };

    D3D12_RESOURCE_DESC desc = {
        .Dimension = D3D12_RESOURCE_DIMENSION_BUFFER,
        .Width = size,
        .Height = 1,
        .DepthOrArraySize = 1,
        .MipLevels = 1,
        .Format = DXGI_FORMAT_UNKNOWN,
        .SampleDesc.Count = 1,
        .Layout = D3D12_TEXTURE_LAYOUT_ROW_MAJOR
    };

    ID3D12Resource* buffer = NULL;

    // Created in COMMON : buffers implicitly promote to COPY_DEST for the copy below.
    if (FAILED(ID3D12Device_CreateCommittedResource(device, &default_heap, D3D12_HEAP_FLAG_NONE,
        &desc, D3D12_RESOURCE_STATE_COMMON, NULL, &IID_ID3D12Resource, (void**)&buffer)))
    {
        ID3D12Resource_Release(upload);
        return NULL;
    }

    ID3D12CommandAllocator_Reset(upload_allocator);
    ID3D12GraphicsCommandList_Reset(upload_command_list, upload_allocator, NULL);
    ID3D12GraphicsCommandList_CopyBufferRegion(upload_command_list, buffer, 0, upload, 0, size);

    D3D12_RESOURCE_BARRIER barrier = { 0 };
    barrier.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
    barrier.Transition.pResource = buffer;
    barrier.Transition.StateBefore = D3D12_RESOURCE_STATE_COPY_DEST;
    barrier.Transition.StateAfter = D3D12_RESOURCE_STATE_GENERIC_READ;
    barrier.Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
    ID3D12GraphicsCommandList_ResourceBarrier(upload_command_list, 1, &barrier);

    ID3D12GraphicsCommandList_Close(upload_command_list);

    ID3D12CommandList* lists[] = { (ID3D12CommandList*)upload_command_list };
    ID3D12CommandQueue_ExecuteCommandLists(command_queue, 1, lists);

    dx12_wait_renderer_idle();

    ID3D12Resource_Release(upload);

    return buffer;
}

static ID3D12Resource* create_filled_upload_buffer(const void* data, UINT64 size)
{
    D3D12_HEAP_PROPERTIES upload_heap = { .Type = D3D12_HEAP_TYPE_UPLOAD };

    D3D12_RESOURCE_DESC desc = {
        .Dimension = D3D12_RESOURCE_DIMENSION_BUFFER,
        .Width = size,
        .Height = 1,
        .DepthOrArraySize = 1,
        .MipLevels = 1,
        .Format = DXGI_FORMAT_UNKNOWN,
        .SampleDesc.Count = 1,
        .Layout = D3D12_TEXTURE_LAYOUT_ROW_MAJOR
    };

    ID3D12Resource* buffer = NULL;

    if (FAILED(ID3D12Device_CreateCommittedResource(device, &upload_heap, D3D12_HEAP_FLAG_NONE,
        &desc, D3D12_RESOURCE_STATE_GENERIC_READ, NULL, &IID_ID3D12Resource, (void**)&buffer)))
    {
        return NULL;
    }

    D3D12_RANGE no_read_range = { 0, 0 };
    void* mapped = NULL;

    if (FAILED(ID3D12Resource_Map(buffer, 0, &no_read_range, &mapped)))
    {
        ID3D12Resource_Release(buffer);
        return NULL;
    }

    SDL_memcpy(mapped, data, (size_t)size);
    ID3D12Resource_Unmap(buffer, 0, NULL);

    return buffer;
}

Mesh* dx12_create_mesh(const MeshVertex* vertices, int vertex_count, const uint32_t* indices, int index_count)
{
    Mesh* mesh = SDL_calloc(1, sizeof(Mesh));
    UINT vertices_size = (UINT)(sizeof(MeshVertex) * (size_t)vertex_count);
    UINT indices_size = (UINT)(sizeof(uint32_t) * (size_t)index_count);

    mesh->vertex_buffer = create_device_buffer(vertices, vertices_size);
    mesh->index_buffer = create_device_buffer(indices, indices_size);

    if (!mesh->vertex_buffer || !mesh->index_buffer)
    {
        printf("Could not create the Direct3D 12 mesh buffers.\n");

        if (mesh->vertex_buffer)
        {
            ID3D12Resource_Release(mesh->vertex_buffer);
        }

        if (mesh->index_buffer)
        {
            ID3D12Resource_Release(mesh->index_buffer);
        }

        SDL_free(mesh);
        return NULL;
    }

    mesh->vertex_view.BufferLocation = ID3D12Resource_GetGPUVirtualAddress(mesh->vertex_buffer);
    mesh->vertex_view.SizeInBytes = vertices_size;
    mesh->vertex_view.StrideInBytes = sizeof(MeshVertex);

    mesh->index_view.BufferLocation = ID3D12Resource_GetGPUVirtualAddress(mesh->index_buffer);
    mesh->index_view.SizeInBytes = indices_size;
    mesh->index_view.Format = DXGI_FORMAT_R32_UINT;

    mesh->index_count = (UINT)index_count;
    mesh->vertex_count = (UINT)vertex_count;
    mesh->vertices_srv = create_raw_buffer_srv(mesh->vertex_buffer, vertices_size);
    mesh->indices_srv = create_raw_buffer_srv(mesh->index_buffer, indices_size);

    DEBUG_LOG("[dx12] mesh created: %d vertices, %d indices\n", vertex_count, index_count);

    return mesh;
}

void dx12_destroy_mesh(Mesh* mesh)
{
    for (int i = 0; i < mesh->blas_count; i++)
    {
        ID3D12Resource_Release(mesh->blas[i].buffer);
    }

    SDL_free(mesh->blas);

    release_srv_index(mesh->vertices_srv);
    release_srv_index(mesh->indices_srv);
    ID3D12Resource_Release(mesh->vertex_buffer);
    ID3D12Resource_Release(mesh->index_buffer);
    SDL_free(mesh);

    DEBUG_LOG("[dx12] mesh destroyed\n");
}

ShadowMap* dx12_create_shadow_map(int size)
{
    ShadowMap* shadow_map = SDL_calloc(1, sizeof(ShadowMap));
    shadow_map->size = size;

    D3D12_HEAP_PROPERTIES default_heap = { .Type = D3D12_HEAP_TYPE_DEFAULT };

    D3D12_RESOURCE_DESC desc = {
        .Dimension = D3D12_RESOURCE_DIMENSION_TEXTURE2D,
        .Width = (UINT64)size,
        .Height = (UINT)size,
        .DepthOrArraySize = 1,
        .MipLevels = 1,
        .Format = DXGI_FORMAT_R32_TYPELESS,
        .SampleDesc.Count = 1,
        .Layout = D3D12_TEXTURE_LAYOUT_UNKNOWN,
        .Flags = D3D12_RESOURCE_FLAG_ALLOW_DEPTH_STENCIL
    };

    D3D12_CLEAR_VALUE clear_value = { .Format = DXGI_FORMAT_D32_FLOAT, .DepthStencil = { 1.0f, 0 } };

    // Resting state : readable like any texture (as render targets do), DEPTH_WRITE only during
    // begin_shadow_pass()/end_shadow_pass().
    if (FAILED(ID3D12Device_CreateCommittedResource(device, &default_heap, D3D12_HEAP_FLAG_NONE,
        &desc, D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE, &clear_value, &IID_ID3D12Resource, (void**)&shadow_map->texture.resource)))
    {
        printf("Could not create a Direct3D 12 shadow map resource.\n");
        SDL_free(shadow_map);
        return NULL;
    }

    D3D12_DESCRIPTOR_HEAP_DESC heap_desc = { .Type = D3D12_DESCRIPTOR_HEAP_TYPE_DSV, .NumDescriptors = 1 };

    if (FAILED(ID3D12Device_CreateDescriptorHeap(device, &heap_desc, &IID_ID3D12DescriptorHeap, (void**)&shadow_map->dsv_heap)))
    {
        printf("Could not create a Direct3D 12 shadow map DSV heap.\n");
        ID3D12Resource_Release(shadow_map->texture.resource);
        SDL_free(shadow_map);
        return NULL;
    }

    ID3D12DescriptorHeap_GetCPUDescriptorHandleForHeapStart(shadow_map->dsv_heap, &shadow_map->dsv_handle);

    D3D12_DEPTH_STENCIL_VIEW_DESC dsv_desc = {
        .Format = DXGI_FORMAT_D32_FLOAT,
        .ViewDimension = D3D12_DSV_DIMENSION_TEXTURE2D
    };
    ID3D12Device_CreateDepthStencilView(device, shadow_map->texture.resource, &dsv_desc, shadow_map->dsv_handle);

    D3D12_SHADER_RESOURCE_VIEW_DESC srv_desc = {
        .Format = DXGI_FORMAT_R32_FLOAT,
        .ViewDimension = D3D12_SRV_DIMENSION_TEXTURE2D,
        .Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING,
        .Texture2D.MipLevels = 1
    };

    shadow_map->texture.srv_index = allocate_srv_index();
    ID3D12Device_CreateShaderResourceView(device, shadow_map->texture.resource, &srv_desc, srv_cpu_handle(shadow_map->texture.srv_index));

    DEBUG_LOG("[dx12] shadow map created: %dx%d\n", size, size);

    return shadow_map;
}

void dx12_destroy_shadow_map(ShadowMap* shadow_map)
{
    release_srv_index(shadow_map->texture.srv_index);
    ID3D12DescriptorHeap_Release(shadow_map->dsv_heap);
    ID3D12Resource_Release(shadow_map->texture.resource);
    SDL_free(shadow_map);

    DEBUG_LOG("[dx12] shadow map destroyed\n");
}

static void transition_shadow_map(ShadowMap* shadow_map, D3D12_RESOURCE_STATES before, D3D12_RESOURCE_STATES after)
{
    D3D12_RESOURCE_BARRIER barrier = { 0 };
    barrier.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
    barrier.Transition.pResource = shadow_map->texture.resource;
    barrier.Transition.StateBefore = before;
    barrier.Transition.StateAfter = after;
    barrier.Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
    ID3D12GraphicsCommandList_ResourceBarrier(active_command_list, 1, &barrier);
}

// Recorded into the render-target command list : like a render-target pass, a synchronous unit of
// work of its own (end_shadow_pass() submits and waits on it).
int dx12_begin_shadow_pass(ShadowMap* shadow_map, const float light_view_projection[16])
{
    if (FAILED(ID3D12CommandAllocator_Reset(render_target_allocator)))
    {
        return 0;
    }

    if (FAILED(ID3D12GraphicsCommandList_Reset(render_target_command_list, render_target_allocator, shadow_pipeline_state)))
    {
        return 0;
    }

    active_command_list = render_target_command_list;
    active_shadow_map = shadow_map;

    ID3D12GraphicsCommandList_SetGraphicsRootSignature(active_command_list, mesh_root_signature);
    ID3D12GraphicsCommandList_IASetPrimitiveTopology(active_command_list, D3D_PRIMITIVE_TOPOLOGY_TRIANGLELIST);

    D3D12_VIEWPORT viewport = {
        .Width = (float)shadow_map->size,
        .Height = (float)shadow_map->size,
        .MinDepth = 0.0f,
        .MaxDepth = 1.0f
    };
    ID3D12GraphicsCommandList_RSSetViewports(active_command_list, 1, &viewport);

    D3D12_RECT scissor_rect = { 0, 0, (LONG)shadow_map->size, (LONG)shadow_map->size };
    ID3D12GraphicsCommandList_RSSetScissorRects(active_command_list, 1, &scissor_rect);

    transition_shadow_map(shadow_map, D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE, D3D12_RESOURCE_STATE_DEPTH_WRITE);

    ID3D12GraphicsCommandList_OMSetRenderTargets(active_command_list, 0, NULL, FALSE, &shadow_map->dsv_handle);
    ID3D12GraphicsCommandList_ClearDepthStencilView(active_command_list, shadow_map->dsv_handle, D3D12_CLEAR_FLAG_DEPTH, 1.0f, 0, 0, NULL);

    mesh_constants_for_shadow_pass(&mesh_constants, light_view_projection);

    return 1;
}

void dx12_end_shadow_pass(void)
{
    transition_shadow_map(active_shadow_map, D3D12_RESOURCE_STATE_DEPTH_WRITE, D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE);

    ID3D12GraphicsCommandList_Close(active_command_list);

    ID3D12CommandList* lists[] = { (ID3D12CommandList*)active_command_list };
    ID3D12CommandQueue_ExecuteCommandLists(command_queue, 1, lists);

    dx12_wait_renderer_idle();

    active_shadow_map = NULL;
}

void dx12_begin_scene(const SceneLighting* lighting, ShadowMap* shadow_map)
{
    D3D12_CPU_DESCRIPTOR_HANDLE rtv_handle = current_rtv_handle();

    ID3D12GraphicsCommandList_SetPipelineState(command_list, mesh_pipeline_state);
    ID3D12GraphicsCommandList_SetGraphicsRootSignature(command_list, mesh_root_signature);

    ID3D12GraphicsCommandList_OMSetRenderTargets(command_list, 1, &rtv_handle, FALSE, &dsv_handle);
    ID3D12GraphicsCommandList_ClearDepthStencilView(command_list, dsv_handle, D3D12_CLEAR_FLAG_DEPTH, 1.0f, 0, 0, NULL);

    // Without a shadow map, the white texture stands in : a stored depth of 1 never occludes.
    Texture* shadow_texture = shadow_map ? &shadow_map->texture : &built_in_white_texture;
    ID3D12GraphicsCommandList_SetGraphicsRootDescriptorTable(command_list, 1, srv_gpu_handle(shadow_texture->srv_index));
    scene_pass_texture = shadow_texture;

    mesh_constants_for_scene(&mesh_constants, lighting, shadow_map ? shadow_map->size : 0);
}

void dx12_end_scene(void)
{
    // Back to the 2D state record_commands() set up : sprite pipeline + root signature (whose
    // root constants switching signatures invalidated), and no depth buffer bound, since the
    // sprite pipeline state declares none.
    D3D12_CPU_DESCRIPTOR_HANDLE rtv_handle = current_rtv_handle();

    ID3D12GraphicsCommandList_SetPipelineState(command_list, pipeline_state);
    ID3D12GraphicsCommandList_SetGraphicsRootSignature(command_list, root_signature);

    ScreenTransform transform = screen_transform_for_size((float)swapchain_width, (float)swapchain_height);
    ID3D12GraphicsCommandList_SetGraphicsRoot32BitConstants(command_list, 0, sizeof(transform) / sizeof(float), &transform, 0);

    ID3D12GraphicsCommandList_OMSetRenderTargets(command_list, 1, &rtv_handle, FALSE, NULL);
}

void dx12_draw_mesh(Mesh* mesh, const float model[16], int first_index, int index_count)
{
    SDL_memcpy(mesh_constants.model, model, sizeof(mesh_constants.model));
    ID3D12GraphicsCommandList_SetGraphicsRoot32BitConstants(active_command_list, 0,
        sizeof(mesh_constants) / sizeof(float), &mesh_constants, 0);

    ID3D12GraphicsCommandList_IASetVertexBuffers(active_command_list, 0, 1, &mesh->vertex_view);
    ID3D12GraphicsCommandList_IASetIndexBuffer(active_command_list, &mesh->index_view);
    UINT count = index_count < 0 ? mesh->index_count : (UINT)index_count;
    ID3D12GraphicsCommandList_DrawIndexedInstanced(active_command_list, count, 1, (UINT)first_index, 0, 0);
}

void dx12_draw_skybox(Texture* equirectangular, float intensity)
{
    MeshConstants sky_constants;
    mesh_constants_for_sky(&sky_constants, mesh_constants.view_projection, intensity);

    ID3D12GraphicsCommandList_SetPipelineState(command_list, sky_pipeline_state);
    ID3D12GraphicsCommandList_SetGraphicsRoot32BitConstants(command_list, 0, sizeof(sky_constants) / sizeof(float), &sky_constants, 0);
    ID3D12GraphicsCommandList_SetGraphicsRootDescriptorTable(command_list, 1, srv_gpu_handle(equirectangular->srv_index));
    ID3D12GraphicsCommandList_DrawInstanced(command_list, 3, 1, 0, 0);

    // Back to the scene's state, so draw_mesh() keeps working after the sky (each draw_mesh()
    // re-pushes the whole constant block, so only the pipeline and texture need restoring).
    ID3D12GraphicsCommandList_SetPipelineState(command_list, mesh_pipeline_state);
    ID3D12GraphicsCommandList_SetGraphicsRootDescriptorTable(command_list, 1, srv_gpu_handle(scene_pass_texture->srv_index));
}

// --- GPU layer (gpu.h) -------------------------------------------------------------------------

// Depth textures are typeless so they can be both a depth target and sampled.
static DXGI_FORMAT to_dxgi_format(GpuFormat format)
{
    switch (format)
    {
    case GPU_FORMAT_RGBA8: return DXGI_FORMAT_R8G8B8A8_UNORM;
    case GPU_FORMAT_RGBA8_SRGB: return DXGI_FORMAT_R8G8B8A8_UNORM_SRGB;
    case GPU_FORMAT_RGBA16F: return DXGI_FORMAT_R16G16B16A16_FLOAT;
    case GPU_FORMAT_RGBA32F: return DXGI_FORMAT_R32G32B32A32_FLOAT;
    case GPU_FORMAT_RG16F: return DXGI_FORMAT_R16G16_FLOAT;
    case GPU_FORMAT_R16F: return DXGI_FORMAT_R16_FLOAT;
    case GPU_FORMAT_R8: return DXGI_FORMAT_R8_UNORM;
    case GPU_FORMAT_D32: return DXGI_FORMAT_D32_FLOAT;
    default: return DXGI_FORMAT_UNKNOWN;
    }
}

static UINT format_pixel_size(GpuFormat format)
{
    switch (format)
    {
    case GPU_FORMAT_RGBA8:
    case GPU_FORMAT_RGBA8_SRGB:
    case GPU_FORMAT_RG16F:
    case GPU_FORMAT_D32: return 4;
    case GPU_FORMAT_RGBA16F: return 8;
    case GPU_FORMAT_RGBA32F: return 16;
    case GPU_FORMAT_R16F: return 2;
    case GPU_FORMAT_R8: return 1;
    default: return 0;
    }
}

int dx12_gpu_supported(void)
{
    // Unbounded descriptor tables need resource binding tier 2.
    D3D12_FEATURE_DATA_D3D12_OPTIONS options = { 0 };

    return SUCCEEDED(ID3D12Device_CheckFeatureSupport(device, D3D12_FEATURE_D3D12_OPTIONS, &options, sizeof(options))) &&
        options.ResourceBindingTier >= D3D12_RESOURCE_BINDING_TIER_2;
}

Texture* dx12_gpu_create_texture(const GpuTextureDesc* desc)
{
    Texture* texture = SDL_calloc(1, sizeof(Texture));
    texture->width = (UINT)desc->width;
    texture->height = (UINT)desc->height;
    texture->is_depth = desc->format == GPU_FORMAT_D32;

    DXGI_FORMAT format = to_dxgi_format(desc->format);
    D3D12_RESOURCE_FLAGS flags = D3D12_RESOURCE_FLAG_NONE;

    if (desc->usage & GPU_USAGE_RENDER_TARGET)
    {
        flags |= D3D12_RESOURCE_FLAG_ALLOW_RENDER_TARGET;
    }

    if (desc->usage & GPU_USAGE_DEPTH_TARGET)
    {
        flags |= D3D12_RESOURCE_FLAG_ALLOW_DEPTH_STENCIL;
    }

    D3D12_HEAP_PROPERTIES default_heap = { .Type = D3D12_HEAP_TYPE_DEFAULT };

    D3D12_RESOURCE_DESC resource_desc = {
        .Dimension = D3D12_RESOURCE_DIMENSION_TEXTURE2D,
        .Width = (UINT64)desc->width,
        .Height = (UINT)desc->height,
        .DepthOrArraySize = 1,
        .MipLevels = (UINT16)desc->mip_count,
        .Format = texture->is_depth ? DXGI_FORMAT_R32_TYPELESS : format,
        .SampleDesc.Count = 1,
        .Layout = D3D12_TEXTURE_LAYOUT_UNKNOWN,
        .Flags = flags
    };

    D3D12_CLEAR_VALUE depth_clear = { .Format = DXGI_FORMAT_D32_FLOAT, .DepthStencil = { 1.0f, 0 } };
    int has_pixels = desc->mips[0] != NULL;

    if (FAILED(ID3D12Device_CreateCommittedResource(device, &default_heap, D3D12_HEAP_FLAG_NONE, &resource_desc,
        has_pixels ? D3D12_RESOURCE_STATE_COPY_DEST : SHADER_READ_STATE, texture->is_depth ? &depth_clear : NULL,
        &IID_ID3D12Resource, (void**)&texture->resource)))
    {
        printf("Could not create a Direct3D 12 GPU texture (%dx%d).\n", desc->width, desc->height);
        SDL_free(texture);
        return NULL;
    }

    if (has_pixels)
    {
        D3D12_PLACED_SUBRESOURCE_FOOTPRINT layouts[GPU_MAX_MIPS];
        UINT row_counts[GPU_MAX_MIPS];
        UINT64 row_sizes[GPU_MAX_MIPS];
        UINT64 total_size = 0;

        ID3D12Device_GetCopyableFootprints(device, &resource_desc, 0, (UINT)desc->mip_count, 0, layouts, row_counts, row_sizes, &total_size);

        D3D12_HEAP_PROPERTIES upload_heap = { .Type = D3D12_HEAP_TYPE_UPLOAD };

        D3D12_RESOURCE_DESC upload_desc = {
            .Dimension = D3D12_RESOURCE_DIMENSION_BUFFER,
            .Width = total_size,
            .Height = 1,
            .DepthOrArraySize = 1,
            .MipLevels = 1,
            .Format = DXGI_FORMAT_UNKNOWN,
            .SampleDesc.Count = 1,
            .Layout = D3D12_TEXTURE_LAYOUT_ROW_MAJOR
        };

        ID3D12Resource* upload_buffer = NULL;

        if (FAILED(ID3D12Device_CreateCommittedResource(device, &upload_heap, D3D12_HEAP_FLAG_NONE, &upload_desc,
            D3D12_RESOURCE_STATE_GENERIC_READ, NULL, &IID_ID3D12Resource, (void**)&upload_buffer)))
        {
            printf("Could not create a Direct3D 12 GPU texture upload buffer.\n");
            ID3D12Resource_Release(texture->resource);
            SDL_free(texture);
            return NULL;
        }

        D3D12_RANGE no_read_range = { 0, 0 };
        uint8_t* mapped = NULL;
        ID3D12Resource_Map(upload_buffer, 0, &no_read_range, (void**)&mapped);

        UINT pixel_size = format_pixel_size(desc->format);

        for (int mip = 0; mip < desc->mip_count; mip++)
        {
            UINT mip_width = SDL_max(1u, texture->width >> mip);
            UINT mip_height = SDL_max(1u, texture->height >> mip);
            const uint8_t* source = (const uint8_t*)desc->mips[mip];

            for (UINT row = 0; row < mip_height; row++)
            {
                SDL_memcpy(mapped + layouts[mip].Offset + (size_t)row * layouts[mip].Footprint.RowPitch,
                    source + (size_t)row * mip_width * pixel_size, (size_t)mip_width * pixel_size);
            }
        }

        ID3D12Resource_Unmap(upload_buffer, 0, NULL);

        ID3D12CommandAllocator_Reset(upload_allocator);
        ID3D12GraphicsCommandList_Reset(upload_command_list, upload_allocator, NULL);

        for (int mip = 0; mip < desc->mip_count; mip++)
        {
            D3D12_TEXTURE_COPY_LOCATION destination = {
                .pResource = texture->resource,
                .Type = D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX,
                .SubresourceIndex = (UINT)mip
            };

            D3D12_TEXTURE_COPY_LOCATION source = {
                .pResource = upload_buffer,
                .Type = D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT,
                .PlacedFootprint = layouts[mip]
            };

            ID3D12GraphicsCommandList_CopyTextureRegion(upload_command_list, &destination, 0, 0, 0, &source, NULL);
        }

        D3D12_RESOURCE_BARRIER barrier = { 0 };
        barrier.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
        barrier.Transition.pResource = texture->resource;
        barrier.Transition.StateBefore = D3D12_RESOURCE_STATE_COPY_DEST;
        barrier.Transition.StateAfter = SHADER_READ_STATE;
        barrier.Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
        ID3D12GraphicsCommandList_ResourceBarrier(upload_command_list, 1, &barrier);

        ID3D12GraphicsCommandList_Close(upload_command_list);

        ID3D12CommandList* lists[] = { (ID3D12CommandList*)upload_command_list };
        ID3D12CommandQueue_ExecuteCommandLists(command_queue, 1, lists);

        dx12_wait_renderer_idle();

        ID3D12Resource_Release(upload_buffer);
    }

    D3D12_SHADER_RESOURCE_VIEW_DESC srv_desc = {
        .Format = texture->is_depth ? DXGI_FORMAT_R32_FLOAT : format,
        .ViewDimension = D3D12_SRV_DIMENSION_TEXTURE2D,
        .Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING,
        .Texture2D.MipLevels = (UINT)desc->mip_count
    };

    texture->srv_index = allocate_srv_index();
    ID3D12Device_CreateShaderResourceView(device, texture->resource, &srv_desc, srv_cpu_handle(texture->srv_index));

    if (flags != D3D12_RESOURCE_FLAG_NONE)
    {
        D3D12_DESCRIPTOR_HEAP_DESC heap_desc = {
            .Type = texture->is_depth ? D3D12_DESCRIPTOR_HEAP_TYPE_DSV : D3D12_DESCRIPTOR_HEAP_TYPE_RTV,
            .NumDescriptors = 1
        };

        if (FAILED(ID3D12Device_CreateDescriptorHeap(device, &heap_desc, &IID_ID3D12DescriptorHeap, (void**)&texture->target_heap)))
        {
            printf("Could not create a Direct3D 12 GPU texture's target heap.\n");
            dx12_destroy_texture(texture);
            return NULL;
        }

        ID3D12DescriptorHeap_GetCPUDescriptorHandleForHeapStart(texture->target_heap, &texture->target_handle);

        if (texture->is_depth)
        {
            D3D12_DEPTH_STENCIL_VIEW_DESC dsv_desc = { .Format = DXGI_FORMAT_D32_FLOAT, .ViewDimension = D3D12_DSV_DIMENSION_TEXTURE2D };
            ID3D12Device_CreateDepthStencilView(device, texture->resource, &dsv_desc, texture->target_handle);
        }
        else
        {
            ID3D12Device_CreateRenderTargetView(device, texture->resource, NULL, texture->target_handle);
        }
    }

    return texture;
}

uint32_t dx12_gpu_texture_index(Texture* texture)
{
    return texture->srv_index;
}

GpuBuffer* dx12_gpu_create_buffer(const void* data, size_t size)
{
    GpuBuffer* buffer = SDL_calloc(1, sizeof(GpuBuffer));
    buffer->resource = create_device_buffer(data, size);

    if (!buffer->resource)
    {
        printf("Could not create a Direct3D 12 GPU buffer.\n");
        SDL_free(buffer);
        return NULL;
    }

    buffer->srv_index = create_raw_buffer_srv(buffer->resource, size);

    return buffer;
}

void dx12_gpu_destroy_buffer(GpuBuffer* buffer)
{
    release_srv_index(buffer->srv_index);
    ID3D12Resource_Release(buffer->resource);
    SDL_free(buffer);
}

uint32_t dx12_gpu_buffer_index(GpuBuffer* buffer)
{
    return buffer->srv_index;
}

uint32_t dx12_gpu_mesh_vertices_index(Mesh* mesh)
{
    return mesh->vertices_srv;
}

uint32_t dx12_gpu_mesh_indices_index(Mesh* mesh)
{
    return mesh->indices_srv;
}

GpuFrameAllocation dx12_gpu_frame_alloc(size_t size)
{
    GpuFrameAllocation allocation = { NULL, 0, 0 };
    UINT aligned = (gpu_frame_offset + 255u) & ~255u;

    if (aligned + size > GPU_FRAME_BUFFER_SIZE)
    {
        return allocation;
    }

    allocation.mapped = (uint8_t*)gpu_frame_mapped[frame_index] + aligned;
    allocation.buffer_index = gpu_frame_srv[frame_index];
    allocation.offset = aligned;
    gpu_frame_offset = aligned + (UINT)size;

    return allocation;
}

// What the debug layer has to say, when it's installed (Debug builds).
static void print_debug_messages(void)
{
#ifndef NDEBUG
    ID3D12InfoQueue* queue = NULL;

    if (FAILED(ID3D12Device_QueryInterface(device, &IID_ID3D12InfoQueue, (void**)&queue)))
    {
        return;
    }

    UINT64 count = ID3D12InfoQueue_GetNumStoredMessages(queue);

    for (UINT64 i = 0; i < count; i++)
    {
        SIZE_T length = 0;
        ID3D12InfoQueue_GetMessage(queue, i, NULL, &length);

        D3D12_MESSAGE* message = SDL_malloc(length);

        if (message && SUCCEEDED(ID3D12InfoQueue_GetMessage(queue, i, message, &length)) && message->Severity <= D3D12_MESSAGE_SEVERITY_ERROR)
        {
            printf("[d3d12] %s\n", message->pDescription);
        }

        SDL_free(message);
    }

    ID3D12InfoQueue_ClearStoredMessages(queue);
    ID3D12InfoQueue_Release(queue);
#endif
}

GpuPipeline* dx12_gpu_create_pipeline(const GpuPipelineDesc* desc)
{
    size_t vs_size = 0;
    size_t ps_size = 0;
    uint8_t* vs_code = load_shader(desc->vertex_shader, &vs_size);
    uint8_t* ps_code = desc->fragment_shader ? load_shader(desc->fragment_shader, &ps_size) : NULL;

    if (!vs_code || (desc->fragment_shader && !ps_code))
    {
        printf("Could not load the '%s' / '%s' shaders.\n", desc->vertex_shader, desc->fragment_shader ? desc->fragment_shader : "");
        SDL_free(vs_code);
        SDL_free(ps_code);
        return NULL;
    }

    D3D12_GRAPHICS_PIPELINE_STATE_DESC state = { 0 };
    state.pRootSignature = gpu_root_signature;
    state.VS.pShaderBytecode = vs_code;
    state.VS.BytecodeLength = vs_size;
    state.PS.pShaderBytecode = ps_code;
    state.PS.BytecodeLength = ps_size;
    state.RasterizerState.FillMode = D3D12_FILL_MODE_SOLID;
    state.RasterizerState.CullMode = desc->cull_back ? D3D12_CULL_MODE_BACK : D3D12_CULL_MODE_NONE;
    state.RasterizerState.FrontCounterClockwise = TRUE;
    state.RasterizerState.DepthClipEnable = TRUE;
    state.RasterizerState.DepthBias = (INT)desc->depth_bias_constant;
    state.RasterizerState.SlopeScaledDepthBias = desc->depth_bias_slope;
    state.DepthStencilState.DepthEnable = desc->depth_test || desc->depth_write;
    state.DepthStencilState.DepthWriteMask = desc->depth_write ? D3D12_DEPTH_WRITE_MASK_ALL : D3D12_DEPTH_WRITE_MASK_ZERO;
    state.DepthStencilState.DepthFunc = desc->depth_test ? D3D12_COMPARISON_FUNC_LESS_EQUAL : D3D12_COMPARISON_FUNC_ALWAYS;
    state.DSVFormat = to_dxgi_format(desc->depth_format);
    state.SampleMask = UINT_MAX;
    state.PrimitiveTopologyType = D3D12_PRIMITIVE_TOPOLOGY_TYPE_TRIANGLE;
    state.NumRenderTargets = (UINT)desc->color_count;
    state.SampleDesc.Count = 1;

    for (int i = 0; i < desc->color_count; i++)
    {
        D3D12_RENDER_TARGET_BLEND_DESC* blend = &state.BlendState.RenderTarget[i];
        state.RTVFormats[i] = to_dxgi_format(desc->color_formats[i]);
        blend->RenderTargetWriteMask = D3D12_COLOR_WRITE_ENABLE_ALL;
        blend->BlendEnable = desc->blend != GPU_BLEND_NONE;
        blend->SrcBlend = desc->blend == GPU_BLEND_ADD ? D3D12_BLEND_ONE : D3D12_BLEND_SRC_ALPHA;
        blend->DestBlend = desc->blend == GPU_BLEND_ADD ? D3D12_BLEND_ONE : D3D12_BLEND_INV_SRC_ALPHA;
        blend->BlendOp = D3D12_BLEND_OP_ADD;
        blend->SrcBlendAlpha = D3D12_BLEND_ONE;
        blend->DestBlendAlpha = D3D12_BLEND_ONE;
        blend->BlendOpAlpha = D3D12_BLEND_OP_ADD;
    }

    state.BlendState.IndependentBlendEnable = FALSE;

    GpuPipeline* pipeline = SDL_calloc(1, sizeof(GpuPipeline));
    HRESULT hr = ID3D12Device_CreateGraphicsPipelineState(device, &state, &IID_ID3D12PipelineState, (void**)&pipeline->state);

    SDL_free(vs_code);
    SDL_free(ps_code);

    if (FAILED(hr))
    {
        print_debug_messages();
        printf("Could not create the Direct3D 12 '%s' / '%s' pipeline (0x%08lx).\n", desc->vertex_shader, desc->fragment_shader ? desc->fragment_shader : "", (unsigned long)hr);
        SDL_free(pipeline);
        return NULL;
    }

    return pipeline;
}

void dx12_gpu_destroy_pipeline(GpuPipeline* pipeline)
{
    ID3D12PipelineState_Release(pipeline->state);
    SDL_free(pipeline);
}

static void transition_texture(Texture* texture, D3D12_RESOURCE_STATES before, D3D12_RESOURCE_STATES after)
{
    D3D12_RESOURCE_BARRIER barrier = { 0 };
    barrier.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
    barrier.Transition.pResource = texture->resource;
    barrier.Transition.StateBefore = before;
    barrier.Transition.StateAfter = after;
    barrier.Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
    ID3D12GraphicsCommandList_ResourceBarrier(command_list, 1, &barrier);
}

void dx12_gpu_begin_pass(const GpuPassDesc* pass)
{
    D3D12_CPU_DESCRIPTOR_HANDLE color_handles[GPU_MAX_COLOR_TARGETS];
    UINT width = 0;
    UINT height = 0;

    gpu_pass_color_count = pass->color_count;
    gpu_pass_depth = pass->depth;

    for (int i = 0; i < pass->color_count; i++)
    {
        Texture* texture = pass->colors[i].texture;

        gpu_pass_colors[i] = texture;
        color_handles[i] = texture->target_handle;
        width = texture->width;
        height = texture->height;
        transition_texture(texture, SHADER_READ_STATE, D3D12_RESOURCE_STATE_RENDER_TARGET);
    }

    if (pass->depth)
    {
        width = pass->depth->width;
        height = pass->depth->height;
        transition_texture(pass->depth, SHADER_READ_STATE, D3D12_RESOURCE_STATE_DEPTH_WRITE);
    }

    ID3D12GraphicsCommandList_OMSetRenderTargets(command_list, (UINT)pass->color_count, pass->color_count ? color_handles : NULL, FALSE,
        pass->depth ? &pass->depth->target_handle : NULL);

    for (int i = 0; i < pass->color_count; i++)
    {
        if (pass->colors[i].load == GPU_LOAD_CLEAR)
        {
            ID3D12GraphicsCommandList_ClearRenderTargetView(command_list, color_handles[i], pass->colors[i].clear, 0, NULL);
        }
    }

    if (pass->depth && pass->depth_load == GPU_LOAD_CLEAR)
    {
        ID3D12GraphicsCommandList_ClearDepthStencilView(command_list, pass->depth->target_handle, D3D12_CLEAR_FLAG_DEPTH, pass->depth_clear, 0, 0, NULL);
    }

    D3D12_VIEWPORT viewport = { 0.0f, 0.0f, (float)width, (float)height, 0.0f, 1.0f };
    ID3D12GraphicsCommandList_RSSetViewports(command_list, 1, &viewport);

    D3D12_RECT scissor_rect = { 0, 0, (LONG)width, (LONG)height };
    ID3D12GraphicsCommandList_RSSetScissorRects(command_list, 1, &scissor_rect);

    ID3D12GraphicsCommandList_SetGraphicsRootSignature(command_list, gpu_root_signature);
    ID3D12GraphicsCommandList_SetGraphicsRootDescriptorTable(command_list, 1, srv_gpu_handle(0));
    ID3D12GraphicsCommandList_SetGraphicsRootDescriptorTable(command_list, 2, srv_gpu_handle(0));

    if (rt_device)
    {
        ID3D12GraphicsCommandList_SetGraphicsRootDescriptorTable(command_list, 3, srv_gpu_handle(tlas_srv_base));
    }

    ID3D12GraphicsCommandList_IASetPrimitiveTopology(command_list, D3D_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
}

void dx12_gpu_end_pass(void)
{
    for (int i = 0; i < gpu_pass_color_count; i++)
    {
        transition_texture(gpu_pass_colors[i], D3D12_RESOURCE_STATE_RENDER_TARGET, SHADER_READ_STATE);
    }

    if (gpu_pass_depth)
    {
        transition_texture(gpu_pass_depth, D3D12_RESOURCE_STATE_DEPTH_WRITE, SHADER_READ_STATE);
    }

    gpu_pass_color_count = 0;
    gpu_pass_depth = NULL;

    // Back to the window's 2D state, as record_commands() set it up.
    D3D12_CPU_DESCRIPTOR_HANDLE rtv_handle = current_rtv_handle();

    ID3D12GraphicsCommandList_SetPipelineState(command_list, pipeline_state);
    ID3D12GraphicsCommandList_SetGraphicsRootSignature(command_list, root_signature);

    ScreenTransform transform = screen_transform_for_size((float)swapchain_width, (float)swapchain_height);
    ID3D12GraphicsCommandList_SetGraphicsRoot32BitConstants(command_list, 0, sizeof(transform) / sizeof(float), &transform, 0);

    ID3D12GraphicsCommandList_OMSetRenderTargets(command_list, 1, &rtv_handle, FALSE, NULL);

    D3D12_VIEWPORT viewport = { 0.0f, 0.0f, (float)swapchain_width, (float)swapchain_height, 0.0f, 1.0f };
    ID3D12GraphicsCommandList_RSSetViewports(command_list, 1, &viewport);

    D3D12_RECT scissor_rect = { 0, 0, (LONG)swapchain_width, (LONG)swapchain_height };
    ID3D12GraphicsCommandList_RSSetScissorRects(command_list, 1, &scissor_rect);
}

void dx12_gpu_bind_pipeline(GpuPipeline* pipeline)
{
    ID3D12GraphicsCommandList_SetPipelineState(command_list, pipeline->state);
}

void dx12_gpu_push_constants(const void* data, int size)
{
    ID3D12GraphicsCommandList_SetGraphicsRoot32BitConstants(command_list, 0, (UINT)size / 4, data, 0);
}

void dx12_gpu_draw(int vertex_count)
{
    ID3D12GraphicsCommandList_DrawInstanced(command_list, (UINT)vertex_count, 1, 0, 0);
}

void dx12_gpu_draw_indexed(Mesh* mesh, int first_index, int index_count)
{
    ID3D12GraphicsCommandList_IASetIndexBuffer(command_list, &mesh->index_view);
    ID3D12GraphicsCommandList_DrawIndexedInstanced(command_list, (UINT)index_count, 1, (UINT)first_index, 0, 0);
}

int dx12_gpu_raytracing_supported(void)
{
    return rt_device != NULL;
}

static void uav_barrier(ID3D12GraphicsCommandList* list, ID3D12Resource* resource)
{
    D3D12_RESOURCE_BARRIER barrier = { 0 };
    barrier.Type = D3D12_RESOURCE_BARRIER_TYPE_UAV;
    barrier.UAV.pResource = resource;
    ID3D12GraphicsCommandList_ResourceBarrier(list, 1, &barrier);
}

// The structure for this range of the mesh's triangles, built the first time it's needed.
static const Blas* ensure_blas(Mesh* mesh, UINT first_index, UINT index_count)
{
    for (int i = 0; i < mesh->blas_count; i++)
    {
        if (mesh->blas[i].first_index == first_index && mesh->blas[i].index_count == index_count)
        {
            return &mesh->blas[i];
        }
    }

    D3D12_RAYTRACING_GEOMETRY_DESC geometry = {
        .Type = D3D12_RAYTRACING_GEOMETRY_TYPE_TRIANGLES,
        .Flags = D3D12_RAYTRACING_GEOMETRY_FLAG_OPAQUE,
        .Triangles = {
            .IndexFormat = DXGI_FORMAT_R32_UINT,
            .VertexFormat = DXGI_FORMAT_R32G32B32_FLOAT,
            .IndexCount = index_count,
            .VertexCount = mesh->vertex_count,
            .IndexBuffer = ID3D12Resource_GetGPUVirtualAddress(mesh->index_buffer) + (UINT64)first_index * sizeof(uint32_t),
            .VertexBuffer = { ID3D12Resource_GetGPUVirtualAddress(mesh->vertex_buffer), sizeof(MeshVertex) }
        }
    };

    D3D12_BUILD_RAYTRACING_ACCELERATION_STRUCTURE_INPUTS inputs = {
        .Type = D3D12_RAYTRACING_ACCELERATION_STRUCTURE_TYPE_BOTTOM_LEVEL,
        .Flags = D3D12_RAYTRACING_ACCELERATION_STRUCTURE_BUILD_FLAG_PREFER_FAST_TRACE,
        .NumDescs = 1,
        .DescsLayout = D3D12_ELEMENTS_LAYOUT_ARRAY,
        .pGeometryDescs = &geometry
    };

    D3D12_RAYTRACING_ACCELERATION_STRUCTURE_PREBUILD_INFO info = { 0 };
    ID3D12Device5_GetRaytracingAccelerationStructurePrebuildInfo(rt_device, &inputs, &info);

    ID3D12Resource* result = create_uav_buffer(info.ResultDataMaxSizeInBytes, D3D12_RESOURCE_STATE_RAYTRACING_ACCELERATION_STRUCTURE);
    ID3D12Resource* scratch = create_uav_buffer(info.ScratchDataSizeInBytes, D3D12_RESOURCE_STATE_COMMON);

    if (!result || !scratch)
    {
        printf("Could not allocate a Direct3D 12 acceleration structure.\n");
        return NULL;
    }

    ID3D12GraphicsCommandList4* list = NULL;
    ID3D12GraphicsCommandList_QueryInterface(upload_command_list, &IID_ID3D12GraphicsCommandList4, (void**)&list);

    ID3D12CommandAllocator_Reset(upload_allocator);
    ID3D12GraphicsCommandList_Reset(upload_command_list, upload_allocator, NULL);

    D3D12_BUILD_RAYTRACING_ACCELERATION_STRUCTURE_DESC build = {
        .DestAccelerationStructureData = ID3D12Resource_GetGPUVirtualAddress(result),
        .Inputs = inputs,
        .ScratchAccelerationStructureData = ID3D12Resource_GetGPUVirtualAddress(scratch)
    };

    ID3D12GraphicsCommandList4_BuildRaytracingAccelerationStructure(list, &build, 0, NULL);
    uav_barrier(upload_command_list, result);

    ID3D12GraphicsCommandList_Close(upload_command_list);

    ID3D12CommandList* lists[] = { (ID3D12CommandList*)upload_command_list };
    ID3D12CommandQueue_ExecuteCommandLists(command_queue, 1, lists);

    dx12_wait_renderer_idle();

    ID3D12GraphicsCommandList4_Release(list);
    ID3D12Resource_Release(scratch);

    mesh->blas = SDL_realloc(mesh->blas, sizeof(Blas) * (size_t)(mesh->blas_count + 1));
    Blas* blas = &mesh->blas[mesh->blas_count++];
    blas->first_index = first_index;
    blas->index_count = index_count;
    blas->buffer = result;

    return blas;
}

uint32_t dx12_gpu_build_ray_scene(const GpuRayInstance* instances, int count)
{
    if (!rt_device)
    {
        return 0;
    }

    count = SDL_min(count, MAX_RAY_INSTANCES);

    D3D12_RAYTRACING_INSTANCE_DESC* out = tlas_instances_mapped[frame_index];

    for (int i = 0; i < count; i++)
    {
        const Blas* blas = ensure_blas(instances[i].mesh, (UINT)instances[i].first_index, (UINT)instances[i].index_count);

        SDL_memcpy(out[i].Transform, instances[i].transform, sizeof(out[i].Transform));
        out[i].InstanceID = (UINT)i;
        out[i].InstanceMask = 0xFF;
        out[i].InstanceContributionToHitGroupIndex = 0;
        out[i].Flags = D3D12_RAYTRACING_INSTANCE_FLAG_TRIANGLE_CULL_DISABLE | (instances[i].alpha_tested ? D3D12_RAYTRACING_INSTANCE_FLAG_FORCE_NON_OPAQUE : 0);
        out[i].AccelerationStructure = blas ? ID3D12Resource_GetGPUVirtualAddress(blas->buffer) : 0;
    }

    D3D12_BUILD_RAYTRACING_ACCELERATION_STRUCTURE_DESC build = {
        .DestAccelerationStructureData = ID3D12Resource_GetGPUVirtualAddress(tlas_buffers[frame_index]),
        .Inputs = {
            .Type = D3D12_RAYTRACING_ACCELERATION_STRUCTURE_TYPE_TOP_LEVEL,
            .Flags = D3D12_RAYTRACING_ACCELERATION_STRUCTURE_BUILD_FLAG_PREFER_FAST_BUILD,
            .NumDescs = (UINT)count,
            .DescsLayout = D3D12_ELEMENTS_LAYOUT_ARRAY,
            .InstanceDescs = ID3D12Resource_GetGPUVirtualAddress(tlas_instances[frame_index])
        },
        .ScratchAccelerationStructureData = ID3D12Resource_GetGPUVirtualAddress(tlas_scratch)
    };

    ID3D12GraphicsCommandList4* list = NULL;
    ID3D12GraphicsCommandList_QueryInterface(command_list, &IID_ID3D12GraphicsCommandList4, (void**)&list);

    uav_barrier(command_list, tlas_scratch); // shared between frames
    ID3D12GraphicsCommandList4_BuildRaytracingAccelerationStructure(list, &build, 0, NULL);
    uav_barrier(command_list, tlas_buffers[frame_index]);

    ID3D12GraphicsCommandList4_Release(list);

    return frame_index;
}

static const RendererBackend dx12_backend = {
    .create_renderer = dx12_create_renderer,
    .destroy_renderer = dx12_destroy_renderer,
    .wait_renderer_idle = dx12_wait_renderer_idle,
    .begin_frame = dx12_begin_frame,
    .end_frame = dx12_end_frame,
    .create_texture = dx12_create_texture,
    .destroy_texture = dx12_destroy_texture,
    .white_texture = dx12_white_texture,
    .texture_get_size = dx12_texture_get_size,
    .create_texture_from_pixels = dx12_create_texture_from_pixels,
    .create_render_target = dx12_create_render_target,
    .destroy_render_target = dx12_destroy_render_target,
    .render_target_texture = dx12_render_target_texture,
    .begin_render_target_pass = dx12_begin_render_target_pass,
    .end_render_target_pass = dx12_end_render_target_pass,
    .draw_vertices = dx12_draw_vertices,
    .set_scissor = dx12_set_scissor,
    .create_mesh = dx12_create_mesh,
    .destroy_mesh = dx12_destroy_mesh,
    .create_shadow_map = dx12_create_shadow_map,
    .destroy_shadow_map = dx12_destroy_shadow_map,
    .begin_shadow_pass = dx12_begin_shadow_pass,
    .end_shadow_pass = dx12_end_shadow_pass,
    .begin_scene = dx12_begin_scene,
    .end_scene = dx12_end_scene,
    .draw_mesh = dx12_draw_mesh,
    .draw_skybox = dx12_draw_skybox,
    .gpu_supported = dx12_gpu_supported,
    .gpu_create_texture = dx12_gpu_create_texture,
    .gpu_texture_index = dx12_gpu_texture_index,
    .gpu_create_buffer = dx12_gpu_create_buffer,
    .gpu_destroy_buffer = dx12_gpu_destroy_buffer,
    .gpu_buffer_index = dx12_gpu_buffer_index,
    .gpu_mesh_vertices_index = dx12_gpu_mesh_vertices_index,
    .gpu_mesh_indices_index = dx12_gpu_mesh_indices_index,
    .gpu_frame_alloc = dx12_gpu_frame_alloc,
    .gpu_create_pipeline = dx12_gpu_create_pipeline,
    .gpu_destroy_pipeline = dx12_gpu_destroy_pipeline,
    .gpu_begin_pass = dx12_gpu_begin_pass,
    .gpu_end_pass = dx12_gpu_end_pass,
    .gpu_bind_pipeline = dx12_gpu_bind_pipeline,
    .gpu_push_constants = dx12_gpu_push_constants,
    .gpu_draw = dx12_gpu_draw,
    .gpu_draw_indexed = dx12_gpu_draw_indexed,
    .gpu_raytracing_supported = dx12_gpu_raytracing_supported,
    .gpu_build_ray_scene = dx12_gpu_build_ray_scene
};

const RendererBackend* dx12_get_backend(void)
{
    return &dx12_backend;
}
