// The editor : working on projects (see src/project). Scenes of models, editable meshes and sprites,
// assets imported through the asset browser, rigid bodies and a character controller simulated in play mode,
// rendered by the photorealistic renderer or the simple forward one, as the project's settings say. Level
// editing (whiteboxing, after Valve's Hammer) is in level_editor.cpp.
//
// The 3D scene is drawn by the engine across the whole window. On top of it, a full-window, transparent
// ImGui window ("viewport") hosts everything that interacts with the 3D view : ImGuizmo needs a real
// window's draw list, and that window also receives the camera/picking mouse input.
//
// Controls, over the viewport (positional keys named after their QWERTY position) :
//   Left click              select (Ctrl : add / remove ; drag on nothing : box selection)
//   Right drag              fly (noclip) : mouse look, W/S forward/backward, A/D left/right,
//                           Q/E down/up, Shift faster, wheel = flying speed
//   Alt + left drag         orbit                   Middle drag    pan
//   Mouse wheel             zoom                    F              focus the selection
//   1 2 3 4 5               vertices / edges / faces / meshes / objects
//   T / R / E               move / rotate / scale gizmo
//   Shift + B, Shift + X    block tool, clipping tool (Enter applies, Escape cancels)
//   [ / ]                   finer / coarser grid
//   Shift (while dragging)  objects : snap ; meshes : clone ; faces, edges : extrude
//   Delete, Ctrl+D          delete / duplicate the selection
//   H, Shift + H, U         hide the selection, hide the rest, show everything
//   Ctrl+Z / Ctrl+Y         undo / redo             Ctrl+S         save
//   Ctrl+P                  play / stop
//
// In play mode : click the view to control the character (W/A/S/D, mouse, Space, Shift), Escape to release.

#include "editor.h"

#include "audio/audio.h"
#include "debug.h"
#include "example.h"
#include "platforms/platforms_input.h"
#include "platforms/platforms_window.h"
#include "meshedit/edit_mesh_gpu.h"
#include "renderer/photoreal/photoreal.h"
#include "renderer/renderer.h"
#include "scene/scene_runtime.h"

// Positional key bindings (scancodes : physical positions, named after US QWERTY). Swap these to
// rebind ; the UI hints show whatever these keys are labeled on the current layout.
#define KEY_FLY_FORWARD SCANCODE_W   // AZERTY : Z
#define KEY_FLY_BACKWARD SCANCODE_S  // AZERTY : S
#define KEY_FLY_LEFT SCANCODE_A      // AZERTY : Q
#define KEY_FLY_RIGHT SCANCODE_D     // AZERTY : D
#define KEY_FLY_DOWN SCANCODE_Q      // AZERTY : A
#define KEY_FLY_UP SCANCODE_E        // AZERTY : E
#define KEY_FLY_FAST SCANCODE_LSHIFT
#define KEY_PLAY_FORWARD SCANCODE_W
#define KEY_PLAY_BACKWARD SCANCODE_S
#define KEY_PLAY_LEFT SCANCODE_A
#define KEY_PLAY_RIGHT SCANCODE_D
#define KEY_PLAY_JUMP SCANCODE_SPACE
#define KEY_PLAY_SPRINT SCANCODE_LSHIFT

#define FLY_LOOK_SENSITIVITY 0.005f // radians per pixel
#define FLY_FAST_MULTIPLIER 4.0f
#define PLAY_LOOK_SENSITIVITY 0.003f

#define MAX_UNDO_STEPS 64
#define SHADOW_MAP_SIZE 2048

const PrimitiveInfo primitive_infos[PRIMITIVE_COUNT] = {
    { "cube", "Cube", ICON_BOX },
    { "plane", "Plane", ICON_SQUARE },
    { "cylinder", "Cylinder", ICON_CYLINDER },
    { "cone", "Cone", ICON_CONE },
    { "sphere", "Sphere", ICON_CIRCLE },
    { "ico-sphere", "Ico Sphere", ICON_GLOBE },
    { "torus", "Torus", ICON_TORUS }
};

// --- Editor state -----------------------------------------------------------------------------

Project editor_project;
Scene editor_scene;
char editor_scene_path[ASSET_PATH_SIZE];
int editor_dirty;

bool editor_show_grid = true;
bool editor_show_project_settings;
bool editor_show_2d_views;
bool editor_view_2d;
bool editor_snap_enabled;
float editor_snap_translation = 0.5f;
float editor_snap_rotation = 15.0f;
float editor_snap_scale = 0.25f;
ImGuizmo::OPERATION editor_gizmo_operation = ImGuizmo::TRANSLATE;
ImGuizmo::MODE editor_gizmo_mode = ImGuizmo::LOCAL;

static char requested_project[PROJECT_PATH_SIZE];
static char status_message[256];

static Scene frame_start_scene; // the scene as it was when this frame started, pushed onto the undo stack when an edit begins

typedef struct
{
    SceneObject* objects;
    int count;
    int selected;
    char skybox[SCENE_PATH_SIZE];
    float sun_yaw, sun_pitch, sun_intensity;
    int use_environment_sun;
} SceneSnapshot;

typedef struct
{
    SceneSnapshot steps[MAX_UNDO_STEPS];
    int count;
} SnapshotStack;

static SnapshotStack undo_stack;
static SnapshotStack redo_stack;

typedef struct
{
    Vec3 target;
    float yaw;      // radians, around +Y
    float pitch;    // radians, above the horizon
    float distance;
} OrbitCamera;

static OrbitCamera camera = { { 0.0f, 0.5f, 0.0f }, 0.75f, 0.45f, 12.0f };

typedef enum
{
    CAMERA_DRAG_NONE,
    CAMERA_DRAG_ORBIT,
    CAMERA_DRAG_PAN,
    CAMERA_DRAG_FLY
} CameraDrag;

static CameraDrag camera_drag;
static float fly_speed = 5.0f;

// Play mode.
static bool playing;
static bool paused;
static bool step_once;
static bool game_has_input;
static Scene play_snapshot; // the scene as it was before pressing play, restored by stop

// Rendering.
static bool photoreal_ready;
static Environment procedural_environment;
static float procedural_sun[3];
static bool procedural_valid;
static ShadowMap* forward_shadow_map;

void editor_set_status(const char* format, ...)
{
    va_list args;
    va_start(args, format);
    SDL_vsnprintf(status_message, sizeof(status_message), format, args);
    va_end(args);
}

const char* editor_status(void)
{
    return status_message;
}

extern "C" void editor_request_project(const char* path)
{
    SDL_snprintf(requested_project, sizeof(requested_project), "%s", path);
}

// --- Undo / redo ------------------------------------------------------------------------------

static void copy_scene(Scene* to, const Scene* from)
{
    SDL_memcpy(to->objects, from->objects, sizeof(SceneObject) * (size_t)from->count);
    to->count = from->count;
    to->selected = from->selected;
    SDL_memcpy(to->skybox, from->skybox, sizeof(to->skybox));
    to->sun_yaw = from->sun_yaw;
    to->sun_pitch = from->sun_pitch;
    to->sun_intensity = from->sun_intensity;
    to->use_environment_sun = from->use_environment_sun;
}

static void snapshot_free(SceneSnapshot* snapshot)
{
    SDL_free(snapshot->objects);
    snapshot->objects = NULL;
}

static void stack_clear(SnapshotStack* stack)
{
    for (int i = 0; i < stack->count; i++)
    {
        snapshot_free(&stack->steps[i]);
    }

    stack->count = 0;
}

static void stack_push(SnapshotStack* stack, const Scene* state)
{
    if (stack->count == MAX_UNDO_STEPS)
    {
        snapshot_free(&stack->steps[0]);
        SDL_memmove(&stack->steps[0], &stack->steps[1], sizeof(SceneSnapshot) * (MAX_UNDO_STEPS - 1));
        stack->count--;
    }

    SceneSnapshot* snapshot = &stack->steps[stack->count++];
    snapshot->objects = (SceneObject*)SDL_malloc(sizeof(SceneObject) * (size_t)SDL_max(1, state->count));
    SDL_memcpy(snapshot->objects, state->objects, sizeof(SceneObject) * (size_t)state->count);
    snapshot->count = state->count;
    snapshot->selected = state->selected;
    SDL_memcpy(snapshot->skybox, state->skybox, sizeof(snapshot->skybox));
    snapshot->sun_yaw = state->sun_yaw;
    snapshot->sun_pitch = state->sun_pitch;
    snapshot->sun_intensity = state->sun_intensity;
    snapshot->use_environment_sun = state->use_environment_sun;
}

static void restore_snapshot(const SceneSnapshot* snapshot)
{
    SDL_memcpy(editor_scene.objects, snapshot->objects, sizeof(SceneObject) * (size_t)snapshot->count);
    editor_scene.count = snapshot->count;
    editor_scene.selected = snapshot->selected;
    SDL_memcpy(editor_scene.skybox, snapshot->skybox, sizeof(editor_scene.skybox));
    editor_scene.sun_yaw = snapshot->sun_yaw;
    editor_scene.sun_pitch = snapshot->sun_pitch;
    editor_scene.sun_intensity = snapshot->sun_intensity;
    editor_scene.use_environment_sun = snapshot->use_environment_sun;
}

static int frame_number;          // counts update() calls
static int last_edit_frame = -1;  // when editor_begin_edit() last recorded a step

