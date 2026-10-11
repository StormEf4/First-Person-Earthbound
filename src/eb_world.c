#include "eb_world.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* Offsets into the headerless US ROM. Table locations as documented by the EarthBound hacking
 * community (CoilSnake / ebsrc). */
#define MAP_POINTERS_PTR 0x00A1DB      /* 3-byte SNES pointer to 8 row-group pointers */
#define LOCAL_TILESETS 0x175000        /* high 2 bits of each map tile */
#define SECTOR_TILESET_PALETTE 0x17A800 /* 1 byte per sector: tileset << 3 | palette */
#define SECTOR_MISC 0x17B200           /* 2 bytes per sector: teleport<<7 | townmap<<3 | setting, item */
#define MAP_TILESET_TO_GFX 0x2F101B    /* 32 x u16: map tileset -> drawing tileset */
#define GFX_POINTERS 0x2F105B          /* 20 x u32 SNES pointers, compressed 4bpp minitiles */
#define ARRANGEMENT_POINTERS 0x2F10AB  /* 20 x u32, compressed 4x4 minitile arrangements */
#define PALETTE_POINTERS 0x2F10FB      /* 32 x u32, per map tileset, 0xC0 bytes per palette */
#define COLLISION_POINTERS 0x2F117B    /* 20 x u32, tables of u16 offsets into bank $D8 */
#define COLLISION_BANK 0x180000
#define PALETTE_BANK 0x1A0000

#define NUM_GFX_TILESETS 20
#define NUM_MAP_TILESETS 32
#define MINITILES 896
#define ARRANGEMENTS 1024
#define PALETTE_BYTES 0xC0

typedef struct {
    bool loaded;
    uint8_t pixels[MINITILES][64];          /* 4-bit colour indices */
    uint16_t arrangement[ARRANGEMENTS][16]; /* SNES tilemap words */
    uint8_t collision[ARRANGEMENTS][16];
} GfxTileset;

static const uint8_t *rom;
static size_t rom_size;
static uint16_t map_tiles[EBW_MAP_H][EBW_MAP_W];
static uint8_t sector_info[EBW_SECTORS_Y][EBW_SECTORS_X];
static uint8_t sector_setting[EBW_SECTORS_Y][EBW_SECTORS_X];
static uint8_t map_to_gfx[NUM_MAP_TILESETS];
static GfxTileset *gfx[NUM_GFX_TILESETS];

static uint32_t rd16(size_t off) { return off + 1 < rom_size ? rom[off] | rom[off + 1] << 8 : 0; }
static uint32_t rd24(size_t off) { return rd16(off) | (off + 2 < rom_size ? rom[off + 2] << 16 : 0); }
static uint32_t rd32(size_t off) { return rd16(off) | rd16(off + 2) << 16; }

static size_t snes_to_file(uint32_t addr)
{
    if (addr >= 0xC00000) return addr - 0xC00000;
    if (addr >= 0x400000 && addr < 0x7E0000) return addr - 0x400000;
    return addr & 0x3FFFFF;
}

/* HAL Laboratory's compression format, used for most EarthBound graphics. */
static uint8_t reverse_bits(uint8_t b)
{
    b = (uint8_t)((b & 0xF0) >> 4 | (b & 0x0F) << 4);
    b = (uint8_t)((b & 0xCC) >> 2 | (b & 0x33) << 2);
    return (uint8_t)((b & 0xAA) >> 1 | (b & 0x55) << 1);
}

static size_t hal_decompress(size_t src, uint8_t *out, size_t cap)
{
    size_t o = 0;
    for (;;) {
        if (src >= rom_size) return 0;
        uint8_t cmd_byte = rom[src++];
        if (cmd_byte == 0xFF) break;
        unsigned cmd, len;
        if ((cmd_byte & 0xE0) == 0xE0) {
            cmd = (cmd_byte >> 2) & 7;
            len = (((cmd_byte & 3) << 8) | rom[src++]) + 1;
        } else {
            cmd = cmd_byte >> 5;
            len = (cmd_byte & 0x1F) + 1;
        }
        if (o + len * (cmd == 2 ? 2 : 1) > cap) return 0;
        switch (cmd) {
        case 0: /* literal bytes */
            if (src + len > rom_size) return 0;
            memcpy(out + o, rom + src, len);
            o += len;
            src += len;
            break;
        case 1: /* byte fill */
            memset(out + o, rom[src++], len);
            o += len;
            break;
        case 2: /* word fill */
            for (unsigned i = 0; i < len; i++) {
                out[o++] = rom[src];
                out[o++] = rom[src + 1];
            }
            src += 2;
            break;
        case 3: /* increasing byte */
            for (unsigned i = 0; i < len; i++) out[o++] = (uint8_t)(rom[src] + i);
            src++;
            break;
        default: { /* back-references; offset is big-endian */
            size_t off = (size_t)rom[src] << 8 | rom[src + 1];
            src += 2;
            for (unsigned i = 0; i < len; i++) {
                size_t from = cmd == 6 ? off - i : off + i;
                if (from >= o) return 0;
                out[o++] = cmd == 5 ? reverse_bits(out[from]) : out[from];
            }
            break;
        }
        }
    }
    return o;
}

