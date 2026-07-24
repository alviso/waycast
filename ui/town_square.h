/**
 * town_square.h — S2, the parked town square (DEVICE_UI2.md).
 *
 * One mixed feed of everything alive on the mesh right now: hazards,
 * local notices, official alerts — channel shown as card color, tabs
 * are filters, never rooms. Opens only when parked (or with the
 * passenger affirmation); starts closing the moment you drive.
 */
#pragma once

#include "vmesh_msg.h"
#include <stdbool.h>

void town_square_open(void);
void town_square_close(void);
bool town_square_is_open(void);

/* The one entry point UI code should use: opens directly when parked;
 * while moving it asks for the passenger affirmation (a consent modal
 * whose grant expires after a few minutes). */
void town_square_request_open(void);

/* Called from the UI tick: enforces the posture gate (auto-close on
 * motion unless the passenger affirmation is set) and refreshes the
 * feed when its content actually changed. */
void town_square_tick(const vmesh_pose_t *pose, uint32_t now_s);

/* dev hook (sim --open square): affirms passenger + opens */
void ui_open_town_square(void);
