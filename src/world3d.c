#include "world3d.h"

#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "eb_world.h"
#include "tree_match.h"
#include "raymath.h"
#include "rlgl.h"

/* Ground colours treated as transparent when cutting art out of the map. */
#define MAX_KEYS 8
typedef struct {
    uint32_t c[MAX_KEYS];
    int n;
} Keys;
static bool is_key(const Keys *k, uint32_t c);
static bool ground_keys_pct(int x0, int x1, int y0, int y1, bool event, Keys *keys, int pct);
#define ground_keys(x0, x1, y0, y1, event, keys) ground_keys_pct(x0, x1, y0, y1, event, keys, 0)

static int art_rows_above(int id, int mx, int base, const Keys *keys, bool event);
static int column_bottom(int id, int mx, int y0, int y1);
static void detect_shape(int id, int W, int R);

static void classify_cliffs(void);
static bool rock_cell(int mx, int my, bool event);
static int owner_at(int mx, int my);
static void solve_ground(void);

static int median(int *v, int n)
{
    for (int a = 1; a < n; a++)
        for (int b = a; b > 0 && v[b] < v[b - 1]; b--) {
            int t = v[b];
            v[b] = v[b - 1];
            v[b - 1] = t;
        }
    return v[n / 2];
}

/* ---- options ---- */

static const struct {
    const char *name, *key;
    int count;
    const char *values[6];
} option_info[OPT_COUNT] = {
    [OPT_RESOLUTION] = {"Resolution", "res", 5, {"256x224 (SNES)", "320x200 (Doom)", "320x240 (PS1)", "400x225 (wide)", "640x360 (sharper)"}},
    [OPT_WALL_STYLE] = {"Wall style", "walls", 2, {"Facades (art stands up)", "Blocks (diorama)"}},
    [OPT_FOG] = {"Fog / draw distance", "fog", 4, {"Thick", "Medium", "Light", "None"}},
    [OPT_MOOD] = {"Lighting mood", "mood", 4, {"Original colors", "Night", "Dusk", "Eerie"}},
    [OPT_PALETTE] = {"Map palette", "palette", 2, {"Normal (day)", "Story-event variant (Onett: night)"}},
};

const char *w3d_option_name(OptionId id) { return option_info[id].name; }
const char *w3d_option_key(OptionId id) { return option_info[id].key; }
int w3d_option_count(OptionId id) { return option_info[id].count; }

const char *w3d_option_value_name(OptionId id, int value)
{
    return value >= 0 && value < option_info[id].count ? option_info[id].values[value] : "?";
}

void w3d_resolution(const W3dOptions *opt, int *w, int *h, float *display_aspect)
{
    static const int sizes[][2] = {{256, 224}, {320, 200}, {320, 240}, {400, 225}, {640, 360}};
    int i = opt->value[OPT_RESOLUTION];
    *w = sizes[i][0];
    *h = sizes[i][1];
    /* SNES and Doom resolutions were shown stretched to 4:3 on CRTs. */
    *display_aspect = (i <= 2) ? 4.0f / 3.0f : (float)*w / (float)*h;
}

/* Context-driven wall height (D23): low indoors, tall outdoors. */
#define OUTDOOR_SCALE 1.25f /* world units per stood-up minitile row */
#define INDOOR_SCALE 1.0f
#define OUTDOOR_MAX_LEVELS 16
#define INDOOR_MAX_LEVELS 4
#define TERRAIN_LEVELS 2 /* cliffs and ledges: a step up in the ground, not a skyscraper */
#define FOREST_LEVELS 6  /* tree walls: well above eye level */

/* ---- source pixels: rendered map tiles, cached ---- */

#define TILE_CACHE 2048
static struct {
    int key;
    uint32_t px[32 * 32];
} *tile_cache;

static const uint32_t *tile_pixels(int tx, int ty, bool event)
{
    static const uint32_t empty[32 * 32];
    if (tx < 0 || ty < 0 || tx >= EBW_MAP_W || ty >= EBW_MAP_H) return empty;
    int key = (ty * EBW_MAP_W + tx) << 1 | (event ? 1 : 0);
    unsigned h = ((unsigned)key * 2654435761u) % TILE_CACHE;
    if (tile_cache[h].key != key) {
        ebw_render_tile(ebw_tileset_at(tx, ty), ebw_palette_at(tx, ty), ebw_tile_at(tx, ty), event,
                        tile_cache[h].px, 32);
        tile_cache[h].key = key;
    }
    return tile_cache[h].px;
}

static uint32_t pixel_at(int px, int py, bool event)
{
    if (px < 0 || py < 0) return 0;
    return tile_pixels(px >> 5, py >> 5, event)[(py & 31) * 32 + (px & 31)];
}

/* ---- texture atlas of 32x32 map tiles, filled on demand ---- */

#define ATLAS_SIZE 4096
#define ATLAS_SLOTS_PER_ROW (ATLAS_SIZE / 32)
#define ATLAS_SLOTS (ATLAS_SLOTS_PER_ROW * ATLAS_SLOTS_PER_ROW)
#define KEY_COUNT (1 << 19) /* tileset(5) | palette(3) | event(1) | tile(10) */
#define MAP_TILES (EBW_MAP_W * EBW_MAP_H)

static Texture2D atlas;
static int32_t *slot_of_key;
static int32_t *slot_of_cleaned; /* [event][map tile]: floor tiles with props erased */
static int slots_used;
static bool atlas_full; /* flying far fills it; everything is rebuilt on the next frame */

static int new_slot(const uint32_t *px)
{
    if (slots_used >= ATLAS_SLOTS) {
        atlas_full = true;
        return 0;
    }
    int slot = slots_used++;
    Rectangle r = {(float)(slot % ATLAS_SLOTS_PER_ROW * 32), (float)(slot / ATLAS_SLOTS_PER_ROW * 32), 32, 32};
    UpdateTextureRec(atlas, r, px);
    return slot;
}

static int atlas_slot(int tx, int ty, bool event)
{
    int key = (ebw_tileset_at(tx, ty) & 31) << 14 | (ebw_palette_at(tx, ty) & 7) << 11 | (event ? 1 : 0) << 10 |
              (ebw_tile_at(tx, ty) & 1023);
    if (slot_of_key[key] < 0) slot_of_key[key] = new_slot(tile_pixels(tx, ty, event));
    return slot_of_key[key];
}

typedef struct {
    float u0, v0, u1, v1;
} UvRect;

static UvRect slot_uv(int slot, int x, int y, int w, int h)
{
    const float inset = 0.02f / ATLAS_SIZE;
    float ax = (float)(slot % ATLAS_SLOTS_PER_ROW * 32 + x), ay = (float)(slot / ATLAS_SLOTS_PER_ROW * 32 + y);
    return (UvRect){ax / ATLAS_SIZE + inset, ay / ATLAS_SIZE + inset, (ax + w) / ATLAS_SIZE - inset,
                    (ay + h) / ATLAS_SIZE - inset};
}

static UvRect minitile_uv(int mx, int my, bool event)
{
    return slot_uv(atlas_slot(mx >> 2, my >> 2, event), (mx & 3) * 8, (my & 3) * 8, 8, 8);
}

/* ---- hand-placed building blueprints (blueprints.c) ---- */

#include "blueprints.h"

/* ---- objects: connected solid areas of the collision map ---- */

typedef struct {
    int x0, y0, x1, y1, cells;
    int depth_sum, runs;
    bool is_prop, prop_made, indoors;
    bool foliage; /* trees and hedges: drawn as cut-out strips, never as boxes */
    bool terrain; /* cliffs and ledges: long, sprawling, and kept low */
    bool forest;  /* big masses of trees: a solid wall of tree art */
    bool thin;    /* fences and railings: cut out with every ground colour removed */
    bool hidden;  /* nothing to show (an invisible blocker with only ground art) */
    bool cliff;   /* a rock face between two ground levels: nothing walks behind it */
    bool green;   /* leafy art (hedges, groves) */
    bool fence_made;
    float fence_h;
    Rectangle fence_src; /* one picket column, repeated along the fence */
    bool voidc;   /* off-map backdrop (the starry sky past the hilltop): not drawn at all */
    bool blueprint; /* a hand-placed building (blueprints.c): never reclassified */
    bool erased;    /* its art above the footprint has been wiped from the floor */
    /* Shaped buildings: the bottom wall_rows of the art are the front wall, the top roof_rows are
     * the roof seen from above, which is also the building's depth. */
    bool shaped, gable;
    int wall_rows, roof_rows;
    int emitted_build;
    int fx, fy; /* first cell in raster order: the chunk holding it emits whole-object models */
    int levels;
} Object;

static bool looks_like_prop(const Object *o);
static int building_depth(const Object *o);

static uint8_t *solid_grid;
static float *ground;      /* ground height per minitile, in world units (cliff cells: their top) */
static int32_t *fg_owner;  /* walk-behind cell -> the object standing directly beneath it, or -1 */
static uint8_t *cell_kind; /* CELL_* below */
enum { CELL_GROUND, CELL_CLIFF, CELL_VOID };
static uint8_t *tree_cell; /* in cliff/forest masses: this cell's art is tree canopy */
static int32_t *object_of; /* per minitile, -1 when open */
static Object *objects;
static int object_count;
static bool analysed;

static bool solid(int mx, int my)
{
    return mx >= 0 && my >= 0 && mx < EBW_MINI_W && my < EBW_MINI_H && solid_grid[my * EBW_MINI_W + mx];
}

static int object_at(int mx, int my)
{
    return (mx >= 0 && my >= 0 && mx < EBW_MINI_W && my < EBW_MINI_H) ? object_of[my * EBW_MINI_W + mx] : -1;
}

