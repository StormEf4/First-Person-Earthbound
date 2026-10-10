#include "world3d.h"

#include <math.h>
#include <stdlib.h>
#include <string.h>

#include "eb_world.h"
#include "raymath.h"
#include "rlgl.h"

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
#define OUTDOOR_MAX_LEVELS 12
#define INDOOR_MAX_LEVELS 4
#define TERRAIN_LEVELS 2 /* cliffs and ledges: a step up in the ground, not a skyscraper */

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

/* ---- objects: connected solid areas of the collision map ---- */

typedef struct {
    int x0, y0, x1, y1, cells;
    int depth_sum, runs;
    bool is_prop, prop_made, indoors;
    bool foliage; /* trees and hedges: drawn as cut-out strips, never as boxes */
    bool terrain; /* cliffs and ledges: long, sprawling, and kept low */
    int levels;
} Object;

static bool looks_like_prop(const Object *o);

static uint8_t *solid_grid;
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

    for (int start = 0; start < n; start++) {
        if (!solid_grid[start] || object_of[start] >= 0) continue;
        if (object_count == cap) {
            cap *= 2;
            objects = realloc(objects, sizeof(Object) * cap);
        }
        Object *o = &objects[object_count];
        memset(o, 0, sizeof *o);
        o->x0 = o->y0 = 1 << 30;
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
            const int nb[4][2] = {{1, 0}, {-1, 0}, {0, 1}, {0, -1}};
            for (int k = 0; k < 4; k++) {
                int nx = x + nb[k][0], ny = y + nb[k][1];
                if (nx < 0 || ny < 0 || nx >= EBW_MINI_W || ny >= EBW_MINI_H) continue;
                int ni = ny * EBW_MINI_W + nx;
                if (solid_grid[ni] && object_of[ni] < 0) {
                    object_of[ni] = object_count;
                    stack[sp++] = ni;
                }
            }
        }
        object_count++;
    }

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
            if (id < 0 || objects[id].indoors || objects[id].is_prop) continue;
            for (int k = 0; k < 4; k++) {
                uint32_t c = pixel_at(mx * 8 + 2 + (k & 1) * 4, my * 8 + 2 + (k >> 1) * 4, false);
                int r = c & 0xFF, g = (c >> 8) & 0xFF, b = (c >> 16) & 0xFF;
                total[id]++;
                if (g > r + 16 && g >= b) green[id]++;
            }
        }
    for (int i = 0; i < object_count; i++) {
        Object *o = &objects[i];
        int w = o->x1 - o->x0 + 1, h = o->y1 - o->y0 + 1;
        /* Thin outdoor things (fences, railings) are cut out column by column like foliage, so
         * the gaps between pickets show through. */
        bool thin = !o->indoors && !o->is_prop && (w <= 2 || h <= 2);
        o->foliage = thin || (total[i] && green[i] * 2 >= total[i]);
        /* Buildings are compact blocks; cliffs sprawl along diagonals and leave their box mostly empty. */
        float fill = (float)o->cells / (float)(w * h);
        o->terrain = !o->indoors && !o->is_prop && !o->foliage && (w > 48 || h > 48 || (w * h > 200 && fill < 0.5f));
        if (o->terrain) o->levels = TERRAIN_LEVELS;
    }
    free(green);
    free(total);
    free(stack);
    analysed = true;
}

/* Small things (trees, bushes, signs, posts) become cut-out sprites; the rest are solid. */
static bool looks_like_prop(const Object *o)
{
    int w = o->x1 - o->x0 + 1, h = o->y1 - o->y0 + 1;
    return w <= 6 && h <= 6 && o->cells <= 30 && !o->indoors;
}

/* ---- props: small objects as camera-facing cut-outs ---- */

#define PROP_ATLAS 2048

typedef struct {
    float x, z, w, h;
    Rectangle src; /* in the prop atlas */
} Prop;

static Texture2D prop_atlas;
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
    int t = (py >> 5) * EBW_MAP_W + (px >> 5);
    uint32_t **patches = floor_patch[event ? 1 : 0];
    if (!patches[t]) patches[t] = calloc(32 * 32, sizeof(uint32_t));
    patches[t][(py & 31) * 32 + (px & 31)] = colour;
}

