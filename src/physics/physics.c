// Box3D (3D) and Box2D (2D) behind physics.h : one body per scene object that has a rigid body or a
// character controller, with a collider resolved from its model, editable mesh or sprite, filtered
// by the project's collision layers. The character controller logic is at the end of this file.

#include "physics/physics.h"

#include <SDL3/SDL.h>
#include <stdio.h>

#include "box2d/box2d.h"
#include "box3d/box3d.h"

#include "assets/asset_database.h"
#include "meshedit/edit_mesh.h"
#include "debug.h"

#define MAX_HULL_VERTICES 64
#define JUMP_COOLDOWN 0.2f // seconds during which a jumping character doesn't count as grounded

typedef struct
{
    int object;      // index in the scene
    int is_2d;
    b3BodyId body3;
    b2BodyId body2;
    float previous_position[3];
    float current_position[3];
    float previous_rotation[4]; // quaternions (x, y, z, w) ; 2D : only z and w
    float current_rotation[4];
} PhysicsBody;

struct PhysicsWorld
{
    Scene* scene;
    const ProjectSettings* settings;

    int has_3d;
    int has_2d;
    b3WorldId world3;
    b2WorldId world2;

    PhysicsBody* bodies;
    int body_count;

    b3MeshData** meshes;  // must outlive their shapes
    int mesh_count;

    int character;        // index in bodies, -1 if none
    float character_radius;
    float character_half_segment;
    float character_center[3]; // of the capsule, relative to the body
    float character_mass;
    int character_grounded;
    float jump_cooldown;
};

// --- Rotations ----------------------------------------------------------------------------------

static void quaternion_from_matrix(const float r[9], float q[4])
{
    float trace = r[0] + r[4] + r[8];

    if (trace > 0.0f)
    {
        float s = SDL_sqrtf(trace + 1.0f) * 2.0f;
        q[3] = 0.25f * s;
        q[0] = (r[7] - r[5]) / s;
        q[1] = (r[2] - r[6]) / s;
        q[2] = (r[3] - r[1]) / s;
    }
    else if (r[0] > r[4] && r[0] > r[8])
    {
        float s = SDL_sqrtf(1.0f + r[0] - r[4] - r[8]) * 2.0f;
        q[3] = (r[7] - r[5]) / s;
        q[0] = 0.25f * s;
        q[1] = (r[1] + r[3]) / s;
        q[2] = (r[2] + r[6]) / s;
    }
    else if (r[4] > r[8])
    {
        float s = SDL_sqrtf(1.0f + r[4] - r[0] - r[8]) * 2.0f;
        q[3] = (r[2] - r[6]) / s;
        q[0] = (r[1] + r[3]) / s;
        q[1] = 0.25f * s;
        q[2] = (r[5] + r[7]) / s;
    }
    else
    {
        float s = SDL_sqrtf(1.0f + r[8] - r[0] - r[4]) * 2.0f;
        q[3] = (r[3] - r[1]) / s;
        q[0] = (r[2] + r[6]) / s;
        q[1] = (r[5] + r[7]) / s;
        q[2] = 0.25f * s;
    }
}

static void matrix_from_quaternion(const float q[4], float r[9])
{
    float x = q[0], y = q[1], z = q[2], w = q[3];

    r[0] = 1.0f - 2.0f * (y * y + z * z);
    r[1] = 2.0f * (x * y - z * w);
    r[2] = 2.0f * (x * z + y * w);
    r[3] = 2.0f * (x * y + z * w);
    r[4] = 1.0f - 2.0f * (x * x + z * z);
    r[5] = 2.0f * (y * z - x * w);
    r[6] = 2.0f * (x * z - y * w);
    r[7] = 2.0f * (y * z + x * w);
    r[8] = 1.0f - 2.0f * (x * x + y * y);
}

// --- Shapes -------------------------------------------------------------------------------------

typedef struct
{
    float half[3];   // half-extents of the object's bounds, scale applied
    float center[3]; // their center, relative to the object, scale applied
    const LoadedModel* model;
    const EditMesh* mesh;
} ObjectExtent;

