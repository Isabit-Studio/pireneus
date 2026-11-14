// volk first : it defines VK_NO_PROTOTYPES and includes <vulkan/vulkan.h> itself, and vk_mem_alloc.h
// needs to see it to declare vmaImportVulkanFunctionsFromVolk().
#include <volk.h>
#include <vk_mem_alloc.h>

#include <SDL3/SDL.h>
#include <SDL3/SDL_vulkan.h>
#include <SDL3_image/SDL_image.h>

#include <stdio.h>
#include <stddef.h>

#include "renderer/renderer.h"
#include "renderer/ortho.h"
#include "renderer/mesh_constants.h"
#include "renderer/vulkan/vulkan_renderer.h"
#include "debug.h"

#define MAX_FRAMES_IN_FLIGHT 2
static int vertex_buffer_full_reported; // the overflow message was printed this frame

// Statistics (renderer_get_stats) : the GPU's name, the GPU time of each frame's command buffer from
// two timestamps (read back when its slot comes round again, the fence having been waited on), and
// video memory from VMA's budgets (exact with VK_EXT_memory_budget, VMA's own allocations otherwise).
static char gpu_name[128];
static VkQueryPool timestamp_pool;   // two queries per frame in flight ; VK_NULL_HANDLE if unsupported
static float timestamp_period_ns;
static uint64_t timestamp_mask;
static int timestamp_pending[MAX_FRAMES_IN_FLIGHT];
static float last_gpu_frame_ms = -1.0f;
static int memory_budget_enabled;

#define VERTEX_BUFFER_SIZE (8 * 1024 * 1024) // per frame in flight : 262144 vertices
#define MAX_TEXTURES 4096
#define BINDLESS_CAPACITY 8192
#define GPU_FRAME_BUFFER_SIZE (8 * 1024 * 1024)
#define NO_BINDLESS_INDEX UINT32_MAX

struct Texture
{
    VkImage image;
    VmaAllocation allocation;
    VkImageView view;
    VkDescriptorSet descriptor_set;
    uint32_t bindless_index; // its slot in the global table (gpu.h), NO_BINDLESS_INDEX if not registered
    uint32_t width, height;
    VkFormat format;
    int is_depth;
};

struct RenderTarget
{
    Texture texture; // same shape as a regular texture, so it can be sampled the same way
    VkFramebuffer framebuffer;
    int width, height;
};

// A host-visible, persistently mapped buffer (vertex buffers, texture staging buffers).
typedef struct
{
    VkBuffer buffer;
    VmaAllocation allocation;
    void* mapped;
} MappedBuffer;

// Static data in device-local memory, readable by shaders through the bindless table.
typedef struct
{
    VkBuffer buffer;
    VmaAllocation allocation;
    uint32_t bindless_index;
} DeviceBuffer;

struct GpuBuffer
{
    DeviceBuffer device;
};

struct GpuPipeline
{
    VkPipeline pipeline;
};

// A bottom-level acceleration structure : one range of a mesh's triangles.
typedef struct
{
    uint32_t first_index;
    uint32_t index_count;
    VkAccelerationStructureKHR handle;
    VkBuffer buffer;
    VmaAllocation allocation;
    VkDeviceAddress address;
} Blas;

struct Mesh
{
    // Device-local : the photoreal renderer's shading pass reads vertices for every pixel.
    DeviceBuffer vertices;
    DeviceBuffer indices;
    uint32_t index_count;
    uint32_t vertex_count;
    Blas* blas;
    int blas_count;
};

struct ShadowMap
{
    Texture texture; // depth image, sampled through a regular texture descriptor set
    VkFramebuffer framebuffer;
    int size;
};

typedef struct
{
    int32_t graphics_family;
    int32_t present_family;
} QueueFamilyIndices;

static SDL_Window* window;

static VkInstance instance;
static VkSurfaceKHR surface;
static VkPhysicalDevice physical_device = VK_NULL_HANDLE;
static VkDevice device;
static QueueFamilyIndices queue_family_indices;
static VkQueue graphics_queue;
static VkQueue present_queue;
static VmaAllocator allocator;

static VkSwapchainKHR swapchain;
static VkFormat swapchain_image_format;
static VkExtent2D swapchain_extent;
static uint32_t swapchain_image_count;
static VkImage* swapchain_images;
static VkImageView* swapchain_image_views;
static VkFramebuffer* swapchain_framebuffers;

static VkRenderPass render_pass;
// Same attachment/subpass shape as render_pass, but ends in SHADER_READ_ONLY_OPTIMAL instead of
// PRESENT_SRC_KHR : used for render-target passes, whose output is sampled as a texture afterwards
// rather than presented.
static VkRenderPass render_target_render_pass;
static VkDescriptorSetLayout descriptor_set_layout;
static VkDescriptorPool descriptor_pool;
static VkSampler sampler;
static VkPipelineLayout pipeline_layout;
static VkPipeline pipeline;
// The same sprite pipeline, for render-target passes : a Vulkan pipeline is only compatible with
// render passes whose attachments have the same formats, and a render target (RGBA8, color only)
// differs from the window's pass (swapchain format + depth).
static VkPipeline render_target_pipeline;

// --- 3D (see begin_scene / begin_shadow_pass) ---
// D32_SFLOAT : usable both as a depth attachment and as a sampled image on every desktop GPU
// (checked at startup), for the window's depth buffer and for shadow maps alike.
#define DEPTH_FORMAT VK_FORMAT_D32_SFLOAT

static VkImage depth_image; // the window pass's depth buffer, recreated with the swapchain
static VmaAllocation depth_allocation;
static VkImageView depth_view;

static VkRenderPass shadow_render_pass; // depth only, ends in SHADER_READ_ONLY_OPTIMAL
static VkPipelineLayout mesh_pipeline_layout;
static VkPipeline mesh_pipeline;   // forward pass, in render_pass
static VkPipeline shadow_pipeline; // depth only, in shadow_render_pass
static VkPipeline sky_pipeline;    // full-screen triangle at the far plane, in render_pass
static Texture* scene_pass_texture; // what begin_scene() bound (shadow map), restored after draw_skybox()
static int mesh_supported;         // 0 if the GPU can't run the 3D pipelines (push constant budget, depth format)
static MeshConstants mesh_constants; // the current pass's constants ; draw_mesh() only changes `model`

static VkCommandPool command_pool;
static VkCommandBuffer command_buffers[MAX_FRAMES_IN_FLIGHT];
static VkCommandBuffer current_command_buffer;

static MappedBuffer vertex_buffers[MAX_FRAMES_IN_FLIGHT];
static uint32_t vertex_write_offset; // in Vertex units, within the current frame's buffer

// Render-target passes are fully synchronous (submitted and waited on before returning, see
// end_render_target_pass), so a single, non-double-buffered vertex buffer is enough : by the time
// it's reused next frame, the GPU has long finished reading it.
static MappedBuffer render_target_vertex_buffer;
static uint32_t render_target_vertex_write_offset;
static int rendering_to_target;

static VkSemaphore image_available_semaphores[MAX_FRAMES_IN_FLIGHT];
static VkSemaphore render_finished_semaphores[MAX_FRAMES_IN_FLIGHT];
static VkFence in_flight_fences[MAX_FRAMES_IN_FLIGHT];
static uint32_t current_frame;
static uint32_t current_image_index;

static Texture built_in_white_texture;

// The window's render pass is started lazily, by the first thing drawn into it, so the frame can
// first record offscreen passes (gpu.h) into the same command buffer.
static int window_pass_active;
static int frame_recording;

// --- Bindless GPU layer (see gpu.h) ---
static int gpu_available;
static VkDescriptorSetLayout bindless_layout;
static VkDescriptorPool bindless_pool;
static VkDescriptorSet bindless_set;
static VkPipelineLayout gpu_pipeline_layout;
static VkSampler gpu_samplers[GPU_SAMPLER_COUNT];
static uint32_t next_bindless_index;
static uint32_t free_bindless_indices[BINDLESS_CAPACITY];
static uint32_t free_bindless_count;

static MappedBuffer gpu_frame_buffers[MAX_FRAMES_IN_FLIGHT];
static uint32_t gpu_frame_buffer_indices[MAX_FRAMES_IN_FLIGHT];
static uint32_t gpu_frame_offset;

// Hardware ray tracing : one top-level structure per frame in flight, of a fixed capacity.
#define MAX_RAY_INSTANCES 4096

static int rt_available;
static VkAccelerationStructureKHR tlas[MAX_FRAMES_IN_FLIGHT];
static VkBuffer tlas_buffers[MAX_FRAMES_IN_FLIGHT];
static VmaAllocation tlas_allocations[MAX_FRAMES_IN_FLIGHT];
static MappedBuffer tlas_instances[MAX_FRAMES_IN_FLIGHT];
static VkBuffer tlas_scratch;
static VmaAllocation tlas_scratch_allocation;
static VkDeviceAddress tlas_scratch_address;
static uint32_t rt_scratch_alignment;

// The attachments of the current gpu_begin_pass(), transitioned back by gpu_end_pass().
static Texture* gpu_pass_colors[GPU_MAX_COLOR_TARGETS];
static int gpu_pass_color_count;
static Texture* gpu_pass_depth;

// Every Vulkan entry point is loaded at runtime by volk, without linking a Vulkan SDK import library
// (the project doesn't require the Vulkan SDK to be installed). volk is bootstrapped from SDL's own
// vkGetInstanceProcAddr rather than volkInitialize(), so that SDL remains the one place locating
// the Vulkan loader library (the same one SDL_Vulkan_CreateSurface relies on).
static int load_global_functions(void)
{
    PFN_vkGetInstanceProcAddr get_instance_proc_addr = (PFN_vkGetInstanceProcAddr)SDL_Vulkan_GetVkGetInstanceProcAddr();

    if (!get_instance_proc_addr)
    {
        printf("Could not get vkGetInstanceProcAddr: %s\n", SDL_GetError());
        return 0;
    }

    volkInitializeCustom(get_instance_proc_addr);

    return 1;
}

static uint32_t instance_api_version;

static int create_instance(void)
{
    // 1.3 when the loader has it : descriptor indexing and dynamic rendering, what gpu.h is built on.
    instance_api_version = volkGetInstanceVersion() >= VK_API_VERSION_1_3 ? VK_API_VERSION_1_3 : VK_API_VERSION_1_1;

    uint32_t extension_count = 0;
    char const* const* sdl_extensions = SDL_Vulkan_GetInstanceExtensions(&extension_count);

    if (!sdl_extensions)
    {
        printf("Could not get the Vulkan instance extensions required by SDL: %s\n", SDL_GetError());
        return 0;
    }

    VkApplicationInfo application_info = {
        .sType = VK_STRUCTURE_TYPE_APPLICATION_INFO,
        .pApplicationName = "Pireneus",
        .applicationVersion = VK_MAKE_VERSION(1, 0, 0),
        .pEngineName = "Pireneus",
        .engineVersion = VK_MAKE_VERSION(1, 0, 0),
        .apiVersion = instance_api_version
    };

    // No validation layers: they require the Vulkan SDK, which this project intentionally
    // avoids depending on (see third-parties/CMakeLists.txt).
    VkInstanceCreateInfo instance_create_info = {
        .sType = VK_STRUCTURE_TYPE_INSTANCE_CREATE_INFO,
        .pApplicationInfo = &application_info,
        .enabledExtensionCount = extension_count,
        .ppEnabledExtensionNames = sdl_extensions
    };

    if (vkCreateInstance(&instance_create_info, NULL, &instance) != VK_SUCCESS)
    {
        printf("Could not create the Vulkan instance.\n");
        return 0;
    }

    // Instance-level functions only : device-level ones are loaded straight from the device once
    // it exists (volkLoadDevice), skipping the loader's dispatch trampoline on every call.
    volkLoadInstanceOnly(instance);

    return 1;
}

static int is_device_suitable(VkPhysicalDevice candidate, QueueFamilyIndices* out_indices)
{
    QueueFamilyIndices indices = { -1, -1 };

    uint32_t queue_family_count = 0;
    vkGetPhysicalDeviceQueueFamilyProperties(candidate, &queue_family_count, NULL);

    VkQueueFamilyProperties* queue_families = SDL_malloc(sizeof(VkQueueFamilyProperties) * queue_family_count);
    vkGetPhysicalDeviceQueueFamilyProperties(candidate, &queue_family_count, queue_families);

    for (uint32_t i = 0; i < queue_family_count; i++)
    {
        if (queue_families[i].queueFlags & VK_QUEUE_GRAPHICS_BIT)
        {
            indices.graphics_family = (int32_t)i;
        }

        VkBool32 present_support = VK_FALSE;
        vkGetPhysicalDeviceSurfaceSupportKHR(candidate, i, surface, &present_support);

        if (present_support)
        {
            indices.present_family = (int32_t)i;
        }

        if (indices.graphics_family >= 0 && indices.present_family >= 0)
        {
            break;
        }
    }

    SDL_free(queue_families);

    if (indices.graphics_family < 0 || indices.present_family < 0)
    {
        return 0;
    }

    uint32_t extension_count = 0;
    vkEnumerateDeviceExtensionProperties(candidate, NULL, &extension_count, NULL);

    VkExtensionProperties* extensions = SDL_malloc(sizeof(VkExtensionProperties) * extension_count);
    vkEnumerateDeviceExtensionProperties(candidate, NULL, &extension_count, extensions);

    int has_swapchain_extension = 0;

    for (uint32_t i = 0; i < extension_count; i++)
    {
        if (SDL_strcmp(extensions[i].extensionName, VK_KHR_SWAPCHAIN_EXTENSION_NAME) == 0)
        {
            has_swapchain_extension = 1;
            break;
        }
    }

    SDL_free(extensions);

    if (!has_swapchain_extension)
    {
        return 0;
    }

    uint32_t format_count = 0;
    vkGetPhysicalDeviceSurfaceFormatsKHR(candidate, surface, &format_count, NULL);

    uint32_t present_mode_count = 0;
    vkGetPhysicalDeviceSurfacePresentModesKHR(candidate, surface, &present_mode_count, NULL);

    if (format_count == 0 || present_mode_count == 0)
    {
        return 0;
    }

    *out_indices = indices;

    return 1;
}

static int pick_physical_device(void)
{
    uint32_t device_count = 0;
    vkEnumeratePhysicalDevices(instance, &device_count, NULL);

    if (device_count == 0)
    {
        printf("No Vulkan-capable GPU found.\n");
        return 0;
    }

    VkPhysicalDevice* devices = SDL_malloc(sizeof(VkPhysicalDevice) * device_count);
    vkEnumeratePhysicalDevices(instance, &device_count, devices);

    for (uint32_t i = 0; i < device_count; i++)
    {
        QueueFamilyIndices indices;

        if (!is_device_suitable(devices[i], &indices))
        {
            continue;
        }

        VkPhysicalDeviceProperties properties;
        vkGetPhysicalDeviceProperties(devices[i], &properties);

        if (physical_device == VK_NULL_HANDLE || properties.deviceType == VK_PHYSICAL_DEVICE_TYPE_DISCRETE_GPU)
        {
            physical_device = devices[i];
            queue_family_indices = indices;

            if (properties.deviceType == VK_PHYSICAL_DEVICE_TYPE_DISCRETE_GPU)
            {
                break;
            }
        }
    }

    SDL_free(devices);

    if (physical_device == VK_NULL_HANDLE)
    {
        printf("No suitable Vulkan GPU found.\n");
        return 0;
    }

    VkPhysicalDeviceProperties chosen_properties;
    vkGetPhysicalDeviceProperties(physical_device, &chosen_properties);
    SDL_strlcpy(gpu_name, chosen_properties.deviceName, sizeof(gpu_name));
    DEBUG_LOG("[vulkan] using device: %s\n", chosen_properties.deviceName);

    return 1;
}

static int create_logical_device(void)
{
    float queue_priority = 1.0f;

    VkDeviceQueueCreateInfo queue_create_infos[2];
    uint32_t queue_create_info_count = 0;

    queue_create_infos[queue_create_info_count++] = (VkDeviceQueueCreateInfo){
        .sType = VK_STRUCTURE_TYPE_DEVICE_QUEUE_CREATE_INFO,
        .queueFamilyIndex = (uint32_t)queue_family_indices.graphics_family,
        .queueCount = 1,
        .pQueuePriorities = &queue_priority
    };

    if (queue_family_indices.present_family != queue_family_indices.graphics_family)
    {
        queue_create_infos[queue_create_info_count++] = (VkDeviceQueueCreateInfo){
            .sType = VK_STRUCTURE_TYPE_DEVICE_QUEUE_CREATE_INFO,
            .queueFamilyIndex = (uint32_t)queue_family_indices.present_family,
            .queueCount = 1,
            .pQueuePriorities = &queue_priority
        };
    }

    // VK_KHR_maintenance1 (core since 1.1, requested unconditionally for the near-universal
    // hardware/driver support it has) allows a negative-height viewport, used to make Vulkan's
    // clip space match D3D12's (see ortho.h) without any per-backend shader differences.
    const char* device_extensions[8] = { VK_KHR_SWAPCHAIN_EXTENSION_NAME, VK_KHR_MAINTENANCE1_EXTENSION_NAME };
    uint32_t device_extension_count = 2;

    // The GPU layer's needs (gpu.h), enabled only if the device has all of them : without them, the
    // engine still runs, on its simple renderer only.
    VkPhysicalDeviceVulkan13Features features13 = { .sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_3_FEATURES };
    VkPhysicalDeviceVulkan12Features features12 = { .sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_2_FEATURES, .pNext = &features13 };
    VkPhysicalDeviceFeatures2 features2 = { .sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_FEATURES_2, .pNext = &features12 };

    VkPhysicalDeviceProperties device_properties;
    vkGetPhysicalDeviceProperties(physical_device, &device_properties);

    gpu_available = 0;

    if (instance_api_version >= VK_API_VERSION_1_3 && device_properties.apiVersion >= VK_API_VERSION_1_3)
    {
        vkGetPhysicalDeviceFeatures2(physical_device, &features2);

        gpu_available = features13.dynamicRendering && features12.runtimeDescriptorArray &&
            features12.descriptorBindingPartiallyBound && features12.descriptorBindingSampledImageUpdateAfterBind &&
            features12.descriptorBindingStorageBufferUpdateAfterBind && features12.descriptorBindingUpdateUnusedWhilePending &&
            features12.shaderSampledImageArrayNonUniformIndexing && features12.shaderStorageBufferArrayNonUniformIndexing &&
            features2.features.geometryShader && features2.features.samplerAnisotropy;
    }

    // Ray tracing, on top of that, if the device has it.
    rt_available = 0;

    if (gpu_available)
    {
        uint32_t extension_count = 0;
        vkEnumerateDeviceExtensionProperties(physical_device, NULL, &extension_count, NULL);
        VkExtensionProperties* extensions = SDL_malloc(sizeof(VkExtensionProperties) * extension_count);
        vkEnumerateDeviceExtensionProperties(physical_device, NULL, &extension_count, extensions);

        int found = 0;

        for (uint32_t i = 0; i < extension_count; i++)
        {
            found += SDL_strcmp(extensions[i].extensionName, VK_KHR_ACCELERATION_STRUCTURE_EXTENSION_NAME) == 0;
            found += SDL_strcmp(extensions[i].extensionName, VK_KHR_RAY_QUERY_EXTENSION_NAME) == 0;
            found += SDL_strcmp(extensions[i].extensionName, VK_KHR_DEFERRED_HOST_OPERATIONS_EXTENSION_NAME) == 0;
        }

        SDL_free(extensions);

        VkPhysicalDeviceRayQueryFeaturesKHR ray_query = { .sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_RAY_QUERY_FEATURES_KHR };
        VkPhysicalDeviceAccelerationStructureFeaturesKHR acceleration = { .sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_ACCELERATION_STRUCTURE_FEATURES_KHR, .pNext = &ray_query };
        VkPhysicalDeviceFeatures2 query = { .sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_FEATURES_2, .pNext = &acceleration };

        if (found == 3)
        {
            vkGetPhysicalDeviceFeatures2(physical_device, &query);
            rt_available = ray_query.rayQuery && acceleration.accelerationStructure && features12.bufferDeviceAddress;
        }
    }

    VkPhysicalDeviceRayQueryFeaturesKHR enable_ray_query = {
        .sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_RAY_QUERY_FEATURES_KHR,
        .rayQuery = VK_TRUE
    };

    VkPhysicalDeviceAccelerationStructureFeaturesKHR enable_acceleration = {
        .sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_ACCELERATION_STRUCTURE_FEATURES_KHR,
        .pNext = &enable_ray_query,
        .accelerationStructure = VK_TRUE
    };

    if (rt_available)
    {
        device_extensions[device_extension_count++] = VK_KHR_ACCELERATION_STRUCTURE_EXTENSION_NAME;
        device_extensions[device_extension_count++] = VK_KHR_RAY_QUERY_EXTENSION_NAME;
        device_extensions[device_extension_count++] = VK_KHR_DEFERRED_HOST_OPERATIONS_EXTENSION_NAME;
    }

    VkPhysicalDeviceVulkan13Features enable13 = {
        .sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_3_FEATURES,
        .pNext = rt_available ? &enable_acceleration : NULL,
        .dynamicRendering = VK_TRUE
    };

    VkPhysicalDeviceVulkan12Features enable12 = {
        .sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_2_FEATURES,
        .pNext = &enable13,
        .runtimeDescriptorArray = VK_TRUE,
        .descriptorBindingPartiallyBound = VK_TRUE,
        .descriptorBindingSampledImageUpdateAfterBind = VK_TRUE,
        .descriptorBindingStorageBufferUpdateAfterBind = VK_TRUE,
        .descriptorBindingUpdateUnusedWhilePending = VK_TRUE,
        .shaderSampledImageArrayNonUniformIndexing = VK_TRUE,
        .shaderStorageBufferArrayNonUniformIndexing = VK_TRUE,
        .bufferDeviceAddress = rt_available ? VK_TRUE : VK_FALSE
    };

    // PrimitiveId in a fragment shader (the visibility buffer) is part of the geometry shader feature.
    VkPhysicalDeviceFeatures2 enable2 = {
        .sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_FEATURES_2,
        .pNext = &enable12,
        .features = { .geometryShader = VK_TRUE, .samplerAnisotropy = VK_TRUE }
    };

    // Video memory usage and budget for the profiler (VMA reads them through this extension).
    memory_budget_enabled = 0;
    {
        uint32_t extension_count = 0;
        vkEnumerateDeviceExtensionProperties(physical_device, NULL, &extension_count, NULL);
        VkExtensionProperties* extensions = SDL_malloc(sizeof(VkExtensionProperties) * SDL_max(1, extension_count));
        vkEnumerateDeviceExtensionProperties(physical_device, NULL, &extension_count, extensions);

        for (uint32_t i = 0; i < extension_count; i++)
        {
            if (SDL_strcmp(extensions[i].extensionName, VK_EXT_MEMORY_BUDGET_EXTENSION_NAME) == 0)
            {
                memory_budget_enabled = 1;
                device_extensions[device_extension_count++] = VK_EXT_MEMORY_BUDGET_EXTENSION_NAME;
            }
        }

        SDL_free(extensions);
    }

    VkDeviceCreateInfo device_create_info = {
        .sType = VK_STRUCTURE_TYPE_DEVICE_CREATE_INFO,
        .pNext = gpu_available ? &enable2 : NULL,
        .queueCreateInfoCount = queue_create_info_count,
        .pQueueCreateInfos = queue_create_infos,
        .enabledExtensionCount = device_extension_count,
        .ppEnabledExtensionNames = device_extensions
    };

    if (vkCreateDevice(physical_device, &device_create_info, NULL, &device) != VK_SUCCESS)
    {
        printf("Could not create the Vulkan logical device.\n");
        return 0;
    }

    volkLoadDevice(device);

    if (!gpu_available)
    {
        printf("This GPU or driver lacks Vulkan 1.3 bindless rendering : only the simple renderer is available.\n");
    }

    vkGetDeviceQueue(device, (uint32_t)queue_family_indices.graphics_family, 0, &graphics_queue);
    vkGetDeviceQueue(device, (uint32_t)queue_family_indices.present_family, 0, &present_queue);

    return 1;
}

