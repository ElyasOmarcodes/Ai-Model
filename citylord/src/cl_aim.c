/* Port of the Battle of Agents aiming / shooting code. Comments name the C# method each
 * block comes from; constants are the literal values read from libil2cpp.so (address in
 * brackets) or from the serialized prefab. */
#include "cl_aim.h"
#include "citylord_data.h"
#include "raymath.h"
#include "rlgl.h"

#include <math.h>
#include <stdio.h>
#include <string.h>

/* rifle_info components of Char3/Main Camera/GameObject/Slot1 (Slot2 holds the same guns) */
const CL_Weapon cl_weapons[7] = {
    {"AK-47", CL_GUN_RIFLE, 0.100000001f, 20.0f, 0.5f, 30},
    {"L96", CL_GUN_SNIPER, 1.79999995f, 60.0f, 1.0f, 10},
    {"M4A1", CL_GUN_RIFLE, 0.0860000029f, 17.0f, 0.349999994f, 30},
    {"MP5K", CL_GUN_SMG, 0.0599999987f, 10.0f, 0.200000003f, 45},
    {"UMP", CL_GUN_SMG, 0.0920000002f, 15.0f, 0.25f, 45},
    {"SCAR-L", CL_GUN_RIFLE, 0.0960000008f, 16.0f, 0.300000012f, 30},
    {"Kar98", CL_GUN_SNIPER, 2.0f, 55.0f, 1.0f, 10},
};

/* World transforms of Element/Network_Manager/spwan* (yaw of their forward axis). */
const CL_Spawn cl_spawns[7] = {
    {{17.3400002f, -12.9119998f, 14.3200006f}, 180.0f},
    {{39.1599979f, -11.4499995f, -2.18000031f}, 180.0f},
    {{-0.570000529f, -11.2729999f, -35.5800009f}, 180.0f},
    {{81.460001f, -10.8619996f, -24.5800009f}, 180.0f},
    {{-23.3999996f, -12.9219996f, 12.0199995f}, 0.0f},
    {{46.3199978f, -10.7919999f, 20.0199995f}, 180.0f},
    {{9.80999947f, -11.2889997f, -27.2400007f}, 180.0f},
};

/* ---- serialized values of the default character prefab "Char3" (MyNetworkManager.prefabs[0],
 *      DataManager default charid 0); the other playable characters share the same script values */
