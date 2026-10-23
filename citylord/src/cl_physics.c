#include "cl_physics.h"
#include "citylord_data.h"
#include "raymath.h"

#include <float.h>
#include <math.h>
#include <stdlib.h>

static Matrix mat_cols(const float *w)
{
    return (Matrix){w[0], w[4], w[8], w[12], w[1], w[5], w[9], w[13], w[2], w[6], w[10], w[14], w[3], w[7], w[11], w[15]};
}

static void grow(Vector3 *mn, Vector3 *mx, Vector3 p)
{
    *mn = Vector3Min(*mn, p);
    *mx = Vector3Max(*mx, p);
}

void cl_world_build(CL_World *w, const CL_Assets *assets)
{
    w->shapes = calloc((size_t)cl_collider_count, sizeof(CL_ColliderShape));
    w->count = 0;
    for (int i = 0; i < cl_collider_count; i++) {
        const CL_Collider *c = &cl_colliders[i];
        CL_ColliderShape *s = &w->shapes[w->count++];
        s->collider = i;
        s->world = mat_cols(c->world);
        s->bmin = (Vector3){FLT_MAX, FLT_MAX, FLT_MAX};
        s->bmax = (Vector3){-FLT_MAX, -FLT_MAX, -FLT_MAX};
        if (c->kind == CL_COLLIDER_BOX) {
            s->inv_world = MatrixInvert(s->world);
            for (int k = 0; k < 8; k++) {
                Vector3 p = {c->center[0] + ((k & 1) ? 0.5f : -0.5f) * c->size[0],
                             c->center[1] + ((k & 2) ? 0.5f : -0.5f) * c->size[1],
                             c->center[2] + ((k & 4) ? 0.5f : -0.5f) * c->size[2]};
                grow(&s->bmin, &s->bmax, Vector3Transform(p, s->world));
            }
        } else {
            const CL_GpuMesh *m = &assets->meshes[c->mesh];
            s->tri_count = m->index_count / 3;
            s->tris = malloc(sizeof(Vector3) * 3 * (size_t)(s->tri_count ? s->tri_count : 1));
            for (int t = 0; t < s->tri_count * 3; t++) {
                const float *p = &m->positions[m->indices[t] * 3];
                s->tris[t] = Vector3Transform((Vector3){p[0], p[1], p[2]}, s->world);
                grow(&s->bmin, &s->bmax, s->tris[t]);
            }
        }
    }
}

void cl_world_free(CL_World *w)
{
    for (int i = 0; i < w->count; i++) free(w->shapes[i].tris);
    free(w->shapes);
    w->shapes = NULL;
    w->count = 0;
}

static int ray_aabb(Vector3 o, Vector3 inv_d, Vector3 mn, Vector3 mx, float max_t, float *t_enter)
{
    float t0 = 0.0f, t1 = max_t;
    const float *po = &o.x, *pi = &inv_d.x, *pmn = &mn.x, *pmx = &mx.x;
    int axis = -1, sign = 0;
    for (int a = 0; a < 3; a++) {
        float ta = (pmn[a] - po[a]) * pi[a], tb = (pmx[a] - po[a]) * pi[a];
        int s = 0;
        if (ta > tb) { float tmp = ta; ta = tb; tb = tmp; s = 1; }
        if (ta > t0) { t0 = ta; axis = a; sign = s; }
        if (tb < t1) t1 = tb;
        if (t0 > t1) return -1;
    }
    *t_enter = t0;
    return axis < 0 ? 3 : axis * 2 + sign; /* 3: origin inside */
}

/* Möller–Trumbore. Unity's Physics.Raycast does not pass eMESH_BOTH_SIDES to PhysX, so
 * mesh colliders are hit only from their front side. Unity triangles are clockwise in a
 * left-handed basis, which makes cross(b-a, c-a) the outward face normal. */
static int ray_tri(Vector3 o, Vector3 d, Vector3 a, Vector3 b, Vector3 c, float *t, Vector3 *n)
{
    Vector3 e1 = Vector3Subtract(b, a), e2 = Vector3Subtract(c, a);
    Vector3 fn = Vector3CrossProduct(e1, e2);
    if (Vector3DotProduct(fn, d) >= 0.0f) return 0;
    Vector3 p = Vector3CrossProduct(d, e2);
    float det = Vector3DotProduct(e1, p);
    if (fabsf(det) < 1e-12f) return 0;
    float inv = 1.0f / det;
    Vector3 s = Vector3Subtract(o, a);
    float u = Vector3DotProduct(s, p) * inv;
    if (u < 0.0f || u > 1.0f) return 0;
    Vector3 q = Vector3CrossProduct(s, e1);
    float v = Vector3DotProduct(d, q) * inv;
    if (v < 0.0f || u + v > 1.0f) return 0;
    *t = Vector3DotProduct(e2, q) * inv;
    *n = Vector3Normalize(fn);
    return *t >= 0.0f;
}

