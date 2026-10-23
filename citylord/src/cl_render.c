#include "cl_render.h"
#include "citylord_data.h"
#include "raymath.h"
#include "rlgl.h"

#include <math.h>
#include <stdlib.h>
#include <string.h>

/* The game's six QualitySettings entries (globalgamemanagers). */
CL_Quality cl_quality_from_level(int level)
{
    static const CL_Quality q[6] = {
        /* Very Low */ {0, 512, 1, 15.0f, 0},
        /* Low      */ {0, 512, 1, 20.0f, 0},
        /* Medium   */ {1, 512, 1, 20.0f, 0},
        /* High     */ {2, 1024, 2, 40.0f, 2},
        /* VeryHigh */ {2, 1024, 2, 70.0f, 2},
        /* Ultra    */ {2, 2048, 4, 150.0f, 2},
    };
    if (level < 0) level = 0;
    if (level > 5) level = 5;
    return q[level];
}

Vector3 cl_quat_rotate(Quaternion q, Vector3 v)
{
    Vector3 u = {q.x, q.y, q.z};
    Vector3 t = Vector3Scale(Vector3CrossProduct(u, v), 2.0f);
    return Vector3Add(Vector3Add(v, Vector3Scale(t, q.w)), Vector3CrossProduct(u, t));
}

/* ---------------------------------------------------------------- shaders */
static const char *VS_MAIN =
    "#version 330\n"
    "layout(location=0) in vec3 aPos; layout(location=1) in vec2 aUV; layout(location=2) in vec3 aNrm; layout(location=4) in vec4 aTan;\n"
    "uniform mat4 uModel; uniform mat4 uViewProj; uniform mat4 uNormalMat; uniform float uTanSign;\n"
    "out vec3 vWorld; out vec3 vNormal; out vec2 vUV; out vec4 vTan;\n"
    "void main(){ vec4 w = uModel*vec4(aPos,1.0); vWorld = w.xyz; vNormal = mat3(uNormalMat)*aNrm;\n"
    "  vTan = vec4(mat3(uModel)*aTan.xyz, aTan.w*uTanSign); vUV = aUV; gl_Position = uViewProj*w; }\n";

