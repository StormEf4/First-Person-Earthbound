/* Tree sprites found by exact pixel matching. EarthBound stamps the same few tree drawings all
 * over its maps; cutting each drawing out once (cleanly) and finding every copy beats guessing
 * trees from collision and colours. */
#pragma once
#include <stdbool.h>
#include <stdint.h>

#define TM_MAX_W 96
#define TM_MAX_H 96

typedef struct {
    int w, h;
    uint32_t px[TM_MAX_W * TM_MAX_H]; /* RGBA, alpha 0 = not part of the tree */
    int phase_x, phase_y;             /* copies sit at the same position modulo 8 as the example */
    int tileset;                      /* map tileset the drawing belongs to */
    int opaque;                       /* number of tree pixels */
    uint32_t grass;                   /* ground colour around the example, for erasing copies */
} TreeSprite;

typedef struct {
    int sprite;
    int x, y; /* map pixel of the sprite's top-left */
} TreeMatch;

typedef uint32_t (*TmPixelFn)(int px, int py, bool event);

/* Cuts the known tree drawings out of the map. Returns how many were cut. */
int tm_init(TmPixelFn pixel, bool event, int (*tileset_at)(int tx, int ty));
const TreeSprite *tm_sprite(int i);
int tm_count(void);

/* Finds trees whose sprite bottom row lies in [y0, y1) and left edge in [x0, x1) (map pixels).
 * Returns the number written to out. */
int tm_find(int x0, int y0, int x1, int y1, bool event, TreeMatch *out, int max);
