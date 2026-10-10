/* First-Person EarthBound — Phase 0: runs EarthBound through a libretro core with a live RAM
 * debug panel. Later phases replace the emulator picture with the 3D first-person view. */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "raylib.h"

#include "eb_ram.h"
#include "eb_world.h"
#include "log.h"
#include "lr_host.h"
#include "rom_check.h"
#include "viewer.h"

#define APP_TITLE "First-Person EarthBound"
#define CONFIG_FILE "fpeb.ini"
#define AUDIO_CHUNK 1024 /* stereo frames per audio stream update */
#define SRAM_FLUSH_FRAMES 300

#ifdef _WIN32
#define CORE_NAME "snes9x_libretro.dll"
#else
#define CORE_NAME "snes9x_libretro.so"
#endif

typedef struct {
    char rom[1024];
    char core[1024];
    int debug; /* show the debug panel */
} Config;

static Config cfg = {.debug = 1};
static char status[512];
static bool world_ok; /* overworld data decoded from the ROM */

/* Where the 3D view starts when Ness isn't on the overworld yet: outside his house. */
#define NESS_HOUSE_PX 2400
#define NESS_HOUSE_PY 400

/* ---- config file: plain key=value lines ---- */

static void config_load(void)
{
    FILE *f = fopen(CONFIG_FILE, "r");
    if (!f) return;
    char line[1200];
    while (fgets(line, sizeof line, f)) {
        line[strcspn(line, "\r\n")] = 0;
        char *eq = strchr(line, '=');
        if (!eq) continue;
        *eq = 0;
        if (strcmp(line, "rom") == 0) snprintf(cfg.rom, sizeof cfg.rom, "%s", eq + 1);
        if (strcmp(line, "core") == 0) snprintf(cfg.core, sizeof cfg.core, "%s", eq + 1);
        if (strcmp(line, "debug") == 0) cfg.debug = atoi(eq + 1) != 0;
        viewer_config_read(line, eq + 1);
    }
    fclose(f);
}

static void config_save(void)
{
    FILE *f = fopen(CONFIG_FILE, "w");
    if (!f) return;
    fprintf(f, "rom=%s\ncore=%s\ndebug=%d\n", cfg.rom, cfg.core, cfg.debug);
    viewer_config_write(f);
    fclose(f);
}

/* Looks for the Snes9x core in the usual RetroArch install locations. */
static void find_core(void)
{
    if (cfg.core[0] && FileExists(cfg.core)) return;
    char candidates[8][1024];
    int n = 0;
    snprintf(candidates[n++], 1024, "%s", CORE_NAME);
#ifdef _WIN32
    const char *appdata = getenv("APPDATA");
    if (appdata) snprintf(candidates[n++], 1024, "%s\\RetroArch\\cores\\" CORE_NAME, appdata);
    snprintf(candidates[n++], 1024, "C:\\RetroArch-Win64\\cores\\" CORE_NAME);
    snprintf(candidates[n++], 1024, "C:\\RetroArch\\cores\\" CORE_NAME);
    snprintf(candidates[n++], 1024, "C:\\Program Files\\RetroArch\\cores\\" CORE_NAME);
    snprintf(candidates[n++], 1024,
             "C:\\Program Files (x86)\\Steam\\steamapps\\common\\RetroArch\\cores\\" CORE_NAME);
#else
    const char *home = getenv("HOME");
    if (home) snprintf(candidates[n++], 1024, "%s/.config/retroarch/cores/" CORE_NAME, home);
#endif
    for (int i = 0; i < n; i++) {
        if (FileExists(candidates[i])) {
            snprintf(cfg.core, sizeof cfg.core, "%s", candidates[i]);
            log_msg("Found core at %s\n", cfg.core);
            return;
        }
    }
    cfg.core[0] = 0;
}

/* ---- input: RetroArch's default keyboard layout plus any gamepad ---- */

