#include "lr_host.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "libretro.h"
#include "log.h"

#ifdef _WIN32
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
typedef HMODULE lib_handle;
static lib_handle lib_open(const char *p) { return LoadLibraryA(p); }
static void *lib_sym(lib_handle h, const char *s) { return (void *)GetProcAddress(h, s); }
static void lib_close(lib_handle h) { FreeLibrary(h); }
#else
#include <dlfcn.h>
typedef void *lib_handle;
static lib_handle lib_open(const char *p) { return dlopen(p, RTLD_NOW | RTLD_LOCAL); }
static void *lib_sym(lib_handle h, const char *s) { return dlsym(h, s); }
static void lib_close(lib_handle h) { dlclose(h); }
#endif

#define MAX_W 512
#define MAX_H 512
#define AUDIO_RING_FRAMES 16384 /* stereo frames; ~0.5 s at 32 kHz */

static struct {
    lib_handle lib;
    void (*init)(void);
    void (*deinit)(void);
    void (*get_system_info)(struct retro_system_info *);
    void (*get_system_av_info)(struct retro_system_av_info *);
    void (*set_environment)(retro_environment_t);
    void (*set_video_refresh)(retro_video_refresh_t);
    void (*set_audio_sample)(retro_audio_sample_t);
    void (*set_audio_sample_batch)(retro_audio_sample_batch_t);
    void (*set_input_poll)(retro_input_poll_t);
    void (*set_input_state)(retro_input_state_t);
    void (*set_controller_port_device)(unsigned, unsigned);
    void (*run)(void);
    size_t (*serialize_size)(void);
    bool (*serialize)(void *, size_t);
    bool (*unserialize)(const void *, size_t);
    bool (*load_game)(const struct retro_game_info *);
    void (*unload_game)(void);
    void *(*get_memory_data)(unsigned);
    size_t (*get_memory_size)(unsigned);
} core;

static bool core_initialized, game_loaded;
static enum retro_pixel_format pixel_format = RETRO_PIXEL_FORMAT_0RGB1555;
static struct retro_system_av_info av_info;
static char system_dir[1024] = ".";

static uint32_t frame_rgba[MAX_W * MAX_H];
static int frame_w, frame_h;
static bool have_frame;

static int16_t audio_ring[AUDIO_RING_FRAMES * 2];
static size_t audio_read, audio_count; /* in stereo frames */

static uint16_t buttons;

/* ---- callbacks the core calls ---- */

static void core_log(enum retro_log_level level, const char *fmt, ...)
{
    static const char *names[] = {"debug", "info", "warn", "error"};
    if (level == RETRO_LOG_DEBUG) return;
    log_msg("[core %s] ", names[level & 3]);
    va_list ap;
    va_start(ap, fmt);
    log_vmsg(fmt, ap);
    va_end(ap);
}

static bool env_cb(unsigned cmd, void *data)
{
    switch (cmd) {
    case RETRO_ENVIRONMENT_GET_CAN_DUPE:
        *(bool *)data = true;
        return true;
    case RETRO_ENVIRONMENT_SET_PIXEL_FORMAT: {
        enum retro_pixel_format f = *(const enum retro_pixel_format *)data;
        if (f != RETRO_PIXEL_FORMAT_0RGB1555 && f != RETRO_PIXEL_FORMAT_XRGB8888 &&
            f != RETRO_PIXEL_FORMAT_RGB565)
            return false;
        pixel_format = f;
        return true;
    }
    case RETRO_ENVIRONMENT_GET_SYSTEM_DIRECTORY:
    case RETRO_ENVIRONMENT_GET_SAVE_DIRECTORY:
        *(const char **)data = system_dir;
        return true;
    case RETRO_ENVIRONMENT_GET_LOG_INTERFACE:
        ((struct retro_log_callback *)data)->log = core_log;
        return true;
    case RETRO_ENVIRONMENT_GET_VARIABLE_UPDATE:
        *(bool *)data = false;
        return true;
    default:
        return false;
    }
}

static uint32_t to_rgba(uint32_t r, uint32_t g, uint32_t b)
{
    return r | (g << 8) | (b << 16) | 0xFF000000u; /* RGBA8888 in memory order */
}

