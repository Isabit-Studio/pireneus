include(FetchContent)

# Everything is built statically throughout the project.
set(BUILD_SHARED_LIBS OFF CACHE BOOL "" FORCE)

# --- SDL3 ---
set(SDL_SHARED OFF)
set(SDL_STATIC ON)

# SDL3 has no UWP support : there it's only used for what doesn't depend on a platform layer (surfaces,
# file I/O, audio conversion...), with every subsystem talking to the desktop turned off.
if(PIRENEUS_UWP)
    set(SDL_VIDEO OFF CACHE BOOL "" FORCE)
    set(SDL_RENDER OFF CACHE BOOL "" FORCE)
    set(SDL_GPU OFF CACHE BOOL "" FORCE)
    set(SDL_CAMERA OFF CACHE BOOL "" FORCE)
    set(SDL_JOYSTICK OFF CACHE BOOL "" FORCE)
    set(SDL_HAPTIC OFF CACHE BOOL "" FORCE)
    set(SDL_HIDAPI OFF CACHE BOOL "" FORCE)
    set(SDL_POWER OFF CACHE BOOL "" FORCE)
    set(SDL_SENSOR OFF CACHE BOOL "" FORCE)
    set(SDL_DIALOG OFF CACHE BOOL "" FORCE)
    set(SDL_TRAY OFF CACHE BOOL "" FORCE)
    set(SDL_WASAPI OFF CACHE BOOL "" FORCE)
    set(SDL_DIRECTX OFF CACHE BOOL "" FORCE)
    set(SDL_TESTS OFF CACHE BOOL "" FORCE)
    set(SDL_TEST_LIBRARY OFF CACHE BOOL "" FORCE)
endif()

FetchContent_Declare(
    SDL3
    GIT_REPOSITORY https://github.com/libsdl-org/SDL.git
    GIT_TAG release-3.4.20
    GIT_SHALLOW TRUE
)
FetchContent_MakeAvailable(SDL3)

# --- SDL_image ---
# Textures imported through the asset browser : PNG, JPEG, TGA and BMP, all through the stb_image
# backend built into SDL_image rather than libpng/libjpeg, to avoid pulling in libpng, libjpeg and
# zlib as extra dependencies.
set(SDLIMAGE_INSTALL OFF CACHE BOOL "" FORCE)
set(SDLIMAGE_SAMPLES OFF CACHE BOOL "" FORCE)
set(SDLIMAGE_TESTS OFF CACHE BOOL "" FORCE)
set(SDLIMAGE_PNG_LIBPNG OFF CACHE BOOL "" FORCE)
set(SDLIMAGE_ANI OFF CACHE BOOL "" FORCE)
set(SDLIMAGE_AVIF OFF CACHE BOOL "" FORCE)
set(SDLIMAGE_BMP ON CACHE BOOL "" FORCE)
set(SDLIMAGE_GIF OFF CACHE BOOL "" FORCE)
set(SDLIMAGE_JPG ON CACHE BOOL "" FORCE)
set(SDLIMAGE_JPG_SAVE OFF CACHE BOOL "" FORCE)
set(SDLIMAGE_JXL OFF CACHE BOOL "" FORCE)
set(SDLIMAGE_LBM OFF CACHE BOOL "" FORCE)
set(SDLIMAGE_PCX OFF CACHE BOOL "" FORCE)
set(SDLIMAGE_PNM OFF CACHE BOOL "" FORCE)
set(SDLIMAGE_QOI OFF CACHE BOOL "" FORCE)
set(SDLIMAGE_SVG OFF CACHE BOOL "" FORCE)
set(SDLIMAGE_TGA ON CACHE BOOL "" FORCE)
set(SDLIMAGE_TIF OFF CACHE BOOL "" FORCE)
set(SDLIMAGE_WEBP OFF CACHE BOOL "" FORCE)
set(SDLIMAGE_XCF OFF CACHE BOOL "" FORCE)
set(SDLIMAGE_XPM OFF CACHE BOOL "" FORCE)
set(SDLIMAGE_XV OFF CACHE BOOL "" FORCE)

