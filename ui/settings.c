/* settings.c — S6: Wi-Fi join + offline map download.
 *
 * Polls the provision seam from an lv_timer (no cross-thread LVGL
 * calls). Opened from the S5 about overlay. The map download grabs
 * the area around the current map view center — pan somewhere, open
 * settings, tap download: tiles land on the card, no ribbon dance.
 */

#include <stdio.h>
#include <string.h>

#include "lvgl.h"

#include "map_view.h"
#include "hazard_store.h"
#include "provision.h"
#include "settings.h"
#include "scenario.h" /* demo-mode toggle */

/* persisted by the device target (NVS); weak no-op in the SDL sim */
void __attribute__((weak)) waycast_save_demo(bool on) { (void)on; }

static void demo_sw_cb(lv_event_t *e)
{
    bool on = lv_obj_has_state(lv_event_get_target(e), LV_STATE_CHECKED);
    scenario_set_demo(on);
    waycast_save_demo(on);
    /* flush any events either way: turning off clears lingering scripted
     * hazards from the real map; turning on starts the demo clean */
    hazard_store_clear(map_view_drop_chip);
}

#define NET_MAX 12
/* bbox half-spans around the view center (matches the device tileset
 * pads: ~4 x 3 km -> a few hundred tiles across z13..16) */

static lv_obj_t *s_ov, *s_status, *s_list, *s_scan_btn;
static lv_obj_t *s_dl_bar, *s_dl_lbl;
static lv_obj_t *s_fw_lbl, *s_fw_btn_lbl;
static lv_obj_t *s_kb, *s_pass_ta, *s_join_panel, *s_join_title;
static lv_timer_t *s_poll;
static char s_sel_ssid[33];
static bool s_scanning;

static void close_cb(lv_event_t *e)
{
    (void)e;
    if (s_poll) { lv_timer_delete(s_poll); s_poll = NULL; }
    if (s_ov) { lv_obj_delete(s_ov); s_ov = NULL; }
}

/* ---- join panel (password entry) ---- */

static void join_go_cb(lv_event_t *e)
{
    (void)e;
    const vmesh_wifi_ops_t *w = vmesh_wifi_ops();
    if (w && w->connect)
        w->connect(s_sel_ssid, lv_textarea_get_text(s_pass_ta));
    lv_obj_add_flag(s_join_panel, LV_OBJ_FLAG_HIDDEN);
}

static void join_cancel_cb(lv_event_t *e)
{
    (void)e;
    lv_obj_add_flag(s_join_panel, LV_OBJ_FLAG_HIDDEN);
}

static void net_click_cb(lv_event_t *e)
{
    const vmesh_wifi_net_t *net =
        (const vmesh_wifi_net_t *)lv_event_get_user_data(e);
    strncpy(s_sel_ssid, net->ssid, sizeof(s_sel_ssid) - 1);
    s_sel_ssid[sizeof(s_sel_ssid) - 1] = 0;

    const vmesh_wifi_ops_t *w = vmesh_wifi_ops();
    if (net->known && w) {
        /* re-prompt only if the stored password just failed here */
        char cur[33] = "";
        if (w->ssid) w->ssid(cur, sizeof(cur));
        char ip[20];
        bool failed_here =
            w->status(ip, sizeof(ip)) == VMESH_WIFI_FAILED &&
            strcmp(cur, net->ssid) == 0;
        if (!failed_here) {
            w->connect(net->ssid, NULL); /* stored credentials */
            return;
        }
    }
    lv_label_set_text_fmt(s_join_title, "Join \"%s\"", s_sel_ssid);
    lv_textarea_set_text(s_pass_ta, "");
    lv_obj_remove_flag(s_join_panel, LV_OBJ_FLAG_HIDDEN);
}

/* ---- scan ---- */

static void scan_cb(lv_event_t *e)
{
    (void)e;
    const vmesh_wifi_ops_t *w = vmesh_wifi_ops();
    if (!w || !w->scan_start) return;
    lv_obj_clean(s_list);
    lv_obj_t *l = lv_label_create(s_list);
    lv_label_set_text(l, "scanning...");
    lv_obj_set_style_text_color(l, lv_color_hex(0x868E96), 0);
    w->scan_start();
    s_scanning = true;
}

