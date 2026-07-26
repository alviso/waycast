#include "map_view.h"
#include "scenario.h" /* scenario_own_origin — delivery dot */
#include "convoy.h"
#include "names.h"
#include "feed.h"

#include <math.h>
#include <stdio.h>
#include <string.h>

/* Upstream lodepng (vendored), NOT LVGL's bundled copy: LVGL's fork
 * silently mis-decodes palette PNGs (OSM tiles are colortype-3) —
 * returns success with garbage pixels. Upstream also reads via plain
 * fopen(), which maps to the VFS on the device. */
#include "lodepng/lodepng.h"
#include <stdlib.h>

/* ---- projection: Web Mercator ----
 * World coordinates are global Mercator pixels at the current zoom;
 * the viewport follows the vehicle unless the user drags (then a
 * recenter button appears). North-up in v0 (heading-up is a follow-up).
 * At z16 / lat 37: ~1.9 m per pixel -> a 1280px view spans ~2.4 km. */
#define MIN_ZOOM 13
#define MAX_ZOOM 16
#define TILE_PX  256

static int s_zoom = 16;

/* pan state: when panning, the view center is a free world point */
static bool   s_panning;
static double pan_wx, pan_wy;   /* view center, world px at s_zoom */
static double veh_wx, veh_wy;   /* vehicle, world px at s_zoom (last tick) */

/* tile pool: enough lv_image objects to cover 1280x720 + one margin row/col */
#define POOL_COLS 7
#define POOL_ROWS 5
#define TILE_POOL (POOL_COLS * POOL_ROWS)

#define N_VGRID 7  /* fallback grid, aligned to tile boundaries */
#define N_HGRID 5
#define TRACK_PTS_MAX 64

/* Tiles are decoded ONCE on slot assignment into an RGB565 buffer in
 * RAM (PSRAM on the device) and handed to LVGL as a plain C-array
 * image. Relying on LVGL's decoder cache instead means the PNG gets
 * re-read + re-decoded from storage on EVERY frame (~400 ms/tile on
 * the P4 — measured seconds per frame). */
typedef struct {
    lv_obj_t      *img;
    int32_t        tx, ty; /* current tile index, -1 = unassigned */
    int8_t         tz;     /* zoom the slot was loaded at */
    uint8_t        fails;  /* consecutive load failures for this tile */
    bool           needed; /* scratch flag during per-tick reassignment */
    lv_draw_buf_t *db;     /* XRGB8888 pixel buffer, lazily created */
} tile_slot_t;

/* transient FS/decode hiccups get retried before we give up and leave
 * the tile dark (a genuinely missing tile stops after this many) */
#define TILE_LOAD_RETRIES 3

/* max PNG decodes per update tick — spreads the cost of entering a new
 * area across frames instead of stalling one frame for seconds */
#define DECODE_BUDGET_PER_TICK 2

static lv_obj_t *map;
static lv_obj_t *vgrid[N_VGRID], *hgrid[N_HGRID];
static tile_slot_t tiles[TILE_POOL];
static lv_obj_t *own_marker, *heading_tick;
static lv_obj_t *track_line, *attribution, *lbl_nodata;
/* +1: the ribbon is every crumb PLUS the live vehicle point. Sized
 * exactly TRACK_PTS_MAX, the vehicle point overflowed once the crumb
 * buffer filled (~5 min of driving) and shredded the neighboring
 * statics — the "sometimes reboots while panning" / stuck-black-tile
 * memory corruption. */
static lv_point_precise_t track_pts[TRACK_PTS_MAX + 1];

static char tile_root[64] = "assets/tiles";

static void (*tap_cb)(hazard_t *);

/* lon/lat -> global Mercator pixels at zoom z (doubles: values ~1e7) */
static double merc_x(double lon, int z)
{
    return (lon + 180.0) / 360.0 * (double)(1 << z) * TILE_PX;
}

static double merc_y(double lat, int z)
{
    double s = asinh(tan(lat * M_PI / 180.0));
    return (1.0 - s / M_PI) / 2.0 * (double)(1 << z) * TILE_PX;
}

/* forget every failed slot so freshly-downloaded tiles load on the
 * next tick (called after an offline-map download completes) */
