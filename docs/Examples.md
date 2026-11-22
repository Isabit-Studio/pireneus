# Examples

All the examples live in the same executable, `pireneus-examples`. Start them from the main menu or
with `--example <id>`.

Each one has its own folder in `examples/`, and exposes an `Example` descriptor (`examples/example.h`).
A "Menu" button, at the bottom right, goes back to the menu.

| Id | Title | What it shows |
| --- | --- | --- |
| `hello-triangle` | Hello Triangle | The low-level `draw_vertices()` primitive alone, with no sprite batch. |
| `2d-test` | 2D Test | A Tiled map (`resources/2D-test/`) panned by a camera, drawn by the sprite batch with culling. |
| `pixel-perfect` | Pixel Perfect | The same map, see [below](#pixel-perfect). |
| `tilemap-editor` | Tilemap Editor | A Tiled map editor, see [below](#tilemap-editor). |
| `3d-test` | 3D Test | The forward renderer on `resources/3D-test/Debug.fbx`: one directional light, a 2048×2048 shadow map, an orbiting camera. |
| `photoreal` | Photorealistic Rendering | The photorealistic renderer on the same scene, under a procedural sky, with an orbiting camera. |

### Pixel Perfect

- The map is rendered 216 pixels tall into a render target, then scaled up by a whole factor.
- The visible width follows the window's aspect ratio, so there are no side bars.
- The camera and sprites snap to virtual pixels.

### Tilemap Editor

- Layers and a tile palette.
- Mouse: paint (left click), erase (right click), pick (Shift+click), zoom.
- Undo and redo.
- Saves to `<map>_edited.json`.

The **editor** (`scene-editor`) is an `Example` too, but it opens through a project: see
[Editor](Editor).

## Adding an example

1. Create `examples/<name>/main.c` (or `.cpp`), defining a `const Example example_<name>`: id, title,
   description and callbacks.
2. Register it in three places:
   - declare it in `examples/example.h`;
   - add it to the `examples[]` array of `examples/launcher/main.cpp`;
   - add it to `add_executable` in `examples/CMakeLists.txt`.
3. Anything placed in `resources/` is copied next to the executable on every build.
