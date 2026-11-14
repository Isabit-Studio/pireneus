# Dear ImGui

`src/imgui/pireneus_imgui.{h,cpp}` makes up the `pireneus_imgui` library. Only the tools that use Dear
ImGui link it: the launcher, the editor and the tilemap editor.

- **Version**: Dear ImGui's *docking* branch (dockable panels). It is fetched with `FetchContent` and
  built with its official SDL3 platform backend.
- **Rendering**: none of ImGui's official renderer backends is used (`imgui_impl_vulkan` and
  `imgui_impl_dx12` would require exposing each backend's internals). Instead, ImGui's draw lists are
  converted to the RHI's `draw_vertices()` and `set_scissor()`: indices are expanded on the CPU and
  colors are converted. It therefore works on any backend.
- **Textures**:
  - An `ImTextureID` is an engine `Texture*` (`pireneus_imgui_texture()`), so any texture can be
    shown with `ImGui::Image`.
  - ImGui's own textures (the font atlas, 1.92 API) are created with `create_texture_from_pixels`.
  - An update recreates the whole texture, after `wait_renderer_idle()`.
- **Input**: native events arrive through `set_event_callback()`. With SDL, they go to the official
  backend; on UWP, they are translated by hand.

## Order within a frame

```c
pireneus_imgui_new_frame();
// ... ImGui calls
pireneus_imgui_prepare_frame();  // before begin_frame() : ImGui::Render() and texture uploads
begin_frame();
// ... game drawing
pireneus_imgui_draw();           // last, on top
end_frame();
```

## Fonts and icons

ImGui's default font is merged with the **Lucide** icon font (ISC), in `MergeMode`
(`examples/launcher/editor_fonts.h`). Icons can thus be mixed with text in any label
(`ICON_FOLDER " Browse"`).

Lucide's codepoints change from one version to the next. So:

- the version is pinned by hash in `third-parties/CMakeLists.txt`;
- the `ICON_*` defines were read from that exact version's `lucide.css`.

## ImGuizmo

[ImGuizmo](https://github.com/CedricGuillemet/ImGuizmo) draws the editor's gizmos. Its matrices are
column-major (like glm), so it is given the transposes of the engine's row-major `Mat4`s
(`mat4_transpose`).