static void video_cb(const void *data, unsigned w, unsigned h, size_t pitch)
{
    if (!data) return; /* duplicate frame: keep the previous one */
    if (w > MAX_W) w = MAX_W;
    if (h > MAX_H) h = MAX_H;
    for (unsigned y = 0; y < h; y++) {
        const uint8_t *row = (const uint8_t *)data + y * pitch;
        uint32_t *out = frame_rgba + y * w;
        if (pixel_format == RETRO_PIXEL_FORMAT_XRGB8888) {
            const uint32_t *px = (const uint32_t *)row;
            for (unsigned x = 0; x < w; x++)
                out[x] = to_rgba((px[x] >> 16) & 0xFF, (px[x] >> 8) & 0xFF, px[x] & 0xFF);
        } else if (pixel_format == RETRO_PIXEL_FORMAT_RGB565) {
            const uint16_t *px = (const uint16_t *)row;
            for (unsigned x = 0; x < w; x++) {
                uint32_t r = (px[x] >> 11) & 31, g = (px[x] >> 5) & 63, b = px[x] & 31;
                out[x] = to_rgba((r << 3) | (r >> 2), (g << 2) | (g >> 4), (b << 3) | (b >> 2));
            }
        } else {
            const uint16_t *px = (const uint16_t *)row;
            for (unsigned x = 0; x < w; x++) {
                uint32_t r = (px[x] >> 10) & 31, g = (px[x] >> 5) & 31, b = px[x] & 31;
                out[x] = to_rgba((r << 3) | (r >> 2), (g << 3) | (g >> 2), (b << 3) | (b >> 2));
            }
        }
    }
    frame_w = (int)w;
    frame_h = (int)h;
    have_frame = true;
}

static size_t audio_batch_cb(const int16_t *data, size_t frames)
{
    for (size_t i = 0; i < frames; i++) {
        if (audio_count == AUDIO_RING_FRAMES) { /* full: drop the oldest frame */
            audio_read = (audio_read + 1) % AUDIO_RING_FRAMES;
            audio_count--;
        }
        size_t w = (audio_read + audio_count) % AUDIO_RING_FRAMES;
        audio_ring[w * 2] = data[i * 2];
        audio_ring[w * 2 + 1] = data[i * 2 + 1];
        audio_count++;
    }
    return frames;
}

static void audio_sample_cb(int16_t l, int16_t r)
{
    int16_t s[2] = {l, r};
    audio_batch_cb(s, 1);
}

static void input_poll_cb(void) {}

static int16_t input_state_cb(unsigned port, unsigned device, unsigned index, unsigned id)
{
    (void)index;
    if (port != 0 || device != RETRO_DEVICE_JOYPAD) return 0;
    if (id == RETRO_DEVICE_ID_JOYPAD_MASK) return (int16_t)buttons;
    return (buttons >> id) & 1;
}

/* ---- public API ---- */

bool lr_load_core(const char *path, char *err, size_t errlen)
{
    core.lib = lib_open(path);
    if (!core.lib) {
        snprintf(err, errlen, "Could not open the emulator core:\n%s", path);
        return false;
    }
#define LOAD(field, name)                                                                  \
    do {                                                                                   \
        *(void **)&core.field = lib_sym(core.lib, name);                                   \
        if (!core.field) {                                                                 \
            snprintf(err, errlen, "%s is not a libretro core (missing %s).", path, name);  \
            lib_close(core.lib);                                                           \
            core.lib = NULL;                                                               \
            return false;                                                                  \
        }                                                                                  \
    } while (0)
    LOAD(init, "retro_init");
    LOAD(deinit, "retro_deinit");
    LOAD(get_system_info, "retro_get_system_info");
    LOAD(get_system_av_info, "retro_get_system_av_info");
    LOAD(set_environment, "retro_set_environment");
    LOAD(set_video_refresh, "retro_set_video_refresh");
    LOAD(set_audio_sample, "retro_set_audio_sample");
    LOAD(set_audio_sample_batch, "retro_set_audio_sample_batch");
    LOAD(set_input_poll, "retro_set_input_poll");
    LOAD(set_input_state, "retro_set_input_state");
    LOAD(set_controller_port_device, "retro_set_controller_port_device");
    LOAD(run, "retro_run");
    LOAD(serialize_size, "retro_serialize_size");
    LOAD(serialize, "retro_serialize");
    LOAD(unserialize, "retro_unserialize");
    LOAD(load_game, "retro_load_game");
    LOAD(unload_game, "retro_unload_game");
    LOAD(get_memory_data, "retro_get_memory_data");
    LOAD(get_memory_size, "retro_get_memory_size");
#undef LOAD

    struct retro_system_info info = {0};
    core.get_system_info(&info);
    log_msg("Loaded core %s %s\n", info.library_name, info.library_version);

    core.set_environment(env_cb);
    core.init();
    core_initialized = true;
    core.set_video_refresh(video_cb);
    core.set_audio_sample(audio_sample_cb);
    core.set_audio_sample_batch(audio_batch_cb);
    core.set_input_poll(input_poll_cb);
    core.set_input_state(input_state_cb);
    return true;
}