// A UNORM swapchain on purpose (not *_SRGB) : textures (UNORM, see create_texture) and vertex colors
// are already sRGB-encoded values, and the shader just passes them through. An *_SRGB swapchain would
// make the GPU apply a second linear->sRGB encode on write, washing colors out (visibly brighter /
// less saturated than DX12, whose swapchain is DXGI_FORMAT_R8G8B8A8_UNORM). It also keeps the swapchain
// render pass format-compatible with the UNORM render target pass, which shares the same pipeline.
static int create_allocator(void)
{
    VmaAllocatorCreateInfo allocator_create_info = {
        .flags = (rt_available ? VMA_ALLOCATOR_CREATE_BUFFER_DEVICE_ADDRESS_BIT : 0) |
            (memory_budget_enabled ? VMA_ALLOCATOR_CREATE_EXT_MEMORY_BUDGET_BIT : 0),
        .physicalDevice = physical_device,
        .device = device,
        .instance = instance,
        .vulkanApiVersion = instance_api_version // must match create_instance()'s apiVersion
    };

    VmaVulkanFunctions vulkan_functions;

    if (vmaImportVulkanFunctionsFromVolk(&allocator_create_info, &vulkan_functions) != VK_SUCCESS)
    {
        printf("Could not import the Vulkan functions into VMA.\n");
        return 0;
    }

    allocator_create_info.pVulkanFunctions = &vulkan_functions;

    if (vmaCreateAllocator(&allocator_create_info, &allocator) != VK_SUCCESS)
    {
        printf("Could not create the Vulkan memory allocator.\n");
        return 0;
    }

    return 1;
}

// Optional : without timestamps on the graphics queue, the GPU time is reported as unknown.
static void create_timestamp_queries(void)
{
    VkPhysicalDeviceProperties properties;
    vkGetPhysicalDeviceProperties(physical_device, &properties);

    uint32_t family_count = 0;
    vkGetPhysicalDeviceQueueFamilyProperties(physical_device, &family_count, NULL);
    VkQueueFamilyProperties* families = SDL_malloc(sizeof(VkQueueFamilyProperties) * SDL_max(1, family_count));
    vkGetPhysicalDeviceQueueFamilyProperties(physical_device, &family_count, families);
    uint32_t valid_bits = families[queue_family_indices.graphics_family].timestampValidBits;
    SDL_free(families);

    if (valid_bits == 0 || properties.limits.timestampPeriod <= 0.0f)
    {
        return;
    }

    VkQueryPoolCreateInfo pool_info = {
        .sType = VK_STRUCTURE_TYPE_QUERY_POOL_CREATE_INFO,
        .queryType = VK_QUERY_TYPE_TIMESTAMP,
        .queryCount = 2 * MAX_FRAMES_IN_FLIGHT
    };

    if (vkCreateQueryPool(device, &pool_info, NULL, &timestamp_pool) != VK_SUCCESS)
    {
        timestamp_pool = VK_NULL_HANDLE;
        return;
    }

    timestamp_period_ns = properties.limits.timestampPeriod;
    timestamp_mask = valid_bits >= 64 ? ~0ull : ((1ull << valid_bits) - 1ull);
}

static void vulkan_get_stats(RendererStats* stats)
{
    stats->backend = "Vulkan";
    SDL_strlcpy(stats->gpu_name, gpu_name, sizeof(stats->gpu_name));
    stats->gpu_frame_ms = last_gpu_frame_ms;

    if (!allocator)
    {
        return;
    }

    const VkPhysicalDeviceMemoryProperties* memory = NULL;
    vmaGetMemoryProperties(allocator, &memory);
    VmaBudget budgets[VK_MAX_MEMORY_HEAPS];
    vmaGetHeapBudgets(allocator, budgets);

    for (uint32_t i = 0; i < memory->memoryHeapCount; i++)
    {
        if (memory->memoryHeaps[i].flags & VK_MEMORY_HEAP_DEVICE_LOCAL_BIT)
        {
            stats->vram_used += budgets[i].usage;
            stats->vram_budget += budgets[i].budget;
        }
    }
}

static VkSurfaceFormatKHR choose_surface_format(const VkSurfaceFormatKHR* formats, uint32_t count)
{
    for (uint32_t i = 0; i < count; i++)
    {
        if ((formats[i].format == VK_FORMAT_B8G8R8A8_UNORM || formats[i].format == VK_FORMAT_R8G8B8A8_UNORM) &&
            formats[i].colorSpace == VK_COLOR_SPACE_SRGB_NONLINEAR_KHR)
        {
            return formats[i];
        }
    }

    return formats[0];
}

static VkPresentModeKHR choose_present_mode(const VkPresentModeKHR* present_modes, uint32_t count)
{
    for (uint32_t i = 0; i < count; i++)
    {
        if (present_modes[i] == VK_PRESENT_MODE_MAILBOX_KHR)
        {
            return present_modes[i];
        }
    }

    return VK_PRESENT_MODE_FIFO_KHR;
}

static VkExtent2D choose_extent(const VkSurfaceCapabilitiesKHR* capabilities)
{
    if (capabilities->currentExtent.width != UINT32_MAX)
    {
        return capabilities->currentExtent;
    }

    int width = 0;
    int height = 0;
    SDL_GetWindowSizeInPixels(window, &width, &height);

    VkExtent2D extent = { (uint32_t)width, (uint32_t)height };

    extent.width = SDL_clamp(extent.width, capabilities->minImageExtent.width, capabilities->maxImageExtent.width);
    extent.height = SDL_clamp(extent.height, capabilities->minImageExtent.height, capabilities->maxImageExtent.height);

    return extent;
}

static int create_swapchain(void)
{
    VkSurfaceCapabilitiesKHR capabilities;
    vkGetPhysicalDeviceSurfaceCapabilitiesKHR(physical_device, surface, &capabilities);

    uint32_t format_count = 0;
    vkGetPhysicalDeviceSurfaceFormatsKHR(physical_device, surface, &format_count, NULL);
    VkSurfaceFormatKHR* formats = SDL_malloc(sizeof(VkSurfaceFormatKHR) * format_count);
    vkGetPhysicalDeviceSurfaceFormatsKHR(physical_device, surface, &format_count, formats);

    uint32_t present_mode_count = 0;
    vkGetPhysicalDeviceSurfacePresentModesKHR(physical_device, surface, &present_mode_count, NULL);
    VkPresentModeKHR* present_modes = SDL_malloc(sizeof(VkPresentModeKHR) * present_mode_count);
    vkGetPhysicalDeviceSurfacePresentModesKHR(physical_device, surface, &present_mode_count, present_modes);

    VkSurfaceFormatKHR surface_format = choose_surface_format(formats, format_count);
    VkPresentModeKHR present_mode = choose_present_mode(present_modes, present_mode_count);
    VkExtent2D extent = choose_extent(&capabilities);

    SDL_free(formats);
    SDL_free(present_modes);

    uint32_t image_count = capabilities.minImageCount + 1;

    if (capabilities.maxImageCount > 0 && image_count > capabilities.maxImageCount)
    {
        image_count = capabilities.maxImageCount;
    }

    uint32_t queue_family_indices_array[2] = {
        (uint32_t)queue_family_indices.graphics_family,
        (uint32_t)queue_family_indices.present_family
    };

    VkSwapchainCreateInfoKHR swapchain_create_info = {
        .sType = VK_STRUCTURE_TYPE_SWAPCHAIN_CREATE_INFO_KHR,
        .surface = surface,
        .minImageCount = image_count,
        .imageFormat = surface_format.format,
        .imageColorSpace = surface_format.colorSpace,
        .imageExtent = extent,
        .imageArrayLayers = 1,
        .imageUsage = VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT,
        .preTransform = capabilities.currentTransform,
        .compositeAlpha = VK_COMPOSITE_ALPHA_OPAQUE_BIT_KHR,
        .presentMode = present_mode,
        .clipped = VK_TRUE,
        .oldSwapchain = VK_NULL_HANDLE
    };

    if (queue_family_indices.graphics_family != queue_family_indices.present_family)
    {
        swapchain_create_info.imageSharingMode = VK_SHARING_MODE_CONCURRENT;
        swapchain_create_info.queueFamilyIndexCount = 2;
        swapchain_create_info.pQueueFamilyIndices = queue_family_indices_array;
    }
    else
    {
        swapchain_create_info.imageSharingMode = VK_SHARING_MODE_EXCLUSIVE;
    }

    if (vkCreateSwapchainKHR(device, &swapchain_create_info, NULL, &swapchain) != VK_SUCCESS)
    {
        printf("Could not create the Vulkan swapchain.\n");
        return 0;
    }

    swapchain_image_format = surface_format.format;
    swapchain_extent = extent;

    vkGetSwapchainImagesKHR(device, swapchain, &swapchain_image_count, NULL);
    swapchain_images = SDL_malloc(sizeof(VkImage) * swapchain_image_count);
    vkGetSwapchainImagesKHR(device, swapchain, &swapchain_image_count, swapchain_images);

    return 1;
}

static int create_image_views(void)
{
    swapchain_image_views = SDL_malloc(sizeof(VkImageView) * swapchain_image_count);

    for (uint32_t i = 0; i < swapchain_image_count; i++)
    {
        VkImageViewCreateInfo view_create_info = {
            .sType = VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO,
            .image = swapchain_images[i],
            .viewType = VK_IMAGE_VIEW_TYPE_2D,
            .format = swapchain_image_format,
            .components = {
                VK_COMPONENT_SWIZZLE_IDENTITY, VK_COMPONENT_SWIZZLE_IDENTITY,
                VK_COMPONENT_SWIZZLE_IDENTITY, VK_COMPONENT_SWIZZLE_IDENTITY
            },
            .subresourceRange = {
                .aspectMask = VK_IMAGE_ASPECT_COLOR_BIT,
                .baseMipLevel = 0,
                .levelCount = 1,
                .baseArrayLayer = 0,
                .layerCount = 1
            }
        };

        if (vkCreateImageView(device, &view_create_info, NULL, &swapchain_image_views[i]) != VK_SUCCESS)
        {
            printf("Could not create a Vulkan image view.\n");
            return 0;
        }
    }

    return 1;
}

static int create_render_pass(void)
{
    VkAttachmentDescription attachments[2] = {
        {
            .format = swapchain_image_format,
            .samples = VK_SAMPLE_COUNT_1_BIT,
            .loadOp = VK_ATTACHMENT_LOAD_OP_CLEAR,
            .storeOp = VK_ATTACHMENT_STORE_OP_STORE,
            .stencilLoadOp = VK_ATTACHMENT_LOAD_OP_DONT_CARE,
            .stencilStoreOp = VK_ATTACHMENT_STORE_OP_DONT_CARE,
            .initialLayout = VK_IMAGE_LAYOUT_UNDEFINED,
            .finalLayout = VK_IMAGE_LAYOUT_PRESENT_SRC_KHR
        },
        {
            // Depth for 3D scenes (begin_scene) : cleared every frame, never stored. The 2D
            // pipelines simply have depth testing disabled.
            .format = DEPTH_FORMAT,
            .samples = VK_SAMPLE_COUNT_1_BIT,
            .loadOp = VK_ATTACHMENT_LOAD_OP_CLEAR,
            .storeOp = VK_ATTACHMENT_STORE_OP_DONT_CARE,
            .stencilLoadOp = VK_ATTACHMENT_LOAD_OP_DONT_CARE,
            .stencilStoreOp = VK_ATTACHMENT_STORE_OP_DONT_CARE,
            .initialLayout = VK_IMAGE_LAYOUT_UNDEFINED,
            .finalLayout = VK_IMAGE_LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL
        }
    };

    VkAttachmentReference color_attachment_ref = {
        .attachment = 0,
        .layout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL
    };

    VkAttachmentReference depth_attachment_ref = {
        .attachment = 1,
        .layout = VK_IMAGE_LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL
    };

    VkSubpassDescription subpass = {
        .pipelineBindPoint = VK_PIPELINE_BIND_POINT_GRAPHICS,
        .colorAttachmentCount = 1,
        .pColorAttachments = &color_attachment_ref,
        .pDepthStencilAttachment = &depth_attachment_ref
    };

    // Also orders this frame's depth clear/writes after the previous frame's : a single depth
    // image is shared by every frame in flight.
    VkSubpassDependency dependency = {
        .srcSubpass = VK_SUBPASS_EXTERNAL,
        .dstSubpass = 0,
        .srcStageMask = VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT | VK_PIPELINE_STAGE_LATE_FRAGMENT_TESTS_BIT,
        .srcAccessMask = VK_ACCESS_DEPTH_STENCIL_ATTACHMENT_WRITE_BIT,
        .dstStageMask = VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT | VK_PIPELINE_STAGE_EARLY_FRAGMENT_TESTS_BIT,
        .dstAccessMask = VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT | VK_ACCESS_DEPTH_STENCIL_ATTACHMENT_WRITE_BIT
    };

    VkRenderPassCreateInfo render_pass_create_info = {
        .sType = VK_STRUCTURE_TYPE_RENDER_PASS_CREATE_INFO,
        .attachmentCount = 2,
        .pAttachments = attachments,
        .subpassCount = 1,
        .pSubpasses = &subpass,
        .dependencyCount = 1,
        .pDependencies = &dependency
    };

    if (vkCreateRenderPass(device, &render_pass_create_info, NULL, &render_pass) != VK_SUCCESS)
    {
        printf("Could not create the Vulkan render pass.\n");
        return 0;
    }

    return 1;
}

static int create_shadow_render_pass(void)
{
    VkFormatProperties format_properties;
    vkGetPhysicalDeviceFormatProperties(physical_device, DEPTH_FORMAT, &format_properties);

    VkFormatFeatureFlags required = VK_FORMAT_FEATURE_DEPTH_STENCIL_ATTACHMENT_BIT | VK_FORMAT_FEATURE_SAMPLED_IMAGE_BIT;

    if ((format_properties.optimalTilingFeatures & required) != required)
    {
        printf("This GPU can't sample D32_SFLOAT depth images : 3D rendering is unavailable.\n");
        mesh_supported = 0;
    }

    VkAttachmentDescription depth_attachment = {
        .format = DEPTH_FORMAT,
        .samples = VK_SAMPLE_COUNT_1_BIT,
        .loadOp = VK_ATTACHMENT_LOAD_OP_CLEAR,
        .storeOp = VK_ATTACHMENT_STORE_OP_STORE,
        .stencilLoadOp = VK_ATTACHMENT_LOAD_OP_DONT_CARE,
        .stencilStoreOp = VK_ATTACHMENT_STORE_OP_DONT_CARE,
        .initialLayout = VK_IMAGE_LAYOUT_UNDEFINED,
        .finalLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL
    };

    VkAttachmentReference depth_attachment_ref = {
        .attachment = 0,
        .layout = VK_IMAGE_LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL
    };

    VkSubpassDescription subpass = {
        .pipelineBindPoint = VK_PIPELINE_BIND_POINT_GRAPHICS,
        .colorAttachmentCount = 0,
        .pDepthStencilAttachment = &depth_attachment_ref
    };

    VkSubpassDependency dependencies[2] = {
        {
            // Earlier frames (still in flight) sampling this shadow map, before it's overwritten.
            .srcSubpass = VK_SUBPASS_EXTERNAL,
            .dstSubpass = 0,
            .srcStageMask = VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT,
            .srcAccessMask = VK_ACCESS_SHADER_READ_BIT,
            .dstStageMask = VK_PIPELINE_STAGE_EARLY_FRAGMENT_TESTS_BIT,
            .dstAccessMask = VK_ACCESS_DEPTH_STENCIL_ATTACHMENT_WRITE_BIT
        },
        {
            // This pass's depth writes, before the scene pass samples them.
            .srcSubpass = 0,
            .dstSubpass = VK_SUBPASS_EXTERNAL,
            .srcStageMask = VK_PIPELINE_STAGE_LATE_FRAGMENT_TESTS_BIT,
            .srcAccessMask = VK_ACCESS_DEPTH_STENCIL_ATTACHMENT_WRITE_BIT,
            .dstStageMask = VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT,
            .dstAccessMask = VK_ACCESS_SHADER_READ_BIT
        }
    };

    VkRenderPassCreateInfo render_pass_create_info = {
        .sType = VK_STRUCTURE_TYPE_RENDER_PASS_CREATE_INFO,
        .attachmentCount = 1,
        .pAttachments = &depth_attachment,
        .subpassCount = 1,
        .pSubpasses = &subpass,
        .dependencyCount = 2,
        .pDependencies = dependencies
    };

    if (vkCreateRenderPass(device, &render_pass_create_info, NULL, &shadow_render_pass) != VK_SUCCESS)
    {
        printf("Could not create the Vulkan shadow render pass.\n");
        return 0;
    }

    return 1;
}

