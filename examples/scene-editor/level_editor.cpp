// Level editing, after Valve's Hammer (Source 2) : whiteboxing and greyboxing with editable meshes
// (meshedit/edit_mesh.h), in the 3D view and in three 2D views (top, front, side).
//
// - Selection modes : vertices (1), edges (2), faces (3), meshes (4), objects (5). In the first three, the
//   selected meshes show their elements, which the gizmo moves, rotates and scales. Starting a drag with
//   Shift held extrudes the selected faces, or extends the selected border edges into new faces ; in the
//   meshes mode, it drags copies of the selected meshes (Hammer's Shift-drag clone).
// - Tools : selection ; block (Shift+B) : drag a rectangle on the grid or on a face, pull its handles,
//   Enter (or Space) creates it ; clipping (Shift+X) : drag a line across the selected meshes, Shift+X
//   again to choose what's kept, Enter cuts.
// - The grid ([ and ]) snaps what's made and moved, except in the objects mode, which keeps the editor's
//   own snapping (the toolbar's increments, or Shift).
// - Hammer's textures workflow : an active material (Materials panel), applied to faces by a click on
//   Apply, a double click in the panel, or a drop ; world or face aligned, with scale, offset, rotation.

#include "editor.h"

#include "platforms/platforms_input.h"

// Positional keys (named after their QWERTY position) : they stay side by side on every layout.
#define KEY_GIZMO_TRANSLATE SCANCODE_T
#define KEY_GIZMO_ROTATE SCANCODE_R
#define KEY_GIZMO_SCALE SCANCODE_E
#define KEY_GRID_FINER SCANCODE_LEFTBRACKET    // AZERTY : ^
#define KEY_GRID_COARSER SCANCODE_RIGHTBRACKET // AZERTY : $

#define PICK_RADIUS 9.0f         // pixels
#define HANDLE_RADIUS 5.0f
#define DRAG_THRESHOLD 4.0f
#define MAX_OCCLUSION_EDGES 400  // more, and a mesh's hidden edges aren't told apart

#define COLOR_SELECTION IM_COL32(255, 160, 40, 255)
#define COLOR_SELECTION_SOFT IM_COL32(255, 160, 40, 150)
#define COLOR_HOVER IM_COL32(110, 215, 255, 255)
#define COLOR_WIRE IM_COL32(235, 235, 235, 160)
#define COLOR_WIRE_HIDDEN IM_COL32(235, 235, 235, 40)
#define COLOR_BLOCK IM_COL32(110, 255, 130, 255)
#define COLOR_KEEP IM_COL32(110, 255, 130, 230)
#define COLOR_OTHER_PART IM_COL32(90, 190, 255, 230)
#define COLOR_CUT IM_COL32(255, 80, 70, 230)
#define COLOR_FACE_FILL IM_COL32(255, 150, 40, 60)

int editor_selection_mode = SELECT_OBJECTS;
int editor_tool = TOOL_SELECT;
float editor_grid_size = 0.25f;
bool editor_grid_snap = true;
char editor_active_material[ASSET_PATH_SIZE] = ASSET_DEFAULT_MATERIAL;
EditShapeParams editor_block_shape;
bool editor_block_hollow;
float editor_block_thickness = 0.25f;
int editor_clip_mode = CLIP_KEEP_BOTH;

static const float grid_sizes[] = { 0.03125f, 0.0625f, 0.125f, 0.25f, 0.5f, 1.0f, 2.0f, 4.0f, 8.0f, 16.0f };
#define GRID_SIZE_COUNT ((int)(sizeof(grid_sizes) / sizeof(grid_sizes[0])))

static const char* const mode_names[SELECTION_MODE_COUNT] = { "Vertices", "Edges", "Faces", "Meshes", "Objects" };
static const char* const mode_icons[SELECTION_MODE_COUNT] = { ICON_VERTEX, ICON_EDGE, ICON_FACE, ICON_MESH, ICON_BOX };
static const char* const clip_mode_names[3] = { "Keep both", "Keep front", "Keep back" };

// --- Small helpers ----------------------------------------------------------------------------------

static Vec3 to_vec3(const float* p)
{
    return vec3(p[0], p[1], p[2]);
}

static Vec3 transform_point(Mat4 m, Vec3 p)
{
    float out[4];
    mat4_transform(m, p.x, p.y, p.z, 1.0f, out);
    return vec3(out[0], out[1], out[2]);
}

static float axis_of(Vec3 v, int axis)
{
    return axis == 0 ? v.x : (axis == 1 ? v.y : v.z);
}

static void set_axis(Vec3* v, int axis, float value)
{
    if (axis == 0)
    {
        v->x = value;
    }
    else if (axis == 1)
    {
        v->y = value;
    }
    else
    {
        v->z = value;
    }
}

static float snap(float value)
{
    return editor_grid_snap ? SDL_roundf(value / editor_grid_size) * editor_grid_size : value;
}

static bool component_mode(void)
{
    return editor_selection_mode <= SELECT_FACES;
}

static int mode_element(void)
{
    return editor_selection_mode == SELECT_VERTICES ? EDIT_ELEMENT_VERTEX : (editor_selection_mode == SELECT_EDGES ? EDIT_ELEMENT_EDGE : EDIT_ELEMENT_FACE);
}

static bool hidden_now(const SceneObject* object)
{
    return object->hidden && !editor_is_playing();
}

static EditMesh* mesh_of(const SceneObject* object)
{
    return object->mesh ? edit_mesh_store_get(object->mesh) : NULL;
}

static float distance_2d(ImVec2 a, ImVec2 b)
{
    return SDL_sqrtf((a.x - b.x) * (a.x - b.x) + (a.y - b.y) * (a.y - b.y));
}

// The distance from p to the segment ab on screen, and where along it (0 .. 1) the closest point is.
static float segment_distance(ImVec2 p, ImVec2 a, ImVec2 b, float* out_along)
{
    float dx = b.x - a.x;
    float dy = b.y - a.y;
    float length = dx * dx + dy * dy;
    float t = length > 1e-6f ? SDL_clamp(((p.x - a.x) * dx + (p.y - a.y) * dy) / length, 0.0f, 1.0f) : 0.0f;

    if (out_along)
    {
        *out_along = t;
    }

    return distance_2d(p, ImVec2(a.x + dx * t, a.y + dy * t));
}

// The window position of a world point ; false behind the camera.
static bool project_point(const CameraMatrices* camera, Vec3 p, ImVec2* out)
{
    float clip[4];
    mat4_transform(camera->view_projection, p.x, p.y, p.z, 1.0f, clip);

    if (clip[3] <= 1e-6f || clip[2] < 0.0f)
    {
        return false;
    }

    ImVec2 display = ImGui::GetIO().DisplaySize;
    out->x = (clip[0] / clip[3] * 0.5f + 0.5f) * display.x;
    out->y = (0.5f - clip[1] / clip[3] * 0.5f) * display.y;

    return true;
}

// A world segment, clipped by the near plane.
static void draw_segment(ImDrawList* draw_list, const CameraMatrices* camera, Vec3 a, Vec3 b, ImU32 color, float thickness)
{
    float ca[4];
    float cb[4];
    mat4_transform(camera->view_projection, a.x, a.y, a.z, 1.0f, ca);
    mat4_transform(camera->view_projection, b.x, b.y, b.z, 1.0f, cb);

    if (ca[2] < 0.0f && cb[2] < 0.0f)
    {
        return;
    }

    if (ca[2] < 0.0f || cb[2] < 0.0f)
    {
        float t = ca[2] / (ca[2] - cb[2]);
        float cut[4];

        for (int k = 0; k < 4; k++)
        {
            cut[k] = ca[k] + (cb[k] - ca[k]) * t;
        }

        SDL_memcpy(ca[2] < 0.0f ? ca : cb, cut, sizeof(cut));
    }

    if (ca[3] <= 1e-6f || cb[3] <= 1e-6f)
    {
        return;
    }

    ImVec2 display = ImGui::GetIO().DisplaySize;
    ImVec2 pa((ca[0] / ca[3] * 0.5f + 0.5f) * display.x, (0.5f - ca[1] / ca[3] * 0.5f) * display.y);
    ImVec2 pb((cb[0] / cb[3] * 0.5f + 0.5f) * display.x, (0.5f - cb[1] / cb[3] * 0.5f) * display.y);
    draw_list->AddLine(pa, pb, color, thickness);
}

static void draw_box(ImDrawList* draw_list, const CameraMatrices* camera, Mat4 matrix, const float bounds[6], ImU32 color, float thickness)
{
    Vec3 corners[8];

    for (int i = 0; i < 8; i++)
    {
        corners[i] = transform_point(matrix, vec3(bounds[(i & 1) ? 3 : 0], bounds[(i & 2) ? 4 : 1], bounds[(i & 4) ? 5 : 2]));
    }

    static const int edges[12][2] = {
        { 0, 1 }, { 2, 3 }, { 4, 5 }, { 6, 7 },
        { 0, 2 }, { 1, 3 }, { 4, 6 }, { 5, 7 },
        { 0, 4 }, { 1, 5 }, { 2, 6 }, { 3, 7 }
    };

    for (int e = 0; e < 12; e++)
    {
        draw_segment(draw_list, camera, corners[edges[e][0]], corners[edges[e][1]], color, thickness);
    }
}

// Whether a world point on a mesh is seen, or hidden by that mesh itself (a box's far edges).
static bool visible_on_mesh(const CameraMatrices* camera, const EditMesh* mesh, Mat4 inverse_model, Vec3 world)
{
    ImVec2 screen;

    if (!project_point(camera, world, &screen))
    {
        return false;
    }

    Vec3 origin;
    Vec3 direction;
    editor_mouse_ray(camera, screen, &origin, &direction);

    float length = vec3_length(direction);
    float t = vec3_dot(vec3_sub(world, origin), direction) / SDL_max(1e-9f, length * length) - 0.01f / SDL_max(1e-6f, length);

    float o[4];
    float d[4];
    mat4_transform(inverse_model, origin.x, origin.y, origin.z, 1.0f, o);
    mat4_transform(inverse_model, direction.x, direction.y, direction.z, 0.0f, d);

    int face = -1;
    return !edit_mesh_raycast(mesh, o, d, &t, &face);
}

// Whether a world point is seen, or hidden by anything in the scene.
static bool visible_in_scene(const CameraMatrices* camera, Vec3 world)
{
    ImVec2 screen;

    if (!project_point(camera, world, &screen))
    {
        return false;
    }

    Vec3 origin;
    Vec3 direction;
    editor_mouse_ray(camera, screen, &origin, &direction);

    float length = vec3_length(direction);
    float t_point = vec3_dot(vec3_sub(world, origin), direction) / SDL_max(1e-9f, length * length);
    float t_hit = 0.0f;

    return editor_raycast(origin, direction, RAYCAST_ALL, &t_hit, NULL) < 0 || t_hit >= t_point - 0.01f / SDL_max(1e-6f, length);
}

// The parameter along a line (p, unit e) closest to a ray (o, d).
static float closest_on_line(Vec3 p, Vec3 e, Vec3 o, Vec3 d)
{
    Vec3 w = vec3_sub(p, o);
    float b = vec3_dot(e, d);
    float c = vec3_dot(d, d);
    float dw = vec3_dot(e, w);
    float ew = vec3_dot(d, w);
    float denominator = c - b * b;

    return SDL_fabsf(denominator) < 1e-12f ? 0.0f : (b * ew - c * dw) / denominator;
}

// Where a ray meets the plane x[axis] = value ; false if it doesn't (parallel, or behind).
static bool ray_plane(Vec3 origin, Vec3 direction, int axis, float value, Vec3* out)
{
    float d = axis_of(direction, axis);

    if (SDL_fabsf(d) < 1e-9f)
    {
        return false;
    }

    float t = (value - axis_of(origin, axis)) / d;

    if (t < 0.0f)
    {
        return false;
    }

    *out = vec3_add(origin, vec3_scale(direction, t));
    set_axis(out, axis, value);

    return true;
}

// --- Selection state --------------------------------------------------------------------------------

typedef struct
{
    int object; // -1 : nothing
    int index;  // the element in the object's mesh, -1 for the object itself
} Pick;

static const Pick no_pick = { -1, -1 };

static void clear_components(int element)
{
    for (int i = 0; i < editor_scene.count; i++)
    {
        EditMesh* mesh = mesh_of(&editor_scene.objects[i]);

        if (mesh)
        {
            edit_mesh_select_all(mesh, element, 0);
        }
    }
}

static bool element_selected(const EditMesh* mesh, int element, int index)
{
    if (element == EDIT_ELEMENT_VERTEX)
    {
        return mesh->vertex_selected[index] != 0;
    }

    return element == EDIT_ELEMENT_EDGE ? mesh->edges[index].selected != 0 : mesh->faces[index].selected != 0;
}

static void set_element(EditMesh* mesh, int element, int index, int value)
{
    if (element == EDIT_ELEMENT_VERTEX)
    {
        mesh->vertex_selected[index] = (unsigned char)value;
    }
    else if (element == EDIT_ELEMENT_EDGE)
    {
        mesh->edges[index].selected = value;
    }
    else
    {
        mesh->faces[index].selected = value;
    }
}

static int element_count(const EditMesh* mesh, int element)
{
    return element == EDIT_ELEMENT_VERTEX ? mesh->vertex_count : (element == EDIT_ELEMENT_EDGE ? mesh->edge_count : mesh->face_count);
}

// An element's position, in the mesh's space : a vertex, an edge's middle, a face's center.
static Vec3 element_position(const EditMesh* mesh, int element, int index)
{
    if (element == EDIT_ELEMENT_VERTEX)
    {
        return to_vec3(edit_mesh_position(mesh, index));
    }

    if (element == EDIT_ELEMENT_EDGE)
    {
        Vec3 a = to_vec3(edit_mesh_position(mesh, mesh->edges[index].a));
        Vec3 b = to_vec3(edit_mesh_position(mesh, mesh->edges[index].b));
        return vec3_scale(vec3_add(a, b), 0.5f);
    }

    float center[3];
    edit_mesh_face_center(mesh, index, center);
    return to_vec3(center);
}

// What a click on an element does : that element alone (and its object), or added / removed with Ctrl.
static void click_element(Pick pick, bool ctrl)
{
    int element = mode_element();

    if (pick.object < 0)
    {
        if (!ctrl)
        {
            clear_components(element);
        }

        return;
    }

    SceneObject* object = &editor_scene.objects[pick.object];
    EditMesh* mesh = mesh_of(object);

    if (!mesh)
    {
        return;
    }

    if (!ctrl)
    {
        clear_components(element);

        if (!object->selected)
        {
            editor_select_object(pick.object, SELECT_REPLACE);
        }
    }
    else if (!object->selected)
    {
        editor_select_object(pick.object, SELECT_ADD);
    }

    bool selected = element_selected(mesh, element, pick.index);
    set_element(mesh, element, pick.index, ctrl ? !selected : 1);
}

static void click_object(Pick pick, bool ctrl)
{
    if (pick.object < 0)
    {
        if (!ctrl)
        {
            editor_select_none();
        }

        return;
    }

    editor_select_object(pick.object, ctrl ? SELECT_TOGGLE : SELECT_REPLACE);
}

// The meshes the component modes work on : the selected objects that have one.
static int selected_meshes(int* out)
{
    int count = 0;

    for (int i = 0; i < editor_scene.count; i++)
    {
        if (editor_scene.objects[i].selected && editor_scene.objects[i].mesh && mesh_of(&editor_scene.objects[i]) && !hidden_now(&editor_scene.objects[i]))
        {
            out[count++] = i;
        }
    }

    return count;
}

// The world bounds of what's selected (elements in the component modes, objects otherwise).
static bool selection_bounds(float out[6])
{
    for (int axis = 0; axis < 3; axis++)
    {
        out[axis] = 1e30f;
        out[axis + 3] = -1e30f;
    }

    bool found = false;
    static int meshes[SCENE_MAX_OBJECTS];
    int count = selected_meshes(meshes);

    for (int k = 0; k < count && component_mode(); k++)
    {
        const SceneObject* object = &editor_scene.objects[meshes[k]];
        const EditMesh* mesh = mesh_of(object);
        unsigned char* flags = (unsigned char*)SDL_malloc((size_t)SDL_max(1, mesh->vertex_count));
        edit_mesh_selection_vertices(mesh, mode_element(), flags);
        Mat4 matrix = scene_object_matrix(object);

        for (int v = 0; v < mesh->vertex_count; v++)
        {
            if (flags[v])
            {
                Vec3 p = transform_point(matrix, to_vec3(edit_mesh_position(mesh, v)));

                for (int axis = 0; axis < 3; axis++)
                {
                    out[axis] = SDL_min(out[axis], axis_of(p, axis));
                    out[axis + 3] = SDL_max(out[axis + 3], axis_of(p, axis));
                }

                found = true;
            }
        }

        SDL_free(flags);
    }

    return found;
}

// --- Gestures : the gizmo, and drags in the 2D views ------------------------------------------------

typedef struct
{
    int object;
    int vertex;
    Vec3 world;
} MovedVertex;

typedef struct
{
    int object;
    Mat4 matrix;
} MovedObject;

static MovedVertex* moved_vertices;
static int moved_vertex_count;
static int moved_vertex_capacity;
static MovedObject* moved_objects;
static int moved_object_count;
static int moved_object_capacity;
static bool gesture_creates;    // the gesture extruded or cloned : undone if nothing ended up moving

// Starts moving the selection : one undo step, the extrusion or the clone Shift asks for, and where
// everything was.
static void begin_move(bool shift)
{
    editor_begin_edit();
    gesture_creates = false;
    moved_vertex_count = 0;
    moved_object_count = 0;

    if (component_mode())
    {
        static int meshes[SCENE_MAX_OBJECTS];
        int count = selected_meshes(meshes);

        for (int k = 0; k < count; k++)
        {
            SceneObject* object = &editor_scene.objects[meshes[k]];
            EditMesh* mesh = editor_writable_mesh(object);

            if (shift && editor_selection_mode == SELECT_FACES && edit_mesh_selection_count(mesh, EDIT_ELEMENT_FACE) > 0)
            {
                gesture_creates |= edit_mesh_extrude_faces(mesh) > 0;
            }
            else if (shift && editor_selection_mode == SELECT_EDGES && edit_mesh_selection_count(mesh, EDIT_ELEMENT_EDGE) > 0)
            {
                gesture_creates |= edit_mesh_extrude_edges(mesh) > 0;
            }

            unsigned char* flags = (unsigned char*)SDL_malloc((size_t)SDL_max(1, mesh->vertex_count));
            edit_mesh_selection_vertices(mesh, mode_element(), flags);
            Mat4 matrix = scene_object_matrix(object);

            for (int v = 0; v < mesh->vertex_count; v++)
            {
                if (!flags[v])
                {
                    continue;
                }

                if (moved_vertex_count == moved_vertex_capacity)
                {
                    moved_vertex_capacity = SDL_max(256, moved_vertex_capacity * 2);
                    moved_vertices = (MovedVertex*)SDL_realloc(moved_vertices, sizeof(MovedVertex) * (size_t)moved_vertex_capacity);
                }

                MovedVertex* moved = &moved_vertices[moved_vertex_count++];
                moved->object = meshes[k];
                moved->vertex = v;
                moved->world = transform_point(matrix, to_vec3(edit_mesh_position(mesh, v)));
            }

            SDL_free(flags);
        }

        return;
    }

    if (shift && editor_selection_mode == SELECT_MESHES)
    {
        editor_clone_selected();
        gesture_creates = true;
    }

    for (int i = 0; i < editor_scene.count; i++)
    {
        if (!editor_scene.objects[i].selected)
        {
            continue;
        }

        if (moved_object_count == moved_object_capacity)
        {
            moved_object_capacity = SDL_max(64, moved_object_capacity * 2);
            moved_objects = (MovedObject*)SDL_realloc(moved_objects, sizeof(MovedObject) * (size_t)moved_object_capacity);
        }

        moved_objects[moved_object_count].object = i;
        moved_objects[moved_object_count].matrix = scene_object_matrix(&editor_scene.objects[i]);
        moved_object_count++;
    }
}

