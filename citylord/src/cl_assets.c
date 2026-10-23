#include "cl_assets.h"
#include "citylord_data.h"
#include "rlgl.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* CLM1 layout (little endian), written by tools/export_scene.py:
 *   char magic[4] = "CLM1"; uint32 vertex_count, submesh_count, flags
 *   float pos[3*n]; [flags&1] float normal[3*n]; [flags&2] float uv0[2*n];
 *   [flags&4] float uv1[2*n]; [flags&8] float color[4*n]; [flags&16] float tangent[4*n]
 *   per submesh: uint32 index_count; uint32 indices[index_count]           */
enum { CLM_NORMALS = 1, CLM_UV0 = 2, CLM_UV1 = 4, CLM_COLORS = 8, CLM_TANGENTS = 16 };

static float *read_floats(FILE *f, int count)
{
    float *p = malloc(sizeof(float) * (size_t)count);
    if (p && fread(p, sizeof(float), (size_t)count, f) != (size_t)count) { free(p); return NULL; }
    return p;
}

static int load_mesh(CL_GpuMesh *m, const char *path)
{
    FILE *f = fopen(path, "rb");
    if (!f) { TraceLog(LOG_ERROR, "CL: cannot open %s", path); return 0; }
    char magic[4];
    unsigned int hdr[3];
    if (fread(magic, 1, 4, f) != 4 || memcmp(magic, "CLM1", 4) || fread(hdr, 4, 3, f) != 3) { fclose(f); return 0; }
    int n = (int)hdr[0], flags = (int)hdr[2];
    memset(m, 0, sizeof(*m));
    m->vertex_count = n;
    m->submesh_count = (int)hdr[1];
    m->positions = read_floats(f, 3 * n);
    if (flags & CLM_NORMALS) m->normals = read_floats(f, 3 * n);
    if (flags & CLM_UV0) m->uv0 = read_floats(f, 2 * n);
    if (flags & CLM_UV1) m->uv1 = read_floats(f, 2 * n);
    if (flags & CLM_COLORS) free(read_floats(f, 4 * n));
    if (flags & CLM_TANGENTS) m->tangents = read_floats(f, 4 * n);
    m->submesh_first = calloc((size_t)m->submesh_count, sizeof(int));
    m->submesh_count_idx = calloc((size_t)m->submesh_count, sizeof(int));
    long start = ftell(f);
    int total = 0;
    for (int s = 0; s < m->submesh_count; s++) {
        unsigned int c;
        if (fread(&c, 4, 1, f) != 1) break;
        m->submesh_first[s] = total;
        m->submesh_count_idx[s] = (int)c;
        total += (int)c;
        fseek(f, (long)c * 4, SEEK_CUR);
    }
    m->indices = malloc(sizeof(unsigned int) * (size_t)(total ? total : 1));
    m->index_count = total;
    fseek(f, start, SEEK_SET);
    for (int s = 0; s < m->submesh_count; s++) {
        unsigned int c;
        if (fread(&c, 4, 1, f) != 1 || fread(m->indices + m->submesh_first[s], 4, c, f) != c) break;
    }
    fclose(f);
    if (!m->normals) m->normals = calloc((size_t)n * 3, sizeof(float));
    if (!m->uv0) m->uv0 = calloc((size_t)n * 2, sizeof(float));
    if (!m->tangents) {
        m->tangents = calloc((size_t)n * 4, sizeof(float));
        for (int i = 0; i < n; i++) { m->tangents[i * 4] = 1.0f; m->tangents[i * 4 + 3] = 1.0f; }
    }

    /* GPU upload. Every mesh in the map has fewer than 65536 vertices, so 16-bit indices
     * (what rlDrawVertexArrayElements expects) are lossless. */
    unsigned short *idx16 = malloc(sizeof(unsigned short) * (size_t)(total ? total : 1));
    for (int i = 0; i < total; i++) idx16[i] = (unsigned short)m->indices[i];
    m->vao = rlLoadVertexArray();
    rlEnableVertexArray(m->vao);
    m->vbo_pos = rlLoadVertexBuffer(m->positions, n * 3 * (int)sizeof(float), false);
    rlSetVertexAttribute(0, 3, RL_FLOAT, false, 0, 0);
    rlEnableVertexAttribute(0);
    m->vbo_uv0 = rlLoadVertexBuffer(m->uv0, n * 2 * (int)sizeof(float), false);
    rlSetVertexAttribute(1, 2, RL_FLOAT, false, 0, 0);
    rlEnableVertexAttribute(1);
    m->vbo_nrm = rlLoadVertexBuffer(m->normals, n * 3 * (int)sizeof(float), false);
    rlSetVertexAttribute(2, 3, RL_FLOAT, false, 0, 0);
    rlEnableVertexAttribute(2);
    m->vbo_tan = rlLoadVertexBuffer(m->tangents, n * 4 * (int)sizeof(float), false);
    rlSetVertexAttribute(4, 4, RL_FLOAT, false, 0, 0);
    rlEnableVertexAttribute(4);
    m->ebo = rlLoadVertexBufferElement(idx16, total * (int)sizeof(unsigned short), false);
    rlDisableVertexArray();
    free(idx16);
    return 1;
}