static int create_render_target_render_pass(void)
{
    VkAttachmentDescription color_attachment = {
        .format = VK_FORMAT_R8G8B8A8_UNORM,
        .samples = VK_SAMPLE_COUNT_1_BIT,
        .loadOp = VK_ATTACHMENT_LOAD_OP_CLEAR,
        .storeOp = VK_ATTACHMENT_STORE_OP_STORE,
        .stencilLoadOp = VK_ATTACHMENT_LOAD_OP_DONT_CARE,
        .stencilStoreOp = VK_ATTACHMENT_STORE_OP_DONT_CARE,
        .initialLayout = VK_IMAGE_LAYOUT_UNDEFINED,
        .finalLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL
    };

    VkAttachmentReference color_attachment_ref = {
        .attachment = 0,
        .layout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL
    };

    VkSubpassDescription subpass = {
        .pipelineBindPoint = VK_PIPELINE_BIND_POINT_GRAPHICS,
        .colorAttachmentCount = 1,
        .pColorAttachments = &color_attachment_ref
    };

    VkSubpassDependency dependency = {
        .srcSubpass = VK_SUBPASS_EXTERNAL,
        .dstSubpass = 0,
        .srcStageMask = VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT,
        .srcAccessMask = 0,
        .dstStageMask = VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT,
        .dstAccessMask = VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT
    };

    VkRenderPassCreateInfo render_pass_create_info = {
        .sType = VK_STRUCTURE_TYPE_RENDER_PASS_CREATE_INFO,
        .attachmentCount = 1,
        .pAttachments = &color_attachment,
        .subpassCount = 1,
        .pSubpasses = &subpass,
        .dependencyCount = 1,
        .pDependencies = &dependency
    };

    if (vkCreateRenderPass(device, &render_pass_create_info, NULL, &render_target_render_pass) != VK_SUCCESS)
    {
        printf("Could not create the Vulkan render-target render pass.\n");
        return 0;
    }

    return 1;
}

static int create_sampler(void)
{
    // Nearest-neighbour filtering: the tileset is a pixel-art atlas, and linear filtering
    // would bleed adjacent tiles into each other at their edges.
    VkSamplerCreateInfo sampler_create_info = {
        .sType = VK_STRUCTURE_TYPE_SAMPLER_CREATE_INFO,
        .magFilter = VK_FILTER_NEAREST,
        .minFilter = VK_FILTER_NEAREST,
        .mipmapMode = VK_SAMPLER_MIPMAP_MODE_NEAREST,
        .addressModeU = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE,
        .addressModeV = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE,
        .addressModeW = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE,
        .maxLod = 0.0f
    };

    if (vkCreateSampler(device, &sampler_create_info, NULL, &sampler) != VK_SUCCESS)
    {
        printf("Could not create the Vulkan sampler.\n");
        return 0;
    }

    return 1;
}

static int create_descriptor_set_layout(void)
{
    VkDescriptorSetLayoutBinding binding = {
        .binding = 0,
        .descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER,
        .descriptorCount = 1,
        .stageFlags = VK_SHADER_STAGE_FRAGMENT_BIT
    };

    VkDescriptorSetLayoutCreateInfo layout_create_info = {
        .sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO,
        .bindingCount = 1,
        .pBindings = &binding
    };

    if (vkCreateDescriptorSetLayout(device, &layout_create_info, NULL, &descriptor_set_layout) != VK_SUCCESS)
    {
        printf("Could not create the Vulkan descriptor set layout.\n");
        return 0;
    }

    return 1;
}

static int create_descriptor_pool(void)
{
    VkDescriptorPoolSize pool_size = {
        .type = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER,
        .descriptorCount = MAX_TEXTURES
    };

    VkDescriptorPoolCreateInfo pool_create_info = {
        .sType = VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO,
        .flags = VK_DESCRIPTOR_POOL_CREATE_FREE_DESCRIPTOR_SET_BIT,
        .maxSets = MAX_TEXTURES,
        .poolSizeCount = 1,
        .pPoolSizes = &pool_size
    };

    if (vkCreateDescriptorPool(device, &pool_create_info, NULL, &descriptor_pool) != VK_SUCCESS)
    {
        printf("Could not create the Vulkan descriptor pool.\n");
        return 0;
    }

    return 1;
}

static int create_bindless(void)
{
    if (!gpu_available)
    {
        return 1;
    }

    // The fixed samplers, g_sampler_* in photoreal.slang (bindings 2 to 5).
    VkSamplerCreateInfo sampler_infos[GPU_SAMPLER_COUNT] = {
        [GPU_SAMPLER_POINT_CLAMP] = {
            .sType = VK_STRUCTURE_TYPE_SAMPLER_CREATE_INFO,
            .magFilter = VK_FILTER_NEAREST, .minFilter = VK_FILTER_NEAREST, .mipmapMode = VK_SAMPLER_MIPMAP_MODE_NEAREST,
            .addressModeU = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE, .addressModeV = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE,
            .addressModeW = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE, .maxLod = VK_LOD_CLAMP_NONE
        },
        [GPU_SAMPLER_LINEAR_CLAMP] = {
            .sType = VK_STRUCTURE_TYPE_SAMPLER_CREATE_INFO,
            .magFilter = VK_FILTER_LINEAR, .minFilter = VK_FILTER_LINEAR, .mipmapMode = VK_SAMPLER_MIPMAP_MODE_LINEAR,
            .addressModeU = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE, .addressModeV = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE,
            .addressModeW = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE, .maxLod = VK_LOD_CLAMP_NONE
        },
        [GPU_SAMPLER_ANISOTROPIC_WRAP] = {
            .sType = VK_STRUCTURE_TYPE_SAMPLER_CREATE_INFO,
            .magFilter = VK_FILTER_LINEAR, .minFilter = VK_FILTER_LINEAR, .mipmapMode = VK_SAMPLER_MIPMAP_MODE_LINEAR,
            .addressModeU = VK_SAMPLER_ADDRESS_MODE_REPEAT, .addressModeV = VK_SAMPLER_ADDRESS_MODE_REPEAT,
            .addressModeW = VK_SAMPLER_ADDRESS_MODE_REPEAT, .anisotropyEnable = VK_TRUE, .maxAnisotropy = 16.0f, .maxLod = VK_LOD_CLAMP_NONE
        },
        [GPU_SAMPLER_PANORAMA] = {
            .sType = VK_STRUCTURE_TYPE_SAMPLER_CREATE_INFO,
            .magFilter = VK_FILTER_LINEAR, .minFilter = VK_FILTER_LINEAR, .mipmapMode = VK_SAMPLER_MIPMAP_MODE_LINEAR,
            .addressModeU = VK_SAMPLER_ADDRESS_MODE_REPEAT, .addressModeV = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE,
            .addressModeW = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE, .maxLod = VK_LOD_CLAMP_NONE
        }
    };

    for (int i = 0; i < GPU_SAMPLER_COUNT; i++)
    {
        if (vkCreateSampler(device, &sampler_infos[i], NULL, &gpu_samplers[i]) != VK_SUCCESS)
        {
            printf("Could not create a Vulkan bindless sampler.\n");
            return 0;
        }
    }

    // The table's two arrays, then one immutable sampler per binding (shaders declare them as separate
    // variables, see photoreal.slang).
    VkDescriptorSetLayoutBinding bindings[3 + GPU_SAMPLER_COUNT] = {
        { .binding = 0, .descriptorType = VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE, .descriptorCount = BINDLESS_CAPACITY, .stageFlags = VK_SHADER_STAGE_ALL },
        { .binding = 1, .descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, .descriptorCount = BINDLESS_CAPACITY, .stageFlags = VK_SHADER_STAGE_ALL }
    };

    VkDescriptorBindingFlags binding_flags[3 + GPU_SAMPLER_COUNT] = {
        VK_DESCRIPTOR_BINDING_PARTIALLY_BOUND_BIT | VK_DESCRIPTOR_BINDING_UPDATE_AFTER_BIND_BIT | VK_DESCRIPTOR_BINDING_UPDATE_UNUSED_WHILE_PENDING_BIT,
        VK_DESCRIPTOR_BINDING_PARTIALLY_BOUND_BIT | VK_DESCRIPTOR_BINDING_UPDATE_AFTER_BIND_BIT | VK_DESCRIPTOR_BINDING_UPDATE_UNUSED_WHILE_PENDING_BIT
    };

    for (uint32_t i = 0; i < GPU_SAMPLER_COUNT; i++)
    {
        VkDescriptorSetLayoutBinding binding = {
            .binding = 2 + i,
            .descriptorType = VK_DESCRIPTOR_TYPE_SAMPLER,
            .descriptorCount = 1,
            .stageFlags = VK_SHADER_STAGE_ALL,
            .pImmutableSamplers = &gpu_samplers[i]
        };

        bindings[2 + i] = binding;
    }

    uint32_t binding_count = 2 + GPU_SAMPLER_COUNT;

    if (rt_available)
    {
        VkDescriptorSetLayoutBinding binding = {
            .binding = binding_count,
            .descriptorType = VK_DESCRIPTOR_TYPE_ACCELERATION_STRUCTURE_KHR,
            .descriptorCount = MAX_FRAMES_IN_FLIGHT,
            .stageFlags = VK_SHADER_STAGE_ALL
        };

        bindings[binding_count] = binding;
        binding_flags[binding_count] = VK_DESCRIPTOR_BINDING_PARTIALLY_BOUND_BIT;
        binding_count++;
    }

    VkDescriptorSetLayoutBindingFlagsCreateInfo flags_info = {
        .sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_BINDING_FLAGS_CREATE_INFO,
        .bindingCount = binding_count,
        .pBindingFlags = binding_flags
    };

    VkDescriptorSetLayoutCreateInfo layout_info = {
        .sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO,
        .pNext = &flags_info,
        .flags = VK_DESCRIPTOR_SET_LAYOUT_CREATE_UPDATE_AFTER_BIND_POOL_BIT,
        .bindingCount = binding_count,
        .pBindings = bindings
    };

    if (vkCreateDescriptorSetLayout(device, &layout_info, NULL, &bindless_layout) != VK_SUCCESS)
    {
        printf("Could not create the Vulkan bindless set layout.\n");
        return 0;
    }

    VkDescriptorPoolSize pool_sizes[4] = {
        { VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE, BINDLESS_CAPACITY },
        { VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, BINDLESS_CAPACITY },
        { VK_DESCRIPTOR_TYPE_SAMPLER, GPU_SAMPLER_COUNT },
        { VK_DESCRIPTOR_TYPE_ACCELERATION_STRUCTURE_KHR, MAX_FRAMES_IN_FLIGHT }
    };

    VkDescriptorPoolCreateInfo pool_info = {
        .sType = VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO,
        .flags = VK_DESCRIPTOR_POOL_CREATE_UPDATE_AFTER_BIND_BIT,
        .maxSets = 1,
        .poolSizeCount = rt_available ? 4 : 3,
        .pPoolSizes = pool_sizes
    };

    if (vkCreateDescriptorPool(device, &pool_info, NULL, &bindless_pool) != VK_SUCCESS)
    {
        printf("Could not create the Vulkan bindless descriptor pool.\n");
        return 0;
    }

    VkDescriptorSetAllocateInfo allocate_info = {
        .sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO,
        .descriptorPool = bindless_pool,
        .descriptorSetCount = 1,
        .pSetLayouts = &bindless_layout
    };

    if (vkAllocateDescriptorSets(device, &allocate_info, &bindless_set) != VK_SUCCESS)
    {
        printf("Could not allocate the Vulkan bindless descriptor set.\n");
        return 0;
    }

    VkPushConstantRange push_range = { VK_SHADER_STAGE_ALL, 0, GPU_MAX_PUSH_CONSTANTS };

    VkPipelineLayoutCreateInfo pipeline_layout_info = {
        .sType = VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO,
        .setLayoutCount = 1,
        .pSetLayouts = &bindless_layout,
        .pushConstantRangeCount = 1,
        .pPushConstantRanges = &push_range
    };

    if (vkCreatePipelineLayout(device, &pipeline_layout_info, NULL, &gpu_pipeline_layout) != VK_SUCCESS)
    {
        printf("Could not create the Vulkan bindless pipeline layout.\n");
        return 0;
    }

    return 1;
}

static uint32_t allocate_bindless_index(void)
{
    if (free_bindless_count > 0)
    {
        return free_bindless_indices[--free_bindless_count];
    }

    if (next_bindless_index >= BINDLESS_CAPACITY)
    {
        printf("The Vulkan bindless table is full (%u entries).\n", BINDLESS_CAPACITY);
        return NO_BINDLESS_INDEX;
    }

    return next_bindless_index++;
}

static void release_bindless_index(uint32_t index)
{
    if (index != NO_BINDLESS_INDEX)
    {
        free_bindless_indices[free_bindless_count++] = index;
    }
}

static void register_bindless_texture(Texture* texture)
{
    texture->bindless_index = NO_BINDLESS_INDEX;

    if (!gpu_available)
    {
        return;
    }

    texture->bindless_index = allocate_bindless_index();

    if (texture->bindless_index == NO_BINDLESS_INDEX)
    {
        return;
    }

    VkDescriptorImageInfo image_info = {
        .imageView = texture->view,
        .imageLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL
    };

    VkWriteDescriptorSet write = {
        .sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET,
        .dstSet = bindless_set,
        .dstBinding = 0,
        .dstArrayElement = texture->bindless_index,
        .descriptorCount = 1,
        .descriptorType = VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE,
        .pImageInfo = &image_info
    };

    vkUpdateDescriptorSets(device, 1, &write, 0, NULL);
}

static uint32_t register_bindless_buffer(VkBuffer buffer)
{
    if (!gpu_available)
    {
        return NO_BINDLESS_INDEX;
    }

    uint32_t index = allocate_bindless_index();

    if (index == NO_BINDLESS_INDEX)
    {
        return index;
    }

    VkDescriptorBufferInfo buffer_info = { buffer, 0, VK_WHOLE_SIZE };

    VkWriteDescriptorSet write = {
        .sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET,
        .dstSet = bindless_set,
        .dstBinding = 1,
        .dstArrayElement = index,
        .descriptorCount = 1,
        .descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER,
        .pBufferInfo = &buffer_info
    };

    vkUpdateDescriptorSets(device, 1, &write, 0, NULL);

    return index;
}

static VkShaderModule create_shader_module(const uint8_t* code, size_t size)
{
    VkShaderModuleCreateInfo create_info = {
        .sType = VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO,
        .codeSize = size,
        .pCode = (const uint32_t*)code
    };

    VkShaderModule shader_module = VK_NULL_HANDLE;

    if (vkCreateShaderModule(device, &create_info, NULL, &shader_module) != VK_SUCCESS)
    {
        return VK_NULL_HANDLE;
    }

    return shader_module;
}

static uint8_t* load_shader(const char* file_name, size_t* out_size)
{
    const char* base_path = SDL_GetBasePath();

    char path[512];
    SDL_snprintf(path, sizeof(path), "%sshaders/%s.spv", base_path ? base_path : "", file_name);

    return SDL_LoadFile(path, out_size);
}

static int create_pipeline_layout(uint32_t push_constant_size, VkShaderStageFlags stages, VkPipelineLayout* out_layout)
{
    VkPushConstantRange push_constant_range = {
        .stageFlags = stages,
        .offset = 0,
        .size = push_constant_size
    };

    VkPipelineLayoutCreateInfo layout_create_info = {
        .sType = VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO,
        .setLayoutCount = 1,
        .pSetLayouts = &descriptor_set_layout,
        .pushConstantRangeCount = 1,
        .pPushConstantRanges = &push_constant_range
    };

    return vkCreatePipelineLayout(device, &layout_create_info, NULL, out_layout) == VK_SUCCESS;
}

static int create_pipeline_layouts(void)
{
    if (!create_pipeline_layout(sizeof(ScreenTransform), VK_SHADER_STAGE_VERTEX_BIT, &pipeline_layout))
    {
        printf("Could not create the Vulkan pipeline layout.\n");
        return 0;
    }

    VkPhysicalDeviceProperties properties;
    vkGetPhysicalDeviceProperties(physical_device, &properties);

    if (properties.limits.maxPushConstantsSize < sizeof(MeshConstants))
    {
        printf("This GPU only has %u bytes of push constants (%u needed) : 3D rendering is unavailable.\n",
            properties.limits.maxPushConstantsSize, (unsigned)sizeof(MeshConstants));
        mesh_supported = 0;
        return 1;
    }

    if (!create_pipeline_layout(sizeof(MeshConstants), VK_SHADER_STAGE_VERTEX_BIT | VK_SHADER_STAGE_FRAGMENT_BIT, &mesh_pipeline_layout))
    {
        printf("Could not create the Vulkan mesh pipeline layout.\n");
        return 0;
    }

    return 1;
}

typedef struct
{
    const char* vertex_shader;   // compiled shader names, see load_shader()
    const char* fragment_shader; // NULL : depth-only pipeline
    const VkVertexInputBindingDescription* binding; // NULL : no vertex buffer (vertices from the vertex index)
    const VkVertexInputAttributeDescription* attributes;
    uint32_t attribute_count;
    VkPipelineLayout layout;
    VkRenderPass render_pass;
    int depth_test;       // depth test + write (LESS)
    int depth_read_only;  // depth test only (LESS_OR_EQUAL), no write : drawing at the far plane
    int alpha_blend;      // premultiplied-style alpha blending, as the 2D pipelines use
    int color_attachment; // 0 for depth-only render passes
} PipelineDescription;