void editor_begin_edit(void)
{
    if (playing || last_edit_frame == frame_number)
    {
        return; // what's changed in play mode is thrown away when it stops ; one step per frame at most
    }

    last_edit_frame = frame_number;
    stack_push(&undo_stack, &frame_start_scene);
    stack_clear(&redo_stack);

    // The step shares the meshes from now on : they're copied before being changed (copy on write).
    edit_mesh_store_freeze_all();
    editor_dirty = 1;
}

void editor_cancel_edit(void)
{
    if (playing || undo_stack.count == 0)
    {
        return;
    }

    undo_stack.count--;
    restore_snapshot(&undo_stack.steps[undo_stack.count]);
    snapshot_free(&undo_stack.steps[undo_stack.count]);
}

static void travel(SnapshotStack* from, SnapshotStack* to)
{
    if (from->count == 0 || playing)
    {
        return;
    }

    stack_push(to, &editor_scene);
    edit_mesh_store_freeze_all();
    from->count--;
    restore_snapshot(&from->steps[from->count]);
    snapshot_free(&from->steps[from->count]);
    editor_dirty = 1;
    level_editor_reset();
}

bool editor_can_undo(void) { return undo_stack.count > 0 && !playing; }
bool editor_can_redo(void) { return redo_stack.count > 0 && !playing; }
void editor_undo(void) { travel(&undo_stack, &redo_stack); }
void editor_redo(void) { travel(&redo_stack, &undo_stack); }

// --- Objects ----------------------------------------------------------------------------------

static void unique_name(const char* base, char* out, size_t out_size)
{
    for (int number = 1; ; number++)
    {
        if (number == 1)
        {
            SDL_snprintf(out, out_size, "%s", base);
        }
        else
        {
            SDL_snprintf(out, out_size, "%s %d", base, number);
        }

        bool taken = false;

        for (int i = 0; i < editor_scene.count && !taken; i++)
        {
            taken = SDL_strcmp(editor_scene.objects[i].name, out) == 0;
        }

        if (!taken)
        {
            return;
        }
    }
}

SceneObject* editor_add_object(const char* name)
{
    if (editor_scene.count == SCENE_MAX_OBJECTS)
    {
        editor_set_status("The scene is full (%d objects).", SCENE_MAX_OBJECTS);
        return NULL;
    }

    editor_begin_edit();

    char unique[64];
    unique_name(name, unique, sizeof(unique));

    SceneObject* object = &editor_scene.objects[editor_scene.count];
    scene_object_init(object, unique);
    object->is_2d = editor_view_2d;

    for (int i = 0; i < editor_scene.count; i++)
    {
        editor_scene.objects[i].selected = 0;
    }

    object->selected = 1;
    editor_scene.selected = editor_scene.count;
    editor_scene.count++;

    return object;
}

SceneObject* editor_add_mesh(EditMesh* mesh, const char* name)
{
    SceneObject* object = editor_add_object(name);

    if (!object)
    {
        edit_mesh_destroy(mesh);
        return NULL;
    }

    object->mesh = edit_mesh_store_add(mesh);
    object->is_2d = 0;
    object->body_type = SCENE_BODY_STATIC; // level geometry : what the player walks on and bumps into

    return object;
}

EditMesh* editor_writable_mesh(SceneObject* object)
{
    if (!object->mesh)
    {
        return NULL;
    }

    if (edit_mesh_store_frozen(object->mesh))
    {
        object->mesh = edit_mesh_store_copy(object->mesh);
    }

    return edit_mesh_store_get(object->mesh);
}

// --- Selection --------------------------------------------------------------------------------

void editor_select_object(int index, int how)
{
    if (how == SELECT_REPLACE)
    {
        for (int i = 0; i < editor_scene.count; i++)
        {
            editor_scene.objects[i].selected = 0;
        }
    }

    if (index < 0 || index >= editor_scene.count)
    {
        if (how == SELECT_REPLACE)
        {
            editor_scene.selected = -1;
        }

        return;
    }

    SceneObject* object = &editor_scene.objects[index];

    if (how == SELECT_TOGGLE && object->selected)
    {
        object->selected = 0;

        if (editor_scene.selected == index)
        {
            // Another selected object becomes the primary one.
            editor_scene.selected = -1;

            for (int i = 0; i < editor_scene.count && editor_scene.selected < 0; i++)
            {
                if (editor_scene.objects[i].selected)
                {
                    editor_scene.selected = i;
                }
            }
        }

        return;
    }

    object->selected = 1;
    editor_scene.selected = index;
}

void editor_select_none(void)
{
    editor_select_object(-1, SELECT_REPLACE);
}

int editor_selection_count(void)
{
    int count = 0;

    for (int i = 0; i < editor_scene.count; i++)
    {
        count += editor_scene.objects[i].selected != 0;
    }

    return count;
}

void editor_fix_selection(void)
{
    if (editor_scene.selected >= editor_scene.count)
    {
        editor_scene.selected = -1;
    }

    // Code that sets the primary object directly (Scene::selected) means it alone ; -1 means nothing.
    if (editor_scene.selected < 0 || !editor_scene.objects[editor_scene.selected].selected)
    {
        for (int i = 0; i < editor_scene.count; i++)
        {
            editor_scene.objects[i].selected = i == editor_scene.selected;
        }
    }
}

void editor_drop_point(float out[3])
{
    out[0] = camera.target.x;
    out[1] = editor_view_2d ? camera.target.y : 0.0f;
    out[2] = editor_view_2d ? 0.0f : camera.target.z;
}

SceneObject* editor_add_model(const char* model_path, const char* name)
{
    const LoadedModel* model = asset_get_model(model_path);

    if (!model)
    {
        editor_set_status("Could not load '%s'.", model_path);
        return NULL;
    }

    SceneObject* object = editor_add_object(name);

    if (object)
    {
        SDL_snprintf(object->model, sizeof(object->model), "%s", model_path);
        object->is_2d = 0;
        editor_drop_point(object->position);
        object->position[1] = -model->bounds[1]; // resting on the ground
    }

    return object;
}

SceneObject* editor_add_sprite(const char* sprite_path)
{
    int width = 0;
    int height = 0;

    if (!asset_get_sprite(sprite_path, &width, &height))
    {
        editor_set_status("Could not load '%s'.", sprite_path);
        return NULL;
    }

    const char* name = SDL_strrchr(sprite_path, '/');
    SceneObject* object = editor_add_object(name ? name + 1 : sprite_path);

    if (object)
    {
        SDL_snprintf(object->sprite, sizeof(object->sprite), "%s", sprite_path);
        object->is_2d = 1;
        object->sprite_size[0] = (float)width / (float)SDL_max(1, SDL_max(width, height));
        object->sprite_size[1] = (float)height / (float)SDL_max(1, SDL_max(width, height));
        editor_drop_point(object->position);
    }

    return object;
}

void editor_delete_selected(void)
{
    editor_fix_selection();

    if (editor_selection_count() == 0 || playing)
    {
        return;
    }

    editor_begin_edit();

    int kept = 0;

    for (int i = 0; i < editor_scene.count; i++)
    {
        if (!editor_scene.objects[i].selected)
        {
            editor_scene.objects[kept++] = editor_scene.objects[i];
        }
    }

    editor_scene.count = kept;
    editor_scene.selected = -1;
}

// Copies of the selected objects (and of their meshes : no two objects share one), which become the
// selection, moved by `offset` in X and Z.
static void duplicate_selection(float offset)
{
    editor_fix_selection();

    if (editor_selection_count() == 0 || playing)
    {
        return;
    }

    editor_begin_edit();

    int count = editor_scene.count;
    int primary = editor_scene.selected;
    int new_primary = -1;

    for (int i = 0; i < count && editor_scene.count < SCENE_MAX_OBJECTS; i++)
    {
        SceneObject* original = &editor_scene.objects[i];

        if (!original->selected)
        {
            continue;
        }

        SceneObject copy = *original;
        unique_name(original->name, copy.name, sizeof(copy.name));
        copy.mesh = copy.mesh ? edit_mesh_store_copy(copy.mesh) : 0;
        copy.position[0] += offset;
        copy.position[2] += editor_view_2d ? 0.0f : offset;
        original->selected = 0;

        if (i == primary)
        {
            new_primary = editor_scene.count;
        }

        editor_scene.objects[editor_scene.count++] = copy;
    }

    editor_scene.selected = new_primary >= 0 ? new_primary : editor_scene.count - 1;
}

void editor_duplicate_selected(void)
{
    duplicate_selection(1.0f);
}

void editor_clone_selected(void)
{
    duplicate_selection(0.0f);
}

// --- Scenes -----------------------------------------------------------------------------------