/* The ground colours around a spot: the two most common colours in the open cells just below
 * and beside it. Those pixels become transparent in cut-outs. */
static bool ground_keys(int x0, int x1, int ybase, bool event, uint32_t *key1, uint32_t *key2)
{
    uint32_t colours[16];
    int counts[16], ncol = 0;
    for (int my = ybase - 1; my <= ybase + 2; my++)
        for (int mx = x0 - 1; mx <= x1 + 1; mx++) {
            if (solid(mx, my)) continue;
            for (int i = 0; i < 64; i++) {
                uint32_t c = pixel_at(mx * 8 + (i & 7), my * 8 + (i >> 3), event);
                int k = 0;
                while (k < ncol && colours[k] != c) k++;
                if (k == ncol) {
                    if (ncol == 16) continue;
                    colours[ncol] = c;
                    counts[ncol++] = 0;
                }
                counts[k]++;
            }
        }
    if (ncol == 0) return false;
    int best = 0, second = -1;
    for (int k = 1; k < ncol; k++)
        if (counts[k] > counts[best]) best = k;
    for (int k = 0; k < ncol; k++)
        if (k != best && (second < 0 || counts[k] > counts[second])) second = k;
    *key1 = colours[best];
    *key2 = (second >= 0 && counts[second] * 4 >= counts[best]) ? colours[second] : *key1;
    return true;
}

/* Cuts an object's art out of a pixel window of the map: its own pixels plus everything drawn
 * connected to them (a tree's canopy), minus the ground colours. The cut-out pixels are erased
 * from the floor. The sprite stands at depth z. False if nothing could be cut. */
static bool cut_out(int id, int wx0, int wy0, int ww, int wh, uint32_t key1, uint32_t key2, bool event, float z,
                    Prop *out)
{
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
            bool allowed = (obj == id || obj == -1) && c != key1 && c != key2;
            mask[y * ww + x] = allowed ? 1 : 0;
            if (allowed && obj == id) {
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
            if (in) patch_floor(wx0 + bx0 + x, wy0 + by0 + y, key1, event);
        }
    UpdateTextureRec(prop_atlas, r, img);

    out->src = r;
    out->w = bw / 8.0f;
    out->h = bh / 8.0f;
    out->x = (wx0 + bx0 + bw * 0.5f) / 8.0f;
    out->z = z;
    return true;
}

static bool make_prop(Object *o, bool event, Prop *out)
{
    uint32_t k1, k2;
    if (!ground_keys(o->x0, o->x1, o->y1, event, &k1, &k2)) return false;
    int id = (int)(o - objects);
    return cut_out(id, (o->x0 - 2) * 8, (o->y0 - 8) * 8, (o->x1 - o->x0 + 5) * 8, (o->y1 - o->y0 + 9) * 8, k1, k2,
                   event, (o->y0 + o->y1 + 1) * 0.5f, out);
}

/* One column of a tree cluster: the run of solid cells ending at `base`, plus canopy above. */
static bool make_strip(int id, int mx, int base, bool event, Prop *out)
{
    uint32_t k1, k2;
    if (!ground_keys(mx, mx, base, event, &k1, &k2)) return false;
    int top = base;
    while (top > base - 9 && object_at(mx, top - 1) == id) top--;
    top -= 6; /* canopy drawn above the solid part */
    return cut_out(id, mx * 8, top * 8, 8, (base - top + 1) * 8, k1, k2, event, base + 0.5f, out);
}

/* ---- per-sector chunks ---- */

#define CHUNK_MW (EBW_SECTOR_W * 4) /* minitiles */
#define CHUNK_MH (EBW_SECTOR_H * 4)

typedef struct {
    bool built, has_mesh, props_done;
    Mesh mesh;
    Prop *props;
    int nprops;
    int last_used;
} Chunk;

static Chunk chunks[EBW_SECTORS_Y][EBW_SECTORS_X];
static int frame_no;

/* Cuts out the props whose base lies in this chunk. Their canopies can reach into the chunk to
 * the north, so a chunk's floor is built only after its southern neighbours have run this. */