static ObjectExtent object_extent(const SceneObject* object)
{
    ObjectExtent extent;
    SDL_zero(extent);

    float bounds[6] = { 0 };
    int has_bounds = 0;

    if (object->mesh)
    {
        extent.mesh = edit_mesh_store_get(object->mesh);

        if (extent.mesh && extent.mesh->face_count > 0)
        {
            edit_mesh_bounds(extent.mesh, bounds);
            has_bounds = 1;
        }
        else
        {
            extent.mesh = NULL;
        }
    }
    else if (object->model[0])
    {
        extent.model = asset_get_model(object->model);

        if (extent.model)
        {
            SDL_memcpy(bounds, extent.model->bounds, sizeof(bounds));
            has_bounds = 1;
        }
    }

    for (int axis = 0; axis < 3; axis++)
    {
        float scale = SDL_fabsf(object->scale[axis]);

        if (has_bounds)
        {
            extent.half[axis] = (bounds[axis + 3] - bounds[axis]) * 0.5f * scale;
            extent.center[axis] = (bounds[axis + 3] + bounds[axis]) * 0.5f * object->scale[axis];
        }
        else if ((object->sprite[0] || object->is_2d) && axis < 2)
        {
            extent.half[axis] = object->sprite_size[axis] * 0.5f * scale;
        }
        else
        {
            extent.half[axis] = 0.5f * scale;
        }
    }

    return extent;
}

// What COLLIDER_AUTO means for this object.
static int resolve_collider(const SceneObject* object)
{
    if (object->collider != SCENE_COLLIDER_AUTO)
    {
        return object->collider;
    }

    if (object->mesh)
    {
        // Level geometry : its exact faces when it doesn't move, its convex hull when it does.
        return object->body_type == SCENE_BODY_STATIC ? SCENE_COLLIDER_MESH : SCENE_COLLIDER_CONVEX;
    }

    if (SDL_strcmp(object->model, SCENE_BUILTIN_PREFIX "sphere") == 0 || SDL_strcmp(object->model, SCENE_BUILTIN_PREFIX "ico-sphere") == 0)
    {
        return SCENE_COLLIDER_SPHERE;
    }

    if (SDL_strcmp(object->model, SCENE_BUILTIN_PREFIX "cube") == 0 || SDL_strcmp(object->model, SCENE_BUILTIN_PREFIX "plane") == 0 || !object->model[0])
    {
        return SCENE_COLLIDER_BOX;
    }

    // Anything else : its triangles when it doesn't move, its convex hull when it does.
    return object->body_type == SCENE_BODY_STATIC ? SCENE_COLLIDER_MESH : SCENE_COLLIDER_CONVEX;
}

// The object's points, and its triangles if `out_indices` isn't NULL (SDL_free() both).
static int collision_geometry(const ObjectExtent* extent, b3Vec3** out_points, int32_t** out_indices, int* out_index_count)
{
    if (extent->mesh)
    {
        const EditMesh* mesh = extent->mesh;
        b3Vec3* points = SDL_malloc(sizeof(b3Vec3) * (size_t)SDL_max(1, mesh->vertex_count));

        for (int i = 0; i < mesh->vertex_count; i++)
        {
            const float* p = edit_mesh_position(mesh, i);
            points[i].x = p[0];
            points[i].y = p[1];
            points[i].z = p[2];
        }

        if (out_indices)
        {
            int capacity = 0;

            for (int f = 0; f < mesh->face_count; f++)
            {
                capacity += (mesh->faces[f].count - 2) * 3;
            }

            int32_t* indices = SDL_malloc(sizeof(int32_t) * (size_t)SDL_max(1, capacity));
            int* corners = SDL_malloc(sizeof(int) * 3 * (size_t)SDL_max(1, mesh->loop_count));
            int count = 0;

            for (int f = 0; f < mesh->face_count; f++)
            {
                int triangles = edit_mesh_triangulate_face(mesh, f, corners);

                for (int t = 0; t < triangles * 3; t++)
                {
                    indices[count++] = edit_mesh_face_vertex(mesh, f, corners[t]);
                }
            }

            SDL_free(corners);
            *out_indices = indices;
            *out_index_count = count;
        }

        *out_points = points;
        return mesh->vertex_count;
    }

    const ModelData* data = &extent->model->data;
    b3Vec3* points = SDL_malloc(sizeof(b3Vec3) * (size_t)SDL_max(1, data->vertex_count));

    for (int i = 0; i < data->vertex_count; i++)
    {
        points[i].x = data->vertices[i].x;
        points[i].y = data->vertices[i].y;
        points[i].z = data->vertices[i].z;
    }

    if (out_indices)
    {
        int32_t* indices = SDL_malloc(sizeof(int32_t) * (size_t)SDL_max(1, data->index_count));
        SDL_memcpy(indices, data->indices, sizeof(int32_t) * (size_t)data->index_count);
        *out_indices = indices;
        *out_index_count = data->index_count;
    }

    *out_points = points;
    return data->vertex_count;
}

