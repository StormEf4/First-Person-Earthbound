/* Builds and draws EarthBound's overworld as 3D geometry, generated from the map and collision data. */
#pragma once
#include <stdbool.h>

#include "raylib.h"

/* Creative options the user flips between while playing (D20). Each cycles with a number key. */
typedef enum { OPT_RESOLUTION, OPT_WALL_STYLE, OPT_WALL_HEIGHT, OPT_FOG, OPT_MOOD, OPT_PALETTE, OPT_COUNT } OptionId;

typedef struct {
    int value[OPT_COUNT];
} W3dOptions;

const char *w3d_option_name(OptionId id);
const char *w3d_option_value_name(OptionId id, int value);
int w3d_option_count(OptionId id);
const char *w3d_option_key(OptionId id); /* key name used in fpeb.ini */

/* Low-resolution render target size for the current resolution option. */
void w3d_resolution(const W3dOptions *opt, int *w, int *h, float *display_aspect);

void w3d_init(void);
void w3d_shutdown(void);

/* Positions are in world units: 1 unit = one 8x8-pixel minitile; x east, z south, y up. */
void w3d_draw(Camera3D cam, const W3dOptions *opt);

/* Throws away built geometry, e.g. after a wall option changes. */
void w3d_invalidate(void);

/* Height of solid geometry under a world position (0 when open ground), for the fly camera. */
bool w3d_is_solid(float x, float z);