static void create_default_scene(Scene* scene, int template_kind)
{
    scene_init(scene);

    if (template_kind == PROJECT_TEMPLATE_2D)
    {
        SceneObject* ground = &scene->objects[scene->count++];
        scene_object_init(ground, "Ground");
        ground->is_2d = 1;
        ground->body_type = SCENE_BODY_STATIC;
        ground->sprite_size[0] = 24.0f;
        ground->sprite_size[1] = 1.0f;
        ground->position[1] = -3.0f;
        ground->base_color[0] = 0.35f;
        ground->base_color[1] = 0.45f;
        ground->base_color[2] = 0.35f;

        SceneObject* player = &scene->objects[scene->count++];
        scene_object_init(player, "Player");
        player->is_2d = 1;
        player->is_character = 1;
        player->layer = 1;
        player->sprite_size[0] = 0.7f;
        player->sprite_size[1] = 1.4f;
        player->base_color[0] = 0.9f;
        player->base_color[1] = 0.6f;
        player->base_color[2] = 0.2f;

        for (int i = 0; i < 5; i++)
        {
            SceneObject* box = &scene->objects[scene->count++];
            char name[32];
            SDL_snprintf(name, sizeof(name), "Crate %d", i + 1);
            scene_object_init(box, name);
            box->is_2d = 1;
            box->body_type = SCENE_BODY_DYNAMIC;
            box->position[0] = 2.0f + (float)(i % 2) * 0.1f;
            box->position[1] = -2.0f + (float)i * 1.05f;
            box->base_color[0] = 0.55f;
            box->base_color[1] = 0.4f;
            box->base_color[2] = 0.25f;
        }

        return;
    }

    // The ground : a slab of level geometry, its top at y = 0.
    EditShapeParams slab;
    edit_mesh_default_shape(&slab, EDIT_SHAPE_BOX);
    const float slab_size[3] = { 30.0f, 0.5f, 30.0f };

    SceneObject* ground = &scene->objects[scene->count++];
    scene_object_init(ground, "Ground");
    ground->mesh = edit_mesh_store_add(edit_mesh_make_shape(&slab, slab_size, ASSET_DEFAULT_MATERIAL));
    ground->position[1] = -0.25f;
    ground->body_type = SCENE_BODY_STATIC;

    SceneObject* player = &scene->objects[scene->count++];
    scene_object_init(player, "Player");
    SDL_snprintf(player->model, sizeof(player->model), SCENE_BUILTIN_PREFIX "cylinder");
    player->position[1] = 1.2f;
    player->position[2] = 4.0f;
    player->scale[0] = player->scale[2] = 0.4f;
    player->scale[1] = 0.9f;
    player->is_character = 1;
    player->layer = 1;
    player->override_material = 1;
    player->base_color[0] = 0.9f;
    player->base_color[1] = 0.55f;
    player->base_color[2] = 0.15f;
    player->roughness = 0.35f;

    static const struct { float x, y, z, r, g, b, metallic, roughness; const char* model; } props[] = {
        { 0.0f, 1.0f, 0.0f, 0.7f, 0.1f, 0.1f, 0.0f, 0.4f, "cube" },
        { 0.1f, 3.05f, 0.0f, 0.1f, 0.3f, 0.75f, 0.0f, 0.25f, "cube" },
        { 3.5f, 1.0f, -1.0f, 0.95f, 0.8f, 0.45f, 1.0f, 0.2f, "sphere" },
        { -3.5f, 1.0f, -1.5f, 0.75f, 0.75f, 0.78f, 1.0f, 0.08f, "sphere" }
    };

    for (size_t i = 0; i < sizeof(props) / sizeof(props[0]); i++)
    {
        SceneObject* object = &scene->objects[scene->count++];
        char name[32];
        SDL_snprintf(name, sizeof(name), "Prop %d", (int)i + 1);
        scene_object_init(object, name);
        SDL_snprintf(object->model, sizeof(object->model), SCENE_BUILTIN_PREFIX "%s", props[i].model);
        object->position[0] = props[i].x;
        object->position[1] = props[i].y;
        object->position[2] = props[i].z;
        object->body_type = SCENE_BODY_DYNAMIC;
        object->override_material = 1;
        object->base_color[0] = props[i].r;
        object->base_color[1] = props[i].g;
        object->base_color[2] = props[i].b;
        object->metallic = props[i].metallic;
        object->roughness = props[i].roughness;
        object->layer = 3;
    }
}

void editor_new_scene(void)
{
    if (playing)
    {
        return;
    }

    create_default_scene(&editor_scene, editor_project.template_kind);
    SDL_snprintf(editor_scene_path, sizeof(editor_scene_path), "Assets/Scenes/Untitled.scene");
    stack_clear(&undo_stack);
    stack_clear(&redo_stack);
    level_editor_reset();
    editor_dirty = 1;
    editor_set_status("New scene.");
}

void editor_save_scene(void)
{
    if (playing)
    {
        editor_set_status("Stop playing before saving.");
        return;
    }

    char path[PROJECT_PATH_SIZE + ASSET_PATH_SIZE];
    asset_absolute_path(editor_scene_path, path, sizeof(path));

    if (scene_save(&editor_scene, path))
    {
        editor_dirty = 0;
        editor_set_status("Saved '%s' (%d objects).", editor_scene_path, editor_scene.count);
        asset_database_refresh();
    }
    else
    {
        editor_set_status("Could not save '%s'.", editor_scene_path);
    }
}

void editor_load_scene(const char* relative_path)
{
    if (playing)
    {
        return;
    }

    char copy[ASSET_PATH_SIZE];
    SDL_snprintf(copy, sizeof(copy), "%s", relative_path);

    char path[PROJECT_PATH_SIZE + ASSET_PATH_SIZE];
    asset_absolute_path(copy, path, sizeof(path));

    if (!scene_load(&editor_scene, path))
    {
        editor_set_status("Could not read '%s'.", copy);
        return;
    }

    SDL_snprintf(editor_scene_path, sizeof(editor_scene_path), "%s", copy);
    stack_clear(&undo_stack);
    stack_clear(&redo_stack);
    level_editor_reset();
    editor_dirty = 0;
    editor_set_status("Loaded '%s' (%d objects).", copy, editor_scene.count);
}

void editor_apply_settings(void)
{
    const ProjectSettings* settings = &editor_project.settings;

    if (photoreal_ready)
    {
        PhotorealQuality quality = settings->quality;
        quality.path_tracing = settings->render_path == RENDER_PATH_PATH_TRACED;
        photoreal_set_settings(&quality, &settings->look);
    }

    audio_set_volumes(settings->master_volume, settings->music_volume, settings->sound_volume);
}

void editor_save_project(void)
{
    if (project_save(&editor_project))
    {
        editor_set_status("Project settings saved.");
    }
    else
    {
        editor_set_status("Could not save the project's settings.");
    }
}

// --- Camera -----------------------------------------------------------------------------------

static Vec3 camera_eye(void)
{
    if (editor_view_2d)
    {
        return vec3(camera.target.x, camera.target.y, 50.0f);
    }

    Vec3 offset = vec3(SDL_cosf(camera.pitch) * SDL_sinf(camera.yaw), SDL_sinf(camera.pitch), SDL_cosf(camera.pitch) * SDL_cosf(camera.yaw));
    return vec3_add(camera.target, vec3_scale(offset, camera.distance));
}

void editor_focus_on(const float bounds[6])
{
    camera.target = vec3((bounds[0] + bounds[3]) * 0.5f, (bounds[1] + bounds[4]) * 0.5f, (bounds[2] + bounds[5]) * 0.5f);

    float radius = 0.5f * vec3_length(vec3(bounds[3] - bounds[0], bounds[4] - bounds[1], bounds[5] - bounds[2]));
    camera.distance = SDL_clamp(radius * 2.8f, 2.0f, 200.0f);
}

void editor_focus_selected(void)
{
    editor_fix_selection();

    float world[6] = { 1e30f, 1e30f, 1e30f, -1e30f, -1e30f, -1e30f };
    int found = 0;

    for (int i = 0; i < editor_scene.count; i++)
    {
        const SceneObject* object = &editor_scene.objects[i];

        if (!object->selected)
        {
            continue;
        }

        float local[6];
        editor_object_bounds(object, local);
        Mat4 matrix = scene_object_matrix(object);

        for (int c = 0; c < 8; c++)
        {
            float p[4];
            mat4_transform(matrix, local[(c & 1) ? 3 : 0], local[(c & 2) ? 4 : 1], local[(c & 4) ? 5 : 2], 1.0f, p);

            for (int axis = 0; axis < 3; axis++)
            {
                world[axis] = SDL_min(world[axis], p[axis]);
                world[axis + 3] = SDL_max(world[axis + 3], p[axis]);
            }
        }

        found = 1;
    }

    if (found)
    {
        editor_focus_on(world);
    }
}

// --- Picking ----------------------------------------------------------------------------------

// Ray / triangle intersection (Möller–Trumbore) : the ray parameter t of the hit, or -1.
static float intersect_triangle(Vec3 origin, Vec3 direction, Vec3 a, Vec3 b, Vec3 c)
{
    Vec3 edge1 = vec3_sub(b, a);
    Vec3 edge2 = vec3_sub(c, a);
    Vec3 p = vec3_cross(direction, edge2);
    float determinant = vec3_dot(edge1, p);

    if (SDL_fabsf(determinant) < 1e-9f)
    {
        return -1.0f;
    }

    float inverse = 1.0f / determinant;
    Vec3 s = vec3_sub(origin, a);
    float u = vec3_dot(s, p) * inverse;

    if (u < 0.0f || u > 1.0f)
    {
        return -1.0f;
    }

    Vec3 q = vec3_cross(s, edge1);
    float v = vec3_dot(direction, q) * inverse;

    if (v < 0.0f || u + v > 1.0f)
    {
        return -1.0f;
    }

    float t = vec3_dot(edge2, q) * inverse;
    return t > 0.0f ? t : -1.0f;
}

// Ray / axis-aligned box (slab test) : whether the ray hits the box at all, as an early-out.
static bool intersects_box(Vec3 origin, Vec3 direction, const float bounds[6])
{
    float t_min = 0.0f;
    float t_max = 1e30f;
    const float o[3] = { origin.x, origin.y, origin.z };
    const float d[3] = { direction.x, direction.y, direction.z };

    for (int axis = 0; axis < 3; axis++)
    {
        float lo = bounds[axis] - 1e-4f;
        float hi = bounds[axis + 3] + 1e-4f;

        if (SDL_fabsf(d[axis]) < 1e-9f)
        {
            if (o[axis] < lo || o[axis] > hi)
            {
                return false;
            }

            continue;
        }

        float t1 = (lo - o[axis]) / d[axis];
        float t2 = (hi - o[axis]) / d[axis];
        t_min = SDL_max(t_min, SDL_min(t1, t2));
        t_max = SDL_min(t_max, SDL_max(t1, t2));
    }

    return t_min <= t_max;
}