// Everything moved by `delta` (world) from where it was when the move began.
static void apply_move(Mat4 delta)
{
    if (component_mode())
    {
        int last_object = -1;
        Mat4 inverse = mat4_identity();
        EditMesh* mesh = NULL;

        for (int i = 0; i < moved_vertex_count; i++)
        {
            const MovedVertex* moved = &moved_vertices[i];

            if (moved->object != last_object)
            {
                if (mesh)
                {
                    edit_mesh_touch(mesh);
                }

                last_object = moved->object;
                inverse = mat4_inverse(scene_object_matrix(&editor_scene.objects[last_object]));
                mesh = mesh_of(&editor_scene.objects[last_object]); // already a writable copy : begin_move() made it
            }

            if (!mesh || moved->vertex >= mesh->vertex_count)
            {
                continue;
            }

            Vec3 p = transform_point(inverse, transform_point(delta, moved->world));
            float* position = &mesh->positions[moved->vertex * 3];
            position[0] = p.x;
            position[1] = p.y;
            position[2] = p.z;
        }

        if (mesh)
        {
            edit_mesh_touch(mesh);
        }

        return;
    }

    for (int i = 0; i < moved_object_count; i++)
    {
        SceneObject* object = &editor_scene.objects[moved_objects[i].object];
        Mat4 moved = mat4_transpose(mat4_multiply(delta, moved_objects[i].matrix));
        ImGuizmo::DecomposeMatrixToComponents(moved.m, object->position, object->rotation, object->scale);
    }
}

static bool is_identity(Mat4 m)
{
    Mat4 identity = mat4_identity();

    for (int i = 0; i < 16; i++)
    {
        if (SDL_fabsf(m.m[i] - identity.m[i]) > 1e-5f)
        {
            return false;
        }
    }

    return true;
}

static void end_move(Mat4 delta)
{
    // An extrusion or a clone that didn't go anywhere would only leave doubled geometry behind.
    if (gesture_creates && is_identity(delta))
    {
        editor_cancel_edit();
    }

    gesture_creates = false;
    moved_vertex_count = 0;
    moved_object_count = 0;
}

static Mat4 translation(Vec3 t)
{
    Mat4 m = mat4_identity();
    m.m[3] = t.x;
    m.m[7] = t.y;
    m.m[11] = t.z;
    return m;
}

// --- Gizmo ------------------------------------------------------------------------------------------

static bool gizmo_was_using;
static bool gizmo_drawn;   // this frame : ImGuizmo::IsOver() only means something then
static Mat4 gizmo_matrix;  // engine layout (row-major)
static Mat4 gizmo_start;

// The gizmo's frame when nothing is being dragged : the primary object's own, or the selected
// elements' center (oriented like their object in local mode).
static bool gizmo_frame(Mat4* out)
{
    if (editor_scene.selected < 0)
    {
        return false;
    }

    const SceneObject* primary = &editor_scene.objects[editor_scene.selected];

    if (!component_mode())
    {
        *out = scene_object_matrix(primary);
        return true;
    }

    float bounds[6];

    if (!selection_bounds(bounds))
    {
        return false;
    }

    Vec3 center = vec3((bounds[0] + bounds[3]) * 0.5f, (bounds[1] + bounds[4]) * 0.5f, (bounds[2] + bounds[5]) * 0.5f);
    Mat4 frame = translation(center);

    if (editor_gizmo_mode == ImGuizmo::LOCAL && editor_gizmo_operation != ImGuizmo::SCALE)
    {
        float r[9];
        scene_rotation_from_euler(primary->rotation, r);

        for (int row = 0; row < 3; row++)
        {
            for (int column = 0; column < 3; column++)
            {
                frame.m[row * 4 + column] = r[row * 3 + column];
            }
        }
    }

    *out = frame;
    return true;
}

static bool run_gizmo(const CameraMatrices* camera)
{
    ImGuiIO& io = ImGui::GetIO();
    gizmo_drawn = false;

    if (!gizmo_was_using && !gizmo_frame(&gizmo_matrix))
    {
        return false;
    }

    gizmo_drawn = true;

    Mat4 view = mat4_transpose(camera->view);
    Mat4 projection = mat4_transpose(camera->projection);
    Mat4 manipulated = mat4_transpose(gizmo_matrix);
    Mat4 before = gizmo_matrix;

    float snap_values[3];
    bool snapping;
    float step;

    if (editor_selection_mode == SELECT_OBJECTS)
    {
        // The editor's own snapping : the toolbar's increments, with the toggle or while Shift is held.
        snapping = editor_snap_enabled || io.KeyShift;
        step = editor_gizmo_operation == ImGuizmo::TRANSLATE ? editor_snap_translation :
            editor_gizmo_operation == ImGuizmo::ROTATE ? editor_snap_rotation : editor_snap_scale;
    }
    else
    {
        snapping = editor_grid_snap;
        step = editor_gizmo_operation == ImGuizmo::TRANSLATE ? editor_grid_size :
            editor_gizmo_operation == ImGuizmo::ROTATE ? editor_snap_rotation : editor_snap_scale;
    }

    snap_values[0] = snap_values[1] = snap_values[2] = step;

    // Objects scale along their own axes (a world-space scale of a rotated object isn't expressible as
    // position / rotation / scale) ; elements may scale either way.
    ImGuizmo::MODE mode = editor_gizmo_operation == ImGuizmo::SCALE && !component_mode() ? ImGuizmo::LOCAL : editor_gizmo_mode;

    ImGuizmo::Manipulate(view.m, projection.m, editor_gizmo_operation, mode, manipulated.m, NULL, snapping ? snap_values : NULL);

    bool using_now = ImGuizmo::IsUsing();

    if (using_now && !gizmo_was_using)
    {
        gizmo_start = before;
        begin_move(io.KeyShift);
    }

    if (using_now)
    {
        gizmo_matrix = mat4_transpose(manipulated);
        apply_move(mat4_multiply(gizmo_matrix, mat4_inverse(gizmo_start)));
    }
    else if (gizmo_was_using)
    {
        end_move(mat4_multiply(gizmo_matrix, mat4_inverse(gizmo_start)));
    }

    gizmo_was_using = using_now;

    return using_now;
}

// --- Picking in the 3D view -------------------------------------------------------------------------

static Pick pick_3d(const CameraMatrices* camera, ImVec2 mouse)
{
    Pick pick = no_pick;
    Vec3 origin;
    Vec3 direction;
    editor_mouse_ray(camera, mouse, &origin, &direction);

    float t = 0.0f;
    int face = -1;

    if (!component_mode())
    {
        pick.object = editor_raycast(origin, direction, editor_selection_mode == SELECT_MESHES ? RAYCAST_MESHES : RAYCAST_ALL, &t, &face);
        return pick;
    }

    int hit = editor_raycast(origin, direction, RAYCAST_MESHES, &t, &face);

    if (editor_selection_mode == SELECT_FACES)
    {
        if (hit >= 0 && face >= 0)
        {
            pick.object = hit;
            pick.index = face;
        }

        return pick;
    }

    // Vertices and edges : the closest one on screen, among the selected meshes' and the hovered one's.
    float best = PICK_RADIUS;

    for (int i = 0; i < editor_scene.count; i++)
    {
        const SceneObject* object = &editor_scene.objects[i];
        const EditMesh* mesh = mesh_of(object);

        if (!mesh || hidden_now(object) || !(object->selected || i == hit))
        {
            continue;
        }

        Mat4 matrix = scene_object_matrix(object);

        if (editor_selection_mode == SELECT_VERTICES)
        {
            for (int v = 0; v < mesh->vertex_count; v++)
            {
                Vec3 world = transform_point(matrix, to_vec3(edit_mesh_position(mesh, v)));
                ImVec2 screen;

                if (project_point(camera, world, &screen))
                {
                    float distance = distance_2d(screen, mouse);

                    if (distance < best && visible_in_scene(camera, world))
                    {
                        best = distance;
                        pick.object = i;
                        pick.index = v;
                    }
                }
            }
        }
        else
        {
            for (int e = 0; e < mesh->edge_count; e++)
            {
                Vec3 a = transform_point(matrix, to_vec3(edit_mesh_position(mesh, mesh->edges[e].a)));
                Vec3 b = transform_point(matrix, to_vec3(edit_mesh_position(mesh, mesh->edges[e].b)));
                ImVec2 sa;
                ImVec2 sb;

                if (!project_point(camera, a, &sa) || !project_point(camera, b, &sb))
                {
                    continue;
                }

                float along = 0.0f;
                float distance = segment_distance(mouse, sa, sb, &along);

                if (distance < best && visible_in_scene(camera, vec3_add(a, vec3_scale(vec3_sub(b, a), along))))
                {
                    best = distance;
                    pick.object = i;
                    pick.index = e;
                }
            }
        }
    }

    return pick;
}

// --- Drawing in the 3D view -------------------------------------------------------------------------

static void draw_grid(ImDrawList* draw_list, const CameraMatrices* camera, int axis, float height)
{
    if (editor_view_2d)
    {
        return;
    }

    ImVec2 area_min;
    ImVec2 area_size;
    editor_view_rect(&area_min, &area_size);

    // Centered where the view looks at, on the grid's plane.
    Vec3 origin;
    Vec3 direction;
    editor_mouse_ray(camera, ImVec2(area_min.x + area_size.x * 0.5f, area_min.y + area_size.y * 0.5f), &origin, &direction);

    Vec3 center;

    if (!ray_plane(origin, direction, axis, height, &center))
    {
        center = camera->eye;
    }

    float distance = vec3_length(vec3_sub(center, camera->eye));
    float radius = SDL_clamp(distance * 1.6f, 4.0f, 150.0f);
    float spacing = editor_grid_size;

    while (radius / spacing > 80.0f)
    {
        spacing *= 2.0f;
    }

    float major = SDL_max(1.0f, spacing * 8.0f);
    int u_axis = axis == 0 ? 1 : 0;
    int v_axis = axis == 2 ? 1 : 2;
    float cu = SDL_floorf(axis_of(center, u_axis) / spacing) * spacing;
    float cv = SDL_floorf(axis_of(center, v_axis) / spacing) * spacing;
    int lines = (int)(radius / spacing);

    for (int direction_index = 0; direction_index < 2; direction_index++)
    {
        int along = direction_index == 0 ? u_axis : v_axis;  // the lines run along this axis
        int across = direction_index == 0 ? v_axis : u_axis;
        float c_along = direction_index == 0 ? cu : cv;
        float c_across = direction_index == 0 ? cv : cu;

        for (int i = -lines; i <= lines; i++)
        {
            float coordinate = c_across + (float)i * spacing;
            bool is_axis = SDL_fabsf(coordinate) < spacing * 0.25f;
            bool is_major = SDL_fabsf(SDL_fmodf(SDL_fabsf(coordinate) + spacing * 0.25f, major)) < spacing * 0.5f;
            float fade = 1.0f - SDL_fabsf(coordinate - c_across) / radius;

            // In pieces, each faded by its distance to the center.
            const int pieces = 8;

            for (int k = 0; k < pieces; k++)
            {
                float s0 = -radius + 2.0f * radius * (float)k / (float)pieces;
                float s1 = -radius + 2.0f * radius * (float)(k + 1) / (float)pieces;
                float piece_fade = fade * (1.0f - SDL_fabsf((s0 + s1) * 0.5f) / radius);

                if (piece_fade <= 0.02f)
                {
                    continue;
                }

                Vec3 a = vec3(0.0f, 0.0f, 0.0f);
                Vec3 b = vec3(0.0f, 0.0f, 0.0f);
                set_axis(&a, axis, height);
                set_axis(&b, axis, height);
                set_axis(&a, across, coordinate);
                set_axis(&b, across, coordinate);
                set_axis(&a, along, c_along + s0);
                set_axis(&b, along, c_along + s1);

                int alpha = (int)((is_axis ? 200.0f : is_major ? 110.0f : 50.0f) * piece_fade);
                ImU32 color = IM_COL32(150, 150, 150, alpha);

                if (is_axis)
                {
                    // The line along X is the X axis : red ; along Z : blue (Y : green).
                    color = along == 0 ? IM_COL32(220, 70, 70, alpha) : along == 2 ? IM_COL32(70, 120, 230, alpha) : IM_COL32(80, 200, 80, alpha);
                }

                draw_segment(draw_list, camera, a, b, color, is_axis ? 1.5f : 1.0f);
            }
        }
    }
}

static void draw_mesh_wire(ImDrawList* draw_list, const CameraMatrices* camera, const SceneObject* object, ImU32 color, float thickness)
{
    const EditMesh* mesh = mesh_of(object);
    Mat4 matrix = scene_object_matrix(object);

    for (int e = 0; e < mesh->edge_count; e++)
    {
        Vec3 a = transform_point(matrix, to_vec3(edit_mesh_position(mesh, mesh->edges[e].a)));
        Vec3 b = transform_point(matrix, to_vec3(edit_mesh_position(mesh, mesh->edges[e].b)));
        draw_segment(draw_list, camera, a, b, color, thickness);
    }
}

// A face, filled : its triangles, projected.
static void fill_face(ImDrawList* draw_list, const CameraMatrices* camera, const EditMesh* mesh, Mat4 matrix, int face, ImU32 color)
{
    static int corners[3 * 256];
    const EditFace* f = &mesh->faces[face];

    if (f->count > 256)
    {
        return;
    }

    int triangles = edit_mesh_triangulate_face(mesh, face, corners);

    for (int t = 0; t < triangles; t++)
    {
        ImVec2 p[3];
        bool ok = true;

        for (int k = 0; k < 3 && ok; k++)
        {
            ok = project_point(camera, transform_point(matrix, to_vec3(edit_mesh_position(mesh, edit_mesh_face_vertex(mesh, face, corners[t * 3 + k])))), &p[k]);
        }

        if (ok)
        {
            draw_list->AddTriangleFilled(p[0], p[1], p[2], color);
        }
    }
}

static void outline_face(ImDrawList* draw_list, const CameraMatrices* camera, const EditMesh* mesh, Mat4 matrix, int face, ImU32 color, float thickness)
{
    const EditFace* f = &mesh->faces[face];

    for (int i = 0; i < f->count; i++)
    {
        Vec3 a = transform_point(matrix, to_vec3(edit_mesh_position(mesh, edit_mesh_face_vertex(mesh, face, i))));
        Vec3 b = transform_point(matrix, to_vec3(edit_mesh_position(mesh, edit_mesh_face_vertex(mesh, face, i + 1))));
        draw_segment(draw_list, camera, a, b, color, thickness);
    }
}

static void draw_hover_element(ImDrawList* draw_list, const CameraMatrices* camera, int object_index, Pick hover);

// The elements of a selected mesh, in the component modes : its wireframe (hidden edges fainter), and
// the selection.
static void draw_components(ImDrawList* draw_list, const CameraMatrices* camera, int object_index, Pick hover)
{
    const SceneObject* object = &editor_scene.objects[object_index];
    const EditMesh* mesh = mesh_of(object);
    Mat4 matrix = scene_object_matrix(object);
    Mat4 inverse = mat4_inverse(matrix);
    bool occlusion = mesh->edge_count <= MAX_OCCLUSION_EDGES;
    int mode = editor_selection_mode;

    if (mode == SELECT_FACES)
    {
        for (int f = 0; f < mesh->face_count; f++)
        {
            if (mesh->faces[f].selected)
            {
                fill_face(draw_list, camera, mesh, matrix, f, COLOR_FACE_FILL);
            }
        }
    }

    for (int e = 0; e < mesh->edge_count; e++)
    {
        Vec3 a = transform_point(matrix, to_vec3(edit_mesh_position(mesh, mesh->edges[e].a)));
        Vec3 b = transform_point(matrix, to_vec3(edit_mesh_position(mesh, mesh->edges[e].b)));
        bool visible = !occlusion || visible_on_mesh(camera, mesh, inverse, vec3_scale(vec3_add(a, b), 0.5f));
        bool selected = mode == SELECT_EDGES && mesh->edges[e].selected;

        if (selected)
        {
            draw_segment(draw_list, camera, a, b, visible ? COLOR_SELECTION : COLOR_SELECTION_SOFT, 3.0f);
        }
        else
        {
            draw_segment(draw_list, camera, a, b, visible ? COLOR_WIRE : COLOR_WIRE_HIDDEN, 1.0f);
        }
    }

    if (mode == SELECT_FACES)
    {
        for (int f = 0; f < mesh->face_count; f++)
        {
            if (mesh->faces[f].selected)
            {
                outline_face(draw_list, camera, mesh, matrix, f, COLOR_SELECTION, 2.0f);
            }
        }
    }

    if (mode == SELECT_VERTICES)
    {
        for (int v = 0; v < mesh->vertex_count; v++)
        {
            Vec3 world = transform_point(matrix, to_vec3(edit_mesh_position(mesh, v)));
            ImVec2 screen;

            if (!project_point(camera, world, &screen))
            {
                continue;
            }

            bool visible = !occlusion || visible_on_mesh(camera, mesh, inverse, world);
            bool selected = mesh->vertex_selected[v] != 0;
            float size = selected ? 4.0f : 3.0f;
            ImU32 color = selected ? COLOR_SELECTION : (visible ? IM_COL32(240, 240, 240, 230) : IM_COL32(240, 240, 240, 60));
            draw_list->AddRectFilled(ImVec2(screen.x - size, screen.y - size), ImVec2(screen.x + size, screen.y + size), color);
        }
    }

    draw_hover_element(draw_list, camera, object_index, hover);
}

// What a click would pick : on any mesh, selected or not.
static void draw_hover_element(ImDrawList* draw_list, const CameraMatrices* camera, int object_index, Pick hover)
{
    const EditMesh* mesh = mesh_of(&editor_scene.objects[object_index]);
    Mat4 matrix = scene_object_matrix(&editor_scene.objects[object_index]);
    int mode = editor_selection_mode;

    if (mesh && hover.object == object_index && hover.index >= 0)
    {
        if (mode == SELECT_VERTICES && hover.index < mesh->vertex_count)
        {
            ImVec2 screen;

            if (project_point(camera, transform_point(matrix, to_vec3(edit_mesh_position(mesh, hover.index))), &screen))
            {
                draw_list->AddRect(ImVec2(screen.x - 6, screen.y - 6), ImVec2(screen.x + 6, screen.y + 6), COLOR_HOVER, 0.0f, 2.0f);
            }
        }
        else if (mode == SELECT_EDGES && hover.index < mesh->edge_count)
        {
            Vec3 a = transform_point(matrix, to_vec3(edit_mesh_position(mesh, mesh->edges[hover.index].a)));
            Vec3 b = transform_point(matrix, to_vec3(edit_mesh_position(mesh, mesh->edges[hover.index].b)));
            draw_segment(draw_list, camera, a, b, COLOR_HOVER, 3.0f);
        }
        else if (mode == SELECT_FACES && hover.index < mesh->face_count)
        {
            outline_face(draw_list, camera, mesh, matrix, hover.index, COLOR_HOVER, 2.0f);
        }
    }
}

static void draw_selection(ImDrawList* draw_list, const CameraMatrices* camera, Pick hover)
{
    for (int i = 0; i < editor_scene.count; i++)
    {
        const SceneObject* object = &editor_scene.objects[i];

        if (hidden_now(object))
        {
            continue;
        }

        bool has_mesh = mesh_of(object) != NULL;

        if (component_mode())
        {
            if (object->selected && has_mesh)
            {
                draw_components(draw_list, camera, i, hover);
            }
            else if (has_mesh && hover.object == i)
            {
                draw_hover_element(draw_list, camera, i, hover);
            }

            continue;
        }

        if (object->selected || (hover.object == i && hover.index < 0))
        {
            ImU32 color = !object->selected ? COLOR_HOVER : (i == editor_scene.selected ? COLOR_SELECTION : COLOR_SELECTION_SOFT);

            if (has_mesh)
            {
                draw_mesh_wire(draw_list, camera, object, color, object->selected ? 1.5f : 1.0f);
            }
            else
            {
                float bounds[6];
                editor_object_bounds(object, bounds);
                draw_box(draw_list, camera, scene_object_matrix(object), bounds, color, 1.5f);
            }
        }
    }
}

