#ifndef _EDITOR_H_
#define _EDITOR_H_

// State shared by the editor's source files : main.cpp (scene, camera, viewport, play mode), panels.cpp
// (menus and panels), asset_browser.cpp, and level_editor.cpp (Hammer-style level editing).

#include <stdio.h>

#include <SDL3/SDL.h>

#include "imgui/pireneus_imgui.h"
#include "imgui_internal.h" // DockBuilderGetCentralNode() : the dockspace's free central area
#include "ImGuizmo.h"

#include "assets/asset_database.h"
#include "launcher/editor_fonts.h"
#include "math/math3d.h"
#include "meshedit/edit_mesh.h"
#include "project/project.h"
#include "scene/scene.h"

#define ASSET_DRAG_TYPE "PIRENEUS_ASSET"

typedef struct
{
    const char* id;     // the model's name : SCENE_BUILTIN_PREFIX + id
    const char* label;
    const char* icon;
} PrimitiveInfo;

#define PRIMITIVE_COUNT 7
extern const PrimitiveInfo primitive_infos[PRIMITIVE_COUNT];

extern Project editor_project;
extern Scene editor_scene;
extern char editor_scene_path[ASSET_PATH_SIZE]; // relative to the project
extern int editor_dirty;

extern bool editor_show_grid;
extern bool editor_show_project_settings;
extern bool editor_show_2d_views;
extern bool editor_view_2d;
extern bool editor_snap_enabled;
extern float editor_snap_translation;
extern float editor_snap_rotation;
extern float editor_snap_scale;
extern ImGuizmo::OPERATION editor_gizmo_operation;
extern ImGuizmo::MODE editor_gizmo_mode;

void editor_set_status(const char* format, ...);
const char* editor_status(void);

// Whether the play mode is running, and paused.
bool editor_is_playing(void);
bool editor_is_paused(void);
void editor_play(void);
void editor_stop(void);
void editor_toggle_pause(void);
void editor_step_once(void);

// --- Editing the scene ---
void editor_begin_edit(void);      // records the scene as it was at the start of this frame, as one undo step (once per frame)
void editor_cancel_edit(void);     // back to the last recorded step, forgetting it (a gesture that changed nothing)
bool editor_can_undo(void);
bool editor_can_redo(void);
void editor_undo(void);
void editor_redo(void);

SceneObject* editor_add_object(const char* name);
SceneObject* editor_add_model(const char* model_path, const char* name);
SceneObject* editor_add_sprite(const char* sprite_path);
SceneObject* editor_add_mesh(EditMesh* mesh, const char* name); // takes the mesh ; a static collider, at the origin
void editor_delete_selected(void);
void editor_duplicate_selected(void);   // copies, moved aside
void editor_clone_selected(void);       // copies in place, which become the selection
void editor_focus_selected(void);

// The object's editable mesh, ready to be changed : copied first if an undo step or the play mode's
// snapshot may share it (copy on write). Call editor_begin_edit() first (this frame, or when the gesture
// making the change started). NULL if the object has none.
EditMesh* editor_writable_mesh(SceneObject* object);

// --- Selection : several objects, one of them the primary (the inspector's, Scene::selected) ---
enum { SELECT_REPLACE, SELECT_ADD, SELECT_TOGGLE };
void editor_select_object(int index, int how);   // -1 with SELECT_REPLACE : nothing
void editor_select_none(void);
int editor_selection_count(void);
void editor_fix_selection(void);                 // after objects were added or removed : the primary stays one of them

void editor_new_scene(void);
void editor_save_scene(void);
void editor_load_scene(const char* relative_path);
void editor_apply_settings(void);  // after the project's settings changed
void editor_save_project(void);

// Where the 3D view is pointing at, for dropping new objects.
void editor_drop_point(float out_position[3]);

// --- The 3D view's camera and what it can see ---
typedef struct
{
    Mat4 view;
    Mat4 projection;
    Mat4 view_projection;
    Vec3 eye;
    float near_plane;
    float far_plane;
    bool orthographic;
} CameraMatrices;