static void add_shape_3d(PhysicsWorld* world, b3BodyId body, const SceneObject* object, const b3ShapeDef* def)
{
    ObjectExtent extent = object_extent(object);
    int collider = resolve_collider(object);
    b3Vec3 center = { extent.center[0], extent.center[1], extent.center[2] };

    if ((collider == SCENE_COLLIDER_CONVEX || collider == SCENE_COLLIDER_MESH) && !extent.model && !extent.mesh)
    {
        collider = SCENE_COLLIDER_BOX;
    }

    switch (collider)
    {
    case SCENE_COLLIDER_SPHERE:
    {
        b3Sphere sphere = { center, SDL_max(extent.half[0], SDL_max(extent.half[1], extent.half[2])) };
        b3CreateSphereShape(body, def, &sphere);
        break;
    }

    case SCENE_COLLIDER_CAPSULE:
    {
        float radius = SDL_max(0.01f, SDL_max(extent.half[0], extent.half[2]));
        float segment = SDL_max(0.0f, extent.half[1] - radius);
        b3Capsule capsule = { { center.x, center.y - segment, center.z }, { center.x, center.y + segment, center.z }, radius };
        b3CreateCapsuleShape(body, def, &capsule);
        break;
    }

    case SCENE_COLLIDER_CONVEX:
    {
        // Every vertex : the hull builder keeps the ones that matter, up to the limit.
        b3Vec3* points = NULL;
        int point_count = collision_geometry(&extent, &points, NULL, NULL);
        b3HullData* hull = b3CreateHull(points, point_count, MAX_HULL_VERTICES);
        SDL_free(points);

        if (hull)
        {
            b3Vec3 scale = { object->scale[0], object->scale[1], object->scale[2] };
            b3CreateTransformedHullShape(body, def, hull, b3Transform_identity, scale);
            b3DestroyHull(hull);
        }
        else
        {
            b3BoxHull box = b3MakeOffsetBoxHull(SDL_max(extent.half[0], 0.01f), SDL_max(extent.half[1], 0.01f), SDL_max(extent.half[2], 0.01f), center);
            b3CreateHullShape(body, def, &box.base);
        }

        break;
    }

    case SCENE_COLLIDER_MESH:
    {
        b3Vec3* vertices = NULL;
        int32_t* indices = NULL;
        int index_count = 0;
        int vertex_count = collision_geometry(&extent, &vertices, &indices, &index_count);

        b3MeshDef mesh_def;
        SDL_zero(mesh_def);
        mesh_def.vertices = vertices;
        mesh_def.indices = indices;
        mesh_def.vertexCount = vertex_count;
        mesh_def.triangleCount = index_count / 3;
        mesh_def.weldVertices = true;
        mesh_def.weldTolerance = 1e-4f;
        mesh_def.identifyEdges = true;

        b3MeshData* mesh = index_count >= 3 ? b3CreateMesh(&mesh_def, NULL, 0) : NULL;
        SDL_free(vertices);
        SDL_free(indices);

        if (mesh)
        {
            world->meshes = SDL_realloc(world->meshes, sizeof(b3MeshData*) * (size_t)(world->mesh_count + 1));
            world->meshes[world->mesh_count++] = mesh;

            b3Vec3 scale = { object->scale[0], object->scale[1], object->scale[2] };
            b3CreateMeshShape(body, def, mesh, scale);
        }

        break;
    }

    default:
    {
        // A box. Flat models (a plane) get a thickness, downward so their top stays where it's drawn.
        float hx = SDL_max(extent.half[0], 0.01f);
        float hy = extent.half[1];
        float hz = SDL_max(extent.half[2], 0.01f);

        if (hy < 0.05f)
        {
            center.y -= 0.05f - hy;
            hy = 0.05f;
        }

        b3BoxHull box = b3MakeOffsetBoxHull(hx, hy, hz, center);
        b3CreateHullShape(body, def, &box.base);
        break;
    }
    }
}

static void add_shape_2d(b2BodyId body, const SceneObject* object, const b2ShapeDef* def)
{
    ObjectExtent extent = object_extent(object);
    int collider = resolve_collider(object);
    b2Vec2 center = { extent.center[0], extent.center[1] };

    switch (collider)
    {
    case SCENE_COLLIDER_SPHERE:
    {
        b2Circle circle = { center, SDL_max(extent.half[0], extent.half[1]) };
        b2CreateCircleShape(body, def, &circle);
        break;
    }

    case SCENE_COLLIDER_CAPSULE:
    {
        float radius = SDL_max(0.01f, extent.half[0]);
        float segment = SDL_max(0.0f, extent.half[1] - radius);
        b2Capsule capsule = { { center.x, center.y - segment }, { center.x, center.y + segment }, radius };
        b2CreateCapsuleShape(body, def, &capsule);
        break;
    }

    default:
    {
        b2Polygon box = b2MakeOffsetBox(SDL_max(extent.half[0], 0.01f), SDL_max(extent.half[1], 0.01f), center, b2Rot_identity);
        b2CreatePolygonShape(body, def, &box);
        break;
    }
    }
}

