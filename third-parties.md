# Third-party libraries

Pireneus is built on the projects below. miniz is vendored in `third-parties/miniz/`. Every other
project is downloaded at configure time by CMake `FetchContent` (see `third-parties/CMakeLists.txt`),
at the version shown. The full text of each license ships with the downloaded sources, in
`<build>/_deps/<name>-src/`.

All of these licenses are permissive. They usually require **keeping the copyright notice and the
license text** in source and binary distributions.

## Compiled into the engine

| Library | Version | License | Purpose |
| --- | --- | --- | --- |
| [SDL3](https://github.com/libsdl-org/SDL) | 3.4.20 | zlib | Window, input, files, platform layer |
| [SDL_image](https://github.com/libsdl-org/SDL_image) | 3.4.8 | zlib | Image loading (built-in stb_image-based decoder) |
| [SDL_mixer](https://github.com/libsdl-org/SDL_mixer) | 3.2.4 | zlib | Audio: mixing, 3D voices, buses |
| [miniz](https://github.com/richgel999/miniz) | 3.1.2 | MIT | zlib decompression for Tiled maps and EXR files |
| [ufbx](https://github.com/ufbx/ufbx) | 0.22.0 | MIT or public domain (Unlicense), at your choice | FBX and OBJ import |
| [cgltf](https://github.com/jkuhlmann/cgltf) | 1.15 | MIT | glTF 2.0 and GLB import |
| [tinyexr](https://github.com/syoyo/tinyexr) | 1.0.9 | BSD 3-Clause | OpenEXR skies |
| [Box2D](https://github.com/erincatto/box2d) | 3.2.0 | MIT | 2D physics |
| [Box3D](https://github.com/erincatto/box3d) | 0.1.0 | MIT | 3D physics |
| [Vulkan-Headers](https://github.com/KhronosGroup/Vulkan-Headers) | SDK 1.4.357.0 | Apache 2.0 or MIT | Vulkan headers |
| [volk](https://github.com/zeux/volk) | SDK 1.4.357.0 | MIT | Vulkan function loading |
| [Vulkan Memory Allocator](https://github.com/GPUOpen-LibrariesAndSDKs/VulkanMemoryAllocator) | 3.5.0 | MIT | GPU memory management on Vulkan |
| [Dear ImGui](https://github.com/ocornut/imgui) (docking branch) | 1.92.9b | MIT | UI of the menu, the editor and the tools |
| [ImGuizmo](https://github.com/CedricGuillemet/ImGuizmo) | commit `105e4fd` | MIT | Editor transform gizmos |

Bundled inside those libraries, and therefore in the binary:

| Component | Bundled by | License |
| --- | --- | --- |
| [stb_image](https://github.com/nothings/stb) | SDL_image | MIT or public domain, at your choice |
| [stb_vorbis](https://github.com/nothings/stb) | SDL_mixer | MIT or public domain, at your choice |
| [dr_libs](https://github.com/mackron/dr_libs) (dr_mp3, dr_flac) | SDL_mixer | MIT-0 or public domain, at your choice |
| OpenEXR code (Industrial Light & Magic) | tinyexr | BSD 3-Clause |

## Assets shipped with the executable

| Asset | Version | License | Purpose |
| --- | --- | --- | --- |
| [Lucide](https://lucide.dev/) icon font (`lucide-static` package) | 1.59.0 | ISC | UI icons. The license is copied next to the font (`resources/fonts/lucide-LICENSE.txt`). |

## Build tools (not distributed)

| Tool | Version | License | Purpose |
| --- | --- | --- | --- |
| [Slang](https://github.com/shader-slang/slang) (`slangc`) | 2026.18 | Apache 2.0 with LLVM exception | Compiles shaders to SPIR-V and HLSL |
| DirectX Shader Compiler (`dxc`, part of the Windows SDK) | the SDK's | Windows SDK license | Compiles HLSL to DXIL |
| Windows SDK (Direct3D 12, DXGI, XAudio2, C++/WinRT) | the installed one | Windows SDK license | System APIs on Windows and UWP |

## Example content

| Asset | Author | License |
| --- | --- | --- |
| `resources/2D-test/roguelikeSheet_transparent.png` | [Kenney](https://kenney.nl/), *Roguelike* pack | CC0 1.0 (public domain) |

### Downloaded separately (not in the repository)

| Asset | Author | License | Used by |
| --- | --- | --- | --- |
| [Amazon Lumberyard Bistro](https://developer.nvidia.com/orca/amazon-lumberyard-bistro) v5.2, in `resources/3D-test/Bistro_v5_2/` | Amazon Lumberyard, NVIDIA ORCA | CC BY 4.0 (its HDR sky: CC0, from Poly Haven) | The `demos/Bistro` project |

The other files in `resources/` are Isabit Studio content. The Blender test scenes and the primitives
are covered by the repository's MIT license.

The Isabit Studio logos (`resources/logo_light.png`, `resources/logo_dark.png`) are **not**: they are
the intellectual property of Isabit Studio, all rights reserved (see [LICENSE](LICENSE)).
