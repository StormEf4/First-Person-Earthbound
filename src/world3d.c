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
    [OPT_WALL_HEIGHT] = {"Wall height", "height", 4, {"Low", "Original proportions", "Tall", "Towering"}},
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

static float wall_scale(const W3dOptions *opt)
{
    static const float s[] = {0.6f, 1.0f, 1.5f, 2.2f};
    return s[opt->value[OPT_WALL_HEIGHT]];
}

/* ---- texture atlas of 32x32 map tiles, filled on demand ---- */

#define ATLAS_SIZE 4096
#define ATLAS_SLOTS_PER_ROW (ATLAS_SIZE / 32)
#define ATLAS_SLOTS (ATLAS_SLOTS_PER_ROW * ATLAS_SLOTS_PER_ROW)
#define KEY_COUNT (1 << 19) /* tileset(5) | palette(3) | event(1) | tile(10) */

static Texture2D atlas;
static int32_t *slot_of_key;
static int slots_used;
static bool atlas_full; /* flying far fills it; it's cleared and refilled on the next frame */

static int atlas_slot(int tileset, int palette, bool event, uint16_t tile)
{
    int key = (tileset & 31) << 14 | (palette & 7) << 11 | (event ? 1 : 0) << 10 | (tile & 1023);
    if (slot_of_key[key] >= 0) return slot_of_key[key];
    if (slots_used >= ATLAS_SLOTS) {
        atlas_full = true;
        return 0;
    }
    int slot = slots_used++;
    uint32_t px[32 * 32];
    ebw_render_tile(tileset, palette, tile, event, px, 32);
    Rectangle r = {(float)(slot % ATLAS_SLOTS_PER_ROW * 32), (float)(slot / ATLAS_SLOTS_PER_ROW * 32), 32, 32};
    UpdateTextureRec(atlas, r, px);
    slot_of_key[key] = slot;
    return slot;
}

/* UV rectangle of one 8x8 minitile (sub = 0..15 within its map tile). */
typedef struct {
    float u0, v0, u1, v1;
} UvRect;

static UvRect minitile_uv(int slot, int sub)
{
    const float inset = 0.02f / ATLAS_SIZE;
    float x = (float)(slot % ATLAS_SLOTS_PER_ROW * 32 + (sub & 3) * 8);
    float y = (float)(slot / ATLAS_SLOTS_PER_ROW * 32 + (sub >> 2) * 8);
    return (UvRect){x / ATLAS_SIZE + inset, y / ATLAS_SIZE + inset, (x + 8) / ATLAS_SIZE - inset,
                    (y + 8) / ATLAS_SIZE - inset};
}

static UvRect maptile_uv(int slot)
{
    const float inset = 0.02f / ATLAS_SIZE;
    float x = (float)(slot % ATLAS_SLOTS_PER_ROW * 32), y = (float)(slot / ATLAS_SLOTS_PER_ROW * 32);
    return (UvRect){x / ATLAS_SIZE + inset, y / ATLAS_SIZE + inset, (x + 32) / ATLAS_SIZE - inset,
                    (y + 32) / ATLAS_SIZE - inset};
}

/* ---- mesh building ---- */

typedef struct {
    float *pos, *uv;
    unsigned char *col;
    unsigned short *idx;
    int verts, tris, cap_v, cap_t;
} MeshBuf;