static int create_pipeline(const PipelineDescription* description, VkPipeline* out_pipeline)
{
    size_t vert_size = 0;
    size_t frag_size = 0;

    uint8_t* vert_code = load_shader(description->vertex_shader, &vert_size);
    uint8_t* frag_code = description->fragment_shader ? load_shader(description->fragment_shader, &frag_size) : NULL;

    if (!vert_code || (description->fragment_shader && !frag_code))
    {
        printf("Could not load the '%s' shaders.\n", description->vertex_shader);
        SDL_free(vert_code);
        SDL_free(frag_code);
        return 0;
    }

    VkShaderModule vert_module = create_shader_module(vert_code, vert_size);
    VkShaderModule frag_module = frag_code ? create_shader_module(frag_code, frag_size) : VK_NULL_HANDLE;

    SDL_free(vert_code);
    SDL_free(frag_code);

    if (vert_module == VK_NULL_HANDLE || (description->fragment_shader && frag_module == VK_NULL_HANDLE))
    {
        printf("Could not create the '%s' shader modules.\n", description->vertex_shader);
        vkDestroyShaderModule(device, vert_module, NULL);
        vkDestroyShaderModule(device, frag_module, NULL);
        return 0;
    }

    VkPipelineShaderStageCreateInfo shader_stages[2] = {
        {
            .sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO,
            .stage = VK_SHADER_STAGE_VERTEX_BIT,
            .module = vert_module,
            .pName = "main"
        },
        {
            .sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO,
            .stage = VK_SHADER_STAGE_FRAGMENT_BIT,
            .module = frag_module,
            .pName = "main"
        }
    };

    VkPipelineVertexInputStateCreateInfo vertex_input_state = {
        .sType = VK_STRUCTURE_TYPE_PIPELINE_VERTEX_INPUT_STATE_CREATE_INFO,
        .vertexBindingDescriptionCount = description->binding ? 1 : 0,
        .pVertexBindingDescriptions = description->binding,
        .vertexAttributeDescriptionCount = description->attribute_count,
        .pVertexAttributeDescriptions = description->attributes
    };

    VkPipelineInputAssemblyStateCreateInfo input_assembly_state = {
        .sType = VK_STRUCTURE_TYPE_PIPELINE_INPUT_ASSEMBLY_STATE_CREATE_INFO,
        .topology = VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST
    };

    VkPipelineViewportStateCreateInfo viewport_state = {
        .sType = VK_STRUCTURE_TYPE_PIPELINE_VIEWPORT_STATE_CREATE_INFO,
        .viewportCount = 1,
        .scissorCount = 1
    };

    // No culling anywhere : 2D quads face the camera by construction, and imported 3D scenes may
    // have open or single-sided geometry (shadow casters included).
    VkPipelineRasterizationStateCreateInfo rasterization_state = {
        .sType = VK_STRUCTURE_TYPE_PIPELINE_RASTERIZATION_STATE_CREATE_INFO,
        .polygonMode = VK_POLYGON_MODE_FILL,
        .cullMode = VK_CULL_MODE_NONE,
        .frontFace = VK_FRONT_FACE_CLOCKWISE,
        .lineWidth = 1.0f
    };

    VkPipelineMultisampleStateCreateInfo multisample_state = {
        .sType = VK_STRUCTURE_TYPE_PIPELINE_MULTISAMPLE_STATE_CREATE_INFO,
        .rasterizationSamples = VK_SAMPLE_COUNT_1_BIT
    };

    VkPipelineDepthStencilStateCreateInfo depth_stencil_state = {
        .sType = VK_STRUCTURE_TYPE_PIPELINE_DEPTH_STENCIL_STATE_CREATE_INFO,
        .depthTestEnable = (description->depth_test || description->depth_read_only) ? VK_TRUE : VK_FALSE,
        .depthWriteEnable = description->depth_test ? VK_TRUE : VK_FALSE,
        .depthCompareOp = description->depth_read_only ? VK_COMPARE_OP_LESS_OR_EQUAL : VK_COMPARE_OP_LESS
    };

    VkPipelineColorBlendAttachmentState color_blend_attachment = {
        .blendEnable = description->alpha_blend ? VK_TRUE : VK_FALSE,
        .srcColorBlendFactor = VK_BLEND_FACTOR_SRC_ALPHA,
        .dstColorBlendFactor = VK_BLEND_FACTOR_ONE_MINUS_SRC_ALPHA,
        .colorBlendOp = VK_BLEND_OP_ADD,
        .srcAlphaBlendFactor = VK_BLEND_FACTOR_ONE,
        .dstAlphaBlendFactor = VK_BLEND_FACTOR_ONE_MINUS_SRC_ALPHA,
        .alphaBlendOp = VK_BLEND_OP_ADD,
        .colorWriteMask = VK_COLOR_COMPONENT_R_BIT | VK_COLOR_COMPONENT_G_BIT |
                           VK_COLOR_COMPONENT_B_BIT | VK_COLOR_COMPONENT_A_BIT
    };

    VkPipelineColorBlendStateCreateInfo color_blend_state = {
        .sType = VK_STRUCTURE_TYPE_PIPELINE_COLOR_BLEND_STATE_CREATE_INFO,
        .attachmentCount = description->color_attachment ? 1 : 0,
        .pAttachments = &color_blend_attachment
    };

    VkDynamicState dynamic_states[2] = { VK_DYNAMIC_STATE_VIEWPORT, VK_DYNAMIC_STATE_SCISSOR };

    VkPipelineDynamicStateCreateInfo dynamic_state = {
        .sType = VK_STRUCTURE_TYPE_PIPELINE_DYNAMIC_STATE_CREATE_INFO,
        .dynamicStateCount = 2,
        .pDynamicStates = dynamic_states
    };

    VkGraphicsPipelineCreateInfo pipeline_create_info = {
        .sType = VK_STRUCTURE_TYPE_GRAPHICS_PIPELINE_CREATE_INFO,
        .stageCount = description->fragment_shader ? 2 : 1,
        .pStages = shader_stages,
        .pVertexInputState = &vertex_input_state,
        .pInputAssemblyState = &input_assembly_state,
        .pViewportState = &viewport_state,
        .pRasterizationState = &rasterization_state,
        .pMultisampleState = &multisample_state,
        .pDepthStencilState = &depth_stencil_state,
        .pColorBlendState = &color_blend_state,
        .pDynamicState = &dynamic_state,
        .layout = description->layout,
        .renderPass = description->render_pass,
        .subpass = 0
    };

    VkResult result = vkCreateGraphicsPipelines(device, VK_NULL_HANDLE, 1, &pipeline_create_info, NULL, out_pipeline);

    vkDestroyShaderModule(device, vert_module, NULL);
    vkDestroyShaderModule(device, frag_module, NULL);

    if (result != VK_SUCCESS)
    {
        printf("Could not create the Vulkan '%s' pipeline.\n", description->vertex_shader);
        return 0;
    }

    return 1;
}

static int create_pipelines(void)
{
    VkVertexInputBindingDescription sprite_binding = {
        .binding = 0,
        .stride = sizeof(Vertex),
        .inputRate = VK_VERTEX_INPUT_RATE_VERTEX
    };

    VkVertexInputAttributeDescription sprite_attributes[3] = {
        { .location = 0, .binding = 0, .format = VK_FORMAT_R32G32_SFLOAT, .offset = offsetof(Vertex, x) },
        { .location = 1, .binding = 0, .format = VK_FORMAT_R32G32_SFLOAT, .offset = offsetof(Vertex, u) },
        { .location = 2, .binding = 0, .format = VK_FORMAT_R32G32B32A32_SFLOAT, .offset = offsetof(Vertex, r) }
    };

    PipelineDescription sprite = {
        .vertex_shader = "sprite.vert",
        .fragment_shader = "sprite.frag",
        .binding = &sprite_binding,
        .attributes = sprite_attributes,
        .attribute_count = 3,
        .layout = pipeline_layout,
        .render_pass = render_pass,
        .depth_test = 0,
        .alpha_blend = 1,
        .color_attachment = 1
    };

    if (!create_pipeline(&sprite, &pipeline))
    {
        return 0;
    }

    sprite.render_pass = render_target_render_pass;

    if (!create_pipeline(&sprite, &render_target_pipeline))
    {
        return 0;
    }

    if (!mesh_supported)
    {
        return 1;
    }

    VkVertexInputBindingDescription mesh_binding = {
        .binding = 0,
        .stride = sizeof(MeshVertex),
        .inputRate = VK_VERTEX_INPUT_RATE_VERTEX
    };

    VkVertexInputAttributeDescription mesh_attributes[3] = {
        { .location = 0, .binding = 0, .format = VK_FORMAT_R32G32B32_SFLOAT, .offset = offsetof(MeshVertex, x) },
        { .location = 1, .binding = 0, .format = VK_FORMAT_R32G32B32_SFLOAT, .offset = offsetof(MeshVertex, nx) },
        { .location = 2, .binding = 0, .format = VK_FORMAT_R32G32B32A32_SFLOAT, .offset = offsetof(MeshVertex, r) }
    };

    PipelineDescription scene = {
        .vertex_shader = "mesh.vert",
        .fragment_shader = "mesh.frag",
        .binding = &mesh_binding,
        .attributes = mesh_attributes,
        .attribute_count = 3,
        .layout = mesh_pipeline_layout,
        .render_pass = render_pass,
        .depth_test = 1,
        .alpha_blend = 0,
        .color_attachment = 1
    };

    PipelineDescription shadow = scene;
    shadow.vertex_shader = "mesh_shadow.vert";
    shadow.fragment_shader = NULL;
    shadow.render_pass = shadow_render_pass;
    shadow.color_attachment = 0;

    PipelineDescription sky = scene;
    sky.vertex_shader = "sky.vert";
    sky.fragment_shader = "sky.frag";
    sky.binding = NULL;
    sky.attributes = NULL;
    sky.attribute_count = 0;
    sky.depth_test = 0;
    sky.depth_read_only = 1;

    return create_pipeline(&scene, &mesh_pipeline) && create_pipeline(&shadow, &shadow_pipeline) &&
        create_pipeline(&sky, &sky_pipeline);
}

static int create_image_view(VkImage image, VkFormat format, VkImageAspectFlags aspect, VkImageView* out_view)
{
    VkImageViewCreateInfo view_create_info = {
        .sType = VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO,
        .image = image,
        .viewType = VK_IMAGE_VIEW_TYPE_2D,
        .format = format,
        .subresourceRange = { aspect, 0, 1, 0, 1 }
    };

    return vkCreateImageView(device, &view_create_info, NULL, out_view) == VK_SUCCESS;
}

static int create_framebuffers(void)
{
    swapchain_framebuffers = SDL_malloc(sizeof(VkFramebuffer) * swapchain_image_count);

    for (uint32_t i = 0; i < swapchain_image_count; i++)
    {
        VkImageView attachments[2] = { swapchain_image_views[i], depth_view };

        VkFramebufferCreateInfo framebuffer_create_info = {
            .sType = VK_STRUCTURE_TYPE_FRAMEBUFFER_CREATE_INFO,
            .renderPass = render_pass,
            .attachmentCount = 2,
            .pAttachments = attachments,
            .width = swapchain_extent.width,
            .height = swapchain_extent.height,
            .layers = 1
        };

        if (vkCreateFramebuffer(device, &framebuffer_create_info, NULL, &swapchain_framebuffers[i]) != VK_SUCCESS)
        {
            printf("Could not create a Vulkan framebuffer.\n");
            return 0;
        }
    }

    return 1;
}

static int create_command_pool(void)
{
    VkCommandPoolCreateInfo pool_create_info = {
        .sType = VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO,
        .flags = VK_COMMAND_POOL_CREATE_RESET_COMMAND_BUFFER_BIT,
        .queueFamilyIndex = (uint32_t)queue_family_indices.graphics_family
    };

    if (vkCreateCommandPool(device, &pool_create_info, NULL, &command_pool) != VK_SUCCESS)
    {
        printf("Could not create the Vulkan command pool.\n");
        return 0;
    }

    return 1;
}

static int create_command_buffers(void)
{
    VkCommandBufferAllocateInfo allocate_info = {
        .sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO,
        .commandPool = command_pool,
        .level = VK_COMMAND_BUFFER_LEVEL_PRIMARY,
        .commandBufferCount = MAX_FRAMES_IN_FLIGHT
    };

    if (vkAllocateCommandBuffers(device, &allocate_info, command_buffers) != VK_SUCCESS)
    {
        printf("Could not allocate the Vulkan command buffers.\n");
        return 0;
    }

    return 1;
}

static int create_sync_objects(void)
{
    VkSemaphoreCreateInfo semaphore_create_info = { .sType = VK_STRUCTURE_TYPE_SEMAPHORE_CREATE_INFO };
    VkFenceCreateInfo fence_create_info = {
        .sType = VK_STRUCTURE_TYPE_FENCE_CREATE_INFO,
        .flags = VK_FENCE_CREATE_SIGNALED_BIT
    };

    for (int i = 0; i < MAX_FRAMES_IN_FLIGHT; i++)
    {
        if (vkCreateSemaphore(device, &semaphore_create_info, NULL, &image_available_semaphores[i]) != VK_SUCCESS ||
            vkCreateSemaphore(device, &semaphore_create_info, NULL, &render_finished_semaphores[i]) != VK_SUCCESS ||
            vkCreateFence(device, &fence_create_info, NULL, &in_flight_fences[i]) != VK_SUCCESS)
        {
            printf("Could not create the Vulkan synchronization objects.\n");
            return 0;
        }
    }

    return 1;
}

// Host-visible, host-coherent and persistently mapped for its whole lifetime : CPU writes (memcpy)
// are visible to the GPU without any explicit flush. VMA picks the actual memory type.
static int create_mapped_buffer(VkDeviceSize size, VkBufferUsageFlags usage, MappedBuffer* out_buffer)
{
    VkBufferCreateInfo buffer_create_info = {
        .sType = VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO,
        .size = size,
        .usage = usage,
        .sharingMode = VK_SHARING_MODE_EXCLUSIVE
    };

    VmaAllocationCreateInfo allocation_create_info = {
        .flags = VMA_ALLOCATION_CREATE_HOST_ACCESS_SEQUENTIAL_WRITE_BIT | VMA_ALLOCATION_CREATE_MAPPED_BIT,
        .usage = VMA_MEMORY_USAGE_AUTO,
        .requiredFlags = VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT
    };

    VmaAllocationInfo allocation_info;

    if (vmaCreateBuffer(allocator, &buffer_create_info, &allocation_create_info,
        &out_buffer->buffer, &out_buffer->allocation, &allocation_info) != VK_SUCCESS)
    {
        return 0;
    }

    out_buffer->mapped = allocation_info.pMappedData;

    return 1;
}

static void destroy_mapped_buffer(MappedBuffer* buffer)
{
    vmaDestroyBuffer(allocator, buffer->buffer, buffer->allocation);
    SDL_zerop(buffer);
}

// A 2D, single-mip image in device-local memory (textures, render targets, depth buffers).
static int create_device_image(uint32_t width, uint32_t height, VkFormat format, VkImageUsageFlags usage, Texture* texture)
{
    VkImageCreateInfo image_create_info = {
        .sType = VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO,
        .imageType = VK_IMAGE_TYPE_2D,
        .format = format,
        .extent = { width, height, 1 },
        .mipLevels = 1,
        .arrayLayers = 1,
        .samples = VK_SAMPLE_COUNT_1_BIT,
        .tiling = VK_IMAGE_TILING_OPTIMAL,
        .usage = usage,
        .sharingMode = VK_SHARING_MODE_EXCLUSIVE,
        .initialLayout = VK_IMAGE_LAYOUT_UNDEFINED
    };

    VmaAllocationCreateInfo allocation_create_info = {
        .usage = VMA_MEMORY_USAGE_AUTO_PREFER_DEVICE
    };

    return vmaCreateImage(allocator, &image_create_info, &allocation_create_info,
        &texture->image, &texture->allocation, NULL) == VK_SUCCESS;
}

static int create_depth_resources(void)
{
    Texture depth = { 0 };

    if (!create_device_image(swapchain_extent.width, swapchain_extent.height, DEPTH_FORMAT,
        VK_IMAGE_USAGE_DEPTH_STENCIL_ATTACHMENT_BIT, &depth))
    {
        printf("Could not create the Vulkan depth buffer.\n");
        return 0;
    }

    depth_image = depth.image;
    depth_allocation = depth.allocation;

    if (!create_image_view(depth_image, DEPTH_FORMAT, VK_IMAGE_ASPECT_DEPTH_BIT, &depth_view))
    {
        printf("Could not create the Vulkan depth buffer view.\n");
        return 0;
    }

    return 1;
}

static void destroy_depth_resources(void)
{
    if (depth_view != VK_NULL_HANDLE)
    {
        vkDestroyImageView(device, depth_view, NULL);
        depth_view = VK_NULL_HANDLE;
    }

    if (depth_image != VK_NULL_HANDLE)
    {
        vmaDestroyImage(allocator, depth_image, depth_allocation);
        depth_image = VK_NULL_HANDLE;
        depth_allocation = NULL;
    }
}

// The combined image sampler descriptor every sampled image (texture, render target, shadow map) binds through.
static int allocate_texture_descriptor(Texture* texture)
{
    VkDescriptorSetAllocateInfo descriptor_allocate_info = {
        .sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO,
        .descriptorPool = descriptor_pool,
        .descriptorSetCount = 1,
        .pSetLayouts = &descriptor_set_layout
    };

    if (vkAllocateDescriptorSets(device, &descriptor_allocate_info, &texture->descriptor_set) != VK_SUCCESS)
    {
        return 0;
    }

    VkDescriptorImageInfo image_info = {
        .sampler = sampler,
        .imageView = texture->view,
        .imageLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL
    };

    VkWriteDescriptorSet write = {
        .sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET,
        .dstSet = texture->descriptor_set,
        .dstBinding = 0,
        .descriptorCount = 1,
        .descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER,
        .pImageInfo = &image_info
    };

    vkUpdateDescriptorSets(device, 1, &write, 0, NULL);

    return 1;
}

static int create_vertex_buffers(void)
{
    for (int i = 0; i < MAX_FRAMES_IN_FLIGHT; i++)
    {
        if (!create_mapped_buffer(VERTEX_BUFFER_SIZE, VK_BUFFER_USAGE_VERTEX_BUFFER_BIT, &vertex_buffers[i]))
        {
            printf("Could not create a Vulkan vertex buffer.\n");
            return 0;
        }
    }

    if (!create_mapped_buffer(VERTEX_BUFFER_SIZE, VK_BUFFER_USAGE_VERTEX_BUFFER_BIT, &render_target_vertex_buffer))
    {
        printf("Could not create the Vulkan render-target vertex buffer.\n");
        return 0;
    }

    return 1;
}

static VkCommandBuffer begin_one_time_commands(void)
{
    VkCommandBufferAllocateInfo allocate_info = {
        .sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO,
        .commandPool = command_pool,
        .level = VK_COMMAND_BUFFER_LEVEL_PRIMARY,
        .commandBufferCount = 1
    };

    VkCommandBuffer command_buffer;
    vkAllocateCommandBuffers(device, &allocate_info, &command_buffer);

    VkCommandBufferBeginInfo begin_info = {
        .sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO,
        .flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT
    };

    vkBeginCommandBuffer(command_buffer, &begin_info);

    return command_buffer;
}

static void end_one_time_commands(VkCommandBuffer command_buffer)
{
    vkEndCommandBuffer(command_buffer);

    VkSubmitInfo submit_info = {
        .sType = VK_STRUCTURE_TYPE_SUBMIT_INFO,
        .commandBufferCount = 1,
        .pCommandBuffers = &command_buffer
    };

    vkQueueSubmit(graphics_queue, 1, &submit_info, VK_NULL_HANDLE);
    vkDeviceWaitIdle(device);

    // Freed rather than left in the pool : a render-target pass allocates one of these every
    // frame (unlike the rare, one-off texture uploads this helper was first written for), so
    // leaving them allocated would leak a command buffer per frame.
    vkFreeCommandBuffers(device, command_pool, 1, &command_buffer);
}

static int upload_texture_pixels(Texture* texture, const void* pixels, uint32_t width, uint32_t height)
{
    VkDeviceSize image_size = (VkDeviceSize)width * height * 4;

    MappedBuffer staging_buffer;

    if (!create_mapped_buffer(image_size, VK_BUFFER_USAGE_TRANSFER_SRC_BIT, &staging_buffer))
    {
        printf("Could not create a staging buffer for texture upload.\n");
        return 0;
    }

    SDL_memcpy(staging_buffer.mapped, pixels, (size_t)image_size);

    if (!create_device_image(width, height, VK_FORMAT_R8G8B8A8_UNORM, VK_IMAGE_USAGE_TRANSFER_DST_BIT | VK_IMAGE_USAGE_SAMPLED_BIT, texture))
    {
        printf("Could not create a Vulkan image for texture upload.\n");
        destroy_mapped_buffer(&staging_buffer);
        return 0;
    }

    VkCommandBuffer command_buffer = begin_one_time_commands();

    VkImageMemoryBarrier to_transfer_barrier = {
        .sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER,
        .oldLayout = VK_IMAGE_LAYOUT_UNDEFINED,
        .newLayout = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
        .srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
        .dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
        .image = texture->image,
        .subresourceRange = { VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1 },
        .srcAccessMask = 0,
        .dstAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT
    };

    vkCmdPipelineBarrier(command_buffer, VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT,
        0, 0, NULL, 0, NULL, 1, &to_transfer_barrier);

    VkBufferImageCopy region = {
        .bufferOffset = 0,
        .bufferRowLength = 0,
        .bufferImageHeight = 0,
        .imageSubresource = { VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1 },
        .imageOffset = { 0, 0, 0 },
        .imageExtent = { width, height, 1 }
    };

    vkCmdCopyBufferToImage(command_buffer, staging_buffer.buffer, texture->image, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1, &region);

    VkImageMemoryBarrier to_shader_read_barrier = {
        .sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER,
        .oldLayout = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
        .newLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL,
        .srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
        .dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
        .image = texture->image,
        .subresourceRange = { VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1 },
        .srcAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT,
        .dstAccessMask = VK_ACCESS_SHADER_READ_BIT
    };

    vkCmdPipelineBarrier(command_buffer, VK_PIPELINE_STAGE_TRANSFER_BIT, VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT,
        0, 0, NULL, 0, NULL, 1, &to_shader_read_barrier);

    end_one_time_commands(command_buffer);

    destroy_mapped_buffer(&staging_buffer);

    VkImageViewCreateInfo view_create_info = {
        .sType = VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO,
        .image = texture->image,
        .viewType = VK_IMAGE_VIEW_TYPE_2D,
        .format = VK_FORMAT_R8G8B8A8_UNORM,
        .subresourceRange = { VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1 }
    };

    if (vkCreateImageView(device, &view_create_info, NULL, &texture->view) != VK_SUCCESS)
    {
        printf("Could not create a Vulkan texture image view.\n");
        return 0;
    }

    VkDescriptorSetAllocateInfo descriptor_allocate_info = {
        .sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO,
        .descriptorPool = descriptor_pool,
        .descriptorSetCount = 1,
        .pSetLayouts = &descriptor_set_layout
    };

    if (vkAllocateDescriptorSets(device, &descriptor_allocate_info, &texture->descriptor_set) != VK_SUCCESS)
    {
        printf("Could not allocate a Vulkan texture descriptor set.\n");
        return 0;
    }

    VkDescriptorImageInfo image_info = {
        .sampler = sampler,
        .imageView = texture->view,
        .imageLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL
    };

    VkWriteDescriptorSet write = {
        .sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET,
        .dstSet = texture->descriptor_set,
        .dstBinding = 0,
        .descriptorCount = 1,
        .descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER,
        .pImageInfo = &image_info
    };

    vkUpdateDescriptorSets(device, 1, &write, 0, NULL);

    texture->width = width;
    texture->height = height;
    texture->format = VK_FORMAT_R8G8B8A8_UNORM;
    texture->is_depth = 0;
    register_bindless_texture(texture);

    return 1;
}