static const char *FS_MAIN =
    "#version 330\n"
    "in vec3 vWorld; in vec3 vNormal; in vec2 vUV; in vec4 vTan; out vec4 fragColor;\n"
    "uniform int uShader; uniform vec4 uColor; uniform sampler2D uMainTex; uniform vec4 uMainST; uniform float uCutoff;\n"
    "uniform sampler2D uBumpTex; uniform vec4 uBumpST; uniform float uBumpScale; uniform int uNormalMap;\n"
    "uniform sampler2D uMetalTex; uniform int uMetalGloss; uniform float uMetallic; uniform float uGlossiness; uniform float uGlossMapScale; uniform int uSmoothChannel;\n"
    "uniform vec3 uEmission; uniform int uSpecHighlights; uniform int uGlossyRefl;\n"
    "uniform vec3 uLightDir; uniform vec3 uLightColor; uniform vec3 uAmbient; uniform vec3 uCamPos;\n"
    "uniform int uFog; uniform int uFogMode; uniform vec3 uFogColor; uniform vec3 uFogParams;\n"
    "uniform sampler2D uShadowMap; uniform int uShadows; uniform int uReceive; uniform float uShadowStrength; uniform int uCascades;\n"
    "uniform mat4 uCascadeVP[4]; uniform vec4 uCascadeRect[4]; uniform float uCascadeFar[4]; uniform vec2 uShadowTexel; uniform float uShadowDistance; uniform vec3 uCamFwd;\n"
    "uniform sampler2D uSky0; uniform sampler2D uSky1; uniform sampler2D uSky2; uniform sampler2D uSky3; uniform sampler2D uSky4; uniform sampler2D uSky5; uniform vec3 uSkyTint;\n"
    /* 6 sided skybox lookup; direction in Unity space */
    "vec3 skyColor(vec3 d, float lod){ vec3 a = abs(d); vec2 uv; vec3 c;\n"
    "  if (a.z >= a.x && a.z >= a.y) { if (d.z > 0.0) { uv = vec2(d.x, d.y)/a.z; c = textureLod(uSky0, uv*0.5+0.5, lod).rgb; } else { uv = vec2(-d.x, d.y)/a.z; c = textureLod(uSky1, uv*0.5+0.5, lod).rgb; } }\n"
    "  else if (a.x >= a.y) { if (d.x > 0.0) { uv = vec2(-d.z, d.y)/a.x; c = textureLod(uSky2, uv*0.5+0.5, lod).rgb; } else { uv = vec2(d.z, d.y)/a.x; c = textureLod(uSky3, uv*0.5+0.5, lod).rgb; } }\n"
    "  else { if (d.y > 0.0) { uv = vec2(d.x, -d.z)/a.y; c = textureLod(uSky4, uv*0.5+0.5, lod).rgb; } else { uv = vec2(d.x, d.z)/a.y; c = textureLod(uSky5, uv*0.5+0.5, lod).rgb; } }\n"
    "  return c*uSkyTint; }\n"
    "float sampleShadow(vec3 wpos){\n"
    "  if (uShadows == 0 || uReceive == 0) return 1.0;\n"
    "  float z = dot(wpos - uCamPos, uCamFwd); if (z > uShadowDistance) return 1.0;\n"
    "  int ci = 0; for (int i = 0; i < 3; i++) if (i < uCascades-1 && z > uCascadeFar[i]) ci = i+1;\n"
    "  vec4 p = uCascadeVP[ci]*vec4(wpos,1.0); vec3 s = p.xyz/p.w*0.5+0.5; vec4 r = uCascadeRect[ci];\n"
    "  vec2 uv = r.xy + s.xy*r.zw; float sh = 0.0;\n"
    "  if (uShadows == 1) { sh = (s.z <= texture(uShadowMap, uv).r) ? 1.0 : 0.0; }\n"
    /* soft: 3x3 tent filter (Unity SHADOWS_SOFT on mobile), done as 4x4 manual bilinear-PCF taps */
    "  else { vec2 tc = uv/uShadowTexel - 0.5; vec2 f = fract(tc); vec2 b = (floor(tc)+0.5)*uShadowTexel; float wsum = 0.0;\n"
    "    for (int y = -1; y <= 2; y++) for (int x = -1; x <= 2; x++) {\n"
    "      float wx = (x == -1) ? (1.0-f.x)*0.5 : (x == 2) ? f.x*0.5 : 0.5 + ((x==0)? (1.0-f.x):f.x)*0.5; \n"
    "      float wy = (y == -1) ? (1.0-f.y)*0.5 : (y == 2) ? f.y*0.5 : 0.5 + ((y==0)? (1.0-f.y):f.y)*0.5; \n"
    "      vec2 t = clamp(b + vec2(x,y)*uShadowTexel, r.xy, r.xy+r.zw); float w = wx*wy;\n"
    "      sh += w*((s.z <= texture(uShadowMap, t).r) ? 1.0 : 0.0); wsum += w; }\n"
    "    sh /= wsum; }\n"
    "  float fade = clamp((z - uShadowDistance*0.9)/(uShadowDistance*0.1), 0.0, 1.0);\n"
    "  return mix(mix(1.0, sh, uShadowStrength), 1.0, fade); }\n"
    "vec3 applyFog(vec3 c){ if (uFog == 0) return c; float d = length(vWorld-uCamPos); float f;\n"
    "  if (uFogMode == 1) f = (uFogParams.z - d)/(uFogParams.z-uFogParams.y); else if (uFogMode == 2) f = exp2(-uFogParams.x*d*1.442695); else { float e = uFogParams.x*d; f = exp2(-e*e*1.442695); }\n"
    "  return mix(uFogColor, c, clamp(f,0.0,1.0)); }\n"
    "const float PI = 3.14159265359;\n"
    "void main(){\n"
    "  vec2 uv = vUV*uMainST.xy + uMainST.zw; vec4 tex = texture(uMainTex, uv);\n"
    "  vec3 N = normalize(vNormal); vec3 L = normalize(uLightDir);\n"
    "  float atten = sampleShadow(vWorld);\n"
    "  vec3 col; float alpha = 1.0;\n"
    "  if (uShader == 0) {\n" /* Mobile/Diffuse: Lambert, albedo = _MainTex.rgb */
    "    vec3 albedo = tex.rgb; float diff = max(0.0, dot(N, L));\n"
    "    col = albedo*uLightColor*(diff*atten) + albedo*uAmbient;\n"
    "  } else if (uShader == 1) {\n" /* Legacy Cutout Diffuse */
    "    vec4 c = tex*uColor; if (c.a - uCutoff < 0.0) discard; float diff = max(0.0, dot(N, L));\n"
    "    col = c.rgb*uLightColor*(diff*atten) + c.rgb*uAmbient; alpha = c.a;\n"
    "  } else {\n" /* Standard, metallic workflow, BRDF1_Unity_PBS in gamma space */
    "    vec3 albedo = tex.rgb*uColor.rgb;\n"
    "    if (uNormalMap != 0) { vec3 tn = texture(uBumpTex, vUV*uBumpST.xy + uBumpST.zw).xyz*2.0-1.0; tn.xy *= uBumpScale; tn = normalize(tn);\n"
    "      vec3 T = normalize(vTan.xyz - N*dot(N, vTan.xyz)); vec3 B = cross(N, T)*vTan.w; N = normalize(T*tn.x + B*tn.y + N*tn.z); }\n"
    "    float metallic, smoothness;\n"
    "    if (uMetalGloss != 0) { vec4 mg = texture(uMetalTex, uv); metallic = mg.r; smoothness = (uSmoothChannel == 1 ? tex.a : mg.a)*uGlossMapScale; }\n"
    "    else { metallic = uMetallic; smoothness = (uSmoothChannel == 1 ? tex.a*uGlossMapScale : uGlossiness); }\n"
    "    vec4 dielectric = vec4(0.220916301, 0.220916301, 0.220916301, 1.0 - 0.220916301);\n"
    "    vec3 specColor = mix(dielectric.rgb, albedo, metallic); float oneMinusRefl = dielectric.a - metallic*dielectric.a;\n"
    "    vec3 diffColor = albedo*oneMinusRefl;\n"
    "    vec3 V = normalize(uCamPos - vWorld); float pr = 1.0 - smoothness; float rough = max(pr*pr, 0.002);\n"
    "    vec3 H = normalize(L + V); float nv = abs(dot(N, V)); float nl = clamp(dot(N, L), 0.0, 1.0); float nh = clamp(dot(N, H), 0.0, 1.0);\n"
    "    float lh = clamp(dot(L, H), 0.0, 1.0);\n"
    "    float fd90 = 0.5 + 2.0*lh*lh*pr; float lightScatter = 1.0 + (fd90-1.0)*pow(1.0-nl, 5.0); float viewScatter = 1.0 + (fd90-1.0)*pow(1.0-nv, 5.0);\n"
    "    float diffuseTerm = lightScatter*viewScatter*nl;\n"
    "    float lambdaV = nl*(nv*(1.0-rough)+rough); float lambdaL = nv*(nl*(1.0-rough)+rough); float Vis = 0.5/(lambdaV+lambdaL+1e-5);\n"
    "    float a2 = rough*rough; float d = (nh*a2 - nh)*nh + 1.0; float D = (1.0/PI)*a2/(d*d + 1e-7);\n"
    "    float spec = Vis*D*PI; spec = sqrt(max(1e-4, spec)); spec = max(0.0, spec*nl); if (uSpecHighlights == 0) spec = 0.0;\n"
    "    float surfaceReduction = 1.0 - 0.28*rough*pr; float grazing = clamp(smoothness + (1.0-oneMinusRefl), 0.0, 1.0);\n"
    "    vec3 R = reflect(-V, N); vec3 Ru = vec3(R.x, R.y, -R.z); float lod = pr*(1.7-0.7*pr)*6.0;\n"
    "    vec3 giSpec = (uGlossyRefl != 0) ? skyColor(Ru, lod) : uAmbient;\n"
    "    vec3 lightCol = uLightColor*atten;\n"
    "    vec3 F = specColor + (1.0-specColor)*pow(1.0-lh, 5.0); vec3 FL = mix(specColor, vec3(grazing), pow(1.0-nv, 5.0));\n"
    "    col = diffColor*(uAmbient + lightCol*diffuseTerm) + spec*lightCol*F + surfaceReduction*giSpec*FL + uEmission;\n"
    "  }\n"
    "  fragColor = vec4(applyFog(col), alpha); }\n";