# None of its submodules (libavif, libjxl, libtiff, aom, dav1d, ...) are needed : the formats kept above
# are all decoded by the stb_image backend built into SDL_image, not by the vendored libraries. Skipping them also avoids their very long checkout
# paths, which exceed Windows' default path length limit during git submodule checkout.
FetchContent_Declare(
    SDL_image
    GIT_REPOSITORY https://github.com/libsdl-org/SDL_image.git
    GIT_TAG release-3.4.8
    GIT_SHALLOW TRUE
    GIT_SUBMODULES ""
)
FetchContent_MakeAvailable(SDL_image)

# --- SDL_mixer ---
# Music and sounds (src/audio). Only its built-in decoders : WAV/AIFF natively, MP3 and FLAC through
# dr_libs, Ogg Vorbis through stb_vorbis, none of which need an external library. Everything that
# would (libFLAC, mpg123, libvorbis, opus, wavpack, MOD, MIDI...) is off, and so are its submodules.
set(SDLMIXER_INSTALL OFF CACHE BOOL "" FORCE)
set(SDLMIXER_TESTS OFF CACHE BOOL "" FORCE)
set(SDLMIXER_EXAMPLES OFF CACHE BOOL "" FORCE)
set(SDLMIXER_VENDORED OFF CACHE BOOL "" FORCE)
set(SDLMIXER_DEPS_SHARED OFF CACHE BOOL "" FORCE)
set(SDLMIXER_FLAC_LIBFLAC OFF CACHE BOOL "" FORCE)
set(SDLMIXER_MP3_MPG123 OFF CACHE BOOL "" FORCE)
set(SDLMIXER_VORBIS_VORBISFILE OFF CACHE BOOL "" FORCE)
set(SDLMIXER_VORBIS_TREMOR OFF CACHE BOOL "" FORCE)
set(SDLMIXER_GME OFF CACHE BOOL "" FORCE)
set(SDLMIXER_MOD OFF CACHE BOOL "" FORCE)
set(SDLMIXER_MIDI OFF CACHE BOOL "" FORCE)
set(SDLMIXER_OPUS OFF CACHE BOOL "" FORCE)
set(SDLMIXER_WAVPACK OFF CACHE BOOL "" FORCE)

FetchContent_Declare(
    SDL_mixer
    GIT_REPOSITORY https://github.com/libsdl-org/SDL_mixer.git
    GIT_TAG release-3.2.4
    GIT_SHALLOW TRUE
    GIT_SUBMODULES ""
)
FetchContent_MakeAvailable(SDL_mixer)

# --- miniz ---
# Vendored directly (third-parties/miniz/miniz.{c,h}, from the upstream release archive)
# rather than fetched/built as a full external project: it's just two files, no build
# system of its own to manage. Used to inflate the zlib-compressed Tiled map layers.
add_library(miniz STATIC miniz/miniz.c)
target_include_directories(miniz PUBLIC miniz)

# --- ufbx ---
# FBX and OBJ loader (src/model) : a single C source/header pair, MIT / public domain, rather than Assimp
# (a large C++ project supporting dozens of formats) — in line with the project's preference for C
# and a minimal footprint. Same
# SOURCE_SUBDIR trick as volk/VMA below : only the two files are used, not its own build.
FetchContent_Declare(
    ufbx
    GIT_REPOSITORY https://github.com/ufbx/ufbx.git
    GIT_TAG v0.22.0
    GIT_SHALLOW TRUE
    SOURCE_SUBDIR pireneus-no-cmake
)
FetchContent_MakeAvailable(ufbx)

add_library(ufbx STATIC ${ufbx_SOURCE_DIR}/ufbx.c)
target_include_directories(ufbx PUBLIC ${ufbx_SOURCE_DIR})

# --- cgltf ---
# glTF 2.0 loader for the asset browser (the usual format for PBR models, which ufbx doesn't read) : a
# single C header, MIT. Its implementation is compiled once, in src/model/model.c.
FetchContent_Declare(
    cgltf
    GIT_REPOSITORY https://github.com/jkuhlmann/cgltf.git
    GIT_TAG v1.15
    GIT_SHALLOW TRUE
    SOURCE_SUBDIR pireneus-no-cmake
)
FetchContent_MakeAvailable(cgltf)