static int create_white_texture(void)
{
    uint8_t pixel[4] = { 255, 255, 255, 255 };
    return upload_texture_pixels(&built_in_white_texture, pixel, 1, 1);
}

static void cleanup_swapchain(void)
{
    destroy_depth_resources();

    for (uint32_t i = 0; i < swapchain_image_count; i++)
    {
        vkDestroyFramebuffer(device, swapchain_framebuffers[i], NULL);
    }
    SDL_free(swapchain_framebuffers);
    swapchain_framebuffers = NULL;

    for (uint32_t i = 0; i < swapchain_image_count; i++)
    {
        vkDestroyImageView(device, swapchain_image_views[i], NULL);
    }
    SDL_free(swapchain_image_views);
    swapchain_image_views = NULL;

    SDL_free(swapchain_images);
    swapchain_images = NULL;

    vkDestroySwapchainKHR(device, swapchain, NULL);
    swapchain = VK_NULL_HANDLE;
}

static int recreate_swapchain(void)
{
    int width = 0;
    int height = 0;
    SDL_GetWindowSizeInPixels(window, &width, &height);

    while (width == 0 || height == 0)
    {
        SDL_GetWindowSizeInPixels(window, &width, &height);
        SDL_PumpEvents();
        SDL_Delay(10);
    }

    vkDeviceWaitIdle(device);

    cleanup_swapchain();

    if (!create_swapchain() || !create_image_views() || !create_depth_resources() || !create_framebuffers())
    {
        return 0;
    }

    DEBUG_LOG("[vulkan] swapchain recreated: %ux%u\n", swapchain_extent.width, swapchain_extent.height);

    return 1;
}

static VkDeviceAddress buffer_address(VkBuffer buffer)
{
    VkBufferDeviceAddressInfo info = { .sType = VK_STRUCTURE_TYPE_BUFFER_DEVICE_ADDRESS_INFO, .buffer = buffer };
    return vkGetBufferDeviceAddress(device, &info);
}

static int create_acceleration_buffer(VkDeviceSize size, VkBufferUsageFlags usage, VkBuffer* out_buffer, VmaAllocation* out_allocation)
{
    VkBufferCreateInfo buffer_create_info = {
        .sType = VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO,
        .size = size,
        .usage = usage | VK_BUFFER_USAGE_SHADER_DEVICE_ADDRESS_BIT,
        .sharingMode = VK_SHARING_MODE_EXCLUSIVE
    };

    VmaAllocationCreateInfo allocation_create_info = { .usage = VMA_MEMORY_USAGE_AUTO_PREFER_DEVICE };

    return vmaCreateBuffer(allocator, &buffer_create_info, &allocation_create_info, out_buffer, out_allocation, NULL) == VK_SUCCESS;
}

static VkDeviceAddress align_address(VkDeviceAddress address)
{
    return (address + rt_scratch_alignment - 1) & ~(VkDeviceAddress)(rt_scratch_alignment - 1);
}

static int create_ray_tracing_resources(void)
{
    VkPhysicalDeviceAccelerationStructurePropertiesKHR acceleration_properties = { .sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_ACCELERATION_STRUCTURE_PROPERTIES_KHR };
    VkPhysicalDeviceProperties2 properties = { .sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_PROPERTIES_2, .pNext = &acceleration_properties };
    vkGetPhysicalDeviceProperties2(physical_device, &properties);
    rt_scratch_alignment = SDL_max(acceleration_properties.minAccelerationStructureScratchOffsetAlignment, 16u);

    VkAccelerationStructureGeometryKHR geometry = {
        .sType = VK_STRUCTURE_TYPE_ACCELERATION_STRUCTURE_GEOMETRY_KHR,
        .geometryType = VK_GEOMETRY_TYPE_INSTANCES_KHR,
        .geometry.instances = { .sType = VK_STRUCTURE_TYPE_ACCELERATION_STRUCTURE_GEOMETRY_INSTANCES_DATA_KHR }
    };

    VkAccelerationStructureBuildGeometryInfoKHR build_info = {
        .sType = VK_STRUCTURE_TYPE_ACCELERATION_STRUCTURE_BUILD_GEOMETRY_INFO_KHR,
        .type = VK_ACCELERATION_STRUCTURE_TYPE_TOP_LEVEL_KHR,
        .flags = VK_BUILD_ACCELERATION_STRUCTURE_PREFER_FAST_BUILD_BIT_KHR,
        .mode = VK_BUILD_ACCELERATION_STRUCTURE_MODE_BUILD_KHR,
        .geometryCount = 1,
        .pGeometries = &geometry
    };

    uint32_t max_instances = MAX_RAY_INSTANCES;
    VkAccelerationStructureBuildSizesInfoKHR sizes = { .sType = VK_STRUCTURE_TYPE_ACCELERATION_STRUCTURE_BUILD_SIZES_INFO_KHR };
    vkGetAccelerationStructureBuildSizesKHR(device, VK_ACCELERATION_STRUCTURE_BUILD_TYPE_DEVICE_KHR, &build_info, &max_instances, &sizes);

    if (!create_acceleration_buffer(sizes.buildScratchSize + rt_scratch_alignment, VK_BUFFER_USAGE_STORAGE_BUFFER_BIT, &tlas_scratch, &tlas_scratch_allocation))
    {
        return 0;
    }

    tlas_scratch_address = align_address(buffer_address(tlas_scratch));

    VkWriteDescriptorSetAccelerationStructureKHR descriptor = {
        .sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET_ACCELERATION_STRUCTURE_KHR,
        .accelerationStructureCount = MAX_FRAMES_IN_FLIGHT,
        .pAccelerationStructures = tlas
    };

    for (int i = 0; i < MAX_FRAMES_IN_FLIGHT; i++)
    {
        if (!create_acceleration_buffer(sizes.accelerationStructureSize, VK_BUFFER_USAGE_ACCELERATION_STRUCTURE_STORAGE_BIT_KHR, &tlas_buffers[i], &tlas_allocations[i]))
        {
            return 0;
        }

        VkAccelerationStructureCreateInfoKHR create_info = {
            .sType = VK_STRUCTURE_TYPE_ACCELERATION_STRUCTURE_CREATE_INFO_KHR,
            .buffer = tlas_buffers[i],
            .size = sizes.accelerationStructureSize,
            .type = VK_ACCELERATION_STRUCTURE_TYPE_TOP_LEVEL_KHR
        };

        if (vkCreateAccelerationStructureKHR(device, &create_info, NULL, &tlas[i]) != VK_SUCCESS)
        {
            return 0;
        }

        if (!create_mapped_buffer(sizeof(VkAccelerationStructureInstanceKHR) * MAX_RAY_INSTANCES,
            VK_BUFFER_USAGE_ACCELERATION_STRUCTURE_BUILD_INPUT_READ_ONLY_BIT_KHR | VK_BUFFER_USAGE_SHADER_DEVICE_ADDRESS_BIT, &tlas_instances[i]))
        {
            return 0;
        }
    }

    VkWriteDescriptorSet write = {
        .sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET,
        .pNext = &descriptor,
        .dstSet = bindless_set,
        .dstBinding = 2 + GPU_SAMPLER_COUNT,
        .descriptorCount = MAX_FRAMES_IN_FLIGHT,
        .descriptorType = VK_DESCRIPTOR_TYPE_ACCELERATION_STRUCTURE_KHR
    };

    vkUpdateDescriptorSets(device, 1, &write, 0, NULL);

    return 1;
}

int vulkan_create_renderer(Window* target_window)
{
    window = (SDL_Window*)target_window;
    current_frame = 0;

    if (!load_global_functions())
    {
        return 0;
    }

    if (!create_instance())
    {
        return 0;
    }

    if (!SDL_Vulkan_CreateSurface(window, instance, NULL, &surface))
    {
        printf("Could not create the Vulkan surface: %s\n", SDL_GetError());
        return 0;
    }

    if (!pick_physical_device())
    {
        return 0;
    }

    if (!create_logical_device())
    {
        return 0;
    }

    if (!create_allocator())
    {
        return 0;
    }

    create_timestamp_queries();

    if (!create_swapchain())
    {
        return 0;
    }

    if (!create_image_views())
    {
        return 0;
    }

    if (!create_render_pass())
    {
        return 0;
    }

    if (!create_render_target_render_pass())
    {
        return 0;
    }

    mesh_supported = 1; // until proven otherwise by create_shadow_render_pass()/create_pipeline_layouts()

    if (!create_shadow_render_pass())
    {
        return 0;
    }

    if (!create_sampler())
    {
        return 0;
    }

    if (!create_descriptor_set_layout())
    {
        return 0;
    }

    if (!create_descriptor_pool())
    {
        return 0;
    }

    if (!create_bindless())
    {
        return 0;
    }

    if (!create_pipeline_layouts() || !create_pipelines())
    {
        return 0;
    }

    if (!create_depth_resources())
    {
        return 0;
    }

    if (!create_framebuffers())
    {
        return 0;
    }

    if (!create_command_pool())
    {
        return 0;
    }

    if (!create_command_buffers())
    {
        return 0;
    }

    if (!create_vertex_buffers())
    {
        return 0;
    }

    if (!create_sync_objects())
    {
        return 0;
    }

    if (!create_white_texture())
    {
        return 0;
    }

    if (gpu_available)
    {
        for (int i = 0; i < MAX_FRAMES_IN_FLIGHT; i++)
        {
            if (!create_mapped_buffer(GPU_FRAME_BUFFER_SIZE, VK_BUFFER_USAGE_STORAGE_BUFFER_BIT, &gpu_frame_buffers[i]))
            {
                printf("Could not create a Vulkan frame buffer.\n");
                return 0;
            }

            gpu_frame_buffer_indices[i] = register_bindless_buffer(gpu_frame_buffers[i].buffer);
        }
    }

    if (rt_available && !create_ray_tracing_resources())
    {
        printf("Could not create the ray tracing resources : ray tracing is unavailable.\n");
        rt_available = 0;
    }

    DEBUG_LOG("[vulkan] renderer ready (swapchain %ux%u)\n", swapchain_extent.width, swapchain_extent.height);

    return 1;
}

Texture* vulkan_create_texture(const char* file_path, int* out_width, int* out_height)
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

    Texture* texture = SDL_malloc(sizeof(Texture));

    if (!upload_texture_pixels(texture, converted->pixels, (uint32_t)converted->w, (uint32_t)converted->h))
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

    DEBUG_LOG("[vulkan] texture loaded: \"%s\" (%dx%d)\n", file_path, out_width ? *out_width : -1, out_height ? *out_height : -1);

    return texture;
}

void vulkan_destroy_texture(Texture* texture)
{
    if (!texture || texture == &built_in_white_texture)
    {
        return;
    }

    release_bindless_index(texture->bindless_index);
    vkFreeDescriptorSets(device, descriptor_pool, 1, &texture->descriptor_set);
    vkDestroyImageView(device, texture->view, NULL);
    vmaDestroyImage(allocator, texture->image, texture->allocation);
    SDL_free(texture);

    DEBUG_LOG("[vulkan] texture destroyed\n");
}

void vulkan_texture_get_size(Texture* texture, int* out_width, int* out_height)
{
    *out_width = (int)texture->width;
    *out_height = (int)texture->height;
}

Texture* vulkan_white_texture(void)
{
    return &built_in_white_texture;
}

Texture* vulkan_create_texture_from_pixels(const void* rgba_pixels, int width, int height)
{
    Texture* texture = SDL_malloc(sizeof(Texture));

    if (!upload_texture_pixels(texture, rgba_pixels, (uint32_t)width, (uint32_t)height))
    {
        SDL_free(texture);
        return NULL;
    }

    DEBUG_LOG("[vulkan] texture created from pixels (%dx%d)\n", width, height);

    return texture;
}

RenderTarget* vulkan_create_render_target(int width, int height)
{
    RenderTarget* target = SDL_calloc(1, sizeof(RenderTarget));
    target->width = width;
    target->height = height;

    if (!create_device_image((uint32_t)width, (uint32_t)height, VK_FORMAT_R8G8B8A8_UNORM,
        VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT | VK_IMAGE_USAGE_SAMPLED_BIT, &target->texture))
    {
        printf("Could not create a Vulkan render target image.\n");
        SDL_free(target);
        return NULL;
    }

    VkImageViewCreateInfo view_create_info = {
        .sType = VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO,
        .image = target->texture.image,
        .viewType = VK_IMAGE_VIEW_TYPE_2D,
        .format = VK_FORMAT_R8G8B8A8_UNORM,
        .subresourceRange = { VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1 }
    };

    if (vkCreateImageView(device, &view_create_info, NULL, &target->texture.view) != VK_SUCCESS)
    {
        printf("Could not create a Vulkan render target image view.\n");
        vmaDestroyImage(allocator, target->texture.image, target->texture.allocation);
        SDL_free(target);
        return NULL;
    }

    VkFramebufferCreateInfo framebuffer_create_info = {
        .sType = VK_STRUCTURE_TYPE_FRAMEBUFFER_CREATE_INFO,
        .renderPass = render_target_render_pass,
        .attachmentCount = 1,
        .pAttachments = &target->texture.view,
        .width = (uint32_t)width,
        .height = (uint32_t)height,
        .layers = 1
    };

    if (vkCreateFramebuffer(device, &framebuffer_create_info, NULL, &target->framebuffer) != VK_SUCCESS)
    {
        printf("Could not create a Vulkan render target framebuffer.\n");
        vkDestroyImageView(device, target->texture.view, NULL);
        vmaDestroyImage(allocator, target->texture.image, target->texture.allocation);
        SDL_free(target);
        return NULL;
    }

    VkDescriptorSetAllocateInfo descriptor_allocate_info = {
        .sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO,
        .descriptorPool = descriptor_pool,
        .descriptorSetCount = 1,
        .pSetLayouts = &descriptor_set_layout
    };

    if (vkAllocateDescriptorSets(device, &descriptor_allocate_info, &target->texture.descriptor_set) != VK_SUCCESS)
    {
        printf("Could not allocate a Vulkan render target descriptor set.\n");
        vkDestroyFramebuffer(device, target->framebuffer, NULL);
        vkDestroyImageView(device, target->texture.view, NULL);
        vmaDestroyImage(allocator, target->texture.image, target->texture.allocation);
        SDL_free(target);
        return NULL;
    }

    VkDescriptorImageInfo image_info = {
        .sampler = sampler,
        .imageView = target->texture.view,
        .imageLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL
    };

    VkWriteDescriptorSet write = {
        .sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET,
        .dstSet = target->texture.descriptor_set,
        .dstBinding = 0,
        .descriptorCount = 1,
        .descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER,
        .pImageInfo = &image_info
    };

    vkUpdateDescriptorSets(device, 1, &write, 0, NULL);

    register_bindless_texture(&target->texture);

    DEBUG_LOG("[vulkan] render target created: %dx%d\n", width, height);

    return target;
}

void vulkan_destroy_render_target(RenderTarget* target)
{
    if (!target)
    {
        return;
    }

    release_bindless_index(target->texture.bindless_index);
    vkFreeDescriptorSets(device, descriptor_pool, 1, &target->texture.descriptor_set);
    vkDestroyFramebuffer(device, target->framebuffer, NULL);
    vkDestroyImageView(device, target->texture.view, NULL);
    vmaDestroyImage(allocator, target->texture.image, target->texture.allocation);
    SDL_free(target);

    DEBUG_LOG("[vulkan] render target destroyed\n");
}

Texture* vulkan_render_target_texture(RenderTarget* target)
{
    return &target->texture;
}

int vulkan_begin_render_target_pass(RenderTarget* target)
{
    if (!target)
    {
        return 0;
    }

    current_command_buffer = begin_one_time_commands();
    rendering_to_target = 1;
    render_target_vertex_write_offset = 0;

    VkClearValue clear_color = { .color.float32 = { 0.0f, 0.0f, 0.0f, 1.0f } };

    VkRenderPassBeginInfo render_pass_begin_info = {
        .sType = VK_STRUCTURE_TYPE_RENDER_PASS_BEGIN_INFO,
        .renderPass = render_target_render_pass,
        .framebuffer = target->framebuffer,
        .renderArea.offset = { 0, 0 },
        .renderArea.extent = { (uint32_t)target->width, (uint32_t)target->height },
        .clearValueCount = 1,
        .pClearValues = &clear_color
    };

    vkCmdBeginRenderPass(current_command_buffer, &render_pass_begin_info, VK_SUBPASS_CONTENTS_INLINE);

    vkCmdBindPipeline(current_command_buffer, VK_PIPELINE_BIND_POINT_GRAPHICS, render_target_pipeline);

    // Same negative-height-viewport convention as the window pass (see begin_frame), so a
    // render target's pixel content is oriented identically on both backends and needs no
    // special-casing when it's later sampled with a plain top-left-origin UV quad.
    VkViewport viewport = {
        .x = 0.0f,
        .y = (float)target->height,
        .width = (float)target->width,
        .height = -(float)target->height,
        .minDepth = 0.0f,
        .maxDepth = 1.0f
    };
    vkCmdSetViewport(current_command_buffer, 0, 1, &viewport);

    VkRect2D scissor = { .offset = { 0, 0 }, .extent = { (uint32_t)target->width, (uint32_t)target->height } };
    vkCmdSetScissor(current_command_buffer, 0, 1, &scissor);

    ScreenTransform transform = screen_transform_for_size((float)target->width, (float)target->height);
    vkCmdPushConstants(current_command_buffer, pipeline_layout, VK_SHADER_STAGE_VERTEX_BIT, 0, sizeof(transform), &transform);

    return 1;
}

void vulkan_end_render_target_pass(void)
{
    vkCmdEndRenderPass(current_command_buffer);
    end_one_time_commands(current_command_buffer);

    rendering_to_target = 0;
}

// Vulkan only guarantees the swapchain gets recreated once vkAcquireNextImageKHR/vkQueuePresentKHR
// report it out of date or suboptimal, which can lag one or more frames behind an actual window
// resize (especially a live, in-progress drag). In the meantime, game code already sees the new
// window size (e.g. for tilemap culling), while the renderer would still be drawing into the old,
// stale-sized swapchain image : a visible mismatch. Checking the real window size proactively here,
// every frame, keeps the swapchain immediately in sync (mirrors resize_if_needed() in the DX12
// backend).
static int resize_if_needed(void)
{
    int width = 0;
    int height = 0;
    SDL_GetWindowSizeInPixels(window, &width, &height);

    if ((uint32_t)width == swapchain_extent.width && (uint32_t)height == swapchain_extent.height)
    {
        return 1;
    }

    return recreate_swapchain();
}