#define PLAYER_SPEED 5.0f          /* Player.speed */
#define PLAYER_WALKSPEED 2.0f      /* Player.walkspeed */
#define PLAYER_JUMP_TIMER 1.0f     /* Player.jump_timer */
#define PLAYER_MOV_SMOOTH 5.0f     /* Player.Start: mov_smooth = 5 */
#define MAXROT_MIN (-35.0f)        /* Player..ctor maxrot.x [0xf15218] */
#define MAXROT_MAX 25.0f           /* Player..ctor maxrot.y [0xf1521c] */
#define CAM_PDIS 2.0f              /* CameraController.pdis */
#define CAM_DUMPING 30.0f          /* CameraController.dumping */
#define CAM_MOVEDUMP 50.0f         /* CameraController.movedump */
#define CAM_AIMDIS 0.0f            /* CameraController.aimdis */
#define CAM_SEMI_AIM 0.8f          /* CameraController.semi_aim */
#define CAM_POFF_X (-10.0f)        /* CameraController.poff (pfs = 100, so rotoff = poff) */
#define CAM_POFF_Y (-6.0f)
#define CAM_FOV 60.0f              /* Main Camera field of view -> nor_fov */
#define CAM_NEAR 0.00999999978f
#define CAM_FAR 150.0f
#define FIRE_RELOAD_TIME 2.5f      /* fire.reload_time */
#define FIRE_GRANADE_TIME 2.0f     /* fire.granade_time */
#define DEFAULT_CAM_SENS 50        /* DataManager default cam_sencitivity [0xf151f8] */
#define DEFAULT_CAM_SENS2 25       /* DataManager default cam_sencitivity2 [0xf151fc] */
#define AMMO_RESERVE 300           /* fire.fill_amo / reset_bullet: total_b = 300 */
/* ---- code constants */
#define TOUCH_SCALE 0.2f           /* [0xe2c5f8] */
#define SENS_DIV 50.0f             /* [0xe2c5f4] */
#define CROUCH_CAM_DROP 0.4f       /* [0xe24e84] */
#define SCOPE_FOV 20.0f
#define SCOPE_FOV_SPEED (-500.0f)  /* [0xf1522c] */
#define CAM_HIT_OFFSET (-0.2f)     /* [0xf15230] */
#define GRAVITY (-9.81000042f)     /* [0xf1523c] */
#define GROUND_PROBE (-0.1f)       /* [0xf15234] */
#define JUMP_VELOCITY 4.429447f
#define SHOOT_RANGE 150.0f         /* [0xe24af4] */
#define ASSIST_RANGE 100.0f        /* [0xe2093c] */
#define ISFIRE_HOLD 0.3f           /* [0xe24e88] */
/* ---- CharacterController of Char3 */
#define CC_RADIUS 0.5f
#define CC_HEIGHT 1.0f
#define CC_CENTER_Y 0.449999988f
#define CC_SLOPE_LIMIT 45.0f
#define CC_STEP_OFFSET 0.5f
/* ---- layer masks used by the code */
#define MASK_NOT_PLAYER 0xFFFFFDFFu          /* -0x201: everything but layer 9 "player" */
#define MASK_NOT_PLAYER_WALL 0xFFFFDDFFu     /* -0x2201: also without layer 13 "WALL" */
/* ---- transforms under the player root (bind pose of Char3) */
static const Vector3 CAMERATARGET = {0.488f, 1.367f, 0.062f};       /* Char3/cameratarget */
static const Vector3 BODY_TAR = {0.259884f, 1.298165f, 0.13992f};       /* .../Head/camtar2 */
static const Vector3 SHOOT_POINT = {0.147688f, 1.358425f, 0.242122f};    /* Hips/shoot_point */

static Quaternion euler_to_quat(Vector3 e)
{
    /* Unity Quaternion.Euler: Z, then X, then Y (degrees) */
    Quaternion qx = QuaternionFromAxisAngle((Vector3){1, 0, 0}, e.x * DEG2RAD);
    Quaternion qy = QuaternionFromAxisAngle((Vector3){0, 1, 0}, e.y * DEG2RAD);
    Quaternion qz = QuaternionFromAxisAngle((Vector3){0, 0, 1}, e.z * DEG2RAD);
    return QuaternionMultiply(qy, QuaternionMultiply(qx, qz));
}

static Quaternion player_rot(const CL_AimSystem *a) { return euler_to_quat((Vector3){0, a->p_yax, 0}); }

static Vector3 player_point(const CL_AimSystem *a, Vector3 local)
{
    return Vector3Add(a->pos, cl_quat_rotate(player_rot(a), local));
}

static Vector3 cam_world(const CL_AimSystem *a) { return player_point(a, a->cam_local); }
static Vector3 cam_forward(const CL_AimSystem *a) { return cl_quat_rotate(a->cam_rot, (Vector3){0, 0, 1}); }

static const CL_Weapon *current_weapon(const CL_AimSystem *a)
{
    return &cl_weapons[a->weapon == 0 ? a->id : a->id2];
}

static int *current_bul(CL_AimSystem *a) { return &a->current_bul[a->weapon][a->weapon == 0 ? a->id : a->id2]; }

/* ---------------------------------------------------------------- weaponManager */
/* weaponManager.change_scope */
static void wm_change_scope(CL_AimSystem *a)
{
    const CL_Weapon *g = current_weapon(a);
    if (g->tag == CL_GUN_SNIPER && a->w_no == 1) return;
    if (g->tag == CL_GUN_SMG && a->w_no == 0) return;
    a->w_no = (a->w_no + 1) % 2; /* the gun model is hidden while w_no == 1 */
}