// A sprite's or 2D shape's local bounds : a quad in the XY plane, centered.
static void flat_bounds(const SceneObject* object, float out[6])
{
    out[0] = -object->sprite_size[0] * 0.5f;
    out[1] = -object->sprite_size[1] * 0.5f;
    out[2] = -0.01f;
    out[3] = object->sprite_size[0] * 0.5f;
    out[4] = object->sprite_size[1] * 0.5f;
    out[5] = 0.01f;
}

static bool hidden_now(const SceneObject* object)
{
    return object->hidden && !playing; // hiding is the editor's : the game shows everything
}

void editor_mouse_ray(const CameraMatrices* matrices, ImVec2 point, Vec3* out_origin, Vec3* out_direction)
{
    ImVec2 display = ImGui::GetIO().DisplaySize;
    Mat4 inverse = mat4_inverse(matrices->view_projection);
    float ndc_x = point.x / SDL_max(1.0f, display.x) * 2.0f - 1.0f;
    float ndc_y = 1.0f - point.y / SDL_max(1.0f, display.y) * 2.0f;

    float near_point[4];
    float far_point[4];
    mat4_transform(inverse, ndc_x, ndc_y, 0.0f, 1.0f, near_point);
    mat4_transform(inverse, ndc_x, ndc_y, 1.0f, 1.0f, far_point);

    Vec3 origin = vec3(near_point[0] / near_point[3], near_point[1] / near_point[3], near_point[2] / near_point[3]);
    Vec3 target = vec3(far_point[0] / far_point[3], far_point[1] / far_point[3], far_point[2] / far_point[3]);

    *out_origin = origin;
    *out_direction = vec3_sub(target, origin);
}

int editor_object_bounds(const SceneObject* object, float out[6])
{
    if (object->mesh)
    {
        const EditMesh* mesh = edit_mesh_store_get(object->mesh);

        if (mesh && mesh->vertex_count > 0)
        {
            edit_mesh_bounds(mesh, out);
            return 1;
        }
    }

    const LoadedModel* model = object->model[0] ? asset_get_model(object->model) : NULL;

    if (model)
    {
        SDL_memcpy(out, model->bounds, sizeof(float) * 6);
        return 1;
    }

    if (object->sprite[0] || object->is_2d)
    {
        flat_bounds(object, out);
        return 1;
    }

    for (int axis = 0; axis < 3; axis++)
    {
        out[axis] = -0.25f;
        out[axis + 3] = 0.25f;
    }

    return 0;
}

// The ray is moved into each object's local space (origin and direction both transformed, direction left
// unnormalized), where its parameter t stays the same as in world space : hits on different objects
// compare directly.
int editor_raycast(Vec3 origin, Vec3 direction, int filter, float* out_t, int* out_face)
{
    int best = -1;
    int best_face = -1;
    float best_t = 1e30f;

    for (int i = 0; i < editor_scene.count; i++)
    {
        const SceneObject* object = &editor_scene.objects[i];

        if (hidden_now(object))
        {
            continue;
        }

        const EditMesh* mesh = object->mesh ? edit_mesh_store_get(object->mesh) : NULL;

        if (filter == RAYCAST_MESHES && !mesh)
        {
            continue;
        }

        Mat4 inverse_model = mat4_inverse(scene_object_matrix(object));

        float local_origin[4];
        float local_direction[4];
        mat4_transform(inverse_model, origin.x, origin.y, origin.z, 1.0f, local_origin);
        mat4_transform(inverse_model, direction.x, direction.y, direction.z, 0.0f, local_direction);

        Vec3 o = vec3(local_origin[0], local_origin[1], local_origin[2]);
        Vec3 d = vec3(local_direction[0], local_direction[1], local_direction[2]);

        if (mesh)
        {
            float bounds[6];
            edit_mesh_bounds(mesh, bounds);

            if (!intersects_box(o, d, bounds))
            {
                continue;
            }

            float t = best_t;
            int face = -1;

            if (edit_mesh_raycast(mesh, local_origin, local_direction, &t, &face))
            {
                best_t = t;
                best = i;
                best_face = face;
            }

            continue;
        }

        const LoadedModel* model = object->model[0] ? asset_get_model(object->model) : NULL;

        if (!model)
        {
            if (object->sprite[0] || object->is_2d)
            {
                // The quad's plane is z = 0 in local space.
                float bounds[6];
                flat_bounds(object, bounds);

                if (SDL_fabsf(d.z) > 1e-9f)
                {
                    float t = -o.z / d.z;
                    float x = o.x + d.x * t;
                    float y = o.y + d.y * t;

                    if (t > 0.0f && t < best_t && x >= bounds[0] && x <= bounds[3] && y >= bounds[1] && y <= bounds[4])
                    {
                        best_t = t;
                        best = i;
                        best_face = -1;
                    }
                }
            }

            continue;
        }

        if (!intersects_box(o, d, model->bounds))
        {
            continue;
        }

        const ModelData* data = &model->data;

        for (int t = 0; t + 2 < data->index_count; t += 3)
        {
            const MeshVertex* a = &data->vertices[data->indices[t]];
            const MeshVertex* b = &data->vertices[data->indices[t + 1]];
            const MeshVertex* c = &data->vertices[data->indices[t + 2]];
            float hit = intersect_triangle(o, d, vec3(a->x, a->y, a->z), vec3(b->x, b->y, b->z), vec3(c->x, c->y, c->z));

            if (hit > 0.0f && hit < best_t)
            {
                best_t = hit;
                best = i;
                best_face = -1;
            }
        }
    }

    if (out_t)
    {
        *out_t = best_t;
    }

    if (out_face)
    {
        *out_face = best_face;
    }

    return best;
}

// --- Viewport ---------------------------------------------------------------------------------

static ImVec2 central_node_min;
static ImVec2 central_node_size;

void editor_view_rect(ImVec2* out_min, ImVec2* out_size)
{
    *out_min = central_node_min;
    *out_size = central_node_size;
}

// The 3D view is rendered over the whole window (panels docked at its edges cover the rest), but
// framed on the central node : a perspective with the central node's aspect ratio, whose clip space
// is then scaled and offset into the central node's sub-rectangle of the window's clip space. The
// optical center stays in the middle of the visible area, however the panels are docked.
static CameraMatrices camera_matrices(void)
{
    ImVec2 display = ImGui::GetIO().DisplaySize;
    ImVec2 area_min = central_node_min;
    ImVec2 area_size = central_node_size;

    if (area_size.x < 1.0f || area_size.y < 1.0f || display.x < 1.0f || display.y < 1.0f)
    {
        area_min = ImVec2(0.0f, 0.0f);
        area_size = display.x >= 1.0f && display.y >= 1.0f ? display : ImVec2(1.0f, 1.0f);
        display = area_size;
    }

    float scale_x = area_size.x / display.x;
    float scale_y = area_size.y / display.y;
    float center_x = ((area_min.x + area_size.x * 0.5f) / display.x) * 2.0f - 1.0f;
    float center_y = 1.0f - ((area_min.y + area_size.y * 0.5f) / display.y) * 2.0f; // clip +Y up, window +Y down

    Mat4 to_area = { {
        scale_x, 0.0f, 0.0f, center_x,
        0.0f, scale_y, 0.0f, center_y,
        0.0f, 0.0f, 1.0f, 0.0f,
        0.0f, 0.0f, 0.0f, 1.0f
    } };

    CameraMatrices matrices;
    float aspect = area_size.x / area_size.y;
    Vec3 eye = camera_eye();
    Vec3 target = camera.target;
    bool orthographic = editor_view_2d;
    float half_height = camera.distance * 0.5f;

    RuntimeCamera game_camera;

    if (playing && scene_runtime_camera(&game_camera))
    {
        eye = game_camera.eye;
        target = game_camera.target;
        orthographic = game_camera.orthographic != 0;
        half_height = game_camera.ortho_half_height;
    }

    matrices.eye = eye;
    matrices.orthographic = orthographic;
    matrices.view = mat4_look_at(eye, target, vec3(0.0f, 1.0f, 0.0f));
    matrices.near_plane = orthographic ? 0.1f : 0.05f;
    matrices.far_plane = orthographic ? 200.0f : 500.0f;

    Mat4 projection = orthographic
        ? mat4_orthographic(-half_height * aspect, half_height * aspect, -half_height, half_height, matrices.near_plane, matrices.far_plane)
        : mat4_perspective(50.0f * SDL_PI_F / 180.0f, aspect, matrices.near_plane, matrices.far_plane);

    matrices.projection = mat4_multiply(to_area, projection);
    matrices.view_projection = mat4_multiply(matrices.projection, matrices.view);

    return matrices;
}