static void show_scan_results(void)
{
    static vmesh_wifi_net_t nets[NET_MAX]; /* names referenced by btns */
    const vmesh_wifi_ops_t *w = vmesh_wifi_ops();
    int n = w->scan_results(nets, NET_MAX);
    if (n < 0) return; /* still scanning */
    s_scanning = false;

    lv_obj_clean(s_list);
    if (n == 0) {
        lv_obj_t *l = lv_label_create(s_list);
        lv_label_set_text(l, "no networks found");
        lv_obj_set_style_text_color(l, lv_color_hex(0x868E96), 0);
        return;
    }
    for (int i = 0; i < n; i++) {
        lv_obj_t *btn = lv_button_create(s_list);
        lv_obj_set_size(btn, lv_pct(100), 48);
        lv_obj_set_style_bg_color(btn, lv_color_hex(0x22262E), 0);
        lv_obj_add_event_cb(btn, net_click_cb, LV_EVENT_CLICKED,
                            &nets[i]);
        lv_obj_t *l = lv_label_create(btn);
        lv_label_set_text_fmt(l, "%s %s  (%d dBm)%s",
                              nets[i].secured ? LV_SYMBOL_WIFI : LV_SYMBOL_WARNING,
                              nets[i].ssid, nets[i].rssi,
                              nets[i].known ? "   " LV_SYMBOL_OK " saved" : "");
        lv_obj_align(l, LV_ALIGN_LEFT_MID, 8, 0);
    }
}

/* ---- map download ---- */

/* tier buttons: coverage rings around the map center (which follows
 * the GPS pose while driving — so "around here") */
static void dl_tier_cb(lv_event_t *e)
{
    vmesh_tiles_tier_t tier =
        (vmesh_tiles_tier_t)(intptr_t)lv_event_get_user_data(e);
    const vmesh_tiledl_ops_t *t = vmesh_tiledl_ops();
    if (!t || !t->start_tier) return;
    double lat, lon;
    map_view_get_center(&lat, &lon);
    if (!t->start_tier(lat, lon, tier)) {
        lv_label_set_text(s_dl_lbl,
                          "can't start (need Wi-Fi + TF card)");
        return;
    }
    lv_obj_remove_flag(s_dl_bar, LV_OBJ_FLAG_HIDDEN);
    lv_label_set_text(s_dl_lbl, "starting...");
}

/* ---- firmware update (device only; sim registers no fw ops) ---- */

static void fw_btn_cb(lv_event_t *e)
{
    (void)e;
    const vmesh_fw_ops_t *f = vmesh_fw_ops();
    if (!f) return;
    if (f->state() == VMESH_FW_READY)
        f->reboot(); /* boot the staged image */
    else
        f->start();
}

/* ---- poll tick ---- */

static void poll_cb(lv_timer_t *t)
{
    (void)t;
    const vmesh_wifi_ops_t *w = vmesh_wifi_ops();
    if (w) {
        char ip[20] = "";
        vmesh_wifi_state_t st = w->status(ip, sizeof(ip));
        switch (st) {
        case VMESH_WIFI_CONNECTED:
            lv_label_set_text_fmt(s_status,
                LV_SYMBOL_WIFI "  connected  %s", ip);
            lv_obj_set_style_text_color(s_status, lv_color_hex(0x51CF66), 0);
            break;
        case VMESH_WIFI_CONNECTING:
            lv_label_set_text(s_status, LV_SYMBOL_REFRESH "  connecting...");
            lv_obj_set_style_text_color(s_status, lv_color_hex(0xF59F00), 0);
            break;
        case VMESH_WIFI_FAILED:
            lv_label_set_text(s_status,
                LV_SYMBOL_CLOSE "  join failed (check password)");
            lv_obj_set_style_text_color(s_status, lv_color_hex(0xFF6B6B), 0);
            break;
        default:
            lv_label_set_text(s_status, LV_SYMBOL_WIFI "  off");
            lv_obj_set_style_text_color(s_status, lv_color_hex(0x868E96), 0);
        }
        if (s_scanning) show_scan_results();
    }

    const vmesh_fw_ops_t *f = vmesh_fw_ops();
    if (f && s_fw_lbl) {
        const char *st = f->status();
        if (st && st[0])
            lv_label_set_text_fmt(s_fw_lbl, "firmware %s — %s",
                                  f->version(), st);
        else
            lv_label_set_text_fmt(s_fw_lbl, "firmware %s", f->version());
        lv_label_set_text(s_fw_btn_lbl,
                          f->state() == VMESH_FW_READY
                              ? LV_SYMBOL_REFRESH " Reboot now"
                              : LV_SYMBOL_DOWNLOAD " Check update");
    }

    const vmesh_tiledl_ops_t *td = vmesh_tiledl_ops();
    if (td) {
        int done = 0, total = 0;
        vmesh_tiledl_state_t st = td->progress(&done, &total);
        if (st == VMESH_TILEDL_RUNNING && total > 0) {
            lv_bar_set_range(s_dl_bar, 0, total);
            lv_bar_set_value(s_dl_bar, done, LV_ANIM_OFF);
            lv_label_set_text_fmt(s_dl_lbl, "%d / %d tiles", done, total);
        } else if (st == VMESH_TILEDL_DONE) {
            lv_bar_set_value(s_dl_bar, total ? total : 1, LV_ANIM_OFF);
            static bool healed;
            if (!healed) { map_view_retry_failed(); healed = true; }
            lv_label_set_text(s_dl_lbl, "map saved for offline use");
        } else if (st == VMESH_TILEDL_ERROR) {
            lv_label_set_text(s_dl_lbl, "download failed - retry later");
        }
    }
}