/* Labels every solid area once, so objects that cross chunk borders get one consistent shape. */
static void analyse_world(void)
{
    const int n = EBW_MINI_W * EBW_MINI_H;
    solid_grid = malloc(n);
    object_of = malloc(sizeof(int32_t) * n);
    int32_t *stack = malloc(sizeof(int32_t) * n);
    int cap = 4096;
    objects = malloc(sizeof(Object) * cap);
    object_count = 0;

    for (int my = 0; my < EBW_MINI_H; my++)
        for (int mx = 0; mx < EBW_MINI_W; mx++)
            solid_grid[my * EBW_MINI_W + mx] = (ebw_collision_at(mx, my) & EBW_SOLID) != 0;
    memset(object_of, 0xFF, sizeof(int32_t) * n);

    /* Hand-placed buildings claim their solid cells first, so they never merge with hedges,
     * neighbours, or cliffs. */
    for (int b = 0; b < blueprint_count; b++) {
        const Blueprint *bp = &blueprints[b];
        for (int y = bp->y0; y <= bp->y1; y++)
            for (int x = bp->x0; x <= bp->x1; x++) {
                int start = y * EBW_MINI_W + x;
                if (!solid_grid[start] || object_of[start] >= 0) continue;
                if (object_count == cap) {
                    cap *= 2;
                    objects = realloc(objects, sizeof(Object) * cap);
                }
                Object *o = &objects[object_count];
                memset(o, 0, sizeof *o);
                o->x0 = o->y0 = 1 << 30;
                o->blueprint = true;
                o->levels = bp->levels;
                o->fx = x;
                o->fy = y;
                int sp = 0;
                stack[sp++] = start;
                object_of[start] = object_count;
                while (sp) {
                    int c = stack[--sp], cx = c % EBW_MINI_W, cy = c / EBW_MINI_W;
                    o->cells++;
                    if (cx < o->x0) o->x0 = cx;
                    if (cx > o->x1) o->x1 = cx;
                    if (cy < o->y0) o->y0 = cy;
                    if (cy > o->y1) o->y1 = cy;
                    const int nb[4][2] = {{1, 0}, {-1, 0}, {0, 1}, {0, -1}};
                    for (int k = 0; k < 4; k++) {
                        int nx = cx + nb[k][0], ny = cy + nb[k][1];
                        if (nx < bp->x0 || ny < bp->y0 || nx > bp->x1 || ny > bp->y1) continue;
                        int ni = ny * EBW_MINI_W + nx;
                        if (solid_grid[ni] && object_of[ni] < 0) {
                            object_of[ni] = object_count;
                            stack[sp++] = ni;
                        }
                    }
                }
                object_count++;
            }
    }

    /* "Bulky" cells sit inside some fully solid 3x3 block; fences, hedges, and trunks are thinner.
     * Bulky and thin cells are grouped separately, so a fence touching a building or a hedge
     * along a wall doesn't fuse with it into one shapeless blob. */
    uint8_t *bulky = calloc(n, 1);
    for (int y = 1; y + 1 < EBW_MINI_H; y++)
        for (int x = 1; x + 1 < EBW_MINI_W; x++) {
            bool full = true;
            for (int dy = -1; dy <= 1 && full; dy++)
                for (int dx = -1; dx <= 1 && full; dx++) full = solid_grid[(y + dy) * EBW_MINI_W + x + dx];
            if (full)
                for (int dy = -1; dy <= 1; dy++)
                    for (int dx = -1; dx <= 1; dx++) bulky[(y + dy) * EBW_MINI_W + x + dx] = 1;
        }

    /* under_art: the column above this cell (through solid cells) ends in walk-behind art within
     * two rows: a trunk under its canopy, walls under a roof. Fences have nothing above them. */
    uint8_t *under_art = calloc(n, 1);
    for (int x = 0; x < EBW_MINI_W; x++)
        for (int y = 0; y < EBW_MINI_H; y++) {
            if (!solid_grid[y * EBW_MINI_W + x]) continue;
            int t = y;
            while (t > 0 && solid_grid[(t - 1) * EBW_MINI_W + x]) t--;
            bool art = false;
            for (int k = 1; k <= 3 && t - k >= 0 && !art; k++) art = ebw_foreground_at(x, t - k);
            under_art[y * EBW_MINI_W + x] = art;
        }

    for (int start = 0; start < n; start++) {
        if (!solid_grid[start] || object_of[start] >= 0) continue;
        if (object_count == cap) {
            cap *= 2;
            objects = realloc(objects, sizeof(Object) * cap);
        }
        Object *o = &objects[object_count];
        memset(o, 0, sizeof *o);
        o->x0 = o->y0 = 1 << 30;
        o->fx = start % EBW_MINI_W;
        o->fy = start / EBW_MINI_W;
        o->thin = !bulky[start]; /* fences, hedges, trunks: never part of a solid 3x3 block */
        int sp = 0;
        stack[sp++] = start;
        object_of[start] = object_count;
        while (sp) {
            int c = stack[--sp], x = c % EBW_MINI_W, y = c / EBW_MINI_W;
            o->cells++;
            if (x < o->x0) o->x0 = x;
            if (x > o->x1) o->x1 = x;
            if (y < o->y0) o->y0 = y;
            if (y > o->y1) o->y1 = y;
            /* Thin cells also join diagonally: a diagonal fence touches only corner to corner. */
            const int nb[8][2] = {{1, 0}, {-1, 0}, {0, 1}, {0, -1}, {1, 1}, {-1, 1}, {1, -1}, {-1, -1}};
            for (int k = 0; k < (bulky[c] || under_art[c] ? 4 : 8); k++) {
                int nx = x + nb[k][0], ny = y + nb[k][1];
                if (nx < 0 || ny < 0 || nx >= EBW_MINI_W || ny >= EBW_MINI_H) continue;
                int ni = ny * EBW_MINI_W + nx;
                if (k >= 4 && under_art[ni]) continue;
                if (solid_grid[ni] && object_of[ni] < 0 && bulky[ni] == bulky[c]) {
                    object_of[ni] = object_count;
                    stack[sp++] = ni;
                }
            }
        }
        object_count++;
    }
    free(bulky);
    free(under_art);

    /* Average depth of each object's vertical runs: how far its art extends "up" the screen. */
    for (int mx = 0; mx < EBW_MINI_W; mx++) {
        int run_obj = -1, run_len = 0;
        for (int my = 0; my <= EBW_MINI_H; my++) {
            int o = my < EBW_MINI_H ? object_of[my * EBW_MINI_W + mx] : -1;
            if (o == run_obj && o >= 0) {
                run_len++;
                continue;
            }
            if (run_obj >= 0) {
                objects[run_obj].depth_sum += run_len;
                objects[run_obj].runs++;
            }
            run_obj = o;
            run_len = 1;
        }
    }

    for (int i = 0; i < object_count; i++) {
        Object *o = &objects[i];
        o->indoors = ebw_indoors_at(o->x0 >> 2, o->y1 >> 2);
        o->is_prop = looks_like_prop(o);
        if (o->blueprint) continue;
        float avg = o->runs ? (float)o->depth_sum / o->runs : 1;
        if (o->indoors) {
            o->levels = (int)(avg + 0.5f);
            if (o->levels > INDOOR_MAX_LEVELS) o->levels = INDOOR_MAX_LEVELS;
            if (o->levels < 1) o->levels = 1;
        } else {
            o->levels = ((int)(avg + 1.0f) / 2) * 2; /* whole "storeys" of two rows: no stair-steps */
            if (o->levels > OUTDOOR_MAX_LEVELS) o->levels = OUTDOOR_MAX_LEVELS;
            if (o->levels < 2) o->levels = 2;
        }
    }
    /* Foliage: mostly green art (judged on the day palette so night doesn't change shapes). */
    int *green = calloc(object_count, sizeof(int)), *total = calloc(object_count, sizeof(int));
    for (int my = 0; my < EBW_MINI_H; my++)
        for (int mx = 0; mx < EBW_MINI_W; mx++) {
            int id = object_of[my * EBW_MINI_W + mx];
            if (id < 0 || objects[id].indoors || objects[id].is_prop || objects[id].blueprint) continue;
            for (int k = 0; k < 4; k++) {
                uint32_t c = pixel_at(mx * 8 + 2 + (k & 1) * 4, my * 8 + 2 + (k >> 1) * 4, false);
                int r = c & 0xFF, g = (c >> 8) & 0xFF, b = (c >> 16) & 0xFF;
                if (r < 70 && g < 70 && b < 70) continue; /* outlines say nothing about colour */
                total[id]++;
                if (g > r + 16 && g >= b) green[id]++;
            }
        }
    for (int i = 0; i < object_count; i++) {
        Object *o = &objects[i];
        if (o->blueprint) continue;
        int w = o->x1 - o->x0 + 1, h = o->y1 - o->y0 + 1;
        /* Thin outdoor things (fences, railings) are cut out column by column like foliage, so
         * the gaps between pickets show through. */
        bool thin = !o->indoors && !o->is_prop && (o->thin || w <= 2 || h <= 2);
        o->thin = thin;
        bool green_art = total[i] && green[i] * 10 >= total[i] * 6;
        o->green = green_art;
        o->forest = green_art && !thin && o->cells > 60;
        o->foliage = thin || (green_art && !o->forest);
        /* Buildings are compact blocks; cliffs sprawl along diagonals and leave their box mostly empty. */
        float fill = (float)o->cells / (float)(w * h);
        o->terrain = !o->indoors && !o->is_prop && !o->foliage && !o->forest &&
                     (w > 48 || h > 48) && fill < 0.6f;
        if (o->terrain) o->levels = TERRAIN_LEVELS;
        if (o->forest) o->levels = FOREST_LEVELS;
    }
    free(green);
    free(total);

    /* Inside cliff and forest masses, decide cell by cell: canopy art (green with dark outlines)
     * becomes a tree wall, the rest a low ledge. */
    tree_cell = calloc(n, 1);
    for (int c = 0; c < n; c++) {
        int id = object_of[c];
        if (id < 0 || !(objects[id].terrain || objects[id].forest)) continue;
        int mx = c % EBW_MINI_W, my = c / EBW_MINI_W, g = 0, dark = 0, lit = 0;
        for (int i = 0; i < 64; i += 2) {
            uint32_t px = pixel_at(mx * 8 + (i & 7) + ((i >> 3) & 1), my * 8 + (i >> 3), false);
            int r = px & 0xFF, gg = (px >> 8) & 0xFF, b = (px >> 16) & 0xFF;
            if (r < 70 && gg < 70 && b < 70) {
                dark++;
                continue;
            }
            lit++;
            if (gg > r + 16 && gg >= b) g++;
        }
        tree_cell[c] = lit && g * 2 >= lit && dark * 10 >= 32 * 1;
    }

    /* Buildings stand as tall as their art rises above the footprint: the median column height,
     * so the whole building gets one clean height. */
    for (int i = 0; i < object_count; i++) {
        Object *o = &objects[i];
        if (o->indoors || o->is_prop || o->foliage || o->forest || o->terrain) continue;
        if (o->blueprint && o->levels > 0) continue;
        Keys keys;
        if (!ground_keys(o->x0, o->x1, o->y0, o->y1, false, &keys)) continue;
        int walls[512], roofs[512], nh = 0;
        for (int mx = o->x0; mx <= o->x1 && nh < 512; mx++) {
            int base = column_bottom(i, mx, o->y0, o->y1);
            if (base < 0) continue;
            int top = base;
            while (object_at(mx, top - 1) == i) top--;
            walls[nh] = base - top + 1;
            roofs[nh++] = art_rows_above(i, mx, base, &keys, false);
        }
        if (!nh) continue;
        int W = median(walls, nh), R = median(roofs, nh);
        int lv = W + R;
        o->levels = lv < 2 ? 2 : lv > OUTDOOR_MAX_LEVELS ? OUTDOOR_MAX_LEVELS : lv;
        detect_shape(i, W, R);
    }
    free(stack);
    /* Each stack of walk-behind cells belongs to the solid run directly beneath it (a canopy to
     * its trunk, a roof to its walls), so cut-outs never borrow a neighbour's art. */
    fg_owner = malloc(sizeof(int32_t) * n);
    memset(fg_owner, 0xFF, sizeof(int32_t) * n);
    for (int mx = 0; mx < EBW_MINI_W; mx++)
        for (int my = EBW_MINI_H - 1; my > 0; my--) {
            int id = object_of[my * EBW_MINI_W + mx];
            if (id < 0 || object_of[(my - 1) * EBW_MINI_W + mx] == id) continue; /* run tops only */
            /* A tree's lowest canopy rows (just above the trunk) aren't marked: allow a gap of up to
             * two unmarked rows before the marked stack starts. */
            int fy = my - 1, gap = 0;
            while (fy >= 0 && gap < 2 && object_of[fy * EBW_MINI_W + mx] < 0 && !ebw_foreground_at(mx, fy)) fy--, gap++;
            for (; fy >= 0 && fy >= my - 18; fy--) {
                int c = fy * EBW_MINI_W + mx;
                if (object_of[c] >= 0 || !ebw_foreground_at(mx, fy)) break;
                fg_owner[c] = id;
            }
        }
    double t0 = GetTime();
    classify_cliffs();
    solve_ground();
    TraceLog(LOG_INFO, "world: %d objects, ground solved in %.2f s", object_count, GetTime() - t0);
    analysed = true;
}

/* ---- ground elevation ---- */

/* Cliffs: outdoor solid masses that nothing walks behind (no walk-behind cells above them, unlike
 * buildings and trees) and that aren't leafy, plus every sprawling terrain mass. Inside those,
 * each cell decides for itself: rock is cliff, sky-blue or black is off-map void, and tree canopy
 * is ground (with trees on it). */
static void classify_cliffs(void)
{
    const int n = EBW_MINI_W * EBW_MINI_H;
    cell_kind = calloc(n, 1);
    for (int i = 0; i < object_count; i++) {
        Object *o = &objects[i];
        o->cliff = o->voidc = false;
        if (o->indoors || o->is_prop || o->blueprint) continue;
        if (o->terrain || o->forest) {
            o->cliff = true;
            continue;
        }
        if (o->foliage && !o->thin) continue; /* hedges and groves */
        int cols = 0, fg_cols = 0;
        for (int mx = o->x0; mx <= o->x1; mx++) {
            int base = column_bottom(i, mx, o->y0, o->y1);
            if (base < 0) continue;
            cols++;
            int top = base;
            while (object_at(mx, top - 1) == i) top--;
            if (ebw_foreground_at(mx, top - 1)) fg_cols++;
        }
        if (!cols || fg_cols * 100 >= cols * 15) continue;
        if (o->thin && o->cells < 12) continue; /* posts and short rails stay cut-outs */
        /* Only rock is a cliff: walls, planters, and hedges keep their own shapes. */
        int rock = 0, all = 0;
        for (int my = o->y0; my <= o->y1; my++)
            for (int mx = o->x0; mx <= o->x1; mx++)
                if (object_at(mx, my) == i) all++, rock += rock_cell(mx, my, false);
        if (rock * 2 < all) continue;
        o->cliff = true;
    }
    for (int c = 0; c < n; c++) {
        int id = object_of[c];
        if (id < 0 || !objects[id].cliff) continue;
        if (tree_cell[c]) continue; /* canopy inside a mass: ground, trees go on it */
        int mx = c % EBW_MINI_W, my = c / EBW_MINI_W;
        long r = 0, g = 0, b = 0;
        for (int i = 0; i < 64; i += 4) {
            uint32_t px = pixel_at(mx * 8 + (i & 7), my * 8 + (i >> 3), false);
            r += px & 0xFF, g += (px >> 8) & 0xFF, b += (px >> 16) & 0xFF;
        }
        r /= 16, g /= 16, b /= 16;
        bool sky = b > r + 20 && b > g + 20, black = r < 40 && g < 40 && b < 40;
        if (sky || black) {
            cell_kind[c] = CELL_VOID;
            continue;
        }
        /* Rock, or an outline row between rock: cliff. Anything else in a mass (grass lips, tree
         * bits) is plain ground on whichever level it touches. */
        bool rock = rock_cell(mx, my, false) || (rock_cell(mx, my - 1, false) && rock_cell(mx, my + 1, false));
        cell_kind[c] = rock ? CELL_CLIFF : CELL_GROUND;
    }
}

/* One relation between two ground cells: ground[upper] = ground[lower] + d. */
typedef struct {
    int lower, upper;
    float d;
} Rise;

#define CLIFF_K 8.0f /* weight of a cliff rise against ground smoothness */

/* y = A x for the ground energy: grid Laplacian over ground cells plus CLIFF_K per rise link. */
static void cg_apply(const float *x, float *y, const uint8_t *dom, const int *st, const int *ot)
{
    const int w = EBW_MINI_W, hgt = EBW_MINI_H;
    for (int yy = 0; yy < hgt; yy++)
        for (int xx = 0; xx < w; xx++) {
            int i = yy * w + xx;
            if (!dom[i]) {
                y[i] = 0;
                continue;
            }
            float v = 0, xi = x[i];
            if (xx > 0 && dom[i - 1]) v += xi - x[i - 1];
            if (xx + 1 < w && dom[i + 1]) v += xi - x[i + 1];
            if (yy > 0 && dom[i - w]) v += xi - x[i - w];
            if (yy + 1 < hgt && dom[i + w]) v += xi - x[i + w];
            for (int e = st[i]; e < st[i + 1]; e++) v += CLIFF_K * (xi - x[ot[e]]);
            y[i] = v;
        }
}

