"""Export the HUD sprites used by the aiming system (sniper scope overlay) from the game data.
usage: export_hud.py <merged_data_dir> <assets_out_dir>"""
import sys, os
import UnityPy

data, out = sys.argv[1], sys.argv[2]
os.makedirs(os.path.join(out, 'hud'), exist_ok=True)
want = {'scope4': 'scope4.png', 'scope-simple-sniper-crosshair-fallout-new-vegas-mods-and-5': 'scope_crosshair.png'}
env = UnityPy.load(data)
for o in env.objects:
    if o.type.name == 'Sprite':
        s = o.read()
        if s.m_Name in want and os.path.basename(o.assets_file.name) == 'sharedassets2.assets':
            try:
                img = s.image
            except ValueError:
                # tight-mesh rendering fails on some sprites; cut the full-rect sprite from its texture
                tex = s.m_RD.texture.read().image
                r = s.m_RD.textureRect
                h = tex.height
                img = tex.crop((int(r.x), int(h - r.y - r.height), int(r.x + r.width), int(h - r.y)))
            img.save(os.path.join(out, 'hud', want[s.m_Name]))
            print('exported', s.m_Name, img.size)
