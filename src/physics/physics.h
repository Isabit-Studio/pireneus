#ifndef _PHYSICS_H_
#define _PHYSICS_H_

#include "project/project.h"
#include "scene/scene.h"

#ifdef __cplusplus
extern "C" {
#endif

// Rigid body simulation of a scene : Box3D for 3D objects, Box2D for 2D ones (in the XY plane).
//
// The character controller is a real dynamic body (a capsule whose rotation is locked), not a kinematic
// shape moved by hand : it falls, is pushed by what hits it, pushes what it walks into according to
// their masses, and rides moving platforms. Walking only applies impulses on top of that.

typedef struct PhysicsWorld PhysicsWorld;

typedef struct
{
    float move[3];  // desired direction in world space, length <= 1 (2D : only X is used)
    int jump;       // held
    int sprint;     // held
} CharacterInput;

// Builds bodies from the scene's objects. The world keeps pointers to `scene` and `settings`.
PhysicsWorld* physics_create(Scene* scene, const ProjectSettings* settings);
void physics_destroy(PhysicsWorld* world);

void physics_step(PhysicsWorld* world, const CharacterInput* input, float time_step);

// Writes objects' transforms back into the scene, between the last two steps (alpha in [0, 1]) : rendering
// stays smooth whatever the difference between the frame rate and the simulation's fixed rate.
void physics_update_scene(PhysicsWorld* world, float alpha);

int physics_character_object(const PhysicsWorld* world); // index in the scene, -1 if none
int physics_character_grounded(const PhysicsWorld* world);

#ifdef __cplusplus
}
#endif

#endif // _PHYSICS_H_