static const char *VS_DEPTH =
    "#version 330\n"
    "layout(location=0) in vec3 aPos; layout(location=1) in vec2 aUV; layout(location=2) in vec3 aNrm;\n"
    "uniform mat4 uModel; uniform mat4 uNormalMat; uniform mat4 uLightVP; uniform vec3 uLightDir; uniform vec2 uBias;\n"
    "out vec2 vUV;\n"
    /* UnityClipSpaceShadowCasterPos (normal bias) + UnityApplyLinearShadowBias (depth bias) */
    "void main(){ vec3 w = (uModel*vec4(aPos,1.0)).xyz; vec3 n = normalize(mat3(uNormalMat)*aNrm);\n"
    "  float c = dot(n, normalize(uLightDir)); float s = sqrt(max(0.0, 1.0-c*c)); w -= n*(uBias.y*s);\n"
    "  w -= normalize(uLightDir)*uBias.x; vUV = aUV; gl_Position = uLightVP*vec4(w,1.0); }\n";

static const char *FS_DEPTH =
    "#version 330\n"
    "in vec2 vUV; out vec4 fragColor; uniform int uCutout; uniform sampler2D uMainTex; uniform vec4 uMainST; uniform vec4 uColor; uniform float uCutoff;\n"
    "void main(){ if (uCutout != 0) { float a = texture(uMainTex, vUV*uMainST.xy+uMainST.zw).a*uColor.a; if (a - uCutoff < 0.0) discard; } fragColor = vec4(1.0); }\n";

static const char *VS_SKY =
    "#version 330\n"
    "layout(location=0) in vec3 aPos; uniform mat4 uViewProjNoTrans; out vec3 vDir;\n"
    "void main(){ vDir = vec3(aPos.x, aPos.y, -aPos.z); vec4 p = uViewProjNoTrans*vec4(aPos,1.0); gl_Position = vec4(p.xy, p.w*0.99999, p.w); }\n";

static const char *FS_SKY =
    "#version 330\n"
    "in vec3 vDir; out vec4 fragColor; uniform sampler2D uSky0; uniform sampler2D uSky1; uniform sampler2D uSky2; uniform sampler2D uSky3; uniform sampler2D uSky4; uniform sampler2D uSky5; uniform vec3 uSkyTint;\n"
    "vec3 skyColor(vec3 d){ vec3 a = abs(d); vec2 uv; vec3 c;\n"
    "  if (a.z >= a.x && a.z >= a.y) { if (d.z > 0.0) { uv = vec2(d.x, d.y)/a.z; c = texture(uSky0, uv*0.5+0.5).rgb; } else { uv = vec2(-d.x, d.y)/a.z; c = texture(uSky1, uv*0.5+0.5).rgb; } }\n"
    "  else if (a.x >= a.y) { if (d.x > 0.0) { uv = vec2(-d.z, d.y)/a.x; c = texture(uSky2, uv*0.5+0.5).rgb; } else { uv = vec2(d.z, d.y)/a.x; c = texture(uSky3, uv*0.5+0.5).rgb; } }\n"
    "  else { if (d.y > 0.0) { uv = vec2(d.x, -d.z)/a.y; c = texture(uSky4, uv*0.5+0.5).rgb; } else { uv = vec2(d.x, d.z)/a.y; c = texture(uSky5, uv*0.5+0.5).rgb; } }\n"
    "  return c*uSkyTint; }\n"
    "void main(){ fragColor = vec4(skyColor(normalize(vDir)), 1.0); }\n";

/* ---------------------------------------------------------------- state */
enum { MAIN_LOCS = 96 }; /* >= L_COUNT */
struct CL_Renderer_ {
    CL_Assets *assets;
    CL_Quality quality;
    Shader main, depth, sky;
    int loc[MAIN_LOCS];
    unsigned int shadow_fbo, shadow_tex;
    int shadow_w, shadow_h;
    unsigned int sky_vao, sky_vbo, sky_ebo;
    Matrix cascade_vp[4];
    float cascade_rect[16];
    float cascade_far[4];
    float cascade_radius[4];
    Vector3 light_dir_gl; /* direction towards the light, GL space */
    Vector3 light_color;
    Vector3 ambient;
    Vector3 sky_tint;
};