static void read_body(PhysicsBody* body)
{
    if (body->is_2d)
    {
        b2Pos p = b2Body_GetPosition(body->body2);
        b2Rot q = b2Body_GetRotation(body->body2);
        float half_angle = b2Rot_GetAngle(q) * 0.5f;

        body->current_position[0] = p.x;
        body->current_position[1] = p.y;
        body->current_rotation[0] = 0.0f;
        body->current_rotation[1] = 0.0f;
        body->current_rotation[2] = SDL_sinf(half_angle);
        body->current_rotation[3] = SDL_cosf(half_angle);
    }
    else
    {
        b3Pos p = b3Body_GetPosition(body->body3);
        b3Quat q = b3Body_GetRotation(body->body3);

        body->current_position[0] = p.x;
        body->current_position[1] = p.y;
        body->current_position[2] = p.z;
        body->current_rotation[0] = q.v.x;
        body->current_rotation[1] = q.v.y;
        body->current_rotation[2] = q.v.z;
        body->current_rotation[3] = q.s;
    }
}

static void create_body(PhysicsWorld* world, int object_index)
{
    SceneObject* object = &world->scene->objects[object_index];
    const ProjectSettings* settings = world->settings;
    PhysicsBody* body = &world->bodies[world->body_count];

    SDL_zerop(body);
    body->object = object_index;
    body->is_2d = object->is_2d;
    SDL_memcpy(body->current_position, object->position, sizeof(body->current_position));

    int layer = SDL_clamp(object->layer, 0, PROJECT_MAX_LAYERS - 1);
    uint64_t category = 1ull << layer;
    uint64_t mask = settings->layer_masks[layer];

    int is_character = object->is_character;
    int type = is_character ? SCENE_BODY_DYNAMIC : object->body_type;

    float rotation[9];
    scene_rotation_from_euler(object->rotation, rotation);

    if (object->is_2d)
    {
        b2BodyDef def = b2DefaultBodyDef();
        def.type = type == SCENE_BODY_DYNAMIC ? b2_dynamicBody : type == SCENE_BODY_KINEMATIC ? b2_kinematicBody : b2_staticBody;
        def.position.x = object->position[0];
        def.position.y = object->position[1];
        def.rotation = b2MakeRot(object->rotation[2] * SDL_PI_F / 180.0f);
        def.motionLocks.angularZ = is_character != 0;
        def.enableSleep = !is_character;

        body->body2 = b2CreateBody(world->world2, &def);

        b2ShapeDef shape = b2DefaultShapeDef();
        shape.density = is_character ? 1.0f : object->density;
        shape.material.friction = is_character ? 0.0f : object->friction;
        shape.material.restitution = is_character ? 0.0f : object->restitution;
        shape.filter.categoryBits = category;
        shape.filter.maskBits = mask;

        if (is_character)
        {
            ObjectExtent extent = object_extent(object);
            float radius = SDL_max(0.05f, extent.half[0]);
            float segment = SDL_max(0.0f, extent.half[1] - radius);
            b2Capsule capsule = { { extent.center[0], extent.center[1] - segment }, { extent.center[0], extent.center[1] + segment }, radius };
            b2CreateCapsuleShape(body->body2, &shape, &capsule);

            world->character = world->body_count;
            world->character_radius = radius;
            world->character_half_segment = segment;
            SDL_memcpy(world->character_center, extent.center, sizeof(world->character_center));
            world->character_mass = b2Body_GetMass(body->body2);
        }
        else
        {
            add_shape_2d(body->body2, object, &shape);
        }
    }
    else
    {
        float q[4];
        quaternion_from_matrix(rotation, q);

        b3BodyDef def = b3DefaultBodyDef();
        def.type = type == SCENE_BODY_DYNAMIC ? b3_dynamicBody : type == SCENE_BODY_KINEMATIC ? b3_kinematicBody : b3_staticBody;
        def.position.x = object->position[0];
        def.position.y = object->position[1];
        def.position.z = object->position[2];
        def.rotation.v.x = q[0];
        def.rotation.v.y = q[1];
        def.rotation.v.z = q[2];
        def.rotation.s = q[3];
        def.motionLocks.angularX = is_character != 0;
        def.motionLocks.angularY = is_character != 0;
        def.motionLocks.angularZ = is_character != 0;
        def.enableSleep = !is_character;

        body->body3 = b3CreateBody(world->world3, &def);

        b3ShapeDef shape = b3DefaultShapeDef();
        shape.density = is_character ? 1.0f : object->density;
        shape.baseMaterial.friction = is_character ? 0.0f : object->friction;
        shape.baseMaterial.restitution = is_character ? 0.0f : object->restitution;
        shape.filter.categoryBits = category;
        shape.filter.maskBits = mask;

        if (is_character)
        {
            ObjectExtent extent = object_extent(object);
            float radius = SDL_max(0.05f, SDL_max(extent.half[0], extent.half[2]));
            float segment = SDL_max(0.0f, extent.half[1] - radius);

            b3Capsule capsule = {
                { extent.center[0], extent.center[1] - segment, extent.center[2] },
                { extent.center[0], extent.center[1] + segment, extent.center[2] },
                radius
            };

            // A character's mass comes from the project, not from its capsule's volume.
            float volume = SDL_PI_F * radius * radius * (segment * 2.0f + radius * 4.0f / 3.0f);
            shape.density = settings->character_mass / SDL_max(volume, 1e-4f);

            b3CreateCapsuleShape(body->body3, &shape, &capsule);

            world->character = world->body_count;
            world->character_radius = radius;
            world->character_half_segment = segment;
            SDL_memcpy(world->character_center, extent.center, sizeof(world->character_center));
            world->character_mass = b3Body_GetMass(body->body3);
        }
        else
        {
            add_shape_3d(world, body->body3, object, &shape);
        }
    }

    read_body(body);
    SDL_memcpy(body->previous_position, body->current_position, sizeof(body->previous_position));
    SDL_memcpy(body->previous_rotation, body->current_rotation, sizeof(body->previous_rotation));

    world->body_count++;
}

