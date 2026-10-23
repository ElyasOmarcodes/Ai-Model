/* Renderer for the CityLord map: reproduces the Unity 2020.3 built-in forward pipeline
 * as used by the game (gamma color space, one directional light with soft shadows,
 * flat ambient, Mobile/Diffuse + Legacy Cutout + Standard shaders, 6-sided skybox). */
#ifndef CL_RENDER_H
#define CL_RENDER_H

#include "raylib.h"
#include "cl_assets.h"

/* Unity QualitySettings that affect the map's look. Defaults come from the game's
 * QualitySettings entry selected at runtime (see cl_quality_from_level). */
typedef struct {
    int shadows;          /* 0 off, 1 hard only, 2 hard + soft */
    int shadow_map_size;  /* directional shadow atlas size */
    int shadow_cascades;  /* 1, 2 or 4 */
    float shadow_distance;
    int anti_aliasing;
} CL_Quality;

CL_Quality cl_quality_from_level(int level);

/* Camera in Unity space. */
typedef struct {
    Vector3 position;
    Quaternion rotation;
    float fov, near_plane, far_plane;
} CL_Camera;

typedef struct CL_Renderer_ CL_Renderer_t;

CL_Renderer_t *cl_render_create(CL_Assets *assets, CL_Quality quality);
void cl_render_destroy(CL_Renderer_t *r);
/* Draws the whole map into the current framebuffer. */
void cl_render_frame(CL_Renderer_t *r, const CL_Camera *cam, int width, int height);
void cl_render_set_quality(CL_Renderer_t *r, CL_Quality quality);

/* Unity <-> OpenGL conversion: Unity is left handed (x right, y up, z forward);
 * we render in a right handed space by mirroring z. */
static inline Vector3 cl_u2gl(Vector3 v) { return (Vector3){ v.x, v.y, -v.z }; }
Vector3 cl_quat_rotate(Quaternion q, Vector3 v); /* Unity's q * v */

#endif
