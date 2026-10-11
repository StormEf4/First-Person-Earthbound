/* Reads EarthBound's overworld straight from the user's ROM: the map, tilesets, palettes, and
 * collision. Nothing here is shipped with the program. */
#pragma once
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#define EBW_MAP_W 256 /* in 32x32-pixel map tiles */
#define EBW_MAP_H 320
#define EBW_SECTOR_W 8 /* map tiles per sector */
#define EBW_SECTOR_H 4
#define EBW_SECTORS_X (EBW_MAP_W / EBW_SECTOR_W)
#define EBW_SECTORS_Y (EBW_MAP_H / EBW_SECTOR_H)
#define EBW_MINI_W (EBW_MAP_W * 4) /* in 8x8-pixel minitiles */
#define EBW_MINI_H (EBW_MAP_H * 4)

/* Collision flags per minitile (ebsrc SURFACE_FLAGS). */
enum {
    EBW_OBSCURE_LOWER = 1 << 0,
    EBW_OBSCURE_UPPER = 1 << 1,
    EBW_SUNSTROKE = 1 << 2,
    EBW_SHALLOW_WATER = 1 << 3,
    EBW_LADDER = 1 << 4,
    EBW_SOLID = 1 << 7,
};

bool ebw_load(const uint8_t *rom, size_t size, char *err, size_t errlen);
void ebw_free(void);

/* Sector properties for a map-tile coordinate. */
int ebw_tileset_at(int tx, int ty); /* map tileset 0-31 */
int ebw_palette_at(int tx, int ty);
bool ebw_indoors_at(int tx, int ty); /* sector "setting" says indoors */

uint16_t ebw_tile_at(int tx, int ty); /* arrangement index 0-1023 */
uint8_t ebw_collision_at(int mx, int my); /* minitile coordinates */
/* True if the minitile is drawn in front of Ness (its collision hides part of his sprite): the
 * parts of buildings and trees he walks behind. */
bool ebw_foreground_at(int mx, int my);

/* Renders one 32x32 map tile as RGBA8888. `event_palette` picks the palette variant some areas
 * switch to after a story event, when one exists. */
void ebw_render_tile(int map_tileset, int palette, uint16_t tile, bool event_palette, uint32_t *out,
                     int out_stride);

/* Event flag that switches a sector's palette to its variant, or 0 if it has none. */
uint16_t ebw_palette_event_flag(int map_tileset, int palette);
