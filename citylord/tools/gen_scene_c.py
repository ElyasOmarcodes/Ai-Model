"""Turn scene.json (from export_scene.py) into C source data + the runtime asset folder.

usage: gen_scene_c.py <export_dir> <map_root> <src_out_dir> <assets_out_dir>
Writes <src_out_dir>/citylord_data.c and copies meshes/textures used by <map_root>
(plus the skybox) into <assets_out_dir>.
"""
import sys, os, json, shutil

exp, root, src_out, assets_out = sys.argv[1:5]
d = json.load(open(os.path.join(exp, 'scene.json')))

SHADERS = {'Mobile/Diffuse': 'CL_SHADER_MOBILE_DIFFUSE',
           'Legacy Shaders/Transparent/Cutout/Diffuse': 'CL_SHADER_LEGACY_CUTOUT_DIFFUSE',
           'Standard': 'CL_SHADER_STANDARD',
           'Skybox/6 Sided': 'CL_SHADER_SKYBOX_6SIDED'}


def f(x):
    return repr(float(x)) + 'f'


def fl(xs):
    return '{' + ', '.join(f(x) for x in xs) + '}'


def cstr(s):
    return '"' + s.replace('\\', '\\\\').replace('"', '\\"') + '"'


def col_major(m):
    # world matrix rows -> column-major float[16] (OpenGL / raylib order)
    return [m[r][c] for c in range(4) for r in range(4)]


textures, tex_index = [], {}
meshes, mesh_index = [], {}
materials, mat_index = [], {}


def tex(info):
    if not info:
        return -1
    k = info['key']
    if k not in tex_index:
        tex_index[k] = len(textures)
        textures.append(info)
    return tex_index[k]


def mesh(k):
    if k is None:
        return -1
    if k not in mesh_index:
        mesh_index[k] = len(meshes)
        meshes.append(d['meshes'][k])
    return mesh_index[k]


def material(k):
    if k is None:
        return -1
    if k in mat_index:
        return mat_index[k]
    m = d['materials'][k]
    t = m['textures']

    def slot(name):
        s = t.get(name)
        if not s or not s['texture']:
            return -1, [1, 1, 0, 0]
        return tex(s['texture']), s['scale'] + s['offset']
    main, main_st = slot('_MainTex')
    bump, bump_st = slot('_BumpMap')
    metal, _ = slot('_MetallicGlossMap')
    occl, _ = slot('_OcclusionMap')
    emis_t, _ = slot('_EmissionMap')
    sky = [slot(n)[0] for n in ('_FrontTex', '_BackTex', '_LeftTex', '_RightTex', '_UpTex', '_DownTex')]
    fl_ = m['floats']
    co = m['colors']
    kw = m['keywords'].split()
    mat_index[k] = len(materials)
    materials.append({
        'name': m['name'], 'shader': SHADERS.get(m['shader'], 'CL_SHADER_MOBILE_DIFFUSE'), 'shader_name': m['shader'],
        'color': co.get('_Color', [1, 1, 1, 1]), 'main': main, 'main_st': main_st,
        'cutoff': fl_.get('_Cutoff', 0.5), 'bump': bump, 'bump_st': bump_st, 'bump_scale': fl_.get('_BumpScale', 1),
        'metal_tex': metal, 'metallic': fl_.get('_Metallic', 0), 'glossiness': fl_.get('_Glossiness', 0.5),
        'gloss_map_scale': fl_.get('_GlossMapScale', 1), 'smooth_channel': fl_.get('_SmoothnessTextureChannel', 0),
        'occlusion_tex': occl, 'occlusion_strength': fl_.get('_OcclusionStrength', 1),
        'emission': co.get('_EmissionColor', [0, 0, 0, 1]) if '_EMISSION' in kw and m['shader'] == 'Standard' else [0, 0, 0, 1],
        'emission_tex': emis_t,
        'normalmap_kw': '_NORMALMAP' in kw, 'metalgloss_kw': '_METALLICGLOSSMAP' in kw,
        'specular_highlights': fl_.get('_SpecularHighlights', 1), 'glossy_reflections': fl_.get('_GlossyReflections', 1),
        'render_queue': m['render_queue'], 'sky': sky, 'tint': co.get('_Tint', [0.5, 0.5, 0.5, 0.5]),
        'exposure': fl_.get('_Exposure', 1), 'rotation': fl_.get('_Rotation', 0)})
    return mat_index[k]


