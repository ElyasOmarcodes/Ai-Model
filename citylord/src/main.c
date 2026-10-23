/* CityLord map viewer (Battle of Agents, map3.unity) in C with raylib.
 *
 *   citylord [--assets DIR] [--quality 0..5] [--screenshot out.png] [--cam x y z yaw pitch]
 *
 * Controls (free camera): mouse = look, WASD = move, Q/E = down/up, Shift = fast,
 *                         Tab = switch to the in-game player/aim mode, F1..F6 = quality level.
 */
#include "raylib.h"
#include "raymath.h"
#include "citylord_data.h"
#include "cl_assets.h"
#include "cl_render.h"
#include "cl_physics.h"
#include "cl_aim.h"

#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static Quaternion yaw_pitch(float yaw, float pitch)
{
    /* Unity Quaternion.Euler(pitch, yaw, 0): rotate around Y then X (ZXY order) */
    Quaternion qy = QuaternionFromAxisAngle((Vector3){0, 1, 0}, yaw * DEG2RAD);
    Quaternion qx = QuaternionFromAxisAngle((Vector3){1, 0, 0}, pitch * DEG2RAD);
    return QuaternionMultiply(qy, qx);
}

int main(int argc, char **argv)
{
    const char *assets_dir = "assets";
    const char *shot = NULL;
    int quality_level = 3; /* "High": the QualitySettings entry the game ships as current */
    int have_cam = 0;
    float cam_args[5] = {0};
    int start_aim = 0, spawn = -1, test_frames = 0;
    for (int i = 1; i < argc; i++) {
        if (!strcmp(argv[i], "--assets") && i + 1 < argc) assets_dir = argv[++i];
        else if (!strcmp(argv[i], "--quality") && i + 1 < argc) quality_level = atoi(argv[++i]);
        else if (!strcmp(argv[i], "--screenshot") && i + 1 < argc) shot = argv[++i];
        else if (!strcmp(argv[i], "--aim")) start_aim = 1;
        else if (!strcmp(argv[i], "--spawn") && i + 1 < argc) spawn = atoi(argv[++i]), start_aim = 1;
        else if (!strcmp(argv[i], "--aim-test") && i + 1 < argc) test_frames = atoi(argv[++i]), start_aim = 1;
        else if (!strcmp(argv[i], "--cam") && i + 5 < argc) {
            for (int k = 0; k < 5; k++) cam_args[k] = (float)atof(argv[++i]);
            have_cam = 1;
        }
    }
    CL_Quality quality = cl_quality_from_level(quality_level);
    if (quality.anti_aliasing) SetConfigFlags(FLAG_MSAA_4X_HINT);
    SetConfigFlags(FLAG_WINDOW_RESIZABLE | FLAG_VSYNC_HINT);
    InitWindow(1280, 720, "CityLord - Battle of Agents map (C port)");
    CL_Assets assets;
    if (!cl_assets_load(&assets, assets_dir)) {
        TraceLog(LOG_ERROR, "CL: assets not found in '%s' (run tools/extract_citylord.sh first)", assets_dir);
        CloseWindow();
        return 1;
    }
    CL_Renderer_t *renderer = cl_render_create(&assets, quality);
    CL_World world;
    cl_world_build(&world, &assets);
    CL_AimSystem aim;
    cl_aim_init(&aim, &world, assets_dir);

    const CL_SceneSettings *s = &cl_settings;
    CL_Camera cam = {
        .position = {s->camera_position[0], s->camera_position[1], s->camera_position[2]},
        .rotation = {s->camera_rotation[0], s->camera_rotation[1], s->camera_rotation[2], s->camera_rotation[3]},
        .fov = s->camera_fov, .near_plane = s->camera_near, .far_plane = s->camera_far};
    /* derive yaw/pitch from the scene camera so the free camera starts where Unity's does */
    Vector3 f0 = cl_quat_rotate(cam.rotation, (Vector3){0, 0, 1});
    float yaw = atan2f(f0.x, f0.z) * RAD2DEG, pitch = -asinf(f0.y) * RAD2DEG;
    if (have_cam) {
        cam.position = (Vector3){cam_args[0], cam_args[1], cam_args[2]};
        yaw = cam_args[3];
        pitch = cam_args[4];
    }
    cam.rotation = yaw_pitch(yaw, pitch);
    int aim_mode = start_aim;
    if (aim_mode) cl_aim_spawn(&aim, spawn, cam.position, yaw);
    if (!shot) DisableCursor();

    int frames = 0;
    while (!WindowShouldClose()) {
        float dt = GetFrameTime();
        for (int k = KEY_F1; k <= KEY_F6; k++)
            if (IsKeyPressed(k)) cl_render_set_quality(renderer, cl_quality_from_level(k - KEY_F1));
        if (IsKeyPressed(KEY_TAB)) {
            aim_mode = !aim_mode;
            if (aim_mode) cl_aim_spawn(&aim, -1, cam.position, yaw);
        }
        if (aim_mode) {
            CL_AimInput in = shot ? (CL_AimInput){0} : cl_aim_read_input();
            if (test_frames) {
                /* scripted run (fixed 60 fps): turn right, aim, fire a burst, then scope in */
                dt = 1.0f / 60.0f;
                in.touch_dist = frames < 30 ? (Vector2){4.0f, 0.5f} : (Vector2){0, 0};
                in.joystick = frames < 60 ? (Vector2){0, 1} : (Vector2){0, 0};
                in.aim_pressed = frames == 70;
                in.fire_held = frames >= 80 && frames < 140;
                if (frames == 150) cl_aim_select_weapon(&aim, 1, 0);
                in.aim_pressed |= frames == 151 || frames == 152;
            }
            cl_aim_update(&aim, &in, dt);
            cl_aim_camera(&aim, &cam);
        } else {
            Vector2 md = shot ? (Vector2){0, 0} : GetMouseDelta();
            yaw += md.x * 0.15f;
            pitch += md.y * 0.15f;
            pitch = Clamp(pitch, -89.0f, 89.0f);
            cam.rotation = yaw_pitch(yaw, pitch);
            Vector3 fwd = cl_quat_rotate(cam.rotation, (Vector3){0, 0, 1});
            Vector3 right = cl_quat_rotate(cam.rotation, (Vector3){1, 0, 0});
            float sp = (IsKeyDown(KEY_LEFT_SHIFT) ? 25.0f : 6.0f) * dt;
            if (IsKeyDown(KEY_W)) cam.position = Vector3Add(cam.position, Vector3Scale(fwd, sp));
            if (IsKeyDown(KEY_S)) cam.position = Vector3Subtract(cam.position, Vector3Scale(fwd, sp));
            if (IsKeyDown(KEY_D)) cam.position = Vector3Add(cam.position, Vector3Scale(right, sp));
            if (IsKeyDown(KEY_A)) cam.position = Vector3Subtract(cam.position, Vector3Scale(right, sp));
            if (IsKeyDown(KEY_E)) cam.position.y += sp;
            if (IsKeyDown(KEY_Q)) cam.position.y -= sp;
            cam.fov = s->camera_fov;
        }

        BeginDrawing();
        ClearBackground((Color){(unsigned char)(s->camera_background[0] * 255), (unsigned char)(s->camera_background[1] * 255),
                                (unsigned char)(s->camera_background[2] * 255), 255});
        cl_render_frame(renderer, &cam, GetScreenWidth(), GetScreenHeight());
        if (aim_mode) {
            cl_aim_draw_world(&aim, &cam);
            cl_aim_draw_hud(&aim, GetScreenWidth(), GetScreenHeight());
        }
        else DrawText(TextFormat("free camera  pos %.2f %.2f %.2f  yaw %.1f pitch %.1f  [Tab] aim mode",
                                 cam.position.x, cam.position.y, cam.position.z, yaw, pitch), 10, 10, 18, RAYWHITE);
        EndDrawing();
        ++frames;
        if (test_frames && (frames == 140 || frames == test_frames)) {
            printf("frame %d pos %.3f %.3f %.3f grounded %d cam %.3f %.3f %.3f pitch %.3f yaw %.3f fov %.2f ammo %d/%d shots %d aim %d scope %d\n",
                   frames, aim.pos.x, aim.pos.y, aim.pos.z, aim.grounded, cam.position.x, cam.position.y, cam.position.z,
                   aim.c_xax, aim.c_yax, aim.fov, aim.curr_b, aim.total_b, aim.shots, aim.isaim, aim.scope_no);
            if (shot) TakeScreenshot(TextFormat("%s_%d.png", shot, frames));
            if (frames == test_frames) break;
        } else if (shot && !test_frames && frames == 3) {
            TakeScreenshot(shot);
            break;
        }
    }
    cl_aim_free(&aim);
    cl_world_free(&world);
    cl_render_destroy(renderer);
    cl_assets_unload(&assets);
    CloseWindow();
    return 0;
}