PhysicsWorld* physics_create(Scene* scene, const ProjectSettings* settings)
{
    PhysicsWorld* world = SDL_calloc(1, sizeof(PhysicsWorld));
    world->scene = scene;
    world->settings = settings;
    world->character = -1;
    world->bodies = SDL_malloc(sizeof(PhysicsBody) * (size_t)SDL_max(1, scene->count));

    for (int i = 0; i < scene->count; i++)
    {
        const SceneObject* object = &scene->objects[i];

        if (object->body_type != SCENE_BODY_NONE || object->is_character)
        {
            world->has_2d |= object->is_2d;
            world->has_3d |= !object->is_2d;
        }
    }

    if (world->has_3d)
    {
        b3WorldDef def = b3DefaultWorldDef();
        def.gravity.x = settings->gravity_3d[0];
        def.gravity.y = settings->gravity_3d[1];
        def.gravity.z = settings->gravity_3d[2];
        world->world3 = b3CreateWorld(&def);
    }

    if (world->has_2d)
    {
        b2WorldDef def = b2DefaultWorldDef();
        def.gravity.x = settings->gravity_2d[0];
        def.gravity.y = settings->gravity_2d[1];
        world->world2 = b2CreateWorld(&def);
    }

    for (int i = 0; i < scene->count; i++)
    {
        const SceneObject* object = &scene->objects[i];

        if (object->body_type != SCENE_BODY_NONE || object->is_character)
        {
            create_body(world, i);
        }
    }

    DEBUG_LOG("[physics] created : %d bodies\n", world->body_count);

    return world;
}

void physics_destroy(PhysicsWorld* world)
{
    if (!world)
    {
        return;
    }

    if (world->has_3d)
    {
        b3DestroyWorld(world->world3);
    }

    if (world->has_2d)
    {
        b2DestroyWorld(world->world2);
    }

    for (int i = 0; i < world->mesh_count; i++)
    {
        b3DestroyMesh(world->meshes[i]);
    }

    SDL_free(world->meshes);
    SDL_free(world->bodies);
    SDL_free(world);
}

// --- The character controller -------------------------------------------------------------------

typedef struct
{
    b3BodyId self;
    int hit;
    float fraction;
    b3Vec3 normal;
    b3Pos point;
    b3BodyId body;
} GroundProbe3;

static float ground_callback_3d(b3ShapeId shape, b3Pos point, b3Vec3 normal, float fraction, uint64_t material, int triangle, int child, void* context)
{
    (void)material;
    (void)triangle;
    (void)child;

    GroundProbe3* probe = context;
    b3BodyId body = b3Shape_GetBody(shape);

    if (B3_ID_EQUALS(body, probe->self))
    {
        return -1.0f;
    }

    if (!probe->hit || fraction < probe->fraction)
    {
        probe->hit = 1;
        probe->fraction = fraction;
        probe->normal = normal;
        probe->point = point;
        probe->body = body;
    }

    return fraction;
}

