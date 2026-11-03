# How the data was recovered

Target: `Battle of Agents - Multiplayer_0.5.8_APKPure.apk` (Unity 2020.3.1f1, IL2CPP,
`lib/arm64-v8a/libil2cpp.so`, metadata v27).

## 1. REA (morluto/rea)

The npm release (`rea-agents` 6.7.0) does not yet expose `inspect-il2cpp-metadata`;
it is on the repository's main branch, so REA was built from source:

```bash
git clone https://github.com/morluto/rea.git && cd rea
npm ci --ignore-scripts --engine-strict=false && npm run compile
node scripts/rea.mjs inspect-il2cpp-metadata Data/Managed/Metadata/global-metadata.dat \
     --binary lib/arm64-v8a/libil2cpp.so --json > meta.json        # 3807 types, 25 577 methods
node scripts/rea.mjs inspect-il2cpp-metadata Data/Managed/Metadata/global-metadata.dat \
     --binary lib/arm64-v8a/libil2cpp.so --ghidra-seeds il2cpp-seeds.tsv --json
```

REA gave the game's own classes (`Assembly-CSharp`: `fire`, `CameraController`,
`Player`, `weaponManager`, `lobby_manager`, …) and the code address of every method.

## 2. Ghidra 12.1.4 (the decompiler REA drives)

`libil2cpp.so` was imported headless without auto-analysis; the scripts in `tools/`
then:

* `ApplyIl2cpp.java` — names every method / metadata usage / string literal,
* `ApplyTypes.java` — parses Il2CppDumper's `il2cpp.h` (after rewriting its C++
  `struct A : B` inheritance into an embedded `B super;` member, which Ghidra's C parser
  needs to keep the 8-byte `m_CachedPtr` base field) and applies all 43 138 method
  signatures; marks the il2cpp throw helpers as no-return,
* `DecompileList.java` — decompiles the classes of interest.

Il2CppDumper 6.7.46 (run on .NET 8) supplied `il2cpp.h`, `dump.cs` (field offsets) and the
dummy DLLs used to read MonoBehaviour fields.

Float constants referenced from the code (`_UNK_00e24af4` etc.) were read from the ELF
`.rodata` (Ghidra address − 0x100000 = file virtual address).

## 3. UnityPy

Scenes, prefabs, materials, meshes and textures were read with UnityPy 1.25 from the
`assets/bin/Data` files (the `.splitN` chunks re-joined first). MonoBehaviour fields were
decoded with type trees generated from the dummy DLLs (`tools/dump_mb.py`).

## Verification done

* Rendering compared visually (screenshots) after each fix: winding/culling, skybox
  orientation (seam continuity measured numerically on all 12 cube edges), shadow bias.
* Aim test (`citylord --spawn 0 --aim-test 220`) checks the decoded formulas at a fixed
  60 fps: 30 frames of 4 px / 0.5 px touch drag give yaw +48° and pitch −6°; 60 frames of
  AK-47 fire give exactly 10 shots (`rate 0.1`) and −5° of recoil (10 × 0.5°); switching to
  the L96 with the aim button puts `scope_no = 1` and the FOV at 20°.
* `tools/extract_citylord.sh` regenerates `src/citylord_data.c` byte-for-byte.