bool lr_load_game(const char *rom_path, const uint8_t *data, size_t size, char *err, size_t errlen)
{
    struct retro_game_info game = {rom_path, data, size, NULL};
    if (!core.load_game(&game)) {
        snprintf(err, errlen, "The emulator core refused to load the ROM.");
        return false;
    }
    game_loaded = true;
    core.get_system_av_info(&av_info);
    core.set_controller_port_device(0, RETRO_DEVICE_JOYPAD);
    log_msg("Game loaded: %ux%u @ %.3f fps, audio %.1f Hz\n", av_info.geometry.base_width,
            av_info.geometry.base_height, av_info.timing.fps, av_info.timing.sample_rate);
    return true;
}

void lr_shutdown(void)
{
    if (game_loaded) core.unload_game();
    if (core_initialized) core.deinit();
    if (core.lib) lib_close(core.lib);
    game_loaded = core_initialized = false;
    core.lib = NULL;
}

void lr_set_buttons(uint16_t mask) { buttons = mask; }
void lr_run_frame(void) { core.run(); }

const uint32_t *lr_frame(int *w, int *h)
{
    if (!have_frame) return NULL;
    *w = frame_w;
    *h = frame_h;
    return frame_rgba;
}

size_t lr_audio_available(void) { return audio_count; }

size_t lr_audio_take(int16_t *dst, size_t frames)
{
    if (frames > audio_count) frames = audio_count;
    for (size_t i = 0; i < frames; i++) {
        dst[i * 2] = audio_ring[audio_read * 2];
        dst[i * 2 + 1] = audio_ring[audio_read * 2 + 1];
        audio_read = (audio_read + 1) % AUDIO_RING_FRAMES;
    }
    audio_count -= frames;
    return frames;
}

void lr_audio_drop(size_t frames)
{
    if (frames > audio_count) frames = audio_count;
    audio_read = (audio_read + frames) % AUDIO_RING_FRAMES;
    audio_count -= frames;
}

double lr_fps(void) { return av_info.timing.fps > 0 ? av_info.timing.fps : 60.0988; }
double lr_sample_rate(void) { return av_info.timing.sample_rate > 0 ? av_info.timing.sample_rate : 32040.0; }

uint8_t *lr_wram(size_t *size)
{
    *size = game_loaded ? core.get_memory_size(RETRO_MEMORY_SYSTEM_RAM) : 0;
    return *size ? (uint8_t *)core.get_memory_data(RETRO_MEMORY_SYSTEM_RAM) : NULL;
}

uint8_t *lr_sram(size_t *size)
{
    *size = game_loaded ? core.get_memory_size(RETRO_MEMORY_SAVE_RAM) : 0;
    return *size ? (uint8_t *)core.get_memory_data(RETRO_MEMORY_SAVE_RAM) : NULL;
}

bool lr_save_state(const char *path)
{
    size_t n = core.serialize_size();
    void *buf = malloc(n);
    bool ok = buf && core.serialize(buf, n);
    if (ok) {
        FILE *f = fopen(path, "wb");
        ok = f && fwrite(buf, 1, n, f) == n;
        if (f) fclose(f);
    }
    free(buf);
    return ok;
}

bool lr_load_state(const char *path)
{
    FILE *f = fopen(path, "rb");
    if (!f) return false;
    fseek(f, 0, SEEK_END);
    long n = ftell(f);
    fseek(f, 0, SEEK_SET);
    void *buf = n > 0 ? malloc((size_t)n) : NULL;
    bool ok = buf && fread(buf, 1, (size_t)n, f) == (size_t)n && core.unserialize(buf, (size_t)n);
    fclose(f);
    free(buf);
    return ok;
}

uint8_t lr_peek8(uint32_t snes_addr)
{
    size_t size;
    uint8_t *wram = lr_wram(&size);
    uint32_t off = snes_addr - 0x7E0000;
    return (wram && snes_addr >= 0x7E0000 && off < size) ? wram[off] : 0;
}

uint16_t lr_peek16(uint32_t snes_addr)
{
    return (uint16_t)(lr_peek8(snes_addr) | (lr_peek8(snes_addr + 1) << 8));
}
