# Building and running

## Requirements

For now the engine builds on **Windows** (see [Platforms and input](Platforms-and-Input) about Linux
and macOS).

- **CMake 4.0** or later.
- **Visual Studio** with the "Desktop development with C++" workload (tested with Visual Studio 2026).
  MSVC compiles both the C and the C++.
- A recent **Windows SDK** (tested with 10.0.26100), for the Direct3D 12 headers and `dxc.exe`, which
  compiles the DX12 shaders. CMake looks for `dxc.exe` in the SDK and stops with an error if it can't
  find it.
- **Git**, for `FetchContent`.
- **Ninja** (optional, required for UWP).

No Vulkan SDK is needed: the headers are downloaded, and the driver's Vulkan loader is found at
runtime. Without the SDK there are no validation layers.

## Dependencies

The first configure downloads every dependency (SDL3, SDL_image, SDL_mixer, Dear ImGui, Box2D, Box3D,
Slang...), which takes a few minutes.

- They are declared in `third-parties/CMakeLists.txt`.
- They are listed with their licenses in `third-parties.md`.

Deleting the build folder means downloading everything again.

## Desktop build

In a terminal with the MSVC environment loaded ("x64 Native Tools Command Prompt", or `vcvars64.bat`):

```bash
cmake -S . -B build -G Ninja -DCMAKE_BUILD_TYPE=Debug
```

```bash
cmake --build build
```

The result is `build/examples/pireneus-examples.exe`, with the compiled `shaders/` and the
`resources/` copied next to it.

The configuration works differently depending on the generator:

- **Visual Studio** (no `-G`): pick the configuration at build time
  (`cmake --build build --config Release`). The executable is in `build/examples/<Config>/`.
- **Ninja**: `--config` is ignored, so use one build folder per configuration.

### CMake options

| Option | Default | Effect |
| --- | --- | --- |
| `PIRENEUS_RENDERER` | `dx12` | Backend selected by default at startup (`vulkan` or `dx12`). Both are always compiled on Windows. |
| `PIRENEUS_UWP` | `OFF` | UWP build, see [UWP](UWP). |

In Debug, the `DEBUG` macro is defined for the whole engine, which turns `DEBUG_LOG()` on
(`src/debug.h`).

## Running

```bash
build/examples/pireneus-examples.exe
```

1. On Windows, a dialog first asks for the graphics backend. Enter, Escape or closing the dialog picks
   the default.
2. The main menu opens. Create or open a project (which opens the editor), or start an
   [example](Examples).

| Argument / variable | Effect |
| --- | --- |
| `--project <folder>` | Opens this project in the editor |
| `--example <id>` or `PIRENEUS_EXAMPLE=<id>` | Starts an example directly |
| `PIRENEUS_RENDERER=vulkan` or `dx12` | Picks the backend without the dialog (handy for scripts) |

Files the executable writes:

- Each example saves its window layout to `imgui_<id>.ini`, next to the executable.
- Recent projects are saved to `recent_projects.json`, in the user's data folder
  (`%APPDATA%\Pireneus\Engine\` on Windows).

## UWP build

See [UWP](UWP).
