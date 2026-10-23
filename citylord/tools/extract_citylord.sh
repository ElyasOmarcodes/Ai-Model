#!/usr/bin/env bash
# Rebuilds citylord/assets and src/citylord_data.c from the game's APK.
#   tools/extract_citylord.sh "Battle of Agents - Multiplayer_0.5.8_APKPure.apk"
# Needs: python3 with UnityPy (pip install UnityPy), unzip.
set -euo pipefail
apk="$(realpath "$1")"
here="$(cd "$(dirname "$0")/.." && pwd)"
work="$(mktemp -d)"
trap 'rm -rf "$work"' EXIT

unzip -q "$apk" 'assets/bin/Data/*' -d "$work/apk"
data="$work/apk/assets/bin/Data"
merged="$work/merged"
mkdir -p "$merged/Resources"
# Unity splits big serialized files into *.splitN chunks; join them back
for first in "$data"/*.split0; do
    base="$(basename "$first" .split0)"
    n=0
    : > "$merged/$base"
    while [ -f "$data/$base.split$n" ]; do cat "$data/$base.split$n" >> "$merged/$base"; n=$((n + 1)); done
done
for f in "$data"/*; do
    case "$f" in *.split*) continue ;; esac
    [ -f "$f" ] && ln -sf "$f" "$merged/$(basename "$f")"
done
ln -sf "$data/Resources/unity_builtin_extra" "$merged/Resources/unity_builtin_extra"

# CityLord = lobby map 0 -> build scene 2 = Assets/Scenes/map3.unity = level2
python3 -I "$here/tools/export_scene.py" "$merged" level2 "$work/export"
python3 -I "$here/tools/gen_scene_c.py" "$work/export" Map_v1 "$here/src" "$here/assets"
python3 -I "$here/tools/export_hud.py" "$merged" "$here/assets"
cp "$work/export/scene.json" "$here/assets/scene.json"
echo "assets written to $here/assets"