static void cg_solve(float *x, const uint8_t *dom, const int *st, const int *ot, const float *dl, int iters)
{
    const int n = EBW_MINI_W * EBW_MINI_H;
    float *r = malloc(sizeof(float) * n), *p = malloc(sizeof(float) * n), *ap = malloc(sizeof(float) * n);
    cg_apply(x, ap, dom, st, ot);
    double rr = 0;
    for (int i = 0; i < n; i++) {
        float b = 0;
        if (dom[i])
            for (int e = st[i]; e < st[i + 1]; e++) b += CLIFF_K * dl[e];
        r[i] = dom[i] ? b - ap[i] : 0;
        p[i] = r[i];
        rr += (double)r[i] * r[i];
    }
    for (int it = 0; it < iters && rr > 1e-6; it++) {
        cg_apply(p, ap, dom, st, ot);
        double pap = 0;
        for (int i = 0; i < n; i++) pap += (double)p[i] * ap[i];
        if (pap <= 0) break;
        float alpha = (float)(rr / pap);
        double rr2 = 0;
        for (int i = 0; i < n; i++) {
            x[i] += alpha * p[i];
            r[i] -= alpha * ap[i];
            rr2 += (double)r[i] * r[i];
        }
        float beta = (float)(rr2 / rr);
        rr = rr2;
        for (int i = 0; i < n; i++) p[i] = r[i] + beta * p[i];
    }
    free(r), free(p), free(ap);
}

/* Builds per-cell link lists (CSR) from rises. */
static void link_lists(const Rise *rises, int nr, int ncells, int **start, int **other, float **delta)
{
    *start = calloc(ncells + 1, sizeof(int));
    for (int r = 0; r < nr; r++) (*start)[rises[r].lower + 1]++, (*start)[rises[r].upper + 1]++;
    for (int i = 0; i < ncells; i++) (*start)[i + 1] += (*start)[i];
    int *fill = malloc(sizeof(int) * ncells);
    memcpy(fill, *start, sizeof(int) * ncells);
    *other = malloc(sizeof(int) * (nr * 2 + 1));
    *delta = malloc(sizeof(float) * (nr * 2 + 1));
    for (int r = 0; r < nr; r++) {
        int a = rises[r].lower, b = rises[r].upper;
        (*other)[fill[a]] = b, (*delta)[fill[a]++] = -rises[r].d;
        (*other)[fill[b]] = a, (*delta)[fill[b]++] = rises[r].d;
    }
    free(fill);
}

/* Every cliff column says "the ground above me is my height higher than the ground below me".
 * Solving for one smooth ground that keeps all of those makes plateaus flat, terraces stepped,
 * and the paths between them ramps. Solved coarse (4x) first, then refined. */
static void solve_ground(void)
{
    const int n = EBW_MINI_W * EBW_MINI_H;
    uint8_t *dom = malloc(n);
    for (int c = 0; c < n; c++) dom[c] = cell_kind[c] == CELL_GROUND;

    int cap = 65536, nr = 0;
    Rise *rises = malloc(sizeof(Rise) * cap);
    for (int mx = 0; mx < EBW_MINI_W; mx++)
        for (int my = 1; my + 1 < EBW_MINI_H; my++) {
            if (cell_kind[my * EBW_MINI_W + mx] != CELL_CLIFF || cell_kind[(my + 1) * EBW_MINI_W + mx] == CELL_CLIFF)
                continue; /* run bottoms */
            int top = my;
            while (top > 0 && cell_kind[(top - 1) * EBW_MINI_W + mx] == CELL_CLIFF) top--;
            int lo = (my + 1) * EBW_MINI_W + mx, hi = (top - 1) * EBW_MINI_W + mx;
            if (top - 1 < 0 || !dom[lo] || !dom[hi]) continue;
            if (nr == cap) rises = realloc(rises, sizeof(Rise) * (cap *= 2));
            rises[nr++] = (Rise){lo, hi, (my - top + 1) * OUTDOOR_SCALE};
        }

    /* Terraces: connected ground between cliffs. Each gets one flat height; the heights are fitted
     * (least squares) so cliff rises between terraces match their art. A rise that starts and ends
     * on the same terrace (a ramp drawn as a slope) is smoothed in afterwards. */
    int32_t *terr = malloc(sizeof(int32_t) * n);
    memset(terr, 0xFF, sizeof(int32_t) * n);
    int nterr = 0;
    int32_t *stk = malloc(sizeof(int32_t) * n);
    for (int c0 = 0; c0 < n; c0++) {
        if (!dom[c0] || terr[c0] >= 0) continue;
        int sp = 0;
        stk[sp++] = c0;
        terr[c0] = nterr;
        while (sp) {
            int c = stk[--sp], x = c % EBW_MINI_W, y = c / EBW_MINI_W;
            const int nb[4][2] = {{1, 0}, {-1, 0}, {0, 1}, {0, -1}};
            for (int k = 0; k < 4; k++) {
                int nx = x + nb[k][0], ny = y + nb[k][1];
                if (nx < 0 || ny < 0 || nx >= EBW_MINI_W || ny >= EBW_MINI_H) continue;
                int ni = ny * EBW_MINI_W + nx;
                if (dom[ni] && terr[ni] < 0) terr[ni] = nterr, stk[sp++] = ni;
            }
        }
        nterr++;
    }
    free(stk);

    /* Edges between terraces: summed rise and count per (lower, upper) link. */
    Rise *trises = malloc(sizeof(Rise) * (nr + 1));
    int ntr = 0;
    uint8_t *self_rise = calloc(nterr, 1);
    for (int r = 0; r < nr; r++) {
        int a = terr[rises[r].lower], b = terr[rises[r].upper];
        if (a == b) {
            self_rise[a] = 1;
            continue;
        }
        trises[ntr++] = (Rise){a, b, rises[r].d};
    }
    float *th = calloc(nterr, sizeof(float));
    int *st, *ot;
    float *dl;
    link_lists(trises, ntr, nterr, &st, &ot, &dl);
    /* Gauss-Seidel on the terrace graph: each terrace sits at the average its cliffs ask for. */
    for (int it = 0; it < 4000; it++) {
        float change = 0;
        for (int t = 0; t < nterr; t++) {
            if (st[t] == st[t + 1]) continue;
            float sum = 0;
            for (int e = st[t]; e < st[t + 1]; e++) sum += th[ot[e]] + dl[e];
            float v = sum / (st[t + 1] - st[t]);
            change += fabsf(v - th[t]);
            th[t] = v;
        }
        if (change < 0.01f) break;
    }
    free(st), free(ot), free(dl);

    ground = calloc(n, sizeof(float));
    for (int c = 0; c < n; c++) ground[c] = dom[c] ? th[terr[c]] : 0;

    /* Refine the whole field: least squares over smoothness between neighbouring ground cells plus
     * every cliff rise (including those within one terrace), solved by conjugate gradients from the
     * terrace solution. Plateaus stay nearly flat, cliffs keep their drops, and where the ground
     * wraps around a cliff's end it ramps. */
    link_lists(rises, nr, n, &st, &ot, &dl);
    cg_solve(ground, dom, st, ot, dl, 600);
    free(st), free(ot), free(dl);

    /* Flatten: grow regions of ground within half a unit of their seed and set each to its mean.
     * Level ground comes out dead flat; real slopes become gentle steps that the corner averaging
     * smooths back out. */
    {
        int32_t *reg = malloc(sizeof(int32_t) * n), *q = malloc(sizeof(int32_t) * n);
        memset(reg, 0xFF, sizeof(int32_t) * n);
        int nreg = 0;
        for (int c0 = 0; c0 < n; c0++) {
            if (!dom[c0] || reg[c0] >= 0) continue;
            float seed = ground[c0];
            int qh = 0, qt = 0;
            q[qt++] = c0;
            reg[c0] = nreg;
            double sum = 0;
            while (qh < qt) {
                int c = q[qh++], x = c % EBW_MINI_W, y = c / EBW_MINI_W;
                sum += ground[c];
                const int nb[4][2] = {{1, 0}, {-1, 0}, {0, 1}, {0, -1}};
                for (int k = 0; k < 4; k++) {
                    int nx = x + nb[k][0], ny = y + nb[k][1];
                    if (nx < 0 || ny < 0 || nx >= EBW_MINI_W || ny >= EBW_MINI_H) continue;
                    int ni = ny * EBW_MINI_W + nx;
                    if (dom[ni] && reg[ni] < 0 && fabsf(ground[ni] - seed) < 0.5f) reg[ni] = nreg, q[qt++] = ni;
                }
            }
            float mean = (float)(sum / qt);
            for (int i = 0; i < qt; i++) ground[q[i]] = mean;
            nreg++;
        }
        free(reg), free(q);
    }
    free(th), free(trises), free(self_rise), free(terr);

    float *cliff_top = malloc(sizeof(float) * n);
    /* Shift so the lowest ground sits at 0. */
    float lo = 1e9f;
    for (int c = 0; c < n; c++)
        if (dom[c] && ground[c] < lo) lo = ground[c];
    for (int c = 0; c < n; c++) ground[c] = dom[c] ? ground[c] - lo : 0;
    /* Non-ground cells that aren't cliffs (void) take their neighbours' level, never 0. */

    /* Cliff cells carry the height of the ground above them (their top surface). Where there's no
     * ground above (the edge of the world), the face rises its own height from the ground below. */
    for (int mx = 0; mx < EBW_MINI_W; mx++)
        for (int my = 0; my < EBW_MINI_H; my++) {
            if (cell_kind[my * EBW_MINI_W + mx] != CELL_CLIFF) continue;
            int top = my, bot = my;
            while (top > 0 && cell_kind[(top - 1) * EBW_MINI_W + mx] == CELL_CLIFF) top--;
            while (bot + 1 < EBW_MINI_H && cell_kind[(bot + 1) * EBW_MINI_W + mx] == CELL_CLIFF) bot++;
            int above = (top - 1) * EBW_MINI_W + mx, below = (bot + 1) * EBW_MINI_W + mx;
            float h;
            if (top > 0 && dom[above]) h = ground[above];
            else if (bot + 1 < EBW_MINI_H && dom[below]) h = ground[below] + (bot - top + 1) * OUTDOOR_SCALE;
            else h = ground[my * EBW_MINI_W + mx];
            cliff_top[my * EBW_MINI_W + mx] = h;
        }
    for (int c = 0; c < n; c++)
        if (cell_kind[c] == CELL_CLIFF) ground[c] = cliff_top[c];
    free(cliff_top);
    free(dom), free(rises);
}

static int owner_at(int mx, int my)
{
    if (mx < 0 || my < 0 || mx >= EBW_MINI_W || my >= EBW_MINI_H || !fg_owner) return -1;
    return fg_owner[my * EBW_MINI_W + mx];
}

/* Cliff art mixes rock with the grass lip and the diagonal slopes drawn in green. Faces should be
 * rock: use the wanted row if it's rock-coloured, else the nearest rock row in the column (or a
 * neighbouring column). */
static bool rock_cell(int mx, int my, bool event)
{
    int rock = 0, lit = 0;
    for (int i = 0; i < 64; i += 3) {
        uint32_t c = pixel_at(mx * 8 + (i & 7), my * 8 + (i >> 3), event);
        int r = c & 0xFF, g = (c >> 8) & 0xFF, b = (c >> 16) & 0xFF;
        if (r < 60 && g < 60 && b < 60) continue;
        lit++;
        if (r > g + 15 && r > b) rock++;
    }
    return lit && rock * 10 >= lit * 6;
}

static int rock_row(int mx, int top, int bot, int want, bool event)
{
    if (rock_cell(mx, want, event)) return want;
    for (int d = 1; d <= bot - top; d++) {
        if (want + d <= bot && rock_cell(mx, want + d, event)) return want + d;
        if (want - d >= top && rock_cell(mx, want - d, event)) return want - d;
    }
    return want;
}

static float ground_at(int mx, int my)
{
    if (mx < 0 || my < 0 || mx >= EBW_MINI_W || my >= EBW_MINI_H || !ground) return 0;
    return ground[my * EBW_MINI_W + mx];
}

static int kind_at(int mx, int my)
{
    if (mx < 0 || my < 0 || mx >= EBW_MINI_W || my >= EBW_MINI_H || !cell_kind) return CELL_VOID;
    return cell_kind[my * EBW_MINI_W + mx];
}

/* Height of a cell corner (vertex at mx,my = top-left of cell mx,my), averaged over the cells of
 * the given kind that share it, so ground stays smooth but cliff edges stay sharp. */
static float corner_height(int vx, int vy, int kind, float fallback)
{
    float sum = 0;
    int cnt = 0;
    for (int dy = -1; dy <= 0; dy++)
        for (int dx = -1; dx <= 0; dx++)
            if (kind_at(vx + dx, vy + dy) == kind) sum += ground_at(vx + dx, vy + dy), cnt++;
    return cnt ? sum / cnt : fallback;
}

/* Small things (trees, bushes, signs, posts) become cut-out sprites; the rest are solid. */
static bool looks_like_prop(const Object *o)
{
    int w = o->x1 - o->x0 + 1, h = o->y1 - o->y0 + 1;
    return w <= 6 && h <= 6 && o->cells <= 30 && !o->indoors;
}

/* How many rows of art stand above the solid run ending at `base` in column mx: the walk-behind
 * cells directly above it (a roof, a canopy, a sign's board). */
static int art_rows_above(int id, int mx, int base, const Keys *keys, bool event)
{
    int top = base;
    while (object_at(mx, top - 1) == id) top--;
    (void)keys;
    (void)event;
    /* The game marks the cells Ness walks behind (they hide part of his sprite): exactly the art
     * standing above a footprint. Roads and lawns behind a building are never marked. */
    int extra = 0;
    for (int my = top - 1; my >= top - 16 && my >= 0; my--) {
        if (object_at(mx, my) != -1 || !ebw_foreground_at(mx, my)) break;
        extra++;
    }
    return extra;
}

