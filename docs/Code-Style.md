# Code style

## Orthodox C++

The engine follows [Orthodox C++](https://bkaradzic.github.io/posts/orthodoxc++/):

- **The engine is C** (`src/`). C++ only appears where a library requires it:
  - `vulkan_memory_allocator.cpp` (VMA);
  - `tinyexr_impl.cpp`;
  - the UWP backend (C++/WinRT);
  - `pireneus_imgui.cpp`.
- **The tools** (launcher, editor) are simple, C-like C++.
  - No advanced templates, no exceptions, no RTTI, no heavy STL.
  - Plain structs, arrays and free functions.
- **No abstraction without a real need.** The RHI is a struct of function pointers, and modules expose
  functions and opaque types.

## Conventions

- **Naming**: `snake_case` functions and variables, `PascalCase` types, `UPPER_CASE` constants and
  macros. Functions take their module's prefix (`scene_`, `audio_`, `edit_mesh_`...).
- **Formatting**: braces on their own line, 4-space indentation.
- **Headers**: public headers include no third-party library. Those usable from C++ have `extern "C"`
  guards.
- **Failure**: functions that fail return 0 or `NULL`, and print a short message on standard output.
- **Logging**: `DEBUG_LOG()` is for lifecycle traces, never in a per-vertex or per-frame loop.
- **Memory**: `SDL_malloc` and `SDL_free`, with fixed sizes where reasonable (`SCENE_MAX_OBJECTS`...).

## Comments

- **What they say**: comments are in English and explain **why**: a constraint, a choice, a pitfall
  avoided. They don't paraphrase the code or tell the history of changes.
- **Module descriptions**: every public header starts with a description of the module and its
  contract.
- **Mirrored structures**: structures shared with shaders are commented as mirrors, because changing
  one means changing the other.

## Keyboard

Use scancodes for anything positional and ImGui keys for mnemonic shortcuts. See
[Platforms and input](Platforms-and-Input).

## Checking a change

There are no automated tests yet. Before committing:

1. Build in Debug with no warning.
2. Run every example and the editor, on both Vulkan **and** DirectX 12
   (`PIRENEUS_RENDERER=vulkan|dx12`).
3. Close cleanly, and check in Debug that no DX12 debug layer error is printed.
