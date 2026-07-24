/**
 * map_view.h — the moving map. Phase-0 stub renderer: world-anchored grid
 * + track ribbon + own-vehicle marker + hazard chips. Real raster tiles
 * replace the grid at milestone M2; this module's interface is the
 * "renderer-agnostic" boundary from handoff-spec §6.
 */
#pragma once

#include "lvgl.h"
#include "vmesh_msg.h"
#include "hazard_store.h"

void map_view_create(lv_obj_t *parent);

/* Filesystem root of the slippy-tile tree ({root}/{z}/{x}/{y}.png).
 * "assets/tiles" on the simulator, "/sdcard/tiles" on the device.
 * Missing tiles degrade gracefully to the grid background. */
void map_view_set_tile_root(const char *root);

/* Reposition everything around the current pose. Call every UI tick. */
void map_view_update(const vmesh_pose_t *pose, uint32_t now_s);

/* Forget failed tile slots (post-download heal). */
void map_view_retry_failed(void);

/* Current view center (pan point if panning, else vehicle). */
void map_view_get_center(double *lat, double *lon);

/* Called when a hazard chip is tapped; index is the store slot. */
void map_view_set_tap_cb(void (*cb)(hazard_t *h));

/* Drop the chip of an expired hazard (hazard_store prune callback). */
void map_view_drop_chip(hazard_t *h);