add_library(cgltf INTERFACE)
target_include_directories(cgltf INTERFACE ${cgltf_SOURCE_DIR})

# --- tinyexr ---
# OpenEXR reader for HDR skyboxes (src/assets/hdr_image.c), BSD-3. Its implementation is C++, compiled
# once in src/assets/tinyexr_impl.cpp against the vendored miniz above (no zlib, no extra dependency).
FetchContent_Declare(
    tinyexr
    GIT_REPOSITORY https://github.com/syoyo/tinyexr.git
    GIT_TAG v1.0.9
    GIT_SHALLOW TRUE
    GIT_SUBMODULES ""
    SOURCE_SUBDIR pireneus-no-cmake
)
FetchContent_MakeAvailable(tinyexr)

add_library(tinyexr INTERFACE)
target_include_directories(tinyexr INTERFACE ${tinyexr_SOURCE_DIR})
target_link_libraries(tinyexr INTERFACE miniz)

# --- Box2D / Box3D ---
# 2D and 3D rigid body physics (src/physics), MIT, both plain C17 from the same author and with the
# same API shape. SOURCE_SUBDIR trick again : their own CMake projects build samples and force a static
# CRT, so the sources are compiled here as ordinary static libraries instead.
FetchContent_Declare(
    box2d
    GIT_REPOSITORY https://github.com/erincatto/box2d.git
    GIT_TAG v3.2.0
    GIT_SHALLOW TRUE
    SOURCE_SUBDIR pireneus-no-cmake
)
FetchContent_MakeAvailable(box2d)

