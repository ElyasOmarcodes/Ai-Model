/* Static collision world of the CityLord map (BoxCollider + MeshCollider from the scene)
 * with a Physics.Raycast equivalent used by the aiming / shooting code. Unity space. */
#ifndef CL_PHYSICS_H
#define CL_PHYSICS_H

#include "raylib.h"
#include "cl_assets.h"

typedef struct {
    Vector3 point;
    Vector3 normal;
    float distance;
    int collider;     /* index into cl_colliders, -1 when nothing was hit */
} CL_RaycastHit;

typedef struct {
    int collider;
    Vector3 *tris;    /* world-space triangles (3 vertices each) for mesh colliders */
    int tri_count;
    Matrix world, inv_world; /* box colliders */
    Vector3 bmin, bmax;      /* world AABB for early-out */
} CL_ColliderShape;

typedef struct {
    CL_ColliderShape *shapes;
    int count;
} CL_World;

void cl_world_build(CL_World *w, const CL_Assets *assets);
void cl_world_free(CL_World *w);
/* Physics.Raycast(origin, direction, out hit, maxDistance, layerMask). The game's PhysicsManager has
 * queriesHitTriggers = true and queriesHitBackfaces = false, so queries hit trigger volumes
 * (the ammo boxes) and mesh colliders only from their front side. */
int cl_raycast(const CL_World *w, Vector3 origin, Vector3 direction, float max_distance, unsigned int layer_mask,
               CL_RaycastHit *hit);
/* Physics.CheckSphere: does a sphere overlap any (non-trigger) collider? */
int cl_check_sphere(const CL_World *w, Vector3 center, float radius, unsigned int layer_mask);
/* Physics.CheckCapsule, evaluated as spheres swept along the segment (spacing radius/4). */
int cl_check_capsule(const CL_World *w, Vector3 a, Vector3 b, float radius, unsigned int layer_mask);
/* Depenetration used by the CharacterController port: pushes a sphere out of the
 * static geometry. Returns the summed push; *ground is set when a supporting
 * surface (normal.y >= cos(slope_limit)) was touched. */
Vector3 cl_sphere_depenetrate(const CL_World *w, Vector3 *center, float radius, unsigned int layer_mask,
                              float slope_limit_deg, int *ground);
/* Trigger volumes (the ammo boxes): index of the trigger collider containing p, or -1. */
int cl_trigger_at(Vector3 p);

#endif