static void viewport_camera_input(bool hovered)
{
    ImGuiIO& io = ImGui::GetIO();

    if (editor_view_2d)
    {
        // Flat view : right or middle drag pans, the wheel zooms.
        if (camera_drag == CAMERA_DRAG_NONE && hovered)
        {
            if (ImGui::IsMouseClicked(ImGuiMouseButton_Right) || ImGui::IsMouseClicked(ImGuiMouseButton_Middle))
            {
                camera_drag = CAMERA_DRAG_PAN;
            }

            if (io.MouseWheel != 0.0f)
            {
                camera.distance = SDL_clamp(camera.distance * SDL_powf(0.88f, io.MouseWheel), 1.0f, 200.0f);
            }
        }

        if (camera_drag == CAMERA_DRAG_PAN)
        {
            float speed = camera.distance / SDL_max(1.0f, central_node_size.y);
            camera.target.x -= io.MouseDelta.x * speed;
            camera.target.y += io.MouseDelta.y * speed;

            if (!ImGui::IsMouseDown(ImGuiMouseButton_Right) && !ImGui::IsMouseDown(ImGuiMouseButton_Middle))
            {
                camera_drag = CAMERA_DRAG_NONE;
            }
        }

        return;
    }

    if (camera_drag == CAMERA_DRAG_NONE && hovered)
    {
        if (ImGui::IsMouseClicked(ImGuiMouseButton_Right))
        {
            camera_drag = CAMERA_DRAG_FLY;
        }
        else if (ImGui::IsMouseClicked(ImGuiMouseButton_Left) && io.KeyAlt)
        {
            camera_drag = CAMERA_DRAG_ORBIT;
        }
        else if (ImGui::IsMouseClicked(ImGuiMouseButton_Middle))
        {
            camera_drag = CAMERA_DRAG_PAN;
        }

        if (io.MouseWheel != 0.0f && camera_drag != CAMERA_DRAG_FLY)
        {
            camera.distance = SDL_clamp(camera.distance * SDL_powf(0.88f, io.MouseWheel), 0.5f, 200.0f);
        }
    }

    if (camera_drag == CAMERA_DRAG_FLY)
    {
        // Noclip : the camera turns in place (mouse look) and moves freely, through everything.
        // The orbit camera's state is kept consistent throughout : looking around re-places the
        // orbit target in front of the eye, and moving translates eye and target together, so
        // orbiting/panning/zooming afterwards picks up exactly where flying left off.
        Vec3 eye = camera_eye();

        camera.yaw -= io.MouseDelta.x * FLY_LOOK_SENSITIVITY;
        camera.pitch = SDL_clamp(camera.pitch + io.MouseDelta.y * FLY_LOOK_SENSITIVITY, -1.5f, 1.5f);

        Vec3 offset = vec3(SDL_cosf(camera.pitch) * SDL_sinf(camera.yaw), SDL_sinf(camera.pitch), SDL_cosf(camera.pitch) * SDL_cosf(camera.yaw));
        Vec3 forward = vec3_scale(offset, -1.0f); // the eye looks at the target, opposite to the target->eye offset
        Vec3 right = vec3_normalize(vec3_cross(forward, vec3(0.0f, 1.0f, 0.0f)));
        Vec3 up = vec3(0.0f, 1.0f, 0.0f);

        float axis_forward = (float)(input_is_key_down(KEY_FLY_FORWARD) - input_is_key_down(KEY_FLY_BACKWARD));
        float axis_right = (float)(input_is_key_down(KEY_FLY_RIGHT) - input_is_key_down(KEY_FLY_LEFT));
        float axis_up = (float)(input_is_key_down(KEY_FLY_UP) - input_is_key_down(KEY_FLY_DOWN));

        Vec3 move = vec3_add(vec3_add(vec3_scale(forward, axis_forward), vec3_scale(right, axis_right)), vec3_scale(up, axis_up));

        if (io.MouseWheel != 0.0f)
        {
            fly_speed = SDL_clamp(fly_speed * SDL_powf(1.2f, io.MouseWheel), 0.25f, 200.0f);
        }

        float speed = fly_speed * (input_is_key_down(KEY_FLY_FAST) ? FLY_FAST_MULTIPLIER : 1.0f);

        if (vec3_length(move) > 0.0f)
        {
            eye = vec3_add(eye, vec3_scale(vec3_normalize(move), speed * io.DeltaTime));
        }

        camera.target = vec3_add(eye, vec3_scale(forward, camera.distance));

        if (!ImGui::IsMouseDown(ImGuiMouseButton_Right))
        {
            camera_drag = CAMERA_DRAG_NONE;
        }
    }
    else if (camera_drag == CAMERA_DRAG_ORBIT)
    {
        camera.yaw -= io.MouseDelta.x * 0.008f;
        camera.pitch = SDL_clamp(camera.pitch + io.MouseDelta.y * 0.008f, -1.5f, 1.5f);

        if (!ImGui::IsMouseDown(ImGuiMouseButton_Left))
        {
            camera_drag = CAMERA_DRAG_NONE;
        }
    }
    else if (camera_drag == CAMERA_DRAG_PAN)
    {
        // Move the orbit target in the camera's screen plane, proportionally to the distance, so
        // the point under the cursor roughly follows it.
        Vec3 forward = vec3_normalize(vec3_sub(camera.target, camera_eye()));
        Vec3 right = vec3_normalize(vec3_cross(forward, vec3(0.0f, 1.0f, 0.0f)));
        Vec3 up = vec3_cross(right, forward);
        float speed = camera.distance * 0.0015f;

        camera.target = vec3_add(camera.target, vec3_scale(right, -io.MouseDelta.x * speed));
        camera.target = vec3_add(camera.target, vec3_scale(up, io.MouseDelta.y * speed));

        if (!ImGui::IsMouseDown(ImGuiMouseButton_Middle))
        {
            camera_drag = CAMERA_DRAG_NONE;
        }
    }
}

// An asset dropped onto the view.
static void handle_viewport_drop(const CameraMatrices* matrices)
{
    const ImGuiPayload* payload = ImGui::AcceptDragDropPayload(ASSET_DRAG_TYPE);

    if (!payload)
    {
        return;
    }

    const char* path = (const char*)payload->Data;
    AssetType type = ASSET_OTHER;
    bool builtin_material = asset_find_builtin_material(path) != NULL;

    for (int i = 0; i < asset_database_count(); i++)
    {
        if (SDL_strcmp(asset_database_get(i)->path, path) == 0)
        {
            type = asset_database_get(i)->type;
        }
    }

    if (builtin_material)
    {
        type = ASSET_TEXTURE;
    }

    // Where the mouse points at on the ground (or the XY plane in 2D).
    Vec3 a;
    Vec3 direction;
    editor_mouse_ray(matrices, ImGui::GetIO().MousePos, &a, &direction);
    Vec3 b = vec3_add(a, direction);
    Vec3 hit = camera.target;

    float denominator = editor_view_2d ? b.z - a.z : b.y - a.y;

    if (SDL_fabsf(denominator) > 1e-6f)
    {
        float t = (editor_view_2d ? -a.z : -a.y) / denominator;

        if (t > 0.0f && t < 1.0f)
        {
            hit = vec3_add(a, vec3_scale(vec3_sub(b, a), t));
        }
    }

    const char* file_name = SDL_strrchr(path, '/');
    file_name = file_name ? file_name + 1 : path;

    SceneObject* object = NULL;

    switch (type)
    {
    case ASSET_MODEL:
        object = editor_add_model(path, file_name);

        if (object)
        {
            object->position[0] = hit.x;
            object->position[2] = hit.z;
        }

        break;

    case ASSET_SPRITE:
    case ASSET_TEXTURE:
        if (type == ASSET_TEXTURE && (!editor_view_2d || builtin_material))
        {
            // A texture dropped on a mesh's face becomes its material (or the selected faces', if it's
            // one of them) ; on another 3D object, its albedo.
            int face = -1;
            int target = editor_raycast(a, direction, RAYCAST_ALL, NULL, &face);

            if (target >= 0 && editor_scene.objects[target].mesh && face >= 0)
            {
                editor_begin_edit();
                SceneObject* surface = &editor_scene.objects[target];
                EditMesh* mesh = editor_writable_mesh(surface);
                int material = edit_mesh_material(mesh, path);
                bool to_selection = mesh->faces[face].selected != 0;

                for (int f = 0; f < mesh->face_count; f++)
                {
                    if (f == face || (to_selection && mesh->faces[f].selected))
                    {
                        mesh->faces[f].style.material = material;
                    }
                }

                edit_mesh_touch(mesh);
                SDL_snprintf(editor_active_material, sizeof(editor_active_material), "%s", path);
                editor_set_status("Material : %s", file_name);
            }
            else if (target >= 0)
            {
                editor_begin_edit();
                editor_scene.objects[target].override_material = 1;
                SDL_snprintf(editor_scene.objects[target].albedo_texture, SCENE_PATH_SIZE, "%s", path);
                editor_select_object(target, SELECT_REPLACE);
            }

            break;
        }

        object = editor_add_sprite(path);

        if (object)
        {
            object->position[0] = hit.x;
            object->position[1] = editor_view_2d ? hit.y : object->position[1];
        }

        break;

    case ASSET_AUDIO:
        object = editor_add_object(file_name);

        if (object)
        {
            SDL_snprintf(object->audio, sizeof(object->audio), "%s", path);
            object->audio_autoplay = 1;
            object->position[0] = hit.x;
            object->position[2] = hit.z;
        }

        break;

    case ASSET_SKYBOX:
        editor_begin_edit();
        SDL_snprintf(editor_scene.skybox, sizeof(editor_scene.skybox), "%s", path);
        editor_set_status("Skybox : %s", file_name);
        break;

    case ASSET_SCENE:
        editor_load_scene(path);
        break;

    default:
        break;
    }
}