static uint16_t read_buttons(void)
{
    static const struct { int key; int pad; int bit; } map[] = {
        {KEY_UP, GAMEPAD_BUTTON_LEFT_FACE_UP, PAD_UP},
        {KEY_DOWN, GAMEPAD_BUTTON_LEFT_FACE_DOWN, PAD_DOWN},
        {KEY_LEFT, GAMEPAD_BUTTON_LEFT_FACE_LEFT, PAD_LEFT},
        {KEY_RIGHT, GAMEPAD_BUTTON_LEFT_FACE_RIGHT, PAD_RIGHT},
        {KEY_X, GAMEPAD_BUTTON_RIGHT_FACE_RIGHT, PAD_A},
        {KEY_Z, GAMEPAD_BUTTON_RIGHT_FACE_DOWN, PAD_B},
        {KEY_S, GAMEPAD_BUTTON_RIGHT_FACE_UP, PAD_X},
        {KEY_A, GAMEPAD_BUTTON_RIGHT_FACE_LEFT, PAD_Y},
        {KEY_Q, GAMEPAD_BUTTON_LEFT_TRIGGER_1, PAD_L},
        {KEY_W, GAMEPAD_BUTTON_RIGHT_TRIGGER_1, PAD_R},
        {KEY_ENTER, GAMEPAD_BUTTON_MIDDLE_RIGHT, PAD_START},
        {KEY_RIGHT_SHIFT, GAMEPAD_BUTTON_MIDDLE_LEFT, PAD_SELECT},
    };
    uint16_t mask = 0;
    for (size_t i = 0; i < sizeof map / sizeof map[0]; i++) {
        bool down = IsKeyDown(map[i].key);
        for (int g = 0; g < 4 && !down; g++)
            down = IsGamepadAvailable(g) && IsGamepadButtonDown(g, map[i].pad);
        if (down) mask |= (uint16_t)(1u << map[i].bit);
    }
    for (int g = 0; g < 4; g++) { /* left stick as d-pad */
        if (!IsGamepadAvailable(g)) continue;
        float x = GetGamepadAxisMovement(g, GAMEPAD_AXIS_LEFT_X);
        float y = GetGamepadAxisMovement(g, GAMEPAD_AXIS_LEFT_Y);
        if (x < -0.5f) mask |= 1u << PAD_LEFT;
        if (x > 0.5f) mask |= 1u << PAD_RIGHT;
        if (y < -0.5f) mask |= 1u << PAD_UP;
        if (y > 0.5f) mask |= 1u << PAD_DOWN;
    }
    return mask;
}

/* ---- save RAM (EarthBound's save files) ---- */

static char sram_path[1100], state_path[1200];
static uint8_t *sram_shadow;

static void sram_paths(void)
{
    snprintf(sram_path, sizeof sram_path, "%s", cfg.rom);
    char *dot = strrchr(sram_path, '.');
    if (dot) *dot = 0;
    snprintf(state_path, sizeof state_path, "%s.fpeb-state", sram_path);
    strcat(sram_path, ".srm");
}

static void sram_load(void)
{
    size_t size;
    uint8_t *sram = lr_sram(&size);
    if (!sram) return;
    FILE *f = fopen(sram_path, "rb");
    if (f) {
        size_t got = fread(sram, 1, size, f);
        fclose(f);
        log_msg("Loaded %zu bytes of save data from %s\n", got, sram_path);
    }
    sram_shadow = malloc(size);
    if (sram_shadow) memcpy(sram_shadow, sram, size);
}

static void sram_flush(void)
{
    size_t size;
    uint8_t *sram = lr_sram(&size);
    if (!sram || !sram_shadow || memcmp(sram, sram_shadow, size) == 0) return;
    FILE *f = fopen(sram_path, "wb");
    if (!f) return;
    fwrite(sram, 1, size, f);
    fclose(f);
    memcpy(sram_shadow, sram, size);
    log_msg("Saved game data to %s\n", sram_path);
}

/* ---- startup ---- */

static uint8_t *read_file(const char *path, size_t *size)
{
    FILE *f = fopen(path, "rb");
    if (!f) return NULL;
    fseek(f, 0, SEEK_END);
    long n = ftell(f);
    fseek(f, 0, SEEK_SET);
    uint8_t *buf = n > 0 ? malloc((size_t)n) : NULL;
    if (buf && fread(buf, 1, (size_t)n, f) != (size_t)n) {
        free(buf);
        buf = NULL;
    }
    fclose(f);
    *size = buf ? (size_t)n : 0;
    return buf;
}

/* Returns true once the game is running. On failure, explains why in `status` and forgets
 * whichever path was bad so the setup screen asks for it again. */
