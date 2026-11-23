# Level editing (whiteboxing and greyboxing)

The editor's level design tools follow the ergonomics of **Valve's Hammer (Source 2)**:

1. Build a level out of **editable polygon meshes**.
2. Texture them with "dev" materials.
3. Play it right away: static meshes get exact collision.

## Selection modes

| Key | Mode | The gizmo acts on |
| --- | --- | --- |
| 1 | Vertices | the selected vertices |
| 2 | Edges | the selected edges |
| 3 | Faces | the selected faces |
| 4 | Meshes | mesh objects |
| 5 | Objects | every object (models, sprites, meshes) |

The keys are positional: the number row, whatever the keyboard layout. In the vertex, edge and face
modes, the selected meshes show their elements.

**Shift at the start of a gizmo drag**:

- **extrudes** the selected faces, in faces mode;
- **extends** the selected border edges with new faces, in edges mode;
- **clones** the meshes, in meshes mode (Hammer's Shift-drag);
- enables temporary snapping, in objects mode.

A gesture that creates geometry without moving anything is undone, so it leaves no duplicates behind.

## Tools

### Block tool (Shift+B)

1. Drag a rectangle on the grid, or on an axis-aligned face.
2. Adjust its height and sides with the six handles. The size is shown in meters.
3. Enter (or Space) creates the block; Escape cancels.

Shapes: box, wedge, cylinder, cone, stairs, arch, sphere. Each can be solid or hollow, with an
adjustable thickness. The block gets the active material and a static body.

### Clipping tool (Shift+X)

1. Select meshes.
2. Drag a line in the 3D view (the cutting plane goes through the camera) or in a 2D view.
3. Shift+X chooses what is kept: both sides, the front or the back. The preview is colored:
   - green: kept;
   - red: removed;
   - blue: the other part.
4. Enter cuts.

The pieces are capped. Keeping both sides creates a "(cut)" object.

### Operations (Level panel)

| Mode | Operations |
| --- | --- |
| Vertices | Merge (M), connect (splits a face between two vertices), dissolve (Delete), snap to grid, weld (with a distance) |
| Edges | Loop (L) and ring (G) selection, split, connect (with Ring: a loop cut), collapse (M), dissolve (Delete), fill holes |
| Faces | Extrude, inset, flip, bridge (a tunnel between two faces), detach, collapse (M), delete (Delete). Selection: grow, shrink, all (Ctrl+A), invert |
| Meshes | Merge, hollow (with a thickness), flip (a room seen from inside), fill holes, snap to grid, center origin |

## Grid and nudging

- **Grid size**: `[` and `]` (`^` and `$` on AZERTY) make the grid finer or coarser, from 1/32 m to
  16 m (0.25 m by default).
- **Snapping**: in the mesh modes, the grid snaps everything that is created or moved. This is
  independent of the objects mode snapping.
- **Nudging**: the arrow keys and Page Up / Page Down move the selection by one grid step, when the
  mouse is over a view.

## 2D views

Three orthographic wireframe views: top (X/Z), front (X/Y) and side (Z/Y). They open from the View
menu or the Tools panel. The panel shows one view, or all three ("All"):

- usually, the top view fills the left half, with the front and side views stacked on the right;
- in a very wide or very tall panel, the three views sit in a row or in a column.

Model wireframes are drawn from each model's unique edges, built once and cached. Edges outside a
view, or shorter than a pixel, are skipped, so dense models stay cheap to draw.

- **Editing**: select, move (Shift to extrude or clone), draw blocks and clip lines.
- **Picking**: a click picks stacked vertices and edges, as in Hammer.
- **Navigation**: the wheel zooms around the cursor; right drag, middle drag or Space + left drag pan.

## Materials and textures

- **Materials panel**: the built-in "dev" materials (colored grids, `tool_nodraw`, `tool_clip`) and
  the project's textures.
  - A click picks the **active material**.
  - A double click, the Apply button, or a drag onto a face applies it.
- **Texture mapping**: each face has its own projection.
  - It is aligned to the world or to the face.
  - It has a scale (in meters per repeat), an offset and a rotation.
  - The Fit button fits the texture to the face.

## Visibility and selection

| Action | Key |
| --- | --- |
| Hide the selection | H |
| Hide everything else | Shift+H |
| Show everything | U |
| Select all | Ctrl+A |

## Internals

### Editable meshes (`src/meshedit/edit_mesh.{h,c}`)

- **Structure**: vertices are shared by arbitrary polygonal faces (concave allowed). Each face has its
  own material and texture projection.
- **Winding**: faces wind counter-clockwise seen from outside. Normals use Newell's method.
- **Edges** are **derived** from the faces by `edit_mesh_update()`, which must end every topology
  change. It also removes degenerate faces and unused vertices.
- **Selection** (vertices, edges, faces) is stored in the mesh and isn't saved.
- **Triangulation**: faces are triangulated by ear clipping.
- **Saving**: the JSON format is compact, and written inline in the scene.
- **Testing**: the module does no rendering, so it can be tested on its own.

### Store and copy-on-write

- **Handles**: a scene refers to its meshes by **handle** (`SceneObject::mesh`), so it stays copyable
  with `=` (undo, play mode snapshot). Copies therefore share meshes.
- **Freezing**: a shareable mesh is **frozen**. `edit_mesh_store_freeze_all()` is called on every
  `editor_begin_edit()`, every undo or redo, and when play starts.
- **Writing**: `editor_writable_mesh()` copies a frozen mesh before it is changed. So always call
  `editor_begin_edit()` before `editor_writable_mesh()`.
- **Collecting**: `edit_mesh_store_collect()` frees the handles that no copy refers to anymore (scene,
  undo and redo stacks, snapshot).
- **Duplicating**: duplicating an object duplicates its mesh. Two objects never share a handle.

### Rendering (`src/meshedit/edit_mesh_gpu.{h,c}`)

- **Building**: meshes are triangulated with flat shading, one part per material.
- **Lifetime**: they are rebuilt when their revision changes, and released a few frames after their
  last use.
- **Drawing**: the 3D renderer draws each part with its material's texture. The mesh's vertices and
  indices, and the face of each vertex, stay on the CPU too: the baker unwraps one lightmap chart per
  face (see [Baked lighting](Baked-Lighting)).