enum {
    L_MODEL, L_VIEWPROJ, L_NORMALMAT, L_TANSIGN, L_SHADER, L_COLOR, L_MAINTEX, L_MAINST, L_CUTOFF, L_BUMPTEX, L_BUMPST,
    L_BUMPSCALE, L_NORMALMAP, L_METALTEX, L_METALGLOSS, L_METALLIC, L_GLOSS, L_GLOSSMAPSCALE, L_SMOOTHCH, L_EMISSION,
    L_SPECHL, L_GLOSSYREFL, L_LIGHTDIR, L_LIGHTCOLOR, L_AMBIENT, L_CAMPOS, L_FOG, L_FOGMODE, L_FOGCOLOR, L_FOGPARAMS,
    L_SHADOWMAP, L_SHADOWS, L_RECEIVE, L_SHADOWSTRENGTH, L_CASCADES, L_CASCADEVP, L_CASCADERECT, L_CASCADEFAR,
    L_SHADOWTEXEL, L_SHADOWDIST, L_CAMFWD, L_SKY0, L_SKYTINT = L_SKY0 + 6,
    /* depth shader */
    D_MODEL, D_NORMALMAT, D_LIGHTVP, D_LIGHTDIR, D_BIAS, D_CUTOUT, D_MAINTEX, D_MAINST, D_COLOR, D_CUTOFF,
    /* sky shader */
    S_VP, S_SKY0, S_SKYTINT = S_SKY0 + 6,
    L_COUNT
};

static void setup_locations(CL_Renderer_t *r)
{
    static const char *mn[] = {"uModel", "uViewProj", "uNormalMat", "uTanSign", "uShader", "uColor", "uMainTex", "uMainST",
        "uCutoff", "uBumpTex", "uBumpST", "uBumpScale", "uNormalMap", "uMetalTex", "uMetalGloss", "uMetallic", "uGlossiness",
        "uGlossMapScale", "uSmoothChannel", "uEmission", "uSpecHighlights", "uGlossyRefl", "uLightDir", "uLightColor",
        "uAmbient", "uCamPos", "uFog", "uFogMode", "uFogColor", "uFogParams", "uShadowMap", "uShadows", "uReceive",
        "uShadowStrength", "uCascades", "uCascadeVP", "uCascadeRect", "uCascadeFar", "uShadowTexel", "uShadowDistance",
        "uCamFwd", "uSky0", "uSky1", "uSky2", "uSky3", "uSky4", "uSky5", "uSkyTint"};
    for (int i = 0; i <= L_SKYTINT; i++) r->loc[i] = GetShaderLocation(r->main, mn[i]);
    static const char *dn[] = {"uModel", "uNormalMat", "uLightVP", "uLightDir", "uBias", "uCutout", "uMainTex", "uMainST", "uColor", "uCutoff"};
    for (int i = 0; i < 10; i++) r->loc[D_MODEL + i] = GetShaderLocation(r->depth, dn[i]);
    static const char *sn[] = {"uViewProjNoTrans", "uSky0", "uSky1", "uSky2", "uSky3", "uSky4", "uSky5", "uSkyTint"};
    for (int i = 0; i < 8; i++) r->loc[S_VP + i] = GetShaderLocation(r->sky, sn[i]);
}

static void create_shadow_atlas(CL_Renderer_t *r)
{
    if (r->shadow_fbo) { rlUnloadFramebuffer(r->shadow_fbo); r->shadow_fbo = 0; }
    if (!r->quality.shadows) return;
    /* Unity packs the cascades in one atlas: 1 cascade = full size, 2 = two halves side by side, 4 = 2x2 */
    r->shadow_w = r->quality.shadow_map_size;
    r->shadow_h = r->quality.shadow_cascades == 2 ? r->quality.shadow_map_size / 2 : r->quality.shadow_map_size;
    r->shadow_fbo = rlLoadFramebuffer();
    r->shadow_tex = rlLoadTextureDepth(r->shadow_w, r->shadow_h, false);
    rlFramebufferAttach(r->shadow_fbo, r->shadow_tex, RL_ATTACHMENT_DEPTH, RL_ATTACHMENT_TEXTURE2D, 0);
    rlTextureParameters(r->shadow_tex, RL_TEXTURE_WRAP_S, RL_TEXTURE_WRAP_CLAMP);
    rlTextureParameters(r->shadow_tex, RL_TEXTURE_WRAP_T, RL_TEXTURE_WRAP_CLAMP);
}

static void create_sky_cube(CL_Renderer_t *r)
{
    static const float v[] = {-1, -1, -1, 1, -1, -1, 1, 1, -1, -1, 1, -1, -1, -1, 1, 1, -1, 1, 1, 1, 1, -1, 1, 1};
    static const unsigned short idx[] = {0, 2, 1, 0, 3, 2, 4, 5, 6, 4, 6, 7, 0, 1, 5, 0, 5, 4, 3, 6, 2, 3, 7, 6, 0, 4, 7, 0, 7, 3, 1, 2, 6, 1, 6, 5};
    r->sky_vao = rlLoadVertexArray();
    rlEnableVertexArray(r->sky_vao);
    r->sky_vbo = rlLoadVertexBuffer(v, sizeof(v), false);
    rlSetVertexAttribute(0, 3, RL_FLOAT, false, 0, 0);
    rlEnableVertexAttribute(0);
    r->sky_ebo = rlLoadVertexBufferElement(idx, sizeof(idx), false);
    rlDisableVertexArray();
}