void map_view_retry_failed(void)
{
    for (int i = 0; i < TILE_POOL; i++) {
        if (tiles[i].fails) {
            tiles[i].tx = tiles[i].ty = -1;
            tiles[i].fails = 0;
            if (tiles[i].img)
                lv_obj_add_flag(tiles[i].img, LV_OBJ_FLAG_HIDDEN);
        }
    }
}

/* inverse: current view center (pan point or vehicle) -> lat/lon */
void map_view_get_center(double *lat, double *lon)
{
    double wx = s_panning ? pan_wx : veh_wx;
    double wy = s_panning ? pan_wy : veh_wy;
    double n = (double)(1 << s_zoom) * TILE_PX;
    *lon = wx / n * 360.0 - 180.0;
    *lat = atan(sinh(M_PI * (1.0 - 2.0 * wy / n))) * 180.0 / M_PI;
}

static lv_color_t severity_color(uint8_t sev)
{
    switch (sev) {
    case 3:  return lv_color_hex(0xE03131); /* danger  */
    case 2:  return lv_color_hex(0xF59F00); /* caution */
    default: return lv_color_hex(0x5C7CFA); /* info    */
    }
}

static const char *hz_symbol(uint8_t hz)
{
    switch (hz) {
    case VMESH_HZ_CRASH:     return LV_SYMBOL_WARNING;
    case VMESH_HZ_SLOWDOWN:  return LV_SYMBOL_SHUFFLE;
    case VMESH_HZ_DEBRIS:    return LV_SYMBOL_MINUS;
    case VMESH_HZ_POTHOLE:   return LV_SYMBOL_DOWN;
    case VMESH_HZ_WEATHER:   return LV_SYMBOL_TINT;
    case VMESH_HZ_CLOSURE:   return LV_SYMBOL_CLOSE;
    case VMESH_HZ_EMERGENCY: return LV_SYMBOL_BELL;
    default:                 return LV_SYMBOL_WARNING;
    }
}

#define LOCAL_PIN_COLOR 0x0CA678 /* teal: informational, not a warning */

static const char *local_symbol(uint8_t cat)
{
    switch (cat) {
    case VMESH_LC_LODGING: return LV_SYMBOL_HOME;
    case VMESH_LC_FUEL:    return LV_SYMBOL_CHARGE;
    case VMESH_LC_EVENT:   return LV_SYMBOL_BELL;
    case VMESH_LC_AID:     return LV_SYMBOL_PLUS;
    default:               return LV_SYMBOL_LIST;
    }
}

static void chip_event_cb(lv_event_t *e)
{
    hazard_t *h = lv_event_get_user_data(e);
    if (tap_cb && h && h->active) tap_cb(h);
}

void map_view_set_tap_cb(void (*cb)(hazard_t *)) { tap_cb = cb; }

void map_view_set_tile_root(const char *root)
{
    snprintf(tile_root, sizeof(tile_root), "%s", root);
}

void map_view_drop_chip(hazard_t *h)
{
    if (h->chip) {
        lv_obj_delete((lv_obj_t *)h->chip);
        h->chip = NULL;
    }
}

/* ---------------- pan / zoom controls ---------------- */

static lv_obj_t *btn_recenter;

static void map_press_cb(lv_event_t *e)
{
    (void)e;
    lv_indev_t *indev = lv_indev_active();
    if (!indev) return;
    lv_point_t v;
    lv_indev_get_vect(indev, &v);
    if (!s_panning) {
        if (v.x == 0 && v.y == 0) return; /* a tap, not a drag */
        s_panning = true;
        pan_wx = veh_wx;
        pan_wy = veh_wy;
    }
    pan_wx -= v.x;
    pan_wy -= v.y;
}

static void recenter_cb(lv_event_t *e)
{
    (void)e;
    s_panning = false;
}

static void zoom_cb(lv_event_t *e)
{
    int dz = (int)(intptr_t)lv_event_get_user_data(e);
    int nz = s_zoom + dz;
    if (nz < MIN_ZOOM || nz > MAX_ZOOM) return;
    /* keep the view center fixed: world px scale by 2 per level */
    double k = (dz > 0) ? 2.0 : 0.5;
    pan_wx *= k;
    pan_wy *= k;
    s_zoom = nz;
    /* slots at the old zoom are invalidated by their tz mismatch and
     * reload progressively under the per-tick decode budget */
}