static void ensure_props(int sx, int sy, bool event)
{
    if (sx < 0 || sy < 0 || sx >= EBW_SECTORS_X || sy >= EBW_SECTORS_Y) return;
    Chunk *c = &chunks[sy][sx];
    if (c->props_done) return;
    c->props_done = true;
    for (int my = sy * CHUNK_MH; my < (sy + 1) * CHUNK_MH; my++)
        for (int mx = sx * CHUNK_MW; mx < (sx + 1) * CHUNK_MW; mx++) {
            int id = object_at(mx, my);
            if (id < 0) continue;
            Object *o = &objects[id];
            Prop p;
            if (o->foliage && !o->is_prop) {
                /* Each column of a grove stands up as its own strip at the bottom of its run. */
                if (object_at(mx, my + 1) == id || !make_strip(id, mx, my, event, &p)) continue;
            } else {
                /* The first chunk to reach the object's bottom row cuts it out. */
                if (!o->is_prop || o->prop_made || my != o->y1) continue;
                o->prop_made = true;
                if (!make_prop(o, event, &p)) {
                    o->is_prop = false; /* fall back to a solid block */
                    continue;
                }
            }
            c->props = realloc(c->props, sizeof(Prop) * (c->nprops + 1));
            c->props[c->nprops++] = p;
        }
}

typedef struct {
    float *pos, *uv;
    unsigned char *col;
    unsigned short *idx;
    int verts, tris, cap_v, cap_t;
} MeshBuf;

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
        m->pos[v * 3 + 1] = p[i].y;
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

/* Bottom row of the object's column through (mx, my): where its front wall stands. */
static int column_base(int mx, int my, int id)
{
    while (object_at(mx, my + 1) == id) my++;
    return my;
}