/* weaponManager.set_weapon */
static void wm_set_weapon(CL_AimSystem *a)
{
    a->w_no = 0;
    if (current_weapon(a)->tag == CL_GUN_SNIPER) wm_change_scope(a);
}

void cl_aim_select_weapon(CL_AimSystem *a, int primary, int secondary)
{
    a->id = primary;
    a->id2 = secondary;
    a->weapon = 0;
    wm_set_weapon(a);
}

/* ---------------------------------------------------------------- CameraController helpers */
/* CameraController.set_def */
static void cam_set_def(CL_AimSystem *a, int cc)
{
    if (cc && a->is_courch) a->is_courch = 0;
    a->isaim = 0;
    a->dis = a->normal;
    a->fov = a->nor_fov; /* scope canvas off, cullingMask = ~(1 << 10) */
}

/* CameraController.recoil / Player.recoil */
static void recoil(CL_AimSystem *a, float amount)
{
    a->c_xax = Clamp(a->c_xax + a->c_assist_val * amount, -35.0f, 25.0f);
    a->p_xax = Clamp(a->p_xax + a->p_assist_val * amount, MAXROT_MIN, MAXROT_MAX);
}

/* ---------------------------------------------------------------- fire */
static void add_impact(CL_AimSystem *a, Vector3 p, Vector3 n)
{
    CL_Impact *im = &a->impacts[a->impact_head++ % 64];
    im->point = p;
    im->normal = n;
    im->time = a->time;
}

/* fire.reload */
static void fire_reload(CL_AimSystem *a)
{
    if (a->total_b == 0 || a->r_c < a->reload_time || a->curr_b == a->bullet_mag) return;
    cam_set_def(a, 0);
    int amount = a->bullet_mag - a->curr_b;
    if (a->total_b < amount) amount = a->total_b;
    a->total_b -= amount;
    int wm_current_before = *current_bul(a); /* fire reads wap_man.wm_current, refreshed next frame */
    *current_bul(a) += amount;               /* weaponManager.minus_bul(amount) */
    a->curr_b = wm_current_before;
    a->is_rc = a->reload_time;
    a->r_c = 0.0f;
}

/* fire.shoot */
static void fire_shoot(CL_AimSystem *a)
{
    if (a->cn < a->firerate || a->is_reloding || a->is_swaping || a->curr_b < 1) return;
    a->cn2 = 0.0f;
    a->isfire = 1;
    a->shots++;
    recoil(a, -a->recoil_val);
    Vector3 origin = cam_world(a), dir = cam_forward(a);
    CL_RaycastHit h1, h2;
    if (cl_raycast(a->world, origin, dir, SHOOT_RANGE, MASK_NOT_PLAYER_WALL, &h1)) {
        /* second ray from the body's shoot_point towards the aimed point decides what is hit */
        Vector3 sp = player_point(a, SHOOT_POINT);
        if (cl_raycast(a->world, sp, Vector3Subtract(h1.point, sp), SHOOT_RANGE, MASK_NOT_PLAYER_WALL, &h2)) {
            /* the map has no "Player"/"head"/"bot"/"zombie" tags: CmdonHit(1, point, normal) -> hitefct */
            add_impact(a, h2.point, h2.normal);
        }
    }
    a->cn = 0.0f;
    *current_bul(a) -= 1; /* weaponManager.minus_bul(-1) */
    a->curr_b -= 1;
    if (a->curr_b == 0 && a->total_b != 0) a->invoke_reload_at = a->time + 0.5f;
}

/* fire.assist */
static void fire_assist(CL_AimSystem *a)
{
    CL_RaycastHit h;
    /* aim_assist is set only when the ray hits a collider tagged Player/head/bot/bot_head;
     * CityLord's static colliders are all untagged */
    a->aim_assist = 0;
    (void)cl_raycast(a->world, cam_world(a), cam_forward(a), ASSIST_RANGE, MASK_NOT_PLAYER, &h);
}