static int column_bottom(int id, int mx, int y0, int y1)
{
    for (int my = y1; my >= y0; my--)
        if (object_at(mx, my) == id) return my;
    return -1;
}

/* A building's solid rows are its front wall; the walk-behind rows above are its roof seen from
 * above, which is also its depth. Bright, saturated roofs are pitched; pale ones are flat. */
static void detect_shape(int id, int W, int R)
{
    Object *o = &objects[id];
    o->shaped = W >= 2 && R >= 2;
    o->gable = false;
    if (!o->shaped) return;
    o->wall_rows = W;
    o->roof_rows = R;
    if (R < 3) return;
    uint32_t pal[48];
    int cnt[48], np = 0, best = -1;
    for (int mx = o->x0; mx <= o->x1; mx++) {
        int base = column_bottom(id, mx, o->y0, o->y1);
        if (base < 0) continue;
        for (int j = 0; j < R; j++)
            for (int i = 0; i < 64; i += 3) {
                uint32_t c = pixel_at(mx * 8 + (i & 7), (base - W - j) * 8 + (i >> 3), false);
                int cr = c & 0xFF, cg = (c >> 8) & 0xFF, cb = (c >> 16) & 0xFF;
                if (cr < 70 && cg < 70 && cb < 70) continue;
                int k = 0;
                while (k < np && pal[k] != c) k++;
                if (k == np) {
                    if (np == 48) continue;
                    pal[np] = c;
                    cnt[np++] = 0;
                }
                cnt[k]++;
            }
    }
    for (int k = 0; k < np; k++)
        if (best < 0 || cnt[k] > cnt[best]) best = k;
    if (best < 0) return;
    uint32_t d = pal[best];
    int dr = d & 0xFF, dg = (d >> 8) & 0xFF, db = (d >> 16) & 0xFF;
    int hi = dr > dg ? (dr > db ? dr : db) : (dg > db ? dg : db), lo = dr < dg ? (dr < db ? dr : db) : (dg < db ? dg : db);
    bool greenish = dg > dr + 16 && dg >= db; /* grassy cliff lips are not roofs */
    o->gable = hi - lo >= 60 && !greenish;
    if (greenish) o->shaped = false;
}

/* ---- props: small objects as camera-facing cut-outs ---- */

#define PROP_ATLAS 2048

typedef struct {
    float x, y, z, w, h; /* y: ground height at its base */
    Rectangle src;       /* in the prop atlas */
} Prop;

static Texture2D prop_atlas;

/* Standalone trees already cut out, reused to plant forests as groves of individual trees. */
typedef struct {
    Rectangle src;
    float w, h;
    uint32_t ground;
} TreeTemplate;
static TreeTemplate tree_tpl[8];
static int tree_tpl_count;
static int shelf_x, shelf_y, shelf_h;

/* Pixels erased from the floor where a prop used to be painted; per map tile, 0 = unchanged. */
static uint32_t **floor_patch[2];

static bool pack_prop(int w, int h, Rectangle *out)
{
    if (shelf_x + w > PROP_ATLAS) {
        shelf_x = 0;
        shelf_y += shelf_h;
        shelf_h = 0;
    }
    if (shelf_y + h > PROP_ATLAS || w > PROP_ATLAS) {
        atlas_full = true;
        return false;
    }
    *out = (Rectangle){(float)shelf_x, (float)shelf_y, (float)w, (float)h};
    shelf_x += w + 1;
    if (h + 1 > shelf_h) shelf_h = h + 1;
    return true;
}

static void patch_floor(int px, int py, uint32_t colour, bool event)
{
    if (px < 0 || py < 0 || px >= EBW_MAP_W * 32 || py >= EBW_MAP_H * 32) return;
    int t = (py >> 5) * EBW_MAP_W + (px >> 5);
    uint32_t **patches = floor_patch[event ? 1 : 0];
    if (!patches[t]) patches[t] = calloc(32 * 32, sizeof(uint32_t));
    patches[t][(py & 31) * 32 + (px & 31)] = colour;
}

/* The ground colours around a spot, from the open cells just
 * below the base and beside the solid part. Those pixels become transparent in cut-outs. */
static bool is_key(const Keys *k, uint32_t c)
{
    for (int i = 0; i < k->n; i++)
        if (k->c[i] == c) return true;
    return false;
}

static bool ground_keys_pct(int x0, int x1, int y0, int y1, bool event, Keys *keys, int pct)
{
    uint32_t colours[32];
    int counts[32], ncol = 0, total = 0;
    for (int my = y0; my <= y1 + 2; my++)
        for (int mx = x0 - 2; mx <= x1 + 2; mx++) {
            bool below = my > y1, beside = (mx < x0 || mx > x1) && my == y1;
            if (!(below || beside) || solid(mx, my)) continue;
            /* Never sample leaves: walk-behind cells and the canopy rows just under them. */
            if (ebw_foreground_at(mx, my) || ebw_foreground_at(mx, my - 1) || ebw_foreground_at(mx, my - 2)) continue;
            for (int i = 0; i < 64; i++) {
                uint32_t c = pixel_at(mx * 8 + (i & 7), my * 8 + (i >> 3), event);
                int k = 0;
                while (k < ncol && colours[k] != c) k++;
                if (k == ncol) {
                    if (ncol == 32) continue;
                    colours[ncol] = c;
                    counts[ncol++] = 0;
                }
                counts[k]++;
                total++;
            }
        }
    keys->n = 0;
    if (pct > 0) {
        /* Loose: every colour above pct% (used right at the base, where shadows are painted). */
        for (int k = 0; k < ncol && keys->n < MAX_KEYS; k++)
            if (counts[k] * 100 >= total * pct) keys->c[keys->n++] = colours[k];
    } else {
        /* Strict: the dominant ground colour plus any close runner-up. Canopies share the
         * grass's minor colours, so those must stay. */
        int best = 0;
        for (int k = 1; k < ncol; k++)
            if (counts[k] > counts[best]) best = k;
        if (ncol) keys->c[keys->n++] = colours[best];
        for (int k = 0; k < ncol && keys->n < 3; k++)
            if (k != best && counts[k] * 4 >= counts[best]) keys->c[keys->n++] = colours[k];
    }
    return keys->n > 0;
}

/* Mostly green art in the walk-behind cells of a window: a tree or bush rather than a sign. */
static bool is_leafy(int id, int wx0, int wy0, int ww, int wh, bool event)
{
    (void)id;
    int green = 0, lit = 0;
    for (int y = 0; y < wh; y += 2)
        for (int x = 0; x < ww; x += 2) {
            int px = wx0 + x, py = wy0 + y;
            if (px < 0 || py < 0 || !ebw_foreground_at(px >> 3, py >> 3)) continue;
            uint32_t c = pixel_at(px, py, event);
            int r = c & 0xFF, g = (c >> 8) & 0xFF, b = (c >> 16) & 0xFF;
            if (r < 70 && g < 70 && b < 70) continue;
            lit++;
            if (g > r + 16 && g >= b) green++;
        }
    return lit && green * 2 >= lit;
}

/* Cuts an object's art out of a pixel window of the map: its own pixels plus everything drawn
 * connected to them (a tree's canopy), minus the ground colours. The cut-out pixels are erased
 * from the floor. The sprite stands at depth z. False if nothing could be cut. */
static bool cut_out(int id, int wx0, int wy0, int ww, int wh, const Keys *keys, const Keys *base_keys,
                    int base_py, int open_max_py, bool event, float z, Prop *out)
{
    bool leafy = is_leafy(id, wx0, wy0, ww, wh, event);
    enum { MAXW = 96, MAXH = 128 };
    if (ww > MAXW || wh > MAXH || ww <= 0 || wh <= 0) return false;
    static uint32_t win[MAXW * MAXH];
    static uint8_t mask[MAXW * MAXH];
    static int queue[MAXW * MAXH];
    int qn = 0;
    for (int y = 0; y < wh; y++)
        for (int x = 0; x < ww; x++) {
            int px = wx0 + x, py = wy0 + y;
            uint32_t c = pixel_at(px, py, event);
            win[y * ww + x] = c;
            int obj = (px >= 0 && py >= 0) ? object_at(px >> 3, py >> 3) : -2;
            /* Open ground only counts above the object (a canopy, a sign's board), never beside or
             * below it, so neighbouring roads and lawns don't get dragged in. */
            /* Around the base, shadows are painted in colours the canopy also uses; there a wider
             * set of ground colours applies. */
            const Keys *k = py >= base_py ? base_keys : keys;
            /* Open ground only counts where the game draws it in front of Ness (a canopy, a sign's
             * board), so neighbouring roads and lawns never get dragged in. */
            bool fg = false;
            if (obj == -1 && px >= 0 && py >= 0) {
                int cx = px >> 3, cy = py >> 3;
                fg = owner_at(cx, cy) == id || (owner_at(cx, cy) < 0 && ebw_foreground_at(cx, cy));
                /* Leafy things: the game marks only the upper canopy as walk-behind; the rows just
                 * below it, down to the trunk, are canopy too. */
                if (!fg && leafy && py < open_max_py && !ebw_foreground_at(cx, cy))
                    for (int up = 1; up <= 2 && !fg; up++)
                        fg = ebw_foreground_at(cx, cy - up) && (owner_at(cx, cy - up) == id || owner_at(cx, cy - up) < 0);
            }
            bool allowed = (obj == id || fg) && !is_key(k, c);
            /* Leafy cut-outs keep only tree colours (greens, outlines, trunk browns): canopies are
             * painted over walls in the original art, and the wall mustn't come along. */
            if (allowed && leafy) {
                int r = c & 0xFF, g = (c >> 8) & 0xFF, b = (c >> 16) & 0xFF;
                int hi = r > g ? (r > b ? r : b) : (g > b ? g : b);
                bool tree_colour = (g > r && g + 10 >= b) || hi < 90 || (r > g && r > b && b < 140);
                if (!tree_colour) allowed = false;
                /* At the base only the trunk and roots stand up; the painted shadow stays on the
                 * ground (outlines only next to bark). */
                if (allowed && py >= base_py) {
                    bool bark = r > g + 10 && r > b;
                    if (!bark && hi < 90) {
                        bark = false;
                        const int nb[4][2] = {{1, 0}, {-1, 0}, {0, 1}, {0, -1}};
                        for (int q = 0; q < 4 && !bark; q++) {
                            uint32_t n2 = pixel_at(px + nb[q][0], py + nb[q][1], event);
                            int nr = n2 & 0xFF, ng = (n2 >> 8) & 0xFF, nbb = (n2 >> 16) & 0xFF;
                            bark = nr > ng + 10 && nr > nbb;
                        }
                    }
                    if (!bark) allowed = false;
                }
            }
            mask[y * ww + x] = allowed ? 1 : 0;
            /* Seeds: the object's own cells and the walk-behind art it owns (its canopy), so a
             * trunk row trimmed to bark can't cut the canopy off. */
            bool owned = obj == id || (px >= 0 && py >= 0 && owner_at(px >> 3, py >> 3) == id);
            if (allowed && owned) {
                mask[y * ww + x] = 2;
                queue[qn++] = y * ww + x;
            }
        }
    for (int qi = 0; qi < qn; qi++) {
        int p = queue[qi], x = p % ww, y = p / ww;
        const int nb[4][2] = {{1, 0}, {-1, 0}, {0, 1}, {0, -1}};
        for (int k = 0; k < 4; k++) {
            int nx = x + nb[k][0], ny = y + nb[k][1];
            if (nx < 0 || ny < 0 || nx >= ww || ny >= wh) continue;
            int np = ny * ww + nx;
            if (mask[np] == 1) {
                mask[np] = 2;
                queue[qn++] = np;
            }
        }
    }
    int bx0 = ww, by0 = wh, bx1 = -1, by1 = -1;
    for (int y = 0; y < wh; y++)
        for (int x = 0; x < ww; x++)
            if (mask[y * ww + x] == 2) {
                if (x < bx0) bx0 = x;
                if (x > bx1) bx1 = x;
                if (y < by0) by0 = y;
                if (y > by1) by1 = y;
            }
    if (bx1 < 0) return false;
    by1 = wh - 1; /* keep the sprite's feet on the ground line */
    int bw = bx1 - bx0 + 1, bh = by1 - by0 + 1;
    Rectangle r;
    if (!pack_prop(bw, bh, &r)) return false;

    static uint32_t img[MAXW * MAXH];
    for (int y = 0; y < bh; y++)
        for (int x = 0; x < bw; x++) {
            int wi = (by0 + y) * ww + bx0 + x;
            bool in = mask[wi] == 2;
            img[y * bw + x] = in ? win[wi] : 0;
            if (in) patch_floor(wx0 + bx0 + x, wy0 + by0 + y, keys->c[0], event);
        }
    UpdateTextureRec(prop_atlas, r, img);

    out->src = r;
    out->w = bw / 8.0f;
    out->h = bh / 8.0f;
    out->x = (wx0 + bx0 + bw * 0.5f) / 8.0f;
    out->z = z;
    out->y = ground_at((int)out->x, (int)z);
    return true;
}