/* ---- build ---- */

void settings_open(void)
{
    if (s_ov) return;

    s_ov = lv_obj_create(lv_screen_active());
    lv_obj_set_size(s_ov, lv_pct(100), lv_pct(100));
    lv_obj_set_style_bg_color(s_ov, lv_color_hex(0x14161B), 0);
    lv_obj_set_style_border_width(s_ov, 0, 0);
    lv_obj_clear_flag(s_ov, LV_OBJ_FLAG_SCROLLABLE);

    lv_obj_t *title = lv_label_create(s_ov);
    lv_label_set_text(title, "Wi-Fi & offline maps");
    lv_obj_set_style_text_font(title, &lv_font_montserrat_28, 0);
    lv_obj_set_style_text_color(title, lv_color_white(), 0);
    lv_obj_align(title, LV_ALIGN_TOP_LEFT, 40, 24);

    s_status = lv_label_create(s_ov);
    lv_label_set_text(s_status, LV_SYMBOL_WIFI "  off");
    lv_obj_set_style_text_font(s_status, &lv_font_montserrat_16, 0);
    lv_obj_align(s_status, LV_ALIGN_TOP_LEFT, 40, 72);

    s_scan_btn = lv_button_create(s_ov);
    lv_obj_set_size(s_scan_btn, 180, 48);
    lv_obj_align(s_scan_btn, LV_ALIGN_TOP_RIGHT, -40, 60);
    lv_obj_set_style_bg_color(s_scan_btn, lv_color_hex(0x2B6CB0), 0);
    lv_obj_add_event_cb(s_scan_btn, scan_cb, LV_EVENT_CLICKED, NULL);
    lv_obj_t *sl = lv_label_create(s_scan_btn);
    lv_label_set_text(sl, LV_SYMBOL_REFRESH "  Scan");
    lv_obj_center(sl);

    /* network list (left half) */
    s_list = lv_obj_create(s_ov);
    lv_obj_set_size(s_list, lv_pct(46), lv_pct(52));
    lv_obj_align(s_list, LV_ALIGN_TOP_LEFT, 40, 110);
    lv_obj_set_style_bg_color(s_list, lv_color_hex(0x1A1D23), 0);
    lv_obj_set_style_border_width(s_list, 0, 0);
    lv_obj_set_flex_flow(s_list, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_style_pad_row(s_list, 6, 0);
    lv_obj_t *hint = lv_label_create(s_list);
    lv_label_set_text(hint, "tap " LV_SYMBOL_REFRESH " Scan to find\n"
                            "nearby networks");
    lv_obj_set_style_text_color(hint, lv_color_hex(0x868E96), 0);

    /* maps card (right half) */
    lv_obj_t *card = lv_obj_create(s_ov);
    lv_obj_set_size(card, lv_pct(42), lv_pct(52));
    lv_obj_align(card, LV_ALIGN_TOP_RIGHT, -40, 110);
    lv_obj_set_style_bg_color(card, lv_color_hex(0x1A1D23), 0);
    lv_obj_set_style_border_width(card, 0, 0);
    lv_obj_clear_flag(card, LV_OBJ_FLAG_SCROLLABLE);

    lv_obj_t *mt = lv_label_create(card);
    lv_label_set_text(mt, "Offline maps");
    lv_obj_set_style_text_font(mt, &lv_font_montserrat_20, 0);
    lv_obj_set_style_text_color(mt, lv_color_white(), 0);
    lv_obj_align(mt, LV_ALIGN_TOP_LEFT, 4, 4);

    lv_obj_t *mi = lv_label_create(card);
    lv_label_set_text(mi, "Grabs street maps around the\n"
                          "current map view onto the card\n"
                          "(zoom 13-16, ~4 x 3 km).");
    lv_obj_set_style_text_font(mi, &lv_font_montserrat_16, 0);
    lv_obj_set_style_text_color(mi, lv_color_hex(0xADB5BD), 0);
    lv_obj_align(mi, LV_ALIGN_TOP_LEFT, 4, 40);

    /* offline coverage tiers — ring downloads centered on the map */
    static const char *tier_txt[3] = {
        "Near\n15 km - 140 MB", "Region\n60 km - 330 MB",
        "Wide\n120 km - 1 GB",
    };
    for (int i = 0; i < 3; i++) {
        lv_obj_t *b = lv_button_create(card);
        lv_obj_set_size(b, lv_pct(29), 60);
        lv_obj_align(b, LV_ALIGN_BOTTOM_LEFT,
                     lv_pct(4 + i * 31), -66);
        lv_obj_set_style_bg_color(b, lv_color_hex(0x2F9E44), 0);
        lv_obj_add_event_cb(b, dl_tier_cb, LV_EVENT_CLICKED,
                            (void *)(intptr_t)i);
        lv_obj_t *l = lv_label_create(b);
        lv_label_set_text(l, tier_txt[i]);
        lv_obj_set_style_text_align(l, LV_TEXT_ALIGN_CENTER, 0);
        lv_obj_set_style_text_font(l, &lv_font_montserrat_14, 0);
        lv_obj_center(l);
    }

    /* firmware row — only when a target registered fw ops (device) */
    if (vmesh_fw_ops()) {
        s_fw_lbl = lv_label_create(card);
        lv_label_set_text(s_fw_lbl, "firmware");
        lv_obj_set_style_text_font(s_fw_lbl, &lv_font_montserrat_14, 0);
        lv_obj_set_style_text_color(s_fw_lbl, lv_color_hex(0xADB5BD), 0);
        lv_obj_align(s_fw_lbl, LV_ALIGN_BOTTOM_LEFT, lv_pct(4), -146);

        lv_obj_t *fb = lv_button_create(card);
        lv_obj_set_size(fb, lv_pct(29), 40);
        lv_obj_align(fb, LV_ALIGN_BOTTOM_RIGHT, lv_pct(-4), -138);
        lv_obj_set_style_bg_color(fb, lv_color_hex(0x1971C2), 0);
        lv_obj_add_event_cb(fb, fw_btn_cb, LV_EVENT_CLICKED, NULL);
        s_fw_btn_lbl = lv_label_create(fb);
        lv_label_set_text(s_fw_btn_lbl, LV_SYMBOL_DOWNLOAD " Check update");
        lv_obj_set_style_text_font(s_fw_btn_lbl, &lv_font_montserrat_14, 0);
        lv_obj_center(s_fw_btn_lbl);
    } else {
        s_fw_lbl = NULL;
    }

    s_dl_bar = lv_bar_create(card);
    lv_obj_set_size(s_dl_bar, lv_pct(90), 12);
    lv_obj_align(s_dl_bar, LV_ALIGN_BOTTOM_MID, 0, -44);
    lv_obj_add_flag(s_dl_bar, LV_OBJ_FLAG_HIDDEN);

    s_dl_lbl = lv_label_create(card);
    lv_label_set_text(s_dl_lbl, "");
    lv_obj_set_style_text_font(s_dl_lbl, &lv_font_montserrat_16, 0);
    lv_obj_set_style_text_color(s_dl_lbl, lv_color_hex(0xADB5BD), 0);
    lv_obj_align(s_dl_lbl, LV_ALIGN_BOTTOM_MID, 0, -14);

    /* demo-data toggle (bottom-left): off = real GPS + real radio only;
     * on = replay the scripted town for showcasing */
    lv_obj_t *demo_lbl = lv_label_create(s_ov);
    lv_label_set_text(demo_lbl, "Demo data");
    lv_obj_set_style_text_font(demo_lbl, &lv_font_montserrat_16, 0);
    lv_obj_set_style_text_color(demo_lbl, lv_color_hex(0xADB5BD), 0);
    lv_obj_align(demo_lbl, LV_ALIGN_BOTTOM_LEFT, 40, -30);
    lv_obj_t *demo_sw = lv_switch_create(s_ov);
    lv_obj_align_to(demo_sw, demo_lbl, LV_ALIGN_OUT_RIGHT_MID, 16, 0);
    if (scenario_demo()) lv_obj_add_state(demo_sw, LV_STATE_CHECKED);
    lv_obj_add_event_cb(demo_sw, demo_sw_cb, LV_EVENT_VALUE_CHANGED, NULL);

    /* close */
    lv_obj_t *close = lv_button_create(s_ov);
    lv_obj_set_size(close, lv_pct(24), 52);
    lv_obj_align(close, LV_ALIGN_BOTTOM_MID, 0, -16);
    lv_obj_set_style_bg_color(close, lv_color_hex(0x22262E), 0);
    lv_obj_add_event_cb(close, close_cb, LV_EVENT_CLICKED, NULL);
    lv_obj_t *cl = lv_label_create(close);
    lv_label_set_text(cl, "Close");
    lv_obj_center(cl);

    /* join panel: password + keyboard, hidden until a net is tapped */
    s_join_panel = lv_obj_create(s_ov);
    lv_obj_set_size(s_join_panel, lv_pct(100), lv_pct(100));
    lv_obj_set_style_bg_color(s_join_panel, lv_color_hex(0x14161B), 0);
    lv_obj_set_style_bg_opa(s_join_panel, LV_OPA_COVER, 0);
    lv_obj_set_style_border_width(s_join_panel, 0, 0);
    lv_obj_clear_flag(s_join_panel, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_add_flag(s_join_panel, LV_OBJ_FLAG_HIDDEN);

    s_join_title = lv_label_create(s_join_panel);
    lv_label_set_text(s_join_title, "Join");
    lv_obj_set_style_text_font(s_join_title, &lv_font_montserrat_28, 0);
    lv_obj_set_style_text_color(s_join_title, lv_color_white(), 0);
    lv_obj_align(s_join_title, LV_ALIGN_TOP_LEFT, 40, 24);

    s_pass_ta = lv_textarea_create(s_join_panel);
    lv_obj_set_size(s_pass_ta, lv_pct(60), 52);
    lv_obj_align(s_pass_ta, LV_ALIGN_TOP_LEFT, 40, 76);
    lv_textarea_set_one_line(s_pass_ta, true);
    lv_textarea_set_password_mode(s_pass_ta, true);
    lv_textarea_set_placeholder_text(s_pass_ta, "password");

    lv_obj_t *join = lv_button_create(s_join_panel);
    lv_obj_set_size(join, lv_pct(14), 52);
    lv_obj_align(join, LV_ALIGN_TOP_RIGHT, -230, 76);
    lv_obj_set_style_bg_color(join, lv_color_hex(0x2F9E44), 0);
    lv_obj_add_event_cb(join, join_go_cb, LV_EVENT_CLICKED, NULL);
    lv_obj_t *jl = lv_label_create(join);
    lv_label_set_text(jl, "Join");
    lv_obj_center(jl);

    lv_obj_t *jc = lv_button_create(s_join_panel);
    lv_obj_set_size(jc, lv_pct(12), 52);
    lv_obj_align(jc, LV_ALIGN_TOP_RIGHT, -40, 76);
    lv_obj_set_style_bg_color(jc, lv_color_hex(0x22262E), 0);
    lv_obj_add_event_cb(jc, join_cancel_cb, LV_EVENT_CLICKED, NULL);
    lv_obj_t *jcl = lv_label_create(jc);
    lv_label_set_text(jcl, "Back");
    lv_obj_center(jcl);

    s_kb = lv_keyboard_create(s_join_panel);
    lv_keyboard_set_textarea(s_kb, s_pass_ta);
    lv_obj_set_size(s_kb, lv_pct(100), lv_pct(55));

    s_poll = lv_timer_create(poll_cb, 400, NULL);
}

/* dev hook: show the join panel without a scan (sim screenshots) */
void settings_debug_show_join(const char *ssid)
{
    if (!s_ov) return;
    strncpy(s_sel_ssid, ssid, sizeof(s_sel_ssid) - 1);
    lv_label_set_text_fmt(s_join_title, "Join \"%s\"", s_sel_ssid);
    lv_obj_remove_flag(s_join_panel, LV_OBJ_FLAG_HIDDEN);
}

/* dev hook: kick a scan as if Scan was tapped (sim screenshots) */
void settings_debug_scan(void) { if (s_ov) scan_cb(NULL); }
