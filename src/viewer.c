#include "viewer.h"

#include <math.h>
#include <stdlib.h>
#include <string.h>

#include "raylib.h"
#include "raymath.h"
#include "world3d.h"

#define EYE_HEIGHT 2.5f /* world units (minitiles); about Ness's eye level */

/* Defaults are the user's Phase 1 picks (D21): 640x360, facades, light fog. */
static W3dOptions opt = {{4, 0, 2, 0, 0}};
static Vector3 pos;
static float yaw, pitch; /* radians; yaw 0 looks north (-z) */
static RenderTexture2D target;
static int target_w, target_h;

void viewer_init(void) { w3d_init(); }

void viewer_shutdown(void)
{
    if (target.id) UnloadRenderTexture(target);
    target = (RenderTexture2D){0};
    w3d_shutdown();
}

bool viewer_config_read(const char *key, const char *value)
{
    for (int i = 0; i < OPT_COUNT; i++)
        if (!strcmp(key, w3d_option_key(i))) {
            int v = atoi(value);
            if (v >= 0 && v < w3d_option_count(i)) opt.value[i] = v;
            return true;
        }
    return false;
}

void viewer_config_write(FILE *f)
{
    for (int i = 0; i < OPT_COUNT; i++) fprintf(f, "%s=%d\n", w3d_option_key(i), opt.value[i]);
}

void viewer_place(float px, float py, int direction)
{
    pos = (Vector3){px / 8.0f, EYE_HEIGHT, py / 8.0f};
    pos.y = w3d_ground(pos.x, pos.z) + EYE_HEIGHT;
    yaw = (direction >= 0 && direction < 8) ? direction * PI / 4 : 0;
    pitch = 0;
}

void viewer_set_angles(float yaw_deg, float pitch_deg)
{
    yaw = yaw_deg * DEG2RAD;
    pitch = pitch_deg * DEG2RAD;
}

void viewer_set_height(float units) { pos.y = w3d_ground(pos.x, pos.z) + units; /* above the ground */ }

void viewer_set_option(int id, int value)
{
    if (id >= 0 && id < OPT_COUNT && value >= 0 && value < w3d_option_count(id) && opt.value[id] != value) {
        opt.value[id] = value;
        if (id == OPT_WALL_STYLE || id == OPT_PALETTE) w3d_invalidate();
    }
}

/* Renders the current view repeatedly until every chunk in sight is built, then saves it. */
bool viewer_snapshot(const char *path)
{
    for (int i = 0; i < 120; i++) {
        BeginDrawing();
        viewer_draw(false);
        EndDrawing();
        if (!w3d_pending()) break;
    }
    BeginDrawing();
    viewer_draw(false);
    EndDrawing();
    /* Save the low-res frame itself, at its native size. */
    Image img = LoadImageFromTexture(target.texture);
    ImageFlipVertical(&img);
    bool ok = ExportImage(img, path);
    UnloadImage(img);
    return ok;
}

static Vector3 forward(void) { return (Vector3){sinf(yaw), 0, -cosf(yaw)}; }

