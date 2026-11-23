# Baked lighting

- Code: `src/bake/` (the baker), `examples/scene-editor/lighting.cpp` (the editor's side).
- Used by both [3D pipelines](3D-Rendering), and what Forward+ is meant to rely on.

The baker computes on the CPU, on every core, by ray tracing the static geometry through a BVH:

| What | For | How |
| --- | --- | --- |
| **Lightmaps** | Editable meshes (level geometry) | Each face is a chart; charts are packed into pages. Every texel gathers the direct light of the baked lights, and the indirect light of all the lights and of the sky (path tracing, several bounces). |
| **Probe volume** | Everything without a lightmap: models, anything moving | An adaptive grid: coarse cells everywhere, and a brick of 5 x 5 x 5 probes four times denser where there's geometry. Each probe holds L1 spherical harmonics of irradiance; probes buried in geometry are left out. |
| **Occlusion culling** | Every static object | The space is cut into cells. For each cell, rays sample which static objects are potentially visible from it. |
| **Reflection probes** | Reflections | Captured by the renderer itself, after the rest: six faces, read back, turned into a prefiltered panorama. |

Everything is stored as irradiance / PI, the convention of the shaders' environment lighting.

## In the editor

- **Lighting > Bake lighting** bakes the open scene, in the background, with its progress shown. The
  reflection probes are captured right after.
- **Lighting > Capture reflection probes** captures them alone.
- **Lighting > Use baked lighting** and **Occlusion culling** turn each off, to compare.
- The result is saved next to the scene, `Assets/Scenes/<scene>.bake`, and loaded with it.
- Settings: **Project Settings > Lighting**: texels per meter, page size, samples, bounces, probe spacing,
  rays per probe, culling cell size.

What gets baked:

- Editable meshes and models, unless hidden, 2D, or moving (dynamic or kinematic bodies, characters).
- Tool materials (nodraw, clip) neither block nor bounce light.

## Light modes

The Light component has a mode:

| Mode | Direct light | Bounces | Cost at runtime |
| --- | --- | --- | --- |
| **Realtime** | Every frame | None baked | Full |
| **Mixed** (default) | Every frame, shadows included | Baked | Direct light only |
| **Baked** | Baked into lightmaps and probes | Baked | None: the light isn't drawn at all once baked |

The sun is always mixed: its direct light stays realtime, and its bounces are baked.

## Objects and their data

- An object's baked data is found by its name's hash (`bake_key()`).
- A lightmap only applies if the mesh still has the same vertex count. After editing a mesh, bake again.
- Occlusion culling only hides objects from the camera: their shadows are still drawn.

## API

```c
BakeJob* job = bake_start(meshes, mesh_count, lights, light_count, &settings);  // copies its inputs
float progress = bake_progress(job, &stage);
BakeResult* result = bake_finish(job);        // when bake_done(job) ; NULL if cancelled
bake_save(result, path);                      // and bake_load(path)

bake_object_lightmap(result, key, vertex_count, &page, &uvs);   // + bake_page_pixels()
bake_probe_volume(result, &volume);           // the GPU buffer's contents and layout
bake_object_visible(result, key, point);
```

`BakeMesh` gives a static object's triangles, in its GPU mesh's vertex order (lightmap UVs are per
vertex), and the chart of each vertex. `EditMeshGpu` keeps those on the CPU for this.
