/* gps_sniff.c — on-screen serial line sniffer (diagnostic build only).
 *
 * The L76K is awake (standby switch OFF, LEDs alive) yet no NMEA
 * reaches any pin we've tried — and the schematic's mirrored columns
 * have burned us before. So: stop guessing. Sample a spread of header
 * GPIOs as inputs, count edges and minimum pulse width per pin, and
 * paint the report on the display (the only reliable channel we have).
 *
 * Reading: "38:9600~120" = pin 38, ~9600 edges counted in the window,
 * min pulse ~120 us (=> ~9600 baud). "h"/"l" = idle high/low, no
 * activity. A UART TX line idles HIGH and bursts once per second.
 */
#include "driver/gpio.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "bsp/esp-bsp.h"
#include "lvgl.h"

#include <stdio.h>
#include <string.h>

/* v2: sweep EVERY matrix GPIO — the kit's 40-pin header map is not
 * trustworthy, so let the whole chip report. Input-enable only (never
 * reconfigures driven pins). Excluded: 24/25 (USB-Serial-JTAG pads). */
static int PINS[56];
static int npins;
#define NPINS npins
static void pins_init(void)
{
    npins = 0;
    for (int g = 0; g <= 54; g++) {
        if (g == 24 || g == 25) continue; /* USB-JTAG */
        PINS[npins++] = g;
    }
}

static volatile uint32_t edges[56];
static volatile uint32_t minpulse_us[56];
static volatile uint8_t  level_now[56];
static char report[220];
/* latched verdict: the last pin whose window matched slow-serial */
static volatile int hit_pin = -1;
static volatile uint32_t hit_edges, hit_pulse, uptime_s;

static void sniff_task(void *arg)
{
    (void)arg;
    pins_init();
    /* input-enable only: readable without touching output paths, so
     * pins driven by peripherals keep working and read as constant */
    for (int i = 0; i < NPINS; i++)
        gpio_input_enable((gpio_num_t)PINS[i]);

    static uint8_t prev[56];
    static int64_t last_edge_us[56];
    for (int i = 0; i < NPINS; i++) {
        prev[i] = gpio_get_level((gpio_num_t)PINS[i]);
        last_edge_us[i] = 0;
        minpulse_us[i] = 0xFFFFFFFFu;
    }

    int64_t window_start = esp_timer_get_time();
    static uint32_t wedges[56];

    for (;;) {
        int64_t now = esp_timer_get_time();
        for (int i = 0; i < NPINS; i++) {
            uint8_t lv = gpio_get_level((gpio_num_t)PINS[i]);
            if (lv != prev[i]) {
                wedges[i]++;
                if (last_edge_us[i]) {
                    uint32_t pw = (uint32_t)(now - last_edge_us[i]);
                    if (pw < minpulse_us[i]) minpulse_us[i] = pw;
                }
                last_edge_us[i] = now;
                prev[i] = lv;
            }
            level_now[i] = lv;
        }
        if (now - window_start >= 1000000) { /* 1 s window */
            for (int i = 0; i < NPINS; i++) {
                edges[i] = wedges[i];
                /* the hunt: a 4800-19200 baud UART line has 60-250 us
                 * minimum pulses and sustained edges. Latch matches. */
                if (wedges[i] >= 50 && minpulse_us[i] >= 60 &&
                    minpulse_us[i] <= 250) {
                    hit_pin = PINS[i];
                    hit_edges = wedges[i];
                    hit_pulse = minpulse_us[i];
                }
                /* per-window min pulse so a one-off glitch can't
                 * poison the classification forever */
                minpulse_us[i] = 0xFFFFFFFFu;
                wedges[i] = 0;
            }
            window_start = now;
            uptime_s++;
        }
        (void)edges;
        /* stay hot but let lower-prio housekeeping breathe */
        if ((now & 0x3FF) == 0) taskYIELD();
    }
}

static lv_obj_t *lbl;

static void sniff_lv_cb(lv_timer_t *t)
{
    (void)t;
    /* live stats for the UART-pair pads, requested explicitly */
    int i37 = -1, i38 = -1;
    for (int i = 0; i < NPINS; i++) {
        if (PINS[i] == 37) i37 = i;
        if (PINS[i] == 38) i38 = i;
    }
    char live[80] = "";
    if (i37 >= 0 && i38 >= 0)
        snprintf(live, sizeof(live),
                 "G37:%c %lue/s   G38:%c %lue/s\n",
                 level_now[i37] ? 'h' : 'l', (unsigned long)edges[i37],
                 level_now[i38] ? 'h' : 'l', (unsigned long)edges[i38]);
    if (hit_pin >= 0)
        snprintf(report, sizeof(report),
                 "%sGPS FOUND on GPIO %d  (%lu edges/s, %lu us pulses)",
                 live, hit_pin, (unsigned long)hit_edges,
                 (unsigned long)hit_pulse);
    else
        snprintf(report, sizeof(report),
                 "%shunt: nothing slow-serial yet (%lu s)",
                 live, (unsigned long)uptime_s);
    lv_label_set_text(lbl, report);
    lv_obj_set_style_text_color(lbl,
        hit_pin >= 0 ? lv_color_hex(0x51CF66) : lv_color_hex(0xFAC775), 0);
}

void gps_sniff_start(void)
{
    xTaskCreate(sniff_task, "gps_sniff", 4096, NULL, 4, NULL);

    bsp_display_lock(0);
    lbl = lv_label_create(lv_layer_top());
    lv_obj_set_style_text_font(lbl, &lv_font_montserrat_20, 0);
    lv_obj_set_style_text_color(lbl, lv_color_hex(0xFAC775), 0);
    lv_obj_set_style_bg_color(lbl, lv_color_hex(0x14161B), 0);
    lv_obj_set_style_bg_opa(lbl, LV_OPA_80, 0);
    lv_obj_set_style_pad_all(lbl, 4, 0);
    lv_obj_set_width(lbl, lv_pct(100));
    lv_label_set_long_mode(lbl, LV_LABEL_LONG_WRAP);
    lv_obj_align(lbl, LV_ALIGN_BOTTOM_LEFT, 0, 0);
    lv_label_set_text(lbl, "GPS hunt starting...");
    lv_timer_create(sniff_lv_cb, 700, NULL);
    bsp_display_unlock();

    ESP_LOGI("vmesh-sniff", "line sniffer up on %d pins", NPINS);
}