static unsigned int load_texture(const CL_Texture *t, const char *dir)
{
    char path[1024];
    snprintf(path, sizeof(path), "%s/%s", dir, t->file);
    Image img = LoadImage(path);
    if (!img.data) return 0;
    ImageFormat(&img, PIXELFORMAT_UNCOMPRESSED_R8G8B8A8);
    /* PNGs are stored top row first; Unity's v=0 is the bottom row */
    ImageFlipVertical(&img);
    unsigned int id = rlLoadTexture(img.data, img.width, img.height, img.format, 1);
    int mips = 1;
    rlGenTextureMipmaps(id, img.width, img.height, img.format, &mips);
    /* Unity filter modes: 0 point, 1 bilinear (linear within a mip, nearest between mips), 2 trilinear */
    int minf = t->filter_mode == 0 ? RL_TEXTURE_FILTER_MIP_NEAREST
             : t->filter_mode == 1 ? RL_TEXTURE_FILTER_LINEAR_MIP_NEAREST : RL_TEXTURE_FILTER_MIP_LINEAR;
    rlTextureParameters(id, RL_TEXTURE_MIN_FILTER, minf);
    rlTextureParameters(id, RL_TEXTURE_MAG_FILTER, t->filter_mode == 0 ? RL_TEXTURE_FILTER_NEAREST : RL_TEXTURE_FILTER_LINEAR);
    rlTextureParameters(id, RL_TEXTURE_WRAP_S, t->wrap_u == 0 ? RL_TEXTURE_WRAP_REPEAT : RL_TEXTURE_WRAP_CLAMP);
    rlTextureParameters(id, RL_TEXTURE_WRAP_T, t->wrap_v == 0 ? RL_TEXTURE_WRAP_REPEAT : RL_TEXTURE_WRAP_CLAMP);
    if (t->aniso > 1) rlTextureParameters(id, RL_TEXTURE_FILTER_ANISOTROPIC, t->aniso);
    if (t->mip_bias != 0.0f) rlTextureParameters(id, RL_TEXTURE_MIPMAP_BIAS_RATIO, (int)(t->mip_bias * 100.0f));
    UnloadImage(img);
    return id;
}

int cl_assets_load(CL_Assets *a, const char *dir)
{
    memset(a, 0, sizeof(*a));
    a->meshes = calloc((size_t)cl_mesh_count, sizeof(CL_GpuMesh));
    a->textures = calloc((size_t)cl_texture_count, sizeof(unsigned int));
    char path[1024];
    for (int i = 0; i < cl_mesh_count; i++) {
        snprintf(path, sizeof(path), "%s/%s", dir, cl_meshes[i].file);
        if (!load_mesh(&a->meshes[i], path)) return 0;
    }
    for (int i = 0; i < cl_texture_count; i++) {
        a->textures[i] = load_texture(&cl_textures[i], dir);
        if (!a->textures[i]) { TraceLog(LOG_ERROR, "CL: missing texture %s", cl_textures[i].file); return 0; }
    }
    unsigned char white[4] = {255, 255, 255, 255}, flat[4] = {128, 128, 255, 255};
    a->white_texture = rlLoadTexture(white, 1, 1, PIXELFORMAT_UNCOMPRESSED_R8G8B8A8, 1);
    a->flat_normal_texture = rlLoadTexture(flat, 1, 1, PIXELFORMAT_UNCOMPRESSED_R8G8B8A8, 1);
    return 1;
}

void cl_assets_unload(CL_Assets *a)
{
    for (int i = 0; i < cl_mesh_count; i++) {
        CL_GpuMesh *m = &a->meshes[i];
        rlUnloadVertexArray(m->vao);
        rlUnloadVertexBuffer(m->vbo_pos); rlUnloadVertexBuffer(m->vbo_nrm);
        rlUnloadVertexBuffer(m->vbo_uv0); rlUnloadVertexBuffer(m->vbo_tan); rlUnloadVertexBuffer(m->ebo);
        free(m->positions); free(m->normals); free(m->uv0); free(m->uv1); free(m->tangents);
        free(m->indices); free(m->submesh_first); free(m->submesh_count_idx);
    }
    for (int i = 0; i < cl_texture_count; i++) rlUnloadTexture(a->textures[i]);
    rlUnloadTexture(a->white_texture);
    rlUnloadTexture(a->flat_normal_texture);
    free(a->meshes);
    free(a->textures);
}
