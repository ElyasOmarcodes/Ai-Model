/* Loading of the exported CityLord meshes (CLM1 files) and textures. */
#ifndef CL_ASSETS_H
#define CL_ASSETS_H

#include "raylib.h"

typedef struct {
    int vertex_count;
    int submesh_count;
    float *positions;    /* Unity space xyz, kept on the CPU for the collision raycasts */
    float *normals;
    float *uv0, *uv1;
    float *tangents;     /* xyzw */
    unsigned int *indices;
    int index_count;
    int *submesh_first;  /* first index of each submesh inside indices */
    int *submesh_count_idx;
    unsigned int vao, vbo_pos, vbo_nrm, vbo_uv0, vbo_tan, ebo;
} CL_GpuMesh;

typedef struct {
    CL_GpuMesh *meshes;
    unsigned int *textures; /* GL ids, index matches cl_textures */
    unsigned int white_texture, flat_normal_texture;
} CL_Assets;

int cl_assets_load(CL_Assets *a, const char *asset_dir);
void cl_assets_unload(CL_Assets *a);

#endif
