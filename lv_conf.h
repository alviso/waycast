/* lv_conf.h — minimal LVGL 9 config; everything not set here falls back
 * to lv_conf_internal.h defaults. Shared by both targets (the esp32p4
 * target adds its display driver via the BSP, not here). */
#ifndef LV_CONF_H
#define LV_CONF_H

#define LV_COLOR_DEPTH 32

/* SDL desktop driver (ignored by the ESP-IDF build) */
#define LV_USE_SDL 1

#define LV_FONT_MONTSERRAT_12 1
#define LV_FONT_MONTSERRAT_14 1
#define LV_FONT_MONTSERRAT_16 1
#define LV_FONT_MONTSERRAT_20 1
#define LV_FONT_MONTSERRAT_28 1

/* System allocator, not LVGL's tiny builtin pool: a single decoded
 * 256x256 tile is 256KB and must not fight widgets for a 64KB heap. */
#define LV_USE_STDLIB_MALLOC LV_STDLIB_CLIB

/* NOTE: LVGL's bundled lodepng/FS drivers stay OFF — tiles are decoded
 * by vendored upstream lodepng (third_party/lodepng) into RAM buffers;
 * LVGL's fork mis-decodes palette PNGs and its per-draw decode path is
 * far too slow on the device anyway. */

/* dev tooling: --shot self-screenshots in the simulator */
#define LV_USE_SNAPSHOT 1

#endif /* LV_CONF_H */