// --- Block tool -------------------------------------------------------------------------------------

enum { BLOCK_NONE, BLOCK_DRAWING, BLOCK_PENDING };

typedef struct
{
    int stage;
    int axis;          // the plane it's drawn on : x[axis] = base
    float base;
    float rise;        // signed extent along the axis
    Vec3 start;        // where the drawing started, on that plane
    Vec3 min;
    Vec3 max;
    int handle;        // being dragged in the 3D view : axis * 2 + (max side), -1 : none
} BlockState;

static BlockState block;
static float block_rise = 1.0f;  // remembered between blocks, like Hammer does
static Vec3 last_block_min = { 0.0f, 0.0f, 0.0f };
static Vec3 last_block_max = { 1.0f, 1.0f, 1.0f };

static void block_extent_from_drawing(Vec3 current)
{
    int u = block.axis == 0 ? 1 : 0;
    int v = block.axis == 2 ? 1 : 2;

    set_axis(&block.min, u, SDL_min(axis_of(block.start, u), axis_of(current, u)));
    set_axis(&block.max, u, SDL_max(axis_of(block.start, u), axis_of(current, u)));
    set_axis(&block.min, v, SDL_min(axis_of(block.start, v), axis_of(current, v)));
    set_axis(&block.max, v, SDL_max(axis_of(block.start, v), axis_of(current, v)));
    set_axis(&block.min, block.axis, SDL_min(block.base, block.base + block.rise));
    set_axis(&block.max, block.axis, SDL_max(block.base, block.base + block.rise));
}

static bool block_valid(void)
{
    return block.max.x - block.min.x > 1e-4f && block.max.y - block.min.y > 1e-4f && block.max.z - block.min.z > 1e-4f;
}

static void create_block(void)
{
    if (block.stage != BLOCK_PENDING || !block_valid())
    {
        return;
    }

    float size[3] = { block.max.x - block.min.x, block.max.y - block.min.y, block.max.z - block.min.z };
    EditMesh* mesh = edit_mesh_make_shape(&editor_block_shape, size, editor_active_material);

    if (editor_block_hollow)
    {
        float thinnest = SDL_min(size[0], SDL_min(size[1], size[2]));
        edit_mesh_thicken(mesh, SDL_clamp(editor_block_thickness, 0.001f, thinnest * 0.45f));
    }

    SceneObject* object = editor_add_mesh(mesh, edit_mesh_shape_name(editor_block_shape.shape));

    if (object)
    {
        object->position[0] = (block.min.x + block.max.x) * 0.5f;
        object->position[1] = (block.min.y + block.max.y) * 0.5f;
        object->position[2] = (block.min.z + block.max.z) * 0.5f;
        editor_set_status("%s created (%.2f x %.2f x %.2f m).", object->name, size[0], size[1], size[2]);
    }

    last_block_min = block.min;
    last_block_max = block.max;
    block.stage = BLOCK_NONE;
}

static Vec3 block_handle_position(int handle)
{
    Vec3 p = vec3_scale(vec3_add(block.min, block.max), 0.5f);
    int axis = handle / 2;
    set_axis(&p, axis, axis_of(handle % 2 ? block.max : block.min, axis));
    return p;
}

static int hovered_block_handle(const CameraMatrices* camera, ImVec2 mouse)
{
    for (int h = 0; h < 6; h++)
    {
        ImVec2 screen;

        if (project_point(camera, block_handle_position(h), &screen) && distance_2d(screen, mouse) < HANDLE_RADIUS + 4.0f)
        {
            return h;
        }
    }

    return -1;
}

// A face of the pending block moved to a coordinate, the block staying at least a little thick.
static void set_block_side(int axis, int max_side, float value)
{
    const float thinnest = 0.01f;

    if (max_side)
    {
        set_axis(&block.max, axis, SDL_max(value, axis_of(block.min, axis) + thinnest));
    }
    else
    {
        set_axis(&block.min, axis, SDL_min(value, axis_of(block.max, axis) - thinnest));
    }

    if (axis == block.axis)
    {
        block_rise = axis_of(block.max, axis) - axis_of(block.min, axis);
        block.rise = block.rise < 0.0f ? -block_rise : block_rise;
    }
}

static void draw_block(ImDrawList* draw_list, const CameraMatrices* camera, ImVec2 mouse)
{
    if (block.stage == BLOCK_NONE)
    {
        return;
    }

    float bounds[6] = { block.min.x, block.min.y, block.min.z, block.max.x, block.max.y, block.max.z };
    draw_box(draw_list, camera, mat4_identity(), bounds, COLOR_BLOCK, 2.0f);

    if (block.stage == BLOCK_PENDING)
    {
        int hovered = block.handle >= 0 ? block.handle : hovered_block_handle(camera, mouse);

        for (int h = 0; h < 6; h++)
        {
            ImVec2 screen;

            if (project_point(camera, block_handle_position(h), &screen))
            {
                if (h == hovered)
                {
                    draw_list->AddCircleFilled(screen, HANDLE_RADIUS + 1.0f, COLOR_HOVER);
                }
                else
                {
                    draw_list->AddCircleFilled(screen, HANDLE_RADIUS, IM_COL32(20, 60, 25, 220));
                    draw_list->AddCircle(screen, HANDLE_RADIUS, COLOR_BLOCK, 0, 1.5f);
                }
            }
        }
    }

    // Its size, under it.
    ImVec2 screen;

    if (project_point(camera, vec3((block.min.x + block.max.x) * 0.5f, block.min.y, (block.min.z + block.max.z) * 0.5f), &screen))
    {
        char text[96];
        SDL_snprintf(text, sizeof(text), "%.2f x %.2f x %.2f", block.max.x - block.min.x, block.max.y - block.min.y, block.max.z - block.min.z);
        ImVec2 size = ImGui::CalcTextSize(text);
        draw_list->AddRectFilled(ImVec2(screen.x - size.x * 0.5f - 4, screen.y + 8), ImVec2(screen.x + size.x * 0.5f + 4, screen.y + 12 + size.y), IM_COL32(0, 0, 0, 170), 3.0f);
        draw_list->AddText(ImVec2(screen.x - size.x * 0.5f, screen.y + 10), COLOR_BLOCK, text);
    }
}

static bool block_tool_3d(const CameraMatrices* camera, bool hovered, ImDrawList* draw_list)
{
    ImGuiIO& io = ImGui::GetIO();
    ImVec2 mouse = io.MousePos;
    Vec3 origin;
    Vec3 direction;
    editor_mouse_ray(camera, mouse, &origin, &direction);

    // Pulling one of the pending block's handles.
    if (block.handle >= 0)
    {
        int axis = block.handle / 2;
        Vec3 e = vec3(axis == 0 ? 1.0f : 0.0f, axis == 1 ? 1.0f : 0.0f, axis == 2 ? 1.0f : 0.0f);
        Vec3 p = block_handle_position(block.handle);
        float along = closest_on_line(p, e, origin, direction);
        set_block_side(axis, block.handle % 2, snap(axis_of(p, axis) + along));

        if (!ImGui::IsMouseDown(ImGuiMouseButton_Left))
        {
            block.handle = -1;
        }

        draw_block(draw_list, camera, mouse);
        return true;
    }

    if (block.stage == BLOCK_DRAWING)
    {
        Vec3 point;

        if (ray_plane(origin, direction, block.axis, block.base, &point))
        {
            int u = block.axis == 0 ? 1 : 0;
            int v = block.axis == 2 ? 1 : 2;
            set_axis(&point, u, snap(axis_of(point, u)));
            set_axis(&point, v, snap(axis_of(point, v)));
            block_extent_from_drawing(point);
        }

        if (!ImGui::IsMouseDown(ImGuiMouseButton_Left))
        {
            block.stage = block_valid() ? BLOCK_PENDING : BLOCK_NONE;
        }

        draw_block(draw_list, camera, mouse);
        return true;
    }

    // Where a new block would start : a small cross on the grid or the face under the cursor.
    int axis = 1;
    float base = 0.0f;
    float sign = 1.0f;
    float t = 0.0f;
    int face = -1;
    int hit = hovered ? editor_raycast(origin, direction, RAYCAST_ALL, &t, &face) : -1;
    Vec3 surface_point = vec3_add(origin, vec3_scale(direction, t));

    if (hit >= 0 && face >= 0)
    {
        // On a face that's aligned with an axis : drawn on it, rising out of it.
        const SceneObject* object = &editor_scene.objects[hit];
        const EditMesh* mesh = mesh_of(object);
        float normal[3];
        edit_mesh_face_normal(mesh, face, normal);

        Mat4 matrix = scene_object_matrix(object);
        float world_normal[4];
        Mat4 normal_matrix = mat4_transpose(mat4_inverse(matrix));
        mat4_transform(normal_matrix, normal[0], normal[1], normal[2], 0.0f, world_normal);
        Vec3 n = vec3_normalize(vec3(world_normal[0], world_normal[1], world_normal[2]));

        for (int k = 0; k < 3; k++)
        {
            if (SDL_fabsf(axis_of(n, k)) > 0.99f)
            {
                axis = k;
                sign = axis_of(n, k) > 0.0f ? 1.0f : -1.0f;
                base = axis_of(surface_point, k);
            }
        }
    }
    else if (hit >= 0)
    {
        // A model : on top of it, at the height where the ray hit it.
        base = surface_point.y;
    }

    Vec3 point;
    bool on_plane = hovered && ray_plane(origin, direction, axis, base, &point);

    if (on_plane)
    {
        int u = axis == 0 ? 1 : 0;
        int v = axis == 2 ? 1 : 2;
        set_axis(&point, u, snap(axis_of(point, u)));
        set_axis(&point, v, snap(axis_of(point, v)));

        ImVec2 screen;

        if (project_point(camera, point, &screen))
        {
            draw_list->AddLine(ImVec2(screen.x - 6, screen.y), ImVec2(screen.x + 6, screen.y), COLOR_BLOCK, 1.5f);
            draw_list->AddLine(ImVec2(screen.x, screen.y - 6), ImVec2(screen.x, screen.y + 6), COLOR_BLOCK, 1.5f);
        }
    }

    bool clicked = hovered && ImGui::IsMouseClicked(ImGuiMouseButton_Left) && !io.KeyAlt;

    if (clicked && block.stage == BLOCK_PENDING)
    {
        int handle = hovered_block_handle(camera, mouse);

        if (handle >= 0)
        {
            block.handle = handle;
            draw_block(draw_list, camera, mouse);
            return true;
        }
    }

    if (clicked && on_plane)
    {
        // A new block (replacing the pending one, if any).
        block.stage = BLOCK_DRAWING;
        block.axis = axis;
        block.base = base;
        block.rise = sign * block_rise;
        block.start = point;
        block.handle = -1;
        block_extent_from_drawing(point);
    }

    draw_block(draw_list, camera, mouse);

    return block.stage == BLOCK_DRAWING;
}

// --- Clipping tool ----------------------------------------------------------------------------------

enum { CLIP_NONE, CLIP_DRAWING, CLIP_READY };

typedef struct
{
    int stage;
    int view;          // -1 : drawn in the 3D view ; 0 .. 2 : in that 2D view
    ImVec2 start;      // 3D view : the line on screen
    ImVec2 end;
    Vec3 a;            // 2D views : its ends in the world
    Vec3 b;
    float plane[4];    // n.xyz, d : what's in front has n . x > d
} ClipState;

static ClipState clip;

static void clip_from_screen(const CameraMatrices* camera)
{
    // The plane through both rays : it slices along the line, as seen from the camera.
    Vec3 o0, d0, o1, d1;
    editor_mouse_ray(camera, clip.start, &o0, &d0);
    editor_mouse_ray(camera, clip.end, &o1, &d1);

    Vec3 n = vec3_normalize(vec3_cross(vec3_sub(o1, o0), d0));
    clip.plane[0] = n.x;
    clip.plane[1] = n.y;
    clip.plane[2] = n.z;
    clip.plane[3] = vec3_dot(n, o0);
}

static float plane_distance(const float plane[4], Vec3 p)
{
    return plane[0] * p.x + plane[1] * p.y + plane[2] * p.z - plane[3];
}

// The color of what's on one side of the plane, for the preview.
static ImU32 clip_side_color(bool front)
{
    if (editor_clip_mode == CLIP_KEEP_BOTH)
    {
        return front ? COLOR_KEEP : COLOR_OTHER_PART;
    }

    return front == (editor_clip_mode == CLIP_KEEP_FRONT) ? COLOR_KEEP : COLOR_CUT;
}

// The selected meshes' edges, colored by the side they end up on.
static void draw_clip_preview(ImDrawList* draw_list, const CameraMatrices* camera)
{
    if (clip.stage != CLIP_READY)
    {
        return;
    }

    for (int i = 0; i < editor_scene.count; i++)
    {
        const SceneObject* object = &editor_scene.objects[i];
        const EditMesh* mesh = mesh_of(object);

        if (!object->selected || !mesh || hidden_now(object))
        {
            continue;
        }

        Mat4 matrix = scene_object_matrix(object);

        for (int e = 0; e < mesh->edge_count; e++)
        {
            Vec3 a = transform_point(matrix, to_vec3(edit_mesh_position(mesh, mesh->edges[e].a)));
            Vec3 b = transform_point(matrix, to_vec3(edit_mesh_position(mesh, mesh->edges[e].b)));
            float da = plane_distance(clip.plane, a);
            float db = plane_distance(clip.plane, b);

            if ((da >= 0.0f) == (db >= 0.0f))
            {
                draw_segment(draw_list, camera, a, b, clip_side_color(da >= 0.0f), 2.0f);
                continue;
            }

            Vec3 cut = vec3_add(a, vec3_scale(vec3_sub(b, a), da / (da - db)));
            draw_segment(draw_list, camera, a, cut, clip_side_color(da >= 0.0f), 2.0f);
            draw_segment(draw_list, camera, cut, b, clip_side_color(db >= 0.0f), 2.0f);

            ImVec2 screen;

            if (project_point(camera, cut, &screen))
            {
                draw_list->AddCircleFilled(screen, 3.0f, COLOR_CUT);
            }
        }
    }
}

static void apply_clip(void)
{
    if (clip.stage != CLIP_READY)
    {
        return;
    }

    static int meshes[SCENE_MAX_OBJECTS];
    int count = selected_meshes(meshes);

    if (count == 0)
    {
        editor_set_status("Clipping : select the meshes to cut first.");
        return;
    }

    editor_begin_edit();

    int cut = 0;
    int original_count = editor_scene.count;
    static unsigned char doomed[SCENE_MAX_OBJECTS];
    SDL_memset(doomed, 0, sizeof(doomed));

    for (int k = 0; k < count; k++)
    {
        SceneObject* object = &editor_scene.objects[meshes[k]];
        const EditMesh* mesh = mesh_of(object);
        Mat4 matrix = scene_object_matrix(object);

        // The plane in the mesh's space : n . (A x + t) = d  ->  (A^T n) . x = d - n . t.
        float local[4];
        float n[3] = { clip.plane[0], clip.plane[1], clip.plane[2] };
        float t[3] = { matrix.m[3], matrix.m[7], matrix.m[11] };

        for (int c = 0; c < 3; c++)
        {
            local[c] = matrix.m[0 * 4 + c] * n[0] + matrix.m[1 * 4 + c] * n[1] + matrix.m[2 * 4 + c] * n[2];
        }

        float length = SDL_sqrtf(local[0] * local[0] + local[1] * local[1] + local[2] * local[2]);

        if (length < 1e-9f)
        {
            continue;
        }

        local[3] = (clip.plane[3] - (n[0] * t[0] + n[1] * t[1] + n[2] * t[2])) / length;
        local[0] /= length;
        local[1] /= length;
        local[2] /= length;

        EditMesh* front = NULL;
        EditMesh* back = NULL;
        edit_mesh_clip(mesh, local, &front, &back);

        if (!front || !back)
        {
            // Not across this mesh : only what's kept matters.
            bool keeps = editor_clip_mode == CLIP_KEEP_BOTH || (front && editor_clip_mode == CLIP_KEEP_FRONT) || (back && editor_clip_mode == CLIP_KEEP_BACK);
            edit_mesh_destroy(front);
            edit_mesh_destroy(back);
            doomed[meshes[k]] = !keeps;
            cut += !keeps;
            continue;
        }

        cut++;

        if (editor_clip_mode == CLIP_KEEP_FRONT)
        {
            object->mesh = edit_mesh_store_add(front);
            edit_mesh_destroy(back);
        }
        else if (editor_clip_mode == CLIP_KEEP_BACK)
        {
            object->mesh = edit_mesh_store_add(back);
            edit_mesh_destroy(front);
        }
        else
        {
            object->mesh = edit_mesh_store_add(front);

            if (editor_scene.count < SCENE_MAX_OBJECTS)
            {
                SceneObject other = *object;
                SDL_snprintf(other.name, sizeof(other.name), "%.56s (cut)", object->name);
                other.mesh = edit_mesh_store_add(back);
                other.selected = 1;
                editor_scene.objects[editor_scene.count++] = other;
            }
            else
            {
                edit_mesh_destroy(back);
            }
        }
    }

    // What the cut left nothing of.
    int kept = 0;

    for (int i = 0; i < editor_scene.count; i++)
    {
        if (i < original_count && doomed[i])
        {
            continue;
        }

        editor_scene.objects[kept++] = editor_scene.objects[i];
    }

    editor_scene.count = kept;
    editor_scene.selected = -1;

    for (int i = 0; i < editor_scene.count && editor_scene.selected < 0; i++)
    {
        if (editor_scene.objects[i].selected)
        {
            editor_scene.selected = i;
        }
    }

    editor_set_status("Clipped %d mesh%s (%s).", cut, cut == 1 ? "" : "es", clip_mode_names[editor_clip_mode]);
    clip.stage = CLIP_NONE;
}

static bool clip_tool_3d(const CameraMatrices* camera, bool hovered, ImDrawList* draw_list)
{
    ImGuiIO& io = ImGui::GetIO();

    if (hovered && ImGui::IsMouseClicked(ImGuiMouseButton_Left) && !io.KeyAlt)
    {
        clip.stage = CLIP_DRAWING;
        clip.view = -1;
        clip.start = clip.end = io.MousePos;
    }

    if (clip.stage == CLIP_DRAWING && clip.view < 0)
    {
        clip.end = io.MousePos;
        draw_list->AddLine(clip.start, clip.end, COLOR_CUT, 2.0f);

        if (!ImGui::IsMouseDown(ImGuiMouseButton_Left))
        {
            if (distance_2d(clip.start, clip.end) > DRAG_THRESHOLD)
            {
                clip_from_screen(camera);
                clip.stage = CLIP_READY;
            }
            else
            {
                clip.stage = CLIP_NONE;
            }
        }

        return true;
    }

    if (clip.stage == CLIP_READY && clip.view < 0)
    {
        draw_list->AddLine(clip.start, clip.end, IM_COL32(255, 80, 70, 120), 1.0f);
    }

    draw_clip_preview(draw_list, camera);

    return false;
}

// --- Selection tool in the 3D view ------------------------------------------------------------------

static bool press_pending;
static ImVec2 press_position;
static bool press_ctrl;
static Pick press_pick;
static bool marquee_active;

