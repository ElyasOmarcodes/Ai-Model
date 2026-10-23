# CityLord map — how it is built

Source: `Battle of Agents - Multiplayer` 0.5.8 (Unity 2020.3.1f1, IL2CPP, arm64).

## Which scene is CityLord

The lobby (`Assets/Scenes/lobby_scene.unity`, file `level10`) has a map `Dropdown`
with options `CityLord, FallTown, Village, Sandstorm` and a `lobby_manager` whose
serialized `ids` array is `[2, 4, 7, 8]`. Dropdown index 0 (CityLord) therefore loads
build scene **2 = `Assets/Scenes/map3.unity` = `assets/bin/Data/level2`**; the scene's
own HUD text reads `Map: CityLord`.

| build index | scene | file |
|---|---|---|
| 2 | map3 (**CityLord**) | level2 |
| 4 | town (FallTown) | level4 |
| 7 | forest (Village) | level7 |
| 8 | desert (Sandstorm) | level8 |

## Scene graph

```
Directional Light            Light (sun)
Map_v1  (0, 0, -0.595)       the map (43 children)
  Static                     321 renderers: buildings, hangars, roads, fences, pipes, props
  new                        9 children
  Grid Fence … Grid Fence (11)   12 fence groups, 21 renderers each
  Cover … Cover (5)          cover blocks
  AmmoBox … AmmoBox (5)      6 ammo boxes (Ammo_box script + trigger BoxCollider)
  Barrel_*, Wooden_box_*, Cargo_container_*, TallLadder
Element (0, 0.88, 0)
  Canvas (new_gui, Load_Data), GameManager, NetWok_Dis, EventSystem
  Camera                     spectator camera (fov 60, near 0.3, far 1000)
  Network_Manager            7 spawn points (spwan1, spwan2 …)
  Add_Manager
```

* 815 GameObjects, 625 MeshRenderers (623 active), 426 MeshColliders, 189 BoxColliders
  (6 of them triggers = ammo pickups). Every map collider is on layer 0 and untagged.
* 89 distinct mesh names (most common: `Wooden_box_v1_LD1square` ×35,
  `Concrete_fence_v1_wall_set_v2` ×17, `Palet_v1_set` ×16, `Road_set_v1_s_floor` ×15, …).
* **Static batching**: 623 renderers draw from 3 `Combined Mesh (root: scene)` meshes
  (167 808 vertices / 116 704 triangles together). Their vertices are already in world
  space; each renderer owns a range of sub-meshes (`m_StaticBatchInfo`). The port keeps
  exactly that layout (`CL_Renderer.first_submesh / submesh_count / static_batched`).
* No baked lightmaps (`LightmapSettings` is empty, every renderer has lightmap index 65535).

## Materials (31)

All but five use `Mobile/Diffuse` — a Lambert surface shader whose albedo is the
texture only (`_Color` is ignored even though many materials store 0.588 grey).

| material | shader | _Color | _MainTex | size |
|---|---|---|---|---|
| Seamless_asphalt_v1 | Mobile/Diffuse | 1,1,1,1 | Seamless_asphalt_v1 | 1024² |
| Road_with_pavements_v1 | Mobile/Diffuse | 1,1,1,1 | Road_with_pavements_v1 | 1024×512 |
| Hangar_v1 … v4, Oil_tank_v1/v2, Pipes_set_v1, Palet_v1, Generator_v1, Conditioner_v1, Electric_box_v1/v2, Dumpsters_v1, Road_block_v1, Concrete_fence_v1, Industrial_pipe_v1 (256×512), Concrete_fence_v2 (512×256) | Mobile/Diffuse | 0.588 grey (unused) | same name | 512² |
| Wooden_box_v1, Barrel_v1/v2/v3, Cargo_container_v1, UNIConcrete_wall_v1 (512×256) | Mobile/Diffuse | 1,1,1,1 | same name | 512² |
| Lattice_v1 | Legacy Shaders/Transparent/Cutout/Diffuse | 0.632 grey | Lattice_v1 (ETC2 RGBA) | 512², `_Cutoff` 0.129 |
| AmmoBox | Standard (metallic, normal map, emission) | 0.054, 0.189, 0 | AmmoBox_Albedo + _Normal + _Metallic | 512², GlossMapScale 0.47, emission 0.042, 0.142, 0.003 |
| Ladder | Standard (metallic, normal map) | 1,1,1 | Ladder_Albedo + _Normal + _Metallic | 256², GlossMapScale 0.163 |
| New Material | Standard | 0.179, 0.028, 0.028 | – | metallic 1, smoothness 0 |
| New Material 1 | Standard | 0.255 grey | – | metallic 0.531, smoothness 0.869 |
| sky | Skybox/6 Sided | _Tint 0.5, _Exposure 1, _Rotation 0 | skybox_country_paths_* | 1024² each |

Textures are ETC_RGB4 (ETC2_RGBA8 for the lattice), bilinear, aniso 1, repeat, with
mip chains; they are decoded losslessly from the ETC data into PNG.

## Lighting and render settings (exact values)

| item | value |
|---|---|
| Color space | Gamma |
| Sun | Directional, rotation (0.40822, −0.23457, 0.10938, 0.87543), color (1, 0.95686, 0.83922), intensity 1.2, **soft shadows**, strength 1, bias 0.05, normal bias 0.4, mode Mixed (no bake) |
| Ambient | Flat (mode 3), color (0.73113, 0.82075, 1.0); intensity field 1.45 is unused in flat mode |
| Fog | off (mode ExponentialSquared, density 0.01, color 0.5 grey) |
| Skybox | `sky` (6-sided, country paths) |
| Quality used by the game | `DataManager` default `gid = 3` → QualitySettings "High": soft shadows, 2 cascades, shadow distance 40, shadow resolution Medium, 2× MSAA, pixel light count 2 |

## How the C port reproduces it

* `src/citylord_data.c` (generated) holds every renderer (path, world matrix as
  stored by Unity, mesh, sub-mesh range, material slots, shadow flags), every collider,
  every material property and the light / ambient / fog / camera settings, as exact
  float literals.
* `assets/meshes/*.mesh` are the mesh buffers (positions, normals, UV0, UV1, tangents,
  index buffers per sub-mesh) exactly as stored; `assets/textures/*.png` are the decoded
  textures.
* `src/cl_render.c` re-implements the shaders: Mobile/Diffuse
  (`albedo·_LightColor0·max(0,N·L)·shadow + albedo·ambient`), Legacy cutout,
  Standard (Unity BRDF1 in gamma space, sky reflection), 6-sided skybox
  (`tex·_Tint·2·_Exposure`), cascaded soft shadow maps (stable fit, 3×3 tent PCF).
  Unity's left-handed space is mirrored on Z for OpenGL; winding and culling are kept
  identical (verified by rendering).

### Known, measurable differences

* Shadow maps: same cascade split (1/3 for 2 cascades), distance and resolution, but
  Unity's internal bias scaling is not public — the bias factors were tuned to remove acne.
* Standard-shader reflections sample the skybox directly instead of Unity's baked 128 px
  reflection probe (affects only the 4 Standard materials).
* MSAA: raylib exposes 4× only (game "High" uses 2×).
* The bottom skybox face does not line up with the side faces in the asset itself
  (checked numerically for all 8 orientations); it is below the ground and never visible.
