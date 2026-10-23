# CityLord — Battle of Agents نقشه او د نښه نیولو سیستم په C ژبه

## لنډیز (پښتو)

* **کوم ابزار:** د [REA](https://github.com/morluto/rea) له سرچینې جوړ شو (د npm نسخه کې د
  IL2CPP قومانده نه وه)، او `inspect-il2cpp-metadata --binary` یې د ګیم ټولې کلاسونه او د هرې
  میتود پته راکړه. بیا Ghidra 12.1.4 (هغه decompiler چې REA یې چلوي)، Il2CppDumper او UnityPy
  وکارول شول. ټول ګامونه په `docs/REVERSE_ENGINEERING.md` کې دي.
* **CityLord نقشه:** د لابي په `lobby_manager` کې `ids = [2,4,7,8]` دي، نو CityLord
  (لومړی انتخاب) = سین ۲ = `Assets/Scenes/map3.unity` = فایل `level2`. ریښه یې `Map_v1` ده:
  623 رینډررونه، 31 مواد (material)، 38 ټکسچرونه، 615 کولایډرونه، یو لمر (Directional Light) له
  نرم سیوري سره، Flat ambient او 6-اړخیز skybox. تفصیل: `docs/MAP.md`.
* **C نسخه:** `src/citylord_data.c` د هر شي دقیق ارقام لري (موقعیت، چرخش، اندازه د Unity د
  ماتریس په بڼه، رنګونه، د موادو ټول ارقام، رڼا، سیوري). `src/cl_render.c` د ګیم shaderونه
  (Mobile/Diffuse، Legacy Cutout، Standard، Skybox) په GLSL کې بیا جوړوي.
* **د نښه نیولو سیستم:** `src/cl_aim.c` د `CameraController`، `Player`، `fire` او
  `weaponManager` کرښه په کرښه ژباړه ده: د کمرې حساسیت، د Aim حالت، د sniper scope (FOV 20)،
  recoil، د ډزو سرعت، reload، aim assist او د دوه raycast ډزې. ټول فورمولونه او ثابتونه په
  `docs/AIM_SYSTEM.md` کې.
* **د ګیم اصلي فایلونه (ټکسچر/میش) په دې عامه ریپو کې نه دي اچول شوي** ځکه چې د ګیم د جوړونکي
  ملکیت دي؛ `tools/extract_citylord.sh` یې ستاسو له APK څخه په ۲۰ ثانیو کې بیا جوړوي.

## Build & run

```bash
pip install UnityPy
tools/extract_citylord.sh "Battle of Agents - Multiplayer_0.5.8_APKPure.apk"   # -> assets/, src/citylord_data.c

cmake -B build && cmake --build build      # fetches raylib 5.5
./build/citylord --assets assets           # free camera over the map
./build/citylord --assets assets --spawn 0 # play from spawn point 0 with the game's aim system
```

Linux alternative: `make RAYLIB=/path/to/raylib/src`.

| key | free camera | aim mode (Tab or `--spawn N`) |
|---|---|---|
| mouse | look | `Fixedf.TouchDist` (look) |
| WASD | fly | `VJHandler` joystick |
| LMB / RMB | – | Fire (hold) / Aim toggle |
| F, R, C, Space, Q, E | – | scope, reload, crouch, jump, swap gun, pick ammo |
| 1–7 | – | AK-47, L96, M4A1, MP5K, UMP, SCAR-L, Kar98 |
| F1–F6 | quality level (game default: F4 = "High") | same |

Other options: `--quality 0..5`, `--cam x y z yaw pitch`, `--screenshot file.png`,
`--aim-test N` (scripted check, see `docs/REVERSE_ENGINEERING.md`).

## Layout

```
src/citylord_data.[ch]   generated scene tables (renderers, colliders, materials, settings)
src/cl_assets.c          mesh (CLM1) and texture loading
src/cl_render.c          Unity built-in forward pipeline equivalent (gamma, shadows, skybox)
src/cl_physics.c         raycasts / overlap tests against the map colliders
src/cl_aim.c             the aiming and shooting system
tools/                   extraction (UnityPy) and Ghidra scripts
docs/                    MAP.md, AIM_SYSTEM.md, REVERSE_ENGINEERING.md
```

Differences that remain are listed at the end of `docs/MAP.md` and `docs/AIM_SYSTEM.md`.