static void apply_marquee_3d(const CameraMatrices* camera, ImVec2 a, ImVec2 b, bool ctrl)
{
    ImVec2 lo(SDL_min(a.x, b.x), SDL_min(a.y, b.y));
    ImVec2 hi(SDL_max(a.x, b.x), SDL_max(a.y, b.y));

    if (!component_mode())
    {
        if (!ctrl)
        {
            editor_select_none();
        }

        for (int i = 0; i < editor_scene.count; i++)
        {
            const SceneObject* object = &editor_scene.objects[i];

            if (hidden_now(object) || (editor_selection_mode == SELECT_MESHES && !object->mesh))
            {
                continue;
            }

            float bounds[6];
            editor_object_bounds(object, bounds);
            Vec3 center = transform_point(scene_object_matrix(object), vec3((bounds[0] + bounds[3]) * 0.5f, (bounds[1] + bounds[4]) * 0.5f, (bounds[2] + bounds[5]) * 0.5f));
            ImVec2 screen;

            if (project_point(camera, center, &screen) && screen.x >= lo.x && screen.x <= hi.x && screen.y >= lo.y && screen.y <= hi.y)
            {
                editor_select_object(i, SELECT_ADD);
            }
        }

        return;
    }

    int element = mode_element();

    if (!ctrl)
    {
        clear_components(element);
    }

    // The selected meshes' visible elements ; with no mesh selected, every mesh's.
    static int meshes[SCENE_MAX_OBJECTS];
    int count = selected_meshes(meshes);
    bool any_mesh = count > 0;

    if (!any_mesh)
    {
        for (int i = 0; i < editor_scene.count; i++)
        {
            if (editor_scene.objects[i].mesh && !hidden_now(&editor_scene.objects[i]))
            {
                meshes[count++] = i;
            }
        }
    }

    for (int k = 0; k < count; k++)
    {
        SceneObject* object = &editor_scene.objects[meshes[k]];
        EditMesh* mesh = mesh_of(object);
        Mat4 matrix = scene_object_matrix(object);
        Mat4 inverse = mat4_inverse(matrix);
        int found = 0;

        for (int index = 0; index < element_count(mesh, element); index++)
        {
            Vec3 world = transform_point(matrix, element_position(mesh, element, index));
            ImVec2 screen;

            if (project_point(camera, world, &screen) && screen.x >= lo.x && screen.x <= hi.x && screen.y >= lo.y && screen.y <= hi.y &&
                visible_on_mesh(camera, mesh, inverse, world))
            {
                set_element(mesh, element, index, 1);
                found++;
            }
        }

        if (found && !object->selected)
        {
            editor_select_object(meshes[k], SELECT_ADD);
        }
    }
}

static bool select_tool_3d(const CameraMatrices* camera, bool hovered, ImDrawList* draw_list)
{
    ImGuiIO& io = ImGui::GetIO();

    if (run_gizmo(camera))
    {
        draw_selection(draw_list, camera, no_pick);
        return true;
    }

    bool over_gizmo = gizmo_drawn && ImGuizmo::IsOver();
    Pick hover = no_pick;

    if (hovered && !marquee_active && !over_gizmo && !io.KeyAlt && !editor_camera_dragging())
    {
        hover = pick_3d(camera, io.MousePos);
    }

    draw_selection(draw_list, camera, hover);

    if (hovered && ImGui::IsMouseClicked(ImGuiMouseButton_Left) && !io.KeyAlt && !over_gizmo)
    {
        press_pending = true;
        press_position = io.MousePos;
        press_ctrl = io.KeyCtrl;
        press_pick = hover;
    }

    if (press_pending)
    {
        if (!ImGui::IsMouseDown(ImGuiMouseButton_Left))
        {
            // A click.
            if (component_mode())
            {
                click_element(press_pick, press_ctrl);
            }
            else
            {
                click_object(press_pick, press_ctrl);
            }

            press_pending = false;
        }
        else if (distance_2d(io.MousePos, press_position) > DRAG_THRESHOLD && press_pick.object < 0)
        {
            marquee_active = true;
            press_pending = false;
        }
    }

    if (marquee_active)
    {
        ImVec2 lo(SDL_min(press_position.x, io.MousePos.x), SDL_min(press_position.y, io.MousePos.y));
        ImVec2 hi(SDL_max(press_position.x, io.MousePos.x), SDL_max(press_position.y, io.MousePos.y));
        draw_list->AddRectFilled(lo, hi, IM_COL32(110, 215, 255, 30));
        draw_list->AddRect(lo, hi, COLOR_HOVER, 0.0f, 1.0f);

        if (!ImGui::IsMouseDown(ImGuiMouseButton_Left))
        {
            apply_marquee_3d(camera, press_position, io.MousePos, press_ctrl);
            marquee_active = false;
        }

        return true;
    }

    return false;
}

// --- The 3D view ------------------------------------------------------------------------------------

static void draw_hud(ImDrawList* draw_list)
{
    ImVec2 area_min;
    ImVec2 area_size;
    editor_view_rect(&area_min, &area_size);

    static const char* const tool_names[3] = { "Select", "Block", "Clip" };
    char line[192];
    SDL_snprintf(line, sizeof(line), "%s %s   |   %s   |   Grid %g m%s", mode_icons[editor_selection_mode], mode_names[editor_selection_mode],
        tool_names[editor_tool], editor_grid_size, editor_grid_snap ? "" : " (no snap)");

    const char* hint = NULL;

    if (editor_tool == TOOL_BLOCK)
    {
        hint = block.stage == BLOCK_PENDING ? "Pull the handles  -  Enter : create  -  Escape : cancel" : "Drag on the grid or on a face";
    }
    else if (editor_tool == TOOL_CLIP)
    {
        hint = clip.stage == CLIP_READY ? "Enter : clip  -  Shift+X : what's kept  -  Escape : cancel" : "Drag a line across the selected meshes";
    }
    else if (editor_selection_mode == SELECT_FACES)
    {
        hint = "Shift + drag : extrude";
    }
    else if (editor_selection_mode == SELECT_EDGES)
    {
        hint = "Shift + drag a border edge : extend";
    }
    else if (editor_selection_mode == SELECT_MESHES)
    {
        hint = "Shift + drag : clone";
    }

    char text[320];
    SDL_snprintf(text, sizeof(text), "%s%s%s%s%s", line, hint ? "\n" : "", hint ? hint : "",
        editor_tool == TOOL_CLIP ? "\n" : "", editor_tool == TOOL_CLIP ? clip_mode_names[editor_clip_mode] : "");

    // At the top of the view, centered : out of the way of the panels on its sides.
    ImVec2 size = ImGui::CalcTextSize(text);
    ImVec2 position(area_min.x + (area_size.x - size.x) * 0.5f, area_min.y + 8.0f);
    draw_list->AddRectFilled(ImVec2(position.x - 6, position.y - 4), ImVec2(position.x + size.x + 6, position.y + size.y + 4), IM_COL32(0, 0, 0, 140), 4.0f);
    draw_list->AddText(position, IM_COL32(255, 255, 255, 220), text);
}

static bool viewport_hovered; // this frame : the 3D view is under the mouse

bool level_viewport(const CameraMatrices* camera, bool hovered)
{
    ImDrawList* draw_list = ImGui::GetWindowDrawList();
    viewport_hovered = hovered;

    if (editor_show_grid)
    {
        bool drawing = editor_tool == TOOL_BLOCK && block.stage == BLOCK_DRAWING;
        draw_grid(draw_list, camera, drawing ? block.axis : 1, drawing ? block.base : 0.0f);
    }

    bool busy = false;

    if (editor_tool == TOOL_BLOCK)
    {
        draw_selection(draw_list, camera, no_pick);
        busy = block_tool_3d(camera, hovered, draw_list);
    }
    else if (editor_tool == TOOL_CLIP)
    {
        draw_selection(draw_list, camera, no_pick);
        busy = clip_tool_3d(camera, hovered, draw_list);
    }
    else
    {
        busy = select_tool_3d(camera, hovered, draw_list);
    }

    draw_hud(draw_list);

    return !busy;
}

// --- State ------------------------------------------------------------------------------------------

void level_editor_init(void)
{
    edit_mesh_default_shape(&editor_block_shape, EDIT_SHAPE_BOX);
    level_editor_reset();
}

void level_editor_reset(void)
{
    block.stage = BLOCK_NONE;
    block.handle = -1;
    clip.stage = CLIP_NONE;
    press_pending = false;
    marquee_active = false;
    moved_vertex_count = 0;
    moved_object_count = 0;
    gesture_creates = false;
}

const char* level_selection_mode_name(int mode)
{
    return mode >= 0 && mode < SELECTION_MODE_COUNT ? mode_names[mode] : "?";
}

void level_set_selection_mode(int mode)
{
    editor_selection_mode = SDL_clamp(mode, 0, SELECTION_MODE_COUNT - 1);
    press_pending = false;
    marquee_active = false;
}

void level_set_tool(int tool)
{
    editor_tool = tool;
    block.stage = BLOCK_NONE;
    block.handle = -1;
    clip.stage = CLIP_NONE;
}

void level_change_grid(int direction)
{
    int current = 0;

    for (int i = 0; i < GRID_SIZE_COUNT; i++)
    {
        if (SDL_fabsf(grid_sizes[i] - editor_grid_size) < SDL_fabsf(grid_sizes[current] - editor_grid_size))
        {
            current = i;
        }
    }

    editor_grid_size = grid_sizes[SDL_clamp(current + direction, 0, GRID_SIZE_COUNT - 1)];
    editor_set_status("Grid : %g m", editor_grid_size);
}

// --- Operations -------------------------------------------------------------------------------------

static void report(const char* what, int count)
{
    if (count > 0)
    {
        editor_set_status("%s (%d).", what, count);
    }
    else
    {
        editor_set_status("%s : nothing to do with this selection.", what);
    }
}

// Moves the selected faces of a mesh along their average normal.
static void push_selected_faces(EditMesh* mesh, float distance)
{
    float normal[3] = { 0.0f, 0.0f, 0.0f };

    for (int f = 0; f < mesh->face_count; f++)
    {
        if (mesh->faces[f].selected)
        {
            float n[3];
            edit_mesh_face_normal(mesh, f, n);
            normal[0] += n[0];
            normal[1] += n[1];
            normal[2] += n[2];
        }
    }

    float length = SDL_sqrtf(normal[0] * normal[0] + normal[1] * normal[1] + normal[2] * normal[2]);

    if (length < 1e-6f)
    {
        return;
    }

    unsigned char* flags = (unsigned char*)SDL_malloc((size_t)SDL_max(1, mesh->vertex_count));
    edit_mesh_selection_vertices(mesh, EDIT_ELEMENT_FACE, flags);

    for (int v = 0; v < mesh->vertex_count; v++)
    {
        if (flags[v])
        {
            for (int c = 0; c < 3; c++)
            {
                mesh->positions[v * 3 + c] += normal[c] / length * distance;
            }
        }
    }

    SDL_free(flags);
    edit_mesh_touch(mesh);
}

static void merge_meshes(int* meshes, int count)
{
    if (count < 2)
    {
        editor_set_status("Merge : select two meshes or more.");
        return;
    }

    editor_begin_edit();

    int target_index = editor_scene.selected >= 0 && editor_scene.objects[editor_scene.selected].mesh ? editor_scene.selected : meshes[0];
    SceneObject* target = &editor_scene.objects[target_index];
    EditMesh* merged = editor_writable_mesh(target);
    Mat4 to_target = mat4_inverse(scene_object_matrix(target));
    static unsigned char merged_away[SCENE_MAX_OBJECTS];
    SDL_memset(merged_away, 0, sizeof(merged_away));

    for (int k = 0; k < count; k++)
    {
        if (meshes[k] == target_index)
        {
            continue;
        }

        SceneObject* other = &editor_scene.objects[meshes[k]];
        Mat4 matrix = mat4_multiply(to_target, scene_object_matrix(other));
        edit_mesh_append(merged, mesh_of(other), matrix.m);
        merged_away[meshes[k]] = 1;
    }

    int kept = 0;

    for (int i = 0; i < editor_scene.count; i++)
    {
        if (!merged_away[i])
        {
            if (i == target_index)
            {
                target_index = kept;
            }

            editor_scene.objects[kept++] = editor_scene.objects[i];
        }
    }

    editor_scene.count = kept;
    editor_select_object(target_index, SELECT_REPLACE);
    editor_set_status("Merged %d meshes into '%s'.", count, editor_scene.objects[target_index].name);
}

void level_mesh_operation(int operation, float amount)
{
    if (editor_is_playing())
    {
        return;
    }

    static int meshes[SCENE_MAX_OBJECTS];
    int count = selected_meshes(meshes);
    int element = mode_element();
    bool components = component_mode();

    // Selection only : no undo step, no copy.
    if (operation >= MESH_OP_SELECT_LOOP)
    {
        if (operation == MESH_OP_SELECT_ALL && !components)
        {
            for (int i = 0; i < editor_scene.count; i++)
            {
                const SceneObject* object = &editor_scene.objects[i];

                if (!hidden_now(object) && (editor_selection_mode == SELECT_OBJECTS || object->mesh))
                {
                    editor_select_object(i, SELECT_ADD);
                }
            }

            return;
        }

        int changed = 0;

        for (int k = 0; k < count && components; k++)
        {
            EditMesh* mesh = mesh_of(&editor_scene.objects[meshes[k]]);

            switch (operation)
            {
            case MESH_OP_SELECT_LOOP: changed += element == EDIT_ELEMENT_EDGE ? edit_mesh_select_loop(mesh) : 0; break;
            case MESH_OP_SELECT_RING: changed += element == EDIT_ELEMENT_EDGE ? edit_mesh_select_ring(mesh) : 0; break;
            case MESH_OP_GROW: changed += edit_mesh_grow_selection(mesh, element); break;
            case MESH_OP_SHRINK: changed += edit_mesh_shrink_selection(mesh, element); break;
            case MESH_OP_SELECT_ALL: edit_mesh_select_all(mesh, element, 1); changed++; break;
            case MESH_OP_INVERT: edit_mesh_invert_selection(mesh, element); changed++; break;
            default: break;
            }
        }

        (void)changed;
        return;
    }

    if (count == 0)
    {
        editor_set_status("Select a mesh first.");
        return;
    }

    if (operation == MESH_OP_MERGE_MESHES)
    {
        merge_meshes(meshes, count);
        return;
    }

    editor_begin_edit();

    int changed = 0;
    const char* name = "";

    for (int k = 0; k < count; k++)
    {
        SceneObject* object = &editor_scene.objects[meshes[k]];

        // Only the meshes that have something selected, in the component modes.
        if (components && edit_mesh_selection_count(mesh_of(object), element) == 0 && operation != MESH_OP_HOLLOW && operation != MESH_OP_CENTER_ORIGIN)
        {
            continue;
        }

        EditMesh* mesh = editor_writable_mesh(object);
        unsigned char* filter = NULL;

        if (components)
        {
            filter = (unsigned char*)SDL_malloc((size_t)SDL_max(1, mesh->vertex_count));
            edit_mesh_selection_vertices(mesh, element, filter);
        }

        switch (operation)
        {
        case MESH_OP_EXTRUDE:
            name = "Extrude";

            if (element == EDIT_ELEMENT_FACE && edit_mesh_extrude_faces(mesh))
            {
                push_selected_faces(mesh, amount);
                changed++;
            }
            else if (element == EDIT_ELEMENT_EDGE)
            {
                changed += edit_mesh_extrude_edges(mesh);
            }

            break;

        case MESH_OP_INSET:
            name = "Inset";
            changed += element == EDIT_ELEMENT_FACE ? edit_mesh_inset_faces(mesh, amount) : 0;
            break;

        case MESH_OP_FLIP:
            name = "Flip";
            changed += edit_mesh_flip_faces(mesh, components && element == EDIT_ELEMENT_FACE);
            break;

        case MESH_OP_DELETE:
        case MESH_OP_DISSOLVE:
            name = element == EDIT_ELEMENT_FACE ? "Delete" : "Dissolve";
            changed += element == EDIT_ELEMENT_FACE ? edit_mesh_delete_faces(mesh) :
                element == EDIT_ELEMENT_EDGE ? edit_mesh_dissolve_edges(mesh) : edit_mesh_dissolve_vertices(mesh);
            break;

        case MESH_OP_DETACH:
        {
            name = "Detach";
            EditMesh* part = element == EDIT_ELEMENT_FACE ? edit_mesh_detach_faces(mesh) : NULL;

            if (part && editor_scene.count < SCENE_MAX_OBJECTS)
            {
                SceneObject copy = *object;
                SDL_snprintf(copy.name, sizeof(copy.name), "%.56s part", object->name);
                copy.mesh = edit_mesh_store_add(part);
                copy.selected = 0;
                editor_scene.objects[editor_scene.count++] = copy;
                changed++;
            }
            else
            {
                edit_mesh_destroy(part);
            }

            break;
        }

        case MESH_OP_BRIDGE:
            name = "Bridge";
            changed += element == EDIT_ELEMENT_FACE ? edit_mesh_bridge_faces(mesh) : 0;
            break;

        case MESH_OP_MERGE:
            name = "Merge";
            changed += element == EDIT_ELEMENT_VERTEX ? edit_mesh_merge_vertices(mesh) : edit_mesh_collapse(mesh, element);
            break;

        case MESH_OP_WELD:
            name = "Weld";
            changed += edit_mesh_weld(mesh, amount, element == EDIT_ELEMENT_VERTEX && components);
            break;

        case MESH_OP_COLLAPSE:
            name = "Collapse";
            changed += edit_mesh_collapse(mesh, element);
            break;

        case MESH_OP_SPLIT:
            name = "Split";
            changed += element == EDIT_ELEMENT_EDGE ? edit_mesh_split_edges(mesh) : 0;
            break;

        case MESH_OP_CONNECT:
            name = "Connect";
            changed += element != EDIT_ELEMENT_FACE ? edit_mesh_connect(mesh, element) : 0;
            break;

        case MESH_OP_FILL:
            name = "Fill holes";
            changed += edit_mesh_fill_holes(mesh, filter);
            break;

        case MESH_OP_HOLLOW:
            name = "Hollow";
            changed += edit_mesh_thicken(mesh, amount);
            break;

        case MESH_OP_SNAP:
        {
            name = "Snap to grid";
            Mat4 matrix = scene_object_matrix(object);
            changed += edit_mesh_snap_to_grid(mesh, matrix.m, editor_grid_size, filter) > 0;
            break;
        }

        case MESH_OP_CENTER_ORIGIN:
        {
            // The object's origin moved to its mesh's center, the mesh staying where it is in the world.
            name = "Center origin";
            float bounds[6];
            edit_mesh_bounds(mesh, bounds);
            float center[3] = { (bounds[0] + bounds[3]) * 0.5f, (bounds[1] + bounds[4]) * 0.5f, (bounds[2] + bounds[5]) * 0.5f };
            Mat4 matrix = scene_object_matrix(object);
            Vec3 world = transform_point(matrix, to_vec3(center));
            edit_mesh_translate(mesh, center);
            object->position[0] = world.x;
            object->position[1] = world.y;
            object->position[2] = world.z;
            changed++;
            break;
        }

        default:
            break;
        }

        SDL_free(filter);
    }

    report(name, changed);
}

void level_apply_material(const char* material)
{
    SDL_snprintf(editor_active_material, sizeof(editor_active_material), "%s", material);

    if (editor_is_playing())
    {
        return;
    }

    static int meshes[SCENE_MAX_OBJECTS];
    int count = selected_meshes(meshes);
    bool faces_only = editor_selection_mode == SELECT_FACES;
    int changed = 0;

    if (count == 0)
    {
        editor_set_status("Active material : %s", material);
        return;
    }

    editor_begin_edit();

    for (int k = 0; k < count; k++)
    {
        SceneObject* object = &editor_scene.objects[meshes[k]];

        if (faces_only && edit_mesh_selection_count(mesh_of(object), EDIT_ELEMENT_FACE) == 0)
        {
            continue;
        }

        EditMesh* mesh = editor_writable_mesh(object);
        int index = edit_mesh_material(mesh, material);

        for (int f = 0; f < mesh->face_count; f++)
        {
            if (!faces_only || mesh->faces[f].selected)
            {
                mesh->faces[f].style.material = index;
                changed++;
            }
        }

        edit_mesh_touch(mesh);
    }

    report("Material applied", changed);
}

