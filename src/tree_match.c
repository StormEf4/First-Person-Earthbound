#include "tree_match.h"

#include <stdlib.h>
#include <string.h>

/* Clean examples of each drawing, in map pixels (Onett, around Ness's house): a rectangle that
 * holds one tree standing alone on grass. */
static const struct {
    int x, y, w, h;
} sources[] = {
    {2004, 88, 56, 48}, /* small round tree */
    {2392, 56, 72, 72}, /* big oak */
};
#define NSRC (int)(sizeof sources / sizeof sources[0])

static TreeSprite sprites[NSRC];
static int nsprites;
static TmPixelFn pixel_fn;
static int (*tileset_fn)(int, int);

static uint32_t rgb(uint32_t c) { return c & 0xFFFFFFu; }

/* Grass blades: green-dominant and not an outline. */
static bool greenish(uint32_t c)
{
    int r = c & 0xFF, g = (c >> 8) & 0xFF, b = (c >> 16) & 0xFF;
    return g >= 70 && g > r + 20 && g > b;
}

/* Ground shadow: a muted cool colour, never an outline or bark. */
static bool shadowish(uint32_t c)
{
    int r = c & 0xFF, g = (c >> 8) & 0xFF, b = (c >> 16) & 0xFF;
    return g >= 70 && g >= r && b >= r;
}

/* Background is grass (its two colours) and the gray-green ground shadow; the tree is whatever the
 * flood from the edges can't reach (the dark outline closes the canopy). */
static bool cut(int sx, int sy, int w, int h, bool event, TreeSprite *out)
{
    static uint32_t win[TM_MAX_W * TM_MAX_H];
    static uint8_t bg[TM_MAX_W * TM_MAX_H];
    static int stk[TM_MAX_W * TM_MAX_H * 4];
    static int comp[TM_MAX_W * TM_MAX_H];
    if (w > TM_MAX_W || h > TM_MAX_H) return false;
    for (int y = 0; y < h; y++)
        for (int x = 0; x < w; x++) win[y * w + x] = rgb(pixel_fn(sx + x, sy + y, event));

    /* Grass = the most common colour in the window plus the most common colour that isn't the
     * outline and occurs on the window's border (blades); shadow = the colour most common on the
     * border ring just below the middle that isn't grass. Robust to day/night palettes. */
    uint32_t cols[64] = {0};
    int cnt[64] = {0}, nc = 0;
    for (int i = 0; i < w * h; i++) {
        int k = 0;
        while (k < nc && cols[k] != win[i]) k++;
        if (k == nc) {
            if (nc == 64) continue;
            cols[nc] = win[i];
            cnt[nc++] = 0;
        }
        cnt[k]++;
    }
    int g0 = 0;
    for (int k = 1; k < nc; k++)
        if (cnt[k] > cnt[g0]) g0 = k;
    uint32_t grass = cols[g0];
    /* Border colours other than grass: blades. */
    int bcnt[64] = {0};
    for (int x = 0; x < w; x++)
        for (int k = 0; k < nc; k++) {
            if (cols[k] == win[x]) bcnt[k]++;
            if (cols[k] == win[(h - 1) * w + x]) bcnt[k]++;
        }
    int g1 = -1;
    for (int k = 0; k < nc; k++)
        if (k != g0 && bcnt[k] && greenish(cols[k]) && (g1 < 0 || bcnt[k] > bcnt[g1])) g1 = k;
    uint32_t blade = g1 >= 0 ? cols[g1] : grass;
    /* Shadow: the non-grass colour most common in the bottom fifth of the window. */
    int scnt[64] = {0};
    for (int y = h * 4 / 5; y < h; y++)
        for (int x = 0; x < w; x++)
            for (int k = 0; k < nc; k++)
                if (cols[k] == win[y * w + x] && cols[k] != grass && cols[k] != blade && shadowish(cols[k])) scnt[k]++;
    int s = -1;
    for (int k = 0; k < nc; k++)
        if (scnt[k] && (s < 0 || scnt[k] > scnt[s])) s = k;
    uint32_t shadow = s >= 0 ? cols[s] : grass;

    memset(bg, 0, (size_t)w * h);
    int sp = 0;
    for (int x = 0; x < w; x++) stk[sp++] = x, stk[sp++] = (h - 1) * w + x;
    for (int y = 0; y < h; y++) stk[sp++] = y * w, stk[sp++] = y * w + w - 1;
    while (sp) {
        int i = stk[--sp];
        if (bg[i]) continue;
        uint32_t c = win[i];
        if (c != grass && c != blade && c != shadow) continue;
        bg[i] = 1;
        int x = i % w, y = i / w;
        if (x > 0) stk[sp++] = i - 1;
        if (x + 1 < w) stk[sp++] = i + 1;
        if (y > 0) stk[sp++] = i - w;
        if (y + 1 < h) stk[sp++] = i + w;
    }
    /* Keep the largest connected non-background blob: the tree, not bits of its neighbours. */
    for (int i = 0; i < w * h; i++) comp[i] = -1;
    int best = -1, best_n = 0, ncomp = 0;
    for (int i0 = 0; i0 < w * h; i0++) {
        if (bg[i0] || comp[i0] >= 0) continue;
        int n = 0;
        sp = 0;
        stk[sp++] = i0;
        comp[i0] = ncomp;
        while (sp) {
            int i = stk[--sp], x = i % w, y = i / w;
            n++;
            int nb[4] = {x > 0 ? i - 1 : -1, x + 1 < w ? i + 1 : -1, y > 0 ? i - w : -1, y + 1 < h ? i + w : -1};
            for (int k = 0; k < 4; k++)
                if (nb[k] >= 0 && !bg[nb[k]] && comp[nb[k]] < 0) comp[nb[k]] = ncomp, stk[sp++] = nb[k];
        }
        if (n > best_n) best_n = n, best = ncomp;
        ncomp++;
    }
    if (best < 0 || best_n < 64) return false;
    int x0 = w, y0 = h, x1 = -1, y1 = -1;
    for (int i = 0; i < w * h; i++)
        if (comp[i] == best) {
            int x = i % w, y = i / w;
            if (x < x0) x0 = x;
            if (x > x1) x1 = x;
            if (y < y0) y0 = y;
            if (y > y1) y1 = y;
        }
    out->w = x1 - x0 + 1;
    out->h = y1 - y0 + 1;
    out->opaque = 0;
    for (int y = 0; y < out->h; y++)
        for (int x = 0; x < out->w; x++) {
            int i = (y0 + y) * w + x0 + x;
            out->px[y * out->w + x] = comp[i] == best ? (win[i] | 0xFF000000u) : 0;
            out->opaque += comp[i] == best;
        }
    out->grass = grass | 0xFF000000u;
    out->phase_x = (sx + x0) & 7;
    out->phase_y = (sy + y0) & 7;
    out->tileset = tileset_fn ? tileset_fn((sx + x0) / 32, (sy + y1) / 32) : -1;
    return true;
}