static bool make_prop(Object *o, bool event, Prop *out)
{
    Keys keys, base_keys;
    if (!ground_keys(o->x0, o->x1, o->y0, o->y1, event, &keys)) return false;
    ground_keys_pct(o->x0 - 1, o->x1 + 1, o->y0, o->y1, event, &base_keys, 3);
    int id = (int)(o - objects);
    /* A canopy may hang over the top half of the trunk rows, but not down to the ground. */
    int open_max = (o->y0 + (o->y1 - o->y0 + 1) / 2) * 8;
    int wx0 = (o->x0 - 2) * 8, wy0 = (o->y0 - 8) * 8, ww = (o->x1 - o->x0 + 5) * 8, wh = (o->y1 - o->y0 + 9) * 8;
    /* Bark-only through the trunk rows (from the middle of the top one), where the painted shadow
     * sits; the canopy above is unaffected. */
    int bark_from = o->y1 > o->y0 ? o->y0 * 8 + 5 : o->y1 * 8 + 4;
    bool ok = cut_out(id, wx0, wy0, ww, wh, &keys, &base_keys, bark_from, open_max,
                      event, (o->y0 + o->y1 + 1) * 0.5f, out);
    /* A typical round tree becomes a template for planting forests. */
    if (ok && tree_tpl_count < 8 && is_leafy(id, wx0, wy0, ww, wh, event) && out->w >= 3 && out->w <= 7 &&
        out->h >= 4 && out->h <= 9)
        tree_tpl[tree_tpl_count++] = (TreeTemplate){out->src, out->w, out->h, keys.c[0]};
    return ok;
}

/* True if any walk-behind cell above the object belongs to it (a canopy, a board, a roof). */
static bool owns_art_above(int id, const Object *o)
{
    for (int mx = o->x0; mx <= o->x1; mx++)
        for (int my = o->y0 - 4; my < o->y0; my++)
            if (owner_at(mx, my) == id) return true;
    return false;
}

/* One column of a tree cluster: the run of solid cells ending at `base`, plus canopy above. */
static bool make_strip(int id, int mx, int base, bool event, Prop *out)
{
    Keys keys, loose;
    int top = base;
    while (top > base - 9 && object_at(mx, top - 1) == id) top--;
    if (!ground_keys(mx, mx, top, base, event, &keys)) return false;
    /* Wider ground sample for the loose keys: a lone column sees little ground of its own. */
    ground_keys_pct(mx - 2, mx + 2, top, base, event, &loose, 3);
    int run_top = top;
    top -= 6; /* canopy drawn above the solid part */
    /* Fences share no colours with grass: drop every ground colour, all the way up, so the gaps
     * between pickets show through. Hedges are green, so only their base gets that treatment. */
    int loose_from = objects[id].thin ? 0 : base * 8 + 4;
    return cut_out(id, mx * 8, top * 8, 8, (base - top + 1) * 8, &keys, &loose, loose_from, run_top * 8, event,
                   base + 0.5f, out);
}

/* ---- per-sector chunks ---- */

#define CHUNK_MW (EBW_SECTOR_W * 4) /* minitiles */
#define CHUNK_MH (EBW_SECTOR_H * 4)

/* A straight run of fence between two cell centres, wearing a repeated picket column. */
typedef struct {
    float x0, z0, y0, x1, z1, y1, h;
    Rectangle src;
} FenceSeg;

typedef struct {
    bool built, has_mesh, props_done, trees_done;
    Mesh mesh;
    Prop *props;
    int nprops;
    FenceSeg *fences;
    int nfences;
    int last_used;
} Chunk;

static Chunk chunks[EBW_SECTORS_Y][EBW_SECTORS_X];
static int frame_no;
static bool chunks_pending; /* last frame wanted to build more chunks than its budget allowed */

static void chunk_add_prop(Chunk *c, Prop p)
{
    c->props = realloc(c->props, sizeof(Prop) * (c->nprops + 1));
    c->props[c->nprops++] = p;
}

static void ensure_props(int sx, int sy, bool event);

/* One fence cell: link it to its fence neighbours with straight picket runs, and wipe the
 * painted fence (and its shadow) off the ground. */
static void add_fence_cell(Chunk *c, int id, int mx, int my, bool event)
{
    Object *o = &objects[id];
    if (!o->fence_made) {
        /* Picket template: a column where the fence runs straight across (neighbours east and
         * west), else any of its cells. */
        int bx = -1, by = -1;
        for (int y = o->y0; y <= o->y1 && bx < 0; y++)
            for (int x = o->x0; x <= o->x1; x++)
                if (object_at(x, y) == id && object_at(x - 1, y) == id && object_at(x + 1, y) == id &&
                    object_at(x, y + 1) != id) {
                    bx = x, by = y;
                    break;
                }
        if (bx < 0) bx = mx, by = column_bottom(id, mx, o->y0, o->y1);
        Prop p;
        o->fence_made = true;
        Keys fk, floose;
        bool keyed = by >= 0 && ground_keys(bx, bx, by, by, event, &fk);
        if (keyed) ground_keys_pct(bx - 2, bx + 2, by, by, event, &floose, 3);
        /* Pickets: the fence's own cell and the row above it (the picket tops), nothing more. */
        if (keyed && cut_out(id, bx * 8, (by - 1) * 8, 8, 16, &fk, &floose, 0, (by - 1) * 8, event, by + 0.5f, &p)) {
            o->fence_src = p.src;
            o->fence_h = p.h * OUTDOOR_SCALE;
        } else {
            o->fence_h = 0;
        }
    }
    Keys loose;
    if (ground_keys_pct(mx - 2, mx + 2, my, my, event, &loose, 3)) {
        /* Fill with the ground right under this cell (grass, not the path beside it). */
        uint32_t cols[64];
        int cnt[64] = {0}, nc = 0, best = 0;
        for (int i = 0; i < 64; i++) {
            uint32_t c = pixel_at(mx * 8 + (i & 7), (my + 1) * 8 + (i >> 3), event);
            int k = 0;
            while (k < nc && cols[k] != c) k++;
            if (k == nc) cols[nc++] = c;
            if (++cnt[k] > cnt[best]) best = k;
        }
        uint32_t fill = nc ? cols[best] : loose.c[0];
        for (int ry = my - 3; ry <= my; ry++) {
            if (ry != my && object_at(mx, ry) >= 0) continue;
            for (int i = 0; i < 64; i++) {
                int px = mx * 8 + (i & 7), py = ry * 8 + (i >> 3);
                if (!is_key(&loose, pixel_at(px, py, event))) patch_floor(px, py, fill, event);
            }
        }
    }
    if (o->fence_h <= 0) return;
    /* Links: east and south; diagonals only where no straight path joins the two cells. */
    const int links[4][2] = {{1, 0}, {0, 1}, {1, 1}, {-1, 1}};
    for (int k = 0; k < 4; k++) {
        int nx = mx + links[k][0], ny = my + links[k][1];
        if (object_at(nx, ny) != id) continue;
        if (k == 2 && (object_at(mx + 1, my) == id || object_at(mx, my + 1) == id)) continue;
        if (k == 3 && (object_at(mx - 1, my) == id || object_at(mx, my + 1) == id)) continue;
        c->fences = realloc(c->fences, sizeof(FenceSeg) * (c->nfences + 1));
        c->fences[c->nfences++] = (FenceSeg){mx + 0.5f, my + 0.5f, ground_at(mx, my), nx + 0.5f, ny + 0.5f,
                                             ground_at(nx, ny), o->fence_h, o->fence_src};
    }
}

/* Forest canopy cells: wipe the canopy off the ground and plant individual trees, cloned from
 * standalone trees cut out nearby, on a jittered 3-cell grid. */
static void plant_forest(Chunk *c, int sx, int sy, bool event)
{
    bool any = false;
    for (int my = sy * CHUNK_MH; my < (sy + 1) * CHUNK_MH && !any; my++)
        for (int mx = sx * CHUNK_MW; mx < (sx + 1) * CHUNK_MW && !any; mx++)
            any = kind_at(mx, my) == CELL_GROUND && tree_cell[my * EBW_MINI_W + mx];
    if (!any) return;
    for (int r = 1; r <= 6 && !tree_tpl_count; r++) /* find standalone trees to copy */
        for (int dy = -r; dy <= r; dy++)
            for (int dx = -r; dx <= r; dx++) ensure_props(sx + dx, sy + dy, event);
    if (!tree_tpl_count) {
        /* No tree to copy anywhere near: at least clear the canopy off the ground. */
        Keys k;
        for (int my = sy * CHUNK_MH; my < (sy + 1) * CHUNK_MH; my++)
            for (int mx = sx * CHUNK_MW; mx < (sx + 1) * CHUNK_MW; mx++)
                if (kind_at(mx, my) == CELL_GROUND && tree_cell[my * EBW_MINI_W + mx] &&
                    ground_keys(mx - 3, mx + 3, my, my, event, &k))
                    for (int i = 0; i < 64; i++) patch_floor(mx * 8 + (i & 7), my * 8 + (i >> 3), k.c[0], event);
        return;
    }
    for (int my = sy * CHUNK_MH; my < (sy + 1) * CHUNK_MH; my++)
        for (int mx = sx * CHUNK_MW; mx < (sx + 1) * CHUNK_MW; mx++) {
            int ci = my * EBW_MINI_W + mx;
            if (kind_at(mx, my) != CELL_GROUND || !tree_cell[ci]) continue;
            unsigned hsh = (unsigned)(mx * 73856093) ^ (unsigned)(my * 19349663);
            const TreeTemplate *t = &tree_tpl[hsh % tree_tpl_count];
            for (int i = 0; i < 64; i++) patch_floor(mx * 8 + (i & 7), my * 8 + (i >> 3), t->ground, event);
            if (mx % 3 || my % 3) continue;
            Prop p = {0};
            p.src = t->src;
            p.w = t->w;
            p.h = t->h;
            p.x = mx + 0.5f + (float)((hsh >> 8) % 100) / 100.0f - 0.5f;
            p.z = my + 0.5f + (float)((hsh >> 16) % 100) / 100.0f - 0.5f;
            p.y = ground_at(mx, my);
            chunk_add_prop(c, p);
        }
}

/* ---- matched trees ---- */

static int tm_event = -1;          /* palette the tree sprites were cut with; -1 = not yet */
static Rectangle tm_rect[8];       /* where each sprite sits in the prop atlas */
static uint8_t *tm_claim;          /* minitiles whose solid cells belong to a matched tree */

static void ensure_tree_sprites(bool event)
{
    if (tm_event == (event ? 1 : 0)) return;
    tm_event = event ? 1 : 0;
    int n = tm_init(pixel_at, event, ebw_tileset_at);
    for (int i = 0; i < n && i < 8; i++) {
        const TreeSprite *t = tm_sprite(i);
        if (pack_prop(t->w, t->h, &tm_rect[i])) UpdateTextureRec(prop_atlas, tm_rect[i], t->px);
        if (getenv("FPEB_DUMP_TREES")) {
            Image im = {(void *)t->px, t->w, t->h, 1, PIXELFORMAT_UNCOMPRESSED_R8G8B8A8};
            ExportImage(im, TextFormat("/tmp/tm_%d.png", i));
        }
    }
    if (!tm_claim) tm_claim = calloc(EBW_MINI_W * EBW_MINI_H, 1);
    else memset(tm_claim, 0, EBW_MINI_W * EBW_MINI_H);
    TraceLog(LOG_INFO, "trees: %d sprite(s) cut", n);
}

/* Finds the trees standing in a chunk: each becomes a clean sprite, its painted copy is wiped
 * off the ground, and the collision objects under its trunk are claimed so nothing else draws
 * them. */
static void detect_trees(int sx, int sy, bool event)
{
    if (sx < 0 || sy < 0 || sx >= EBW_SECTORS_X || sy >= EBW_SECTORS_Y) return;
    Chunk *c = &chunks[sy][sx];
    if (c->trees_done) return;
    c->trees_done = true;
    ensure_tree_sprites(event);
    static TreeMatch found[256];
    int n = tm_find(sx * CHUNK_MW * 8, sy * CHUNK_MH * 8, (sx + 1) * CHUNK_MW * 8, (sy + 1) * CHUNK_MH * 8, event,
                    found, 256);
    for (int i = 0; i < n; i++) {
        const TreeSprite *t = tm_sprite(found[i].sprite);
        int ox = found[i].x, oy = found[i].y;
        for (int y = 0; y < t->h; y++)
            for (int x = 0; x < t->w; x++) {
                uint32_t p = t->px[y * t->w + x];
                if (p >> 24 && pixel_at(ox + x, oy + y, event) == p) patch_floor(ox + x, oy + y, t->grass, event);
            }
        /* Claim the solid cells under the lower part (trunk and roots). */
        for (int my = (oy + t->h / 2) / 8; my <= (oy + t->h - 1) / 8; my++)
            for (int mx = ox / 8; mx <= (ox + t->w - 1) / 8; mx++)
                if (mx >= 0 && my >= 0 && mx < EBW_MINI_W && my < EBW_MINI_H && solid(mx, my)) {
                    tm_claim[my * EBW_MINI_W + mx] = 1;
                    int id = object_at(mx, my);
                    if (id >= 0 && objects[id].cells <= 400) objects[id].hidden = true;
                }
        Prop p = {0};
        p.src = tm_rect[found[i].sprite];
        p.w = t->w / 8.0f;
        p.h = t->h / 8.0f;
        p.x = (ox + t->w * 0.5f) / 8.0f;
        p.z = (oy + t->h) / 8.0f - 0.4f;
        p.y = ground_at((int)p.x, (int)(p.z));
        chunk_add_prop(c, p);
    }
    /* Inside forest masses, canopy left over from trees hidden behind others becomes grass. */
    if (tm_count() > 0)
        for (int my = sy * CHUNK_MH; my < (sy + 1) * CHUNK_MH; my++)
            for (int mx = sx * CHUNK_MW; mx < (sx + 1) * CHUNK_MW; mx++)
                if (tree_cell[my * EBW_MINI_W + mx] && kind_at(mx, my) == CELL_GROUND)
                    for (int i = 0; i < 64; i++) patch_floor(mx * 8 + (i & 7), my * 8 + (i >> 3), tm_sprite(0)->grass, event);
}