void level_hide_selected(bool hide_unselected)
{
    editor_begin_edit();

    for (int i = 0; i < editor_scene.count; i++)
    {
        SceneObject* object = &editor_scene.objects[i];

        if (hide_unselected ? !object->selected : object->selected)
        {
            object->hidden = 1;
            object->selected = 0;
        }
    }

    editor_scene.selected = -1;

    for (int i = 0; i < editor_scene.count && editor_scene.selected < 0; i++)
    {
        if (editor_scene.objects[i].selected)
        {
            editor_scene.selected = i;
        }
    }
}

void level_unhide_all(void)
{
    editor_begin_edit();

    for (int i = 0; i < editor_scene.count; i++)
    {
        editor_scene.objects[i].hidden = 0;
    }
}

SceneObject* level_add_block(int shape)
{
    EditShapeParams params;
    edit_mesh_default_shape(&params, shape);

    const float size[3] = { 2.0f, 2.0f, 2.0f };
    EditMesh* mesh = edit_mesh_make_shape(&params, size, editor_active_material);
    SceneObject* object = editor_add_mesh(mesh, edit_mesh_shape_name(shape));

    if (object)
    {
        editor_drop_point(object->position);
        object->position[0] = SDL_roundf(object->position[0] / editor_grid_size) * editor_grid_size;
        object->position[2] = SDL_roundf(object->position[2] / editor_grid_size) * editor_grid_size;
        object->position[1] = size[1] * 0.5f; // resting on the ground
    }

    return object;
}

// The selection moved by a world offset : elements in the component modes, objects otherwise.
static void nudge(Vec3 offset)
{
    if (editor_selection_count() == 0)
    {
        return;
    }

    begin_move(false);
    apply_move(translation(offset));
    end_move(translation(offset));
}

// --- Shortcuts --------------------------------------------------------------------------------------

static bool positional_pressed(Scancode scancode)
{
    static bool previous[SCANCODE_COUNT];

    bool down = input_is_key_down(scancode) != 0;
    bool pressed = down && !previous[scancode];
    previous[scancode] = down;

    return pressed;
}

static int hovered_view = -1; // the 2D view under the mouse, this frame
static void focus_2d_views(const float bounds[6]);

void level_shortcuts(bool active)
{
    // Sampled every frame, whatever happens next : their pressed / held tracking stays current.
    bool mode_pressed[SELECTION_MODE_COUNT];

    for (int i = 0; i < SELECTION_MODE_COUNT; i++)
    {
        mode_pressed[i] = positional_pressed((Scancode)(SCANCODE_1 + i));
    }

    bool translate_pressed = positional_pressed(KEY_GIZMO_TRANSLATE);
    bool rotate_pressed = positional_pressed(KEY_GIZMO_ROTATE);
    bool scale_pressed = positional_pressed(KEY_GIZMO_SCALE);
    bool finer_pressed = positional_pressed(KEY_GRID_FINER);
    bool coarser_pressed = positional_pressed(KEY_GRID_COARSER);

    if (!active)
    {
        return;
    }

    ImGuiIO& io = ImGui::GetIO();
    bool plain = !io.KeyCtrl && !io.KeyAlt;

    if (plain && !io.KeyShift)
    {
        for (int i = 0; i < SELECTION_MODE_COUNT; i++)
        {
            if (mode_pressed[i])
            {
                level_set_selection_mode(i);
            }
        }

        if (translate_pressed) editor_gizmo_operation = ImGuizmo::TRANSLATE;
        if (rotate_pressed) editor_gizmo_operation = ImGuizmo::ROTATE;
        if (scale_pressed) editor_gizmo_operation = ImGuizmo::SCALE;
    }

    if (plain)
    {
        if (finer_pressed) level_change_grid(-1);
        if (coarser_pressed) level_change_grid(1);
    }

    if (ImGui::IsKeyChordPressed(ImGuiMod_Shift | ImGuiKey_B))
    {
        level_set_tool(editor_tool == TOOL_BLOCK ? TOOL_SELECT : TOOL_BLOCK);
    }

    if (ImGui::IsKeyChordPressed(ImGuiMod_Shift | ImGuiKey_X))
    {
        if (editor_tool == TOOL_CLIP)
        {
            editor_clip_mode = (editor_clip_mode + 1) % 3; // Hammer : again cycles what's kept
        }
        else
        {
            level_set_tool(TOOL_CLIP);
        }
    }

    // Space confirms too (Hammer), except over a 2D view, where it pans.
    bool confirm = ImGui::IsKeyPressed(ImGuiKey_Enter, false) || ImGui::IsKeyPressed(ImGuiKey_KeypadEnter, false) ||
        (plain && !io.KeyShift && hovered_view < 0 && ImGui::IsKeyPressed(ImGuiKey_Space, false));

    if (confirm && editor_tool == TOOL_BLOCK)
    {
        create_block();
    }
    else if (confirm && editor_tool == TOOL_CLIP)
    {
        apply_clip();
    }

    if (ImGui::IsKeyPressed(ImGuiKey_Escape, false))
    {
        if (editor_tool == TOOL_BLOCK && block.stage != BLOCK_NONE)
        {
            block.stage = BLOCK_NONE;
        }
        else if (editor_tool == TOOL_CLIP && clip.stage != CLIP_NONE)
        {
            clip.stage = CLIP_NONE;
        }
        else if (editor_tool != TOOL_SELECT)
        {
            level_set_tool(TOOL_SELECT);
        }
        else if (component_mode())
        {
            clear_components(mode_element());
        }
        else
        {
            editor_select_none();
        }
    }

    if (!plain)
    {
        if (ImGui::IsKeyChordPressed(ImGuiMod_Ctrl | ImGuiKey_A))
        {
            level_mesh_operation(MESH_OP_SELECT_ALL, 0.0f);
        }

        return;
    }

    if (ImGui::IsKeyPressed(ImGuiKey_Delete, false))
    {
        if (component_mode())
        {
            level_mesh_operation(MESH_OP_DELETE, 0.0f);
        }
        else
        {
            editor_delete_selected();
        }
    }

    if (ImGui::IsKeyPressed(ImGuiKey_F, false) && !io.KeyShift)
    {
        float bounds[6];

        if (component_mode() && selection_bounds(bounds))
        {
            editor_focus_on(bounds);
            focus_2d_views(bounds);
        }
        else
        {
            editor_focus_selected();
        }
    }

    if (ImGui::IsKeyPressed(ImGuiKey_H, false))
    {
        level_hide_selected(io.KeyShift);
    }

    if (ImGui::IsKeyPressed(ImGuiKey_U, false) && !io.KeyShift)
    {
        level_unhide_all();
    }

    if (ImGui::IsKeyPressed(ImGuiKey_M, false) && !io.KeyShift && component_mode())
    {
        level_mesh_operation(MESH_OP_MERGE, 0.0f);
    }

    if (ImGui::IsKeyPressed(ImGuiKey_L, false) && !io.KeyShift && editor_selection_mode == SELECT_EDGES)
    {
        level_mesh_operation(MESH_OP_SELECT_LOOP, 0.0f);
    }

    if (ImGui::IsKeyPressed(ImGuiKey_G, false) && !io.KeyShift && editor_selection_mode == SELECT_EDGES)
    {
        level_mesh_operation(MESH_OP_SELECT_RING, 0.0f);
    }

    // Nudging by one grid step : along a 2D view's axes when one is under the mouse, along the world's
    // horizontal axes closest to the camera's otherwise ; Page Up / Down : vertically. Only with the
    // mouse over a view : elsewhere, the arrows are the panels' (ImGui's keyboard navigation).
    int right = (int)ImGui::IsKeyPressed(ImGuiKey_RightArrow) - (int)ImGui::IsKeyPressed(ImGuiKey_LeftArrow);
    int up = (int)ImGui::IsKeyPressed(ImGuiKey_UpArrow) - (int)ImGui::IsKeyPressed(ImGuiKey_DownArrow);
    int vertical = (int)ImGui::IsKeyPressed(ImGuiKey_PageUp) - (int)ImGui::IsKeyPressed(ImGuiKey_PageDown);

    if ((right != 0 || up != 0 || vertical != 0) && (viewport_hovered || hovered_view >= 0))
    {
        Vec3 offset = vec3(0.0f, (float)vertical * editor_grid_size, 0.0f);
        const CameraMatrices* camera = editor_camera();

        // Camera right and forward, from the view matrix's rows.
        Vec3 camera_right = vec3(camera->view.m[0], camera->view.m[1], camera->view.m[2]);
        Vec3 camera_forward = vec3(-camera->view.m[8], 0.0f, -camera->view.m[10]);

        Vec3 axis_right = SDL_fabsf(camera_right.x) >= SDL_fabsf(camera_right.z) ? vec3(camera_right.x > 0 ? 1.0f : -1.0f, 0, 0) : vec3(0, 0, camera_right.z > 0 ? 1.0f : -1.0f);
        Vec3 axis_up = SDL_fabsf(camera_forward.x) >= SDL_fabsf(camera_forward.z) ? vec3(camera_forward.x > 0 ? 1.0f : -1.0f, 0, 0) : vec3(0, 0, camera_forward.z > 0 ? 1.0f : -1.0f);

        if (hovered_view >= 0)
        {
            static const int u_axes[3] = { 0, 0, 2 };
            static const int v_axes[3] = { 2, 1, 1 };
            static const float u_signs[3] = { 1.0f, 1.0f, -1.0f };
            static const float v_signs[3] = { -1.0f, 1.0f, 1.0f };
            axis_right = vec3(0, 0, 0);
            axis_up = vec3(0, 0, 0);
            set_axis(&axis_right, u_axes[hovered_view], u_signs[hovered_view]);
            set_axis(&axis_up, v_axes[hovered_view], v_signs[hovered_view]);
        }

        offset = vec3_add(offset, vec3_scale(axis_right, (float)right * editor_grid_size));
        offset = vec3_add(offset, vec3_scale(axis_up, (float)up * editor_grid_size));
        nudge(offset);
    }
}

// --- 2D views ---------------------------------------------------------------------------------------

typedef struct
{
    const char* name;
    int u, v;          // the world axes along the view's right and up
    float su, sv;      // their signs
    float center_u;    // what's at the view's center, in view coordinates
    float center_v;
    float zoom;        // pixels per meter
} OrthoView;

static OrthoView views[3] = {
    { "Top (X/Z)", 0, 2, 1.0f, -1.0f, 0.0f, 0.0f, 30.0f },
    { "Front (X/Y)", 0, 1, 1.0f, 1.0f, 0.0f, 0.0f, 30.0f },
    { "Side (Z/Y)", 2, 1, -1.0f, 1.0f, 0.0f, 0.0f, 30.0f }
};

static int views_shown = 3; // 0 .. 2 : that view alone ; 3 : all three

static int depth_axis(const OrthoView* view)
{
    return 3 - view->u - view->v;
}

static ImVec2 to_screen(const OrthoView* view, ImVec2 center, Vec3 p)
{
    float u = axis_of(p, view->u) * view->su;
    float v = axis_of(p, view->v) * view->sv;
    return ImVec2(center.x + (u - view->center_u) * view->zoom, center.y - (v - view->center_v) * view->zoom);
}

// The world point under a screen position, `depth` along the view's depth axis.
static Vec3 to_world(const OrthoView* view, ImVec2 center, ImVec2 s, float depth)
{
    float u = view->center_u + (s.x - center.x) / view->zoom;
    float v = view->center_v - (s.y - center.y) / view->zoom;
    Vec3 p = vec3(0.0f, 0.0f, 0.0f);
    set_axis(&p, view->u, u * view->su);
    set_axis(&p, view->v, v * view->sv);
    set_axis(&p, depth_axis(view), depth);
    return p;
}

static void focus_2d_views(const float bounds[6])
{
    Vec3 center = vec3((bounds[0] + bounds[3]) * 0.5f, (bounds[1] + bounds[4]) * 0.5f, (bounds[2] + bounds[5]) * 0.5f);
    float size = SDL_max(0.5f, SDL_max(bounds[3] - bounds[0], SDL_max(bounds[4] - bounds[1], bounds[5] - bounds[2])));

    for (int i = 0; i < 3; i++)
    {
        views[i].center_u = axis_of(center, views[i].u) * views[i].su;
        views[i].center_v = axis_of(center, views[i].v) * views[i].sv;
        views[i].zoom = SDL_clamp(200.0f / size, 2.0f, 2000.0f);
    }
}

static void draw_view_grid(ImDrawList* draw_list, const OrthoView* view, ImVec2 lo, ImVec2 hi, ImVec2 center)
{
    float spacing = editor_grid_size;

    while (spacing * view->zoom < 8.0f)
    {
        spacing *= 2.0f;
    }

    float major = SDL_max(1.0f, spacing * 8.0f);

    float u0 = view->center_u + (lo.x - center.x) / view->zoom;
    float u1 = view->center_u + (hi.x - center.x) / view->zoom;
    float v0 = view->center_v - (hi.y - center.y) / view->zoom;
    float v1 = view->center_v - (lo.y - center.y) / view->zoom;

    for (float u = SDL_floorf(u0 / spacing) * spacing; u <= u1; u += spacing)
    {
        float x = center.x + (u - view->center_u) * view->zoom;
        bool is_axis = SDL_fabsf(u) < spacing * 0.25f;
        bool is_major = SDL_fabsf(SDL_fmodf(SDL_fabsf(u) + spacing * 0.25f, major)) < spacing * 0.5f;
        ImU32 color = is_axis ? IM_COL32(110, 110, 150, 255) : is_major ? IM_COL32(72, 72, 72, 255) : IM_COL32(48, 48, 48, 255);
        draw_list->AddLine(ImVec2(x, lo.y), ImVec2(x, hi.y), color);
    }

    for (float v = SDL_floorf(v0 / spacing) * spacing; v <= v1; v += spacing)
    {
        float y = center.y - (v - view->center_v) * view->zoom;
        bool is_axis = SDL_fabsf(v) < spacing * 0.25f;
        bool is_major = SDL_fabsf(SDL_fmodf(SDL_fabsf(v) + spacing * 0.25f, major)) < spacing * 0.5f;
        ImU32 color = is_axis ? IM_COL32(110, 110, 150, 255) : is_major ? IM_COL32(72, 72, 72, 255) : IM_COL32(48, 48, 48, 255);
        draw_list->AddLine(ImVec2(lo.x, y), ImVec2(hi.x, y), color);
    }
}

static void object_rect_2d(const OrthoView* view, ImVec2 center, const SceneObject* object, ImVec2* out_lo, ImVec2* out_hi);

// --- Wireframes of models in the 2D views ---
//
// Models are flattened into separate triangles (each with its own vertices), so drawing every triangle's
// outline draws every edge twice, plus all the ones too small to see : a dense sphere, in three views,
// overflowed the renderer's per-frame vertex budget and whole parts of the UI went missing. Each model's
// unique edges are built once and cached ; edges outside the view or shorter than a pixel are skipped.

#define MODEL_EDGE_CACHE_SIZE 32
#define MODEL_EDGE_MAX_INDICES 300000 // past this, the model's bounds are drawn instead

typedef struct
{
    const void* key;        // the model's vertex array
    int index_count;
    float* points;          // unique positions, xyz
    int point_count;
    int* edges;             // pairs of indices into points
    int edge_count;
} ModelEdges;

static ModelEdges model_edges[MODEL_EDGE_CACHE_SIZE];
static int model_edges_next;     // the entry replaced next, round robin
static ImVec2 view_clip_lo;      // the 2D view being drawn, for culling
static ImVec2 view_clip_hi;

static uint64_t hash_u64(uint64_t x)
{
    x ^= x >> 33;
    x *= 0xff51afd7ed558ccdULL;
    x ^= x >> 33;
    x *= 0xc4ceb9fe1a85ec53ULL;
    x ^= x >> 33;
    return x;
}

static void free_model_edges(ModelEdges* entry)
{
    SDL_free(entry->points);
    SDL_free(entry->edges);
    SDL_zerop(entry);
}

void level_editor_free_caches(void)
{
    for (int i = 0; i < MODEL_EDGE_CACHE_SIZE; i++)
    {
        free_model_edges(&model_edges[i]);
    }

    model_edges_next = 0;
}

// Welds the model's vertices by position (to 0.1 mm), then keeps each triangle edge once.
static const ModelEdges* get_model_edges(const ModelData* data)
{
    for (int i = 0; i < MODEL_EDGE_CACHE_SIZE; i++)
    {
        if (model_edges[i].key == data->vertices && model_edges[i].index_count == data->index_count)
        {
            return &model_edges[i];
        }
    }

    ModelEdges* entry = &model_edges[model_edges_next];
    model_edges_next = (model_edges_next + 1) % MODEL_EDGE_CACHE_SIZE;
    free_model_edges(entry);

    int capacity = 1;

    while (capacity < data->index_count * 2)
    {
        capacity *= 2;
    }

    // One open-addressing table, reused : first positions -> point index, then edge keys.
    uint64_t* keys = (uint64_t*)SDL_malloc(sizeof(uint64_t) * (size_t)capacity);
    int* values = (int*)SDL_malloc(sizeof(int) * (size_t)capacity);
    int* point_of_vertex = (int*)SDL_malloc(sizeof(int) * (size_t)SDL_max(1, data->vertex_count));
    entry->points = (float*)SDL_malloc(sizeof(float) * 3 * (size_t)SDL_max(1, data->vertex_count));
    entry->edges = (int*)SDL_malloc(sizeof(int) * 2 * (size_t)SDL_max(1, data->index_count));

    for (int i = 0; i < capacity; i++)
    {
        values[i] = -1;
    }

    for (int v = 0; v < data->vertex_count; v++)
    {
        const MeshVertex* vertex = &data->vertices[v];
        int32_t q[3] = { (int32_t)SDL_lroundf(vertex->x * 1e4f), (int32_t)SDL_lroundf(vertex->y * 1e4f), (int32_t)SDL_lroundf(vertex->z * 1e4f) };
        uint64_t key = ((uint64_t)(uint32_t)q[0] * 73856093ULL) ^ ((uint64_t)(uint32_t)q[1] * 19349663ULL << 21) ^ ((uint64_t)(uint32_t)q[2] * 83492791ULL << 42);
        int slot = (int)(hash_u64(key) & (uint64_t)(capacity - 1));
        int found = -1;

        while (values[slot] >= 0)
        {
            const float* p = &entry->points[values[slot] * 3];

            if (keys[slot] == key && SDL_fabsf(p[0] - vertex->x) < 1e-4f && SDL_fabsf(p[1] - vertex->y) < 1e-4f && SDL_fabsf(p[2] - vertex->z) < 1e-4f)
            {
                found = values[slot];
                break;
            }

            slot = (slot + 1) & (capacity - 1);
        }

        if (found < 0)
        {
            found = entry->point_count++;
            entry->points[found * 3 + 0] = vertex->x;
            entry->points[found * 3 + 1] = vertex->y;
            entry->points[found * 3 + 2] = vertex->z;
            keys[slot] = key;
            values[slot] = found;
        }

        point_of_vertex[v] = found;
    }

    for (int i = 0; i < capacity; i++)
    {
        values[i] = -1;
    }

    for (int t = 0; t + 2 < data->index_count; t += 3)
    {
        for (int k = 0; k < 3; k++)
        {
            int a = point_of_vertex[data->indices[t + k]];
            int b = point_of_vertex[data->indices[t + (k + 1) % 3]];

            if (a == b)
            {
                continue;
            }

            uint64_t key = a < b ? ((uint64_t)a << 32 | (uint32_t)b) : ((uint64_t)b << 32 | (uint32_t)a);
            int slot = (int)(hash_u64(key) & (uint64_t)(capacity - 1));
            bool exists = false;

            while (values[slot] >= 0)
            {
                if (keys[slot] == key)
                {
                    exists = true;
                    break;
                }

                slot = (slot + 1) & (capacity - 1);
            }

            if (!exists)
            {
                keys[slot] = key;
                values[slot] = 1;
                entry->edges[entry->edge_count * 2 + 0] = a;
                entry->edges[entry->edge_count * 2 + 1] = b;
                entry->edge_count++;
            }
        }
    }

    SDL_free(keys);
    SDL_free(values);
    SDL_free(point_of_vertex);

    entry->key = data->vertices;
    entry->index_count = data->index_count;
    return entry;
}