renderers, colliders = [], []
light = cam = None
for o in d['objects']:
    p = o['path']
    for c in o['components']:
        if c['type'] == 'Light' and light is None:
            light = (o, c)
        if c['type'] == 'Camera' and cam is None:
            cam = (o, c)
    if not (p == root or p.startswith(root + '/')) or not o['active']:
        continue
    for c in o['components']:
        if c['type'] == 'MeshRenderer' and c['enabled'] and c['mesh']:
            batched = c['static_batch_submesh_count'] > 0
            mi = mesh(c['mesh'])
            n_sub = c['static_batch_submesh_count'] if batched else len(meshes[mi]['submeshes'])
            renderers.append({'path': p, 'mesh': mi, 'first': c['static_batch_first_submesh'] if batched else 0,
                              'count': n_sub, 'batched': batched,
                              'mats': [material(m) for m in c['materials']],
                              'world': col_major(o['world_matrix']), 'cast': c['cast_shadows'],
                              'recv': c['receive_shadows'], 'layer': o['layer']})
        elif c['type'] == 'BoxCollider' and c['enabled']:
            colliders.append({'path': p, 'kind': 'CL_COLLIDER_BOX', 'world': col_major(o['world_matrix']),
                              'center': c['center'], 'size': c['size'], 'mesh': -1, 'trigger': c['is_trigger'],
                              'layer': o['layer']})
        elif c['type'] == 'MeshCollider' and c['enabled'] and c['mesh']:
            colliders.append({'path': p, 'kind': 'CL_COLLIDER_MESH', 'world': col_major(o['world_matrix']),
                              'center': [0, 0, 0], 'size': [0, 0, 0], 'mesh': mesh(c['mesh']), 'trigger': False,
                              'layer': o['layer']})

rs = d['settings']['render']
sky_mat = material(rs['skybox'])

os.makedirs(os.path.join(assets_out, 'meshes'), exist_ok=True)
os.makedirs(os.path.join(assets_out, 'textures'), exist_ok=True)
for m in meshes:
    shutil.copy(os.path.join(exp, m['file']), os.path.join(assets_out, m['file']))
for t in textures:
    shutil.copy(os.path.join(exp, t['file']), os.path.join(assets_out, t['file']))

L = []
w = L.append
w('/* Generated by tools/gen_scene_c.py from the game files - do not edit by hand.')
w(' * Scene: %s (Assets/Scenes/map3.unity), root object "%s".' % (d['scene'], root))
w(' * All values are copied bit-exact from the serialized Unity data (Unity left-handed, Y-up space). */')
w('#include "citylord_data.h"\n')
w('const CL_Texture cl_textures[%d] = {' % len(textures))
for t in textures:
    w('    {%s, %s, %d, %d, %d, %d, %d, %d, %s},' % (cstr(t['file']), cstr(t['name']), t['width'], t['height'],
      t['filter'], t['aniso'], t['wrap_u'], t['wrap_v'], f(t['mip_bias'])))
w('};\nconst int cl_texture_count = %d;\n' % len(textures))
w('const CL_MeshInfo cl_meshes[%d] = {' % len(meshes))
for m in meshes:
    w('    {%s, %s, %d, %d},' % (cstr(m['file']), cstr(m['name']), m['vertices'], len(m['submeshes'])))
