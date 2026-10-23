"""Dump serialized MonoBehaviour fields for chosen script classes across all asset files.
usage: dump_mb.py <merged_data_dir> <DummyDll dir> <out.json> Class1 Class2 ...
"""
import sys, os, json
import UnityPy
from UnityPy.helpers.TypeTreeGenerator import TypeTreeGenerator

data, dlls, out = sys.argv[1], sys.argv[2], sys.argv[3]
want = set(sys.argv[4:])
env = UnityPy.load(data)
gen = TypeTreeGenerator('2020.3.1f1')
for fn in sorted(os.listdir(dlls)):
    try:
        gen.load_dll(open(os.path.join(dlls, fn), 'rb').read())
    except Exception:
        pass


def clean(v):
    if isinstance(v, dict):
        if set(v.keys()) == {'m_FileID', 'm_PathID'}:
            return 'PPtr(%d,%d)' % (v['m_FileID'], v['m_PathID'])
        return {k: clean(x) for k, x in v.items()}
    if isinstance(v, list):
        return [clean(x) for x in v]
    return v


def go_path(mb):
    try:
        go = mb.m_GameObject.read()
        tr = None
        for c in go.m_Component:
            p = c.component if hasattr(c, 'component') else c
            ob = p.deref()
            if ob.type.name in ('Transform', 'RectTransform'):
                tr = ob.read(); break
        names = [go.m_Name]
        while tr is not None and tr.m_Father.path_id:
            tr = tr.m_Father.read(); names.append(tr.m_GameObject.read().m_Name)
        return '/'.join(reversed(names))
    except Exception as e:
        return '?'


res = []
for name, f in env.files.items():
    objs = getattr(f, 'objects', None)
    if not objs:
        continue
    for pid, o in objs.items():
        if o.type.name != 'MonoBehaviour':
            continue
        try:
            mb = o.read(check_read=False)
            cls = mb.m_Script.read().m_ClassName
        except Exception:
            continue
        if cls not in want:
            continue
        try:
            env.typetree_generator = gen
            t = o.read_typetree()
        except Exception as e:
            t = {'error': str(e)}
        finally:
            env.typetree_generator = None
        res.append({'file': os.path.basename(name), 'path_id': pid, 'class': cls, 'object': go_path(mb), 'fields': clean(t)})
json.dump(res, open(out, 'w'), indent=1, default=str)
print(len(res))