static void draw_viewport(const CameraMatrices* matrices)
{
    ImGuiIO& io = ImGui::GetIO();

    // Full window, transparent, always behind the panels : hosts the gizmo and the 3D view's input.
    ImGui::SetNextWindowPos(ImVec2(0.0f, 0.0f));
    ImGui::SetNextWindowSize(io.DisplaySize);
    ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(0.0f, 0.0f));
    ImGui::Begin("##viewport", NULL,
        ImGuiWindowFlags_NoDecoration | ImGuiWindowFlags_NoBackground | ImGuiWindowFlags_NoBringToFrontOnFocus |
        ImGuiWindowFlags_NoMove | ImGuiWindowFlags_NoSavedSettings | ImGuiWindowFlags_NoDocking |
        ImGuiWindowFlags_NoNav | ImGuiWindowFlags_NoScrollWithMouse | ImGuiWindowFlags_NoFocusOnAppearing);
    ImGui::PopStyleVar();

    bool hovered = ImGui::IsWindowHovered();
    ImDrawList* draw_list = ImGui::GetWindowDrawList();

    // Assets dropped onto the free area of the window.
    ImRect drop_area(central_node_min, ImVec2(central_node_min.x + central_node_size.x, central_node_min.y + central_node_size.y));

    if (ImGui::BeginDragDropTargetCustom(drop_area, ImGui::GetID("##viewport_drop")))
    {
        handle_viewport_drop(matrices);
        ImGui::EndDragDropTarget();
    }

    if (playing)
    {
        // Click to give the game the mouse and keyboard ; Escape takes them back.
        if (hovered && !game_has_input && ImGui::IsMouseClicked(ImGuiMouseButton_Left))
        {
            game_has_input = true;
            input_set_mouse_captured(1);
        }

        if (game_has_input && ImGui::IsKeyPressed(ImGuiKey_Escape, false))
        {
            game_has_input = false;
            input_set_mouse_captured(0);
        }

        if (!game_has_input)
        {
            const char* hint = "Click to control the character  -  Escape to release";
            ImVec2 size = ImGui::CalcTextSize(hint);
            ImVec2 position(central_node_min.x + (central_node_size.x - size.x) * 0.5f, central_node_min.y + 12.0f);
            draw_list->AddRectFilled(ImVec2(position.x - 8, position.y - 4), ImVec2(position.x + size.x + 8, position.y + size.y + 4), IM_COL32(0, 0, 0, 160), 4.0f);
            draw_list->AddText(position, IM_COL32(255, 255, 255, 230), hint);
        }

        ImGui::End();
        return;
    }

    ImGuizmo::SetDrawlist(draw_list);
    ImGuizmo::SetRect(0.0f, 0.0f, io.DisplaySize.x, io.DisplaySize.y);
    ImGuizmo::SetOrthographic(matrices->orthographic);

    // Grid, selection, gizmos, tools : the level editor's. The camera gets the mouse when it's free.
    if (level_viewport(matrices, hovered) && camera_drag != CAMERA_DRAG_FLY)
    {
        viewport_camera_input(hovered);
    }
    else if (camera_drag == CAMERA_DRAG_FLY)
    {
        viewport_camera_input(hovered);
    }

    ImGui::End();
}

static void handle_shortcuts(void)
{
    // The level editor's keys are sampled every frame (positional ones track their previous state),
    // and act only when no text is typed, nothing's flying, and the game isn't playing.
    bool typing = ImGui::GetIO().WantTextInput;
    level_shortcuts(!typing && camera_drag != CAMERA_DRAG_FLY && !playing);

    if (typing)
    {
        return;
    }

    if (ImGui::IsKeyChordPressed(ImGuiMod_Ctrl | ImGuiKey_P))
    {
        if (playing)
        {
            editor_stop();
        }
        else
        {
            editor_play();
        }
    }

    // While flying, the positional keys move the camera ; in play mode they're the game's.
    if (camera_drag == CAMERA_DRAG_FLY || playing)
    {
        return;
    }

    if (ImGui::IsKeyChordPressed(ImGuiMod_Ctrl | ImGuiKey_Z))
    {
        editor_undo();
    }

    if (ImGui::IsKeyChordPressed(ImGuiMod_Ctrl | ImGuiKey_Y) || ImGui::IsKeyChordPressed(ImGuiMod_Ctrl | ImGuiMod_Shift | ImGuiKey_Z))
    {
        editor_redo();
    }

    if (ImGui::IsKeyChordPressed(ImGuiMod_Ctrl | ImGuiKey_S))
    {
        editor_save_scene();
    }

    if (ImGui::IsKeyChordPressed(ImGuiMod_Ctrl | ImGuiKey_D))
    {
        editor_duplicate_selected();
    }
}

// --- Play mode --------------------------------------------------------------------------------

bool editor_is_playing(void) { return playing; }
bool editor_is_paused(void) { return paused; }

void editor_play(void)
{
    if (playing)
    {
        return;
    }

    copy_scene(&play_snapshot, &editor_scene);
    edit_mesh_store_freeze_all(); // shared with the snapshot
    level_editor_reset();

    if (!scene_runtime_start(&editor_scene, &editor_project.settings))
    {
        editor_set_status("Could not start the simulation.");
        return;
    }

    playing = true;
    paused = false;
    game_has_input = false;
    camera_drag = CAMERA_DRAG_NONE;
    editor_set_status("Playing.");
}

void editor_stop(void)
{
    if (!playing)
    {
        return;
    }

    scene_runtime_stop();
    input_set_mouse_captured(0);

    playing = false;
    paused = false;
    game_has_input = false;

    copy_scene(&editor_scene, &play_snapshot);
    editor_set_status("Stopped.");
}

void editor_toggle_pause(void)
{
    paused = !paused;
}

void editor_step_once(void)
{
    step_once = true;
}

static void update_play_mode(void)
{
    if (!playing)
    {
        return;
    }

    ImGuiIO& io = ImGui::GetIO();

    RuntimeInput input;
    SDL_zero(input);

    if (game_has_input)
    {
        float dx = 0.0f;
        float dy = 0.0f;
        input_get_mouse_delta(&dx, &dy);

        input.move_forward = (float)(input_is_key_down(KEY_PLAY_FORWARD) - input_is_key_down(KEY_PLAY_BACKWARD));
        input.move_right = (float)(input_is_key_down(KEY_PLAY_RIGHT) - input_is_key_down(KEY_PLAY_LEFT));
        input.look_x = dx * PLAY_LOOK_SENSITIVITY;
        input.look_y = dy * PLAY_LOOK_SENSITIVITY;
        input.jump = input_is_key_down(KEY_PLAY_JUMP);
        input.sprint = input_is_key_down(KEY_PLAY_SPRINT);
    }

    if (!paused)
    {
        scene_runtime_update(&input, io.DeltaTime);
    }
    else if (step_once)
    {
        scene_runtime_update(&input, 1.0f / (float)editor_project.settings.physics_rate);
    }

    step_once = false;
}

// --- Rendering --------------------------------------------------------------------------------

static CameraMatrices frame_matrices;

static const Environment* current_environment(float out_sun_direction[3], float out_sun_color[3])
{
    Scene* scene = &editor_scene;
    float desired_sun[3] = {
        SDL_cosf(scene->sun_pitch) * SDL_sinf(scene->sun_yaw),
        SDL_sinf(scene->sun_pitch),
        SDL_cosf(scene->sun_pitch) * SDL_cosf(scene->sun_yaw)
    };

    const Environment* environment = scene->skybox[0] ? asset_get_environment(scene->skybox) : NULL;

    if (!environment)
    {
        // The procedural sky follows the sun : recomputed only when it moved.
        if (!procedural_valid || SDL_memcmp(procedural_sun, desired_sun, sizeof(desired_sun)) != 0)
        {
            wait_renderer_idle();

            if (procedural_valid)
            {
                environment_destroy(&procedural_environment);
            }

            procedural_valid = environment_create_procedural(desired_sun, &procedural_environment) != 0;
            SDL_memcpy(procedural_sun, desired_sun, sizeof(desired_sun));
        }

        environment = procedural_valid ? &procedural_environment : NULL;
    }

    if (!environment)
    {
        return NULL;
    }

    if (environment->has_sun && (scene->use_environment_sun || !scene->skybox[0]))
    {
        SDL_memcpy(out_sun_direction, environment->sun_direction, sizeof(float) * 3);

        for (int c = 0; c < 3; c++)
        {
            out_sun_color[c] = environment->sun_color[c] * scene->sun_intensity;
        }
    }
    else
    {
        SDL_memcpy(out_sun_direction, desired_sun, sizeof(float) * 3);

        for (int c = 0; c < 3; c++)
        {
            out_sun_color[c] = environment->average_luminance * 12.0f * scene->sun_intensity;
        }
    }

    return environment;
}

static void material_of(const SceneObject* object, const PbrMaterial* source, PbrMaterial* out)
{
    *out = *source;

    if (!object->override_material)
    {
        return;
    }

    SDL_memcpy(out->base_color, object->base_color, sizeof(out->base_color));
    SDL_memcpy(out->emissive, object->emissive, sizeof(out->emissive));
    out->metallic = object->metallic;
    out->roughness = object->roughness;
    out->uv_scale[0] = object->uv_scale[0];
    out->uv_scale[1] = object->uv_scale[1];
    out->ignore_vertex_color = 1;
    out->base_color_texture = object->albedo_texture[0] ? asset_get_material_texture(object->albedo_texture, 1) : NULL;
    out->normal_texture = object->normal_texture[0] ? asset_get_material_texture(object->normal_texture, 0) : NULL;
    out->metallic_roughness_texture = object->metallic_roughness_texture[0] ? asset_get_material_texture(object->metallic_roughness_texture, 0) : NULL;
}