CL_Renderer_t *cl_render_create(CL_Assets *assets, CL_Quality quality)
{
    CL_Renderer_t *r = calloc(1, sizeof(*r));
    r->assets = assets;
    r->quality = quality;
    r->main = LoadShaderFromMemory(VS_MAIN, FS_MAIN);
    r->depth = LoadShaderFromMemory(VS_DEPTH, FS_DEPTH);
    r->sky = LoadShaderFromMemory(VS_SKY, FS_SKY);
    setup_locations(r);
    create_shadow_atlas(r);
    create_sky_cube(r);

    const CL_SceneSettings *s = &cl_settings;
    Quaternion lq = {s->light_rotation[0], s->light_rotation[1], s->light_rotation[2], s->light_rotation[3]};
    Vector3 fwd = cl_quat_rotate(lq, (Vector3){0, 0, 1}); /* Unity light shines along its +Z */
    r->light_dir_gl = cl_u2gl(Vector3Negate(fwd));
    /* _LightColor0 = color * intensity (gamma space, no conversion) */
    r->light_color = (Vector3){s->light_color[0] * s->light_intensity, s->light_color[1] * s->light_intensity,
                               s->light_color[2] * s->light_intensity};
    /* Flat ambient: SH holds the ambient color; in gamma space ShadeSH9 returns it unchanged */
    if (s->ambient_mode == 3)
        r->ambient = (Vector3){s->ambient_sky[0], s->ambient_sky[1], s->ambient_sky[2]};
    else /* trilight average (used only if another map changes the mode) */
        r->ambient = (Vector3){(s->ambient_sky[0] + s->ambient_equator[0] + s->ambient_ground[0]) / 3.0f,
                               (s->ambient_sky[1] + s->ambient_equator[1] + s->ambient_ground[1]) / 3.0f,
                               (s->ambient_sky[2] + s->ambient_equator[2] + s->ambient_ground[2]) / 3.0f};
    const CL_Material *sky = &cl_materials[s->skybox_material];
    /* Skybox/6 Sided: tex * _Tint * unity_ColorSpaceDouble(2 in gamma) * _Exposure */
    r->sky_tint = (Vector3){sky->tint[0] * 2.0f * sky->exposure, sky->tint[1] * 2.0f * sky->exposure,
                            sky->tint[2] * 2.0f * sky->exposure};
    /* Unity draws each skybox face as its own quad, so face edges never wrap around */
    for (int i = 0; i < 6; i++) {
        if (sky->sky_tex[i] < 0) continue;
        rlTextureParameters(assets->textures[sky->sky_tex[i]], RL_TEXTURE_WRAP_S, RL_TEXTURE_WRAP_CLAMP);
        rlTextureParameters(assets->textures[sky->sky_tex[i]], RL_TEXTURE_WRAP_T, RL_TEXTURE_WRAP_CLAMP);
    }
    return r;
}

void cl_render_set_quality(CL_Renderer_t *r, CL_Quality q)
{
    r->quality = q;
    create_shadow_atlas(r);
}

void cl_render_destroy(CL_Renderer_t *r)
{
    UnloadShader(r->main);
    UnloadShader(r->depth);
    UnloadShader(r->sky);
    if (r->shadow_fbo) rlUnloadFramebuffer(r->shadow_fbo);
    rlUnloadVertexArray(r->sky_vao);
    rlUnloadVertexBuffer(r->sky_vbo);
    rlUnloadVertexBuffer(r->sky_ebo);
    free(r);
}

/* ---------------------------------------------------------------- helpers */
static Matrix mat_from_cols(const float *w)
{
    Matrix m = {w[0], w[4], w[8], w[12], w[1], w[5], w[9], w[13], w[2], w[6], w[10], w[14], w[3], w[7], w[11], w[15]};
    return m;
}

/* GL model matrix = F * M_unity, F = diag(1,1,-1) */
static Matrix gl_model(const CL_Renderer *rd)
{
    if (rd->static_batched) return MatrixScale(1, 1, -1);
    return MatrixMultiply(mat_from_cols(rd->world), MatrixScale(1, 1, -1));
}

static Matrix normal_matrix(Matrix m)
{
    return MatrixTranspose(MatrixInvert(m));
}

static float det3(Matrix m)
{
    return m.m0 * (m.m5 * m.m10 - m.m9 * m.m6) - m.m4 * (m.m1 * m.m10 - m.m9 * m.m2) + m.m8 * (m.m1 * m.m6 - m.m5 * m.m2);
}

static void bind_tex(int slot, unsigned int id, int loc)
{
    rlActiveTextureSlot(slot);
    rlEnableTexture(id);
    rlSetUniform(loc, &slot, RL_SHADER_UNIFORM_INT, 1);
}

static unsigned int tex_or(CL_Renderer_t *r, int idx, unsigned int fallback)
{
    return idx >= 0 ? r->assets->textures[idx] : fallback;
}

/* Material index for submesh i of a renderer: extra materials re-draw the last submesh (Unity behaviour). */
typedef void (*draw_fn)(CL_Renderer_t *, const CL_Renderer *, const CL_Material *, int submesh, Matrix model);

static void for_each_draw(CL_Renderer_t *r, int pass_queue_min, int pass_queue_max, int shadow_pass, draw_fn fn)
{
    for (int i = 0; i < cl_renderer_count; i++) {
        const CL_Renderer *rd = &cl_renderers[i];
        if (shadow_pass && !rd->cast_shadows) continue;
        Matrix model = gl_model(rd);
        int n = rd->material_count > rd->submesh_count ? rd->material_count : rd->submesh_count;
        for (int k = 0; k < n; k++) {
            int mi = rd->materials[k < rd->material_count ? k : rd->material_count - 1];
            if (mi < 0) continue;
            const CL_Material *m = &cl_materials[mi];
            int queue = m->render_queue >= 0 ? m->render_queue : (m->shader == CL_SHADER_LEGACY_CUTOUT_DIFFUSE ? 2450 : 2000);
            if (queue < pass_queue_min || queue > pass_queue_max) continue;
            int sub = rd->first_submesh + (k < rd->submesh_count ? k : rd->submesh_count - 1);
            fn(r, rd, m, sub, model);
        }
    }
}