/* fire.Update */
static void fire_update(CL_AimSystem *a, const CL_AimInput *in, float dt)
{
    if (a->g_timer < a->granade_time) a->g_timer += dt;
    const CL_Weapon *g = current_weapon(a);
    a->recoil_val = g->recoil;
    a->curr_b = *current_bul(a);
    a->bullet_mag = g->mag;
    a->firerate = g->rate;
    a->damage_per_hit = (int)g->damage;
    if (a->spd_c < 0.5f) a->spd_c += dt;
    if (a->cn < a->firerate) a->cn += dt;
    if (a->is_rc > 0.0f) a->is_rc -= dt;
    if (a->r_c < a->reload_time) a->r_c += dt;
    a->is_reloding = a->is_rc > 0.0f;
    if (a->cn2 >= ISFIRE_HOLD) a->isfire = 0;
    else a->cn2 += dt;
    if (a->curr_b == 0 && a->total_b != 0 && !a->is_reloding) fire_reload(a);
    if (a->keep_firing) fire_shoot(a);
    if (in->reload_pressed) fire_reload(a);
    fire_assist(a);
}

/* fire.swap_gun + swap_time / swap_set coroutines */
static void fire_swap_update(CL_AimSystem *a, const CL_AimInput *in, float dt)
{
    if (in->swap_pressed && !a->is_reloding && !a->is_swaping && a->swap_state == 0) {
        a->is_swaping = 1;
        a->swap_state = 1;
        a->swap_t = 0.5f;
    }
    if (a->swap_state) {
        a->swap_t -= dt;
        if (a->swap_t <= 0.0f) {
            if (a->swap_state == 1) { /* Rpcgun_swap: switch the active slot */
                a->weapon ^= 1;
                wm_set_weapon(a);
                a->swap_state = 2;
                a->swap_t = 0.5f;
            } else {                  /* swap_set */
                a->cn = 5.0f;
                a->is_swaping = 0;
                a->swap_state = 0;
            }
        }
    }
}

/* ---------------------------------------------------------------- Player */
/* Player.Is_ground: Physics.CheckCapsule(center, (center.x, bounds.min.y - 0.1, center.z), radius) */
static int player_is_ground(const CL_AimSystem *a, float radius_scale)
{
    Vector3 c = {a->pos.x, a->pos.y + CC_CENTER_Y, a->pos.z};
    float min_y = c.y - fmaxf(CC_HEIGHT, 2.0f * CC_RADIUS) * 0.5f;
    Vector3 b = {c.x, min_y + GROUND_PROBE, c.z};
    return cl_check_capsule(a->world, c, b, CC_RADIUS * radius_scale, MASK_NOT_PLAYER);
}

/* CharacterController.Move: sphere collide-and-slide against the static colliders. */
static void cc_move(CL_AimSystem *a, Vector3 motion)
{
    float len = Vector3Length(motion);
    if (len < 0.001f) return; /* minMoveDistance */
    int steps = (int)ceilf(len / (CC_RADIUS * 0.5f));
    Vector3 step = Vector3Scale(motion, 1.0f / (float)steps);
    for (int i = 0; i < steps; i++) {
        Vector3 c = {a->pos.x + step.x, a->pos.y + CC_CENTER_Y + step.y, a->pos.z + step.z};
        int ground = 0;
        cl_sphere_depenetrate(a->world, &c, CC_RADIUS, MASK_NOT_PLAYER, CC_SLOPE_LIMIT, &ground);
        a->pos = (Vector3){c.x, c.y - CC_CENTER_Y, c.z};
    }
}

/* Player.jump */
static void player_jump(CL_AimSystem *a)
{
    if (a->j_timer < PLAYER_JUMP_TIMER) return;
    a->is_courch = 0;
    if (player_is_ground(a, 0.5f)) a->vec_y = JUMP_VELOCITY;
    a->j_timer = 0.0f;
}