file(GLOB box2d_sources CONFIGURE_DEPENDS ${box2d_SOURCE_DIR}/src/*.c)
add_library(box2d STATIC ${box2d_sources})
target_include_directories(box2d PUBLIC ${box2d_SOURCE_DIR}/include PRIVATE ${box2d_SOURCE_DIR}/src)
set_target_properties(box2d PROPERTIES C_STANDARD 17 C_STANDARD_REQUIRED YES C_EXTENSIONS YES)
# The 8-wide contact solver (chosen at runtime on CPUs that have AVX2) is the one file built for AVX2.
if(MSVC)
    set_source_files_properties(${box2d_SOURCE_DIR}/src/contact_solver_w8.c PROPERTIES COMPILE_OPTIONS /arch:AVX2)
else()
    set_source_files_properties(${box2d_SOURCE_DIR}/src/contact_solver_w8.c PROPERTIES COMPILE_OPTIONS "-mavx2;-mfma")
endif()

FetchContent_Declare(
    box3d
    GIT_REPOSITORY https://github.com/erincatto/box3d.git
    GIT_TAG v0.1.0
    GIT_SHALLOW TRUE
    SOURCE_SUBDIR pireneus-no-cmake
)
FetchContent_MakeAvailable(box3d)

file(GLOB box3d_sources CONFIGURE_DEPENDS ${box3d_SOURCE_DIR}/src/*.c)
add_library(box3d STATIC ${box3d_sources})
target_include_directories(box3d PUBLIC ${box3d_SOURCE_DIR}/include PRIVATE ${box3d_SOURCE_DIR}/src)
set_target_properties(box3d PROPERTIES C_STANDARD 17 C_STANDARD_REQUIRED YES C_EXTENSIONS YES)

set(PIRENEUS_THIRDPARTIES_INCLUDES ${sdl3_SOURCE_DIR}/include)

# --- Vulkan-Headers ---
# Only the headers/types: Vulkan functions are loaded at runtime by volk (below), no Vulkan SDK /
# loader import library required.
# Vulkan compiles on every platform but UWP (on Windows, both backends are in the same binary and
# one is chosen at runtime, see src/renderer/renderer.c).
FetchContent_Declare(
    Vulkan-Headers
    GIT_REPOSITORY https://github.com/KhronosGroup/Vulkan-Headers.git
    GIT_TAG vulkan-sdk-1.4.357.0
    GIT_SHALLOW TRUE
)
FetchContent_MakeAvailable(Vulkan-Headers)

# --- volk ---
# Vulkan meta-loader : loads every Vulkan entry point at runtime. Initialized from SDL's own
# vkGetInstanceProcAddr (volkInitializeCustom, see vulkan_renderer.c) so SDL stays the one place
# that locates the Vulkan loader library. Same tag as Vulkan-Headers above, so the generated
# function list matches the headers exactly.
# SOURCE_SUBDIR points at a directory that doesn't exist on purpose : FetchContent then only
# downloads the sources, without add_subdirectory()-ing volk's own CMakeLists.txt (which looks
# for a Vulkan SDK / find_package(Vulkan)). A one-file static library is declared by hand instead,
# against the fetched Vulkan-Headers target.
FetchContent_Declare(
    volk
    GIT_REPOSITORY https://github.com/zeux/volk.git
    GIT_TAG vulkan-sdk-1.4.357.0
    GIT_SHALLOW TRUE
    SOURCE_SUBDIR pireneus-no-cmake
)
FetchContent_MakeAvailable(volk)

add_library(volk STATIC ${volk_SOURCE_DIR}/volk.c)
target_include_directories(volk PUBLIC ${volk_SOURCE_DIR})
target_link_libraries(volk PUBLIC Vulkan-Headers)

# --- VulkanMemoryAllocator (VMA) ---
# Header-only (vk_mem_alloc.h) ; its implementation is compiled once, in C++, by
# src/renderer/vulkan/vulkan_memory_allocator.cpp. Same SOURCE_SUBDIR trick as volk : only the
# header is needed, not VMA's own CMake project. Its Vulkan functions come from volk
# (vmaImportVulkanFunctionsFromVolk), so neither the static nor the dynamic built-in loading paths
# are used : both are disabled for every translation unit that includes the header (an INTERFACE
# definition, so the C side and the C++ implementation always agree).
FetchContent_Declare(
    VulkanMemoryAllocator
    GIT_REPOSITORY https://github.com/GPUOpen-LibrariesAndSDKs/VulkanMemoryAllocator.git
    GIT_TAG v3.5.0
    GIT_SHALLOW TRUE
    SOURCE_SUBDIR pireneus-no-cmake
)
FetchContent_MakeAvailable(VulkanMemoryAllocator)

add_library(vma INTERFACE)
target_include_directories(vma INTERFACE ${vulkanmemoryallocator_SOURCE_DIR}/include)
target_compile_definitions(vma INTERFACE VMA_STATIC_VULKAN_FUNCTIONS=0 VMA_DYNAMIC_VULKAN_FUNCTIONS=0)
target_link_libraries(vma INTERFACE volk)

# --- Dear ImGui ---
# Immediate-mode UI, used by the launcher, the editor and the tools through src/imgui (the
# pireneus_imgui library, see the root CMakeLists.txt). The docking branch, so editor panels can be
# docked around the viewport. ImGui ships no CMake project : its core sources plus the official SDL3
# platform backend (input, cursors, clipboard) are compiled into a static library here. Rendering
# does NOT use an official renderer backend (imgui_impl_vulkan/imgui_impl_dx12) : src/imgui renders
# ImGui's draw lists through the engine's own renderer.h, so it works on every RHI backend as-is.
FetchContent_Declare(
    imgui
    GIT_REPOSITORY https://github.com/ocornut/imgui.git
    GIT_TAG v1.92.9b-docking
    GIT_SHALLOW TRUE
)
FetchContent_MakeAvailable(imgui)

add_library(imgui STATIC
    ${imgui_SOURCE_DIR}/imgui.cpp
    ${imgui_SOURCE_DIR}/imgui_draw.cpp
    ${imgui_SOURCE_DIR}/imgui_tables.cpp
    ${imgui_SOURCE_DIR}/imgui_widgets.cpp
    ${imgui_SOURCE_DIR}/imgui_demo.cpp
)

# On UWP, src/imgui feeds ImGui from the platform layer's own events.
if(NOT PIRENEUS_UWP)
    target_sources(imgui PRIVATE ${imgui_SOURCE_DIR}/backends/imgui_impl_sdl3.cpp)
endif()
target_include_directories(imgui PUBLIC ${imgui_SOURCE_DIR} ${imgui_SOURCE_DIR}/backends)
target_compile_definitions(imgui PUBLIC IMGUI_DISABLE_OBSOLETE_FUNCTIONS)
target_link_libraries(imgui PUBLIC SDL3-static)

# --- ImGuizmo ---
# 3D transform gizmo (translate/rotate/scale) drawn with Dear ImGui, MIT. Used by
# examples/scene-editor. Pinned to a commit : the project's tags stop at 1.83 (2021), long before
# the Dear ImGui 1.92 API changes it has since been updated for. Its matrices are float[16] in the
# glm/OpenGL memory layout (column-major), i.e. the transpose of the engine's row-major Mat4.
FetchContent_Declare(
    imguizmo
    GIT_REPOSITORY https://github.com/CedricGuillemet/ImGuizmo.git
    GIT_TAG 105e4fde45e4e74aae8973d98750820ef7aca7e2
    SOURCE_SUBDIR pireneus-no-cmake
)
FetchContent_MakeAvailable(imguizmo)

add_library(imguizmo STATIC ${imguizmo_SOURCE_DIR}/src/ImGuizmo.cpp)
target_include_directories(imguizmo PUBLIC ${imguizmo_SOURCE_DIR}/src)
target_link_libraries(imguizmo PUBLIC imgui)

# --- Lucide icon font ---
# Open source icon font (ISC license), merged into Dear ImGui's font by examples/scene-editor for
# its toolbar/primitive icons (it has dedicated box/cone/cylinder/torus and move-3d/rotate-3d/
# scale-3d glyphs). Fetched as the npm release archive, which ships the TTF plus a CSS file mapping
# icon names to codepoints ; codepoints are specific to a Lucide version, so this one is pinned by
# hash and the editor's ICON_* defines were read from this exact version's lucide.css.
FetchContent_Declare(
    lucide
    URL https://registry.npmjs.org/lucide-static/-/lucide-static-1.59.0.tgz
    URL_HASH SHA256=39e2f835692c193233ac7bcdf34eb0cc2da0f839fd9f3674c34d2751bccf37f0
    SOURCE_SUBDIR pireneus-no-cmake
)
FetchContent_MakeAvailable(lucide)

set(PIRENEUS_LUCIDE_FONT ${lucide_SOURCE_DIR}/font/lucide.ttf PARENT_SCOPE)
set(PIRENEUS_LUCIDE_LICENSE ${lucide_SOURCE_DIR}/LICENSE PARENT_SCOPE)

# --- Slang ---
# Provides the "slangc" CLI tool, which compiles the shaders of src/renderer/shaders/*.slang to SPIR-V
# for Vulkan, and to HLSL text for DX12, that dxc then compiles (see cmake/pireneus_shaders.cmake ;
# Slang's own DXIL backend needs dxcompiler.dll, which its release archives don't bundle).
# Fetched as a prebuilt binary release : building Slang needs Python and several submodules, for
# only its CLI. Windows-only for now, like dxc : Linux/macOS releases exist, but their URLs aren't
# wired up until those platforms are brought up on a machine where that's verifiable.
if(WIN32)
    FetchContent_Declare(
        slang
        URL https://github.com/shader-slang/slang/releases/download/v2026.18/slang-2026.18-windows-x86_64.zip
        URL_HASH SHA256=6ffa4827b519fd0a85b38407049d87ab0c1f045fe2289cb1e6831f965169f8a1
    )
    FetchContent_MakeAvailable(slang)

    # slang_SOURCE_DIR (set above by FetchContent_MakeAvailable) is scoped to this subdirectory :
    # it doesn't have a CMakeLists.txt for add_subdirectory() to pick up as a target, so unlike
    # SDL3/Vulkan-Headers (visible everywhere as CMake targets) it needs to be propagated to the
    # root scope explicitly for cmake/pireneus_shaders.cmake to use it.
    set(PIRENEUS_SLANGC_EXECUTABLE ${slang_SOURCE_DIR}/bin/slangc.exe PARENT_SCOPE)
endif()

# DirectX 12 needs no third-party fetch: d3d12.h/dxgi1_6.h, the d3d12/dxgi/dxguid import
# libraries and the dxc shader compiler all ship with the Windows SDK (see the WIN32 branch in
# the root CMakeLists.txt). It only compiles on Windows, where those headers exist.