int vulkan_begin_frame(void)
{
    if (!resize_if_needed())
    {
        return 0;
    }

    vkWaitForFences(device, 1, &in_flight_fences[current_frame], VK_TRUE, UINT64_MAX);

    VkResult result = vkAcquireNextImageKHR(device, swapchain, UINT64_MAX,
        image_available_semaphores[current_frame], VK_NULL_HANDLE, &current_image_index);

    if (result == VK_ERROR_OUT_OF_DATE_KHR)
    {
        recreate_swapchain();
        return 0;
    }

    if (result != VK_SUCCESS && result != VK_SUBOPTIMAL_KHR)
    {
        printf("Could not acquire the next Vulkan swapchain image.\n");
        return 0;
    }

    vkResetFences(device, 1, &in_flight_fences[current_frame]);

    current_command_buffer = command_buffers[current_frame];
    vkResetCommandBuffer(current_command_buffer, 0);

    VkCommandBufferBeginInfo begin_info = { .sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO };
    vkBeginCommandBuffer(current_command_buffer, &begin_info);

    if (timestamp_pool)
    {
        // This slot's last frame is done (its fence was waited on above) : its two timestamps are there.
        uint32_t first = (uint32_t)current_frame * 2;

        if (timestamp_pending[current_frame])
        {
            uint64_t times[2];

            if (vkGetQueryPoolResults(device, timestamp_pool, first, 2, sizeof(times), times, sizeof(uint64_t), VK_QUERY_RESULT_64_BIT) == VK_SUCCESS)
            {
                uint64_t ticks = (times[1] - times[0]) & timestamp_mask;
                last_gpu_frame_ms = (float)((double)ticks * (double)timestamp_period_ns * 1e-6);
            }

            timestamp_pending[current_frame] = 0;
        }

        vkCmdResetQueryPool(current_command_buffer, timestamp_pool, first, 2);
        vkCmdWriteTimestamp(current_command_buffer, VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT, timestamp_pool, first);
    }

    vertex_write_offset = 0;
    vertex_buffer_full_reported = 0;
    gpu_frame_offset = 0;
    window_pass_active = 0;
    frame_recording = 1;

    return 1;
}

// Starts the window's render pass, the first time something is drawn into it this frame.
static void ensure_window_pass(void)
{
    if (window_pass_active || !frame_recording || rendering_to_target)
    {
        return;
    }

    window_pass_active = 1;

    VkClearValue clear_values[2] = {
        { .color.float32 = { 0.0f, 0.0f, 0.0f, 1.0f } },
        { .depthStencil = { 1.0f, 0 } }
    };

    VkRenderPassBeginInfo render_pass_begin_info = {
        .sType = VK_STRUCTURE_TYPE_RENDER_PASS_BEGIN_INFO,
        .renderPass = render_pass,
        .framebuffer = swapchain_framebuffers[current_image_index],
        .renderArea.offset = { 0, 0 },
        .renderArea.extent = swapchain_extent,
        .clearValueCount = 2,
        .pClearValues = clear_values
    };

    vkCmdBeginRenderPass(current_command_buffer, &render_pass_begin_info, VK_SUBPASS_CONTENTS_INLINE);

    vkCmdBindPipeline(current_command_buffer, VK_PIPELINE_BIND_POINT_GRAPHICS, pipeline);

    // Negative-height viewport: flips Vulkan's default (clip +Y down) to match the shared,
    // backend-agnostic ScreenTransform in ortho.h (see its comment for the full rationale).
    VkViewport viewport = {
        .x = 0.0f,
        .y = (float)swapchain_extent.height,
        .width = (float)swapchain_extent.width,
        .height = -(float)swapchain_extent.height,
        .minDepth = 0.0f,
        .maxDepth = 1.0f
    };
    vkCmdSetViewport(current_command_buffer, 0, 1, &viewport);

    VkRect2D scissor = { .offset = { 0, 0 }, .extent = swapchain_extent };
    vkCmdSetScissor(current_command_buffer, 0, 1, &scissor);

    ScreenTransform transform = screen_transform_for_size((float)swapchain_extent.width, (float)swapchain_extent.height);
    vkCmdPushConstants(current_command_buffer, pipeline_layout, VK_SHADER_STAGE_VERTEX_BIT, 0, sizeof(transform), &transform);
}

void vulkan_draw_vertices(const Vertex* vertices, int vertex_count, Texture* texture)
{
    if (vertex_count <= 0 || !texture)
    {
        return;
    }

    ensure_window_pass();

    MappedBuffer* vertex_buffer = rendering_to_target ? &render_target_vertex_buffer : &vertex_buffers[current_frame];
    VkBuffer buffer = vertex_buffer->buffer;
    void* mapped = vertex_buffer->mapped;
    uint32_t* write_offset = rendering_to_target ? &render_target_vertex_write_offset : &vertex_write_offset;

    if (*write_offset + (uint32_t)vertex_count > VERTEX_BUFFER_SIZE / sizeof(Vertex))
    {
        // Once per frame : a full buffer usually drops many draws in a row.
        if (!vertex_buffer_full_reported)
        {
            printf("Vulkan vertex buffer is full this frame : draw calls are dropped.\n");
            vertex_buffer_full_reported = 1;
        }

        return;
    }

    Vertex* destination = (Vertex*)mapped + *write_offset;
    SDL_memcpy(destination, vertices, sizeof(Vertex) * (size_t)vertex_count);

    VkDeviceSize offset = (VkDeviceSize)(*write_offset) * sizeof(Vertex);
    vkCmdBindVertexBuffers(current_command_buffer, 0, 1, &buffer, &offset);

    vkCmdBindDescriptorSets(current_command_buffer, VK_PIPELINE_BIND_POINT_GRAPHICS, pipeline_layout,
        0, 1, &texture->descriptor_set, 0, NULL);

    vkCmdDraw(current_command_buffer, (uint32_t)vertex_count, 1, 0, 0);

    *write_offset += (uint32_t)vertex_count;
}

void vulkan_set_scissor(int x, int y, int width, int height)
{
    ensure_window_pass();

    VkRect2D scissor = { .offset = { x, y }, .extent = { (uint32_t)width, (uint32_t)height } };
    vkCmdSetScissor(current_command_buffer, 0, 1, &scissor);
}

void vulkan_end_frame(void)
{
    ensure_window_pass(); // nothing drawn this frame : the pass still clears and presents the image
    vkCmdEndRenderPass(current_command_buffer);

    if (timestamp_pool)
    {
        vkCmdWriteTimestamp(current_command_buffer, VK_PIPELINE_STAGE_BOTTOM_OF_PIPE_BIT, timestamp_pool, (uint32_t)current_frame * 2 + 1);
        timestamp_pending[current_frame] = 1;
    }

    vkEndCommandBuffer(current_command_buffer);

    window_pass_active = 0;
    frame_recording = 0;

    VkPipelineStageFlags wait_stage = VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT;

    VkSubmitInfo submit_info = {
        .sType = VK_STRUCTURE_TYPE_SUBMIT_INFO,
        .waitSemaphoreCount = 1,
        .pWaitSemaphores = &image_available_semaphores[current_frame],
        .pWaitDstStageMask = &wait_stage,
        .commandBufferCount = 1,
        .pCommandBuffers = &current_command_buffer,
        .signalSemaphoreCount = 1,
        .pSignalSemaphores = &render_finished_semaphores[current_frame]
    };

    if (vkQueueSubmit(graphics_queue, 1, &submit_info, in_flight_fences[current_frame]) != VK_SUCCESS)
    {
        printf("Could not submit the Vulkan command buffer.\n");
        return;
    }

    VkPresentInfoKHR present_info = {
        .sType = VK_STRUCTURE_TYPE_PRESENT_INFO_KHR,
        .waitSemaphoreCount = 1,
        .pWaitSemaphores = &render_finished_semaphores[current_frame],
        .swapchainCount = 1,
        .pSwapchains = &swapchain,
        .pImageIndices = &current_image_index
    };

    VkResult result = vkQueuePresentKHR(present_queue, &present_info);

    if (result == VK_ERROR_OUT_OF_DATE_KHR || result == VK_SUBOPTIMAL_KHR)
    {
        recreate_swapchain();
    }
    else if (result != VK_SUCCESS)
    {
        printf("Could not present the Vulkan swapchain image.\n");
    }

    current_frame = (current_frame + 1) % MAX_FRAMES_IN_FLIGHT;
}

void vulkan_wait_renderer_idle(void)
{
    if (device != VK_NULL_HANDLE)
    {
        vkDeviceWaitIdle(device);
    }
}

void vulkan_destroy_renderer(void)
{
    // destroy_texture() refuses to free the built-in white texture (see its guard), so it's
    // released directly here instead.
    vkFreeDescriptorSets(device, descriptor_pool, 1, &built_in_white_texture.descriptor_set);
    vkDestroyImageView(device, built_in_white_texture.view, NULL);
    vmaDestroyImage(allocator, built_in_white_texture.image, built_in_white_texture.allocation);

    for (int i = 0; i < MAX_FRAMES_IN_FLIGHT; i++)
    {
        vkDestroySemaphore(device, render_finished_semaphores[i], NULL);
        vkDestroySemaphore(device, image_available_semaphores[i], NULL);
        vkDestroyFence(device, in_flight_fences[i], NULL);

        destroy_mapped_buffer(&vertex_buffers[i]);
    }

    destroy_mapped_buffer(&render_target_vertex_buffer);

    if (timestamp_pool)
    {
        vkDestroyQueryPool(device, timestamp_pool, NULL);
        timestamp_pool = VK_NULL_HANDLE;
    }

    vkDestroyCommandPool(device, command_pool, NULL);

    cleanup_swapchain();

    if (rt_available)
    {
        for (int i = 0; i < MAX_FRAMES_IN_FLIGHT; i++)
        {
            vkDestroyAccelerationStructureKHR(device, tlas[i], NULL);
            vmaDestroyBuffer(allocator, tlas_buffers[i], tlas_allocations[i]);
            destroy_mapped_buffer(&tlas_instances[i]);
        }

        vmaDestroyBuffer(allocator, tlas_scratch, tlas_scratch_allocation);
    }

    if (gpu_available)
    {
        for (int i = 0; i < MAX_FRAMES_IN_FLIGHT; i++)
        {
            destroy_mapped_buffer(&gpu_frame_buffers[i]);
        }

        vkDestroyPipelineLayout(device, gpu_pipeline_layout, NULL);
        vkDestroyDescriptorPool(device, bindless_pool, NULL);
        vkDestroyDescriptorSetLayout(device, bindless_layout, NULL);

        for (int i = 0; i < GPU_SAMPLER_COUNT; i++)
        {
            vkDestroySampler(device, gpu_samplers[i], NULL);
        }
    }

    vkDestroyPipeline(device, pipeline, NULL);
    vkDestroyPipeline(device, render_target_pipeline, NULL);
    vkDestroyPipeline(device, mesh_pipeline, NULL);
    vkDestroyPipeline(device, shadow_pipeline, NULL);
    vkDestroyPipeline(device, sky_pipeline, NULL);
    vkDestroyPipelineLayout(device, mesh_pipeline_layout, NULL);
    vkDestroyRenderPass(device, shadow_render_pass, NULL);
    vkDestroyPipelineLayout(device, pipeline_layout, NULL);
    vkDestroyDescriptorPool(device, descriptor_pool, NULL);
    vkDestroyDescriptorSetLayout(device, descriptor_set_layout, NULL);
    vkDestroySampler(device, sampler, NULL);
    vkDestroyRenderPass(device, render_pass, NULL);
    vkDestroyRenderPass(device, render_target_render_pass, NULL);

    vmaDestroyAllocator(allocator);
    vkDestroyDevice(device, NULL);

    SDL_Vulkan_DestroySurface(instance, surface, NULL);

    vkDestroyInstance(instance, NULL);
}

// --- 3D ---------------------------------------------------------------------------------------

static int create_device_buffer(const void* data, VkDeviceSize size, VkBufferUsageFlags usage, DeviceBuffer* out_buffer)
{
    MappedBuffer staging;

    if (!create_mapped_buffer(size, VK_BUFFER_USAGE_TRANSFER_SRC_BIT, &staging))
    {
        return 0;
    }

    SDL_memcpy(staging.mapped, data, (size_t)size);

    VkBufferCreateInfo buffer_create_info = {
        .sType = VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO,
        .size = size,
        .usage = usage | VK_BUFFER_USAGE_TRANSFER_DST_BIT,
        .sharingMode = VK_SHARING_MODE_EXCLUSIVE
    };

    VmaAllocationCreateInfo allocation_create_info = { .usage = VMA_MEMORY_USAGE_AUTO_PREFER_DEVICE };

    if (vmaCreateBuffer(allocator, &buffer_create_info, &allocation_create_info, &out_buffer->buffer, &out_buffer->allocation, NULL) != VK_SUCCESS)
    {
        destroy_mapped_buffer(&staging);
        return 0;
    }

    VkCommandBuffer command_buffer = begin_one_time_commands();
    VkBufferCopy region = { 0, 0, size };
    vkCmdCopyBuffer(command_buffer, staging.buffer, out_buffer->buffer, 1, &region);
    end_one_time_commands(command_buffer);

    destroy_mapped_buffer(&staging);

    out_buffer->bindless_index = (usage & VK_BUFFER_USAGE_STORAGE_BUFFER_BIT) ? register_bindless_buffer(out_buffer->buffer) : NO_BINDLESS_INDEX;

    return 1;
}

static void destroy_device_buffer(DeviceBuffer* buffer)
{
    release_bindless_index(buffer->bindless_index);
    vmaDestroyBuffer(allocator, buffer->buffer, buffer->allocation);
    SDL_zerop(buffer);
}

Mesh* vulkan_create_mesh(const MeshVertex* vertices, int vertex_count, const uint32_t* indices, int index_count)
{
    if (!mesh_supported)
    {
        return NULL;
    }

    Mesh* mesh = SDL_calloc(1, sizeof(Mesh));
    VkDeviceSize vertices_size = sizeof(MeshVertex) * (VkDeviceSize)vertex_count;
    VkDeviceSize indices_size = sizeof(uint32_t) * (VkDeviceSize)index_count;

    VkBufferUsageFlags ray_usage = rt_available ? VK_BUFFER_USAGE_ACCELERATION_STRUCTURE_BUILD_INPUT_READ_ONLY_BIT_KHR | VK_BUFFER_USAGE_SHADER_DEVICE_ADDRESS_BIT : 0;

    mesh->vertex_count = (uint32_t)vertex_count;

    if (!create_device_buffer(vertices, vertices_size, VK_BUFFER_USAGE_VERTEX_BUFFER_BIT | VK_BUFFER_USAGE_STORAGE_BUFFER_BIT | ray_usage, &mesh->vertices))
    {
        printf("Could not create a Vulkan mesh vertex buffer.\n");
        SDL_free(mesh);
        return NULL;
    }

    if (!create_device_buffer(indices, indices_size, VK_BUFFER_USAGE_INDEX_BUFFER_BIT | VK_BUFFER_USAGE_STORAGE_BUFFER_BIT | ray_usage, &mesh->indices))
    {
        printf("Could not create a Vulkan mesh index buffer.\n");
        destroy_device_buffer(&mesh->vertices);
        SDL_free(mesh);
        return NULL;
    }

    mesh->index_count = (uint32_t)index_count;

    DEBUG_LOG("[vulkan] mesh created: %d vertices, %d indices\n", vertex_count, index_count);

    return mesh;
}

void vulkan_destroy_mesh(Mesh* mesh)
{
    for (int i = 0; i < mesh->blas_count; i++)
    {
        vkDestroyAccelerationStructureKHR(device, mesh->blas[i].handle, NULL);
        vmaDestroyBuffer(allocator, mesh->blas[i].buffer, mesh->blas[i].allocation);
    }

    SDL_free(mesh->blas);

    destroy_device_buffer(&mesh->vertices);
    destroy_device_buffer(&mesh->indices);
    SDL_free(mesh);

    DEBUG_LOG("[vulkan] mesh destroyed\n");
}

ShadowMap* vulkan_create_shadow_map(int size)
{
    if (!mesh_supported)
    {
        return NULL;
    }

    ShadowMap* shadow_map = SDL_calloc(1, sizeof(ShadowMap));
    shadow_map->size = size;

    if (!create_device_image((uint32_t)size, (uint32_t)size, DEPTH_FORMAT,
        VK_IMAGE_USAGE_DEPTH_STENCIL_ATTACHMENT_BIT | VK_IMAGE_USAGE_SAMPLED_BIT, &shadow_map->texture))
    {
        printf("Could not create a Vulkan shadow map image.\n");
        SDL_free(shadow_map);
        return NULL;
    }

    if (!create_image_view(shadow_map->texture.image, DEPTH_FORMAT, VK_IMAGE_ASPECT_DEPTH_BIT, &shadow_map->texture.view))
    {
        printf("Could not create a Vulkan shadow map view.\n");
        vmaDestroyImage(allocator, shadow_map->texture.image, shadow_map->texture.allocation);
        SDL_free(shadow_map);
        return NULL;
    }

    VkFramebufferCreateInfo framebuffer_create_info = {
        .sType = VK_STRUCTURE_TYPE_FRAMEBUFFER_CREATE_INFO,
        .renderPass = shadow_render_pass,
        .attachmentCount = 1,
        .pAttachments = &shadow_map->texture.view,
        .width = (uint32_t)size,
        .height = (uint32_t)size,
        .layers = 1
    };

    if (vkCreateFramebuffer(device, &framebuffer_create_info, NULL, &shadow_map->framebuffer) != VK_SUCCESS ||
        !allocate_texture_descriptor(&shadow_map->texture))
    {
        printf("Could not create a Vulkan shadow map framebuffer/descriptor.\n");
        vkDestroyFramebuffer(device, shadow_map->framebuffer, NULL);
        vkDestroyImageView(device, shadow_map->texture.view, NULL);
        vmaDestroyImage(allocator, shadow_map->texture.image, shadow_map->texture.allocation);
        SDL_free(shadow_map);
        return NULL;
    }

    register_bindless_texture(&shadow_map->texture);

    DEBUG_LOG("[vulkan] shadow map created: %dx%d\n", size, size);

    return shadow_map;
}

void vulkan_destroy_shadow_map(ShadowMap* shadow_map)
{
    release_bindless_index(shadow_map->texture.bindless_index);
    vkFreeDescriptorSets(device, descriptor_pool, 1, &shadow_map->texture.descriptor_set);
    vkDestroyFramebuffer(device, shadow_map->framebuffer, NULL);
    vkDestroyImageView(device, shadow_map->texture.view, NULL);
    vmaDestroyImage(allocator, shadow_map->texture.image, shadow_map->texture.allocation);
    SDL_free(shadow_map);

    DEBUG_LOG("[vulkan] shadow map destroyed\n");
}

int vulkan_begin_shadow_pass(ShadowMap* shadow_map, const float light_view_projection[16])
{
    if (!mesh_supported)
    {
        return 0;
    }

    current_command_buffer = begin_one_time_commands();

    VkClearValue clear_depth = { .depthStencil = { 1.0f, 0 } };

    VkRenderPassBeginInfo render_pass_begin_info = {
        .sType = VK_STRUCTURE_TYPE_RENDER_PASS_BEGIN_INFO,
        .renderPass = shadow_render_pass,
        .framebuffer = shadow_map->framebuffer,
        .renderArea.offset = { 0, 0 },
        .renderArea.extent = { (uint32_t)shadow_map->size, (uint32_t)shadow_map->size },
        .clearValueCount = 1,
        .pClearValues = &clear_depth
    };

    vkCmdBeginRenderPass(current_command_buffer, &render_pass_begin_info, VK_SUBPASS_CONTENTS_INLINE);
    vkCmdBindPipeline(current_command_buffer, VK_PIPELINE_BIND_POINT_GRAPHICS, shadow_pipeline);

    // Same negative-height viewport as every other pass : the shadow map's texel rows then match
    // the "clip +Y up, texture V down" lookup in mesh.slang exactly as on DX12.
    VkViewport viewport = {
        .x = 0.0f,
        .y = (float)shadow_map->size,
        .width = (float)shadow_map->size,
        .height = -(float)shadow_map->size,
        .minDepth = 0.0f,
        .maxDepth = 1.0f
    };
    vkCmdSetViewport(current_command_buffer, 0, 1, &viewport);

    VkRect2D scissor = { .offset = { 0, 0 }, .extent = { (uint32_t)shadow_map->size, (uint32_t)shadow_map->size } };
    vkCmdSetScissor(current_command_buffer, 0, 1, &scissor);

    mesh_constants_for_shadow_pass(&mesh_constants, light_view_projection);

    return 1;
}