static float vector_length(b3Vec3 v)
{
    return SDL_sqrtf(v.x * v.x + v.y * v.y + v.z * v.z);
}

static float vector_dot(b3Vec3 a, b3Vec3 b)
{
    return a.x * b.x + a.y * b.y + a.z * b.z;
}

static b3Vec3 vector_add_scaled(b3Vec3 a, b3Vec3 b, float s)
{
    b3Vec3 r = { a.x + b.x * s, a.y + b.y * s, a.z + b.z * s };
    return r;
}

static b3Vec3 vector_clamp_length(b3Vec3 v, float max_length)
{
    float length = vector_length(v);

    if (length > max_length && length > 0.0f)
    {
        float s = max_length / length;
        b3Vec3 r = { v.x * s, v.y * s, v.z * s };
        return r;
    }

    return v;
}

static void character_step_3d(PhysicsWorld* world, const CharacterInput* input, float dt)
{
    const ProjectSettings* s = world->settings;
    PhysicsBody* body = &world->bodies[world->character];
    b3BodyId id = body->body3;
    const SceneObject* object = &world->scene->objects[body->object];

    b3Vec3 gravity = { s->gravity_3d[0], s->gravity_3d[1], s->gravity_3d[2] };
    float gravity_length = vector_length(gravity);
    b3Vec3 up = { 0.0f, 1.0f, 0.0f };

    if (gravity_length > 1e-4f)
    {
        up.x = -gravity.x / gravity_length;
        up.y = -gravity.y / gravity_length;
        up.z = -gravity.z / gravity_length;
    }

    b3Pos position = b3Body_GetPosition(id);
    b3Vec3 velocity = b3Body_GetLinearVelocity(id);
    float m = world->character_mass;

    // Is there walkable ground right under the capsule's lower sphere ?
    float radius = world->character_radius;
    b3Pos foot = {
        position.x + world->character_center[0] - up.x * world->character_half_segment,
        position.y + world->character_center[1] - up.y * world->character_half_segment,
        position.z + world->character_center[2] - up.z * world->character_half_segment
    };

    float probe_distance = radius * 0.1f + 0.12f;
    b3Vec3 proxy_point = { 0.0f, 0.0f, 0.0f };
    b3ShapeProxy proxy = { &proxy_point, 1, radius * 0.9f };
    b3Vec3 translation = { -up.x * probe_distance, -up.y * probe_distance, -up.z * probe_distance };

    int layer = SDL_clamp(object->layer, 0, PROJECT_MAX_LAYERS - 1);

    b3QueryFilter filter = b3DefaultQueryFilter();
    filter.categoryBits = 1ull << layer;
    filter.maskBits = s->layer_masks[layer];

    GroundProbe3 probe;
    SDL_zero(probe);
    probe.self = id;
    b3World_CastShape(world->world3, foot, &proxy, translation, filter, ground_callback_3d, &probe);

    float min_up = SDL_cosf(s->character_max_slope * SDL_PI_F / 180.0f);

    world->jump_cooldown = SDL_max(0.0f, world->jump_cooldown - dt);
    world->character_grounded = probe.hit && vector_dot(probe.normal, up) >= min_up && world->jump_cooldown <= 0.0f;

    float speed = s->character_speed * (input->sprint ? s->character_sprint_multiplier : 1.0f);
    b3Vec3 desired = { input->move[0] * speed, input->move[1] * speed, input->move[2] * speed };
    b3Vec3 delta;

    if (world->character_grounded)
    {
        b3Vec3 n = probe.normal;
        b3Vec3 ground_velocity = b3Body_GetWorldPointVelocity(probe.body, probe.point);

        // The desired velocity along the ground's slope, relative to the ground (moving platforms).
        float desired_speed = vector_length(desired);
        b3Vec3 tangent = vector_add_scaled(desired, n, -vector_dot(desired, n));
        float tangent_length = vector_length(tangent);

        if (tangent_length > 1e-5f)
        {
            float scale = desired_speed / tangent_length;
            tangent.x *= scale;
            tangent.y *= scale;
            tangent.z *= scale;
        }

        b3Vec3 relative = { velocity.x - ground_velocity.x, velocity.y - ground_velocity.y, velocity.z - ground_velocity.z };
        b3Vec3 change = { tangent.x - relative.x, tangent.y - relative.y, tangent.z - relative.z };
        change = vector_add_scaled(change, n, -vector_dot(change, n));
        delta = vector_clamp_length(change, s->character_acceleration * dt);

        // Gravity's pull along the slope, cancelled : standing still on a slope doesn't slide.
        b3Vec3 slope_gravity = vector_add_scaled(gravity, n, -vector_dot(gravity, n));
        delta = vector_add_scaled(delta, slope_gravity, -dt);

        // Newton's third law : what the character pushes off from gets pushed back.
        if (b3Body_GetType(probe.body) == b3_dynamicBody)
        {
            b3Vec3 reaction = { -delta.x * m, -delta.y * m, -delta.z * m };
            b3Body_ApplyLinearImpulse(probe.body, reaction, probe.point, true);
        }

        if (input->jump)
        {
            float up_speed = vector_dot(velocity, up);
            float boost = s->character_jump_speed - up_speed;

            if (boost > 0.0f)
            {
                delta = vector_add_scaled(delta, up, boost);
                world->jump_cooldown = JUMP_COOLDOWN;
                world->character_grounded = 0;
            }
        }
    }
    else
    {
        // In the air : gravity does its work, and the player only steers a little.
        b3Vec3 horizontal = vector_add_scaled(velocity, up, -vector_dot(velocity, up));
        b3Vec3 change = { desired.x - horizontal.x, desired.y - horizontal.y, desired.z - horizontal.z };
        delta = vector_clamp_length(change, s->character_acceleration * s->character_air_control * dt);
    }

    b3Vec3 impulse = { delta.x * m, delta.y * m, delta.z * m };
    b3Body_ApplyLinearImpulseToCenter(id, impulse, true);
}