static lv_obj_t *make_map_button(const char *txt, lv_event_cb_t cb,
                                 void *user, lv_align_t align,
                                 int32_t ox, int32_t oy)
{
    lv_obj_t *b = lv_button_create(map);
    lv_obj_set_size(b, 52, 52);
    lv_obj_align(b, align, ox, oy);
    lv_obj_set_style_radius(b, LV_RADIUS_CIRCLE, 0);
    lv_obj_set_style_bg_color(b, lv_color_hex(0x22262E), 0);
    lv_obj_set_style_bg_opa(b, LV_OPA_80, 0);
    lv_obj_set_style_border_color(b, lv_color_hex(0x3B4252), 0);
    lv_obj_set_style_border_width(b, 1, 0);
    lv_obj_add_event_cb(b, cb, LV_EVENT_CLICKED, user);
    lv_obj_t *l = lv_label_create(b);
    lv_label_set_text(l, txt);
    lv_obj_set_style_text_font(l, &lv_font_montserrat_20, 0);
    lv_obj_center(l);
    return b;
}

/* ---------------- creation ---------------- */

static lv_obj_t *make_gridline(bool vertical)
{
    lv_obj_t *o = lv_obj_create(map);
    if (vertical) lv_obj_set_size(o, 1, lv_pct(100));
    else          lv_obj_set_size(o, lv_pct(100), 1);
    lv_obj_set_style_bg_color(o, lv_color_hex(0x2A2E37), 0);
    lv_obj_set_style_border_width(o, 0, 0);
    lv_obj_set_style_radius(o, 0, 0);
    lv_obj_clear_flag(o, LV_OBJ_FLAG_CLICKABLE);
    return o;
}