static void build_chunk(int sx, int sy, const W3dOptions *opt)
{
    bool event = opt->value[OPT_PALETTE] == 1;
    bool facades = opt->value[OPT_WALL_STYLE] == 0;
    for (int dy = 0; dy <= 1; dy++)
        for (int dx = -1; dx <= 1; dx++) ensure_props(sx + dx, sy + dy, event);

    MeshBuf m = {0};
    for (int ty = sy * EBW_SECTOR_H; ty < (sy + 1) * EBW_SECTOR_H; ty++)
        for (int tx = sx * EBW_SECTOR_W; tx < (sx + 1) * EBW_SECTOR_W; tx++) {
            float x0 = tx * 4.0f, z0 = ty * 4.0f;
            mb_quad(&m, (Vector3){x0, 0, z0}, (Vector3){x0 + 4, 0, z0}, (Vector3){x0 + 4, 0, z0 + 4},
                    (Vector3){x0, 0, z0 + 4}, slot_uv(floor_slot(tx, ty, event), 0, 0, 32, 32), 255);
        }

    for (int my = sy * CHUNK_MH; my < (sy + 1) * CHUNK_MH; my++)
        for (int mx = sx * CHUNK_MW; mx < (sx + 1) * CHUNK_MW; mx++) {
            int id = object_at(mx, my);
            if (id < 0 || objects[id].is_prop || objects[id].foliage) continue;
            Object *o = &objects[id];
            float x = (float)mx, z = (float)my;
            float s = o->indoors ? INDOOR_SCALE : OUTDOOR_SCALE;
            int levels = facades ? o->levels : 2;
            float h = levels * s;
            bool open_s = object_at(mx, my + 1) != id, open_n = object_at(mx, my - 1) != id;
            bool open_e = object_at(mx + 1, my) != id, open_w = object_at(mx - 1, my) != id;

            mb_quad(&m, (Vector3){x, h, z}, (Vector3){x + 1, h, z}, (Vector3){x + 1, h, z + 1},
                    (Vector3){x, h, z + 1}, minitile_uv(mx, my, event), 235);
            if (!(open_s || open_n || open_e || open_w)) continue;

            /* Every face of the object wears its front art: level k shows the row k above the
             * base of this column, so a building's walls continue its facade all the way round. */
            int base = facades ? column_base(mx, my, id) : my;
            /* Side walls borrow the siding from two columns in, not the roof's edge. */
            int side_col[2] = {mx, mx};
            for (int i = 0; i < 2; i++) {
                int dir = i ? -1 : 1; /* east face looks inward to the west, and vice versa */
                for (int step = 2; step >= 1; step--)
                    if (object_at(mx + dir * step, my) == id) {
                        side_col[i] = mx + dir * step;
                        break;
                    }
            }
            int side_base[2] = {column_base(side_col[0], my, id), column_base(side_col[1], my, id)};
            for (int k = 0; k < levels; k++) {
                UvRect uv = minitile_uv(mx, facades ? base - k : my, event);
                UvRect uv_e = facades ? minitile_uv(side_col[1], side_base[1] - k, event) : uv;
                UvRect uv_w = facades ? minitile_uv(side_col[0], side_base[0] - k, event) : uv;
                float ya = k * s, yb = (k + 1) * s;
                if (open_s)
                    mb_quad(&m, (Vector3){x, yb, z + 1}, (Vector3){x + 1, yb, z + 1}, (Vector3){x + 1, ya, z + 1},
                            (Vector3){x, ya, z + 1}, uv, 225);
                if (open_n)
                    mb_quad(&m, (Vector3){x + 1, yb, z}, (Vector3){x, yb, z}, (Vector3){x, ya, z},
                            (Vector3){x + 1, ya, z}, uv, 150);
                if (open_e)
                    mb_quad(&m, (Vector3){x + 1, yb, z + 1}, (Vector3){x + 1, yb, z}, (Vector3){x + 1, ya, z},
                            (Vector3){x + 1, ya, z + 1}, uv_e, 190);
                if (open_w)
                    mb_quad(&m, (Vector3){x, yb, z}, (Vector3){x, yb, z + 1}, (Vector3){x, ya, z + 1},
                            (Vector3){x, ya, z}, uv_w, 190);
            }
        }

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
    atlas_full = false;
    if (analysed)
        for (int i = 0; i < object_count; i++) {
            objects[i].prop_made = false;
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
                tint = o->is_prop ? 0xFF00FFFF : o->foliage ? 0xFFFFFF00 : o->terrain ? 0xFFFF0000 : 0xFF0000FF;
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

void w3d_probe(int px, int py)
{
    if (!analysed) analyse_world();
    int id = object_at(px / 8, py / 8);
    if (id < 0) {
        TraceLog(LOG_WARNING, "probe %d,%d: open ground", px, py);
        return;
    }
    Object *o = &objects[id];
    TraceLog(LOG_WARNING, "probe %d,%d: object %d box %d,%d-%d,%d cells %d levels %d prop %d foliage %d terrain %d indoors %d",
             px, py, id, o->x0, o->y0, o->x1, o->y1, o->cells, o->levels, o->is_prop, o->foliage, o->terrain, o->indoors);
}

bool w3d_is_solid(float x, float z) { return analysed && solid((int)floorf(x), (int)floorf(z)); }

static void draw_props(const Chunk *c, Vector3 right)
{
    for (int i = 0; i < c->nprops; i++) {
        const Prop *p = &c->props[i];
        float u0 = p->src.x / PROP_ATLAS, v0 = p->src.y / PROP_ATLAS;
        float u1 = (p->src.x + p->src.width) / PROP_ATLAS, v1 = (p->src.y + p->src.height) / PROP_ATLAS;
        float hw = p->w * 0.5f * OUTDOOR_SCALE, h = p->h * OUTDOOR_SCALE;
        Vector3 l = {p->x - right.x * hw, 0, p->z - right.z * hw}, r = {p->x + right.x * hw, 0, p->z + right.z * hw};
        rlTexCoord2f(u0, v1); rlVertex3f(l.x, 0, l.z);
        rlTexCoord2f(u1, v1); rlVertex3f(r.x, 0, r.z);
        rlTexCoord2f(u1, v0); rlVertex3f(r.x, h, r.z);
        rlTexCoord2f(u0, v0); rlVertex3f(l.x, h, l.z);
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
    for (int i = 0; i < nvisible; i++) draw_props(visible[i], right);
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