static float smooth_axis(float v, float input, float dt)
{
    if (input > 0.5f) {
        if (v < 1.0f) v += dt * PLAYER_MOV_SMOOTH;
    } else if (input < -0.5f) {
        if (v > -1.0f) v -= dt * PLAYER_MOV_SMOOTH;
    } else {
        if (v > 0.0f) v -= dt * PLAYER_MOV_SMOOTH;
        if (v < 0.0f) v += dt * PLAYER_MOV_SMOOTH;
        if (fabsf(v) < 0.2f) v = 0.0f;
    }
    return v;
}

/* Player.movement + Player.Update */
static void player_update(CL_AimSystem *a, const CL_AimInput *in, float dt)
{
    if (in->crouch_pressed) a->is_courch ^= 1;
    if (in->jump_pressed) player_jump(a);
    /* movement() */
    a->grounded = player_is_ground(a, 1.0f);
    if (a->grounded && a->vec_y < 0.0f) a->vec_y = -2.0f;
    a->vec_y += dt * GRAVITY;
    if (a->j_timer < PLAYER_JUMP_TIMER) a->j_timer += dt;
    float movespeed = (a->isaim || a->isfire || a->is_courch) ? PLAYER_WALKSPEED : PLAYER_SPEED;
    Vector2 m = {a->xv, a->yv};
    float ml = sqrtf(m.x * m.x + m.y * m.y);
    m = ml > 1e-5f ? (Vector2){m.x / ml, m.y / ml} : (Vector2){0, 0}; /* Vector2.normalized */
    a->moving = ml > 1e-5f;
    Quaternion pr = player_rot(a);
    Vector3 right = cl_quat_rotate(pr, (Vector3){1, 0, 0}), fwd = cl_quat_rotate(pr, (Vector3){0, 0, 1});
    cc_move(a, Vector3Scale(Vector3Add(Vector3Scale(right, m.x), Vector3Scale(fwd, m.y)), movespeed * dt));
    cc_move(a, (Vector3){0, a->vec_y * dt, 0});
    /* look: Fixedf.TouchDist drives the body yaw (and the animator's spine pitch) */
    a->p_assist_val = a->aim_assist ? 0.5f : 1.0f;
    float s1 = a->sensitivity / SENS_DIV, s2 = a->sensitivity2 / SENS_DIV;
    float k = (a->isaim && a->scope_no == 1) ? s2 : s1 + s1;
    a->p_yax += in->touch_dist.x * TOUCH_SCALE * a->p_assist_val * k;
    a->p_xax = Clamp(a->p_xax - in->touch_dist.y * TOUCH_SCALE * a->p_assist_val * k, MAXROT_MIN, MAXROT_MAX);
    /* Update(): joystick smoothing, applied for the next frame */
    a->yv = smooth_axis(a->yv, in->joystick.y, dt);
    a->xv = smooth_axis(a->xv, in->joystick.x, dt);
}

