cmake_minimum_required(VERSION 4.0)

# C++ is only needed for VulkanMemoryAllocator's implementation (vk_mem_alloc.h is a C++ library
# with a C API, see src/renderer/vulkan/vulkan_memory_allocator.cpp) ; the engine itself stays C.
project(pireneus LANGUAGES C CXX)

# A Universal Windows Platform build : a CoreWindow application running in an AppContainer, on DirectX 12
# (Vulkan doesn't exist there), packaged as MSIX. Configure it in a build directory of its own :
#     cmake -S . -B build-uwp -G Ninja -DCMAKE_BUILD_TYPE=Release -DPIRENEUS_UWP=ON
option(PIRENEUS_UWP "Build for the Universal Windows Platform" OFF)

if(PIRENEUS_UWP)
    # Static CRT : the package is self-contained, with no dependency on a VCLibs framework package.
    set(CMAKE_MSVC_RUNTIME_LIBRARY "MultiThreaded$<$<CONFIG:Debug>:Debug>")
    set(CMAKE_CXX_STANDARD 20) # C++/WinRT's coroutines, in src/platforms/uwp only
    add_compile_definitions(PIRENEUS_UWP)
endif()

# Default rendering backend, used at runtime when the PIRENEUS_RENDERER environment variable isn't
# set (both backends are compiled into the same binary on Windows and chosen at runtime via that
# environment variable, see src/renderer/renderer.c ; this CMake variable only feeds the compiled-in
# default). Non-Windows builds only ever compile Vulkan, regardless of this value.
set(PIRENEUS_RENDERER "dx12" CACHE STRING "Default rendering backend: vulkan, dx12 (dx12 only takes effect on Windows)")
set_property(CACHE PIRENEUS_RENDERER PROPERTY STRINGS vulkan dx12)

# Export compile commands for tools like clangd
set(CMAKE_EXPORT_COMPILE_COMMANDS ON)

include(cmake/pireneus_shaders.cmake)

# Include third-party libraries
add_subdirectory(third-parties)

add_library(pireneus STATIC
    src/renderer/sprite_batch.c
    src/renderer/photoreal/photoreal.c
    src/renderer/photoreal/environment.c
    src/renderer/photoreal/pbr_texture.c
    src/assets/hdr_image.c
    src/assets/tinyexr_impl.cpp
    src/assets/asset_database.c
    src/audio/audio.c
    src/physics/physics.c
    src/project/project.c
    src/scene/scene.c
    src/scene/scene_runtime.c
    src/json/json.c
    src/tiled/tiled_map.c
    src/tiled/tiled_map_render.c
    src/model/model.c
    src/meshedit/edit_mesh.c
    src/meshedit/edit_mesh_gpu.c
    src/platforms/platforms_system.c
)

target_include_directories(pireneus PUBLIC ${PIRENEUS_THIRDPARTIES_INCLUDES} src)
target_link_libraries(pireneus PUBLIC SDL3-static SDL3_image::SDL3_image SDL3_mixer::SDL3_mixer miniz box2d box3d)
target_link_libraries(pireneus PRIVATE ufbx cgltf tinyexr)

# Enables DEBUG_LOG() (see src/debug.h) throughout the engine and the examples, in Debug builds only.
target_compile_definitions(pireneus PUBLIC $<$<CONFIG:Debug>:DEBUG>)

# Vulkan compiles everywhere but UWP (source-only headers, functions loaded at runtime by volk, memory
# managed by VMA, see src/renderer/vulkan). DX12 only compiles on Windows, where d3d12.h/dxgi1_6.h and
# the d3d12/dxgi/dxguid import libs exist. Both, plus src/renderer/renderer.c (the runtime dispatch
# layer), are compiled into the same binary : PIRENEUS_RENDERER above only picks the default.
target_sources(pireneus PRIVATE src/renderer/renderer.c)

if(PIRENEUS_UWP)
    target_sources(pireneus PRIVATE src/platforms/uwp/uwp_platform.cpp src/platforms/uwp/uwp_audio.cpp)
    target_link_libraries(pireneus PUBLIC WindowsApp xaudio2)
    set_source_files_properties(src/platforms/uwp/uwp_platform.cpp PROPERTIES COMPILE_OPTIONS "/EHsc;/await:strict")
else()
    target_sources(pireneus PRIVATE
        src/platforms/sdl/sdl_platform_window.c
        src/platforms/sdl/sdl_platform_input.c
        src/platforms/sdl/sdl_platform_paths.c
        src/renderer/vulkan/vulkan_renderer.c
        src/renderer/vulkan/vulkan_memory_allocator.cpp
    )
    target_link_libraries(pireneus PUBLIC Vulkan-Headers PRIVATE volk vma)
endif()

if(WIN32)
    target_sources(pireneus PRIVATE src/renderer/dx12/dx12_renderer.c)
    target_link_libraries(pireneus PUBLIC d3d12 dxgi dxguid)
elseif(PIRENEUS_RENDERER STREQUAL "dx12")
    message(WARNING "PIRENEUS_RENDERER default is 'dx12' but DX12 isn't available on this platform; the runtime default will fall back to Vulkan.")
endif()

if(WIN32)
    target_compile_definitions(pireneus PRIVATE PIRENEUS_DEFAULT_RENDERER="${PIRENEUS_RENDERER}")
else()
    target_compile_definitions(pireneus PRIVATE PIRENEUS_DEFAULT_RENDERER="vulkan")
endif()

# Dear ImGui integration (src/imgui) : a separate library, so only the tools that actually use
# ImGui link it, rather than every program built on the engine.
add_library(pireneus_imgui STATIC src/imgui/pireneus_imgui.cpp)
target_link_libraries(pireneus_imgui PUBLIC pireneus imgui)

pireneus_compile_shaders()

# Every example, in one executable with a Dear ImGui menu.
add_subdirectory(examples)
