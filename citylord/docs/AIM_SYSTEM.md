# Aiming system of Battle of Agents

Recovered from `libil2cpp.so` + `global-metadata.dat` (REA IL2CPP metadata + Ghidra
decompilation with Il2CppDumper types) and from the serialized values of the player
prefabs in `sharedassets2.assets`. `MyNetworkManager.OnServerAddPlayer` instantiates
`prefabs[chosenClass]` = `[Char3, male2F, male3F, Fem1, Char6]` at a start position with
identity rotation; the default character (`DataManager.charid = 0`) is **Char3**. All five
share the script values below (only bind-pose bone positions differ by millimetres).
(`malenew`, which carries a disabled CameraController and a ThirdPersonOrbitCamBasic, is
not in that list and is never spawned.) All numbers below are exact; addresses are
Ghidra addresses (image base 0x100000) of the float constants in `.rodata`.

## Components on the player

| object | script | role |
|---|---|---|
| `Char3` | `Player` | movement, body yaw, spine pitch, recoil on the body |
| `Char3` | `fire` | fire rate, ammo, reload, raycast shooting, aim assist flag |
| `Char3/Main Camera` (local 0, 2.27, 0; fov 60, near 0.01, far 150) | `CameraController` | the aiming camera |
| `Char3/Main Camera/GameObject` | `weaponManager` | current gun, scope mode `w_no` |
| `Canvas/Panel` | `Fixedf` | right-half touch drag → `TouchDist` |
| `Canvas/Container` | `VJHandler` | left joystick → `InputDirection` |
| `Main Camera/GameObject/Canvas` | – | sniper scope overlay |

## Look input (Fixedf → CameraController / Player)

`Fixedf.Update`: a finger that starts on the **right half** of the screen is tracked;
`TouchDist = touch.position − previous position` (pixels, per frame), zero otherwise.

Every frame, both `CameraController.Update` and `Player.movement` integrate it:

```
s1 = cam_sens  / 50          (default cam_sens  = 50 [0xf151f8])
s2 = cam_sens2 / 50          (default cam_sens2 = 25 [0xf151fc])
k  = (isaim && scope_no == 1) ? s2 : 2*s1        // 2.0 normally, 0.5 in a scope
assist = fire.aim_assist ? 0.5 : 1
xax = clamp(xax - TouchDist.y * 0.2 * assist * k, -35, 25)   // pitch (0.2 = [0xe2c5f8])
yax =       yax + TouchDist.x * 0.2 * assist * k             // yaw
```

Player: `transform.eulerAngles = (0, yax, 0)`; its `xax` drives the animator `rot`
(spine). Player clamp `maxrot = (-35, 25)` [0xf15218].

## CameraController.Update

Serialized: `pdis 2, dumping 30, movedump 50, aimdis 0, semi_aim 0.8, poff (-10, -6),
pfs 100 (so rotoff = poff), target = cameratarget (0.488, 1.367, 0.062),
body_tar = Head/camtar2`.

```
camorgpos = target.pos - forward * pdis
t  = clamp01(30 * dt)
targetrot = lerp(targetrot, (xax + 10, yax + 6, 0), t)     // camera looks 10° lower, 6° right
transform.eulerAngles = targetrot
crouch = is_courch ? target.up * 0.4 : 0                   // [0xe24e84]
goal = target.pos - crouch - forward * dis
transform.position = lerp(position, goal, clamp01(50 * dt))
if (!isaim) { fov = 60; scope off; dis = fire.isfire ? min(normal, 0.8) : normal;
              cullingMask = ~(1 << 10) }
else        { dis = aimdis (0 → camera on the shoulder point);
              if (scope_no == 1) { scope on; fov -= 500*dt until 20 (snap when |fov-20|<2) }
              else fov = 60;  cullingMask = ~(1 << 9) }
// camera collision: SphereCast r=0.01 [0xe20ab0] head→camorgpos (then reverse)
normal = hit ? distance(head, hit.point) - 0.2 : pdis       // [0xf15230]
```

`setting_aim` (Aim button): `isaim = !isaim && !fire.is_reloding`.
`set_def` (on reload): `isaim = false, dis = normal, fov = 60, scope off`.