static bool start_game(bool allow_any_rom)
{
    char err[512];
    size_t size;
    uint8_t *data = read_file(cfg.rom, &size);
    if (!data) {
        snprintf(status, sizeof status, "Couldn't read the ROM file:\n%s", cfg.rom);
        cfg.rom[0] = 0;
        return false;
    }
    RomInfo info = rom_check(data, size);
    if (!info.ok && !allow_any_rom) {
        snprintf(status, sizeof status, "%s", info.message);
        cfg.rom[0] = 0;
        free(data);
        return false;
    }
    if (info.message[0]) log_msg("ROM note: %s\n", info.message);
    log_msg("ROM: \"%s\" header=%zu crc32=%08X\n", info.title, info.header_size, info.crc32);

    if (!lr_load_core(cfg.core, err, sizeof err)) {
        snprintf(status, sizeof status, "%s", err);
        cfg.core[0] = 0;
        free(data);
        return false;
    }
    if (!lr_load_game(cfg.rom, data + info.header_size, size - info.header_size, err, sizeof err)) {
        snprintf(status, sizeof status, "%s", err);
        cfg.rom[0] = 0;
        lr_shutdown();
        free(data);
        return false;
    }
    /* `data` stays allocated while the game runs: libretro lets the core keep pointing at it,
     * and the 3D world reads map data from it. */
    world_ok = ebw_load(data + info.header_size, size - info.header_size, err, sizeof err);
    if (!world_ok) log_msg("Map data unavailable: %s\n", err);
    sram_paths();
    sram_load();
    return true;
}

/* ---- drawing ---- */

static const char *direction_name(unsigned d)
{
    static const char *names[] = {"up", "up-right", "right", "down-right",
                                  "down", "down-left", "left", "up-left", "none"};
    return d < 9 ? names[d] : "?";
}

static void draw_debug_panel(void)
{
    char buf[1024];
    unsigned dir = lr_peek16(EB_LEADER_DIRECTION);
    snprintf(buf, sizeof buf,
             "Tab or F1: hide this panel\n"
             "F5: save state   F9: load state\n"
             "V: 3D world view\n"
             "\n"
             "Ness X        %5u\n"
             "Ness Y        %5u\n"
             "Facing        %u (%s)\n"
             "Walking style %u\n"
             "Party count   %u\n"
             "Battle flag   %u\n"
             "Battle mode   %u\n"
             "Frame counter %u\n"
             "Audio buffer  %zu",
             lr_peek16(EB_LEADER_X), lr_peek16(EB_LEADER_Y), dir, direction_name(dir),
             lr_peek16(EB_WALKING_STYLE), lr_peek8(EB_PARTY_COUNT), lr_peek16(EB_BATTLE_MODE_FLAG),
             lr_peek16(EB_BATTLE_MODE), lr_peek8(EB_FRAME_COUNTER), lr_audio_available());
    DrawRectangle(8, 8, 380, 290, Fade(BLACK, 0.75f));
    DrawText(buf, 18, 18, 18, RAYWHITE);
}

static void draw_debug_hint(void)
{
    DrawText("Tab: show debug info", 10, GetScreenHeight() - 24, 16, Fade(RAYWHITE, 0.5f));
}

static void draw_setup_screen(void)
{
    ClearBackground((Color){24, 16, 40, 255});
    int y = 60;
    DrawText(APP_TITLE, 40, y, 36, (Color){255, 220, 120, 255});
    y += 70;
    DrawText(cfg.core[0] ? "Emulator core: found" : "Emulator core: missing", 40, y, 22,
             cfg.core[0] ? GREEN : ORANGE);
    y += 30;
    if (!cfg.core[0]) {
        DrawText("Drag snes9x_libretro.dll onto this window.\n"
                 "It's in your RetroArch folder, inside \"cores\".\n"
                 "Not installed? In RetroArch: Online Updater > Core Downloader >\n"
                 "Nintendo - SNES / SFC (Snes9x).",
                 60, y, 18, RAYWHITE);
        y += 100;
    }
    y += 10;
    DrawText(cfg.rom[0] ? "EarthBound ROM: chosen" : "EarthBound ROM: missing", 40, y, 22,
             cfg.rom[0] ? GREEN : ORANGE);
    y += 30;
    if (!cfg.rom[0]) {
        DrawText("Drag your EarthBound (US) .sfc or .smc file onto this window.", 60, y, 18, RAYWHITE);
        y += 40;
    }
    if (status[0]) DrawText(status, 40, y + 20, 18, (Color){255, 120, 120, 255});
}