/* Cuts out the props whose base lies in this chunk. Their canopies can reach into the chunk to
 * the north, so a chunk's floor is built only after its southern neighbours have run this. */
static void ensure_props(int sx, int sy, bool event)
{
    if (sx < 0 || sy < 0 || sx >= EBW_SECTORS_X || sy >= EBW_SECTORS_Y) return;
    Chunk *c = &chunks[sy][sx];
    if (c->props_done) return;
    c->props_done = true;
    for (int dy = -1; dy <= 1; dy++)
        for (int dx = -1; dx <= 1; dx++) detect_trees(sx + dx, sy + dy, event);
    for (int my = sy * CHUNK_MH; my < (sy + 1) * CHUNK_MH; my++)
        for (int mx = sx * CHUNK_MW; mx < (sx + 1) * CHUNK_MW; mx++) {
            int id = object_at(mx, my);
            if (id < 0) continue;
            Object *o = &objects[id];
            Prop p;
            if (o->hidden || (tm_claim && tm_claim[my * EBW_MINI_W + mx])) continue;
            bool building = !o->indoors && !o->is_prop && !o->foliage && !o->forest && !o->terrain && !o->cliff &&
                            !o->voidc;
            if (building) {
                /* Its art now stands up as walls: wipe the copy painted on the ground behind it. */
                if (o->erased || my != o->y1) continue;
                o->erased = true;
                Keys keys;
                if (!ground_keys(o->x0, o->x1, o->y0, o->y1, event, &keys)) continue;
                for (int cx = o->x0; cx <= o->x1; cx++) {
                    int base = column_bottom(id, cx, o->y0, o->y1);
                    if (base < 0) continue;
                    int top = base;
                    while (object_at(cx, top - 1) == id) top--;
                    int extra = art_rows_above(id, cx, base, &keys, event);
                    int erase_to = o->shaped ? base - building_depth(o) : top - 1; /* behind the footprint */
                    int erase_from = top - extra;
                    if (o->shaped && base - o->levels + 1 < erase_from) erase_from = base - o->levels + 1;
                    for (int ry = erase_from; ry <= erase_to; ry++)
                        for (int i = 0; i < 64; i++) {
                            int px = cx * 8 + (i & 7), py = ry * 8 + (i >> 3);
                            if (!is_key(&keys, pixel_at(px, py, event))) patch_floor(px, py, keys.c[0], event);
                        }
                }
                continue;
            }
            if (o->cliff || o->voidc) continue;
            if (o->thin && !o->green && (!o->is_prop || o->cells >= 4)) {
                add_fence_cell(c, id, mx, my, event);
                continue;
            }
            if (o->foliage && !o->is_prop) {
                /* Each column of a grove stands up as its own strip at the bottom of its run. */
                if (object_at(mx, my + 1) == id || !make_strip(id, mx, my, event, &p)) continue;
            } else {
                /* The first chunk to reach the object's bottom row cuts it out. */
                if (!o->is_prop || o->prop_made || my != o->y1) continue;
                o->prop_made = true;
                /* Ground decorations (crater rims, manholes, flowerbeds) have nothing standing above
                 * them: they stay painted on the floor. */
                if (!owns_art_above(id, o) && !o->green) {
                    o->hidden = true;
                    continue;
                }
                if (!make_prop(o, event, &p)) {
                    /* Nothing but ground colours: an invisible blocker. Draw nothing. */
                    o->hidden = true;
                    continue;
                }
            }
            c->props = realloc(c->props, sizeof(Prop) * (c->nprops + 1));
            c->props[c->nprops++] = p;
        }
    (void)plant_forest; /* forests are now matched tree by tree */
}

typedef struct {
    float *pos, *uv;
    unsigned char *col;
    unsigned short *idx;
    int verts, tris, cap_v, cap_t;
} MeshBuf;

static float mb_yoff; /* raises whatever is being emitted onto its ground height */

static void mb_quad(MeshBuf *m, Vector3 a, Vector3 b, Vector3 c, Vector3 d, UvRect uv, unsigned char shade)
{
    /* uv: a=(u0,v0) b=(u1,v0) c=(u1,v1) d=(u0,v1) */
    if (m->verts + 4 > 65535) return;
    if (m->verts + 4 > m->cap_v) {
        m->cap_v = m->cap_v ? m->cap_v * 2 : 1024;
        m->pos = RL_REALLOC(m->pos, sizeof(float) * 3 * m->cap_v);
        m->uv = RL_REALLOC(m->uv, sizeof(float) * 2 * m->cap_v);
        m->col = RL_REALLOC(m->col, 4 * m->cap_v);
    }
    if (m->tris + 2 > m->cap_t) {
        m->cap_t = m->cap_t ? m->cap_t * 2 : 1024;
        m->idx = RL_REALLOC(m->idx, sizeof(unsigned short) * 3 * m->cap_t);
    }
    Vector3 p[4] = {a, b, c, d};
    float uvs[4][2] = {{uv.u0, uv.v0}, {uv.u1, uv.v0}, {uv.u1, uv.v1}, {uv.u0, uv.v1}};
    for (int i = 0; i < 4; i++) {
        int v = m->verts + i;
        m->pos[v * 3] = p[i].x;
        m->pos[v * 3 + 1] = p[i].y + mb_yoff;
        m->pos[v * 3 + 2] = p[i].z;
        m->uv[v * 2] = uvs[i][0];
        m->uv[v * 2 + 1] = uvs[i][1];
        m->col[v * 4] = m->col[v * 4 + 1] = m->col[v * 4 + 2] = shade;
        m->col[v * 4 + 3] = 255;
    }
    unsigned short base = (unsigned short)m->verts;
    unsigned short tri[6] = {base, (unsigned short)(base + 2), (unsigned short)(base + 1),
                             base, (unsigned short)(base + 3), (unsigned short)(base + 2)};
    memcpy(m->idx + m->tris * 3, tri, sizeof tri);
    m->verts += 4;
    m->tris += 2;
}

static int floor_slot(int tx, int ty, bool event)
{
    if (tx < 0) tx = 0;
    if (ty < 0) ty = 0;
    if (tx >= EBW_MAP_W) tx = EBW_MAP_W - 1;
    if (ty >= EBW_MAP_H) ty = EBW_MAP_H - 1;
    uint32_t *patch = floor_patch[event ? 1 : 0][ty * EBW_MAP_W + tx];
    if (!patch) return atlas_slot(tx, ty, event);
    int32_t *cached = &slot_of_cleaned[(event ? MAP_TILES : 0) + ty * EBW_MAP_W + tx];
    if (*cached < 0) {
        uint32_t px[32 * 32];
        memcpy(px, tile_pixels(tx, ty, event), sizeof px);
        for (int i = 0; i < 32 * 32; i++)
            if (patch[i]) px[i] = patch[i];
        *cached = new_slot(px);
    }
    return *cached;
}

static float cell_scale(int id)
{
    return (id >= 0 && objects[id].indoors) ? INDOOR_SCALE : OUTDOOR_SCALE;
}

/* Height of the solid geometry on a cell, in world units: 0 for open ground and for things
 * drawn as cut-outs. Cliff/forest masses vary cell by cell (tree walls tall, ledges low). */
static float cell_height(int mx, int my, bool facades)
{
    int id = object_at(mx, my);
    if (id < 0) return 0;
    const Object *o = &objects[id];
    if (o->is_prop || o->foliage || o->hidden || o->cliff || o->voidc) return 0;
    if (o->shaped && facades) return 0; /* drawn as a whole model by emit_building */
    int levels = o->levels;
    if (o->terrain || o->forest) levels = tree_cell[my * EBW_MINI_W + mx] ? FOREST_LEVELS : TERRAIN_LEVELS;
    if (!facades) levels = 2;
    return levels * cell_scale(id);
}

/* Bottom row of the object's column through (mx, my): where its front wall stands. */
static int column_base(int mx, int my, int id)
{
    while (object_at(mx, my + 1) == id) my++;
    return my;
}

static int build_serial;

static void tri(MeshBuf *m, Vector3 a, Vector3 b, Vector3 c, UvRect uv, unsigned char shade)
{
    mb_quad(m, a, b, c, c, uv, shade); /* degenerate quad */
}

/* Depth of a shaped building in cells: its roof rows, but at least half its wall height (a tall
 * downtown block draws only a thin strip of roof), and no deeper than its solid footprint. */
static int building_depth(const Object *o)
{
    int d = o->roof_rows, half = o->wall_rows / 2;
    if (half > d) d = half;
    int span = o->y1 - o->y0 + 1;
    if (d > span) d = span > o->roof_rows ? span : o->roof_rows;
    return d < 1 ? 1 : d;
}

/* A shaped building as one model: walls of wall_rows levels on a footprint building_depth deep,
 * topped by a pitched (gable) or flat roof wearing the roof art. */
static void emit_building(MeshBuf *m, int id, bool event)
{
    const Object *o = &objects[id];
    float s = OUTDOOR_SCALE;
    int W = o->wall_rows, R = o->roof_rows, D = building_depth(o);
    float yw = W * s, yr = yw + (o->gable ? D * 0.5f * s : 0);
    for (int mx = o->x0; mx <= o->x1; mx++) {
        int base = column_bottom(id, mx, o->y0, o->y1);
        if (base < 0) continue;
        float x = (float)mx, zf = base + 1.0f, zb = zf - D, zr = (zf + zb) * 0.5f;
        /* Neighbouring columns' footprints, to close the sides where they don't cover ours. */
        int nb_w = column_bottom(id, mx - 1, o->y0, o->y1), nb_e = column_bottom(id, mx + 1, o->y0, o->y1);
        float wf = nb_w < 0 ? -1e9f : nb_w + 1.0f, ef = nb_e < 0 ? -1e9f : nb_e + 1.0f;
        int inner = nb_w < 0 ? mx + 1 : nb_e < 0 ? mx - 1 : mx;
        int inner_base = column_bottom(id, inner, o->y0, o->y1);
        if (inner_base < 0) {
            inner = mx;
            inner_base = base;
        }
        for (int k = 0; k < W; k++) {
            float ya = k * s, yb = (k + 1) * s;
            UvRect front = minitile_uv(mx, base - k, event);
            UvRect siding = minitile_uv(inner, inner_base - k, event);
            mb_quad(m, (Vector3){x, yb, zf}, (Vector3){x + 1, yb, zf}, (Vector3){x + 1, ya, zf}, (Vector3){x, ya, zf},
                    front, 230);
            mb_quad(m, (Vector3){x + 1, yb, zb}, (Vector3){x, yb, zb}, (Vector3){x, ya, zb}, (Vector3){x + 1, ya, zb},
                    siding, 150);
            for (int j = 0; j < D; j++) {
                float za = zb + j, zc = zb + j + 1;
                /* A side slice shows unless the neighbour's footprint covers it. */
                bool w_covered = za >= wf - D - 0.01f && zc <= wf + 0.01f;
                bool e_covered = za >= ef - D - 0.01f && zc <= ef + 0.01f;
                if (!w_covered)
                    mb_quad(m, (Vector3){x, yb, za}, (Vector3){x, yb, zc}, (Vector3){x, ya, zc}, (Vector3){x, ya, za},
                            siding, 190);
                if (!e_covered)
                    mb_quad(m, (Vector3){x + 1, yb, zc}, (Vector3){x + 1, yb, za}, (Vector3){x + 1, ya, za},
                            (Vector3){x + 1, ya, zc}, siding, 190);
            }
        }
        /* Gable ends: triangles of siding under the roof, where no neighbour continues it. */
        if (o->gable) {
            UvRect siding = minitile_uv(inner, inner_base - W + 1, event);
            if (nb_w < 0 || nb_w != base)
                tri(m, (Vector3){x, yw, zb}, (Vector3){x, yw, zf}, (Vector3){x, yr, zr}, siding, 185);
            if (nb_e < 0 || nb_e != base)
                tri(m, (Vector3){x + 1, yw, zf}, (Vector3){x + 1, yw, zb}, (Vector3){x + 1, yr, zr}, siding, 185);
        }
        /* Roof: the art rows above the wall, eave first, stretched over the depth. */
        for (int j = 0; j < R; j++) {
            UvRect uv = minitile_uv(mx, base - W - j, event);
            float t0 = (float)j / R, t1 = (float)(j + 1) / R;
            if (o->gable) {
                /* Front slope climbs from the front eave to the ridge; the back slope mirrors it. */
                float fz0 = zf + (zr - zf) * t0, fz1 = zf + (zr - zf) * t1;
                float bz0 = zb + (zr - zb) * t0, bz1 = zb + (zr - zb) * t1;
                float y0 = yw + (yr - yw) * t0, y1 = yw + (yr - yw) * t1;
                mb_quad(m, (Vector3){x, y1, fz1}, (Vector3){x + 1, y1, fz1}, (Vector3){x + 1, y0, fz0},
                        (Vector3){x, y0, fz0}, uv, 240);
                mb_quad(m, (Vector3){x + 1, y1, bz1}, (Vector3){x, y1, bz1}, (Vector3){x, y0, bz0},
                        (Vector3){x + 1, y0, bz0}, uv, 170);
            } else {
                float za = zf - D * t1, zc = zf - D * t0;
                mb_quad(m, (Vector3){x, yw, za}, (Vector3){x + 1, yw, za}, (Vector3){x + 1, yw, zc},
                        (Vector3){x, yw, zc}, uv, 235);
            }
        }
    }
}