## Shooting (fire)

Per-gun data (`rifle_info`, copied into `weaponManager` then `fire` every frame):

| gun | tag | rate (s) | damage | recoil (°) | mag |
|---|---|---|---|---|---|
| AK-47 | – | 0.1 | 20 | 0.5 | 30 |
| L96 | sniper | 1.8 | 60 | 1.0 | 10 |
| M4A1 | – | 0.086 | 17 | 0.35 | 30 |
| MP5K | smg | 0.06 | 10 | 0.2 | 45 |
| UMP | smg | 0.092 | 15 | 0.25 | 45 |
| SCAR-L | – | 0.096 | 16 | 0.3 | 30 |
| Kar98 | sniper | 2.0 | 55 | 1.0 | 10 |

`fire.Update`: `cn += dt` while `cn < rate`; `is_reloding = is_rc > 0`; `isfire` stays
true for 0.3 s after a shot [0xe24e88]; auto reload when the magazine is empty;
`Input.GetKeyDown('r')` reloads; `keep_firing` (Fire button held) calls `shoot()`.

`fire.shoot()`:

```
if (cn < rate || is_reloding || is_swaping || curr_b < 1) return
isfire = true; cn2 = 0
CameraController.recoil(-recoil): cam.xax = clamp(cam.xax - assist*recoil, -35, 25)
Player.recoil(-recoil)                         // kick up, no automatic recovery
if Raycast(camera.pos, camera.forward, 150 [0xe24af4], mask ~(1<<9 | 1<<13)) → A
   if Raycast(shoot_point, A.point - shoot_point, 150, same mask) → B
       tag Player/bot/zombie → damage;  head/bot_head/zombie_head → damage × 3
       anything else → hit effect at B.point (CmdonHit)
cn = 0; ammo -= 1; if empty and reserve > 0: Invoke("reload", 0.5)
```

There is **no spread**: the bullet goes exactly along the camera's forward axis; walls
between the gun (`shoot_point` on the hips) and the aimed point block it.

`fire.assist()` (every frame): Raycast(camera, forward, 100 [0xe2093c], ~(1<<9));
`aim_assist = hit.tag ∈ {Player, head, bot, bot_head}` → look speed and recoil halved.

`fire.reload()`: needs reserve > 0, `r_c ≥ 2.5`, magazine not full; calls `set_def`,
moves `min(mag - curr, reserve)` bullets immediately, then `is_rc = 2.5` (2.5 s during
which aiming and shooting are blocked). Reserve is 300 (`fill_amo` / `reset_bullet`).

`weaponManager.change_scope` (scope button): `w_no` toggles 0↔1, except a sniper never
leaves 1 and an SMG never leaves 0; `set_weapon` puts snipers in `w_no = 1` (so a sniper
always uses the scope). `scope_no = w_no`.

Weapon swap: blocked while reloading/swapping; 0.5 s → slot change → 0.5 s → `cn = 5`.

## Movement relevant to aiming (Player.movement)

speed 5, walkspeed 2 (used while aiming, firing or crouched); joystick smoothed at 5/s and
normalised (any push gives full speed); gravity −9.81 [0xf1523c]; grounded via
`CheckCapsule` 0.1 below the CharacterController (radius 0.5, height 1, center y 0.45);
jump velocity 4.429447 every ≥ 1 s.

## Sniper scope overlay

Canvas scaler 800×600, match height. `scope4` 2061.8×1162.8 at (+10, +190) and the
reticle 457.7×466.7 at (−1.8, −1.9) from the screen centre (reference pixels).

## What the C port changes

* PC input mapping: mouse delta = `TouchDist` (y inverted to touch convention), WASD =
  joystick, LMB = Fire, RMB = Aim, F = scope, R = reload, C = crouch, Space = jump,
  Q = swap, E = pick up ammo, 1–7 = choose gun. H toggles a centre dot that the game
  does **not** have (off by default).
* The CharacterController is a sphere collide-and-slide against the same colliders
  (PhysX's internal step/slope solver is not public).
* `body_tar`/`shoot_point` use the skeleton's bind pose (the game animates them).
* The 1 cm camera SphereCast is evaluated as a ray.