static GfxTileset *load_gfx(int id)
{
    if (id < 0 || id >= NUM_GFX_TILESETS) return NULL;
    if (gfx[id]) return gfx[id];
    GfxTileset *t = calloc(1, sizeof *t);
    uint8_t *buf = malloc(65536);
    if (!t || !buf) {
        free(t);
        free(buf);
        return NULL;
    }

    size_t n = hal_decompress(snes_to_file(rd32(GFX_POINTERS + id * 4)), buf, 65536);
    for (size_t tile = 0; tile < MINITILES && (tile + 1) * 32 <= n; tile++) {
        const uint8_t *p = buf + tile * 32; /* SNES 4bpp planar */
        for (int y = 0; y < 8; y++)
            for (int x = 0; x < 8; x++) {
                int bit = 7 - x;
                t->pixels[tile][y * 8 + x] =
                    (uint8_t)(((p[y * 2] >> bit) & 1) | ((p[y * 2 + 1] >> bit) & 1) << 1 |
                              ((p[16 + y * 2] >> bit) & 1) << 2 | ((p[16 + y * 2 + 1] >> bit) & 1) << 3);
            }
    }

    n = hal_decompress(snes_to_file(rd32(ARRANGEMENT_POINTERS + id * 4)), buf, 65536);
    size_t count = n / 32;
    if (count > ARRANGEMENTS) count = ARRANGEMENTS;
    size_t coll_table = snes_to_file(rd32(COLLISION_POINTERS + id * 4));
    for (size_t a = 0; a < count; a++) {
        for (int i = 0; i < 16; i++) t->arrangement[a][i] = (uint16_t)(buf[a * 32 + i * 2] | buf[a * 32 + i * 2 + 1] << 8);
        size_t c = COLLISION_BANK | rd16(coll_table + a * 2);
        if (c + 16 <= rom_size) memcpy(t->collision[a], rom + c, 16);
    }
    free(buf);
    t->loaded = true;
    gfx[id] = t;
    return t;
}

bool ebw_load(const uint8_t *data, size_t size, char *err, size_t errlen)
{
    rom = data;
    rom_size = size;
    if (size < 0x300000) {
        snprintf(err, errlen, "ROM too small for EarthBound map data.");
        return false;
    }

    size_t row_ptrs = snes_to_file(rd24(MAP_POINTERS_PTR));
    size_t row_group[8];
    for (int i = 0; i < 8; i++) row_group[i] = snes_to_file(rd32(row_ptrs + i * 4));
    for (int y = 0; y < EBW_MAP_H; y++) {
        size_t off = row_group[y % 8] + (size_t)(y >> 3) * 256;
        for (int x = 0; x < EBW_MAP_W; x++) map_tiles[y][x] = off + x < size ? rom[off + x] : 0;
    }
    /* Two extra bits per tile, packed four rows per byte in two 0x3000-byte halves. */
    size_t k = LOCAL_TILESETS;
    for (int i = 0; i < EBW_MAP_H / 8; i++)
        for (int x = 0; x < EBW_MAP_W; x++, k++)
            for (int r = 0; r < 8; r++) {
                uint8_t b = rom[k + (r >= 4 ? 0x3000 : 0)];
                map_tiles[i * 8 + r][x] |= (uint16_t)(((b >> ((r & 3) * 2)) & 3) << 8);
            }

    memcpy(sector_info, rom + SECTOR_TILESET_PALETTE, sizeof sector_info);
    for (int i = 0; i < EBW_SECTORS_X * EBW_SECTORS_Y; i++)
        sector_setting[i / EBW_SECTORS_X][i % EBW_SECTORS_X] = rom[SECTOR_MISC + i * 2] & 7;
    for (int i = 0; i < NUM_MAP_TILESETS; i++) map_to_gfx[i] = (uint8_t)rd16(MAP_TILESET_TO_GFX + i * 2);
    return true;
}