// A line of a 2D view, unless it can't be seen : outside the view, or shorter than a pixel.
static void add_line_2d(ImDrawList* draw_list, ImVec2 a, ImVec2 b, ImU32 color)
{
    if ((a.x < view_clip_lo.x && b.x < view_clip_lo.x) || (a.x > view_clip_hi.x && b.x > view_clip_hi.x) ||
        (a.y < view_clip_lo.y && b.y < view_clip_lo.y) || (a.y > view_clip_hi.y && b.y > view_clip_hi.y))
    {
        return;
    }

    if (SDL_fabsf(a.x - b.x) < 1.0f && SDL_fabsf(a.y - b.y) < 1.0f)
    {
        return;
    }

    draw_list->AddLine(a, b, color);
}

static void draw_object_2d(ImDrawList* draw_list, const OrthoView* view, ImVec2 center, const SceneObject* object, ImU32 color)
{
    Mat4 matrix = scene_object_matrix(object);
    const EditMesh* mesh = mesh_of(object);

    if (mesh)
    {
        for (int e = 0; e < mesh->edge_count; e++)
        {
            ImVec2 a = to_screen(view, center, transform_point(matrix, to_vec3(edit_mesh_position(mesh, mesh->edges[e].a))));
            ImVec2 b = to_screen(view, center, transform_point(matrix, to_vec3(edit_mesh_position(mesh, mesh->edges[e].b))));
            add_line_2d(draw_list, a, b, color);
        }

        return;
    }

    const LoadedModel* model = object->model[0] ? asset_get_model(object->model) : NULL;

    if (model && model->data.index_count > 0 && model->data.index_count <= MODEL_EDGE_MAX_INDICES)
    {
        // Nothing to do when the whole model is outside the view.
        ImVec2 r_lo;
        ImVec2 r_hi;
        object_rect_2d(view, center, object, &r_lo, &r_hi);

        if (r_hi.x < view_clip_lo.x || r_lo.x > view_clip_hi.x || r_hi.y < view_clip_lo.y || r_lo.y > view_clip_hi.y)
        {
            return;
        }

        const ModelEdges* edges = get_model_edges(&model->data);
        ImVec2* projected = (ImVec2*)SDL_malloc(sizeof(ImVec2) * (size_t)SDL_max(1, edges->point_count));

        for (int p = 0; p < edges->point_count; p++)
        {
            const float* point = &edges->points[p * 3];
            projected[p] = to_screen(view, center, transform_point(matrix, vec3(point[0], point[1], point[2])));
        }

        for (int e = 0; e < edges->edge_count; e++)
        {
            add_line_2d(draw_list, projected[edges->edges[e * 2]], projected[edges->edges[e * 2 + 1]], color);
        }

        SDL_free(projected);
        return;
    }

    float bounds[6];

    if (editor_object_bounds(object, bounds))
    {
        static const int edges[12][2] = {
            { 0, 1 }, { 2, 3 }, { 4, 5 }, { 6, 7 }, { 0, 2 }, { 1, 3 }, { 4, 6 }, { 5, 7 }, { 0, 4 }, { 1, 5 }, { 2, 6 }, { 3, 7 }
        };
        ImVec2 corners[8];

        for (int i = 0; i < 8; i++)
        {
            corners[i] = to_screen(view, center, transform_point(matrix, vec3(bounds[(i & 1) ? 3 : 0], bounds[(i & 2) ? 4 : 1], bounds[(i & 4) ? 5 : 2])));
        }

        for (int e = 0; e < 12; e++)
        {
            add_line_2d(draw_list, corners[edges[e][0]], corners[edges[e][1]], color);
        }

        return;
    }

    ImVec2 p = to_screen(view, center, vec3(object->position[0], object->position[1], object->position[2]));
    draw_list->AddLine(ImVec2(p.x - 5, p.y - 5), ImVec2(p.x + 5, p.y + 5), color);
    draw_list->AddLine(ImVec2(p.x - 5, p.y + 5), ImVec2(p.x + 5, p.y - 5), color);
}

// An object's world bounds, as a rectangle in a 2D view.
static void object_rect_2d(const OrthoView* view, ImVec2 center, const SceneObject* object, ImVec2* out_lo, ImVec2* out_hi)
{
    float bounds[6];
    editor_object_bounds(object, bounds);
    Mat4 matrix = scene_object_matrix(object);
    ImVec2 lo(1e30f, 1e30f);
    ImVec2 hi(-1e30f, -1e30f);

    for (int i = 0; i < 8; i++)
    {
        ImVec2 p = to_screen(view, center, transform_point(matrix, vec3(bounds[(i & 1) ? 3 : 0], bounds[(i & 2) ? 4 : 1], bounds[(i & 4) ? 5 : 2])));
        lo = ImVec2(SDL_min(lo.x, p.x), SDL_min(lo.y, p.y));
        hi = ImVec2(SDL_max(hi.x, p.x), SDL_max(hi.y, p.y));
    }

    *out_lo = lo;
    *out_hi = hi;
}

// The object under a point of a 2D view : the smallest whose rectangle contains it.
static int pick_object_2d(const OrthoView* view, ImVec2 center, ImVec2 mouse)
{
    int best = -1;
    float best_area = 1e30f;

    for (int i = 0; i < editor_scene.count; i++)
    {
        const SceneObject* object = &editor_scene.objects[i];

        if (hidden_now(object) || (editor_selection_mode == SELECT_MESHES && !object->mesh))
        {
            continue;
        }

        ImVec2 lo;
        ImVec2 hi;
        object_rect_2d(view, center, object, &lo, &hi);

        if (mouse.x >= lo.x - 3 && mouse.x <= hi.x + 3 && mouse.y >= lo.y - 3 && mouse.y <= hi.y + 3)
        {
            float area = (hi.x - lo.x + 1.0f) * (hi.y - lo.y + 1.0f);

            if (area < best_area)
            {
                best_area = area;
                best = i;
            }
        }
    }

    return best;
}

// The element under a point of a 2D view, among the selected meshes (or all of them if none is).
static Pick pick_element_2d(const OrthoView* view, ImVec2 center, ImVec2 mouse)
{
    Pick pick = no_pick;
    float best = PICK_RADIUS;
    int element = mode_element();
    static int meshes[SCENE_MAX_OBJECTS];
    int count = selected_meshes(meshes);

    if (count == 0)
    {
        for (int i = 0; i < editor_scene.count; i++)
        {
            if (editor_scene.objects[i].mesh && !hidden_now(&editor_scene.objects[i]))
            {
                meshes[count++] = i;
            }
        }
    }

    for (int k = 0; k < count; k++)
    {
        const SceneObject* object = &editor_scene.objects[meshes[k]];
        const EditMesh* mesh = mesh_of(object);
        Mat4 matrix = scene_object_matrix(object);

        for (int index = 0; index < element_count(mesh, element); index++)
        {
            float distance;

            if (element == EDIT_ELEMENT_EDGE)
            {
                ImVec2 a = to_screen(view, center, transform_point(matrix, to_vec3(edit_mesh_position(mesh, mesh->edges[index].a))));
                ImVec2 b = to_screen(view, center, transform_point(matrix, to_vec3(edit_mesh_position(mesh, mesh->edges[index].b))));
                distance = segment_distance(mouse, a, b, NULL);
            }
            else
            {
                distance = distance_2d(mouse, to_screen(view, center, transform_point(matrix, element_position(mesh, element, index))));
            }

            if (distance < best)
            {
                best = distance;
                pick.object = meshes[k];
                pick.index = index;
            }
        }
    }

    return pick;
}

static void draw_elements_2d(ImDrawList* draw_list, const OrthoView* view, ImVec2 center, const SceneObject* object)
{
    const EditMesh* mesh = mesh_of(object);
    Mat4 matrix = scene_object_matrix(object);
    int mode = editor_selection_mode;

    for (int e = 0; e < mesh->edge_count; e++)
    {
        bool selected = mode == SELECT_EDGES && mesh->edges[e].selected;
        ImVec2 a = to_screen(view, center, transform_point(matrix, to_vec3(edit_mesh_position(mesh, mesh->edges[e].a))));
        ImVec2 b = to_screen(view, center, transform_point(matrix, to_vec3(edit_mesh_position(mesh, mesh->edges[e].b))));
        draw_list->AddLine(a, b, selected ? COLOR_SELECTION : IM_COL32(255, 110, 110, 255), selected ? 2.5f : 1.0f);
    }

    if (mode == SELECT_FACES)
    {
        for (int f = 0; f < mesh->face_count; f++)
        {
            ImVec2 c = to_screen(view, center, transform_point(matrix, element_position(mesh, EDIT_ELEMENT_FACE, f)));
            bool selected = mesh->faces[f].selected != 0;

            if (selected)
            {
                for (int i = 0; i < mesh->faces[f].count; i++)
                {
                    ImVec2 a = to_screen(view, center, transform_point(matrix, to_vec3(edit_mesh_position(mesh, edit_mesh_face_vertex(mesh, f, i)))));
                    ImVec2 b = to_screen(view, center, transform_point(matrix, to_vec3(edit_mesh_position(mesh, edit_mesh_face_vertex(mesh, f, i + 1)))));
                    draw_list->AddLine(a, b, COLOR_SELECTION, 2.0f);
                }
            }

            draw_list->AddCircleFilled(c, selected ? 3.5f : 2.5f, selected ? COLOR_SELECTION : IM_COL32(230, 230, 230, 200));
        }
    }

    if (mode == SELECT_VERTICES)
    {
        for (int v = 0; v < mesh->vertex_count; v++)
        {
            ImVec2 p = to_screen(view, center, transform_point(matrix, to_vec3(edit_mesh_position(mesh, v))));
            bool selected = mesh->vertex_selected[v] != 0;
            float size = selected ? 3.5f : 2.5f;
            draw_list->AddRectFilled(ImVec2(p.x - size, p.y - size), ImVec2(p.x + size, p.y + size), selected ? COLOR_SELECTION : IM_COL32(240, 240, 240, 230));
        }
    }
}

// 2D gestures.
enum { DRAG_NONE, DRAG_PAN, DRAG_MOVE, DRAG_MARQUEE, DRAG_BLOCK, DRAG_BLOCK_EDIT, DRAG_CLIP };

static int drag_kind;
static int drag_view = -1;
static ImVec2 drag_start;
static Vec3 drag_world_start;
static bool drag_moving;           // DRAG_MOVE : past the threshold, the move has begun
static bool drag_shift;
static bool drag_ctrl;
static int block_edit_sides;       // DRAG_BLOCK_EDIT : bits 0 / 1 = u min / max, 2 / 3 = v min / max ; 0 : the whole block

// The pending block's sides near a point of a 2D view (see block_edit_sides) ; -1 if it's away.
static int block_sides_at(const OrthoView* view, ImVec2 center, ImVec2 mouse)
{
    if (editor_tool != TOOL_BLOCK || block.stage != BLOCK_PENDING)
    {
        return -1;
    }

    float u_lo = axis_of(block.min, view->u);
    float u_hi = axis_of(block.max, view->u);
    float v_lo = axis_of(block.min, view->v);
    float v_hi = axis_of(block.max, view->v);
    ImVec2 a = to_screen(view, center, block.min);
    ImVec2 b = to_screen(view, center, block.max);

    // a has the min coordinates : on screen, they may be on the right / bottom side (negative signs).
    float x_min = a.x, x_max = b.x, y_min = a.y, y_max = b.y;
    ImVec2 lo(SDL_min(x_min, x_max), SDL_min(y_min, y_max));
    ImVec2 hi(SDL_max(x_min, x_max), SDL_max(y_min, y_max));
    const float reach = 6.0f;

    if (mouse.x < lo.x - reach || mouse.x > hi.x + reach || mouse.y < lo.y - reach || mouse.y > hi.y + reach)
    {
        return -1;
    }

    int sides = 0;

    if (SDL_fabsf(mouse.x - x_min) < reach) sides |= 1;
    else if (SDL_fabsf(mouse.x - x_max) < reach) sides |= 2;

    if (SDL_fabsf(mouse.y - y_min) < reach) sides |= 4;
    else if (SDL_fabsf(mouse.y - y_max) < reach) sides |= 8;

    (void)u_lo; (void)u_hi; (void)v_lo; (void)v_hi;
    return sides;
}

static void handle_view_input(int index, ImVec2 lo, ImVec2 hi, bool hovered)
{
    OrthoView* view = &views[index];
    ImGuiIO& io = ImGui::GetIO();
    ImVec2 center((lo.x + hi.x) * 0.5f, (lo.y + hi.y) * 0.5f);
    ImVec2 mouse = io.MousePos;
    int depth = depth_axis(view);

    if (hovered)
    {
        hovered_view = index;

        // Zoom around the cursor.
        if (io.MouseWheel != 0.0f)
        {
            Vec3 before = to_world(view, center, mouse, 0.0f);
            view->zoom = SDL_clamp(view->zoom * SDL_powf(1.18f, io.MouseWheel), 2.0f, 4000.0f);
            Vec3 after = to_world(view, center, mouse, 0.0f);
            view->center_u -= (axis_of(after, view->u) - axis_of(before, view->u)) * view->su;
            view->center_v -= (axis_of(after, view->v) - axis_of(before, view->v)) * view->sv;
        }

        if (drag_kind == DRAG_NONE && (ImGui::IsMouseClicked(ImGuiMouseButton_Right) || ImGui::IsMouseClicked(ImGuiMouseButton_Middle) ||
            (ImGui::IsMouseClicked(ImGuiMouseButton_Left) && ImGui::IsKeyDown(ImGuiKey_Space))))
        {
            drag_kind = DRAG_PAN;
            drag_view = index;
        }
        else if (drag_kind == DRAG_NONE && ImGui::IsMouseClicked(ImGuiMouseButton_Left) && !editor_is_playing())
        {
            drag_view = index;
            drag_start = mouse;
            drag_world_start = to_world(view, center, mouse, 0.0f);
            drag_shift = io.KeyShift;
            drag_ctrl = io.KeyCtrl;
            drag_moving = false;

            if (editor_tool == TOOL_BLOCK)
            {
                int sides = block_sides_at(view, center, mouse);

                if (sides >= 0)
                {
                    drag_kind = DRAG_BLOCK_EDIT;
                    block_edit_sides = sides;
                }
                else
                {
                    // A new block on this view's plane, as deep as the last one along the view's depth.
                    drag_kind = DRAG_BLOCK;
                    block.stage = BLOCK_DRAWING;
                    block.axis = depth;
                    block.base = axis_of(last_block_min, depth);
                    block.rise = axis_of(last_block_max, depth) - axis_of(last_block_min, depth);
                    block.handle = -1;
                    Vec3 start = drag_world_start;
                    set_axis(&start, view->u, snap(axis_of(start, view->u)));
                    set_axis(&start, view->v, snap(axis_of(start, view->v)));
                    block.start = start;
                    block_extent_from_drawing(start);
                }
            }
            else if (editor_tool == TOOL_CLIP)
            {
                drag_kind = DRAG_CLIP;
                clip.stage = CLIP_DRAWING;
                clip.view = index;
                Vec3 a = drag_world_start;
                set_axis(&a, view->u, snap(axis_of(a, view->u)));
                set_axis(&a, view->v, snap(axis_of(a, view->v)));
                clip.a = clip.b = a;
            }
            else if (component_mode())
            {
                Pick pick = pick_element_2d(view, center, mouse);

                if (pick.object >= 0)
                {
                    EditMesh* mesh = mesh_of(&editor_scene.objects[pick.object]);
                    int element = mode_element();

                    if (!element_selected(mesh, element, pick.index) || drag_ctrl)
                    {
                        click_element(pick, drag_ctrl);

                        // As in Hammer : what's stacked under the cursor in this view (the vertices or
                        // edges at the front and at the back) goes with it.
                        if (element != EDIT_ELEMENT_FACE && element_selected(mesh, element, pick.index))
                        {
                            Mat4 matrix = scene_object_matrix(&editor_scene.objects[pick.object]);

                            for (int index = 0; index < element_count(mesh, element); index++)
                            {
                                bool stacked;

                                if (element == EDIT_ELEMENT_VERTEX)
                                {
                                    ImVec2 a = to_screen(view, center, transform_point(matrix, to_vec3(edit_mesh_position(mesh, pick.index))));
                                    ImVec2 b = to_screen(view, center, transform_point(matrix, to_vec3(edit_mesh_position(mesh, index))));
                                    stacked = distance_2d(a, b) < 1.5f;
                                }
                                else
                                {
                                    const EditEdge* picked = &mesh->edges[pick.index];
                                    const EditEdge* other = &mesh->edges[index];
                                    ImVec2 a0 = to_screen(view, center, transform_point(matrix, to_vec3(edit_mesh_position(mesh, picked->a))));
                                    ImVec2 a1 = to_screen(view, center, transform_point(matrix, to_vec3(edit_mesh_position(mesh, picked->b))));
                                    ImVec2 b0 = to_screen(view, center, transform_point(matrix, to_vec3(edit_mesh_position(mesh, other->a))));
                                    ImVec2 b1 = to_screen(view, center, transform_point(matrix, to_vec3(edit_mesh_position(mesh, other->b))));
                                    stacked = (distance_2d(a0, b0) < 1.5f && distance_2d(a1, b1) < 1.5f) || (distance_2d(a0, b1) < 1.5f && distance_2d(a1, b0) < 1.5f);
                                }

                                if (stacked)
                                {
                                    set_element(mesh, element, index, 1);
                                }
                            }
                        }
                    }

                    drag_kind = DRAG_MOVE;
                }
                else
                {
                    drag_kind = DRAG_MARQUEE;
                }
            }
            else
            {
                int object = pick_object_2d(view, center, mouse);

                if (object >= 0)
                {
                    if (drag_ctrl)
                    {
                        editor_select_object(object, SELECT_TOGGLE);
                    }
                    else if (!editor_scene.objects[object].selected)
                    {
                        editor_select_object(object, SELECT_REPLACE);
                    }

                    drag_kind = editor_scene.objects[object].selected ? DRAG_MOVE : DRAG_NONE;
                }
                else
                {
                    drag_kind = DRAG_MARQUEE;
                }
            }
        }
    }

    if (drag_view != index || drag_kind == DRAG_NONE)
    {
        return;
    }

    Vec3 world = to_world(view, center, mouse, 0.0f);

    switch (drag_kind)
    {
    case DRAG_PAN:
        view->center_u -= io.MouseDelta.x / view->zoom;
        view->center_v += io.MouseDelta.y / view->zoom;

        if (!ImGui::IsMouseDown(ImGuiMouseButton_Right) && !ImGui::IsMouseDown(ImGuiMouseButton_Middle) && !ImGui::IsMouseDown(ImGuiMouseButton_Left))
        {
            drag_kind = DRAG_NONE;
        }

        break;

    case DRAG_MOVE:
    {
        Vec3 offset = vec3(0.0f, 0.0f, 0.0f);
        set_axis(&offset, view->u, snap(axis_of(world, view->u) - axis_of(drag_world_start, view->u)));
        set_axis(&offset, view->v, snap(axis_of(world, view->v) - axis_of(drag_world_start, view->v)));

        if (!drag_moving && distance_2d(mouse, drag_start) > DRAG_THRESHOLD)
        {
            drag_moving = true;
            begin_move(drag_shift);
        }

        if (drag_moving)
        {
            apply_move(translation(offset));
        }

        if (!ImGui::IsMouseDown(ImGuiMouseButton_Left))
        {
            if (drag_moving)
            {
                end_move(translation(offset));
            }

            drag_kind = DRAG_NONE;
        }

        break;
    }

    case DRAG_MARQUEE:
        if (!ImGui::IsMouseDown(ImGuiMouseButton_Left))
        {
            ImVec2 a(SDL_min(drag_start.x, mouse.x), SDL_min(drag_start.y, mouse.y));
            ImVec2 b(SDL_max(drag_start.x, mouse.x), SDL_max(drag_start.y, mouse.y));
            bool clicked = distance_2d(drag_start, mouse) <= DRAG_THRESHOLD;

            if (component_mode())
            {
                int element = mode_element();

                if (!drag_ctrl)
                {
                    clear_components(element);
                }

                static int meshes[SCENE_MAX_OBJECTS];
                int count = clicked ? 0 : selected_meshes(meshes);

                for (int k = 0; k < count; k++)
                {
                    SceneObject* object = &editor_scene.objects[meshes[k]];
                    EditMesh* mesh = mesh_of(object);
                    Mat4 matrix = scene_object_matrix(object);

                    for (int e = 0; e < element_count(mesh, element); e++)
                    {
                        ImVec2 p = to_screen(view, center, transform_point(matrix, element_position(mesh, element, e)));

                        if (p.x >= a.x && p.x <= b.x && p.y >= a.y && p.y <= b.y)
                        {
                            set_element(mesh, element, e, 1);
                        }
                    }
                }
            }
            else
            {
                if (!drag_ctrl)
                {
                    editor_select_none();
                }

                for (int i = 0; i < editor_scene.count && !clicked; i++)
                {
                    const SceneObject* object = &editor_scene.objects[i];

                    if (hidden_now(object) || (editor_selection_mode == SELECT_MESHES && !object->mesh))
                    {
                        continue;
                    }

                    ImVec2 r_lo;
                    ImVec2 r_hi;
                    object_rect_2d(view, center, object, &r_lo, &r_hi);
                    ImVec2 c((r_lo.x + r_hi.x) * 0.5f, (r_lo.y + r_hi.y) * 0.5f);

                    if (c.x >= a.x && c.x <= b.x && c.y >= a.y && c.y <= b.y)
                    {
                        editor_select_object(i, SELECT_ADD);
                    }
                }
            }

            drag_kind = DRAG_NONE;
        }

        break;

    case DRAG_BLOCK:
    {
        Vec3 current = world;
        set_axis(&current, view->u, snap(axis_of(current, view->u)));
        set_axis(&current, view->v, snap(axis_of(current, view->v)));
        block_extent_from_drawing(current);

        if (!ImGui::IsMouseDown(ImGuiMouseButton_Left))
        {
            block.stage = block_valid() ? BLOCK_PENDING : BLOCK_NONE;
            drag_kind = DRAG_NONE;
        }

        break;
    }

    case DRAG_BLOCK_EDIT:
    {
        float du = snap(axis_of(world, view->u) - axis_of(drag_world_start, view->u));
        float dv = snap(axis_of(world, view->v) - axis_of(drag_world_start, view->v));

        if (du != 0.0f || dv != 0.0f)
        {
            if (block_edit_sides == 0)
            {
                // The whole block.
                set_axis(&block.min, view->u, axis_of(block.min, view->u) + du);
                set_axis(&block.max, view->u, axis_of(block.max, view->u) + du);
                set_axis(&block.min, view->v, axis_of(block.min, view->v) + dv);
                set_axis(&block.max, view->v, axis_of(block.max, view->v) + dv);
            }
            else
            {
                if (block_edit_sides & 1) set_block_side(view->u, 0, axis_of(block.min, view->u) + du);
                if (block_edit_sides & 2) set_block_side(view->u, 1, axis_of(block.max, view->u) + du);
                if (block_edit_sides & 4) set_block_side(view->v, 0, axis_of(block.min, view->v) + dv);
                if (block_edit_sides & 8) set_block_side(view->v, 1, axis_of(block.max, view->v) + dv);
            }

            set_axis(&drag_world_start, view->u, axis_of(drag_world_start, view->u) + du);
            set_axis(&drag_world_start, view->v, axis_of(drag_world_start, view->v) + dv);
        }

        if (!ImGui::IsMouseDown(ImGuiMouseButton_Left))
        {
            drag_kind = DRAG_NONE;
        }

        break;
    }

    case DRAG_CLIP:
    {
        Vec3 b = world;
        set_axis(&b, view->u, snap(axis_of(b, view->u)));
        set_axis(&b, view->v, snap(axis_of(b, view->v)));
        clip.b = b;

        if (!ImGui::IsMouseDown(ImGuiMouseButton_Left))
        {
            Vec3 along = vec3_sub(clip.b, clip.a);

            if (vec3_length(along) > 1e-5f)
            {
                // The plane through the line, perpendicular to the view.
                Vec3 view_axis = vec3(0.0f, 0.0f, 0.0f);
                set_axis(&view_axis, depth, 1.0f);
                Vec3 n = vec3_normalize(vec3_cross(along, view_axis));
                clip.plane[0] = n.x;
                clip.plane[1] = n.y;
                clip.plane[2] = n.z;
                clip.plane[3] = vec3_dot(n, clip.a);
                clip.stage = CLIP_READY;
            }
            else
            {
                clip.stage = CLIP_NONE;
            }

            drag_kind = DRAG_NONE;
        }

        break;
    }

    default:
        break;
    }
}

