"""Export one Unity scene from the merged Data folder into a portable pack.

Output layout (out_dir):
  scene.json        full description (objects, transforms, renderers, materials, lights, settings)
  meshes/<key>.mesh binary mesh (see write_mesh)
  textures/<key>.png
Everything is kept in Unity's own (left-handed, Y-up) coordinates; the C viewer converts.
"""
import sys, os, json, struct, math
import UnityPy
from UnityPy.helpers.MeshHelper import MeshHandler

data_dir, scene_file, out_dir = sys.argv[1], sys.argv[2], sys.argv[3]
os.makedirs(out_dir + '/meshes', exist_ok=True)
os.makedirs(out_dir + '/textures', exist_ok=True)

env = UnityPy.load(data_dir)
scene = env.files[[k for k in env.files if k.endswith('/' + scene_file) or k == scene_file][0]]


def key_of(ptr_obj):
    return '%s_%d' % (os.path.basename(ptr_obj.assets_file.name).replace(' ', '_'), ptr_obj.path_id)


def v3(v): return [v.x, v.y, v.z]
def v4(v): return [v.x, v.y, v.z, v.w]
def col(c): return [c.r, c.g, c.b, c.a]


def deref(pptr):
    if pptr is None or pptr.path_id == 0:
        return None
    try:
        return pptr.deref()
    except Exception:
        return None


# ---------------- textures ----------------
tex_done = {}


def export_texture(pptr):
    ob = deref(pptr)
    if ob is None:
        return None
    k = key_of(ob)
    if k in tex_done:
        return tex_done[k]
    t = ob.read()
    info = {'key': k, 'name': t.m_Name, 'type': ob.type.name}
    if ob.type.name == 'Texture2D':
        info.update(width=t.m_Width, height=t.m_Height, format=str(t.m_TextureFormat),
                    mip_count=getattr(t, 'm_MipCount', 1),
                    filter=t.m_TextureSettings.m_FilterMode, aniso=t.m_TextureSettings.m_Aniso,
                    mip_bias=t.m_TextureSettings.m_MipBias,
                    wrap_u=t.m_TextureSettings.m_WrapU, wrap_v=t.m_TextureSettings.m_WrapV,
                    lightmap_format=getattr(t, 'm_LightmapFormat', 0),
                    color_space=getattr(t, 'm_ColorSpace', 1))
        try:
            img = t.image
            img.save('%s/textures/%s.png' % (out_dir, k))
            info['file'] = 'textures/%s.png' % k
        except Exception as e:
            info['error'] = str(e)
    tex_done[k] = info
    return info


# ---------------- materials ----------------
mat_done = {}


def export_material(pptr):
    ob = deref(pptr)
    if ob is None:
        return None
    k = key_of(ob)
    if k in mat_done:
        return k
    m = ob.read()
    sh = deref(m.m_Shader)
    shader_name = None
    if sh is not None:
        try:
            s = sh.read()
            shader_name = s.m_ParsedForm.m_Name if getattr(s, 'm_ParsedForm', None) else s.m_Name
        except Exception:
            shader_name = '?'
    sp = m.m_SavedProperties
    texs = {}
    for name, te in sp.m_TexEnvs:
        texs[name] = {'texture': export_texture(te.m_Texture), 'scale': [te.m_Scale.x, te.m_Scale.y],
                      'offset': [te.m_Offset.x, te.m_Offset.y]}
    floats = {n: v for n, v in sp.m_Floats}
    colors = {n: col(c) for n, c in sp.m_Colors}
    kw = getattr(m, 'm_ShaderKeywords', '')
    if not isinstance(kw, str):
        kw = ' '.join(kw)
    mat_done[k] = {'key': k, 'name': m.m_Name, 'shader': shader_name, 'keywords': kw,
                   'render_queue': m.m_CustomRenderQueue, 'textures': texs, 'floats': floats, 'colors': colors,
                   'tags': dict(getattr(m, 'm_StringTagMap', []) or [])}
    return k


# ---------------- meshes ----------------
mesh_done = {}


