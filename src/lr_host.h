/* Minimal libretro frontend: loads an emulator core (Snes9x) and runs one game. */
#pragma once
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

/* Bit positions match RETRO_DEVICE_ID_JOYPAD_*. */
enum {
    PAD_B, PAD_Y, PAD_SELECT, PAD_START, PAD_UP, PAD_DOWN, PAD_LEFT, PAD_RIGHT,
    PAD_A, PAD_X, PAD_L, PAD_R
};

bool lr_load_core(const char *path, char *err, size_t errlen);
bool lr_load_game(const char *rom_path, const uint8_t *data, size_t size, char *err, size_t errlen);
void lr_shutdown(void);

void lr_set_buttons(uint16_t mask);
void lr_run_frame(void);

/* Last video frame as RGBA8888, or NULL before the first frame. */
const uint32_t *lr_frame(int *w, int *h);

/* Interleaved stereo int16 samples produced by the core, waiting to be played. */
size_t lr_audio_available(void);
size_t lr_audio_take(int16_t *dst, size_t frames);
void lr_audio_drop(size_t frames);

double lr_fps(void);
double lr_sample_rate(void);

uint8_t *lr_wram(size_t *size);
uint8_t *lr_sram(size_t *size);

bool lr_save_state(const char *path);
bool lr_load_state(const char *path);

/* Reads a little-endian value from SNES work RAM ($7E0000-$7FFFFF). Returns 0 if out of range. */
uint8_t lr_peek8(uint32_t snes_addr);
uint16_t lr_peek16(uint32_t snes_addr);