// An editable mesh's surface : its material's texture, rough and dielectric (or the object's material,
// if it overrides it, keeping the face's texture unless it has its own).
static void mesh_material(const SceneObject* object, const char* path, PbrMaterial* out)
{
    photoreal_default_material(out);
    out->ignore_vertex_color = 1;
    out->roughness = 0.85f;
    out->metallic = 0.0f;
    out->base_color_texture = asset_get_material_texture(path, 1);

    if (!object->override_material)
    {
        return;
    }

    SDL_memcpy(out->base_color, object->base_color, sizeof(out->base_color));
    SDL_memcpy(out->emissive, object->emissive, sizeof(out->emissive));
    out->metallic = object->metallic;
    out->roughness = object->roughness;

    if (object->albedo_texture[0])
    {
        out->base_color_texture = asset_get_material_texture(object->albedo_texture, 1);
    }

    out->normal_texture = object->normal_texture[0] ? asset_get_material_texture(object->normal_texture, 0) : NULL;
    out->metallic_roughness_texture = object->metallic_roughness_texture[0] ? asset_get_material_texture(object->metallic_roughness_texture, 0) : NULL;
}

static float frame_sun_direction[3];
static float frame_sun_color[3];
static const Environment* frame_environment;
static Mat4 frame_forward_light;
static float frame_forward_radius;

static Mat4 forward_light_matrix(Vec3 light_direction, float* out_radius)
{
    Vec3 lo = vec3(1e30f, 1e30f, 1e30f);
    Vec3 hi = vec3(-1e30f, -1e30f, -1e30f);

    for (int i = 0; i < editor_scene.count; i++)
    {
        const SceneObject* object = &editor_scene.objects[i];
        float bounds[6];

        if ((!object->model[0] && !object->mesh) || hidden_now(object) || !editor_object_bounds(object, bounds))
        {
            continue;
        }

        Mat4 matrix = scene_object_matrix(object);

        for (int c = 0; c < 8; c++)
        {
            float world[4];
            mat4_transform(matrix, bounds[(c & 1) ? 3 : 0], bounds[(c & 2) ? 4 : 1], bounds[(c & 4) ? 5 : 2], 1.0f, world);

            lo = vec3(SDL_min(lo.x, world[0]), SDL_min(lo.y, world[1]), SDL_min(lo.z, world[2]));
            hi = vec3(SDL_max(hi.x, world[0]), SDL_max(hi.y, world[1]), SDL_max(hi.z, world[2]));
        }
    }

    if (lo.x > hi.x)
    {
        lo = vec3(-1.0f, -1.0f, -1.0f);
        hi = vec3(1.0f, 1.0f, 1.0f);
    }

    Vec3 center = vec3_scale(vec3_add(lo, hi), 0.5f);
    float radius = SDL_max(1.0f, vec3_length(vec3_sub(hi, lo)) * 0.5f);
    *out_radius = radius;

    Vec3 eye = vec3_sub(center, vec3_scale(light_direction, radius * 2.0f));
    Vec3 up = SDL_fabsf(light_direction.y) > 0.99f ? vec3(0.0f, 0.0f, 1.0f) : vec3(0.0f, 1.0f, 0.0f);

    return mat4_multiply(mat4_orthographic(-radius, radius, -radius, radius, radius, radius * 3.0f), mat4_look_at(eye, center, up));
}

static bool use_photoreal(void)
{
    return photoreal_ready && editor_project.settings.render_path != RENDER_PATH_FORWARD;
}

// The parts of an editable mesh to draw : tool materials (nodraw, clip) only in the editor.
static bool draw_part(const EditMesh* mesh, const EditMeshGpuPart* part)
{
    return !playing || !asset_is_tool_material(mesh->materials[part->material]);
}

static void draw_forward_meshes(void)
{
    for (int i = 0; i < editor_scene.count; i++)
    {
        const SceneObject* object = &editor_scene.objects[i];

        if (hidden_now(object))
        {
            continue;
        }

        if (object->mesh)
        {
            const EditMeshGpu* gpu = edit_mesh_gpu_get(object->mesh);
            const EditMesh* mesh = edit_mesh_store_get(object->mesh);

            if (gpu && gpu->mesh && mesh)
            {
                Mat4 matrix = scene_object_matrix(object);

                for (int p = 0; p < gpu->part_count; p++)
                {
                    if (draw_part(mesh, &gpu->parts[p]))
                    {
                        draw_mesh_range(gpu->mesh, matrix.m, gpu->parts[p].first_index, gpu->parts[p].index_count);
                    }
                }
            }

            continue;
        }

        const LoadedModel* model = object->model[0] ? asset_get_model(object->model) : NULL;

        if (model)
        {
            Mat4 matrix = scene_object_matrix(object);

            if (object->override_material)
            {
                draw_mesh_colored(model->mesh, matrix.m, object->base_color);
            }
            else
            {
                draw_mesh(model->mesh, matrix.m);
            }
        }
    }
}

static void prepare_rendering(void)
{
    frame_environment = current_environment(frame_sun_direction, frame_sun_color);

    // Editable meshes changed this frame get their GPU meshes now, before the frame starts.
    for (int i = 0; i < editor_scene.count; i++)
    {
        if (editor_scene.objects[i].mesh && !hidden_now(&editor_scene.objects[i]))
        {
            edit_mesh_gpu_get(editor_scene.objects[i].mesh);
        }
    }

    if (use_photoreal() || !frame_environment)
    {
        return;
    }

    // The forward renderer's shadow map is its own synchronous pass, before the frame.
    Vec3 to_sun = vec3_normalize(vec3(frame_sun_direction[0], frame_sun_direction[1], frame_sun_direction[2]));
    frame_forward_light = forward_light_matrix(vec3_scale(to_sun, -1.0f), &frame_forward_radius);

    if (begin_shadow_pass(forward_shadow_map, frame_forward_light.m))
    {
        draw_forward_meshes();
        end_shadow_pass();
    }
}

static void render_forward(int width, int height)
{
    (void)width;
    (void)height;

    if (!frame_environment)
    {
        return;
    }

    // In this path, light values are display-referred : scaled by the same exposure as the other one.
    float exposure = 0.22f / SDL_max(frame_environment->average_luminance, 1e-3f) * SDL_powf(2.0f, editor_project.settings.look.exposure_ev);

    SceneLighting lighting;
    SDL_memcpy(lighting.view_projection, frame_matrices.view_projection.m, sizeof(lighting.view_projection));
    SDL_memcpy(lighting.light_view_projection, frame_forward_light.m, sizeof(lighting.light_view_projection));

    for (int c = 0; c < 3; c++)
    {
        lighting.light_direction[c] = -frame_sun_direction[c];
        lighting.light_color[c] = SDL_min(1.0f, frame_sun_color[c] * exposure / SDL_PI_F);
        float dc = frame_environment->sh[0][c] * 0.282095f;
        float up = frame_environment->sh[1][c] * 0.488603f;
        lighting.ambient_color[c] = SDL_min(1.0f, SDL_max(0.0f, dc + up) * exposure);
        lighting.ground_color[c] = SDL_min(1.0f, SDL_max(0.0f, dc - up) * exposure);
    }

    lighting.shadow_depth_bias = 0.001f;
    lighting.shadow_normal_offset = (2.0f * frame_forward_radius) / (float)SHADOW_MAP_SIZE * 3.0f;

    begin_scene(&lighting, forward_shadow_map);
    draw_forward_meshes();

    if (editor_scene.skybox[0] || !editor_view_2d)
    {
        // No tone mapping here : this puts the sky's mid-gray where the other path's display encoding would.
        draw_skybox(frame_environment->panorama, exposure * 2.27f);
    }

    end_scene();
}

static void render_photoreal(int width, int height)
{
    if (!frame_environment)
    {
        return;
    }

    photoreal_set_environment(frame_environment, frame_sun_direction, frame_sun_color);

    PhotorealCamera photoreal_camera;
    photoreal_camera.view = frame_matrices.view;
    photoreal_camera.projection = frame_matrices.projection;
    photoreal_camera.eye = frame_matrices.eye;
    photoreal_camera.near_plane = frame_matrices.near_plane;
    photoreal_camera.far_plane = frame_matrices.far_plane;

    photoreal_begin(&photoreal_camera);

    for (int i = 0; i < editor_scene.count; i++)
    {
        const SceneObject* object = &editor_scene.objects[i];

        if (hidden_now(object))
        {
            continue;
        }

        if (object->mesh)
        {
            const EditMeshGpu* gpu = edit_mesh_gpu_get(object->mesh);
            const EditMesh* mesh = edit_mesh_store_get(object->mesh);

            if (gpu && gpu->mesh && mesh)
            {
                Mat4 matrix = scene_object_matrix(object);

                for (int p = 0; p < gpu->part_count; p++)
                {
                    if (draw_part(mesh, &gpu->parts[p]))
                    {
                        PbrMaterial material;
                        mesh_material(object, mesh->materials[gpu->parts[p].material], &material);
                        photoreal_add(gpu->mesh, gpu->parts[p].first_index, gpu->parts[p].index_count, matrix.m, gpu->bounds, &material);
                    }
                }
            }

            continue;
        }

        const LoadedModel* model = object->model[0] ? asset_get_model(object->model) : NULL;

        if (!model)
        {
            continue;
        }

        Mat4 matrix = scene_object_matrix(object);

        for (int s = 0; s < model->data.submesh_count; s++)
        {
            const ModelSubmesh* submesh = &model->data.submeshes[s];
            PbrMaterial material;
            material_of(object, &model->materials[submesh->material], &material);
            photoreal_add(model->mesh, submesh->first_index, submesh->index_count, matrix.m, model->bounds, &material);
        }
    }

    photoreal_render(width, height);
    photoreal_present();
}

