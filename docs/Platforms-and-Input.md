# Platforms and input

## Platform status

| Platform | Status |
| --- | --- |
| Windows (Win32) | Supported: SDL3, Vulkan and DirectX 12 |
| Windows (UWP) | Supported: custom CoreWindow backend, DirectX 12 (see [UWP](UWP)) |
| Linux, macOS | Planned by the architecture (SDL3, Vulkan). They don't build yet: shader compilation is only wired up on Windows |
| Android, iOS, consoles | Future |

Graphics APIs planned in the long run: Metal, DirectX 11, OpenGL, and the consoles' APIs.

## SDL3

The engine relies on SDL3 rather than rewriting a layer per system: window, events, keyboard, mouse,
files, dialogs. The implementation is in `src/platforms/sdl/`. UWP is the exception, since SDL3 no
longer supports it.

## Window (`platforms_window.h`)

- **Window**: `create_window`, `destroy_window`, `get_window_size`, `set_window_title`, and
  `handle_events()` (0 when it's time to quit).
- **`set_event_callback()`** receives every native event (a `const SDL_Event*` with SDL). Only code
  already tied to the platform layer reads it; that's how Dear ImGui gets its input.
- **`set_redraw_callback()` and `call_redraw_callback()`**: all drawing goes through this callback. It
  prevents reentrancy (DXGI can pump the message queue during `Present`), and keeps drawing while the
  window is being resized.
- **`set_file_drop_callback()`**: files dragged onto the window from the system.

## Keyboard by scancode (`platforms_input.h`)

`Scancode`s (`SCANCODE_W`...) name a key by its **physical position**, whatever the keyboard layout:
`SCANCODE_W` is "W" on QWERTY and "Z" on AZERTY.

- Names follow US QWERTY.
- Values are USB HID usage IDs, the same as SDL's (checked at compile time).

Functions:

- `input_is_key_down(scancode)`: current state.
- `input_key_label(scancode)`: the key's label on the active layout, for on-screen hints ("Z" for
  `SCANCODE_W` on AZERTY).

Convention:

- **Scancodes** for anything **positional**: WASD/ZQSD movement, grouped tool keys.
- **Layout-dependent keys** (ImGui's) for **mnemonic** shortcuts: Ctrl+Z, Ctrl+S, F.

The API doesn't detect key presses: callers compare states from one frame to the next when they need
to.

## Paths (`platforms_paths.h`)

- `platform_user_data_path()`: the engine's settings and caches (recent projects...).
- `platform_documents_path()`: default location of new projects.
- `platform_show_open_files_dialog()` and `platform_show_open_folder_dialog()`: file and folder
  pickers. They are asynchronous: the result arrives through a callback.

On UWP, these are the app's own folders (`LocalState`).

## Resizing

Fast interactive resizing used to crash the DX12 backend. Several safeguards are in place:

- the reentrancy guard above;
- a descriptor free list;
- a minimum interval between two `ResizeBuffers` calls;
- in *Pixel Perfect*, a render target that is only recreated in steps.