static void build_chunk(int sx, int sy, const W3dOptions *opt)
{
    bool event = opt->value[OPT_PALETTE] == 1;
    bool facades = opt->value[OPT_WALL_STYLE] == 0;
    for (int dy = 0; dy <= 2; dy++) /* art can reach up to two chunks north of its base */
        for (int dx = -1; dx <= 1; dx++) ensure_props(sx + dx, sy + dy, event);

    MeshBuf m = {0};
    mb_yoff = 0;
    /* Ground: one quad per minitile on the solved height field. Cliff cells show the ground
     * above them (their top); their faces drop to the ground below. The off-map sky isn't drawn. */
    for (int my = sy * CHUNK_MH; my < (sy + 1) * CHUNK_MH; my++)
        for (int mx = sx * CHUNK_MW; mx < (sx + 1) * CHUNK_MW; mx++) {
            int kind = kind_at(mx, my);
            if (kind == CELL_VOID) continue;
            float own = ground_at(mx, my);
            float h00 = corner_height(mx, my, kind, own), h10 = corner_height(mx + 1, my, kind, own);
            float h11 = corner_height(mx + 1, my + 1, kind, own), h01 = corner_height(mx, my + 1, kind, own);
            float x = (float)mx, z = (float)my;
            UvRect uv;
            if (kind == CELL_CLIFF) {
                int top = my;
                while (kind_at(mx, top - 1) == CELL_CLIFF) top--;
                /* Plain ground from the terrace above (past the dark lip drawn along the edge). */
                int gy = top > 0 ? top - 1 : 0;
                for (int up = 1; up <= 5; up++) {
                    int yy = top - up;
                    if (kind_at(mx, yy) != CELL_GROUND) break;
                    if (object_at(mx, yy) < 0 && !ebw_foreground_at(mx, yy)) gy = yy;
                    if (up >= 3 && gy != top - 1) break;
                }
                uv = slot_uv(floor_slot(mx >> 2, gy >> 2, event), (mx & 3) * 8, (gy & 3) * 8, 8, 8);
            } else {
                uv = slot_uv(floor_slot(mx >> 2, my >> 2, event), (mx & 3) * 8, (my & 3) * 8, 8, 8);
            }
            mb_quad(&m, (Vector3){x, h00, z}, (Vector3){x + 1, h10, z}, (Vector3){x + 1, h11, z + 1},
                    (Vector3){x, h01, z + 1}, uv, 255);

            if (kind != CELL_CLIFF) continue;
            /* Faces where the neighbouring ground is lower: the cliff's own art, stood up. */
            int bot = my, top = my;
            while (kind_at(mx, bot + 1) == CELL_CLIFF) bot++;
            while (kind_at(mx, top - 1) == CELL_CLIFF) top--;
            int L = bot - top + 1;
            const int nb[4][2] = {{0, 1}, {0, -1}, {1, 0}, {-1, 0}};
            for (int f = 0; f < 4; f++) {
                int nx = mx + nb[f][0], ny = my + nb[f][1];
                if (kind_at(nx, ny) != CELL_GROUND) continue;
                /* Edge endpoints (a then b, seen from outside) and their heights on both sides. */
                int ax, az, bx, bz;
                if (f == 0) ax = mx, az = my + 1, bx = mx + 1, bz = my + 1;
                else if (f == 1) ax = mx + 1, az = my, bx = mx, bz = my;
                else if (f == 2) ax = mx + 1, az = my + 1, bx = mx + 1, bz = my;
                else ax = mx, az = my, bx = mx, bz = my + 1;
                float ha = corner_height(ax, az, CELL_CLIFF, own), hb = corner_height(bx, bz, CELL_CLIFF, own);
                float la = corner_height(ax, az, CELL_GROUND, ground_at(nx, ny));
                float lb = corner_height(bx, bz, CELL_GROUND, ground_at(nx, ny));
                if (ha - la < 0.05f && hb - lb < 0.05f) continue;
                for (int k = 0; k < L; k++) {
                    float t0 = (float)k / L, t1 = (float)(k + 1) / L;
                    UvRect fuv = minitile_uv(mx, rock_row(mx, top, bot, bot - k, event), event);
                    mb_quad(&m, (Vector3){(float)ax, la + (ha - la) * t1, (float)az},
                            (Vector3){(float)bx, lb + (hb - lb) * t1, (float)bz},
                            (Vector3){(float)bx, lb + (hb - lb) * t0, (float)bz},
                            (Vector3){(float)ax, la + (ha - la) * t0, (float)az}, fuv, f == 0 ? 225 : f == 1 ? 150 : 190);
                }
            }
        }

    /* Whole-model buildings, emitted by the chunk holding their bottom-left corner. */
    build_serial++;
    if (facades)
        for (int my = sy * CHUNK_MH; my < (sy + 1) * CHUNK_MH; my++)
            for (int mx = sx * CHUNK_MW; mx < (sx + 1) * CHUNK_MW; mx++) {
                int id = object_at(mx, my);
                if (id < 0) continue;
                Object *o = &objects[id];
                if (!o->shaped || o->emitted_build == build_serial || mx != o->fx || my != o->fy) continue;
                o->emitted_build = build_serial;
                int cx = (o->x0 + o->x1) / 2, cb = column_bottom(id, cx, o->y0, o->y1);
                mb_yoff = ground_at(cx, cb < 0 ? o->y1 : cb);
                emit_building(&m, id, event);
                mb_yoff = 0;
            }

    for (int my = sy * CHUNK_MH; my < (sy + 1) * CHUNK_MH; my++)
        for (int mx = sx * CHUNK_MW; mx < (sx + 1) * CHUNK_MW; mx++) {
            float h = cell_height(mx, my, facades);
            if (h <= 0) continue;
            int id = object_at(mx, my);
            mb_yoff = ground_at(mx, my);
            float s = cell_scale(id);
            float x = (float)mx, z = (float)my;
            const Object *o = &objects[id];
            int levels = (int)(h / s + 0.5f);
            /* Walls wherever a neighbour is lower. Every face wears front art: level k shows the
             * row k above the base of the column, so a facade continues round the building. */
            int base = facades ? column_base(mx, my, id) : my;
            bool building = !o->terrain && !o->forest;
            /* A building's roof wears the top row of its art; ledges and tree walls their own. */
            UvRect roof = (facades && building) ? minitile_uv(mx, base - levels + 1, event) : minitile_uv(mx, my, event);
            mb_quad(&m, (Vector3){x, h, z}, (Vector3){x + 1, h, z}, (Vector3){x + 1, h, z + 1},
                    (Vector3){x, h, z + 1}, roof, 235);

            /* Side walls borrow the siding from two columns in, not the roof's edge. */
            int side_col[2] = {mx, mx};
            for (int i = 0; i < 2; i++) {
                int dir = i ? -1 : 1;
                for (int step = 2; step >= 1; step--)
                    if (object_at(mx + dir * step, my) == id) {
                        side_col[i] = mx + dir * step;
                        break;
                    }
            }
            int side_base[2] = {column_base(side_col[0], my, id), column_base(side_col[1], my, id)};
            float nh_s = cell_height(mx, my + 1, facades), nh_n = cell_height(mx, my - 1, facades);
            float nh_e = cell_height(mx + 1, my, facades), nh_w = cell_height(mx - 1, my, facades);
            /* Only the front shows the whole picture (roof included). Sides and back repeat the
             * lower half, the walls, so roof art doesn't climb up them in stripes. */
            int wall_rows = (building && levels >= 4) ? levels / 2 : levels;
            for (int k = 0; k < levels; k++) {
                int ks = k % wall_rows;
                UvRect uv = minitile_uv(mx, facades ? base - k : my, event);
                UvRect uv_back = minitile_uv(mx, facades ? base - ks : my, event);
                UvRect uv_e = facades ? minitile_uv(side_col[1], side_base[1] - ks, event) : uv;
                UvRect uv_w = facades ? minitile_uv(side_col[0], side_base[0] - ks, event) : uv;
                float ya = k * s, yb = (k + 1) * s;
                if (nh_s < yb) {
                    float lo = ya > nh_s ? ya : nh_s;
                    mb_quad(&m, (Vector3){x, yb, z + 1}, (Vector3){x + 1, yb, z + 1}, (Vector3){x + 1, lo, z + 1},
                            (Vector3){x, lo, z + 1}, uv, 225);
                }
                if (nh_n < yb) {
                    float lo = ya > nh_n ? ya : nh_n;
                    mb_quad(&m, (Vector3){x + 1, yb, z}, (Vector3){x, yb, z}, (Vector3){x, lo, z},
                            (Vector3){x + 1, lo, z}, uv_back, 150);
                }
                if (nh_e < yb) {
                    float lo = ya > nh_e ? ya : nh_e;
                    mb_quad(&m, (Vector3){x + 1, yb, z + 1}, (Vector3){x + 1, yb, z}, (Vector3){x + 1, lo, z},
                            (Vector3){x + 1, lo, z + 1}, uv_e, 190);
                }
                if (nh_w < yb) {
                    float lo = ya > nh_w ? ya : nh_w;
                    mb_quad(&m, (Vector3){x, yb, z}, (Vector3){x, yb, z + 1}, (Vector3){x, lo, z + 1},
                            (Vector3){x, lo, z}, uv_w, 190);
                }
            }
        }

    mb_yoff = 0;
    Chunk *c = &chunks[sy][sx];
    c->built = true;
    c->last_used = frame_no;
    c->has_mesh = m.tris > 0;
    if (c->has_mesh) {
        Mesh mesh = {0};
        mesh.vertexCount = m.verts;
        mesh.triangleCount = m.tris;
        mesh.vertices = m.pos;
        mesh.texcoords = m.uv;
        mesh.colors = m.col;
        mesh.indices = m.idx;
        UploadMesh(&mesh, false);
        c->mesh = mesh;
    }
}

static void free_chunk_mesh(Chunk *c)
{
    if (c->has_mesh) UnloadMesh(c->mesh);
    c->has_mesh = c->built = false;
}

/* Throws everything away: geometry, props, and both atlases. */
void w3d_invalidate(void)
{
    for (int y = 0; y < EBW_SECTORS_Y; y++)
        for (int x = 0; x < EBW_SECTORS_X; x++) {
            Chunk *c = &chunks[y][x];
            free_chunk_mesh(c);
            free(c->props);
            free(c->fences);
            memset(c, 0, sizeof *c);
        }
    for (int e = 0; e < 2; e++)
        if (floor_patch[e])
            for (int t = 0; t < MAP_TILES; t++) {
                free(floor_patch[e][t]);
                floor_patch[e][t] = NULL;
            }
    if (slot_of_key) {
        memset(slot_of_key, 0xFF, sizeof(int32_t) * KEY_COUNT);
        memset(slot_of_cleaned, 0xFF, sizeof(int32_t) * MAP_TILES * 2);
    }
    slots_used = 0;
    shelf_x = shelf_y = shelf_h = 0;
    tree_tpl_count = 0;
    tm_event = -1;
    atlas_full = false;
    if (analysed)
        for (int i = 0; i < object_count; i++) {
            objects[i].prop_made = false;
            objects[i].hidden = false;
            objects[i].fence_made = false;
            objects[i].is_prop = looks_like_prop(&objects[i]);
        }
}

/* ---- shader: fog, colour mood, and cut-out transparency ---- */

static const char *vs_src =
    "#version 330\n"
    "in vec3 vertexPosition; in vec2 vertexTexCoord; in vec4 vertexColor;\n"
    "uniform mat4 mvp; uniform mat4 matModel;\n"
    "out vec2 uv; out vec4 col; out vec3 wpos;\n"
    "void main() {\n"
    "  uv = vertexTexCoord; col = vertexColor;\n"
    "  wpos = (matModel * vec4(vertexPosition, 1.0)).xyz;\n"
    "  gl_Position = mvp * vec4(vertexPosition, 1.0);\n"
    "}\n";

static const char *fs_src =
    "#version 330\n"
    "in vec2 uv; in vec4 col; in vec3 wpos;\n"
    "uniform sampler2D texture0;\n"
    "uniform vec3 camPos; uniform vec3 fogColor; uniform float fogStart; uniform float fogEnd; uniform vec3 tint;\n"
    "out vec4 finalColor;\n"
    "void main() {\n"
    "  vec4 t = texture(texture0, uv);\n"
    "  if (t.a < 0.5) discard;\n"
    "  vec3 c = t.rgb * col.rgb * tint;\n"
    "  float f = clamp((length(wpos - camPos) - fogStart) / (fogEnd - fogStart), 0.0, 1.0);\n"
    "  finalColor = vec4(mix(c, fogColor, f), 1.0);\n"
    "}\n";

static Shader shader;
static Material material;
static int loc_cam, loc_fog_color, loc_fog_start, loc_fog_end, loc_tint, loc_model;

typedef struct {
    Vector3 fog, tint;
} Mood;

static Mood mood_for(int i)
{
    static const Mood moods[] = {
        {{0.55f, 0.75f, 0.95f}, {1.00f, 1.00f, 1.00f}}, /* original */
        {{0.03f, 0.03f, 0.10f}, {0.45f, 0.52f, 0.95f}}, /* night */
        {{0.85f, 0.45f, 0.40f}, {1.05f, 0.80f, 0.70f}}, /* dusk */
        {{0.30f, 0.45f, 0.30f}, {0.80f, 1.00f, 0.80f}}, /* eerie */
    };
    return moods[i];
}

static float fog_end_for(int i)
{
    static const float ends[] = {40, 80, 140, 400};
    return ends[i];
}

