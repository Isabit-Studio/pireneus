# Projects, assets and scenes

## Projects (`src/project`)

A project is a folder:

```
My Game/
  project.pireneus     project settings (JSON)
  Assets/
    Audio/  Models/  Scenes/  Skyboxes/  Sprites/  Textures/  Tilemaps/
```

`ProjectSettings` holds:

- **Physics**: 3D and 2D gravity, rate and substeps, default friction and restitution.
- **Collision**: up to 16 named layers and their collision matrix.
- **Character controller**: speed, sprint, jump, acceleration, air control, maximum slope, mass.
- **Rendering**:
  - the render path: `RENDER_PATH_DEFERRED_PLUS`, `RENDER_PATH_FORWARD_PLUS` or `RENDER_PATH_2D`;
  - the look: exposure, tone mapping, fog and volumetrics, reflections...;
  - the 2D path's settings: pixel perfect, its height in pixels, the background;
  - the quality: Low to Ultra, or Custom. A preset means different things to each pipeline (MSAA for
    Forward+, TAA and reflections for Deferred+), and is applied again when the render path changes.
- **Baked lighting**: the bake's settings, and whether to use it and occlusion culling (see
  [Baked lighting](Baked-Lighting)).
- **Audio**: master, music and sound volumes.
- **Application**: startup scene.

Functions:

- `project_create`, `project_open`, `project_save`.
- Recent projects (12 at most): `project_recent_list`, `project_recent_add` and
  `project_recent_remove`.

A 2D project starts with the 2D render path, a 3D project with Deferred+. Settings saved by older
versions are read too: `photoreal` (and `path_traced`) become Deferred+, `forward` becomes 2D in a 2D
project and Forward+ in a 3D one.

## Asset database (`src/assets/asset_database`)

- **Scan** of the `Assets` folder: each file is classified by type (texture, sprite, model, sound,
  sky, scene, map).
- **Import**: copies a file into the right project folder.
- **Caches**, loaded on demand: models (GPU and CPU, for picking and physics), textures, thumbnails,
  environments, sounds.
- **Built-in assets**:
  - `builtin/<name>`: the primitives of `resources/3D-test/Primitives` (cube, plane, cylinder, cone,
    sphere, ico sphere, torus).
  - `builtin/materials/dev_*`: the level editor's "dev" materials. They are generated at startup: a
    1 m tile with lines every 25 cm, in grey, light grey, dark grey, orange, blue, green, red and
    yellow.
  - `builtin/materials/tool_nodraw` and `tool_clip`: invisible in game, but they keep their collision.

Supported formats:

| Kind | Formats | Library |
| --- | --- | --- |
| Models | FBX, OBJ, glTF 2.0, GLB | ufbx, cgltf (`src/model`) |
| Images | PNG, JPEG, TGA, BMP | SDL_image (stb_image) |
| Compressed textures | DDS (BC1, BC3, BC5) | `pbr_texture.c` |
| Skies | HDR (Radiance), EXR, 8-bit images | `hdr_image.c`, tinyexr |
| Sounds | WAV, AIFF, MP3, FLAC, Ogg Vorbis | SDL_mixer |
| Maps | Tiled JSON | `src/tiled` |

### Models (`src/model`)

`model_load()` flattens a file's whole scene into triangles, in model space: right-handed, +Y up, in
meters. The result is split into submeshes (`ModelSubmesh`), with PBR materials, external or embedded
textures, and tangents. The module doesn't depend on rendering.

- Each submesh has its own box (`bounds`): the renderer culls them one by one.
- **FBX and OBJ** (ufbx):
  - each node's faces are read material by material, and their identical corners welded;
  - the pieces are then grouped by material and by cells of space (8 m, coarser if there would be
    more than 1536 submeshes): a large scene, a whole street, is drawn piece by piece rather than in full
    for every view and shadow;
  - a texture in the specular slot named `*_Specular` or `*_ORM` is read as packed occlusion,
    roughness and metalness (Lumberyard's and Falcor's convention, Unreal's);
  - the base color's alpha decides on cutouts (`alpha_from_texture`).
- **glTF**: one submesh per material, its alpha mode as the file says.

## Scenes (`src/scene`)

A scene (`Scene`) is plain data, saved as JSON (`Assets/Scenes/*.scene`). Its objects (`SceneObject`,
1024 at most) carry:

- a transform: position, Euler angles in degrees (R = Rz·Ry·Rx), scale;
- what is drawn: a model, an **editable mesh** or a sprite;
- a material that can override the model's;
- a light: point or spot, color, intensity, range, cone, shadows, and its mode (realtime, mixed, baked);
- a reflection probe: its box, its blend, its capture resolution;
- a physics body (static, kinematic or dynamic, 2D or 3D), its collider and its layer;
- an optional character controller;
- an audio source;
- two editor-only fields, which aren't saved: `hidden` and `selected`.

Every component is stored flat in the struct, so a whole scene can be copied with `=`. Undo and the
play mode's snapshot rely on this.

Editable meshes are the exception:

- they are referenced by a *handle* into a shared store (see [Level editing](Level-Editing));
- they are written inline in the scene's JSON.

The scene also holds its sky (an HDR panorama, or nothing for the procedural sky), the sun's direction
and intensity, and its procedural clouds (coverage, density, altitude, scale, wind, shadows). Its baked
lighting is saved next to it, `<scene>.bake`.

It keeps the editor's view too (`view`: the orbit camera's target, yaw, pitch and distance), written when
it's saved: a scene reopens where it was left.

A model's path is relative to the project's root, like every asset. It can lead out of the project
(`../../resources/...`): that's how the [demo scenes](Demo-Scenes) use assets too large to copy.

### Playing a scene (`scene_runtime`)

`scene_runtime` plays a scene:

- fixed-step physics, interpolated for display;
- the player drives the character controller;
- a third-person camera in 3D, an orthographic one in 2D;
- audio sources.

Objects move inside the scene it is given. A caller that wants the original back plays a copy.