w('};\nconst int cl_mesh_count = %d;\n' % len(meshes))
w('const CL_Material cl_materials[%d] = {' % len(materials))
for m in materials:
    w('    { /* %s : %s */' % (m['name'], m['shader_name']))
    w('      .name = %s, .shader = %s, .render_queue = %d,' % (cstr(m['name']), m['shader'], m['render_queue']))
    w('      .color = %s, .main_tex = %d, .main_st = %s, .cutoff = %s,' % (fl(m['color']), m['main'], fl(m['main_st']), f(m['cutoff'])))
    w('      .bump_tex = %d, .bump_st = %s, .bump_scale = %s, .normalmap = %d,' % (m['bump'], fl(m['bump_st']), f(m['bump_scale']), m['normalmap_kw']))
    w('      .metal_tex = %d, .metallic = %s, .glossiness = %s, .gloss_map_scale = %s, .metalgloss = %d, .smoothness_channel = %d,' % (
        m['metal_tex'], f(m['metallic']), f(m['glossiness']), f(m['gloss_map_scale']), m['metalgloss_kw'], int(m['smooth_channel'])))
    w('      .occlusion_tex = %d, .occlusion_strength = %s, .emission = %s, .emission_tex = %d,' % (
        m['occlusion_tex'], f(m['occlusion_strength']), fl(m['emission']), m['emission_tex']))
    w('      .specular_highlights = %d, .glossy_reflections = %d,' % (int(m['specular_highlights']), int(m['glossy_reflections'])))
    w('      .sky_tex = {%s}, .tint = %s, .exposure = %s, .rotation = %s },' % (
        ', '.join(str(x) for x in m['sky']), fl(m['tint']), f(m['exposure']), f(m['rotation'])))
w('};\nconst int cl_material_count = %d;\n' % len(materials))
w('const CL_Renderer cl_renderers[%d] = {' % len(renderers))
for r in renderers:
    mats = r['mats'] + [-1] * 8
    w('    {%s, %d, %d, %d, %d, %d, {%s}, %s, %d, %d, %d},' % (
        cstr(r['path']), r['mesh'], r['first'], r['count'], int(r['batched']), len(r['mats']),
        ', '.join(str(x) for x in mats[:8]), fl(r['world']), r['cast'], r['recv'], r['layer']))
w('};\nconst int cl_renderer_count = %d;\n' % len(renderers))
w('const CL_Collider cl_colliders[%d] = {' % len(colliders))
for c in colliders:
    w('    {%s, %s, %s, %s, %s, %d, %d, %d},' % (cstr(c['path']), c['kind'], fl(c['world']), fl(c['center']),
      fl(c['size']), c['mesh'], int(c['trigger']), c['layer']))
w('};\nconst int cl_collider_count = %d;\n' % len(colliders))
lo, lc = light
co_, cc = cam
w('const CL_SceneSettings cl_settings = {')
w('    .light_rotation = %s, /* quaternion x,y,z,w of "%s" */' % (fl(lo['local_rotation']), lo['path']))
w('    .light_color = %s, .light_intensity = %s,' % (fl(lc['color']), f(lc['intensity'])))
w('    .light_shadows = %d, .shadow_strength = %s, .shadow_bias = %s, .shadow_normal_bias = %s,' % (
    lc['shadows'], f(lc['shadow_strength']), f(lc['shadow_bias']), f(lc['shadow_normal_bias'])))
w('    .ambient_mode = %d, .ambient_sky = %s, .ambient_equator = %s, .ambient_ground = %s, .ambient_intensity = %s,' % (
    rs['ambient_mode'], fl(rs['ambient_sky']), fl(rs['ambient_equator']), fl(rs['ambient_ground']), f(rs['ambient_intensity'])))
w('    .fog = %d, .fog_mode = %d, .fog_color = %s, .fog_density = %s, .fog_start = %s, .fog_end = %s,' % (
    int(rs['fog']), rs['fog_mode'], fl(rs['fog_color']), f(rs['fog_density']), f(rs['fog_start']), f(rs['fog_end'])))
w('    .skybox_material = %d,' % sky_mat)
w('    .camera_position = %s, .camera_rotation = %s, /* "%s" */' % (fl(co_['world_matrix'][r][3] for r in range(3)), fl(co_['local_rotation']), co_['path']))
w('    .camera_fov = %s, .camera_near = %s, .camera_far = %s, .camera_background = %s,' % (
    f(cc['fov']), f(cc['near']), f(cc['far']), fl(cc['background'])))
w('};')
open(os.path.join(src_out, 'citylord_data.c'), 'w').write('\n'.join(L) + '\n')
print('textures', len(textures), 'meshes', len(meshes), 'materials', len(materials), 'renderers', len(renderers),
      'colliders', len(colliders))