bool viewer_update(float dt)
{
    bool changed = false;
    static const int keys[OPT_COUNT] = {KEY_ONE, KEY_TWO, KEY_THREE, KEY_FOUR, KEY_FIVE};
    for (int i = 0; i < OPT_COUNT; i++)
        if (IsKeyPressed(keys[i])) {
            opt.value[i] = (opt.value[i] + 1) % w3d_option_count(i);
            if (i == OPT_WALL_STYLE || i == OPT_PALETTE) w3d_invalidate();
            changed = true;
        }

    float speed = (IsKeyDown(KEY_LEFT_SHIFT) ? 40.0f : 12.0f) * dt;
    float turn = 2.2f * dt;
    Vector3 f = forward(), r = {-f.z, 0, f.x};
    Vector3 move = {0};
    if (IsKeyDown(KEY_W)) move = Vector3Add(move, f);
    if (IsKeyDown(KEY_S)) move = Vector3Subtract(move, f);
    if (IsKeyDown(KEY_D)) move = Vector3Add(move, r);
    if (IsKeyDown(KEY_A)) move = Vector3Subtract(move, r);
    if (IsKeyDown(KEY_LEFT)) yaw -= turn;
    if (IsKeyDown(KEY_RIGHT)) yaw += turn;
    if (IsKeyDown(KEY_UP)) pitch += turn * 0.6f;
    if (IsKeyDown(KEY_DOWN)) pitch -= turn * 0.6f;
    if (IsKeyDown(KEY_SPACE) || IsKeyDown(KEY_E)) pos.y += speed;
    if (IsKeyDown(KEY_C) || IsKeyDown(KEY_Q)) pos.y -= speed;

    if (IsMouseButtonDown(MOUSE_BUTTON_RIGHT)) {
        Vector2 d = GetMouseDelta();
        yaw += d.x * 0.004f;
        pitch -= d.y * 0.004f;
    }
    for (int g = 0; g < 4; g++) {
        if (!IsGamepadAvailable(g)) continue;
        float lx = GetGamepadAxisMovement(g, GAMEPAD_AXIS_LEFT_X), ly = GetGamepadAxisMovement(g, GAMEPAD_AXIS_LEFT_Y);
        float rx = GetGamepadAxisMovement(g, GAMEPAD_AXIS_RIGHT_X), ry = GetGamepadAxisMovement(g, GAMEPAD_AXIS_RIGHT_Y);
        if (fabsf(lx) > 0.2f) move = Vector3Add(move, Vector3Scale(r, lx));
        if (fabsf(ly) > 0.2f) move = Vector3Subtract(move, Vector3Scale(f, ly));
        if (fabsf(rx) > 0.2f) yaw += rx * turn;
        if (fabsf(ry) > 0.2f) pitch -= ry * turn * 0.6f;
        if (IsGamepadButtonDown(g, GAMEPAD_BUTTON_RIGHT_TRIGGER_1)) pos.y += speed;
        if (IsGamepadButtonDown(g, GAMEPAD_BUTTON_LEFT_TRIGGER_1)) pos.y -= speed;
    }
    if (Vector3Length(move) > 1) move = Vector3Normalize(move);
    pos = Vector3Add(pos, Vector3Scale(move, speed));
    float floor_y = w3d_ground(pos.x, pos.z) + 0.5f;
    if (pos.y < floor_y) pos.y = floor_y;
    pitch = Clamp(pitch, -1.4f, 1.4f);
    return changed;
}

static void draw_panel(void)
{
    char buf[1024];
    int n = snprintf(buf, sizeof buf,
                     "3D WORLD VIEW  (V: back to game)\n"
                     "Move: WASD   Turn: arrows / hold right mouse\n"
                     "Up/down: Space, C   Fast: Shift   R: jump to Ness\n"
                     "Tab: hide this panel\n\n");
    for (int i = 0; i < OPT_COUNT && n < (int)sizeof buf; i++)
        n += snprintf(buf + n, sizeof buf - n, "%d  %-20s %s\n", i + 1, w3d_option_name(i),
                      w3d_option_value_name(i, opt.value[i]));
    snprintf(buf + n, sizeof buf - n, "\nPosition: %d, %d (game pixels)   %d fps", (int)(pos.x * 8),
             (int)(pos.z * 8), GetFPS());
    DrawRectangle(8, 8, 560, 330, Fade(BLACK, 0.7f));
    DrawText(buf, 18, 18, 18, RAYWHITE);
}

void viewer_draw(bool show_panel)
{
    int w, h;
    float aspect;
    w3d_resolution(&opt, &w, &h, &aspect);
    if (!target.id || w != target_w || h != target_h) {
        if (target.id) UnloadRenderTexture(target);
        target = LoadRenderTexture(w, h);
        SetTextureFilter(target.texture, TEXTURE_FILTER_POINT);
        target_w = w;
        target_h = h;
    }

    Vector3 look = {sinf(yaw) * cosf(pitch), sinf(pitch), -cosf(yaw) * cosf(pitch)};
    Camera3D cam = {pos, Vector3Add(pos, look), {0, 1, 0}, 70.0f, CAMERA_PERSPECTIVE};

    BeginTextureMode(target);
    w3d_draw(cam, &opt);
    EndTextureMode();

    float sw = (float)GetScreenWidth(), sh = (float)GetScreenHeight();
    float dw = sw, dh = sw / aspect;
    if (dh > sh) {
        dh = sh;
        dw = sh * aspect;
    }
    ClearBackground(BLACK);
    /* Render textures are stored upside down. */
    DrawTexturePro(target.texture, (Rectangle){0, 0, (float)w, (float)-h},
                   (Rectangle){(sw - dw) / 2, (sh - dh) / 2, dw, dh}, (Vector2){0, 0}, 0, WHITE);
    if (show_panel) draw_panel();
    else DrawText("Tab: show controls and options", 10, GetScreenHeight() - 24, 16, Fade(RAYWHITE, 0.6f));
}