void w3d_init(void)
{
    Image blank = GenImageColor(ATLAS_SIZE, ATLAS_SIZE, MAGENTA);
    atlas = LoadTextureFromImage(blank);
    UnloadImage(blank);
    SetTextureFilter(atlas, TEXTURE_FILTER_POINT);
    blank = GenImageColor(PROP_ATLAS, PROP_ATLAS, BLANK);
    prop_atlas = LoadTextureFromImage(blank);
    UnloadImage(blank);
    SetTextureFilter(prop_atlas, TEXTURE_FILTER_POINT);

    slot_of_key = malloc(sizeof(int32_t) * KEY_COUNT);
    slot_of_cleaned = malloc(sizeof(int32_t) * MAP_TILES * 2);
    for (int e = 0; e < 2; e++) floor_patch[e] = calloc(MAP_TILES, sizeof(uint32_t *));
    tile_cache = malloc(sizeof(*tile_cache) * TILE_CACHE);
    for (int i = 0; i < TILE_CACHE; i++) tile_cache[i].key = -1;
    w3d_invalidate();

    shader = LoadShaderFromMemory(vs_src, fs_src);
    loc_model = GetShaderLocation(shader, "matModel");
    shader.locs[SHADER_LOC_MATRIX_MODEL] = loc_model;
    loc_cam = GetShaderLocation(shader, "camPos");
    loc_fog_color = GetShaderLocation(shader, "fogColor");
    loc_fog_start = GetShaderLocation(shader, "fogStart");
    loc_fog_end = GetShaderLocation(shader, "fogEnd");
    loc_tint = GetShaderLocation(shader, "tint");

    material = LoadMaterialDefault();
    material.shader = shader;
    material.maps[MATERIAL_MAP_DIFFUSE].texture = atlas;
}

void w3d_shutdown(void)
{
    w3d_invalidate();
    material.maps[MATERIAL_MAP_DIFFUSE].texture = (Texture2D){0}; /* don't double-free the atlas */
    material.shader = (Shader){0};
    UnloadMaterial(material);
    UnloadShader(shader);
    UnloadTexture(atlas);
    UnloadTexture(prop_atlas);
    free(slot_of_key);
    free(slot_of_cleaned);
    for (int e = 0; e < 2; e++) free(floor_patch[e]);
    free(tile_cache);
    free(solid_grid);
    free(ground);
    free(fg_owner);
    fg_owner = NULL;
    free(cell_kind);
    ground = NULL;
    cell_kind = NULL;
    free(tree_cell);
    free(object_of);
    free(objects);
    slot_of_key = slot_of_cleaned = NULL;
    floor_patch[0] = floor_patch[1] = NULL;
    analysed = false;
}

void w3d_class_map(const char *path, int tx, int ty, int tw, int th)
{
    if (!analysed) analyse_world();
    int w = tw * 32, h = th * 32;
    Image img = GenImageColor(w, h, BLACK);
    uint32_t *out = img.data;
    for (int y = 0; y < h; y++)
        for (int x = 0; x < w; x++) {
            int px = tx * 32 + x, py = ty * 32 + y;
            uint32_t c = pixel_at(px, py, false);
            int id = object_at(px / 8, py / 8);
            uint32_t tint = 0;
            if (id >= 0) {
                Object *o = &objects[id];
                bool tree = (o->terrain || o->forest) && tree_cell[(py / 8) * EBW_MINI_W + px / 8];
                int kind = kind_at(px / 8, py / 8);
                tint = kind == CELL_VOID ? 0xFF000000 : kind == CELL_CLIFF ? 0xFF0080FF : tree ? 0xFF008000
                     : o->is_prop ? 0xFF00FFFF : o->foliage ? 0xFFFFFF00 : o->terrain || o->forest ? 0xFFFF0000
                     : o->blueprint ? 0xFFFF00FF : 0xFF0000FF;
            }
            if (tint) { /* blend 50% */
                uint32_t r = ((c & 0xFF) + (tint & 0xFF)) / 2, g = (((c >> 8) & 0xFF) + ((tint >> 8) & 0xFF)) / 2,
                         b = (((c >> 16) & 0xFF) + ((tint >> 16) & 0xFF)) / 2;
                c = r | g << 8 | b << 16 | 0xFF000000u;
            }
            out[y * w + x] = c;
        }
    ExportImage(img, path);
    UnloadImage(img);
}

void w3d_dump_ground(const char *path, int tx, int ty, int tw, int th)
{
    if (!analysed) analyse_world();
    FILE *f = fopen(path, "w");
    if (!f) return;
    for (int my = ty * 4; my < (ty + th) * 4; my++) {
        for (int mx = tx * 4; mx < (tx + tw) * 4; mx++)
            fprintf(f, "%s%.2f:%d", mx == tx * 4 ? "" : " ", ground_at(mx, my), kind_at(mx, my));
        fprintf(f, "\n");
    }
    fclose(f);
}

void w3d_dump_objects(const char *path)
{
    if (!analysed) analyse_world();
    FILE *f = fopen(path, "w");
    if (!f) return;
    fprintf(f, "id,x0,y0,x1,y1,cells,levels,prop,foliage,forest,terrain,indoors,blueprint,shaped,gable,wall,roof,thin\n");
    for (int i = 0; i < object_count; i++) {
        Object *o = &objects[i];
        fprintf(f, "%d,%d,%d,%d,%d,%d,%d,%d,%d,%d,%d,%d,%d,%d,%d,%d,%d,%d\n", i, o->x0, o->y0, o->x1, o->y1, o->cells,
                o->levels, o->is_prop, o->foliage, o->forest, o->terrain, o->indoors, o->blueprint, o->shaped, o->gable,
                o->wall_rows, o->roof_rows, o->thin);
    }
    fclose(f);
}

void w3d_probe(int px, int py)
{
    if (!analysed) analyse_world();
    TraceLog(LOG_WARNING, "probe cell %d,%d: obj %d fg %d owner %d kind %d tree %d ground %.2f", px / 8, py / 8,
             object_at(px / 8, py / 8), ebw_foreground_at(px / 8, py / 8), owner_at(px / 8, py / 8),
             kind_at(px / 8, py / 8), tree_cell[(py / 8) * EBW_MINI_W + px / 8], ground_at(px / 8, py / 8));
    int id = object_at(px / 8, py / 8);
    if (id < 0) {
        TraceLog(LOG_WARNING, "probe %d,%d: open ground", px, py);
        return;
    }
    Object *o = &objects[id];
    TraceLog(LOG_WARNING, "probe %d,%d: object %d box %d,%d-%d,%d cells %d levels %d prop %d foliage %d terrain %d indoors %d",
             px, py, id, o->x0, o->y0, o->x1, o->y1, o->cells, o->levels, o->is_prop, o->foliage, o->terrain, o->indoors);
}

bool w3d_pending(void) { return chunks_pending; }

float w3d_ground(float x, float z)
{
    if (!analysed) analyse_world();
    return ground_at((int)floorf(x), (int)floorf(z));
}

bool w3d_is_solid(float x, float z) { return analysed && solid((int)floorf(x), (int)floorf(z)); }

static void draw_fences(const Chunk *c)
{
    for (int i = 0; i < c->nfences; i++) {
        const FenceSeg *f = &c->fences[i];
        float len = sqrtf((f->x1 - f->x0) * (f->x1 - f->x0) + (f->z1 - f->z0) * (f->z1 - f->z0));
        int pieces = (int)(len + 0.5f);
        if (pieces < 1) pieces = 1;
        float u0 = f->src.x / PROP_ATLAS, v0 = f->src.y / PROP_ATLAS;
        float u1 = (f->src.x + f->src.width) / PROP_ATLAS, v1 = (f->src.y + f->src.height) / PROP_ATLAS;
        for (int k = 0; k < pieces; k++) {
            float t0 = (float)k / pieces, t1 = (float)(k + 1) / pieces;
            float ax = f->x0 + (f->x1 - f->x0) * t0, az = f->z0 + (f->z1 - f->z0) * t0, ay = f->y0 + (f->y1 - f->y0) * t0;
            float bx = f->x0 + (f->x1 - f->x0) * t1, bz = f->z0 + (f->z1 - f->z0) * t1, by = f->y0 + (f->y1 - f->y0) * t1;
            rlTexCoord2f(u0, v1); rlVertex3f(ax, ay, az);
            rlTexCoord2f(u1, v1); rlVertex3f(bx, by, bz);
            rlTexCoord2f(u1, v0); rlVertex3f(bx, by + f->h, bz);
            rlTexCoord2f(u0, v0); rlVertex3f(ax, ay + f->h, az);
        }
    }
}

static void draw_props(const Chunk *c, Vector3 right)
{
    for (int i = 0; i < c->nprops; i++) {
        const Prop *p = &c->props[i];
        float u0 = p->src.x / PROP_ATLAS, v0 = p->src.y / PROP_ATLAS;
        float u1 = (p->src.x + p->src.width) / PROP_ATLAS, v1 = (p->src.y + p->src.height) / PROP_ATLAS;
        float hw = p->w * 0.5f * OUTDOOR_SCALE, h = p->h * OUTDOOR_SCALE;
        Vector3 l = {p->x - right.x * hw, 0, p->z - right.z * hw}, r = {p->x + right.x * hw, 0, p->z + right.z * hw};
        float y0 = p->y, y1 = p->y + h;
        rlTexCoord2f(u0, v1); rlVertex3f(l.x, y0, l.z);
        rlTexCoord2f(u1, v1); rlVertex3f(r.x, y0, r.z);
        rlTexCoord2f(u1, v0); rlVertex3f(r.x, y1, r.z);
        rlTexCoord2f(u0, v0); rlVertex3f(l.x, y1, l.z);
    }
}

void w3d_draw(Camera3D cam, const W3dOptions *opt)
{
    if (!analysed) analyse_world();
    frame_no++;
    if (atlas_full) w3d_invalidate();

    Mood mood = mood_for(opt->value[OPT_MOOD]);
    float fog_end = fog_end_for(opt->value[OPT_FOG]);
    float fog_start = fog_end * 0.35f;
    SetShaderValue(shader, loc_cam, &cam.position, SHADER_UNIFORM_VEC3);
    SetShaderValue(shader, loc_fog_color, &mood.fog, SHADER_UNIFORM_VEC3);
    SetShaderValue(shader, loc_fog_start, &fog_start, SHADER_UNIFORM_FLOAT);
    SetShaderValue(shader, loc_fog_end, &fog_end, SHADER_UNIFORM_FLOAT);
    SetShaderValue(shader, loc_tint, &mood.tint, SHADER_UNIFORM_VEC3);

    ClearBackground((Color){(unsigned char)(mood.fog.x * 255), (unsigned char)(mood.fog.y * 255),
                            (unsigned char)(mood.fog.z * 255), 255});
    BeginMode3D(cam);
    rlDisableBackfaceCulling(); /* walls and cut-outs are seen from both sides */

    float reach = fog_end + 16;
    Vector3 fwd = Vector3Normalize(Vector3Subtract(cam.target, cam.position));
    Vector3 right = Vector3Normalize((Vector3){-fwd.z, 0, fwd.x});
    int csx = (int)floorf(cam.position.x / CHUNK_MW), csy = (int)floorf(cam.position.z / CHUNK_MH);
    int rx = (int)(reach / CHUNK_MW) + 1, ry = (int)(reach / CHUNK_MH) + 1;
    int builds_left = 6; /* spread chunk building over frames */
    chunks_pending = false;
    static Chunk *visible[EBW_SECTORS_X * EBW_SECTORS_Y];
    int nvisible = 0;

    for (int pass = 0; pass < 2; pass++) /* pass 0 builds nearest chunks first */
        for (int ring = 0; ring <= (rx > ry ? rx : ry); ring++)
            for (int sy = csy - ring; sy <= csy + ring; sy++)
                for (int sx = csx - ring; sx <= csx + ring; sx++) {
                    if (abs(sx - csx) != ring && abs(sy - csy) != ring) continue;
                    if (sx < 0 || sy < 0 || sx >= EBW_SECTORS_X || sy >= EBW_SECTORS_Y) continue;
                    if (abs(sx - csx) > rx || abs(sy - csy) > ry) continue;
                    Vector3 centre = {(sx + 0.5f) * CHUNK_MW, 0, (sy + 0.5f) * CHUNK_MH};
                    Vector3 to = Vector3Subtract(centre, cam.position);
                    to.y = 0;
                    float dist = Vector3Length(to);
                    if (dist > reach + CHUNK_MW) continue;
                    if (dist > CHUNK_MW && Vector3DotProduct(to, fwd) < -0.3f * dist) continue;
                    Chunk *c = &chunks[sy][sx];
                    if (pass == 0) {
                        if (!c->built && builds_left == 0) chunks_pending = true;
                        if (!c->built && builds_left > 0) {
                            build_chunk(sx, sy, opt);
                            builds_left--;
                        }
                        continue;
                    }
                    if (c->has_mesh) DrawMesh(c->mesh, material, MatrixIdentity());
                    c->last_used = frame_no;
                    if (c->built) visible[nvisible++] = c;
                }

    /* Cut-outs turn to face the camera, like sprites in Doom. */
    rlDrawRenderBatchActive();
    BeginShaderMode(shader);
    Matrix identity = MatrixIdentity();
    SetShaderValueMatrix(shader, loc_model, identity);
    rlSetTexture(prop_atlas.id);
    rlBegin(RL_QUADS);
    rlColor4ub(255, 255, 255, 255);
    for (int i = 0; i < nvisible; i++) {
        draw_props(visible[i], right);
        draw_fences(visible[i]);
    }
    rlEnd();
    rlSetTexture(0);
    EndShaderMode();

    rlEnableBackfaceCulling();
    EndMode3D();

    /* Drop meshes not drawn for a while so GPU memory stays bounded; props stay cut out. */
    for (int y = 0; y < EBW_SECTORS_Y; y++)
        for (int x = 0; x < EBW_SECTORS_X; x++)
            if (chunks[y][x].built && frame_no - chunks[y][x].last_used > 600) free_chunk_mesh(&chunks[y][x]);
}
