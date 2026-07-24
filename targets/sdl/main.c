/* targets/sdl/main.c — desktop simulator target.
 * All product code is in ui/, msg/, sim/; this file is only display
 * glue + the main loop. The esp32p4 target will mirror this shape. */

#include "lvgl.h"
#include "map_view.h"
#include "scenario.h"
#include "ui.h"
#include <unistd.h>
#include "feed.h"
#include "settings.h"
#include "provision.h"
#include "lodepng/lodepng.h"

#include <SDL2/SDL.h>
#include <stdio.h>
#include <string.h>

#define WIN_W 1280 /* dev-kit panel: 720x1280 native, mounted landscape */
#define WIN_H 720  /* (device rotates 90°) — sim matches 1:1            */

static uint32_t tick_cb(void) { return SDL_GetTicks(); }

int serial_radio_start(const char *dev);
void serial_radio_tick(void);

int main(int argc, char **argv)
{
    const char *path = "sim/scenarios/highway_demo.json";
    const char *radio = NULL;
    const char *shot = NULL;   /* --shot out.png: dump screen + exit */
    float shot_t = 2.0f;       /* --shot-t: seconds before the dump */
    const char *open_scr = NULL; /* --open s6: open an overlay first */
    float warp = 1.0f;           /* --warp N: sim-time multiplier */
    double at_lat = 0.0, at_lon = 0.0; /* --at LAT,LON: pin pose */
    for (int i = 1; i < argc; i++) {
        if (!strcmp(argv[i], "--radio") && i + 1 < argc)
            radio = argv[++i];
        else if (!strcmp(argv[i], "--shot") && i + 1 < argc)
            shot = argv[++i];
        else if (!strcmp(argv[i], "--shot-t") && i + 1 < argc)
            shot_t = (float)atof(argv[++i]);
        else if (!strcmp(argv[i], "--open") && i + 1 < argc)
            open_scr = argv[++i];
        else if (!strcmp(argv[i], "--warp") && i + 1 < argc)
            warp = (float)atof(argv[++i]);
        else if (!strcmp(argv[i], "--at") && i + 1 < argc)
            sscanf(argv[++i], "%lf,%lf", &at_lat, &at_lon);
        else if (argv[i][0] != '-')
            path = argv[i];
    }
    if (!scenario_load(path)) {
        fprintf(stderr, "usage: %s [scenario.json]  (run from repo root)\n",
                argv[0]);
        return 1;
    }

    lv_init();
    lv_tick_set_cb(tick_cb);
    lv_sdl_window_create(WIN_W, WIN_H);
    lv_sdl_mouse_create();

    scenario_set_own_origin(0x02000000u | ((unsigned)getpid() & 0x00FFFFFFu));
    scenario_set_demo(true); /* the desktop sim is a demo tool */
    if (at_lat != 0.0) { /* --at pins pose: co-locate for RF bench tests */
        vmesh_pose_t p = { .lat = at_lat, .lon = at_lon,
                           .heading_deg = 0, .speed_mps = 0 };
        vmesh_pose_set_live(&p);
    }
    extern void fake_provision_init(void);
    fake_provision_init();
    ui_init();
    map_view_set_tile_root("assets/tiles"); /* repo-relative; run from root */
    if (radio) serial_radio_start(radio);
    extern void ui_open_about(void);
    extern bool ui_debug_ask(void);
    if (open_scr && !strcmp(open_scr, "wifidemo"))
        vmesh_wifi_ops()->connect("DECODDOG", "hunter2");
    if (open_scr && !strcmp(open_scr, "s5")) ui_open_about();
    if (open_scr && !strcmp(open_scr, "s6")) settings_open();
    if (open_scr && !strcmp(open_scr, "s6join")) {
        settings_open();
        settings_debug_show_join("DECODDOG");
    }
    if (open_scr && !strcmp(open_scr, "s6scan")) {
        settings_open();
        settings_debug_scan();
    }

    uint32_t last = SDL_GetTicks();
    while (1) {
        uint32_t now = SDL_GetTicks();
        scenario_update((now - last) / 1000.0f * warp);
        last = now;

        serial_radio_tick();
        static bool qa_fired;
        if (open_scr && !strcmp(open_scr, "qa") && !qa_fired &&
            now > 5000 && ui_debug_ask()) {
            qa_fired = true;
            if (shot) shot_t = (now + 4000) / 1000.0f; /* after reply */
        }
        extern void ui_open_town_square(void);
        static bool sq_fired;
        if (open_scr && !strcmp(open_scr, "square") && !sq_fired &&
            now > 5000) {
            ui_open_town_square();
            sq_fired = true;
        }
        uint32_t wait = lv_timer_handler();

        if (shot && now >= (uint32_t)(shot_t * 1000.0f)) {
            lv_draw_buf_t *db =
                lv_snapshot_take(lv_screen_active(),
                                 LV_COLOR_FORMAT_ARGB8888);
            if (db) {
                uint32_t w = db->header.w, h = db->header.h;
                uint8_t *rgba = malloc((size_t)w * h * 4);
                const uint8_t *src = db->data;
                for (uint32_t p = 0; p < w * h; p++) {
                    rgba[p * 4 + 0] = src[p * 4 + 2];
                    rgba[p * 4 + 1] = src[p * 4 + 1];
                    rgba[p * 4 + 2] = src[p * 4 + 0];
                    rgba[p * 4 + 3] = 0xFF;
                }
                unsigned err = lodepng_encode32_file(shot, rgba, w, h);
                fprintf(stderr, "shot -> %s (%ux%u) err=%u\n",
                        shot, w, h, err);
                free(rgba);
                lv_draw_buf_destroy(db);
                return err ? 1 : 0;
            }
            fprintf(stderr, "snapshot failed\n");
            return 1;
        }
        SDL_Delay(wait > 30 ? 30 : (wait < 1 ? 1 : wait));
    }
    return 0;
}