void ebw_free(void)
{
    for (int i = 0; i < NUM_GFX_TILESETS; i++) {
        free(gfx[i]);
        gfx[i] = NULL;
    }
}

static bool in_map(int tx, int ty) { return tx >= 0 && ty >= 0 && tx < EBW_MAP_W && ty < EBW_MAP_H; }

int ebw_tileset_at(int tx, int ty)
{
    return in_map(tx, ty) ? sector_info[ty / EBW_SECTOR_H][tx / EBW_SECTOR_W] >> 3 : 0;
}

int ebw_palette_at(int tx, int ty)
{
    return in_map(tx, ty) ? sector_info[ty / EBW_SECTOR_H][tx / EBW_SECTOR_W] & 7 : 0;
}

bool ebw_indoors_at(int tx, int ty)
{
    if (!in_map(tx, ty)) return false;
    int setting = sector_setting[ty / EBW_SECTOR_H][tx / EBW_SECTOR_W];
    return setting == 1 || setting == 7; /* "Indoors", "Indoors and butterflies" */
}

uint16_t ebw_tile_at(int tx, int ty) { return in_map(tx, ty) ? map_tiles[ty][tx] : 0; }

uint8_t ebw_collision_at(int mx, int my)
{
    int tx = mx >> 2, ty = my >> 2;
    if (!in_map(tx, ty)) return EBW_SOLID;
    GfxTileset *t = load_gfx(map_to_gfx[ebw_tileset_at(tx, ty)]);
    return t ? t->collision[map_tiles[ty][tx]][(my & 3) * 4 + (mx & 3)] : 0;
}

bool ebw_foreground_at(int mx, int my)
{
    int tx = mx >> 2, ty = my >> 2;
    if (!in_map(tx, ty)) return false;
    GfxTileset *t = load_gfx(map_to_gfx[ebw_tileset_at(tx, ty)]);
    if (!t) return false;
    /* EarthBound marks walk-behind cells in collision: they hide part of Ness's sprite. */
    return (t->collision[map_tiles[ty][tx]][(my & 3) * 4 + (mx & 3)] & (EBW_OBSCURE_LOWER | EBW_OBSCURE_UPPER)) != 0;
}

static size_t palette_offset(int map_tileset, int palette, bool event_palette)
{
    size_t off = snes_to_file(rd32(PALETTE_POINTERS + (map_tileset & 31) * 4)) + (size_t)palette * PALETTE_BYTES;
    if (event_palette && rd16(off) != 0) off = PALETTE_BANK | rd16(off + 0x20);
    return off;
}

uint16_t ebw_palette_event_flag(int map_tileset, int palette)
{
    return (uint16_t)rd16(palette_offset(map_tileset, palette, false));
}

void ebw_render_tile(int map_tileset, int palette, uint16_t tile, bool event_palette, uint32_t *out,
                     int out_stride)
{
    GfxTileset *t = load_gfx(map_to_gfx[map_tileset & 31]);
    size_t pal = palette_offset(map_tileset, palette, event_palette);
    uint32_t colors[6][16];
    for (int s = 0; s < 6; s++)
        for (int c = 0; c < 16; c++) {
            uint32_t bgr = c == 0 ? 0 : rd16(pal + (size_t)(s * 16 + c) * 2); /* colour 0 holds metadata */
            uint32_t r = (bgr & 31) << 3, g = ((bgr >> 5) & 31) << 3, b = ((bgr >> 10) & 31) << 3;
            colors[s][c] = (r | r >> 5) | (g | g >> 5) << 8 | (b | b >> 5) << 16 | 0xFF000000u;
        }

    for (int i = 0; i < 16; i++) {
        uint16_t w = t ? t->arrangement[tile & 1023][i] : 0;
        unsigned mini = w & 0x3FF;
        int sub = ((w >> 10) & 7) - 2; /* map palettes start at CGRAM palette 2 */
        if (sub < 0 || sub > 5) sub = 0;
        bool hflip = w & 0x4000, vflip = w & 0x8000;
        int ox = (i & 3) * 8, oy = (i >> 2) * 8;
        for (int y = 0; y < 8; y++)
            for (int x = 0; x < 8; x++) {
                int sx = hflip ? 7 - x : x, sy = vflip ? 7 - y : y;
                uint8_t idx = (t && mini < MINITILES) ? t->pixels[mini][sy * 8 + sx] : 0;
                out[(oy + y) * out_stride + ox + x] = colors[sub][idx];
            }
    }
}
