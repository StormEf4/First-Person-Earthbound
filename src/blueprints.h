/* Hand-placed building blueprints: where automatic classification can't tell a building from
 * a cliff or hedge, these rectangles (in 8x8 minitiles) claim the solid cells inside them as one
 * clean building with a fixed height in levels. */
#pragma once

typedef struct {
    const char *name;
    int x0, y0, x1, y1; /* inclusive minitile rectangle */
    int levels;         /* wall height in stood-up minitile rows */
} Blueprint;

extern const Blueprint blueprints[];
extern const int blueprint_count;