def write_mesh(ob):
    k = key_of(ob)
    if k in mesh_done:
        return k
    mesh = ob.read()
    h = MeshHandler(mesh)
    h.process()
    subs = h.get_triangles()
    n = h.m_VertexCount
    pos = h.m_Vertices or []
    nrm = h.m_Normals or []
    uv0 = h.m_UV0 or []
    uv1 = h.m_UV1 or []
    colr = h.m_Colors or []
    tan = h.m_Tangents or []
    flags = (1 if nrm else 0) | (2 if uv0 else 0) | (4 if uv1 else 0) | (8 if colr else 0) | (16 if tan else 0)
    with open('%s/meshes/%s.mesh' % (out_dir, k), 'wb') as f:
        # header: magic, vertex count, submesh count, flags (1 normals, 2 uv0, 4 uv1, 8 colors, 16 tangents)
        # then per-vertex arrays in that order, then per submesh: index count + uint32 indices
        f.write(struct.pack('<4sIII', b'CLM1', n, len(subs), flags))
        for i in range(n):
            f.write(struct.pack('<3f', *pos[i][:3]))
        if nrm:
            for i in range(n): f.write(struct.pack('<3f', *nrm[i][:3]))
        if uv0:
            for i in range(n): f.write(struct.pack('<2f', *uv0[i][:2]))
        if uv1:
            for i in range(n): f.write(struct.pack('<2f', *uv1[i][:2]))
        if colr:
            for i in range(n): f.write(struct.pack('<4f', *colr[i][:4]))
        if tan:
            for i in range(n): f.write(struct.pack('<4f', *tan[i][:4]))
        for tris in subs:
            f.write(struct.pack('<I', len(tris) * 3))
            for t in tris: f.write(struct.pack('<3I', *t))
    aabb = mesh.m_LocalAABB
    mesh_done[k] = {'key': k, 'name': mesh.m_Name, 'vertices': n, 'submeshes': [len(t) * 3 for t in subs],
                    'flags': flags, 'file': 'meshes/%s.mesh' % k,
                    'aabb_center': v3(aabb.m_Center), 'aabb_extent': v3(aabb.m_Extent)}
    return k


# ---------------- scene graph ----------------
transforms = {}
for pid, o in scene.objects.items():
    if o.type.name in ('Transform', 'RectTransform'):
        transforms[pid] = o.read()


def quat_mat(q):
    x, y, z, w = q
    return [[1 - 2 * (y * y + z * z), 2 * (x * y - z * w), 2 * (x * z + y * w)],
            [2 * (x * y + z * w), 1 - 2 * (x * x + z * z), 2 * (y * z - x * w)],
            [2 * (x * z - y * w), 2 * (y * z + x * w), 1 - 2 * (x * x + y * y)]]


def trs(p, q, s):
    r = quat_mat(q)
    return [[r[0][0] * s[0], r[0][1] * s[1], r[0][2] * s[2], p[0]],
            [r[1][0] * s[0], r[1][1] * s[1], r[1][2] * s[2], p[1]],
            [r[2][0] * s[0], r[2][1] * s[1], r[2][2] * s[2], p[2]],
            [0, 0, 0, 1]]


def mul(a, b):
    return [[sum(a[i][k] * b[k][j] for k in range(4)) for j in range(4)] for i in range(4)]


world = {}


def world_of(pid):
    if pid in world:
        return world[pid]
    t = transforms[pid]
    local = trs(v3(t.m_LocalPosition), v4(t.m_LocalRotation), v3(t.m_LocalScale))
    fp = t.m_Father.path_id
    w = mul(world_of(fp), local) if fp else local
    world[pid] = w
    return w


def active_in_hierarchy(pid):
    while pid:
        t = transforms[pid]
        if not t.m_GameObject.read().m_IsActive:
            return False
        pid = t.m_Father.path_id
    return True


def path_of(pid):
    names = []
    while pid:
        t = transforms[pid]
        names.append(t.m_GameObject.read().m_Name)
        pid = t.m_Father.path_id
    return '/'.join(reversed(names))