int tm_init(TmPixelFn pixel, bool event, int (*tileset_at)(int tx, int ty))
{
    pixel_fn = pixel;
    tileset_fn = tileset_at;
    nsprites = 0;
    for (int i = 0; i < NSRC; i++)
        if (cut(sources[i].x, sources[i].y, sources[i].w, sources[i].h, event, &sprites[nsprites])) nsprites++;
    return nsprites;
}

const TreeSprite *tm_sprite(int i) { return &sprites[i]; }
int tm_count(void) { return nsprites; }

/* A copy matches when most of the drawing's pixels are identical in the map. Trees standing
 * behind others are partly covered, so 55% is enough; a quick sample rejects most spots early. */
static bool matches(const TreeSprite *t, int ox, int oy, bool event)
{
    int need = t->opaque * 55 / 100, miss_limit = t->opaque - need, miss = 0, hit = 0;
    /* Early rejection on a sparse sample first. */
    int sample_miss = 0, sampled = 0;
    for (int i = 0; i < t->w * t->h && sampled < 24; i += 37) {
        if (!(t->px[i] >> 24)) continue;
        sampled++;
        if (rgb(pixel_fn(ox + i % t->w, oy + i / t->w, event)) != rgb(t->px[i])) sample_miss++;
    }
    if (sample_miss * 2 > sampled) return false;
    /* The trunk (bottom quarter) must be there: it's what stands on the ground. */
    int tmiss = 0, tall = 0;
    for (int y = t->h * 3 / 4; y < t->h; y++)
        for (int x = 0; x < t->w; x++) {
            uint32_t c = t->px[y * t->w + x];
            if (!(c >> 24)) continue;
            tall++;
            if (rgb(pixel_fn(ox + x, oy + y, event)) != rgb(c)) tmiss++;
        }
    if (tall && tmiss * 10 > tall * 3) return false;
    for (int i = 0; i < t->w * t->h; i++) {
        if (!(t->px[i] >> 24)) continue;
        if (rgb(pixel_fn(ox + i % t->w, oy + i / t->w, event)) == rgb(t->px[i])) hit++;
        else if (++miss > miss_limit) return false;
    }
    return hit >= need;
}

int tm_find(int x0, int y0, int x1, int y1, bool event, TreeMatch *out, int max)
{
    int n = 0;
    for (int s = 0; s < nsprites; s++) {
        const TreeSprite *t = &sprites[s];
        /* Bottom row in [y0, y1): top-left y = bottom - h + 1. */
        for (int by = y0; by < y1; by++) {
            int oy = by - t->h + 1;
            if (((oy - t->phase_y) & 7) != 0 || oy < 0) continue;
            for (int ox = x0; ox < x1; ox++) {
                if (((ox - t->phase_x) & 7) != 0 || ox < 0) continue;
                if (tileset_fn && t->tileset >= 0 && tileset_fn(ox / 32, by / 32) != t->tileset) continue;
                if (n < max && matches(t, ox, oy, event)) out[n++] = (TreeMatch){s, ox, oy};
            }
        }
    }
    return n;
}
