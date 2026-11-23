# Demo scenes

Demo projects test the engine on real content, too large to live in the repository. They are ordinary
projects, in `demos/`, and the main menu lists them under **Projects > Demos**. They're opened where they
are, in the source tree: `resources/` is copied next to the executable at every build, but not their
assets (see `cmake/copy_resources.cmake`).

## Bistro

[Amazon Lumberyard Bistro](https://developer.nvidia.com/orca/amazon-lumberyard-bistro) (NVIDIA ORCA,
CC BY 4.0): the exterior of a Parisian street at dusk.

- 2.8 million triangles, 132 materials, 400 textures (1.4 GB of BC-compressed DDS).
- The project: `demos/Bistro`, its scene `Assets/Scenes/Bistro.scene`.
- The scene:
  - the model;
  - 43 point lights placed on the model's own lamps: 28 street lamps (those around the square cast
    shadows), 5 hanging lanterns by the bistro's terrace, and 10 bulbs under its awnings;
  - a low sun, the procedural sky and its clouds;
  - a view down the main street, where the editor opens it.
- Deferred+ at the High preset: switch to Forward+ in **Project Settings > Rendering** to compare.

### Getting the data

The Bistro isn't in the repository (one of its files is over GitHub's 100 MB limit). Download
**Bistro_v5_2** from the ORCA page and extract it so that the model is at
`resources/3D-test/Bistro_v5_2/BistroExterior.fbx` (the folder is in `.gitignore`).

### What it relies on

- **DDS textures**, kept compressed on the GPU (BC1, BC3, BC5 with their mips), see
  [3D rendering](3D-Rendering#materials).
- The FBX's packed maps (`*_Specular`: occlusion, roughness, metalness), its DirectX-convention normal
  maps, and the base color's alpha for foliage cutouts.
- **Submeshes by region**: an FBX model is split by material and by cells of space, about a thousand
  submeshes here, each culled on its own (view, shadow cascades, light tiles). See
  [Projects, assets and scenes](Projects-Assets-and-Scenes#models-srcmodel).
- Vertices welded at import: 2.9 million rather than 8.5 million.

### Performance

Measured in the editor, Release build, 1600 x 900 window, Radeon RX 6900 XT, from the scene's view:

| Pipeline | Backend | Frames per second | GPU time |
| --- | --- | --- | --- |
| Deferred+ (High, realtime GI) | Vulkan | about 205 | 4.7 ms |
| Deferred+ (High, realtime GI) | DirectX 12 | about 135 | 5.4 ms |
| Forward+ (High, MSAA) | DirectX 12 | about 150 | 5.9 ms |

The CPU spends about 1 ms per frame on the 3D renderer. The first frame comes about 5 seconds after
opening the project: loading the model and its textures, then building the ray tracing structures.

### Limits

- The model has no collider: the play mode's character would fall through the ground.
- Lights are realtime (`mode` 0). Baking (**Lighting > Bake lighting**) works, but over the whole street
  it takes a while; the model gets the probe volume rather than lightmaps.
- Glass is opaque: the renderer has no transparency.
- Occlusion culling works per object, and the Bistro is one object: only frustum culling applies to its
  submeshes.