typedef struct
{
    b2BodyId self;
    int hit;
    float fraction;
    b2Vec2 normal;
    b2Pos point;
    b2BodyId body;
} GroundProbe2;

static float ground_callback_2d(b2ShapeId shape, b2Pos point, b2Vec2 normal, float fraction, void* context)
{
    GroundProbe2* probe = context;
    b2BodyId body = b2Shape_GetBody(shape);

    if (B2_ID_EQUALS(body, probe->self))
    {
        return -1.0f;
    }

    if (!probe->hit || fraction < probe->fraction)
    {
        probe->hit = 1;
        probe->fraction = fraction;
        probe->normal = normal;
        probe->point = point;
        probe->body = body;
    }

    return fraction;
}

static void character_step_2d(PhysicsWorld* world, const CharacterInput* input, float dt)
{
    const ProjectSettings* s = world->settings;
    PhysicsBody* body = &world->bodies[world->character];
    b2BodyId id = body->body2;
    const SceneObject* object = &world->scene->objects[body->object];

    b2Vec2 gravity = { s->gravity_2d[0], s->gravity_2d[1] };
    float gravity_length = SDL_sqrtf(gravity.x * gravity.x + gravity.y * gravity.y);
    b2Vec2 up = { 0.0f, 1.0f };

    if (gravity_length > 1e-4f)
    {
        up.x = -gravity.x / gravity_length;
        up.y = -gravity.y / gravity_length;
    }

    b2Pos position = b2Body_GetPosition(id);
    b2Vec2 velocity = b2Body_GetLinearVelocity(id);
    float m = world->character_mass;
    float radius = world->character_radius;

    b2Pos foot = {
        position.x + world->character_center[0] - up.x * world->character_half_segment,
        position.y + world->character_center[1] - up.y * world->character_half_segment
    };

    float probe_distance = radius * 0.1f + 0.08f;
    b2Vec2 point = { 0.0f, 0.0f };
    b2ShapeProxy proxy = b2MakeProxy(&point, 1, radius * 0.9f);
    b2Vec2 translation = { -up.x * probe_distance, -up.y * probe_distance };

    int layer = SDL_clamp(object->layer, 0, PROJECT_MAX_LAYERS - 1);
    b2QueryFilter filter = b2DefaultQueryFilter();
    filter.categoryBits = 1ull << layer;
    filter.maskBits = s->layer_masks[layer];

    GroundProbe2 probe;
    SDL_zero(probe);
    probe.self = id;
    b2World_CastShape(world->world2, foot, &proxy, translation, filter, ground_callback_2d, &probe);

    float min_up = SDL_cosf(s->character_max_slope * SDL_PI_F / 180.0f);

    world->jump_cooldown = SDL_max(0.0f, world->jump_cooldown - dt);
    world->character_grounded = probe.hit && (probe.normal.x * up.x + probe.normal.y * up.y) >= min_up && world->jump_cooldown <= 0.0f;

    float speed = s->character_speed * (input->sprint ? s->character_sprint_multiplier : 1.0f);
    float desired = input->move[0] * speed;
    b2Vec2 delta = { 0.0f, 0.0f };

    if (world->character_grounded)
    {
        b2Vec2 n = probe.normal;
        b2Vec2 tangent = { n.y, -n.x }; // along the ground, toward +X
        b2Vec2 ground_velocity = b2Body_GetWorldPointVelocity(probe.body, probe.point);

        float along = (velocity.x - ground_velocity.x) * tangent.x + (velocity.y - ground_velocity.y) * tangent.y;
        float change = SDL_clamp(desired - along, -s->character_acceleration * dt, s->character_acceleration * dt);

        // Plus what cancels gravity's pull along the slope.
        float slope_gravity = gravity.x * tangent.x + gravity.y * tangent.y;
        change -= slope_gravity * dt;

        delta.x = tangent.x * change;
        delta.y = tangent.y * change;

        if (b2Body_GetType(probe.body) == b2_dynamicBody)
        {
            b2Vec2 reaction = { -delta.x * m, -delta.y * m };
            b2Body_ApplyLinearImpulse(probe.body, reaction, probe.point, true);
        }

        if (input->jump)
        {
            float up_speed = velocity.x * up.x + velocity.y * up.y;
            float boost = s->character_jump_speed - up_speed;

            if (boost > 0.0f)
            {
                delta.x += up.x * boost;
                delta.y += up.y * boost;
                world->jump_cooldown = JUMP_COOLDOWN;
                world->character_grounded = 0;
            }
        }
    }
    else
    {
        float limit = s->character_acceleration * s->character_air_control * dt;
        float change = SDL_clamp(desired - velocity.x, -limit, limit);
        delta.x = change;
    }

    b2Vec2 impulse = { delta.x * m, delta.y * m };
    b2Body_ApplyLinearImpulseToCenter(id, impulse, true);
}

