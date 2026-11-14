# Physics and play mode

## Physics (`src/physics`)

Two engines simulate the scene. Both are built as static libraries from their sources:

- [Box3D](https://github.com/erincatto/box3d) (v0.1.0, still alpha) for 3D objects;
- [Box2D](https://github.com/erincatto/box2d) (v3.2.0) for 2D objects, in the XY plane.

```c
PhysicsWorld* world = physics_create(&scene, &project.settings);
physics_step(world, &input, 1.0f / rate);      // at a fixed rate
physics_update_scene(world, alpha);            // interpolated transforms, written back to the scene
physics_destroy(world);
```

- **Bodies**: one body is created per object that has a body (static, kinematic, dynamic) or a
  character controller.
- **Collider**: chosen per object (box, sphere or circle, capsule, convex hull, triangles). In
  automatic mode, it comes from the model, the editable mesh or the sprite:
  - a static editable mesh gets an exact collider (its triangles);
  - a dynamic one gets its convex hull.
- **Layers**: collisions are filtered by the project's layers and their matrix.

### Character controller

The character is a real dynamic body, a capsule with locked rotation, not a kinematic shape moved by
hand. It falls, gets pushed by what hits it, pushes what it walks into according to their masses, and
rides moving platforms.

- **Ground**: zero friction, and the ground is detected with a shape cast.
- **Walking** applies impulses with a bounded change of velocity.
- **Slopes**: gravity is cancelled on walkable slopes, so the character doesn't slide.
- **Standing on objects**: the character pushes back on the dynamic objects it stands on.
- **Jumping**: a cooldown separates two jumps.

## Play mode (editor)

- **Starting**: **Ctrl+P** or the Play button. `editor_play()` copies the scene and starts
  `scene_runtime_start`.
- **While playing**: Pause and Step are available, and the menu bar is tinted.
- **Controls**: click in the view to take control of the character; Escape releases it.
- **Stopping**: Stop (`editor_stop()`) restores the copy, so nothing that moved in game stays in the
  scene.
- **Tool materials**: `tool_nodraw` and `tool_clip` are invisible in game, but keep their collision.