void map_view_create(lv_obj_t *parent)
{
    map = lv_obj_create(parent);
    lv_obj_set_size(map, lv_pct(100), lv_pct(100));
    lv_obj_set_pos(map, 0, 0);
    lv_obj_set_style_bg_color(map, lv_color_hex(0x1A1D23), 0);
    lv_obj_set_style_border_width(map, 0, 0);
    lv_obj_set_style_radius(map, 0, 0);
    lv_obj_set_style_pad_all(map, 0, 0);
    lv_obj_clear_flag(map, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_add_flag(map, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_add_event_cb(map, map_press_cb, LV_EVENT_PRESSING, NULL);

    /* fallback grid, visible wherever a tile is missing (under tiles) */
    for (int i = 0; i < N_VGRID; i++) vgrid[i] = make_gridline(true);
    for (int i = 0; i < N_HGRID; i++) hgrid[i] = make_gridline(false);

    /* tile pool */
    for (int i = 0; i < TILE_POOL; i++) {
        tiles[i].img = lv_image_create(map);
        tiles[i].tx = tiles[i].ty = -1;
        lv_obj_add_flag(tiles[i].img, LV_OBJ_FLAG_HIDDEN);
        lv_obj_clear_flag(tiles[i].img, LV_OBJ_FLAG_CLICKABLE);
    }

    /* track ribbon (breadcrumb of where we've driven) */
    track_line = lv_line_create(map);
    lv_obj_set_style_line_width(track_line, 10, 0);
    lv_obj_set_style_line_color(track_line, lv_color_hex(0x339AF0), 0);
    lv_obj_set_style_line_opa(track_line, LV_OPA_50, 0);
    lv_obj_set_style_line_rounded(track_line, true, 0);
    lv_obj_clear_flag(track_line, LV_OBJ_FLAG_CLICKABLE);

    /* own vehicle: fixed at screen center, world moves underneath */
    own_marker = lv_obj_create(map);
    lv_obj_set_size(own_marker, 22, 22);
    lv_obj_set_style_radius(own_marker, LV_RADIUS_CIRCLE, 0);
    lv_obj_set_style_bg_color(own_marker, lv_color_hex(0x339AF0), 0);
    lv_obj_set_style_border_color(own_marker, lv_color_white(), 0);
    lv_obj_set_style_border_width(own_marker, 3, 0);
    lv_obj_clear_flag(own_marker, LV_OBJ_FLAG_CLICKABLE);

    heading_tick = lv_obj_create(map);
    lv_obj_set_size(heading_tick, 8, 8);
    lv_obj_set_style_radius(heading_tick, LV_RADIUS_CIRCLE, 0);
    lv_obj_set_style_bg_color(heading_tick, lv_color_white(), 0);
    lv_obj_set_style_border_width(heading_tick, 0, 0);
    lv_obj_clear_flag(heading_tick, LV_OBJ_FLAG_CLICKABLE);

    /* OSM attribution — required by the tile license */
    attribution = lv_label_create(map);
    lv_label_set_text(attribution, "(c) OpenStreetMap contributors");
    lv_obj_set_style_text_font(attribution, &lv_font_montserrat_12, 0);
    lv_obj_set_style_text_color(attribution, lv_color_hex(0x868E96), 0);
    lv_obj_align(attribution, LV_ALIGN_BOTTOM_LEFT, 6, -4);

    /* shown while any visible tile is beyond the stored map — so the
     * data boundary reads as a boundary, not as a rendering bug */
    lbl_nodata = lv_label_create(map);
    lv_label_set_text(lbl_nodata, "edge of stored map");
    lv_obj_set_style_text_font(lbl_nodata, &lv_font_montserrat_14, 0);
    lv_obj_set_style_text_color(lbl_nodata, lv_color_hex(0x868E96), 0);
    lv_obj_set_style_bg_color(lbl_nodata, lv_color_hex(0x14161B), 0);
    lv_obj_set_style_bg_opa(lbl_nodata, LV_OPA_70, 0);
    lv_obj_set_style_pad_all(lbl_nodata, 6, 0);
    lv_obj_set_style_radius(lbl_nodata, 6, 0);
    lv_obj_align(lbl_nodata, LV_ALIGN_TOP_MID, 0, 8);
    lv_obj_add_flag(lbl_nodata, LV_OBJ_FLAG_HIDDEN);

    /* zoom + recenter controls */
    make_map_button(LV_SYMBOL_PLUS, zoom_cb, (void *)(intptr_t)1,
                    LV_ALIGN_BOTTOM_RIGHT, -12, -72);
    make_map_button(LV_SYMBOL_MINUS, zoom_cb, (void *)(intptr_t)-1,
                    LV_ALIGN_BOTTOM_RIGHT, -12, -12);
    btn_recenter = make_map_button(LV_SYMBOL_GPS, recenter_cb, NULL,
                                   LV_ALIGN_BOTTOM_LEFT, 12, -32);
    lv_obj_add_flag(btn_recenter, LV_OBJ_FLAG_HIDDEN);
}

/* load {root}/{z}/{tx}/{ty}.png into the slot's pixel buffer.
 * Returns 0 on success, lodepng error 78 = file missing (permanent),
 * anything else = possibly transient, worth retrying. */
#define TILE_ERR_BADSIZE 999
#define TILE_ERR_NOMEM   1000

static unsigned tile_load(tile_slot_t *slot, int32_t tx, int32_t ty)
{
    char path[136];
    snprintf(path, sizeof(path), "%s/%d/%ld/%ld.png",
             tile_root, s_zoom, (long)tx, (long)ty);

    unsigned char *rgba = NULL;
    unsigned w = 0, h = 0;
    unsigned err = lodepng_decode32_file(&rgba, &w, &h, path);
    if (err != 0) {
        free(rgba);
        return err;
    }
    if (w != TILE_PX || h != TILE_PX) {
        free(rgba);
        return TILE_ERR_BADSIZE;
    }

    if (!slot->db)
        slot->db = lv_draw_buf_create(TILE_PX, TILE_PX,
                                      LV_COLOR_FORMAT_XRGB8888,
                                      LV_STRIDE_AUTO);
    if (!slot->db) {
        free(rgba);
        return TILE_ERR_NOMEM;
    }

    /* lodepng gives R,G,B,A bytes; XRGB8888 wants B,G,R,X in memory.
     * Respect the draw buf's stride — it may be alignment-padded. */
    for (int y = 0; y < TILE_PX; y++) {
        const unsigned char *s = rgba + (size_t)y * TILE_PX * 4;
        uint32_t *d = (uint32_t *)(slot->db->data +
                                   (size_t)y * slot->db->header.stride);
        for (int x = 0; x < TILE_PX; x++, s += 4)
            *d++ = 0xFF000000u | ((uint32_t)s[0] << 16) |
                   ((uint32_t)s[1] << 8) | s[2];
    }
    free(rgba);

    /* No LVGL image cache in play (LV_CACHE_DEF_SIZE=0 on both targets):
     * draws read our buffer directly, so re-filling it is enough. */
    lv_image_set_src(slot->img, slot->db);
    lv_obj_invalidate(slot->img);
    return 0;
}

/* failure bookkeeping shared by fresh assigns and retries.
 * NOTE: err 78 ("can't open") is NOT treated as proof-of-missing —
 * SPIFFS can transiently fail opens under load, and a permanent
 * blacklist turned those into tiles stuck black forever. Everything
 * gets TILE_LOAD_RETRIES, and exhausted tiles heal (below). */
static void tile_load_failed(tile_slot_t *slot, unsigned err)
{
    lv_obj_add_flag(slot->img, LV_OBJ_FLAG_HIDDEN);
    if (slot->fails == 0) /* log once per attempt-cycle, not per retry */
        printf("[tile] z%d %ld/%ld load failed err=%u\n",
               (int)slot->tz, (long)slot->tx, (long)slot->ty, err);
    slot->fails = (uint8_t)(slot->fails + 1);
}

/* ---------------- per-tick update ---------------- */

/* breadcrumb trail (geo, so it survives zoom changes) */
static double crumb_lat[TRACK_PTS_MAX], crumb_lon[TRACK_PTS_MAX];
static int    n_crumbs;

static void tiles_update(double view_x0, double view_y0,
                         int32_t W, int32_t H)
{
    int32_t tx0 = (int32_t)floor(view_x0 / TILE_PX);
    int32_t ty0 = (int32_t)floor(view_y0 / TILE_PX);
    int32_t tx1 = (int32_t)floor((view_x0 + W) / TILE_PX);
    int32_t ty1 = (int32_t)floor((view_y0 + H) / TILE_PX);
    int32_t max_t = (1 << s_zoom) - 1;

    /* pass 1: slots whose tile is still in view keep their slot.
     * (Single-pass steal-anything causes reassignment thrash: a "free"
     * slot may hold a tile that IS visible but not yet visited.) */
    for (int i = 0; i < TILE_POOL; i++) {
        tile_slot_t *t = &tiles[i];
        t->needed = (t->tz == s_zoom &&
                     t->tx >= tx0 && t->tx <= tx1 &&
                     t->ty >= ty0 && t->ty <= ty1);
    }

    /* pass 2: every visible tile gets a slot; new tiles take unneeded ones */
    int decode_budget = DECODE_BUDGET_PER_TICK;
    for (int32_t tx = tx0; tx <= tx1; tx++) {
        for (int32_t ty = ty0; ty <= ty1; ty++) {
            if (tx < 0 || ty < 0 || tx > max_t || ty > max_t) continue;

            tile_slot_t *slot = NULL;
            for (int i = 0; i < TILE_POOL; i++) {
                if (tiles[i].tz == s_zoom &&
                    tiles[i].tx == tx && tiles[i].ty == ty) {
                    slot = &tiles[i];
                    break;
                }
            }
            if (slot) {
                /* a healthy tile returning to view after scroll-out:
                 * its pixels are still in the buffer — just unhide it.
                 * (Without this, revisited tiles stayed black forever:
                 * the loop-demo lap seam and pan-back both hit it.) */
                if (slot->fails == 0)
                    lv_obj_remove_flag(slot->img, LV_OBJ_FLAG_HIDDEN);
                /* assigned but dark from a transient failure? retry */
                if (slot->fails > 0 && slot->fails < TILE_LOAD_RETRIES &&
                    decode_budget > 0) {
                    decode_budget--;
                    unsigned err = tile_load(slot, tx, ty);
                    if (err == 0) {
                        slot->fails = 0;
                        lv_obj_remove_flag(slot->img, LV_OBJ_FLAG_HIDDEN);
                    } else {
                        tile_load_failed(slot, err);
                    }
                }
            } else {
                if (decode_budget <= 0) continue; /* pick it up next tick */
                for (int i = 0; i < TILE_POOL; i++) {
                    if (!tiles[i].needed) {
                        slot = &tiles[i];
                        break;
                    }
                }
                if (!slot) continue; /* pool exhausted (shouldn't happen) */

                slot->tx = tx;
                slot->ty = ty;
                slot->tz = (int8_t)s_zoom;
                slot->needed = true;
                slot->fails = 0;
                decode_budget--;
                unsigned err = tile_load(slot, tx, ty);
                if (err == 0) {
                    lv_obj_remove_flag(slot->img, LV_OBJ_FLAG_HIDDEN);
                } else {
                    tile_load_failed(slot, err);
                }
            }
            lv_obj_set_pos(slot->img,
                           (int32_t)(tx * TILE_PX - view_x0),
                           (int32_t)(ty * TILE_PX - view_y0));
        }
    }

    /* park anything that scrolled out (else it lingers at a stale pos),
     * and clear its failure history — scrolling back must retry */
    for (int i = 0; i < TILE_POOL; i++) {
        if (!tiles[i].needed) {
            lv_obj_add_flag(tiles[i].img, LV_OBJ_FLAG_HIDDEN);
            if (tiles[i].fails) {
                tiles[i].tx = -1; /* unassign -> fresh attempt on return */
                tiles[i].fails = 0;
            }
        }
    }

    /* slow heal for retry-exhausted tiles still in view: one fresh
     * attempt every ~6s. Transient failures recover; a genuinely
     * missing tile costs one decode attempt per cycle — negligible. */
    static uint32_t heal_tick;
    if (++heal_tick % 64 == 0) {
        for (int i = 0; i < TILE_POOL; i++) {
            if (tiles[i].needed && tiles[i].fails >= TILE_LOAD_RETRIES) {
                tiles[i].tx = -1;
                tiles[i].fails = 0;
            }
        }
    }

    /* boundary hint: any visible tile confirmed-missing? */
    bool any_missing = false;
    for (int i = 0; i < TILE_POOL; i++) {
        if (tiles[i].needed && tiles[i].fails >= TILE_LOAD_RETRIES) {
            any_missing = true;
            break;
        }
    }
    if (any_missing) lv_obj_remove_flag(lbl_nodata, LV_OBJ_FLAG_HIDDEN);
    else             lv_obj_add_flag(lbl_nodata, LV_OBJ_FLAG_HIDDEN);
}

void map_view_update(const vmesh_pose_t *pose, uint32_t now_s)
{
    const int32_t W = lv_obj_get_width(map);
    const int32_t H = lv_obj_get_height(map);
    if (W == 0 || H == 0) return;

    veh_wx = merc_x(pose->lon, s_zoom);
    veh_wy = merc_y(pose->lat, s_zoom);
    double cx_w = s_panning ? pan_wx : veh_wx;
    double cy_w = s_panning ? pan_wy : veh_wy;
    double view_x0 = cx_w - W / 2.0;
    double view_y0 = cy_w - H / 2.0;

    if (btn_recenter) {
        if (s_panning) lv_obj_remove_flag(btn_recenter, LV_OBJ_FLAG_HIDDEN);
        else           lv_obj_add_flag(btn_recenter, LV_OBJ_FLAG_HIDDEN);
    }

    /* fallback grid, aligned to tile boundaries */
    double gox = fmod(view_x0, TILE_PX);
    double goy = fmod(view_y0, TILE_PX);
    for (int i = 0; i < N_VGRID; i++)
        lv_obj_set_pos(vgrid[i], (int32_t)(i * TILE_PX - gox), 0);
    for (int i = 0; i < N_HGRID; i++)
        lv_obj_set_pos(hgrid[i], 0, (int32_t)(i * TILE_PX - goy));

    tiles_update(view_x0, view_y0, W, H);

    /* breadcrumb ribbon (crumbs are geo; ~100 m spacing at z16).
     * A jump > ~1 km means a scenario wrap/teleport: restart the trail. */
    if (n_crumbs > 0 &&
        fabs(pose->lat - crumb_lat[n_crumbs - 1]) +
        fabs(pose->lon - crumb_lon[n_crumbs - 1]) > 0.01)
        n_crumbs = 0;
    if (n_crumbs == 0 ||
        fabs(pose->lat - crumb_lat[n_crumbs - 1]) +
        fabs(pose->lon - crumb_lon[n_crumbs - 1]) > 0.0008) {
        if (n_crumbs == TRACK_PTS_MAX) {
            memmove(crumb_lat, crumb_lat + 1,
                    sizeof(double) * (TRACK_PTS_MAX - 1));
            memmove(crumb_lon, crumb_lon + 1,
                    sizeof(double) * (TRACK_PTS_MAX - 1));
            n_crumbs--;
        }
        crumb_lat[n_crumbs] = pose->lat;
        crumb_lon[n_crumbs] = pose->lon;
        n_crumbs++;
    }
    int pts = 0;
    for (int i = 0; i < n_crumbs; i++) {
        track_pts[pts].x =
            (lv_value_precise_t)(merc_x(crumb_lon[i], s_zoom) - view_x0);
        track_pts[pts].y =
            (lv_value_precise_t)(merc_y(crumb_lat[i], s_zoom) - view_y0);
        pts++;
    }
    track_pts[pts].x = (lv_value_precise_t)(veh_wx - view_x0);
    track_pts[pts].y = (lv_value_precise_t)(veh_wy - view_y0);
    pts++;
    lv_line_set_points(track_line, track_pts, pts);

    /* own marker + heading tick (at the vehicle, which is only the
     * screen center while following) */
    int32_t mx = (int32_t)(veh_wx - view_x0), my = (int32_t)(veh_wy - view_y0);
    lv_obj_set_pos(own_marker, mx - 11, my - 11);
    /* grey the marker while the position is stale (last-known, no live
     * GPS fix yet); blue once a real fix is in. Change-detected. */
    {
        static int prev_live = -1;
        int live = vmesh_pose_is_live() ? 1 : 0;
        if (live != prev_live) {
            prev_live = live;
            lv_obj_set_style_bg_color(own_marker,
                lv_color_hex(live ? 0x339AF0 : 0x868E96), 0);
            lv_obj_set_style_opa(heading_tick, live ? LV_OPA_COVER : LV_OPA_40, 0);
        }
    }
    float hr = pose->heading_deg * (float)M_PI / 180.f;
    lv_obj_set_pos(heading_tick,
                   (int32_t)(mx + sinf(hr) * 18.f) - 4,
                   (int32_t)(my - cosf(hr) * 18.f) - 4);

    /* hazard chips */
    for (int i = 0; i < HAZARD_MAX; i++) {
        hazard_t *h = hazard_store_slot(i);
        if (!h || !h->active) continue;

        if (!h->chip) {
            bool local = h->msg.channel == VMESH_CH_LOCAL;
            bool group = h->msg.channel == VMESH_CH_GROUP;
            lv_obj_t *chip = lv_button_create(map);
            lv_obj_set_size(chip, local || group ? 46 : 56,
                                  local || group ? 46 : 56);
            lv_obj_set_style_radius(chip, LV_RADIUS_CIRCLE, 0);
            lv_obj_set_style_bg_color(chip,
                group ? lv_color_hex(0x2B3A55)
                      : local ? lv_color_hex(LOCAL_PIN_COLOR)
                              : severity_color(h->msg.severity), 0);
            lv_obj_t *lbl = lv_label_create(chip);
            /* group frames carry a SUBTYPE in hazard_type, not a
             * hazard id — never run it through hz_symbol() */
            lv_label_set_text(lbl,
                group ? LV_SYMBOL_SHUFFLE
                      : local ? local_symbol(h->msg.hazard_type)
                              : hz_symbol(h->msg.hazard_type));
            lv_obj_center(lbl);
            lv_obj_add_event_cb(chip, chip_event_cb, LV_EVENT_CLICKED, h);
            /* delivery dot — OWN reports only: red until at least one
             * relay echoes the report back ("shouting into the void"),
             * green once the town has demonstrably heard it */
            if (h->msg.origin_id == scenario_own_origin()) {
                lv_obj_t *dot = lv_obj_create(chip);
                lv_obj_set_size(dot, 12, 12);
                lv_obj_set_style_radius(dot, LV_RADIUS_CIRCLE, 0);
                lv_obj_set_style_border_width(dot, 1, 0);
                lv_obj_set_style_border_color(dot, lv_color_hex(0x14161B), 0);
                lv_obj_set_style_bg_color(dot, lv_color_hex(0xE03131), 0);
                lv_obj_clear_flag(dot, LV_OBJ_FLAG_CLICKABLE);
                /* on the rim at 60 deg from 12 o'clock (the 2 o'clock
                 * position): offset = r*(sin60, -cos60) from center */
                {
                    int r = (local ? 46 : 56) / 2 - 2;
                    lv_obj_align(dot, LV_ALIGN_CENTER,
                                 (int)(r * 0.866f), -(r / 2));
                }
            }
            h->chip = chip;
        }

        lv_obj_t *chip = (lv_obj_t *)h->chip;
        double hx = merc_x(h->msg.lon_e7 / 1e7, s_zoom);
        double hy = merc_y(h->msg.lat_e7 / 1e7, s_zoom);
        lv_obj_set_pos(chip, (int32_t)(hx - view_x0) - 28,
                             (int32_t)(hy - view_y0) - 28);

        /* ephemerality made visible: fade with age (plan §4); doubt
         * fades harder — a disputed report dims regardless of age */
        float age = hazard_age_frac(h, now_s);
        lv_opa_t opa = (lv_opa_t)(255 - (int)(age * 165));
        if (h->denies > h->confirms && opa > 100) opa = 100;
        lv_obj_set_style_opa(chip, opa, 0);

        /* delivery dot tracks the echo state (child 1 when present) */
        if (h->msg.origin_id == scenario_own_origin() &&
            lv_obj_get_child_count(chip) > 1) {
            lv_obj_t *dot = lv_obj_get_child(chip, 1);
            lv_obj_set_style_bg_color(dot,
                lv_color_hex(h->echoed ? 0x2F9E44 : 0xE03131), 0);
        }

        /* fresh pulse: bright ring for the first 30 s */
        bool fresh = (now_s - h->msg.created_s) < 30;
        lv_obj_set_style_border_width(chip, fresh ? 3 : 0, 0);
        lv_obj_set_style_border_color(chip, lv_color_white(), 0);
    }

    /* convoy members: named dots, the "where did Dave go" answer.
     * Distinct from hazard chips — smaller, blue-grey, never tappable. */
    {
        static lv_obj_t *cm_dot[CONVOY_MAX], *cm_lbl[CONVOY_MAX];
        for (int i = 0; i < CONVOY_MAX; i++) {
            convoy_member_t *mb = convoy_slot(i);
            bool live = mb && mb->origin;
            if (live && !cm_dot[i]) {
                cm_dot[i] = lv_obj_create(map);
                lv_obj_set_size(cm_dot[i], 18, 18);
                lv_obj_set_style_radius(cm_dot[i], LV_RADIUS_CIRCLE, 0);
                lv_obj_set_style_bg_color(cm_dot[i],
                                          lv_color_hex(0x8AB4F8), 0);
                lv_obj_set_style_border_color(cm_dot[i],
                                              lv_color_hex(0x14161B), 0);
                lv_obj_set_style_border_width(cm_dot[i], 2, 0);
                lv_obj_clear_flag(cm_dot[i], LV_OBJ_FLAG_CLICKABLE);
                cm_lbl[i] = lv_label_create(map);
                lv_obj_set_style_text_color(cm_lbl[i],
                                            lv_color_hex(0x8AB4F8), 0);
                lv_obj_set_style_text_font(cm_lbl[i],
                                           &lv_font_montserrat_12, 0);
                lv_obj_clear_flag(cm_lbl[i], LV_OBJ_FLAG_CLICKABLE);
            }
            if (!cm_dot[i]) continue;
            if (!live) {
                lv_obj_add_flag(cm_dot[i], LV_OBJ_FLAG_HIDDEN);
                lv_obj_add_flag(cm_lbl[i], LV_OBJ_FLAG_HIDDEN);
                continue;
            }
            lv_obj_remove_flag(cm_dot[i], LV_OBJ_FLAG_HIDDEN);
            lv_obj_remove_flag(cm_lbl[i], LV_OBJ_FLAG_HIDDEN);
            int32_t mx = (int32_t)(merc_x(mb->lon, s_zoom) - view_x0);
            int32_t my = (int32_t)(merc_y(mb->lat, s_zoom) - view_y0);
            lv_obj_set_pos(cm_dot[i], mx - 9, my - 9);
            const char *nm = names_get(mb->origin, now_s);
            char idbuf[12];
            if (!nm) {
                snprintf(idbuf, sizeof(idbuf), "%06X",
                         (unsigned)(mb->origin & 0xFFFFFF));
                nm = idbuf;
            }
            lv_label_set_text(cm_lbl[i], nm);
            lv_obj_set_pos(cm_lbl[i], mx + 12, my - 8);
        }
    }
}