static void draw_game_texture(Texture2D tex, int w, int h)
{
    /* SNES pixels are slightly wide: 256x224 is shown at 4:3. */
    float sw = (float)GetScreenWidth(), sh = (float)GetScreenHeight();
    float aspect = 4.0f / 3.0f, dw = sw, dh = sw / aspect;
    if (dh > sh) {
        dh = sh;
        dw = sh * aspect;
    }
    Rectangle src = {0, 0, (float)w, (float)h};
    Rectangle dst = {(sw - dw) / 2, (sh - dh) / 2, dw, dh};
    DrawTexturePro(tex, src, dst, (Vector2){0, 0}, 0, WHITE);
}

/* ---- main ---- */

int main(int argc, char **argv)
{
    /* Test hooks for automated runs: --rom, --core, --frames N, --screenshot file, --any-rom,
     * --debug (start with the F1 panel open). Paths are relative to the program's folder. */
    long test_frames = -1;
    const char *screenshot = NULL;
    bool allow_any_rom = false, force_debug = false, view3d = false;
    float cam[4] = {-1, -1, 0, 0}; /* --cam px py yaw pitch */
    float cam_y = -1;                /* --cam-y units */

    /* Keep config, log, and saves next to the program even if launched from elsewhere. */
    ChangeDirectory(GetApplicationDirectory());
    log_open("fpeb.log");
    config_load();
    for (int i = 1; i < argc; i++) {
        if (!strcmp(argv[i], "--rom") && i + 1 < argc) snprintf(cfg.rom, sizeof cfg.rom, "%s", argv[++i]);
        else if (!strcmp(argv[i], "--core") && i + 1 < argc) snprintf(cfg.core, sizeof cfg.core, "%s", argv[++i]);
        else if (!strcmp(argv[i], "--frames") && i + 1 < argc) test_frames = atol(argv[++i]);
        else if (!strcmp(argv[i], "--screenshot") && i + 1 < argc) screenshot = argv[++i];
        else if (!strcmp(argv[i], "--any-rom")) allow_any_rom = true;
        else if (!strcmp(argv[i], "--debug")) force_debug = true;
        else if (!strcmp(argv[i], "--view3d")) view3d = true;
        else if (!strcmp(argv[i], "--cam") && i + 4 < argc)
            for (int k = 0; k < 4; k++) cam[k] = (float)atof(argv[++i]);
        else if (!strcmp(argv[i], "--cam-y") && i + 1 < argc) cam_y = (float)atof(argv[++i]);
        else if (!strcmp(argv[i], "--set") && i + 1 < argc) {
            char kv[128];
            snprintf(kv, sizeof kv, "%s", argv[++i]);
            char *eq = strchr(kv, '=');
            if (eq) {
                *eq = 0;
                viewer_config_read(kv, eq + 1);
            }
        }
    }
    if (force_debug) cfg.debug = 1;
    find_core();

    SetConfigFlags(FLAG_WINDOW_RESIZABLE);
    InitWindow(1024, 768, APP_TITLE);
    SetTargetFPS(60);
    SetExitKey(KEY_NULL);
    viewer_init();

    bool running = cfg.core[0] && cfg.rom[0] && start_game(allow_any_rom);
    if (running) config_save();
    view3d = view3d && world_ok;
    if (view3d) {
        if (cam[0] >= 0) {
            viewer_place(cam[0], cam[1], 0);
            viewer_set_angles(cam[2], cam[3]);
            if (cam_y > 0) viewer_set_height(cam_y);
        } else {
            viewer_place(NESS_HOUSE_PX, NESS_HOUSE_PY, 0);
        }
    }

    InitAudioDevice();
    AudioStream stream = {0};
    bool audio_ok = false;
    Texture2D tex = {0};
    int tex_w = 0, tex_h = 0;
    long frames = 0;
    int16_t chunk[AUDIO_CHUNK * 2];

    while (!WindowShouldClose()) {
        if (IsFileDropped()) {
            FilePathList dropped = LoadDroppedFiles();
            for (unsigned i = 0; i < dropped.count; i++) {
                const char *p = dropped.paths[i];
                if (IsFileExtension(p, ".dll;.so;.dylib")) snprintf(cfg.core, sizeof cfg.core, "%s", p);
                else snprintf(cfg.rom, sizeof cfg.rom, "%s", p);
            }
            UnloadDroppedFiles(dropped);
            status[0] = 0;
            if (!running && cfg.core[0] && cfg.rom[0]) {
                running = start_game(allow_any_rom);
                if (running) config_save();
            }
        }

        if (running && !audio_ok && IsAudioDeviceReady()) {
            SetAudioStreamBufferSizeDefault(AUDIO_CHUNK);
            stream = LoadAudioStream((unsigned)(lr_sample_rate() + 0.5), 16, 2);
            PlayAudioStream(stream);
            audio_ok = true;
        }

        if (running) {
            /* Tab as well as F1: many laptop keyboards need Fn for the F-keys. */
            if (IsKeyPressed(KEY_F1) || IsKeyPressed(KEY_TAB)) {
                cfg.debug = !cfg.debug;
                config_save();
            }
            if (IsKeyPressed(KEY_F5)) log_msg(lr_save_state(state_path) ? "State saved\n" : "State save failed\n");
            if (IsKeyPressed(KEY_F9)) log_msg(lr_load_state(state_path) ? "State loaded\n" : "No state to load\n");

            /* V: switch between the game and the 3D world view. R in 3D: jump to Ness. */
            bool to_ness = false;
            if (IsKeyPressed(KEY_V) && world_ok) {
                view3d = !view3d;
                to_ness = view3d;
            }
            if (view3d && IsKeyPressed(KEY_R)) to_ness = true;
            if (to_ness) {
                unsigned nx = lr_peek16(EB_LEADER_X), ny = lr_peek16(EB_LEADER_Y);
                bool on_map = nx > 0 && ny > 0 && nx < EBW_MAP_W * 32 && ny < EBW_MAP_H * 32;
                if (on_map) viewer_place((float)nx, (float)ny, lr_peek16(EB_LEADER_DIRECTION));
                else viewer_place(NESS_HOUSE_PX, NESS_HOUSE_PY, 0);
            }
            if (view3d && viewer_update(GetFrameTime())) config_save();

            /* The game keeps running (music and all) but ignores input while you look around. */
            lr_set_buttons(view3d ? 0 : read_buttons());
            lr_run_frame();
            /* The SNES runs at ~60.1 fps and we draw at 60: run an extra frame when audio runs low. */
            if (audio_ok && lr_audio_available() < AUDIO_CHUNK) lr_run_frame();
            frames++;

            if (audio_ok) {
                while (IsAudioStreamProcessed(stream) && lr_audio_available() >= AUDIO_CHUNK) {
                    lr_audio_take(chunk, AUDIO_CHUNK);
                    UpdateAudioStream(stream, chunk, AUDIO_CHUNK);
                }
                if (lr_audio_available() > AUDIO_CHUNK * 6) lr_audio_drop(AUDIO_CHUNK);
            } else {
                lr_audio_drop(lr_audio_available());
            }

            if (frames % SRAM_FLUSH_FRAMES == 0) sram_flush();

            int w, h;
            const uint32_t *px = lr_frame(&w, &h);
            if (px) {
                if (w != tex_w || h != tex_h) {
                    if (tex.id) UnloadTexture(tex);
                    Image img = GenImageColor(w, h, BLACK);
                    tex = LoadTextureFromImage(img);
                    UnloadImage(img);
                    tex_w = w;
                    tex_h = h;
                }
                UpdateTexture(tex, px);
            }
        }

        BeginDrawing();
        if (running && view3d) {
            viewer_draw(cfg.debug);
        } else if (running) {
            ClearBackground(BLACK);
            if (tex.id) draw_game_texture(tex, tex_w, tex_h);
            if (cfg.debug) draw_debug_panel();
            else draw_debug_hint();
        } else {
            draw_setup_screen();
        }
        EndDrawing();

        if (test_frames >= 0 && (frames >= test_frames || !running)) {
            if (screenshot) TakeScreenshot(screenshot);
            log_msg("test: ran %ld frames, X=%u Y=%u dir=%u frame_counter=%u\n", frames,
                    lr_peek16(EB_LEADER_X), lr_peek16(EB_LEADER_Y), lr_peek16(EB_LEADER_DIRECTION),
                    lr_peek8(EB_FRAME_COUNTER));
            break;
        }
    }

    if (running) {
        sram_flush();
        lr_shutdown();
    }
    if (audio_ok) UnloadAudioStream(stream);
    if (tex.id) UnloadTexture(tex);
    viewer_shutdown();
    ebw_free();
    CloseAudioDevice();
    CloseWindow();
    free(sram_shadow);
    log_close();
    return 0;
}