const CameraMatrices* editor_camera(void);   // this frame's
void editor_view_rect(ImVec2* out_min, ImVec2* out_size); // the part of the window the 3D view shows in
void editor_focus_on(const float bounds[6]); // world bounds

// The ray under a point of the window (ImGui coordinates), from the near plane (t = 0) to the far one (t = 1).
void editor_mouse_ray(const CameraMatrices* camera, ImVec2 point, Vec3* out_origin, Vec3* out_direction);

// The closest object along a ray, -1 if none : its ray parameter, and the face hit if it's an editable mesh.
enum { RAYCAST_ALL, RAYCAST_MESHES };
int editor_raycast(Vec3 origin, Vec3 direction, int filter, float* out_t, int* out_face);

// An object's local bounds (min xyz, max xyz), 0 if it has none to speak of.
int editor_object_bounds(const SceneObject* object, float out_bounds[6]);

// --- Level editing (level_editor.cpp), after Valve's Hammer (Source 2) ---
enum { SELECT_VERTICES, SELECT_EDGES, SELECT_FACES, SELECT_MESHES, SELECT_OBJECTS, SELECTION_MODE_COUNT };
enum { TOOL_SELECT, TOOL_BLOCK, TOOL_CLIP };
enum { CLIP_KEEP_BOTH, CLIP_KEEP_FRONT, CLIP_KEEP_BACK };

extern int editor_selection_mode;
extern int editor_tool;
extern float editor_grid_size;     // meters
extern bool editor_grid_snap;
extern char editor_active_material[ASSET_PATH_SIZE];
extern EditShapeParams editor_block_shape;
extern bool editor_block_hollow;
extern float editor_block_thickness;
extern int editor_clip_mode;

void level_editor_init(void);
void level_editor_reset(void);     // a scene was loaded : nothing in progress
void level_editor_free_caches(void); // at shutdown : what the 2D views cached about models
const char* level_selection_mode_name(int mode);
void level_set_selection_mode(int mode);
void level_set_tool(int tool);
void level_change_grid(int direction); // -1 : finer, +1 : coarser

// In the 3D view's window, after its grid : selection, gizmos and tools. Returns whether the camera may
// take the mouse (no gesture of the level editor in progress).
bool level_viewport(const CameraMatrices* camera, bool hovered);
void level_shortcuts(bool active); // keys of the level editor : every frame, acting only when `active`

// Mesh operations on the selection, for the panels and the shortcuts.
enum
{
    MESH_OP_EXTRUDE, MESH_OP_INSET, MESH_OP_FLIP, MESH_OP_DELETE, MESH_OP_DETACH, MESH_OP_BRIDGE,
    MESH_OP_MERGE, MESH_OP_WELD, MESH_OP_COLLAPSE, MESH_OP_SPLIT, MESH_OP_CONNECT, MESH_OP_DISSOLVE,
    MESH_OP_FILL, MESH_OP_HOLLOW, MESH_OP_SNAP, MESH_OP_MERGE_MESHES, MESH_OP_CENTER_ORIGIN,
    MESH_OP_SELECT_LOOP, MESH_OP_SELECT_RING, MESH_OP_GROW, MESH_OP_SHRINK, MESH_OP_SELECT_ALL, MESH_OP_INVERT
};
void level_mesh_operation(int operation, float amount);
void level_apply_material(const char* material); // to the selected faces, or the selected meshes' faces
void level_hide_selected(bool hide_unselected);
void level_unhide_all(void);
SceneObject* level_add_block(int shape);         // a default-sized one, on the ground where the view points

// Panels.
void editor_draw_level_panel(void);
void editor_draw_materials_panel(void);
void editor_draw_2d_views(void);

// --- Panels ---
void editor_draw_main_menu(void);
void editor_draw_primitives_panel(void);
void editor_draw_hierarchy(void);
void editor_draw_inspector(void);
void editor_draw_view_panel(void);
void editor_draw_project_settings(void);
void editor_draw_asset_browser(void);

void editor_asset_browser_init(void);
void editor_asset_browser_shutdown(void);

// A drop target for assets, on the previous ImGui item : returns the dropped path if it matches `type`.
// Built-in materials count as textures.
const char* editor_accept_asset(int type);

#endif // _EDITOR_H_
