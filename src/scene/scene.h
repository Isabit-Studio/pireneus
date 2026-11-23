#ifndef _SCENE_H_
#define _SCENE_H_

#include <stdint.h>

#include "math/math3d.h"

#ifdef __cplusplus
extern "C" {
#endif

// A game scene : plain data, saved as JSON. Objects carry a transform and optional "components" (a
// model, an editable mesh or a sprite, a material, a light, a rigid body, a character controller, a sound), all
// stored flat in one struct, so that a whole scene can be copied with `=` for undo and for the play mode's
// snapshot. Editable meshes are the exception : they're referred to by handle (see meshedit/edit_mesh.h).

#define SCENE_MAX_OBJECTS 1024
#define SCENE_PATH_SIZE 192

// A model built into the engine rather than imported into the project.
#define SCENE_BUILTIN_PREFIX "builtin/"

typedef enum
{
    SCENE_BODY_NONE,
    SCENE_BODY_STATIC,
    SCENE_BODY_KINEMATIC,
    SCENE_BODY_DYNAMIC
} SceneBodyType;

typedef enum
{
    SCENE_COLLIDER_AUTO,    // chosen from the model : box, sphere, or its convex hull / triangles
    SCENE_COLLIDER_BOX,
    SCENE_COLLIDER_SPHERE,  // 3D : sphere, 2D : circle
    SCENE_COLLIDER_CAPSULE,
    SCENE_COLLIDER_CONVEX,  // 3D only : the model's convex hull
    SCENE_COLLIDER_MESH     // 3D only, static bodies : the model's triangles
} SceneColliderShape;

typedef enum
{
    SCENE_LIGHT_NONE,
    SCENE_LIGHT_POINT,
    SCENE_LIGHT_SPOT
} SceneLightType;

typedef enum
{
    SCENE_LIGHT_REALTIME,   // lit and shadowed every frame, nothing baked
    SCENE_LIGHT_MIXED,      // its direct light realtime, its bounces baked
    SCENE_LIGHT_BAKED       // all baked : free at runtime, but static
} SceneLightMode;

typedef struct
{
    char name[64];
    float position[3];
    float rotation[3];      // Euler angles in degrees : R = Rz * Ry * Rx
    float scale[3];

    // What's drawn : a model, an editable mesh (the level editor's geometry), or a sprite (a textured
    // quad in the XY plane).
    char model[SCENE_PATH_SIZE];
    uint32_t mesh;          // edit_mesh_store_get() ; 0 = none
    char sprite[SCENE_PATH_SIZE];
    float sprite_size[2];   // in meters
    float sprite_region[4]; // x, y, width, height in pixels ; width = 0 : the whole image

    // Replaces the model's own materials, if set.
    int override_material;
    float base_color[4];
    float emissive[3];
    float metallic;
    float roughness;
    float uv_scale[2];
    char albedo_texture[SCENE_PATH_SIZE];
    char normal_texture[SCENE_PATH_SIZE];
    char metallic_roughness_texture[SCENE_PATH_SIZE];

    // Light : a point or spot light (the sun is the scene's). A spot points along the object's -Y.
    int light_type;          // SceneLightType
    float light_color[3];
    float light_intensity;   // the irradiance it gives at 1 m, in the sun's units
    float light_range;       // meters
    float light_spot_angle;  // degrees : the cone's full angle
    float light_spot_softness; // 0 to 1
    int light_shadows;
    int light_mode;          // SceneLightMode

    // Reflection probe : a capture of the scene from here, baked, reflected by what's inside its box.
    int reflection_probe;
    float probe_box[3];      // the box's size, meters, centered on the object (not rotated)
    float probe_blend;       // meters : the fade inside its walls
    int probe_resolution;    // a capture face's size, texels

    // Physics
    int body_type;          // SceneBodyType
    int collider;           // SceneColliderShape
    int is_2d;              // Box2D rather than Box3D : simulated in the XY plane
    float density;
    float friction;
    float restitution;
    int layer;              // collision layer, see ProjectSettings

    int is_character;       // a character controller, driven by the player in play mode

    // Sound
    char audio[SCENE_PATH_SIZE];
    float audio_volume;
    int audio_loop;
    int audio_autoplay;
    int audio_is_music;
    int audio_3d;

    // The editor's : hidden in its views (not in the game), part of its selection (not saved).
    int hidden;
    int selected;
} SceneObject;

// The sky's procedural clouds (see renderer/render3d) : a drifting layer, and its shadows on the ground.
typedef struct
{
    float coverage;         // 0 = clear, to 1 = overcast
    float density;
    float altitude;         // meters
    float scale;            // meters : the size of their features
    float wind[2];          // m/s, along X and Z
    float shadows;          // 0 to 1 : how much they shade the ground
} SceneClouds;

typedef struct
{
    SceneObject objects[SCENE_MAX_OBJECTS];
    int count;
    int selected;           // index into objects, -1 = none

    char skybox[SCENE_PATH_SIZE]; // an HDR panorama ; "" : the procedural sky
    float sun_yaw;          // radians, around +Y
    float sun_pitch;        // radians, above the horizon
    float sun_intensity;    // relative to the sky's own sun
    int use_environment_sun; // take the sun's direction and color from the skybox, if it has one
    SceneClouds clouds;

    // Where the editor's camera was when the scene was saved (an orbit around `view_target`), so it
    // reopens there. has_view = 0 : the editor's default view.
    int has_view;
    float view_target[3];
    float view_yaw;         // radians, around +Y
    float view_pitch;       // radians, above the horizon
    float view_distance;
} Scene;

void scene_init(Scene* scene);
void scene_object_init(SceneObject* object, const char* name);

// The object's world matrix, row-major.
Mat4 scene_object_matrix(const SceneObject* object);

// Back from a rotation matrix (row-major 3x3 in the upper-left, unit columns) to Euler angles.
void scene_euler_from_rotation(const float rotation[9], float out_degrees[3]);
void scene_rotation_from_euler(const float degrees[3], float out_rotation[9]);

// Loading adds the scene's editable meshes to the mesh store : the previous scene's stay there until
// collected (edit_mesh_store_collect()).
int scene_save(const Scene* scene, const char* path); // absolute path ; 0 on failure
int scene_load(Scene* scene, const char* path);       // 0 on failure

#ifdef __cplusplus
}
#endif

#endif // _SCENE_H_