objects = []
root_filter = sys.argv[4] if len(sys.argv) > 4 else None
for pid, t in transforms.items():
    go = t.m_GameObject.read()
    path = path_of(pid)
    if root_filter and not (path == root_filter or path.startswith(root_filter + '/')):
        continue
    entry = {'path': path, 'name': go.m_Name, 'active': active_in_hierarchy(pid), 'layer': go.m_Layer,
             'tag': go.m_Tag, 'static_flags': getattr(go, 'm_StaticEditorFlags', 0),
             'local_position': v3(t.m_LocalPosition), 'local_rotation': v4(t.m_LocalRotation),
             'local_scale': v3(t.m_LocalScale), 'world_matrix': world_of(pid), 'components': []}
    mf_mesh = None
    for c in go.m_Component:
        p = c.component if hasattr(c, 'component') else c
        ob = deref(p)
        if ob is None:
            continue
        tn = ob.type.name
        if tn == 'MeshFilter':
            mm = deref(ob.read().m_Mesh)
            mf_mesh = write_mesh(mm) if mm is not None else None
            entry['components'].append({'type': tn, 'mesh': mf_mesh})
        elif tn == 'MeshRenderer':
            r = ob.read()
    for c in go.m_Component:
        p = c.component if hasattr(c, 'component') else c
        ob = deref(p)
        if ob is None:
            continue
        tn = ob.type.name
        if tn == 'MeshRenderer':
            r = ob.read()
            sbi = getattr(r, 'm_StaticBatchInfo', None)
            entry['components'].append({
                'type': tn, 'enabled': bool(r.m_Enabled), 'cast_shadows': r.m_CastShadows,
                'receive_shadows': r.m_ReceiveShadows, 'lightmap_index': r.m_LightmapIndex,
                'lightmap_scale_offset': v4(r.m_LightmapTilingOffset),
                'materials': [export_material(m) for m in r.m_Materials],
                'static_batch_first_submesh': sbi.firstSubMesh if sbi else 0,
                'static_batch_submesh_count': sbi.subMeshCount if sbi else 0,
                'static_batch_root': (sbi and getattr(r, 'm_StaticBatchRoot', None) and r.m_StaticBatchRoot.path_id) or 0,
                'mesh': mf_mesh})
        elif tn == 'BoxCollider':
            b = ob.read()
            entry['components'].append({'type': tn, 'enabled': bool(b.m_Enabled), 'is_trigger': bool(b.m_IsTrigger),
                                        'center': v3(b.m_Center), 'size': v3(b.m_Size)})
        elif tn == 'MeshCollider':
            b = ob.read()
            mm = deref(b.m_Mesh)
            entry['components'].append({'type': tn, 'enabled': bool(b.m_Enabled), 'convex': bool(b.m_Convex),
                                        'mesh': write_mesh(mm) if mm is not None else None})
        elif tn == 'Light':
            l = ob.read()
            entry['components'].append({'type': tn, 'light_type': l.m_Type, 'color': col(l.m_Color),
                                        'intensity': l.m_Intensity, 'range': l.m_Range, 'spot_angle': l.m_SpotAngle,
                                        'shadows': l.m_Shadows.m_Type, 'shadow_strength': l.m_Shadows.m_Strength,
                                        'shadow_bias': l.m_Shadows.m_Bias, 'shadow_normal_bias': l.m_Shadows.m_NormalBias,
                                        'lightmap_bake_type': getattr(l, 'm_Lightmapping', None),
                                        'culling_mask': l.m_CullingMask.m_Bits})
        elif tn == 'Camera':
            cam = ob.read()
            entry['components'].append({'type': tn, 'clear_flags': cam.m_ClearFlags, 'background': col(cam.m_BackGroundColor),
                                        'fov': cam.field_of_view, 'near': cam.near_clip_plane, 'far': cam.far_clip_plane,
                                        'orthographic': bool(cam.orthographic), 'depth': cam.m_Depth,
                                        'culling_mask': cam.m_CullingMask.m_Bits})
        elif tn == 'MonoBehaviour':
            mb = ob.read(check_read=False)
            try:
                cls = mb.m_Script.read().m_ClassName
            except Exception:
                cls = '?'
            entry['components'].append({'type': 'MonoBehaviour', 'script': cls, 'enabled': bool(mb.m_Enabled)})
        elif tn not in ('Transform', 'RectTransform'):
            entry['components'].append({'type': tn})
    objects.append(entry)

# ---------------- global settings ----------------
settings = {}
for pid, o in scene.objects.items():
    tn = o.type.name
    if tn == 'RenderSettings':
        rs = o.read()
        settings['render'] = {
            'fog': bool(rs.m_Fog), 'fog_color': col(rs.m_FogColor), 'fog_mode': rs.m_FogMode,
            'fog_density': rs.m_FogDensity, 'fog_start': rs.m_LinearFogStart, 'fog_end': rs.m_LinearFogEnd,
            'ambient_mode': rs.m_AmbientMode, 'ambient_sky': col(rs.m_AmbientSkyColor),
            'ambient_equator': col(rs.m_AmbientEquatorColor), 'ambient_ground': col(rs.m_AmbientGroundColor),
            'ambient_intensity': rs.m_AmbientIntensity, 'skybox': export_material(rs.m_SkyboxMaterial),
            'reflection_intensity': rs.m_ReflectionIntensity, 'subtractive_shadow_color': col(rs.m_SubtractiveShadowColor),
            'sun': rs.m_Sun.path_id,
            'ambient_probe_sh': [list(c) if not hasattr(c, '__dict__') else str(c) for c in []]}
        try:
            settings['render']['ambient_probe'] = [x for x in rs.m_AmbientProbe.sh.__dict__.values()] if hasattr(rs.m_AmbientProbe, 'sh') else None
        except Exception:
            pass
    elif tn == 'LightmapSettings':
        ls = o.read()
        settings['lightmaps'] = [{'color': export_texture(l.m_Lightmap), 'dir': export_texture(l.m_DirLightmap),
                                  'shadowmask': export_texture(getattr(l, 'm_ShadowMask', None))} for l in ls.m_Lightmaps]
        settings['lightmaps_mode'] = ls.m_LightmapsMode

json.dump({'scene': scene_file, 'root': root_filter, 'objects': objects, 'materials': mat_done,
           'meshes': mesh_done, 'textures': tex_done, 'settings': settings},
          open(out_dir + '/scene.json', 'w'), indent=1, default=str)
print('objects', len(objects), 'meshes', len(mesh_done), 'materials', len(mat_done), 'textures', len(tex_done))