void physics_step(PhysicsWorld* world, const CharacterInput* input, float time_step)
{
    for (int i = 0; i < world->body_count; i++)
    {
        PhysicsBody* body = &world->bodies[i];
        SDL_memcpy(body->previous_position, body->current_position, sizeof(body->previous_position));
        SDL_memcpy(body->previous_rotation, body->current_rotation, sizeof(body->previous_rotation));
    }

    if (world->character >= 0 && input)
    {
        if (world->bodies[world->character].is_2d)
        {
            character_step_2d(world, input, time_step);
        }
        else
        {
            character_step_3d(world, input, time_step);
        }
    }

    if (world->has_3d)
    {
        b3World_Step(world->world3, time_step, world->settings->physics_substeps);
    }

    if (world->has_2d)
    {
        b2World_Step(world->world2, time_step, world->settings->physics_substeps);
    }

    for (int i = 0; i < world->body_count; i++)
    {
        read_body(&world->bodies[i]);
    }
}

void physics_update_scene(PhysicsWorld* world, float alpha)
{
    for (int i = 0; i < world->body_count; i++)
    {
        const PhysicsBody* body = &world->bodies[i];
        SceneObject* object = &world->scene->objects[body->object];

        int axes = body->is_2d ? 2 : 3;

        for (int axis = 0; axis < axes; axis++)
        {
            object->position[axis] = body->previous_position[axis] + (body->current_position[axis] - body->previous_position[axis]) * alpha;
        }

        // Normalized linear interpolation, along the shorter arc.
        float dot = 0.0f;

        for (int c = 0; c < 4; c++)
        {
            dot += body->previous_rotation[c] * body->current_rotation[c];
        }

        float sign = dot < 0.0f ? -1.0f : 1.0f;
        float q[4];
        float length = 0.0f;

        for (int c = 0; c < 4; c++)
        {
            q[c] = body->previous_rotation[c] + (body->current_rotation[c] * sign - body->previous_rotation[c]) * alpha;
            length += q[c] * q[c];
        }

        length = SDL_sqrtf(length);

        if (length > 1e-6f)
        {
            for (int c = 0; c < 4; c++)
            {
                q[c] /= length;
            }
        }

        float rotation[9];
        matrix_from_quaternion(q, rotation);
        scene_euler_from_rotation(rotation, object->rotation);
    }
}

int physics_character_object(const PhysicsWorld* world)
{
    return world->character >= 0 ? world->bodies[world->character].object : -1;
}

int physics_character_grounded(const PhysicsWorld* world)
{
    return world->character_grounded;
}