int cl_raycast(const CL_World *w, Vector3 origin, Vector3 dir, float max_distance, unsigned int layer_mask, CL_RaycastHit *hit)
{
    dir = Vector3Normalize(dir);
    Vector3 inv_d = {1.0f / dir.x, 1.0f / dir.y, 1.0f / dir.z};
    float best = max_distance;
    int found = 0;
    for (int i = 0; i < w->count; i++) {
        const CL_ColliderShape *s = &w->shapes[i];
        const CL_Collider *c = &cl_colliders[s->collider];
        if (!(layer_mask & (1u << c->layer))) continue;
        float te;
        if (ray_aabb(origin, inv_d, s->bmin, s->bmax, best, &te) < 0) continue;
        if (c->kind == CL_COLLIDER_BOX) {
            /* intersect in the collider's local space (handles rotation and non-uniform scale) */
            Vector3 lo = Vector3Transform(origin, s->inv_world);
            Vector3 lp = Vector3Transform(Vector3Add(origin, dir), s->inv_world);
            Vector3 ld = Vector3Subtract(lp, lo);
            Vector3 hmn = {c->center[0] - c->size[0] * 0.5f, c->center[1] - c->size[1] * 0.5f, c->center[2] - c->size[2] * 0.5f};
            Vector3 hmx = {c->center[0] + c->size[0] * 0.5f, c->center[1] + c->size[1] * 0.5f, c->center[2] + c->size[2] * 0.5f};
            Vector3 linv = {1.0f / ld.x, 1.0f / ld.y, 1.0f / ld.z};
            float lt;
            int face = ray_aabb(lo, linv, hmn, hmx, best, &lt);
            if (face < 0 || face == 3) continue; /* PhysX ignores boxes that contain the origin */
            if (lt < best) {
                best = lt;
                found = 1;
                Vector3 ln = {0};
                (&ln.x)[face / 2] = (face & 1) ? 1.0f : -1.0f;
                Matrix nm = MatrixTranspose(s->inv_world);
                Vector3 wn = Vector3Normalize((Vector3){nm.m0 * ln.x + nm.m4 * ln.y + nm.m8 * ln.z,
                                                        nm.m1 * ln.x + nm.m5 * ln.y + nm.m9 * ln.z,
                                                        nm.m2 * ln.x + nm.m6 * ln.y + nm.m10 * ln.z});
                hit->normal = wn;
                hit->collider = s->collider;
            }
        } else {
            for (int t = 0; t < s->tri_count; t++) {
                float tt;
                Vector3 n;
                if (ray_tri(origin, dir, s->tris[t * 3], s->tris[t * 3 + 1], s->tris[t * 3 + 2], &tt, &n) && tt < best) {
                    best = tt;
                    found = 1;
                    hit->normal = n;
                    hit->collider = s->collider;
                }
            }
        }
    }
    if (found) {
        hit->distance = best;
        hit->point = Vector3Add(origin, Vector3Scale(dir, best));
    } else {
        hit->collider = -1;
    }
    return found;
}

static Vector3 closest_on_triangle(Vector3 p, Vector3 a, Vector3 b, Vector3 c)
{
    /* Ericson, Real-Time Collision Detection 5.1.5 */
    Vector3 ab = Vector3Subtract(b, a), ac = Vector3Subtract(c, a), ap = Vector3Subtract(p, a);
    float d1 = Vector3DotProduct(ab, ap), d2 = Vector3DotProduct(ac, ap);
    if (d1 <= 0.0f && d2 <= 0.0f) return a;
    Vector3 bp = Vector3Subtract(p, b);
    float d3 = Vector3DotProduct(ab, bp), d4 = Vector3DotProduct(ac, bp);
    if (d3 >= 0.0f && d4 <= d3) return b;
    float vc = d1 * d4 - d3 * d2;
    if (vc <= 0.0f && d1 >= 0.0f && d3 <= 0.0f) return Vector3Add(a, Vector3Scale(ab, d1 / (d1 - d3)));
    Vector3 cp = Vector3Subtract(p, c);
    float d5 = Vector3DotProduct(ab, cp), d6 = Vector3DotProduct(ac, cp);
    if (d6 >= 0.0f && d5 <= d6) return c;
    float vb = d5 * d2 - d1 * d6;
    if (vb <= 0.0f && d2 >= 0.0f && d6 <= 0.0f) return Vector3Add(a, Vector3Scale(ac, d2 / (d2 - d6)));
    float va = d3 * d6 - d5 * d4;
    if (va <= 0.0f && (d4 - d3) >= 0.0f && (d5 - d6) >= 0.0f)
        return Vector3Add(b, Vector3Scale(Vector3Subtract(c, b), (d4 - d3) / ((d4 - d3) + (d5 - d6))));
    float denom = 1.0f / (va + vb + vc);
    return Vector3Add(a, Vector3Add(Vector3Scale(ab, vb * denom), Vector3Scale(ac, vc * denom)));
}