/* ---------------------------------------------------------------- CameraController.Update */
static void camera_update(CL_AimSystem *a, const CL_AimInput *in, float dt)
{
    Vector3 cam_pos = cam_world(a);
    Vector3 target = player_point(a, CAMERATARGET);
    Vector3 fwd = cam_forward(a);
    Vector3 camorgpos = Vector3Subtract(target, Vector3Scale(fwd, CAM_PDIS));
    a->scope_no = a->w_no;
    a->c_assist_val = a->aim_assist ? 0.5f : 1.0f;
    float s1 = a->sensitivity / SENS_DIV, s2 = a->sensitivity2 / SENS_DIV;
    float k = (a->isaim && a->scope_no == 1) ? s2 : s1 + s1;
    a->c_xax -= in->touch_dist.y * TOUCH_SCALE * a->c_assist_val * k;
    a->c_yax += in->touch_dist.x * TOUCH_SCALE * a->c_assist_val * k;
    a->c_xax = Clamp(a->c_xax, -35.0f, 25.0f);
    float t = Clamp(CAM_DUMPING * dt, 0.0f, 1.0f);
    a->targetrot.x += ((a->c_xax - CAM_POFF_X) - a->targetrot.x) * t;
    a->targetrot.y += ((a->c_yax - CAM_POFF_Y) - a->targetrot.y) * t;
    a->targetrot.z += (0.0f - a->targetrot.z) * t;
    a->cam_rot = euler_to_quat(a->targetrot);                     /* transform.eulerAngles = targetrot */
    fwd = cam_forward(a);
    Vector3 side = {0, 0, 0};                                      /* test (0,0,0) / target.right * 0 */
    Vector3 crouch = a->is_courch ? Vector3Scale(cl_quat_rotate(player_rot(a), (Vector3){0, 1, 0}), CROUCH_CAM_DROP)
                                  : (Vector3){0, 0, 0};
    Vector3 goal = Vector3Subtract(Vector3Subtract(Vector3Subtract(target, side), crouch), Vector3Scale(fwd, a->dis));
    float t2 = Clamp(CAM_MOVEDUMP * dt, 0.0f, 1.0f);
    cam_pos = Vector3Add(cam_pos, Vector3Scale(Vector3Subtract(goal, cam_pos), t2));
    /* store back as the child's local position */
    a->cam_local = cl_quat_rotate(QuaternionInvert(player_rot(a)), Vector3Subtract(cam_pos, a->pos));
    if (a->cn_cam < 1.0f) a->cn_cam += dt;
    if (!a->isaim) {
        a->fov = a->nor_fov;
        a->dis = a->isfire ? fminf(a->normal, CAM_SEMI_AIM) : a->normal;
    } else {
        if (a->scope_no == 1) {
            if (fabsf(a->fov - SCOPE_FOV) < 2.0f || a->fov < SCOPE_FOV) a->fov = SCOPE_FOV;
            if (a->fov > SCOPE_FOV) a->fov += dt * SCOPE_FOV_SPEED;
        } else {
            a->fov = a->nor_fov;
        }
        a->dis = CAM_AIMDIS;
    }
    /* collision: Physics.SphereCast(radius 0.01) between the head (camtar2) and the unobstructed
     * camera point, evaluated as a ray (the 1 cm radius is below the collider precision) */
    Vector3 body = player_point(a, BODY_TAR);
    Vector3 to_cam = Vector3Subtract(camorgpos, body);
    CL_RaycastHit h;
    int hit = cl_raycast(a->world, body, to_cam, Vector3Length(to_cam), MASK_NOT_PLAYER, &h);
    if (!hit) {
        Vector3 to_body = Vector3Subtract(body, camorgpos);
        hit = cl_raycast(a->world, camorgpos, to_body, Vector3Length(to_body), MASK_NOT_PLAYER, &h);
    }
    a->normal = hit ? Vector3Distance(body, h.point) + CAM_HIT_OFFSET : CAM_PDIS;
}

/* ---------------------------------------------------------------- public API */
void cl_aim_init(CL_AimSystem *a, const CL_World *w, const char *assets_dir)
{
    memset(a, 0, sizeof(*a));
    a->world = w;
    for (int s = 0; s < 2; s++)
        for (int i = 0; i < 7; i++) a->current_bul[s][i] = cl_weapons[i].mag;
    a->sensitivity = DEFAULT_CAM_SENS;
    a->sensitivity2 = DEFAULT_CAM_SENS2;
    a->reload_time = FIRE_RELOAD_TIME;
    a->granade_time = FIRE_GRANADE_TIME;
    a->total_b = AMMO_RESERVE;
    a->tot_g = 5;
    a->invoke_reload_at = -1.0f;
    cl_aim_select_weapon(a, 0, 1); /* default PlayerData: wid 0 (AK-47), swid as saved; L96 here */
    char path[1024];
    snprintf(path, sizeof(path), "%s/hud/scope4.png", assets_dir);
    a->scope_tex = LoadTexture(path);
    snprintf(path, sizeof(path), "%s/hud/scope_crosshair.png", assets_dir);
    a->scope_cross_tex = LoadTexture(path);
    SetTextureFilter(a->scope_tex, TEXTURE_FILTER_BILINEAR);
    SetTextureFilter(a->scope_cross_tex, TEXTURE_FILTER_BILINEAR);
}