static void draw_view(int index, ImVec2 lo, ImVec2 hi)
{
    OrthoView* view = &views[index];
    ImDrawList* draw_list = ImGui::GetWindowDrawList();
    ImVec2 center((lo.x + hi.x) * 0.5f, (lo.y + hi.y) * 0.5f);

    view_clip_lo = lo;
    view_clip_hi = hi;
    draw_list->PushClipRect(lo, hi, true);
    draw_list->AddRectFilled(lo, hi, IM_COL32(26, 26, 28, 255));
    draw_view_grid(draw_list, view, lo, hi, center);

    // Everything, in wireframe : Hammer's colors, the selection in red.
    for (int i = 0; i < editor_scene.count; i++)
    {
        const SceneObject* object = &editor_scene.objects[i];

        if (hidden_now(object) || object->selected)
        {
            continue;
        }

        ImU32 color = object->mesh ? IM_COL32(205, 205, 205, 255) : object->model[0] ? IM_COL32(110, 190, 255, 200) : IM_COL32(230, 210, 90, 220);
        draw_object_2d(draw_list, view, center, object, color);
    }

    for (int i = 0; i < editor_scene.count; i++)
    {
        const SceneObject* object = &editor_scene.objects[i];

        if (hidden_now(object) || !object->selected)
        {
            continue;
        }

        if (component_mode() && object->mesh && mesh_of(object))
        {
            draw_elements_2d(draw_list, view, center, object);
        }
        else
        {
            draw_object_2d(draw_list, view, center, object, IM_COL32(255, 70, 70, 255));
        }
    }

    // The block being made.
    if (editor_tool == TOOL_BLOCK && block.stage != BLOCK_NONE)
    {
        ImVec2 a = to_screen(view, center, block.min);
        ImVec2 b = to_screen(view, center, block.max);
        ImVec2 r_lo(SDL_min(a.x, b.x), SDL_min(a.y, b.y));
        ImVec2 r_hi(SDL_max(a.x, b.x), SDL_max(a.y, b.y));
        draw_list->AddRectFilled(r_lo, r_hi, IM_COL32(110, 255, 130, 25));
        draw_list->AddRect(r_lo, r_hi, COLOR_BLOCK, 0.0f, 1.5f);

        if (block.stage == BLOCK_PENDING)
        {
            ImVec2 handles[8] = {
                r_lo, ImVec2(r_hi.x, r_lo.y), r_hi, ImVec2(r_lo.x, r_hi.y),
                ImVec2((r_lo.x + r_hi.x) * 0.5f, r_lo.y), ImVec2(r_hi.x, (r_lo.y + r_hi.y) * 0.5f),
                ImVec2((r_lo.x + r_hi.x) * 0.5f, r_hi.y), ImVec2(r_lo.x, (r_lo.y + r_hi.y) * 0.5f)
            };

            for (int h = 0; h < 8; h++)
            {
                draw_list->AddRectFilled(ImVec2(handles[h].x - 3, handles[h].y - 3), ImVec2(handles[h].x + 3, handles[h].y + 3), COLOR_BLOCK);
            }
        }
    }

    // The clipping line : in the view it was drawn in ; elsewhere, the plane's trace is its preview.
    if (editor_tool == TOOL_CLIP && clip.stage != CLIP_NONE)
    {
        if (clip.view == index)
        {
            ImVec2 a = to_screen(view, center, clip.a);
            ImVec2 b = to_screen(view, center, clip.b);
            draw_list->AddLine(a, b, COLOR_CUT, 2.0f);
            draw_list->AddCircleFilled(a, 3.5f, COLOR_CUT);
            draw_list->AddCircleFilled(b, 3.5f, COLOR_CUT);
        }

        if (clip.stage == CLIP_READY)
        {
            // The selected meshes, colored by side.
            for (int i = 0; i < editor_scene.count; i++)
            {
                const SceneObject* object = &editor_scene.objects[i];
                const EditMesh* mesh = mesh_of(object);

                if (!object->selected || !mesh || hidden_now(object))
                {
                    continue;
                }

                Mat4 matrix = scene_object_matrix(object);

                for (int e = 0; e < mesh->edge_count; e++)
                {
                    Vec3 pa = transform_point(matrix, to_vec3(edit_mesh_position(mesh, mesh->edges[e].a)));
                    Vec3 pb = transform_point(matrix, to_vec3(edit_mesh_position(mesh, mesh->edges[e].b)));
                    float da = plane_distance(clip.plane, pa);
                    float db = plane_distance(clip.plane, pb);

                    if ((da >= 0.0f) == (db >= 0.0f))
                    {
                        draw_list->AddLine(to_screen(view, center, pa), to_screen(view, center, pb), clip_side_color(da >= 0.0f), 1.5f);
                    }
                    else
                    {
                        Vec3 cut = vec3_add(pa, vec3_scale(vec3_sub(pb, pa), da / (da - db)));
                        draw_list->AddLine(to_screen(view, center, pa), to_screen(view, center, cut), clip_side_color(da >= 0.0f), 1.5f);
                        draw_list->AddLine(to_screen(view, center, cut), to_screen(view, center, pb), clip_side_color(db >= 0.0f), 1.5f);
                    }
                }
            }
        }
    }

    // The 3D view's camera.
    const CameraMatrices* camera = editor_camera();
    Vec3 forward = vec3(-camera->view.m[8], -camera->view.m[9], -camera->view.m[10]);
    ImVec2 eye = to_screen(view, center, camera->eye);
    ImVec2 look = to_screen(view, center, vec3_add(camera->eye, vec3_scale(forward, 2.0f)));
    draw_list->AddLine(eye, look, IM_COL32(110, 215, 255, 200), 1.5f);
    draw_list->AddCircleFilled(eye, 4.0f, IM_COL32(110, 215, 255, 230));

    // Marquee.
    if (drag_kind == DRAG_MARQUEE && drag_view == index)
    {
        ImVec2 mouse = ImGui::GetIO().MousePos;
        ImVec2 a(SDL_min(drag_start.x, mouse.x), SDL_min(drag_start.y, mouse.y));
        ImVec2 b(SDL_max(drag_start.x, mouse.x), SDL_max(drag_start.y, mouse.y));
        draw_list->AddRectFilled(a, b, IM_COL32(110, 215, 255, 30));
        draw_list->AddRect(a, b, COLOR_HOVER);
    }

    char label[64];
    SDL_snprintf(label, sizeof(label), "%s   %g m", view->name, editor_grid_size);
    draw_list->AddText(ImVec2(lo.x + 6, lo.y + 4), IM_COL32(200, 200, 200, 230), label);
    draw_list->AddRect(lo, hi, IM_COL32(70, 70, 70, 255));
    draw_list->PopClipRect();
}

void editor_draw_2d_views(void)
{
    hovered_view = -1;

    if (!editor_show_2d_views)
    {
        drag_kind = DRAG_NONE;
        return;
    }

    ImGuiIO& io = ImGui::GetIO();
    ImGui::SetNextWindowPos(ImVec2(390.0f, 60.0f), ImGuiCond_FirstUseEver);
    ImGui::SetNextWindowSize(ImVec2(SDL_max(300.0f, io.DisplaySize.x - 390.0f - 600.0f), SDL_max(300.0f, io.DisplaySize.y - 380.0f)), ImGuiCond_FirstUseEver);

    if (!ImGui::Begin(ICON_VIEWS " 2D Views", &editor_show_2d_views))
    {
        ImGui::End();
        return;
    }

    static const char* const labels[4] = { "Top", "Front", "Side", "All" };

    for (int i = 0; i < 4; i++)
    {
        if (i > 0)
        {
            ImGui::SameLine();
        }

        if (ImGui::RadioButton(labels[i], views_shown == i))
        {
            views_shown = i;
        }
    }

    ImGui::SameLine();
    ImGui::TextDisabled("  wheel : zoom   right / middle drag : pan");

    ImVec2 origin = ImGui::GetCursorScreenPos();
    ImVec2 available = ImGui::GetContentRegionAvail();
    available.x = SDL_max(available.x, 50.0f);
    available.y = SDL_max(available.y, 50.0f);

    // Where each view goes. All three : the top view on the left half, front and side stacked on the
    // right (each keeps a usable shape) ; one row or one column only when the panel is very elongated.
    const float gap = 4.0f;
    int count = views_shown == 3 ? 3 : 1;
    ImVec2 los[3];
    ImVec2 his[3];

    if (count == 1)
    {
        los[0] = origin;
        his[0] = ImVec2(origin.x + available.x, origin.y + available.y);
    }
    else if (available.x > available.y * 2.6f || available.y > available.x * 2.6f)
    {
        bool in_row = available.x > available.y;
        float step = ((in_row ? available.x : available.y) - gap * 2.0f) / 3.0f;

        for (int k = 0; k < 3; k++)
        {
            los[k] = in_row ? ImVec2(origin.x + (step + gap) * (float)k, origin.y) : ImVec2(origin.x, origin.y + (step + gap) * (float)k);
            his[k] = in_row ? ImVec2(los[k].x + step, origin.y + available.y) : ImVec2(origin.x + available.x, los[k].y + step);
        }
    }
    else
    {
        float half_width = (available.x - gap) * 0.5f;
        float half_height = (available.y - gap) * 0.5f;
        los[0] = origin;
        his[0] = ImVec2(origin.x + half_width, origin.y + available.y);
        los[1] = ImVec2(origin.x + half_width + gap, origin.y);
        his[1] = ImVec2(origin.x + available.x, origin.y + half_height);
        los[2] = ImVec2(origin.x + half_width + gap, origin.y + half_height + gap);
        his[2] = ImVec2(origin.x + available.x, origin.y + available.y);
    }

    for (int k = 0; k < count; k++)
    {
        int index = views_shown == 3 ? k : views_shown;
        ImVec2 lo = los[k];
        ImVec2 hi = his[k];
        ImVec2 size(hi.x - lo.x, hi.y - lo.y);

        ImGui::SetCursorScreenPos(lo);
        ImGui::PushID(index);
        ImGui::InvisibleButton("##view", size, ImGuiButtonFlags_MouseButtonLeft | ImGuiButtonFlags_MouseButtonRight | ImGuiButtonFlags_MouseButtonMiddle);
        bool hovered = ImGui::IsItemHovered();
        ImGui::PopID();

        handle_view_input(index, lo, hi, hovered);
        draw_view(index, lo, hi);
    }

    ImGui::End();
}

// --- Panels -----------------------------------------------------------------------------------------

static bool tool_button(const char* label, bool active, const char* tooltip, ImVec2 size = ImVec2(0.0f, 0.0f))
{
    if (active)
    {
        ImGui::PushStyleColor(ImGuiCol_Button, ImGui::GetStyleColorVec4(ImGuiCol_ButtonActive));
    }

    bool pressed = ImGui::Button(label, size);

    if (active)
    {
        ImGui::PopStyleColor();
    }

    if (tooltip && ImGui::IsItemHovered())
    {
        ImGui::SetTooltip("%s", tooltip);
    }

    return pressed;
}

static bool operation_button(const char* label, int operation, float amount, const char* tooltip)
{
    bool pressed = ImGui::Button(label);

    if (tooltip && ImGui::IsItemHovered())
    {
        ImGui::SetTooltip("%s", tooltip);
    }

    if (pressed)
    {
        level_mesh_operation(operation, amount);
    }

    return pressed;
}

// The first selected face's style, if any (in the faces mode).
static bool first_selected_face(int* out_object, int* out_face)
{
    static int meshes[SCENE_MAX_OBJECTS];
    int count = selected_meshes(meshes);

    for (int k = 0; k < count; k++)
    {
        const EditMesh* mesh = mesh_of(&editor_scene.objects[meshes[k]]);

        for (int f = 0; f < mesh->face_count; f++)
        {
            if (mesh->faces[f].selected)
            {
                *out_object = meshes[k];
                *out_face = f;
                return true;
            }
        }
    }

    return false;
}

enum { TEXTURE_ALIGN, TEXTURE_SCALE, TEXTURE_OFFSET, TEXTURE_ROTATION, TEXTURE_FIT, TEXTURE_RESET, TEXTURE_NUDGE };

// Changes the selected faces' texture mapping, all of them alike.
static void edit_faces_texture(int what, const EditFaceStyle* values, const float* nudge_by)
{
    static int meshes[SCENE_MAX_OBJECTS];
    int count = selected_meshes(meshes);

    for (int k = 0; k < count; k++)
    {
        SceneObject* object = &editor_scene.objects[meshes[k]];

        if (edit_mesh_selection_count(mesh_of(object), EDIT_ELEMENT_FACE) == 0)
        {
            continue;
        }

        EditMesh* mesh = editor_writable_mesh(object);

        for (int f = 0; f < mesh->face_count; f++)
        {
            EditFaceStyle* style = &mesh->faces[f].style;

            if (!mesh->faces[f].selected)
            {
                continue;
            }

            switch (what)
            {
            case TEXTURE_ALIGN: style->align = values->align; break;
            case TEXTURE_SCALE: style->uv_scale[0] = values->uv_scale[0]; style->uv_scale[1] = values->uv_scale[1]; break;
            case TEXTURE_OFFSET: style->uv_offset[0] = values->uv_offset[0]; style->uv_offset[1] = values->uv_offset[1]; break;
            case TEXTURE_ROTATION: style->uv_rotation = values->uv_rotation; break;
            case TEXTURE_FIT: edit_mesh_fit_texture(mesh, f); break;
            case TEXTURE_RESET:
            {
                int material = style->material;
                edit_mesh_default_style(style, material);
                break;
            }
            case TEXTURE_NUDGE:
                style->uv_offset[0] += nudge_by[0];
                style->uv_offset[1] += nudge_by[1];
                break;
            default: break;
            }
        }

        edit_mesh_touch(mesh);
    }
}