static void draw_submesh(CL_Renderer_t *r, const CL_Renderer *rd, int sub)
{
    CL_GpuMesh *gm = &r->assets->meshes[rd->mesh];
    if (sub >= gm->submesh_count) return;
    rlEnableVertexArray(gm->vao);
    rlEnableVertexBufferElement(gm->ebo);
    rlDrawVertexArrayElements(gm->submesh_first[sub], gm->submesh_count_idx[sub], 0);
    rlDisableVertexArray();
}


static void draw_depth(CL_Renderer_t *r, const CL_Renderer *rd, const CL_Material *m, int sub, Matrix model)
{
    rlSetUniformMatrix(r->loc[D_MODEL], model);
    rlSetUniformMatrix(r->loc[D_NORMALMAT], normal_matrix(model));
    int cut = m->shader == CL_SHADER_LEGACY_CUTOUT_DIFFUSE;
    rlSetUniform(r->loc[D_CUTOUT], &cut, RL_SHADER_UNIFORM_INT, 1);
    if (cut) {
        bind_tex(0, tex_or(r, m->main_tex, r->assets->white_texture), r->loc[D_MAINTEX]);
        rlSetUniform(r->loc[D_MAINST], m->main_st, RL_SHADER_UNIFORM_VEC4, 1);
        rlSetUniform(r->loc[D_COLOR], m->color, RL_SHADER_UNIFORM_VEC4, 1);
        rlSetUniform(r->loc[D_CUTOFF], &m->cutoff, RL_SHADER_UNIFORM_FLOAT, 1);
    }
    draw_submesh(r, rd, sub);
}

static void draw_main(CL_Renderer_t *r, const CL_Renderer *rd, const CL_Material *m, int sub, Matrix model)
{
    CL_Assets *a = r->assets;
    rlSetUniformMatrix(r->loc[L_MODEL], model);
    rlSetUniformMatrix(r->loc[L_NORMALMAT], normal_matrix(model));
    /* binormal sign: mirroring z flips handedness once, a negative-scale object flips it again */
    float tan_sign = det3(model) < 0.0f ? 1.0f : -1.0f;
    if (rd->static_batched) tan_sign = -1.0f;
    rlSetUniform(r->loc[L_TANSIGN], &tan_sign, RL_SHADER_UNIFORM_FLOAT, 1);
    int shader = m->shader == CL_SHADER_MOBILE_DIFFUSE ? 0 : m->shader == CL_SHADER_LEGACY_CUTOUT_DIFFUSE ? 1 : 2;
    rlSetUniform(r->loc[L_SHADER], &shader, RL_SHADER_UNIFORM_INT, 1);
    rlSetUniform(r->loc[L_COLOR], m->color, RL_SHADER_UNIFORM_VEC4, 1);
    rlSetUniform(r->loc[L_MAINST], m->main_st, RL_SHADER_UNIFORM_VEC4, 1);
    rlSetUniform(r->loc[L_CUTOFF], &m->cutoff, RL_SHADER_UNIFORM_FLOAT, 1);
    bind_tex(0, tex_or(r, m->main_tex, a->white_texture), r->loc[L_MAINTEX]);
    int nmap = m->normalmap && m->bump_tex >= 0;
    rlSetUniform(r->loc[L_NORMALMAP], &nmap, RL_SHADER_UNIFORM_INT, 1);
    bind_tex(1, tex_or(r, m->bump_tex, a->flat_normal_texture), r->loc[L_BUMPTEX]);
    rlSetUniform(r->loc[L_BUMPST], m->bump_st, RL_SHADER_UNIFORM_VEC4, 1);
    rlSetUniform(r->loc[L_BUMPSCALE], &m->bump_scale, RL_SHADER_UNIFORM_FLOAT, 1);
    int mg = m->metalgloss && m->metal_tex >= 0;
    rlSetUniform(r->loc[L_METALGLOSS], &mg, RL_SHADER_UNIFORM_INT, 1);
    bind_tex(2, tex_or(r, m->metal_tex, a->white_texture), r->loc[L_METALTEX]);
    rlSetUniform(r->loc[L_METALLIC], &m->metallic, RL_SHADER_UNIFORM_FLOAT, 1);
    rlSetUniform(r->loc[L_GLOSS], &m->glossiness, RL_SHADER_UNIFORM_FLOAT, 1);
    rlSetUniform(r->loc[L_GLOSSMAPSCALE], &m->gloss_map_scale, RL_SHADER_UNIFORM_FLOAT, 1);
    rlSetUniform(r->loc[L_SMOOTHCH], &m->smoothness_channel, RL_SHADER_UNIFORM_INT, 1);
    rlSetUniform(r->loc[L_EMISSION], m->emission, RL_SHADER_UNIFORM_VEC3, 1);
    rlSetUniform(r->loc[L_SPECHL], &m->specular_highlights, RL_SHADER_UNIFORM_INT, 1);
    rlSetUniform(r->loc[L_GLOSSYREFL], &m->glossy_reflections, RL_SHADER_UNIFORM_INT, 1);
    int recv = rd->receive_shadows;
    rlSetUniform(r->loc[L_RECEIVE], &recv, RL_SHADER_UNIFORM_INT, 1);
    draw_submesh(r, rd, sub);
}