static void mb_quad(MeshBuf *m, Vector3 a, Vector3 b, Vector3 c, Vector3 d, UvRect uv, unsigned char shade)
{
    /* a-b-c-d counter-clockwise seen from the front; uv: a=(u0,v0) b=(u1,v0) c=(u1,v1) d=(u0,v1) */
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

/* ---- per-sector chunks ---- */

#define CHUNK_MW (EBW_SECTOR_W * 4) /* minitiles */
#define CHUNK_MH (EBW_SECTOR_H * 4)
#define MAX_RUN 64

typedef struct {
    bool built, has_mesh;
    Mesh mesh;
    int last_used;
} Chunk;

static Chunk chunks[EBW_SECTORS_Y][EBW_SECTORS_X];
static Shader shader;
static Material material;
static int frame_no;
static int loc_cam, loc_fog_color, loc_fog_start, loc_fog_end, loc_tint;

static bool solid(int mx, int my) { return (ebw_collision_at(mx, my) & EBW_SOLID) != 0; }

static UvRect minitile_uv_at(int mx, int my, bool event)
{
    int tx = mx >> 2, ty = my >> 2;
    int slot = atlas_slot(ebw_tileset_at(tx, ty), ebw_palette_at(tx, ty), event, ebw_tile_at(tx, ty));
    return minitile_uv(slot, (my & 3) * 4 + (mx & 3));
}

/* Vertical solid run containing (mx, my): first and last row. */
static void column_run(int mx, int my, int *y0, int *y1)
{
    int a = my, b = my;
    while (a > my - MAX_RUN && solid(mx, a - 1)) a--;
    while (b < my + MAX_RUN && solid(mx, b + 1)) b++;
    *y0 = a;
    *y1 = b;
}

/* In facade mode, a run's height in levels: its depth in minitiles, capped. */
static int facade_levels(int y0, int y1, int cap) { int n = y1 - y0 + 1; return n < cap ? n : cap; }

static void build_chunk(int sx, int sy, const W3dOptions *opt)
{
    MeshBuf m = {0};
    bool event = opt->value[OPT_PALETTE] == 1;
    float s = wall_scale(opt);
    bool facades = opt->value[OPT_WALL_STYLE] == 0;
    const int cap = 12; /* facade levels; deeper runs get a flat roof over the rest */

    /* Floor: one quad per map tile. */
    for (int ty = sy * EBW_SECTOR_H; ty < (sy + 1) * EBW_SECTOR_H; ty++)
        for (int tx = sx * EBW_SECTOR_W; tx < (sx + 1) * EBW_SECTOR_W; tx++) {
            int slot = atlas_slot(ebw_tileset_at(tx, ty), ebw_palette_at(tx, ty), event, ebw_tile_at(tx, ty));
            float x0 = tx * 4.0f, z0 = ty * 4.0f;
            mb_quad(&m, (Vector3){x0, 0, z0}, (Vector3){x0 + 4, 0, z0}, (Vector3){x0 + 4, 0, z0 + 4},
                    (Vector3){x0, 0, z0 + 4}, maptile_uv(slot), 255);
        }

    int mx0 = sx * CHUNK_MW, my0 = sy * CHUNK_MH;
    for (int my = my0; my < my0 + CHUNK_MH; my++)
        for (int mx = mx0; mx < mx0 + CHUNK_MW; mx++) {
            if (!solid(mx, my)) continue;
            float x = (float)mx, z = (float)my;

            if (!facades) {
                /* Blocks: every solid minitile is a box wearing its own art. */
                float h = 2.0f * s;
                UvRect uv = minitile_uv_at(mx, my, event);
                mb_quad(&m, (Vector3){x, h, z}, (Vector3){x + 1, h, z}, (Vector3){x + 1, h, z + 1},
                        (Vector3){x, h, z + 1}, uv, 255);
                int levels = (int)ceilf(h);
                for (int k = 0; k < levels; k++) {
                    float ya = h * k / levels, yb = h * (k + 1) / levels;
                    if (!solid(mx, my + 1))
                        mb_quad(&m, (Vector3){x, yb, z + 1}, (Vector3){x + 1, yb, z + 1},
                                (Vector3){x + 1, ya, z + 1}, (Vector3){x, ya, z + 1}, uv, 225);
                    if (!solid(mx, my - 1))
                        mb_quad(&m, (Vector3){x + 1, yb, z}, (Vector3){x, yb, z}, (Vector3){x, ya, z},
                                (Vector3){x + 1, ya, z}, uv, 150);
                    if (!solid(mx + 1, my))
                        mb_quad(&m, (Vector3){x + 1, yb, z + 1}, (Vector3){x + 1, yb, z},
                                (Vector3){x + 1, ya, z}, (Vector3){x + 1, ya, z + 1}, uv, 190);
                    if (!solid(mx - 1, my))
                        mb_quad(&m, (Vector3){x, yb, z}, (Vector3){x, yb, z + 1}, (Vector3){x, ya, z + 1},
                                (Vector3){x, ya, z}, uv, 190);
                }
                continue;
            }

            /* Facades: a column of solid minitiles stands up so the art drawn "above" a building's
             * base becomes its front wall, the way the original perspective implies. */
            int y0, y1;
            column_run(mx, my, &y0, &y1);
            int levels = facade_levels(y0, y1, cap);
            float h = levels * s;

            /* Roof over this minitile. */
            mb_quad(&m, (Vector3){x, h, z}, (Vector3){x + 1, h, z}, (Vector3){x + 1, h, z + 1},
                    (Vector3){x, h, z + 1}, minitile_uv_at(mx, my, event), 235);

            /* Height of the neighbouring columns, to know how much side wall shows above them. */
            float side_h[2] = {0, 0};
            for (int i = 0; i < 2; i++) {
                int nx = mx + (i ? 1 : -1);
                if (solid(nx, my)) {
                    int a, b;
                    column_run(nx, my, &a, &b);
                    side_h[i] = facade_levels(a, b, cap) * s;
                }
            }

            /* The level-k slice of this column's wall shows minitile row (y1 - k). */
            for (int k = 0; k < levels; k++) {
                UvRect uv = minitile_uv_at(mx, y1 - k, event);
                float ya = k * s, yb = (k + 1) * s;
                if (my == y1)
                    mb_quad(&m, (Vector3){x, yb, z + 1}, (Vector3){x + 1, yb, z + 1}, (Vector3){x + 1, ya, z + 1},
                            (Vector3){x, ya, z + 1}, uv, 225);
                if (my == y0)
                    mb_quad(&m, (Vector3){x + 1, yb, z}, (Vector3){x, yb, z}, (Vector3){x, ya, z},
                            (Vector3){x + 1, ya, z}, uv, 140);
                for (int side = -1; side <= 1; side += 2) {
                    float nh = side_h[side > 0];
                    if (nh >= yb) continue;
                    float lo = ya > nh ? ya : nh;
                    float ex = side > 0 ? x + 1 : x;
                    float za = side > 0 ? z + 1 : z, zb = side > 0 ? z : z + 1;
                    mb_quad(&m, (Vector3){ex, yb, za}, (Vector3){ex, yb, zb}, (Vector3){ex, lo, zb},
                            (Vector3){ex, lo, za}, uv, 185);
                }
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
    } else {
        RL_FREE(m.pos);
        RL_FREE(m.uv);
        RL_FREE(m.col);
        RL_FREE(m.idx);
    }
}

static void free_chunk(Chunk *c)
{
    if (c->has_mesh) UnloadMesh(c->mesh);
    memset(c, 0, sizeof *c);
}

void w3d_invalidate(void)
{
    for (int y = 0; y < EBW_SECTORS_Y; y++)
        for (int x = 0; x < EBW_SECTORS_X; x++) free_chunk(&chunks[y][x]);
}

/* ---- shader: fog and colour mood ---- */

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
    "  vec3 c = texture(texture0, uv).rgb * col.rgb * tint;\n"
    "  float f = clamp((length(wpos - camPos) - fogStart) / (fogEnd - fogStart), 0.0, 1.0);\n"
    "  finalColor = vec4(mix(c, fogColor, f), 1.0);\n"
    "}\n";

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
    slot_of_key = malloc(sizeof(int32_t) * KEY_COUNT);
    memset(slot_of_key, 0xFF, sizeof(int32_t) * KEY_COUNT);
    slots_used = 0;

    shader = LoadShaderFromMemory(vs_src, fs_src);
    shader.locs[SHADER_LOC_MATRIX_MODEL] = GetShaderLocation(shader, "matModel");
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
    free(slot_of_key);
    slot_of_key = NULL;
}

bool w3d_is_solid(float x, float z) { return solid((int)floorf(x), (int)floorf(z)); }

void w3d_draw(Camera3D cam, const W3dOptions *opt)
{
    frame_no++;
    if (atlas_full) {
        w3d_invalidate();
        memset(slot_of_key, 0xFF, sizeof(int32_t) * KEY_COUNT);
        slots_used = 0;
        atlas_full = false;
    }
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
    rlDisableBackfaceCulling(); /* walls are seen from both sides */

    float reach = fog_end + 16;
    Vector3 fwd = Vector3Normalize(Vector3Subtract(cam.target, cam.position));
    int csx = (int)floorf(cam.position.x / CHUNK_MW), csy = (int)floorf(cam.position.z / CHUNK_MH);
    int rx = (int)(reach / CHUNK_MW) + 1, ry = (int)(reach / CHUNK_MH) + 1;
    int builds_left = 6; /* spread chunk building over frames */

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
                    /* Skip chunks well behind the camera. */
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
                }
    rlEnableBackfaceCulling();
    EndMode3D();

    /* Free chunks not drawn for a while so memory stays bounded. */
    for (int y = 0; y < EBW_SECTORS_Y; y++)
        for (int x = 0; x < EBW_SECTORS_X; x++)
            if (chunks[y][x].built && frame_no - chunks[y][x].last_used > 600) free_chunk(&chunks[y][x]);
}
