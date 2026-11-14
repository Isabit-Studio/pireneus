#ifndef _SCENE_RUNTIME_H_
#define _SCENE_RUNTIME_H_

#include "math/math3d.h"
#include "project/project.h"
#include "scene/scene.h"

#ifdef __cplusplus
extern "C" {
#endif

// A scene being played : physics stepped at a fixed rate, sounds, the player's character. Used by the
// editor's play mode, and by anything that runs a game. It modifies `scene` as objects move : the caller
// plays on a copy if it wants the original back.

typedef struct
{
    float move_forward;  // -1 to 1
    float move_right;    // -1 to 1
    float look_x;        // radians this frame
    float look_y;
    int jump;
    int sprint;
} RuntimeInput;

typedef struct
{
    Vec3 eye;
    Vec3 target;
    int orthographic;    // 2D scenes
    float ortho_half_height;
} RuntimeCamera;

int scene_runtime_start(Scene* scene, const ProjectSettings* settings);
void scene_runtime_stop(void);

void scene_runtime_update(const RuntimeInput* input, float delta_time);

// Whether a character drives the camera, and where. 0 if there's no character.
int scene_runtime_camera(RuntimeCamera* out_camera);
int scene_runtime_character_grounded(void);

#ifdef __cplusplus
}
#endif

#endif // _SCENE_RUNTIME_H_