void vulkan_end_shadow_pass(void)
{
    vkCmdEndRenderPass(current_command_buffer);
    end_one_time_commands(current_command_buffer);
}

void vulkan_begin_scene(const SceneLighting* lighting, ShadowMap* shadow_map)
{
    if (!mesh_supported)
    {
        return;
    }

    ensure_window_pass();

    vkCmdBindPipeline(current_command_buffer, VK_PIPELINE_BIND_POINT_GRAPHICS, mesh_pipeline);

    // Without a shadow map, the white texture stands in : a stored depth of 1 never occludes.
    Texture* shadow_texture = shadow_map ? &shadow_map->texture : &built_in_white_texture;
    vkCmdBindDescriptorSets(current_command_buffer, VK_PIPELINE_BIND_POINT_GRAPHICS, mesh_pipeline_layout,
        0, 1, &shadow_texture->descriptor_set, 0, NULL);
    scene_pass_texture = shadow_texture;

    mesh_constants_for_scene(&mesh_constants, lighting, shadow_map ? shadow_map->size : 0);
}

void vulkan_end_scene(void)
{
    if (!mesh_supported)
    {
        return;
    }

    // Back to the 2D state begin_frame() set up : the sprite pipeline, and its push constants
    // (invalidated by the mesh pipeline's different layout).
    vkCmdBindPipeline(current_command_buffer, VK_PIPELINE_BIND_POINT_GRAPHICS, pipeline);

    ScreenTransform transform = screen_transform_for_size((float)swapchain_extent.width, (float)swapchain_extent.height);
    vkCmdPushConstants(current_command_buffer, pipeline_layout, VK_SHADER_STAGE_VERTEX_BIT, 0, sizeof(transform), &transform);
}

void vulkan_draw_mesh(Mesh* mesh, const float model[16], int first_index, int index_count)
{
    if (!mesh_supported)
    {
        return;
    }

    SDL_memcpy(mesh_constants.model, model, sizeof(mesh_constants.model));
    vkCmdPushConstants(current_command_buffer, mesh_pipeline_layout, VK_SHADER_STAGE_VERTEX_BIT | VK_SHADER_STAGE_FRAGMENT_BIT,
        0, sizeof(mesh_constants), &mesh_constants);

    VkDeviceSize offset = 0;
    vkCmdBindVertexBuffers(current_command_buffer, 0, 1, &mesh->vertices.buffer, &offset);
    vkCmdBindIndexBuffer(current_command_buffer, mesh->indices.buffer, 0, VK_INDEX_TYPE_UINT32);
    uint32_t count = index_count < 0 ? mesh->index_count : (uint32_t)index_count;
    vkCmdDrawIndexed(current_command_buffer, count, 1, (uint32_t)first_index, 0, 0);
}

void vulkan_draw_skybox(Texture* equirectangular, float intensity)
{
    if (!mesh_supported)
    {
        return;
    }

    MeshConstants sky_constants;
    mesh_constants_for_sky(&sky_constants, mesh_constants.view_projection, intensity);

    vkCmdBindPipeline(current_command_buffer, VK_PIPELINE_BIND_POINT_GRAPHICS, sky_pipeline);
    vkCmdBindDescriptorSets(current_command_buffer, VK_PIPELINE_BIND_POINT_GRAPHICS, mesh_pipeline_layout,
        0, 1, &equirectangular->descriptor_set, 0, NULL);
    vkCmdPushConstants(current_command_buffer, mesh_pipeline_layout, VK_SHADER_STAGE_VERTEX_BIT | VK_SHADER_STAGE_FRAGMENT_BIT,
        0, sizeof(sky_constants), &sky_constants);
    vkCmdDraw(current_command_buffer, 3, 1, 0, 0);

    // Back to the scene's state, so draw_mesh() keeps working after the sky (each draw_mesh()
    // re-pushes the whole constant block, so only the pipeline and texture need restoring).
    vkCmdBindPipeline(current_command_buffer, VK_PIPELINE_BIND_POINT_GRAPHICS, mesh_pipeline);
    vkCmdBindDescriptorSets(current_command_buffer, VK_PIPELINE_BIND_POINT_GRAPHICS, mesh_pipeline_layout,
        0, 1, &scene_pass_texture->descriptor_set, 0, NULL);
}

// --- GPU layer (gpu.h) -------------------------------------------------------------------------

static VkFormat to_vk_format(GpuFormat format)
{
    switch (format)
    {
    case GPU_FORMAT_RGBA8: return VK_FORMAT_R8G8B8A8_UNORM;
    case GPU_FORMAT_RGBA8_SRGB: return VK_FORMAT_R8G8B8A8_SRGB;
    case GPU_FORMAT_RGBA16F: return VK_FORMAT_R16G16B16A16_SFLOAT;
    case GPU_FORMAT_RGBA32F: return VK_FORMAT_R32G32B32A32_SFLOAT;
    case GPU_FORMAT_RG16F: return VK_FORMAT_R16G16_SFLOAT;
    case GPU_FORMAT_R16F: return VK_FORMAT_R16_SFLOAT;
    case GPU_FORMAT_R8: return VK_FORMAT_R8_UNORM;
    case GPU_FORMAT_D32: return VK_FORMAT_D32_SFLOAT;
    default: return VK_FORMAT_UNDEFINED;
    }
}

static uint32_t format_pixel_size(GpuFormat format)
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

static void image_barrier(VkCommandBuffer command_buffer, Texture* texture, uint32_t mip_count, VkImageLayout from, VkImageLayout to,
    VkPipelineStageFlags src_stage, VkAccessFlags src_access, VkPipelineStageFlags dst_stage, VkAccessFlags dst_access)
{
    VkImageMemoryBarrier barrier = {
        .sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER,
        .srcAccessMask = src_access,
        .dstAccessMask = dst_access,
        .oldLayout = from,
        .newLayout = to,
        .srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
        .dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
        .image = texture->image,
        .subresourceRange = { texture->is_depth ? VK_IMAGE_ASPECT_DEPTH_BIT : VK_IMAGE_ASPECT_COLOR_BIT, 0, mip_count, 0, 1 }
    };

    vkCmdPipelineBarrier(command_buffer, src_stage, dst_stage, 0, 0, NULL, 0, NULL, 1, &barrier);
}

int vulkan_gpu_supported(void)
{
    return gpu_available;
}

Texture* vulkan_gpu_create_texture(const GpuTextureDesc* desc)
{
    if (!gpu_available)
    {
        return NULL;
    }

    Texture* texture = SDL_calloc(1, sizeof(Texture));
    texture->width = (uint32_t)desc->width;
    texture->height = (uint32_t)desc->height;
    texture->format = to_vk_format(desc->format);
    texture->is_depth = desc->format == GPU_FORMAT_D32;

    VkImageUsageFlags usage = VK_IMAGE_USAGE_SAMPLED_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT;

    if (desc->usage & GPU_USAGE_RENDER_TARGET)
    {
        usage |= VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT;
    }

    if (desc->usage & GPU_USAGE_DEPTH_TARGET)
    {
        usage |= VK_IMAGE_USAGE_DEPTH_STENCIL_ATTACHMENT_BIT;
    }

    VkImageCreateInfo image_create_info = {
        .sType = VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO,
        .imageType = VK_IMAGE_TYPE_2D,
        .format = texture->format,
        .extent = { texture->width, texture->height, 1 },
        .mipLevels = (uint32_t)desc->mip_count,
        .arrayLayers = 1,
        .samples = VK_SAMPLE_COUNT_1_BIT,
        .tiling = VK_IMAGE_TILING_OPTIMAL,
        .usage = usage,
        .sharingMode = VK_SHARING_MODE_EXCLUSIVE,
        .initialLayout = VK_IMAGE_LAYOUT_UNDEFINED
    };

    VmaAllocationCreateInfo allocation_create_info = { .usage = VMA_MEMORY_USAGE_AUTO_PREFER_DEVICE };

    if (vmaCreateImage(allocator, &image_create_info, &allocation_create_info, &texture->image, &texture->allocation, NULL) != VK_SUCCESS)
    {
        printf("Could not create a Vulkan GPU texture (%dx%d).\n", desc->width, desc->height);
        SDL_free(texture);
        return NULL;
    }

    VkImageViewCreateInfo view_create_info = {
        .sType = VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO,
        .image = texture->image,
        .viewType = VK_IMAGE_VIEW_TYPE_2D,
        .format = texture->format,
        .subresourceRange = { texture->is_depth ? VK_IMAGE_ASPECT_DEPTH_BIT : VK_IMAGE_ASPECT_COLOR_BIT, 0, (uint32_t)desc->mip_count, 0, 1 }
    };

    if (vkCreateImageView(device, &view_create_info, NULL, &texture->view) != VK_SUCCESS || !allocate_texture_descriptor(texture))
    {
        printf("Could not create a Vulkan GPU texture view.\n");
        vmaDestroyImage(allocator, texture->image, texture->allocation);
        SDL_free(texture);
        return NULL;
    }

    // Every mip's pixels go through one staging buffer and one submission ; a target left uninitialized
    // only needs its first transition to the resting, shader-readable layout.
    uint32_t pixel_size = format_pixel_size(desc->format);
    VkDeviceSize total_size = 0;

    if (desc->mips[0])
    {
        for (int mip = 0; mip < desc->mip_count; mip++)
        {
            uint32_t w = SDL_max(1u, texture->width >> mip);
            uint32_t h = SDL_max(1u, texture->height >> mip);
            total_size += (VkDeviceSize)w * h * pixel_size;
        }
    }

    MappedBuffer staging = { 0 };

    if (total_size > 0 && !create_mapped_buffer(total_size, VK_BUFFER_USAGE_TRANSFER_SRC_BIT, &staging))
    {
        printf("Could not create a staging buffer for a GPU texture.\n");
        vulkan_destroy_texture(texture);
        return NULL;
    }

    VkCommandBuffer command_buffer = begin_one_time_commands();

    if (total_size > 0)
    {
        image_barrier(command_buffer, texture, (uint32_t)desc->mip_count, VK_IMAGE_LAYOUT_UNDEFINED, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
            VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT, 0, VK_PIPELINE_STAGE_TRANSFER_BIT, VK_ACCESS_TRANSFER_WRITE_BIT);

        VkDeviceSize offset = 0;

        for (int mip = 0; mip < desc->mip_count; mip++)
        {
            uint32_t w = SDL_max(1u, texture->width >> mip);
            uint32_t h = SDL_max(1u, texture->height >> mip);
            VkDeviceSize size = (VkDeviceSize)w * h * pixel_size;

            SDL_memcpy((uint8_t*)staging.mapped + offset, desc->mips[mip], (size_t)size);

            VkBufferImageCopy region = {
                .bufferOffset = offset,
                .imageSubresource = { VK_IMAGE_ASPECT_COLOR_BIT, (uint32_t)mip, 0, 1 },
                .imageExtent = { w, h, 1 }
            };

            vkCmdCopyBufferToImage(command_buffer, staging.buffer, texture->image, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1, &region);
            offset += size;
        }

        image_barrier(command_buffer, texture, (uint32_t)desc->mip_count, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL,
            VK_PIPELINE_STAGE_TRANSFER_BIT, VK_ACCESS_TRANSFER_WRITE_BIT, VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT, VK_ACCESS_SHADER_READ_BIT);
    }
    else
    {
        image_barrier(command_buffer, texture, (uint32_t)desc->mip_count, VK_IMAGE_LAYOUT_UNDEFINED, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL,
            VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT, 0, VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT, VK_ACCESS_SHADER_READ_BIT);
    }

    end_one_time_commands(command_buffer);

    if (total_size > 0)
    {
        destroy_mapped_buffer(&staging);
    }

    register_bindless_texture(texture);

    return texture;
}

uint32_t vulkan_gpu_texture_index(Texture* texture)
{
    return texture->bindless_index;
}

GpuBuffer* vulkan_gpu_create_buffer(const void* data, size_t size)
{
    if (!gpu_available)
    {
        return NULL;
    }

    GpuBuffer* buffer = SDL_calloc(1, sizeof(GpuBuffer));

    if (!create_device_buffer(data, size, VK_BUFFER_USAGE_STORAGE_BUFFER_BIT, &buffer->device))
    {
        printf("Could not create a Vulkan GPU buffer.\n");
        SDL_free(buffer);
        return NULL;
    }

    return buffer;
}

void vulkan_gpu_destroy_buffer(GpuBuffer* buffer)
{
    destroy_device_buffer(&buffer->device);
    SDL_free(buffer);
}

uint32_t vulkan_gpu_buffer_index(GpuBuffer* buffer)
{
    return buffer->device.bindless_index;
}

uint32_t vulkan_gpu_mesh_vertices_index(Mesh* mesh)
{
    return mesh->vertices.bindless_index;
}

uint32_t vulkan_gpu_mesh_indices_index(Mesh* mesh)
{
    return mesh->indices.bindless_index;
}

GpuFrameAllocation vulkan_gpu_frame_alloc(size_t size)
{
    GpuFrameAllocation allocation = { NULL, 0, 0 };
    uint32_t aligned = (gpu_frame_offset + 255u) & ~255u;

    if (!gpu_available || aligned + size > GPU_FRAME_BUFFER_SIZE)
    {
        return allocation;
    }

    allocation.mapped = (uint8_t*)gpu_frame_buffers[current_frame].mapped + aligned;
    allocation.buffer_index = gpu_frame_buffer_indices[current_frame];
    allocation.offset = aligned;
    gpu_frame_offset = aligned + (uint32_t)size;

    return allocation;
}

GpuPipeline* vulkan_gpu_create_pipeline(const GpuPipelineDesc* desc)
{
    if (!gpu_available)
    {
        return NULL;
    }

    size_t vert_size = 0;
    size_t frag_size = 0;
    uint8_t* vert_code = load_shader(desc->vertex_shader, &vert_size);
    uint8_t* frag_code = desc->fragment_shader ? load_shader(desc->fragment_shader, &frag_size) : NULL;

    if (!vert_code || (desc->fragment_shader && !frag_code))
    {
        printf("Could not load the '%s' / '%s' shaders.\n", desc->vertex_shader, desc->fragment_shader ? desc->fragment_shader : "");
        SDL_free(vert_code);
        SDL_free(frag_code);
        return NULL;
    }

    VkShaderModule vert_module = create_shader_module(vert_code, vert_size);
    VkShaderModule frag_module = frag_code ? create_shader_module(frag_code, frag_size) : VK_NULL_HANDLE;

    SDL_free(vert_code);
    SDL_free(frag_code);

    VkPipelineShaderStageCreateInfo shader_stages[2] = {
        { .sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO, .stage = VK_SHADER_STAGE_VERTEX_BIT, .module = vert_module, .pName = "main" },
        { .sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO, .stage = VK_SHADER_STAGE_FRAGMENT_BIT, .module = frag_module, .pName = "main" }
    };

    VkPipelineVertexInputStateCreateInfo vertex_input_state = { .sType = VK_STRUCTURE_TYPE_PIPELINE_VERTEX_INPUT_STATE_CREATE_INFO };

    VkPipelineInputAssemblyStateCreateInfo input_assembly_state = {
        .sType = VK_STRUCTURE_TYPE_PIPELINE_INPUT_ASSEMBLY_STATE_CREATE_INFO,
        .topology = VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST
    };

    VkPipelineViewportStateCreateInfo viewport_state = {
        .sType = VK_STRUCTURE_TYPE_PIPELINE_VIEWPORT_STATE_CREATE_INFO,
        .viewportCount = 1,
        .scissorCount = 1
    };

    // A negative viewport height mirrors the winding, so a counter-clockwise front face in clip space is
    // what ends up clockwise on screen, as on DX12.
    VkPipelineRasterizationStateCreateInfo rasterization_state = {
        .sType = VK_STRUCTURE_TYPE_PIPELINE_RASTERIZATION_STATE_CREATE_INFO,
        .polygonMode = VK_POLYGON_MODE_FILL,
        .cullMode = desc->cull_back ? VK_CULL_MODE_BACK_BIT : VK_CULL_MODE_NONE,
        .frontFace = VK_FRONT_FACE_COUNTER_CLOCKWISE,
        .depthBiasEnable = (desc->depth_bias_constant != 0.0f || desc->depth_bias_slope != 0.0f) ? VK_TRUE : VK_FALSE,
        .depthBiasConstantFactor = desc->depth_bias_constant,
        .depthBiasSlopeFactor = desc->depth_bias_slope,
        .lineWidth = 1.0f
    };

    VkPipelineMultisampleStateCreateInfo multisample_state = {
        .sType = VK_STRUCTURE_TYPE_PIPELINE_MULTISAMPLE_STATE_CREATE_INFO,
        .rasterizationSamples = VK_SAMPLE_COUNT_1_BIT
    };

    VkPipelineDepthStencilStateCreateInfo depth_stencil_state = {
        .sType = VK_STRUCTURE_TYPE_PIPELINE_DEPTH_STENCIL_STATE_CREATE_INFO,
        .depthTestEnable = desc->depth_test ? VK_TRUE : VK_FALSE,
        .depthWriteEnable = desc->depth_write ? VK_TRUE : VK_FALSE,
        .depthCompareOp = VK_COMPARE_OP_LESS_OR_EQUAL
    };

    VkPipelineColorBlendAttachmentState blend_attachments[GPU_MAX_COLOR_TARGETS];
    VkFormat color_formats[GPU_MAX_COLOR_TARGETS];

    for (int i = 0; i < desc->color_count; i++)
    {
        VkPipelineColorBlendAttachmentState attachment = {
            .blendEnable = desc->blend != GPU_BLEND_NONE ? VK_TRUE : VK_FALSE,
            .srcColorBlendFactor = desc->blend == GPU_BLEND_ADD ? VK_BLEND_FACTOR_ONE : VK_BLEND_FACTOR_SRC_ALPHA,
            .dstColorBlendFactor = desc->blend == GPU_BLEND_ADD ? VK_BLEND_FACTOR_ONE : VK_BLEND_FACTOR_ONE_MINUS_SRC_ALPHA,
            .colorBlendOp = VK_BLEND_OP_ADD,
            .srcAlphaBlendFactor = VK_BLEND_FACTOR_ONE,
            .dstAlphaBlendFactor = VK_BLEND_FACTOR_ONE,
            .alphaBlendOp = VK_BLEND_OP_ADD,
            .colorWriteMask = VK_COLOR_COMPONENT_R_BIT | VK_COLOR_COMPONENT_G_BIT | VK_COLOR_COMPONENT_B_BIT | VK_COLOR_COMPONENT_A_BIT
        };

        blend_attachments[i] = attachment;
        color_formats[i] = to_vk_format(desc->color_formats[i]);
    }

    VkPipelineColorBlendStateCreateInfo color_blend_state = {
        .sType = VK_STRUCTURE_TYPE_PIPELINE_COLOR_BLEND_STATE_CREATE_INFO,
        .attachmentCount = (uint32_t)desc->color_count,
        .pAttachments = blend_attachments
    };

    VkDynamicState dynamic_states[2] = { VK_DYNAMIC_STATE_VIEWPORT, VK_DYNAMIC_STATE_SCISSOR };

    VkPipelineDynamicStateCreateInfo dynamic_state = {
        .sType = VK_STRUCTURE_TYPE_PIPELINE_DYNAMIC_STATE_CREATE_INFO,
        .dynamicStateCount = 2,
        .pDynamicStates = dynamic_states
    };

    VkPipelineRenderingCreateInfo rendering_info = {
        .sType = VK_STRUCTURE_TYPE_PIPELINE_RENDERING_CREATE_INFO,
        .colorAttachmentCount = (uint32_t)desc->color_count,
        .pColorAttachmentFormats = color_formats,
        .depthAttachmentFormat = to_vk_format(desc->depth_format)
    };

    VkGraphicsPipelineCreateInfo pipeline_create_info = {
        .sType = VK_STRUCTURE_TYPE_GRAPHICS_PIPELINE_CREATE_INFO,
        .pNext = &rendering_info,
        .stageCount = desc->fragment_shader ? 2 : 1,
        .pStages = shader_stages,
        .pVertexInputState = &vertex_input_state,
        .pInputAssemblyState = &input_assembly_state,
        .pViewportState = &viewport_state,
        .pRasterizationState = &rasterization_state,
        .pMultisampleState = &multisample_state,
        .pDepthStencilState = &depth_stencil_state,
        .pColorBlendState = &color_blend_state,
        .pDynamicState = &dynamic_state,
        .layout = gpu_pipeline_layout
    };

    GpuPipeline* pipeline = SDL_calloc(1, sizeof(GpuPipeline));
    VkResult result = vkCreateGraphicsPipelines(device, VK_NULL_HANDLE, 1, &pipeline_create_info, NULL, &pipeline->pipeline);

    vkDestroyShaderModule(device, vert_module, NULL);
    vkDestroyShaderModule(device, frag_module, NULL);

    if (result != VK_SUCCESS)
    {
        printf("Could not create the Vulkan '%s' pipeline (%d).\n", desc->vertex_shader, (int)result);
        SDL_free(pipeline);
        return NULL;
    }

    return pipeline;
}