/* Closest point on an oriented (possibly scaled) box collider, in world space. */
static Vector3 closest_on_box(const CL_ColliderShape *s, const CL_Collider *c, Vector3 p)
{
    Vector3 lp = Vector3Transform(p, s->inv_world);
    Vector3 q = {Clamp(lp.x, c->center[0] - c->size[0] * 0.5f, c->center[0] + c->size[0] * 0.5f),
                 Clamp(lp.y, c->center[1] - c->size[1] * 0.5f, c->center[1] + c->size[1] * 0.5f),
                 Clamp(lp.z, c->center[2] - c->size[2] * 0.5f, c->center[2] + c->size[2] * 0.5f)};
    return Vector3Transform(q, s->world);
}

static int sphere_touches_aabb(const CL_ColliderShape *s, Vector3 c, float r)
{
    return c.x + r >= s->bmin.x && c.x - r <= s->bmax.x && c.y + r >= s->bmin.y && c.y - r <= s->bmax.y &&
           c.z + r >= s->bmin.z && c.z - r <= s->bmax.z;
}

int cl_check_sphere(const CL_World *w, Vector3 center, float radius, unsigned int layer_mask)
{
    float r2 = radius * radius;
    for (int i = 0; i < w->count; i++) {
        const CL_ColliderShape *s = &w->shapes[i];
        const CL_Collider *c = &cl_colliders[s->collider];
        if (!(layer_mask & (1u << c->layer)) || !sphere_touches_aabb(s, center, radius)) continue;
        if (c->kind == CL_COLLIDER_BOX) {
            if (Vector3DistanceSqr(closest_on_box(s, c, center), center) <= r2) return 1;
        } else {
            for (int t = 0; t < s->tri_count; t++)
                if (Vector3DistanceSqr(closest_on_triangle(center, s->tris[t * 3], s->tris[t * 3 + 1], s->tris[t * 3 + 2]), center) <= r2)
                    return 1;
        }
    }
    return 0;
}

int cl_check_capsule(const CL_World *w, Vector3 a, Vector3 b, float radius, unsigned int layer_mask)
{
    float len = Vector3Distance(a, b);
    int steps = (int)ceilf(len / (radius * 0.25f));
    if (steps < 1) steps = 1;
    for (int i = 0; i <= steps; i++)
        if (cl_check_sphere(w, Vector3Lerp(a, b, (float)i / (float)steps), radius, layer_mask)) return 1;
    return 0;
}

Vector3 cl_sphere_depenetrate(const CL_World *w, Vector3 *center, float radius, unsigned int layer_mask,
                              float slope_limit_deg, int *ground)
{
    Vector3 total = {0};
    float min_up = cosf(slope_limit_deg * DEG2RAD);
    for (int iter = 0; iter < 4; iter++) {
        Vector3 push = {0};
        int any = 0;
        for (int i = 0; i < w->count; i++) {
            const CL_ColliderShape *s = &w->shapes[i];
            const CL_Collider *c = &cl_colliders[s->collider];
            if (c->is_trigger || !(layer_mask & (1u << c->layer)) || !sphere_touches_aabb(s, *center, radius)) continue;
            int n = c->kind == CL_COLLIDER_BOX ? 1 : s->tri_count;
            for (int t = 0; t < n; t++) {
                Vector3 q = c->kind == CL_COLLIDER_BOX ? closest_on_box(s, c, *center)
                          : closest_on_triangle(*center, s->tris[t * 3], s->tris[t * 3 + 1], s->tris[t * 3 + 2]);
                Vector3 d = Vector3Subtract(*center, q);
                float dist = Vector3Length(d);
                if (dist >= radius || dist < 1e-6f) continue;
                Vector3 nrm = Vector3Scale(d, 1.0f / dist);
                if (nrm.y >= min_up && ground) *ground = 1;
                Vector3 p = Vector3Scale(nrm, radius - dist);
                /* keep the largest push per axis so overlapping triangles do not add up */
                if (fabsf(p.x) > fabsf(push.x)) push.x = p.x;
                if (fabsf(p.y) > fabsf(push.y)) push.y = p.y;
                if (fabsf(p.z) > fabsf(push.z)) push.z = p.z;
                any = 1;
            }
        }
        if (!any) break;
        *center = Vector3Add(*center, push);
        total = Vector3Add(total, push);
    }
    return total;
}

int cl_trigger_at(Vector3 p)
{
    for (int i = 0; i < cl_collider_count; i++) {
        const CL_Collider *c = &cl_colliders[i];
        if (!c->is_trigger || c->kind != CL_COLLIDER_BOX) continue;
        Matrix inv = MatrixInvert(mat_cols(c->world));
        Vector3 lp = Vector3Transform(p, inv);
        if (fabsf(lp.x - c->center[0]) <= c->size[0] * 0.5f && fabsf(lp.y - c->center[1]) <= c->size[1] * 0.5f &&
            fabsf(lp.z - c->center[2]) <= c->size[2] * 0.5f)
            return i;
    }
    return -1;
}