static void bind_sky(CL_Renderer_t *r, int first_slot, int loc_first, int loc_tint)
{
    const CL_Material *sky = &cl_materials[cl_settings.skybox_material];
    for (int i = 0; i < 6; i++) {
        unsigned int id = tex_or(r, sky->sky_tex[i], r->assets->white_texture);
        bind_tex(first_slot + i, id, r->loc[loc_first + i]);
    }
    rlSetUniform(r->loc[loc_tint], &r->sky_tint, RL_SHADER_UNIFORM_VEC3, 1);
}

/* Unity "Stable Fit" cascades: a bounding sphere per frustum slice, ortho projection
 * snapped to whole shadow-map texels so shadows do not swim when the camera moves. */
static void compute_cascades(CL_Renderer_t *r, Vector3 eye, Vector3 fwd, Vector3 up, float fov, float aspect, float near_plane)
{
    int n = r->quality.shadow_cascades;
    float dist = r->quality.shadow_distance;
    float splits[5];
    splits[0] = near_plane;
    if (n == 1) splits[1] = dist;
    else if (n == 2) { splits[1] = dist * 0.333f; splits[2] = dist; }
    else { splits[1] = dist * 0.067f; splits[2] = dist * 0.2f; splits[3] = dist * 0.467f; splits[4] = dist; }
    Vector3 right = Vector3Normalize(Vector3CrossProduct(fwd, up));
    Vector3 cup = Vector3CrossProduct(right, fwd);
    float th = tanf(fov * 0.5f * DEG2RAD), tw = th * aspect;
    int tile = n == 1 ? r->shadow_w : r->shadow_w / 2;
    Vector3 L = Vector3Normalize(r->light_dir_gl);
    Vector3 lup = fabsf(L.y) > 0.99f ? (Vector3){0, 0, 1} : (Vector3){0, 1, 0};
    for (int c = 0; c < n; c++) {
        float zn = splits[c], zf = splits[c + 1];
        Vector3 pts[8];
        for (int k = 0; k < 8; k++) {
            float z = (k & 4) ? zf : zn;
            float sx = (k & 1) ? 1.0f : -1.0f, sy = (k & 2) ? 1.0f : -1.0f;
            pts[k] = Vector3Add(eye, Vector3Add(Vector3Scale(fwd, z),
                     Vector3Add(Vector3Scale(right, sx * tw * z), Vector3Scale(cup, sy * th * z))));
        }
        Vector3 center = {0};
        for (int k = 0; k < 8; k++) center = Vector3Add(center, Vector3Scale(pts[k], 1.0f / 8.0f));
        float radius = 0.0f;
        for (int k = 0; k < 8; k++) radius = fmaxf(radius, Vector3Distance(center, pts[k]));
        radius = ceilf(radius * 16.0f) / 16.0f;
        Matrix view = MatrixLookAt(Vector3Add(center, Vector3Scale(L, 500.0f)), center, lup);
        /* snap the sphere centre to texel increments in light space */
        float texel = 2.0f * radius / (float)tile;
        Vector3 cl = Vector3Transform(center, view);
        float ox = floorf(cl.x / texel) * texel - cl.x, oy = floorf(cl.y / texel) * texel - cl.y;
        Matrix proj = MatrixOrtho(-radius - ox, radius - ox, -radius - oy, radius - oy, 1.0f, 1000.0f);
        r->cascade_vp[c] = MatrixMultiply(view, proj);
        r->cascade_far[c] = zf;
        r->cascade_radius[c] = radius;
        float *rc = &r->cascade_rect[c * 4];
        if (n == 1) { rc[0] = 0; rc[1] = 0; rc[2] = 1; rc[3] = 1; }
        else { rc[0] = (float)(c % 2) * 0.5f; rc[1] = n == 2 ? 0.0f : (float)(c / 2) * 0.5f; rc[2] = 0.5f; rc[3] = n == 2 ? 1.0f : 0.5f; }
    }
}