void cl_aim_free(CL_AimSystem *a)
{
    UnloadTexture(a->scope_tex);
    UnloadTexture(a->scope_cross_tex);
}

void cl_aim_spawn(CL_AimSystem *a, int spawn, Vector3 pos, float yaw)
{
    if (spawn >= 0) {
        a->pos = cl_spawns[spawn % 7].pos;
        yaw = 0.0f; /* Player.Start never reads the spawn rotation: eulerAngles = (0, yax = 0, 0) */
    } else {
        a->pos = pos;
    }
    a->p_yax = yaw;
    a->c_yax = yaw;
    a->p_xax = a->c_xax = 0.0f;
    a->targetrot = (Vector3){-CAM_POFF_X, yaw - CAM_POFF_Y, 0};
    a->cam_rot = euler_to_quat(a->targetrot);
    a->cam_local = (Vector3){0, 2.26999998f, 0}; /* Main Camera local position */
    a->dis = a->normal = CAM_PDIS;
    a->nor_fov = a->fov = CAM_FOV;
    a->isaim = 0;
    a->vec_y = 0.0f;
    a->j_timer = PLAYER_JUMP_TIMER;
    a->r_c = a->reload_time;
}

CL_AimInput cl_aim_read_input(void)
{
    CL_AimInput in = {0};
    /* PC mapping of the touch controls: mouse = right-half drag, WASD = joystick */
    in.touch_dist = GetMouseDelta();
    in.touch_dist.y = -in.touch_dist.y; /* screen y grows downwards, touch y upwards */
    in.joystick.x = (float)(IsKeyDown(KEY_D) - IsKeyDown(KEY_A));
    in.joystick.y = (float)(IsKeyDown(KEY_W) - IsKeyDown(KEY_S));
    float l = sqrtf(in.joystick.x * in.joystick.x + in.joystick.y * in.joystick.y);
    if (l > 1.0f) in.joystick = (Vector2){in.joystick.x / l, in.joystick.y / l}; /* VJHandler clamps to 1 */
    in.fire_held = IsMouseButtonDown(MOUSE_BUTTON_LEFT);
    in.aim_pressed = IsMouseButtonPressed(MOUSE_BUTTON_RIGHT);
    in.scope_pressed = IsKeyPressed(KEY_F);
    in.reload_pressed = IsKeyPressed(KEY_R);
    in.crouch_pressed = IsKeyPressed(KEY_C);
    in.jump_pressed = IsKeyPressed(KEY_SPACE);
    in.swap_pressed = IsKeyPressed(KEY_Q);
    in.pick_pressed = IsKeyPressed(KEY_E);
    return in;
}

void cl_aim_update(CL_AimSystem *a, const CL_AimInput *in, float dt)
{
    a->time += dt;
    for (int k = 0; k < 7; k++)
        if (IsKeyPressed(KEY_ONE + k)) cl_aim_select_weapon(a, k, a->id2);
    if (IsKeyPressed(KEY_H)) a->show_center_dot ^= 1;
    /* buttons */
    a->keep_firing = in->fire_held;
    if (in->aim_pressed) a->isaim = (!a->isaim && !a->is_reloding) ? 1 : 0; /* setting_aim */
    if (in->scope_pressed) wm_change_scope(a);
    a->near_ammo_box = cl_trigger_at(Vector3Add(a->pos, (Vector3){0, CC_CENTER_Y, 0})) >= 0;
    if (in->pick_pressed && a->near_ammo_box) a->total_b = AMMO_RESERVE, a->tot_g = 5; /* fill_amo */
    if (a->invoke_reload_at >= 0.0f && a->time >= a->invoke_reload_at) {
        a->invoke_reload_at = -1.0f;
        fire_reload(a);
    }
    /* script Update order: Player, fire (+ coroutines), CameraController */
    player_update(a, in, dt);
    fire_swap_update(a, in, dt);
    fire_update(a, in, dt);
    camera_update(a, in, dt);
}