void vulkan_gpu_destroy_pipeline(GpuPipeline* pipeline)
{
    vkDestroyPipeline(device, pipeline->pipeline, NULL);
    SDL_free(pipeline);
}

void vulkan_gpu_begin_pass(const GpuPassDesc* pass)
{
    if (window_pass_active)
    {
        printf("gpu_begin_pass() called after drawing into the window : ignored.\n");
        return;
    }

    VkRenderingAttachmentInfo color_attachments[GPU_MAX_COLOR_TARGETS];
    uint32_t width = 0;
    uint32_t height = 0;

    gpu_pass_color_count = pass->color_count;

    for (int i = 0; i < pass->color_count; i++)
    {
        const GpuColorAttachment* source = &pass->colors[i];
        Texture* texture = source->texture;

        gpu_pass_colors[i] = texture;
        width = texture->width;
        height = texture->height;

        image_barrier(current_command_buffer, texture, 1,
            source->load == GPU_LOAD_KEEP ? VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL : VK_IMAGE_LAYOUT_UNDEFINED,
            VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL,
            VK_PIPELINE_STAGE_ALL_GRAPHICS_BIT, VK_ACCESS_SHADER_READ_BIT,
            VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT, VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT | VK_ACCESS_COLOR_ATTACHMENT_READ_BIT);

        VkRenderingAttachmentInfo attachment = {
            .sType = VK_STRUCTURE_TYPE_RENDERING_ATTACHMENT_INFO,
            .imageView = texture->view,
            .imageLayout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL,
            .loadOp = source->load == GPU_LOAD_CLEAR ? VK_ATTACHMENT_LOAD_OP_CLEAR : source->load == GPU_LOAD_KEEP ? VK_ATTACHMENT_LOAD_OP_LOAD : VK_ATTACHMENT_LOAD_OP_DONT_CARE,
            .storeOp = VK_ATTACHMENT_STORE_OP_STORE,
            .clearValue.color.float32 = { source->clear[0], source->clear[1], source->clear[2], source->clear[3] }
        };

        color_attachments[i] = attachment;
    }

    VkRenderingAttachmentInfo depth_attachment;
    gpu_pass_depth = pass->depth;

    if (pass->depth)
    {
        width = pass->depth->width;
        height = pass->depth->height;

        image_barrier(current_command_buffer, pass->depth, 1,
            pass->depth_load == GPU_LOAD_KEEP ? VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL : VK_IMAGE_LAYOUT_UNDEFINED,
            VK_IMAGE_LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL,
            VK_PIPELINE_STAGE_ALL_GRAPHICS_BIT, VK_ACCESS_SHADER_READ_BIT,
            VK_PIPELINE_STAGE_EARLY_FRAGMENT_TESTS_BIT | VK_PIPELINE_STAGE_LATE_FRAGMENT_TESTS_BIT,
            VK_ACCESS_DEPTH_STENCIL_ATTACHMENT_READ_BIT | VK_ACCESS_DEPTH_STENCIL_ATTACHMENT_WRITE_BIT);

        VkRenderingAttachmentInfo attachment = {
            .sType = VK_STRUCTURE_TYPE_RENDERING_ATTACHMENT_INFO,
            .imageView = pass->depth->view,
            .imageLayout = VK_IMAGE_LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL,
            .loadOp = pass->depth_load == GPU_LOAD_CLEAR ? VK_ATTACHMENT_LOAD_OP_CLEAR : pass->depth_load == GPU_LOAD_KEEP ? VK_ATTACHMENT_LOAD_OP_LOAD : VK_ATTACHMENT_LOAD_OP_DONT_CARE,
            .storeOp = VK_ATTACHMENT_STORE_OP_STORE,
            .clearValue.depthStencil = { pass->depth_clear, 0 }
        };

        depth_attachment = attachment;
    }

    VkRenderingInfo rendering_info = {
        .sType = VK_STRUCTURE_TYPE_RENDERING_INFO,
        .renderArea = { { 0, 0 }, { width, height } },
        .layerCount = 1,
        .colorAttachmentCount = (uint32_t)pass->color_count,
        .pColorAttachments = color_attachments,
        .pDepthAttachment = pass->depth ? &depth_attachment : NULL
    };

    vkCmdBeginRendering(current_command_buffer, &rendering_info);

    // The negative-height viewport of every other pass : clip +Y up on both backends.
    VkViewport viewport = { 0.0f, (float)height, (float)width, -(float)height, 0.0f, 1.0f };
    vkCmdSetViewport(current_command_buffer, 0, 1, &viewport);

    VkRect2D scissor = { { 0, 0 }, { width, height } };
    vkCmdSetScissor(current_command_buffer, 0, 1, &scissor);

    vkCmdBindDescriptorSets(current_command_buffer, VK_PIPELINE_BIND_POINT_GRAPHICS, gpu_pipeline_layout, 0, 1, &bindless_set, 0, NULL);
}

void vulkan_gpu_end_pass(void)
{
    vkCmdEndRendering(current_command_buffer);

    for (int i = 0; i < gpu_pass_color_count; i++)
    {
        image_barrier(current_command_buffer, gpu_pass_colors[i], 1, VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL,
            VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT, VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT,
            VK_PIPELINE_STAGE_VERTEX_SHADER_BIT | VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT, VK_ACCESS_SHADER_READ_BIT);
    }

    if (gpu_pass_depth)
    {
        image_barrier(current_command_buffer, gpu_pass_depth, 1, VK_IMAGE_LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL,
            VK_PIPELINE_STAGE_LATE_FRAGMENT_TESTS_BIT, VK_ACCESS_DEPTH_STENCIL_ATTACHMENT_WRITE_BIT,
            VK_PIPELINE_STAGE_VERTEX_SHADER_BIT | VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT, VK_ACCESS_SHADER_READ_BIT);
    }

    gpu_pass_color_count = 0;
    gpu_pass_depth = NULL;
}

void vulkan_gpu_bind_pipeline(GpuPipeline* pipeline)
{
    vkCmdBindPipeline(current_command_buffer, VK_PIPELINE_BIND_POINT_GRAPHICS, pipeline->pipeline);
}

void vulkan_gpu_push_constants(const void* data, int size)
{
    vkCmdPushConstants(current_command_buffer, gpu_pipeline_layout, VK_SHADER_STAGE_ALL, 0, (uint32_t)size, data);
}

void vulkan_gpu_draw(int vertex_count)
{
    vkCmdDraw(current_command_buffer, (uint32_t)vertex_count, 1, 0, 0);
}

void vulkan_gpu_draw_indexed(Mesh* mesh, int first_index, int index_count)
{
    vkCmdBindIndexBuffer(current_command_buffer, mesh->indices.buffer, 0, VK_INDEX_TYPE_UINT32);
    vkCmdDrawIndexed(current_command_buffer, (uint32_t)index_count, 1, (uint32_t)first_index, 0, 0);
}

int vulkan_gpu_raytracing_supported(void)
{
    return rt_available;
}

// The structure for this range of the mesh's triangles, built the first time it's needed.
static const Blas* ensure_blas(Mesh* mesh, uint32_t first_index, uint32_t index_count)
{
    for (int i = 0; i < mesh->blas_count; i++)
    {
        if (mesh->blas[i].first_index == first_index && mesh->blas[i].index_count == index_count)
        {
            return &mesh->blas[i];
        }
    }

    VkAccelerationStructureGeometryKHR geometry = {
        .sType = VK_STRUCTURE_TYPE_ACCELERATION_STRUCTURE_GEOMETRY_KHR,
        .geometryType = VK_GEOMETRY_TYPE_TRIANGLES_KHR,
        .flags = VK_GEOMETRY_OPAQUE_BIT_KHR,
        .geometry.triangles = {
            .sType = VK_STRUCTURE_TYPE_ACCELERATION_STRUCTURE_GEOMETRY_TRIANGLES_DATA_KHR,
            .vertexFormat = VK_FORMAT_R32G32B32_SFLOAT,
            .vertexData.deviceAddress = buffer_address(mesh->vertices.buffer),
            .vertexStride = sizeof(MeshVertex),
            .maxVertex = mesh->vertex_count - 1,
            .indexType = VK_INDEX_TYPE_UINT32,
            .indexData.deviceAddress = buffer_address(mesh->indices.buffer) + (VkDeviceAddress)first_index * sizeof(uint32_t)
        }
    };

    VkAccelerationStructureBuildGeometryInfoKHR build_info = {
        .sType = VK_STRUCTURE_TYPE_ACCELERATION_STRUCTURE_BUILD_GEOMETRY_INFO_KHR,
        .type = VK_ACCELERATION_STRUCTURE_TYPE_BOTTOM_LEVEL_KHR,
        .flags = VK_BUILD_ACCELERATION_STRUCTURE_PREFER_FAST_TRACE_BIT_KHR,
        .mode = VK_BUILD_ACCELERATION_STRUCTURE_MODE_BUILD_KHR,
        .geometryCount = 1,
        .pGeometries = &geometry
    };

    uint32_t triangles = index_count / 3;
    VkAccelerationStructureBuildSizesInfoKHR sizes = { .sType = VK_STRUCTURE_TYPE_ACCELERATION_STRUCTURE_BUILD_SIZES_INFO_KHR };
    vkGetAccelerationStructureBuildSizesKHR(device, VK_ACCELERATION_STRUCTURE_BUILD_TYPE_DEVICE_KHR, &build_info, &triangles, &sizes);

    mesh->blas = SDL_realloc(mesh->blas, sizeof(Blas) * (size_t)(mesh->blas_count + 1));
    Blas* blas = &mesh->blas[mesh->blas_count];
    SDL_zerop(blas);
    blas->first_index = first_index;
    blas->index_count = index_count;

    VkBuffer scratch = VK_NULL_HANDLE;
    VmaAllocation scratch_allocation = NULL;

    if (!create_acceleration_buffer(sizes.accelerationStructureSize, VK_BUFFER_USAGE_ACCELERATION_STRUCTURE_STORAGE_BIT_KHR, &blas->buffer, &blas->allocation) ||
        !create_acceleration_buffer(sizes.buildScratchSize + rt_scratch_alignment, VK_BUFFER_USAGE_STORAGE_BUFFER_BIT, &scratch, &scratch_allocation))
    {
        printf("Could not allocate a Vulkan acceleration structure.\n");
        return NULL;
    }

    VkAccelerationStructureCreateInfoKHR create_info = {
        .sType = VK_STRUCTURE_TYPE_ACCELERATION_STRUCTURE_CREATE_INFO_KHR,
        .buffer = blas->buffer,
        .size = sizes.accelerationStructureSize,
        .type = VK_ACCELERATION_STRUCTURE_TYPE_BOTTOM_LEVEL_KHR
    };

    vkCreateAccelerationStructureKHR(device, &create_info, NULL, &blas->handle);

    build_info.dstAccelerationStructure = blas->handle;
    build_info.scratchData.deviceAddress = align_address(buffer_address(scratch));

    VkAccelerationStructureBuildRangeInfoKHR range = { .primitiveCount = triangles };
    const VkAccelerationStructureBuildRangeInfoKHR* ranges = &range;

    VkCommandBuffer command_buffer = begin_one_time_commands();
    vkCmdBuildAccelerationStructuresKHR(command_buffer, 1, &build_info, &ranges);
    end_one_time_commands(command_buffer);

    vmaDestroyBuffer(allocator, scratch, scratch_allocation);

    VkAccelerationStructureDeviceAddressInfoKHR address_info = {
        .sType = VK_STRUCTURE_TYPE_ACCELERATION_STRUCTURE_DEVICE_ADDRESS_INFO_KHR,
        .accelerationStructure = blas->handle
    };

    blas->address = vkGetAccelerationStructureDeviceAddressKHR(device, &address_info);
    mesh->blas_count++;

    return blas;
}

uint32_t vulkan_gpu_build_ray_scene(const GpuRayInstance* instances, int count)
{
    if (!rt_available)
    {
        return 0;
    }

    count = SDL_min(count, MAX_RAY_INSTANCES);

    VkAccelerationStructureInstanceKHR* out = tlas_instances[current_frame].mapped;

    for (int i = 0; i < count; i++)
    {
        const Blas* blas = ensure_blas(instances[i].mesh, (uint32_t)instances[i].first_index, (uint32_t)instances[i].index_count);

        SDL_memcpy(&out[i].transform, instances[i].transform, sizeof(out[i].transform));
        out[i].instanceCustomIndex = (uint32_t)i;
        out[i].mask = 0xFF;
        out[i].instanceShaderBindingTableRecordOffset = 0;
        out[i].flags = VK_GEOMETRY_INSTANCE_TRIANGLE_FACING_CULL_DISABLE_BIT_KHR | (instances[i].alpha_tested ? VK_GEOMETRY_INSTANCE_FORCE_NO_OPAQUE_BIT_KHR : 0);
        out[i].accelerationStructureReference = blas ? blas->address : 0;
    }

    VkAccelerationStructureGeometryKHR geometry = {
        .sType = VK_STRUCTURE_TYPE_ACCELERATION_STRUCTURE_GEOMETRY_KHR,
        .geometryType = VK_GEOMETRY_TYPE_INSTANCES_KHR,
        .geometry.instances = {
            .sType = VK_STRUCTURE_TYPE_ACCELERATION_STRUCTURE_GEOMETRY_INSTANCES_DATA_KHR,
            .data.deviceAddress = buffer_address(tlas_instances[current_frame].buffer)
        }
    };

    VkAccelerationStructureBuildGeometryInfoKHR build_info = {
        .sType = VK_STRUCTURE_TYPE_ACCELERATION_STRUCTURE_BUILD_GEOMETRY_INFO_KHR,
        .type = VK_ACCELERATION_STRUCTURE_TYPE_TOP_LEVEL_KHR,
        .flags = VK_BUILD_ACCELERATION_STRUCTURE_PREFER_FAST_BUILD_BIT_KHR,
        .mode = VK_BUILD_ACCELERATION_STRUCTURE_MODE_BUILD_KHR,
        .dstAccelerationStructure = tlas[current_frame],
        .geometryCount = 1,
        .pGeometries = &geometry,
        .scratchData.deviceAddress = tlas_scratch_address
    };

    VkAccelerationStructureBuildRangeInfoKHR range = { .primitiveCount = (uint32_t)count };
    const VkAccelerationStructureBuildRangeInfoKHR* ranges = &range;

    // The scratch memory is shared between frames ; and shaders read the result.
    VkMemoryBarrier before = {
        .sType = VK_STRUCTURE_TYPE_MEMORY_BARRIER,
        .srcAccessMask = VK_ACCESS_ACCELERATION_STRUCTURE_WRITE_BIT_KHR | VK_ACCESS_HOST_WRITE_BIT,
        .dstAccessMask = VK_ACCESS_ACCELERATION_STRUCTURE_READ_BIT_KHR | VK_ACCESS_ACCELERATION_STRUCTURE_WRITE_BIT_KHR
    };

    vkCmdPipelineBarrier(current_command_buffer, VK_PIPELINE_STAGE_ACCELERATION_STRUCTURE_BUILD_BIT_KHR | VK_PIPELINE_STAGE_HOST_BIT,
        VK_PIPELINE_STAGE_ACCELERATION_STRUCTURE_BUILD_BIT_KHR, 0, 1, &before, 0, NULL, 0, NULL);

    vkCmdBuildAccelerationStructuresKHR(current_command_buffer, 1, &build_info, &ranges);

    VkMemoryBarrier after = {
        .sType = VK_STRUCTURE_TYPE_MEMORY_BARRIER,
        .srcAccessMask = VK_ACCESS_ACCELERATION_STRUCTURE_WRITE_BIT_KHR,
        .dstAccessMask = VK_ACCESS_ACCELERATION_STRUCTURE_READ_BIT_KHR
    };

    vkCmdPipelineBarrier(current_command_buffer, VK_PIPELINE_STAGE_ACCELERATION_STRUCTURE_BUILD_BIT_KHR,
        VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT, 0, 1, &after, 0, NULL, 0, NULL);

    return current_frame;
}

static const RendererBackend vulkan_backend = {
    .create_renderer = vulkan_create_renderer,
    .destroy_renderer = vulkan_destroy_renderer,
    .wait_renderer_idle = vulkan_wait_renderer_idle,
    .get_stats = vulkan_get_stats,
    .begin_frame = vulkan_begin_frame,
    .end_frame = vulkan_end_frame,
    .create_texture = vulkan_create_texture,
    .destroy_texture = vulkan_destroy_texture,
    .white_texture = vulkan_white_texture,
    .texture_get_size = vulkan_texture_get_size,
    .create_texture_from_pixels = vulkan_create_texture_from_pixels,
    .create_render_target = vulkan_create_render_target,
    .destroy_render_target = vulkan_destroy_render_target,
    .render_target_texture = vulkan_render_target_texture,
    .begin_render_target_pass = vulkan_begin_render_target_pass,
    .end_render_target_pass = vulkan_end_render_target_pass,
    .draw_vertices = vulkan_draw_vertices,
    .set_scissor = vulkan_set_scissor,
    .create_mesh = vulkan_create_mesh,
    .destroy_mesh = vulkan_destroy_mesh,
    .create_shadow_map = vulkan_create_shadow_map,
    .destroy_shadow_map = vulkan_destroy_shadow_map,
    .begin_shadow_pass = vulkan_begin_shadow_pass,
    .end_shadow_pass = vulkan_end_shadow_pass,
    .begin_scene = vulkan_begin_scene,
    .end_scene = vulkan_end_scene,
    .draw_mesh = vulkan_draw_mesh,
    .draw_skybox = vulkan_draw_skybox,
    .gpu_supported = vulkan_gpu_supported,
    .gpu_create_texture = vulkan_gpu_create_texture,
    .gpu_texture_index = vulkan_gpu_texture_index,
    .gpu_create_buffer = vulkan_gpu_create_buffer,
    .gpu_destroy_buffer = vulkan_gpu_destroy_buffer,
    .gpu_buffer_index = vulkan_gpu_buffer_index,
    .gpu_mesh_vertices_index = vulkan_gpu_mesh_vertices_index,
    .gpu_mesh_indices_index = vulkan_gpu_mesh_indices_index,
    .gpu_frame_alloc = vulkan_gpu_frame_alloc,
    .gpu_create_pipeline = vulkan_gpu_create_pipeline,
    .gpu_destroy_pipeline = vulkan_gpu_destroy_pipeline,
    .gpu_begin_pass = vulkan_gpu_begin_pass,
    .gpu_end_pass = vulkan_gpu_end_pass,
    .gpu_bind_pipeline = vulkan_gpu_bind_pipeline,
    .gpu_push_constants = vulkan_gpu_push_constants,
    .gpu_draw = vulkan_gpu_draw,
    .gpu_draw_indexed = vulkan_gpu_draw_indexed,
    .gpu_raytracing_supported = vulkan_gpu_raytracing_supported,
    .gpu_build_ray_scene = vulkan_gpu_build_ray_scene
};

const RendererBackend* vulkan_get_backend(void)
{
    return &vulkan_backend;
}