void cl_render_frame(CL_Renderer_t *r, const CL_Camera *cam, int width, int height)
{
    rlDrawRenderBatchActive();
    const CL_SceneSettings *s = &cl_settings;
    float aspect = (float)width / (float)height;
    Vector3 eye = cl_u2gl(cam->position);
    Vector3 fwd = cl_u2gl(cl_quat_rotate(cam->rotation, (Vector3){0, 0, 1}));
    Vector3 up = cl_u2gl(cl_quat_rotate(cam->rotation, (Vector3){0, 1, 0}));
    Matrix view = MatrixLookAt(eye, Vector3Add(eye, fwd), up);
    Matrix proj = MatrixPerspective(cam->fov * DEG2RAD, aspect, cam->near_plane, cam->far_plane);
    Matrix vp = MatrixMultiply(view, proj);

    rlEnableDepthTest();
    rlEnableBackfaceCulling();
    /* Unity's front faces are clockwise on screen; mirroring z and rendering with a right-handed
     * camera keeps the picture (and thus the on-screen winding) identical, so the clockwise
     * faces are still the front ones: cull GL's "front" (counter-clockwise) faces. */
    rlSetCullFace(RL_CULL_FACE_FRONT);

    /* ---- shadow caster pass */
    if (r->quality.shadows && s->light_shadows) {
        compute_cascades(r, eye, fwd, up, cam->fov, aspect, cam->near_plane);
        rlEnableFramebuffer(r->shadow_fbo);
        rlViewport(0, 0, r->shadow_w, r->shadow_h);
        rlClearScreenBuffers();
        rlEnableShader(r->depth.id);
        Vector3 L = Vector3Normalize(r->light_dir_gl);
        rlSetUniform(r->loc[D_LIGHTDIR], &L, RL_SHADER_UNIFORM_VEC3, 1);
        int n = r->quality.shadow_cascades;
        for (int c = 0; c < n; c++) {
            float *rc = &r->cascade_rect[c * 4];
            int tw = (int)(rc[2] * r->shadow_w), th = (int)(rc[3] * r->shadow_h);
            rlViewport((int)(rc[0] * r->shadow_w), (int)(rc[1] * r->shadow_h), tw, th);
            float texel = 2.0f * r->cascade_radius[c] / (float)tw; /* world size of one shadow texel */
            /* Unity scales Light.shadowBias / shadowNormalBias by the cascade's world texel size;
             * the factors below reproduce its acne-free result for this map's 0.05 / 0.4 settings */
            float bias[2] = {s->shadow_bias * texel * 20.0f, s->shadow_normal_bias * texel * 2.5f};
            rlSetUniform(r->loc[D_BIAS], bias, RL_SHADER_UNIFORM_VEC2, 1);
            rlSetUniformMatrix(r->loc[D_LIGHTVP], r->cascade_vp[c]);
            for_each_draw(r, 0, 2999, 1, draw_depth);
        }
        rlDisableFramebuffer();
    }
    rlViewport(0, 0, width, height);

    /* ---- skybox (clear flags = Skybox) */
    rlDisableDepthTest();
    rlDisableDepthMask();
    rlDisableBackfaceCulling();
    rlEnableShader(r->sky.id);
    Matrix view_rot = view;
    view_rot.m12 = view_rot.m13 = view_rot.m14 = 0.0f;
    rlSetUniformMatrix(r->loc[S_VP], MatrixMultiply(view_rot, proj));
    bind_sky(r, 0, S_SKY0, S_SKYTINT);
    rlEnableVertexArray(r->sky_vao);
    rlEnableVertexBufferElement(r->sky_ebo);
    rlDrawVertexArrayElements(0, 36, 0);
    rlDisableVertexArray();
    rlEnableDepthTest();
    rlEnableDepthMask();
    rlEnableBackfaceCulling();
    rlSetCullFace(RL_CULL_FACE_FRONT);

    /* ---- forward base pass */
    rlEnableShader(r->main.id);
    rlSetUniformMatrix(r->loc[L_VIEWPROJ], vp);
    rlSetUniform(r->loc[L_LIGHTDIR], &r->light_dir_gl, RL_SHADER_UNIFORM_VEC3, 1);
    rlSetUniform(r->loc[L_LIGHTCOLOR], &r->light_color, RL_SHADER_UNIFORM_VEC3, 1);
    rlSetUniform(r->loc[L_AMBIENT], &r->ambient, RL_SHADER_UNIFORM_VEC3, 1);
    rlSetUniform(r->loc[L_CAMPOS], &eye, RL_SHADER_UNIFORM_VEC3, 1);
    rlSetUniform(r->loc[L_CAMFWD], &fwd, RL_SHADER_UNIFORM_VEC3, 1);
    rlSetUniform(r->loc[L_FOG], &s->fog, RL_SHADER_UNIFORM_INT, 1);
    rlSetUniform(r->loc[L_FOGMODE], &s->fog_mode, RL_SHADER_UNIFORM_INT, 1);
    rlSetUniform(r->loc[L_FOGCOLOR], s->fog_color, RL_SHADER_UNIFORM_VEC3, 1);
    float fogp[3] = {s->fog_density, s->fog_start, s->fog_end};
    rlSetUniform(r->loc[L_FOGPARAMS], fogp, RL_SHADER_UNIFORM_VEC3, 1);
    int shadows = (r->quality.shadows && s->light_shadows) ? (r->quality.shadows == 2 && s->light_shadows == 2 ? 2 : 1) : 0;
    rlSetUniform(r->loc[L_SHADOWS], &shadows, RL_SHADER_UNIFORM_INT, 1);
    rlSetUniform(r->loc[L_SHADOWSTRENGTH], &s->shadow_strength, RL_SHADER_UNIFORM_FLOAT, 1);
    if (shadows) {
        rlSetUniform(r->loc[L_CASCADES], &r->quality.shadow_cascades, RL_SHADER_UNIFORM_INT, 1);
        for (int c = 0; c < r->quality.shadow_cascades; c++)
            rlSetUniformMatrix(r->loc[L_CASCADEVP] + c, r->cascade_vp[c]);
        rlSetUniform(r->loc[L_CASCADERECT], r->cascade_rect, RL_SHADER_UNIFORM_VEC4, 4);
        rlSetUniform(r->loc[L_CASCADEFAR], r->cascade_far, RL_SHADER_UNIFORM_FLOAT, 4);
        float texel[2] = {1.0f / (float)r->shadow_w, 1.0f / (float)r->shadow_h};
        rlSetUniform(r->loc[L_SHADOWTEXEL], texel, RL_SHADER_UNIFORM_VEC2, 1);
        rlSetUniform(r->loc[L_SHADOWDIST], &r->quality.shadow_distance, RL_SHADER_UNIFORM_FLOAT, 1);
        bind_tex(3, r->shadow_tex, r->loc[L_SHADOWMAP]);
    }
    bind_sky(r, 4, L_SKY0, L_SKYTINT);
    for_each_draw(r, 0, 2449, 0, draw_main);     /* Geometry queue */
    for_each_draw(r, 2450, 2999, 0, draw_main);  /* AlphaTest queue */
    for (int i = 0; i < 10; i++) { rlActiveTextureSlot(i); rlDisableTexture(); }
    rlActiveTextureSlot(0);
    rlDisableShader();
    rlSetCullFace(RL_CULL_FACE_BACK); /* restore raylib's default for 2D/HUD drawing */
}