void cl_aim_camera(const CL_AimSystem *a, CL_Camera *cam)
{
    cam->position = cam_world(a);
    cam->rotation = a->cam_rot;
    cam->fov = a->fov;
    cam->near_plane = CAM_NEAR;
    cam->far_plane = CAM_FAR;
}

void cl_aim_draw_world(const CL_AimSystem *a, const CL_Camera *cam)
{
    /* bullet impacts (stand-in for the hitefct particle/decal prefab) */
    Camera3D c3 = {0};
    c3.position = cl_u2gl(cam->position);
    c3.target = Vector3Add(c3.position, cl_u2gl(cl_quat_rotate(cam->rotation, (Vector3){0, 0, 1})));
    c3.up = cl_u2gl(cl_quat_rotate(cam->rotation, (Vector3){0, 1, 0}));
    c3.fovy = cam->fov;
    c3.projection = CAMERA_PERSPECTIVE;
    rlSetClipPlanes(cam->near_plane, cam->far_plane);
    BeginMode3D(c3);
    for (int i = 0; i < 64; i++) {
        const CL_Impact *im = &a->impacts[i];
        if (im->time <= 0.0f || a->time - im->time > 10.0f) continue;
        Vector3 p = cl_u2gl(Vector3Add(im->point, Vector3Scale(im->normal, 0.01f)));
        DrawSphere(p, 0.02f, (Color){20, 20, 20, 255});
    }
    EndMode3D();
}

void cl_aim_draw_hud(const CL_AimSystem *a, int w, int h)
{
    /* sniper scope canvas: CanvasScaler 800x600, match height -> scale = h / 600 */
    if (a->isaim && a->scope_no == 1) {
        float s = (float)h / 600.0f;
        Vector2 c = {w * 0.5f, h * 0.5f};
        Rectangle src2 = {0, 0, (float)a->scope_tex.width, (float)a->scope_tex.height};
        Rectangle dst2 = {c.x + 10.0f * s - 2061.8f * s * 0.5f, c.y - 190.0f * s - 1162.8f * s * 0.5f, 2061.8f * s, 1162.8f * s};
        DrawTexturePro(a->scope_tex, src2, dst2, (Vector2){0, 0}, 0, WHITE);
        Rectangle src1 = {0, 0, (float)a->scope_cross_tex.width, (float)a->scope_cross_tex.height};
        Rectangle dst1 = {c.x - 1.8f * s - 457.7f * s * 0.5f, c.y + 1.9f * s - 466.7f * s * 0.5f, 457.7f * s, 466.7f * s};
        DrawTexturePro(a->scope_cross_tex, src1, dst1, (Vector2){0, 0}, 0, WHITE);
    }
    if (a->show_center_dot) DrawCircle(w / 2, h / 2, 2, (Color){255, 255, 255, 200});
    const CL_Weapon *g = current_weapon(a);
    DrawText(TextFormat("%s  %d / %d%s%s", g->name, a->curr_b, a->total_b, a->is_reloding ? "  RELOADING" : "",
                        a->is_swaping ? "  SWAP" : ""), 10, h - 30, 20, RAYWHITE);
    DrawText(TextFormat("aim %d  scope %d  fov %.1f  pitch %.2f  yaw %.2f  dis %.2f  shots %d", a->isaim, a->scope_no,
                        a->fov, a->c_xax, a->c_yax, a->dis, a->shots), 10, 10, 18, RAYWHITE);
    if (a->near_ammo_box) DrawText("PICK [E]", w / 2 - 40, h / 2 - 70, 20, RAYWHITE);
}
