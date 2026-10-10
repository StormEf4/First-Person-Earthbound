/* Phase 1 world viewer: a free-fly camera around the 3D overworld, with switchable creative
 * options (D20). Rendered at a chunky low resolution and scaled up (D19). */
#pragma once
#include <stdbool.h>
#include <stdio.h>

void viewer_init(void);
void viewer_shutdown(void);

/* Reads/writes the viewer's options in fpeb.ini. Returns true if the key was the viewer's. */
bool viewer_config_read(const char *key, const char *value);
void viewer_config_write(FILE *f);

/* Puts the camera at a pixel position on the EarthBound map, facing an EarthBound direction (0-7). */
void viewer_place(float px, float py, int direction);
void viewer_set_angles(float yaw_deg, float pitch_deg);
void viewer_set_height(float units);

/* Returns true if an option changed (so the caller can save the config). */
bool viewer_update(float dt);
void viewer_draw(bool show_panel);