static void draw_texture_section(void)
{
    int object_index = -1;
    int face = -1;

    if (!first_selected_face(&object_index, &face))
    {
        ImGui::TextDisabled("Select faces to edit their texture mapping.");
        return;
    }

    const EditMesh* mesh = mesh_of(&editor_scene.objects[object_index]);
    EditFaceStyle style = mesh->faces[face].style;
    const char* material = mesh->materials[SDL_clamp(style.material, 0, mesh->material_count - 1)];
    const char* name = SDL_strrchr(material, '/');

    ImGui::Text("Material : %s", name ? name + 1 : material);

    if (ImGui::Button(ICON_PAINT " Apply active material"))
    {
        level_apply_material(editor_active_material);
    }

    if (ImGui::IsItemHovered())
    {
        ImGui::SetTooltip("%s", editor_active_material);
    }

    ImGui::PushItemWidth(150);

    int align = style.align;

    if (ImGui::Combo("Alignment", &align, "World\0Face\0"))
    {
        editor_begin_edit();
        style.align = align;
        edit_faces_texture(TEXTURE_ALIGN, &style, NULL);
    }

    // Drags record one undo step when they start ; the faces follow every change.
    bool changed = ImGui::DragFloat2("Scale (m)", style.uv_scale, 0.01f, 0.01f, 100.0f, "%.3f");

    if (ImGui::IsItemActivated())
    {
        editor_begin_edit();
    }

    if (changed)
    {
        edit_faces_texture(TEXTURE_SCALE, &style, NULL);
    }

    changed = ImGui::DragFloat2("Offset", style.uv_offset, 0.005f, -100.0f, 100.0f, "%.3f");

    if (ImGui::IsItemActivated())
    {
        editor_begin_edit();
    }

    if (changed)
    {
        edit_faces_texture(TEXTURE_OFFSET, &style, NULL);
    }

    changed = ImGui::DragFloat("Rotation", &style.uv_rotation, 0.5f, -360.0f, 360.0f, "%.1f deg");

    if (ImGui::IsItemActivated())
    {
        editor_begin_edit();
    }

    if (changed)
    {
        edit_faces_texture(TEXTURE_ROTATION, &style, NULL);
    }

    ImGui::PopItemWidth();

    if (ImGui::Button("Fit"))
    {
        editor_begin_edit();
        edit_faces_texture(TEXTURE_FIT, &style, NULL);
    }

    ImGui::SameLine();

    if (ImGui::Button("Reset"))
    {
        editor_begin_edit();
        edit_faces_texture(TEXTURE_RESET, &style, NULL);
    }

    ImGui::SameLine();

    // Quarter turns and half-texture shifts, as in Hammer's face edit sheet.
    if (ImGui::Button("+90"))
    {
        editor_begin_edit();
        style.uv_rotation = SDL_fmodf(style.uv_rotation + 90.0f, 360.0f);
        edit_faces_texture(TEXTURE_ROTATION, &style, NULL);
    }

    ImGui::SameLine();

    if (ImGui::Button("Shift U"))
    {
        const float by[2] = { 0.5f, 0.0f };
        editor_begin_edit();
        edit_faces_texture(TEXTURE_NUDGE, &style, by);
    }

    ImGui::SameLine();

    if (ImGui::Button("Shift V"))
    {
        const float by[2] = { 0.0f, 0.5f };
        editor_begin_edit();
        edit_faces_texture(TEXTURE_NUDGE, &style, by);
    }
}

void editor_draw_level_panel(void)
{
    ImGui::SetNextWindowPos(ImVec2(12, 380), ImGuiCond_FirstUseEver);
    ImGui::Begin(ICON_BRICK " Level", NULL, ImGuiWindowFlags_AlwaysAutoResize);

    bool playing = editor_is_playing();
    ImGui::BeginDisabled(playing);

    // Selection modes.
    ImGui::TextDisabled("Select");

    for (int mode = 0; mode < SELECTION_MODE_COUNT; mode++)
    {
        if (mode > 0)
        {
            ImGui::SameLine();
        }

        char label[48];
        char tooltip[64];
        SDL_snprintf(label, sizeof(label), "%s##mode%d", mode_icons[mode], mode);
        SDL_snprintf(tooltip, sizeof(tooltip), "%s (%d)", mode_names[mode], mode + 1);

        ImGui::PushFont(NULL, 20.0f);
        bool pressed = tool_button(label, editor_selection_mode == mode, NULL, ImVec2(38, 34));
        ImGui::PopFont();

        if (ImGui::IsItemHovered())
        {
            ImGui::SetTooltip("%s", tooltip);
        }

        if (pressed)
        {
            level_set_selection_mode(mode);
        }
    }

    // Tools.
    ImGui::TextDisabled("Tools");

    if (tool_button(ICON_POINTER " Select", editor_tool == TOOL_SELECT, "Select and transform (Escape)"))
    {
        level_set_tool(TOOL_SELECT);
    }

    ImGui::SameLine();

    if (tool_button(ICON_BLOCK " Block", editor_tool == TOOL_BLOCK, "Block tool (Shift+B) : drag a box on the grid or a face"))
    {
        level_set_tool(TOOL_BLOCK);
    }

    ImGui::SameLine();

    if (tool_button(ICON_SCISSORS " Clip", editor_tool == TOOL_CLIP, "Clipping tool (Shift+X) : drag a line across the selected meshes"))
    {
        level_set_tool(TOOL_CLIP);
    }

    // Gizmo and grid.
    char label[64];

    if (tool_button(ICON_MOVE_3D "##t", editor_gizmo_operation == ImGuizmo::TRANSLATE, "Move (T)"))
    {
        editor_gizmo_operation = ImGuizmo::TRANSLATE;
    }

    ImGui::SameLine();

    if (tool_button(ICON_ROTATE_3D "##r", editor_gizmo_operation == ImGuizmo::ROTATE, "Rotate (R)"))
    {
        editor_gizmo_operation = ImGuizmo::ROTATE;
    }

    ImGui::SameLine();

    if (tool_button(ICON_SCALE_3D "##e", editor_gizmo_operation == ImGuizmo::SCALE, "Scale (E)"))
    {
        editor_gizmo_operation = ImGuizmo::SCALE;
    }

    ImGui::SameLine();

    if (tool_button(editor_gizmo_mode == ImGuizmo::WORLD ? ICON_EARTH "##space" : ICON_AXIS_3D "##space", false, editor_gizmo_mode == ImGuizmo::WORLD ? "World space" : "Local space"))
    {
        editor_gizmo_mode = editor_gizmo_mode == ImGuizmo::WORLD ? ImGuizmo::LOCAL : ImGuizmo::WORLD;
    }

    ImGui::SameLine();
    ImGui::TextDisabled("|");
    ImGui::SameLine();

    if (ImGui::Button(ICON_MINUS "##finer"))
    {
        level_change_grid(-1);
    }

    if (ImGui::IsItemHovered())
    {
        ImGui::SetTooltip("Finer grid ([)");
    }

    ImGui::SameLine();
    SDL_snprintf(label, sizeof(label), "%s %g m", ICON_GRID, editor_grid_size);
    ImGui::TextUnformatted(label);
    ImGui::SameLine();

    if (ImGui::Button(ICON_PLUS "##coarser"))
    {
        level_change_grid(1);
    }

    if (ImGui::IsItemHovered())
    {
        ImGui::SetTooltip("Coarser grid (])");
    }

    ImGui::SameLine();

    if (tool_button(ICON_MAGNET "##gridsnap", editor_grid_snap, "Snap to the grid (meshes and elements)"))
    {
        editor_grid_snap = !editor_grid_snap;
    }

    // The tool's settings.
    if (editor_tool == TOOL_BLOCK)
    {
        ImGui::SeparatorText("Block");
        ImGui::PushItemWidth(150);

        int shape = editor_block_shape.shape;

        if (ImGui::BeginCombo("Shape", edit_mesh_shape_name(shape)))
        {
            for (int s = 0; s < EDIT_SHAPE_COUNT; s++)
            {
                if (ImGui::Selectable(edit_mesh_shape_name(s), s == shape))
                {
                    int sides = editor_block_shape.sides;
                    int steps = editor_block_shape.steps;
                    float thickness = editor_block_shape.thickness;
                    edit_mesh_default_shape(&editor_block_shape, s);

                    if (s != EDIT_SHAPE_ARCH && s != EDIT_SHAPE_SPHERE && shape != EDIT_SHAPE_ARCH && shape != EDIT_SHAPE_SPHERE)
                    {
                        editor_block_shape.sides = sides;
                    }

                    editor_block_shape.steps = steps;
                    editor_block_shape.thickness = thickness;
                }
            }

            ImGui::EndCombo();
        }

        shape = editor_block_shape.shape;

        if (shape == EDIT_SHAPE_CYLINDER || shape == EDIT_SHAPE_CONE || shape == EDIT_SHAPE_ARCH || shape == EDIT_SHAPE_SPHERE)
        {
            ImGui::SliderInt("Sides", &editor_block_shape.sides, shape == EDIT_SHAPE_ARCH ? 2 : 3, 64);
        }

        if (shape == EDIT_SHAPE_STAIRS)
        {
            ImGui::SliderInt("Steps", &editor_block_shape.steps, 1, 64);
        }

        if (shape == EDIT_SHAPE_ARCH)
        {
            ImGui::DragFloat("Thickness", &editor_block_shape.thickness, 0.01f, 0.01f, 50.0f, "%.2f m");
        }

        ImGui::Checkbox("Hollow", &editor_block_hollow);

        if (editor_block_hollow)
        {
            ImGui::SameLine();
            ImGui::SetNextItemWidth(90);
            ImGui::DragFloat("Walls", &editor_block_thickness, 0.01f, 0.01f, 10.0f, "%.2f m");
        }

        ImGui::PopItemWidth();

        const char* name = SDL_strrchr(editor_active_material, '/');
        ImGui::TextDisabled("Material : %s", name ? name + 1 : editor_active_material);

        ImGui::BeginDisabled(block.stage != BLOCK_PENDING);

        if (ImGui::Button("Create (Enter)"))
        {
            create_block();
        }

        ImGui::SameLine();

        if (ImGui::Button("Cancel (Esc)"))
        {
            block.stage = BLOCK_NONE;
        }

        ImGui::EndDisabled();
    }
    else if (editor_tool == TOOL_CLIP)
    {
        ImGui::SeparatorText("Clip");

        for (int m = 0; m < 3; m++)
        {
            if (m > 0)
            {
                ImGui::SameLine();
            }

            if (ImGui::RadioButton(clip_mode_names[m], editor_clip_mode == m))
            {
                editor_clip_mode = m;
            }
        }

        ImGui::TextDisabled("Green : kept ; red : removed ; blue : the other part.");
        ImGui::BeginDisabled(clip.stage != CLIP_READY);

        if (ImGui::Button("Clip (Enter)"))
        {
            apply_clip();
        }

        ImGui::SameLine();

        if (ImGui::Button("Cancel (Esc)"))
        {
            clip.stage = CLIP_NONE;
        }

        ImGui::EndDisabled();
    }

    // Operations, by selection mode.
    static float extrude_distance = 1.0f;
    static float inset_distance = 0.25f;
    static float hollow_thickness = 0.25f;
    static float weld_distance = 0.01f;

    if (editor_selection_mode == SELECT_VERTICES)
    {
        ImGui::SeparatorText("Vertices");
        operation_button(ICON_MERGE " Merge", MESH_OP_MERGE, 0.0f, "Into one, at their center (M)");
        ImGui::SameLine();
        operation_button("Connect", MESH_OP_CONNECT, 0.0f, "Splits the faces between two selected vertices");
        ImGui::SameLine();
        operation_button("Dissolve", MESH_OP_DISSOLVE, 0.0f, "Removes them from their faces (Delete)");
        operation_button("Snap to grid", MESH_OP_SNAP, 0.0f, NULL);
        ImGui::SameLine();
        operation_button("Weld", MESH_OP_WELD, weld_distance, "Merges the selected vertices closer than the distance");
        ImGui::SameLine();
        ImGui::SetNextItemWidth(80);
        ImGui::DragFloat("##weld", &weld_distance, 0.001f, 0.0001f, 10.0f, "%.3f m");
    }
    else if (editor_selection_mode == SELECT_EDGES)
    {
        ImGui::SeparatorText("Edges");
        operation_button("Loop", MESH_OP_SELECT_LOOP, 0.0f, "Selects the edge loops through the selection (L)");
        ImGui::SameLine();
        operation_button("Ring", MESH_OP_SELECT_RING, 0.0f, "Selects the edge rings across quads (G)");
        ImGui::SameLine();
        operation_button(ICON_SPLIT " Split", MESH_OP_SPLIT, 0.0f, "Splits them at their middle");
        ImGui::SameLine();
        operation_button("Connect", MESH_OP_CONNECT, 0.0f, "Joins the middles of two selected edges of a face : with Ring, a loop cut");
        operation_button("Collapse", MESH_OP_COLLAPSE, 0.0f, "Each to its middle (M)");
        ImGui::SameLine();
        operation_button("Dissolve", MESH_OP_DISSOLVE, 0.0f, "Merges the faces on both sides (Delete)");
        ImGui::SameLine();
        operation_button("Fill holes", MESH_OP_FILL, 0.0f, "Closes the open borders the selection touches");
        ImGui::TextDisabled("Shift + drag a border edge : extend it.");
    }
    else if (editor_selection_mode == SELECT_FACES)
    {
        ImGui::SeparatorText("Faces");
        operation_button(ICON_EXTRUDE " Extrude", MESH_OP_EXTRUDE, extrude_distance, "Along their normal (or Shift + drag)");
        ImGui::SameLine();
        ImGui::SetNextItemWidth(80);
        ImGui::DragFloat("##extrude", &extrude_distance, 0.05f, -100.0f, 100.0f, "%.2f m");
        ImGui::SameLine();
        operation_button(ICON_INSET " Inset", MESH_OP_INSET, inset_distance, "Each face shrunk, with a ring of faces around");
        ImGui::SameLine();
        ImGui::SetNextItemWidth(80);
        ImGui::DragFloat("##inset", &inset_distance, 0.01f, 0.001f, 100.0f, "%.2f m");
        operation_button(ICON_FLIP " Flip", MESH_OP_FLIP, 0.0f, "Turns them inside out");
        ImGui::SameLine();
        operation_button("Bridge", MESH_OP_BRIDGE, 0.0f, "Two faces with as many vertices : a tunnel between them");
        ImGui::SameLine();
        operation_button("Detach", MESH_OP_DETACH, 0.0f, "Into a new object");
        ImGui::SameLine();
        operation_button("Collapse", MESH_OP_COLLAPSE, 0.0f, "Each to its center (M)");
        ImGui::SameLine();
        operation_button(ICON_TRASH "##delete_faces", MESH_OP_DELETE, 0.0f, "Delete (Delete)");
        operation_button("Grow", MESH_OP_GROW, 0.0f, NULL);
        ImGui::SameLine();
        operation_button("Shrink", MESH_OP_SHRINK, 0.0f, NULL);
        ImGui::SameLine();
        operation_button("All", MESH_OP_SELECT_ALL, 0.0f, "Ctrl+A");
        ImGui::SameLine();
        operation_button("Invert", MESH_OP_INVERT, 0.0f, NULL);

        ImGui::SeparatorText("Texture");
        draw_texture_section();
    }
    else
    {
        ImGui::SeparatorText(editor_selection_mode == SELECT_MESHES ? "Meshes" : "Objects");
        operation_button(ICON_MERGE " Merge", MESH_OP_MERGE_MESHES, 0.0f, "The selected meshes into one");
        ImGui::SameLine();
        operation_button("Hollow", MESH_OP_HOLLOW, hollow_thickness, "Walls of this thickness, inside the mesh");
        ImGui::SameLine();
        ImGui::SetNextItemWidth(80);
        ImGui::DragFloat("##hollow", &hollow_thickness, 0.01f, 0.01f, 100.0f, "%.2f m");
        operation_button(ICON_FLIP " Flip", MESH_OP_FLIP, 0.0f, "Inside out (a room seen from inside)");
        ImGui::SameLine();
        operation_button("Fill holes", MESH_OP_FILL, 0.0f, NULL);
        ImGui::SameLine();
        operation_button("Snap", MESH_OP_SNAP, 0.0f, "Every vertex to the grid");
        ImGui::SameLine();
        operation_button("Center origin", MESH_OP_CENTER_ORIGIN, 0.0f, NULL);

        if (ImGui::Button(ICON_PAINT " Apply active material"))
        {
            level_apply_material(editor_active_material);
        }
    }

    // Visibility.
    ImGui::SeparatorText("Visibility");

    if (ImGui::Button(ICON_EYE_OFF " Hide"))
    {
        level_hide_selected(false);
    }

    if (ImGui::IsItemHovered())
    {
        ImGui::SetTooltip("Hides the selection (H)");
    }

    ImGui::SameLine();

    if (ImGui::Button("Hide others"))
    {
        level_hide_selected(true);
    }

    if (ImGui::IsItemHovered())
    {
        ImGui::SetTooltip("Hides everything else (Shift+H)");
    }

    ImGui::SameLine();

    if (ImGui::Button(ICON_EYE " Show all"))
    {
        level_unhide_all();
    }

    if (ImGui::IsItemHovered())
    {
        ImGui::SetTooltip("Shows everything (U)");
    }

    ImGui::Checkbox(ICON_VIEWS " 2D views", &editor_show_2d_views);

    ImGui::EndDisabled();
    ImGui::End();
}

void editor_draw_materials_panel(void)
{
    ImGuiIO& io = ImGui::GetIO();
    ImGui::SetNextWindowPos(ImVec2(io.DisplaySize.x - 312 - 278, 40), ImGuiCond_FirstUseEver);
    ImGui::SetNextWindowSize(ImVec2(270, 300), ImGuiCond_FirstUseEver);
    ImGui::Begin(ICON_PAINT " Materials");

    const char* active_name = SDL_strrchr(editor_active_material, '/');
    ImGui::Text("Active : %s", active_name ? active_name + 1 : editor_active_material);
    ImGui::SameLine();

    ImGui::BeginDisabled(editor_is_playing());

    if (ImGui::SmallButton("Apply"))
    {
        level_apply_material(editor_active_material);
    }

    ImGui::EndDisabled();

    if (ImGui::IsItemHovered())
    {
        ImGui::SetTooltip("To the selected faces (faces mode) or meshes");
    }

    static char filter[64];
    ImGui::SetNextItemWidth(-1.0f);
    ImGui::InputTextWithHint("##filter", ICON_SEARCH " Filter", filter, sizeof(filter));
    ImGui::Separator();

    ImGui::BeginChild("materials");

    const float tile = 56.0f;
    float width = ImGui::GetContentRegionAvail().x;
    int columns = SDL_max(1, (int)(width / (tile + ImGui::GetStyle().ItemSpacing.x)));
    int column = 0;

    // Built-in materials, then the project's textures.
    int builtin_count = asset_builtin_material_count();
    int total = builtin_count + asset_database_count();

    for (int i = 0; i < total; i++)
    {
        const char* path;
        const char* label;

        if (i < builtin_count)
        {
            path = asset_builtin_material(i)->path;
            label = asset_builtin_material(i)->label;
        }
        else
        {
            const AssetEntry* entry = asset_database_get(i - builtin_count);

            if (entry->type != ASSET_TEXTURE)
            {
                continue;
            }

            path = entry->path;
            label = entry->name;
        }

        if (filter[0] && !SDL_strcasestr(label, filter))
        {
            continue;
        }

        if (column > 0)
        {
            ImGui::SameLine();
        }

        ImGui::PushID(path);

        Texture* thumbnail = asset_get_thumbnail(path);
        bool active = SDL_strcmp(path, editor_active_material) == 0;
        ImVec2 position = ImGui::GetCursorScreenPos();
        bool clicked = ImGui::InvisibleButton("##material", ImVec2(tile, tile));
        bool hovered = ImGui::IsItemHovered();
        ImDrawList* draw_list = ImGui::GetWindowDrawList();

        if (thumbnail)
        {
            draw_list->AddImage(pireneus_imgui_texture(thumbnail), position, ImVec2(position.x + tile, position.y + tile));
        }
        else
        {
            float color[3];
            asset_material_color(path, color);
            draw_list->AddRectFilled(position, ImVec2(position.x + tile, position.y + tile), ImGui::ColorConvertFloat4ToU32(ImVec4(color[0], color[1], color[2], 1.0f)));
        }

        draw_list->AddRect(position, ImVec2(position.x + tile, position.y + tile), active ? COLOR_SELECTION : hovered ? COLOR_HOVER : IM_COL32(0, 0, 0, 120), 0.0f, active ? 3.0f : 1.0f);

        if (clicked)
        {
            SDL_snprintf(editor_active_material, sizeof(editor_active_material), "%s", path);
        }

        if (hovered && ImGui::IsMouseDoubleClicked(ImGuiMouseButton_Left) && !editor_is_playing())
        {
            level_apply_material(path);
        }

        if (hovered)
        {
            ImGui::SetTooltip("%s%s\nClick : active material\nDouble click : apply to the selection\nDrag : onto a face",
                label, asset_is_tool_material(path) ? "\n(tool : drawn in the editor only)" : "");
        }

        if (ImGui::BeginDragDropSource())
        {
            ImGui::SetDragDropPayload(ASSET_DRAG_TYPE, path, SDL_strlen(path) + 1);
            ImGui::Text("%s  %s", ICON_PAINT, label);
            ImGui::EndDragDropSource();
        }

        ImGui::PopID();
        column = (column + 1) % columns;
    }

    ImGui::EndChild();
    ImGui::End();
}