// Sprites and 2D shapes : textured quads, painter's order from far to near.
static void render_sprites(int width, int height)
{
    static int order[SCENE_MAX_OBJECTS];
    int count = 0;

    for (int i = 0; i < editor_scene.count; i++)
    {
        const SceneObject* object = &editor_scene.objects[i];

        if (!object->model[0] && !object->mesh && !hidden_now(object) && (object->sprite[0] || object->is_2d))
        {
            order[count++] = i;
        }
    }

    for (int i = 1; i < count; i++)
    {
        int current = order[i];
        int j = i - 1;

        while (j >= 0 && editor_scene.objects[order[j]].position[2] > editor_scene.objects[current].position[2])
        {
            order[j + 1] = order[j];
            j--;
        }

        order[j + 1] = current;
    }

    for (int n = 0; n < count; n++)
    {
        const SceneObject* object = &editor_scene.objects[order[n]];
        Mat4 matrix = mat4_multiply(frame_matrices.view_projection, scene_object_matrix(object));

        int texture_width = 1;
        int texture_height = 1;
        Texture* texture = object->sprite[0] ? asset_get_sprite(object->sprite, &texture_width, &texture_height) : white_texture();

        if (!texture)
        {
            continue;
        }

        float u0 = 0.0f, v0 = 0.0f, u1 = 1.0f, v1 = 1.0f;

        if (object->sprite_region[2] > 0.0f && object->sprite_region[3] > 0.0f)
        {
            u0 = object->sprite_region[0] / (float)texture_width;
            v0 = object->sprite_region[1] / (float)texture_height;
            u1 = (object->sprite_region[0] + object->sprite_region[2]) / (float)texture_width;
            v1 = (object->sprite_region[1] + object->sprite_region[3]) / (float)texture_height;
        }

        float hw = object->sprite_size[0] * 0.5f;
        float hh = object->sprite_size[1] * 0.5f;
        const float corners[4][2] = { { -hw, hh }, { hw, hh }, { hw, -hh }, { -hw, -hh } };
        const float uvs[4][2] = { { u0, v0 }, { u1, v0 }, { u1, v1 }, { u0, v1 } };
        const float* color = object->base_color;

        Vertex quad[4];
        bool visible = true;

        for (int c = 0; c < 4; c++)
        {
            float clip[4];
            mat4_transform(matrix, corners[c][0], corners[c][1], 0.0f, 1.0f, clip);

            if (clip[3] <= 0.001f)
            {
                visible = false;
                break;
            }

            quad[c].x = (clip[0] / clip[3] * 0.5f + 0.5f) * (float)width;
            quad[c].y = (0.5f - clip[1] / clip[3] * 0.5f) * (float)height;
            quad[c].u = uvs[c][0];
            quad[c].v = uvs[c][1];
            quad[c].r = color[0];
            quad[c].g = color[1];
            quad[c].b = color[2];
            quad[c].a = color[3];
        }

        if (visible)
        {
            const Vertex triangles[6] = { quad[0], quad[1], quad[2], quad[0], quad[2], quad[3] };
            draw_vertices(triangles, 6, texture);
        }
    }
}

// --- Example interface ------------------------------------------------------------------------

// Frees the meshes no copy of the scene refers to any more : not the current one, nor an undo / redo
// step, nor the play mode's snapshot. Only once the store has grown enough to be worth it.
static void collect_meshes(void)
{
    static int collected_count;

    if (edit_mesh_store_count() < collected_count * 2 + 32)
    {
        return;
    }

    int capacity = editor_scene.count + frame_start_scene.count + play_snapshot.count;

    for (int i = 0; i < undo_stack.count; i++)
    {
        capacity += undo_stack.steps[i].count;
    }

    for (int i = 0; i < redo_stack.count; i++)
    {
        capacity += redo_stack.steps[i].count;
    }

    uint32_t* live = (uint32_t*)SDL_malloc(sizeof(uint32_t) * (size_t)SDL_max(1, capacity));
    int count = 0;

    const SceneObject* lists[3] = { editor_scene.objects, frame_start_scene.objects, play_snapshot.objects };
    const int counts[3] = { editor_scene.count, frame_start_scene.count, playing ? play_snapshot.count : 0 };

    for (int l = 0; l < 3; l++)
    {
        for (int i = 0; i < counts[l]; i++)
        {
            if (lists[l][i].mesh)
            {
                live[count++] = lists[l][i].mesh;
            }
        }
    }

    const SnapshotStack* stacks[2] = { &undo_stack, &redo_stack };

    for (int k = 0; k < 2; k++)
    {
        for (int step = 0; step < stacks[k]->count; step++)
        {
            for (int i = 0; i < stacks[k]->steps[step].count; i++)
            {
                if (stacks[k]->steps[step].objects[i].mesh)
                {
                    live[count++] = stacks[k]->steps[step].objects[i].mesh;
                }
            }
        }
    }

    edit_mesh_store_collect(live, count);
    SDL_free(live);

    collected_count = edit_mesh_store_count();
}

const CameraMatrices* editor_camera(void)
{
    return &frame_matrices;
}

static void update(void)
{
    frame_number++;
    edit_mesh_gpu_end_frame();

    ImGuizmo::BeginFrame();

    editor_fix_selection();
    copy_scene(&frame_start_scene, &editor_scene);

    editor_draw_main_menu();

    // The dockspace FIRST, then the viewport window : both are NoBringToFrontOnFocus windows, which
    // ImGui inserts at the very BACK of the window stack when they're created. Created second, the
    // viewport ends up behind the dockspace's host, as it must : the host's passthru central node is
    // a hit-test hole letting the mouse through to the viewport, except while a panel is being dragged,
    // when the host itself must be hovered to offer its docking targets (window edges included).
    ImGuiID dockspace_id = ImGui::DockSpaceOverViewport(0, NULL, ImGuiDockNodeFlags_PassthruCentralNode);
    ImGuiDockNode* central_node = ImGui::DockBuilderGetCentralNode(dockspace_id);

    if (central_node)
    {
        central_node_min = central_node->Pos;
        central_node_size = central_node->Size;
    }

    update_play_mode();

    CameraMatrices matrices = camera_matrices();

    frame_matrices = matrices;

    draw_viewport(&matrices);
    editor_draw_primitives_panel();
    editor_draw_level_panel();
    editor_draw_view_panel();
    editor_draw_hierarchy();
    editor_draw_inspector();
    editor_draw_materials_panel();
    editor_draw_asset_browser();
    editor_draw_2d_views();
    editor_draw_project_settings();
    handle_shortcuts();

    // Matrices again : the camera may have moved while handling this frame's input.
    frame_matrices = camera_matrices();

    prepare_rendering();
    collect_meshes();
}

static void render(void)
{
    int width = 0;
    int height = 0;
    get_window_size(&width, &height);

    if (use_photoreal())
    {
        render_photoreal(width, height);
    }
    else
    {
        render_forward(width, height);
    }

    render_sprites(width, height);
}

static int init(void)
{
    if (!requested_project[0])
    {
        printf("The editor needs a project : open or create one from the menu.\n");
        return 0;
    }

    if (!project_open(requested_project, &editor_project))
    {
        return 0;
    }

    asset_database_open(&editor_project);
    audio_init();

    photoreal_ready = photoreal_create() != 0;
    forward_shadow_map = create_shadow_map(SHADOW_MAP_SIZE);

    editor_view_2d = editor_project.template_kind == PROJECT_TEMPLATE_2D;
    camera.target = vec3(0.0f, editor_view_2d ? 0.0f : 0.5f, 0.0f);
    camera.yaw = 0.75f;
    camera.pitch = 0.45f;
    camera.distance = 12.0f;
    camera_drag = CAMERA_DRAG_NONE;

    editor_apply_settings();
    editor_asset_browser_init();
    level_editor_init();

    // The project's startup scene if it exists, a new one otherwise.
    SDL_snprintf(editor_scene_path, sizeof(editor_scene_path), "%s", editor_project.settings.startup_scene);

    char path[PROJECT_PATH_SIZE + ASSET_PATH_SIZE];
    asset_absolute_path(editor_scene_path, path, sizeof(path));

    if (scene_load(&editor_scene, path))
    {
        editor_set_status("Loaded '%s'.", editor_scene_path);
        editor_dirty = 0;
    }
    else
    {
        create_default_scene(&editor_scene, editor_project.template_kind);
        editor_save_scene();
    }

    char title[128];
    SDL_snprintf(title, sizeof(title), "Pireneus Editor - %s", editor_project.name);
    set_window_title(title);

    return 1;
}

static void shutdown_example(void)
{
    if (playing)
    {
        editor_stop();
    }

    wait_renderer_idle();

    stack_clear(&undo_stack);
    stack_clear(&redo_stack);

    edit_mesh_gpu_clear();
    edit_mesh_store_clear();
    editor_scene.count = 0;
    frame_start_scene.count = 0;
    play_snapshot.count = 0;

    editor_asset_browser_shutdown();

    if (procedural_valid)
    {
        environment_destroy(&procedural_environment);
        procedural_valid = false;
    }

    photoreal_destroy();
    photoreal_ready = false;

    destroy_shadow_map(forward_shadow_map);
    forward_shadow_map = NULL;

    audio_shutdown();
    level_editor_free_caches();
    asset_database_close();
}

extern "C" const Example example_scene_editor = {
    "scene-editor", "Editor",
    "Build scenes from your assets, with physics and a play mode.",
    init, update, render, shutdown_example, 1
};
