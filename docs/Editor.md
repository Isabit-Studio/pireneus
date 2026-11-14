# Editor

The editor (`examples/scene-editor/`) opens on a project, from the main menu or with
`--project <folder>`. It is written in simple C++, on Dear ImGui and ImGuizmo:

| File | Contents |
| --- | --- |
| `main.cpp` | Lifecycle, 3D view, camera, picking, rendering, undo, play mode, shortcuts |
| `panels.cpp` | Menus and panels: Tools, Hierarchy, Inspector, Project Settings... |
| `asset_browser.cpp` | Asset browser |
| `level_editor.cpp` | Level design tools (see [Level editing](Level-Editing)) |
| `editor.h` | What those files share |

## Layout

- **Menu bar**: File, Edit, View, Object, with Play, Pause and Step in the middle (Ctrl+P).
- **Panels**, dockable to the edges:
  - Primitives: palette of the built-in primitives;
  - Tools: objects mode snapping, grid, 2D view, sky and sun, controls help;
  - Level and Materials: level editing;
  - 2D Views;
  - Hierarchy: Ctrl for multiple selection, eye icon to hide;
  - Inspector: Transform, Model or Mesh, Sprite, Material, Rigid Body, Character Controller,
    Audio Source, Add Component;
  - Project Settings;
  - Assets.
- **Asset browser**:
  - folder tree, thumbnails, search;
  - import (an image can become a Texture, a Sprite or a Skybox);
  - drag and drop from the system, into the view or the inspector;
  - rename, recycle bin, new folder, sound preview.

The view renders the way the project says: photorealistic, path traced or forward.

## 3D view controls

| Action | Control |
| --- | --- |
| Select | Left click; Ctrl to add or remove; drag on empty space for a box selection |
| Fly (noclip) | Hold right click, then mouse and W/A/S/D + Q/E (Z/Q/S/D + A/E on AZERTY); Shift = faster; wheel = speed |
| Orbit | Alt + left drag |
| Pan | Middle drag |
| Zoom | Wheel |
| Frame the selection | F |
| Move, rotate, scale | T, R, E (physical positions) |
| Temporary snapping (objects mode) | Shift while dragging the gizmo |
| Delete, duplicate | Delete, Ctrl+D |
| Undo, redo | Ctrl+Z, Ctrl+Y (or Ctrl+Shift+Z) |
| Save | Ctrl+S |
| Play, stop | Ctrl+P |

The positional keys are grouped in `#define KEY_*` at the top of `main.cpp` and `level_editor.cpp`.

## Internals

- **3D view**: a full-screen ImGui window, transparent and always at the back.
  - ImGuizmo needs a real window's draw list, and this window also receives camera and picking input.
  - Panels dock around it, thanks to `DockSpaceOverViewport(PassthruCentralNode)`, whose central node
    lets the mouse through.
  - `DockSpaceOverViewport()` must be called **before** `Begin("##viewport")`. In the other order, the
    view covers the dockspace host and the docking targets no longer appear.
- **Camera**: framed on the dockspace's central node.
  - The perspective uses that area's aspect ratio.
  - A clip-space scale and offset then maps it to that sub-rectangle.
  - The optical center stays in the middle of the visible area, and picking, gizmo and grid stay
    consistent.
- **Picking**: the ray is transformed into each object's local space and tested against its bounding
  box. It is then tested against the triangles of a CPU copy (Möller–Trumbore), or against the faces
  for editable meshes.
- **Undo**: snapshots of the whole scene, one per gesture and at most one per frame (a scene is copied
  with `=`). Editable meshes use copy-on-write, see [Level editing](Level-Editing).
- **Play mode**: see [Physics and play mode](Physics-and-Play-Mode).
