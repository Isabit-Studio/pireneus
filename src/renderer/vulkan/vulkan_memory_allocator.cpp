// The one C++ translation unit of the engine : VulkanMemoryAllocator is a C++ library behind a C API,
// so its implementation has to be compiled as C++. Everything else (vulkan_renderer.c included) only
// sees that C API through vk_mem_alloc.h.
//
// volk.h is included first so that vk_mem_alloc.h sees VOLK_HEADER_VERSION and declares
// vmaImportVulkanFunctionsFromVolk() (VMA's own static/dynamic function loading paths are disabled
// for every user of the header, see the vma target in third-parties/CMakeLists.txt).

#include <volk.h>

#define VMA_IMPLEMENTATION
#include <vk_mem_alloc.h>
