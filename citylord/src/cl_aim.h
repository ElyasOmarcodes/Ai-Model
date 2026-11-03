/* Aiming / shooting system of Battle of Agents, ported from the game's IL2CPP code
 * (Player, CameraController, fire, weaponManager, Fixedf, VJHandler, rifle_info) with the
 * serialized values of the default player prefab "Char3". Everything works in Unity space.
 * See docs/AIM_SYSTEM.md for the decompiled logic this follows. */
#ifndef CL_AIM_H
#define CL_AIM_H

#include "raylib.h"
#include "cl_physics.h"
#include "cl_render.h"

typedef enum { CL_GUN_RIFLE = 0, CL_GUN_SNIPER = 1, CL_GUN_SMG = 2 } CL_GunTag;

typedef struct {
    const char *name;
    CL_GunTag tag;       /* GameObject tag: untagged, "sniper" or "smg" */
    float rate;          /* rifle_info.rate: seconds between shots */
    float damage;        /* rifle_info.rifle_damage */
    float recoil;        /* rifle_info.recol: degrees of camera kick per shot */
    int mag;             /* rifle_info.mag */
} CL_Weapon;

extern const CL_Weapon cl_weapons[7];

/* What the game's touch UI feeds the scripts each frame. */
typedef struct {
    Vector2 touch_dist;  /* Fixedf.TouchDist: right-half finger delta in pixels this frame */
    Vector2 joystick;    /* VJHandler.InputDirection.xy (x = right, y = forward), |v| <= 1 */
    int fire_held;       /* "Fire" button EventTrigger: start_fire / stop_fire */
    int aim_pressed;     /* "Aim" button: CameraController.setting_aim */
    int scope_pressed;   /* "Change_scope" button: weaponManager.change_scope */
    int reload_pressed;  /* "Reload" button / keyboard 'r' */
    int crouch_pressed;  /* "Crouch" button: Player.setting_crouch */
    int jump_pressed;    /* "Jump" button: Player.jump */
    int swap_pressed;    /* "Change_gun" button: fire.swap_gun */
    int pick_pressed;    /* "pickup" button: fire.fill_amo near an ammo box */
} CL_AimInput;

typedef struct {
    Vector3 point, normal;
    float time;
} CL_Impact;

typedef struct {
    const CL_World *world;
    /* ---- Player (root transform + CharacterController) */
    Vector3 pos;
    float p_yax, p_xax;          /* Player.yax / xax */
    float xv, yv;                /* smoothed joystick */
    float vec_y;                 /* vertical velocity (Player.vec.y) */
    float j_timer;
    int is_courch, moving, grounded;
    float p_assist_val;
    float sensitivity, sensitivity2;
    /* ---- CameraController ("Main Camera", child of the player) */
    Vector3 cam_local;           /* camera position in the player's local space */
    Quaternion cam_rot;          /* world rotation */
    Vector3 targetrot;           /* euler angles being lerped */
    float c_xax, c_yax;
    float dis, normal, cn_cam;
    float fov, nor_fov;
    int isaim, scope_no;
    float c_assist_val;
    /* ---- fire */
    float cn, cn2, r_c, is_rc, g_timer, spd_c;
    float firerate, recoil_val, reload_time, granade_time;
    int curr_b, total_b, bullet_mag, damage_per_hit, tot_g;
    int keep_firing, isfire, is_reloding, is_swaping, aim_assist;
    float invoke_reload_at;      /* MonoBehaviour.Invoke("reload", 0.5) */
    float swap_t;                /* swap_time / swap_set coroutines */
    int swap_state;
    /* ---- weaponManager */
    int weapon;                  /* 0 = primary slot, 1 = secondary */
    int id, id2;                 /* selected weapons (PlayerData.wid / swid) */
    int w_no;
    int current_bul[2][7];       /* rifle_info.current_bul of every gun object */
    int near_ammo_box;
    /* ---- feedback */
    CL_Impact impacts[64];
    int impact_head;
    float time;
    int shots;
    int show_center_dot;         /* not in the game; optional PC helper (H key) */
    Texture2D scope_tex, scope_cross_tex;
} CL_AimSystem;

void cl_aim_init(CL_AimSystem *a, const CL_World *w, const char *assets_dir);
void cl_aim_free(CL_AimSystem *a);
/* Place the player at a spawn point; pass spawn < 0 to use (pos, yaw) instead. */
void cl_aim_spawn(CL_AimSystem *a, int spawn, Vector3 pos, float yaw);
void cl_aim_select_weapon(CL_AimSystem *a, int primary, int secondary);
CL_AimInput cl_aim_read_input(void);
void cl_aim_update(CL_AimSystem *a, const CL_AimInput *in, float dt);
void cl_aim_camera(const CL_AimSystem *a, CL_Camera *cam);
void cl_aim_draw_world(const CL_AimSystem *a, const CL_Camera *cam);
void cl_aim_draw_hud(const CL_AimSystem *a, int w, int h);

/* Spawn points of the map (NetworkStartPosition objects under Network_Manager). */
typedef struct { Vector3 pos; float yaw; } CL_Spawn;
extern const CL_Spawn cl_spawns[7];

#endif
