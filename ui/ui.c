#include "ui.h"
#include "feed.h"
#include "hazard_store.h"
#include "names.h"
#include "convoy.h"
#include "vmesh_mesh.h" /* vmesh_clock_normalize */
#include <string.h>

#include "map_view.h"
#include "settings.h"
#include "provision.h"
#include "town_square.h"
#include "scenario.h" /* phase 0 only: scenario_make_own_report()      */
                      /* (the one place UI touches sim; swaps with GPS) */

#include "lvgl.h"

#include <math.h>
#include <stdio.h>

/* Split layout (PRODUCT_PLAN.md S1, revised): the map gives spatial
 * context; the info column gives the driver-glanceable answer — what's
 * ahead, how far, and a big fixed Report button that never moves. */
#define MAP_PCT 55

static lv_obj_t *status_bar, *lbl_radios, *lbl_wifi, *lbl_clock;
static lv_obj_t *lbl_town;      /* anchor-presence indicator */
static uint32_t s_anchor_heard_ms; /* lv_tick of last beacon */
static char s_anchor_name[24] = "town"; /* from the beacon note */
static lv_obj_t *info_panel, *lbl_speed_big;
static lv_obj_t *card, *card_kicker, *card_title, *card_dist, *card_sub;
static lv_obj_t *lbl_also;
#define LIST_ROWS 3
static lv_obj_t *list_row[LIST_ROWS];
/* LOCAL channel (plan §7.5): dim counter while moving, browsable when
 * stopped — chatter must never compete with "CRASH 400 m" */
#define LOCAL_ROWS 3
#define PARKED_MPS 2.5f
static lv_obj_t *lbl_local, *local_row[LOCAL_ROWS];
static lv_obj_t *report_btn;
static lv_obj_t *report_overlay; /* S2 */
static lv_obj_t *detail_panel;   /* S3 */
static lv_obj_t *about_overlay;  /* S5 */
static lv_obj_t *lbl_convoy;     /* status-bar convoy chip */
static char s_convoy_ticker[40]; /* latest group message, 60 s */
static uint32_t s_convoy_ticker_ms;
static lv_obj_t *convoy_sheet;   /* member list + quick words */

#define VMESH_FW_VERSION "0.1.0-phase0"

static hazard_t *nearest_hz; /* hazard shown on the NEXT AHEAD card */

static lv_color_t severity_color(uint8_t sev)
{
    switch (sev) {
    case 3:  return lv_color_hex(0xE03131);
    case 2:  return lv_color_hex(0xF59F00);
    default: return lv_color_hex(0x5C7CFA);
    }
}

static double hz_distance_m(const hazard_t *h, const vmesh_pose_t *pose)
{
    double dy = (h->msg.lat_e7 / 1e7 - pose->lat) * 110540.0;
    double dx = (h->msg.lon_e7 / 1e7 - pose->lon) * 111320.0 *
                cos(pose->lat * M_PI / 180.0);
    return sqrt(dx * dx + dy * dy);
}

static void fmt_dist(char *buf, size_t n, double m)
{
    if (m < 950) snprintf(buf, n, "%d m", (int)(m / 10) * 10);
    else         snprintf(buf, n, "%.1f km", m / 1000.0);
}

/* ---------------- S3: hazard detail ---------------- */

static void detail_close_cb(lv_event_t *e)
{
    (void)e;
    if (detail_panel) {
        lv_obj_delete(detail_panel);
        detail_panel = NULL;
    }
}

static hazard_t *detail_hz; /* hazard shown in the open detail sheet */
static void show_detail(hazard_t *h);

/* ---- Tier 2.5: ask-a-place (parked Q&A over the mesh) ---- */
static lv_obj_t *qa_answer_lbl;     /* answer line in the open detail */
static uint32_t qa_pending_origin;  /* our question awaiting a reply */
static uint16_t qa_pending_seq;

static void qa_ask(hazard_t *h, const char *q)
{
    vmesh_msg_t m;
    scenario_make_own_query(&m, h->msg.origin_id, q);
    vmesh_feed_publish(&m);
    qa_pending_origin = m.origin_id;
    qa_pending_seq = m.seq;
    if (qa_answer_lbl) {
        lv_label_set_text_fmt(qa_answer_lbl,
                              LV_SYMBOL_UPLOAD "  \"%s\" - asking...", q);
        lv_obj_set_style_text_color(qa_answer_lbl,
                                    lv_color_hex(0x868E96), 0);
    }
}

static void ask_chip_cb(lv_event_t *e)
{
    if (!detail_hz) return;
    qa_ask(detail_hz, (const char *)lv_event_get_user_data(e));
}

/* demo choreography: each lap's parked beat auto-runs one Q&A with
 * the nearest place — the device demos haiku commerce by itself.
 * Never fights the user: fires once per lap, only with no panel open. */
static bool qa_demo_done;
static float qa_parked_since = -1.0f;

static void qa_demo_tick(const vmesh_pose_t *pose)
{
    float clk = scenario_clock_s();
    static float prev_clk;
    if (clk < prev_clk - 1.0f) qa_demo_done = false; /* lap wrapped */
    prev_clk = clk;
    if (qa_demo_done) return;

    if (pose->speed_mps > 1.0f) { qa_parked_since = -1.0f; return; }
    if (qa_parked_since < 0) { qa_parked_since = clk; return; }
    if (clk - qa_parked_since < 4.0f || detail_panel) return;

    hazard_t *best = NULL;
    double best_d = 1500.0; /* only places actually around us */
    for (int i = 0; i < HAZARD_MAX; i++) {
        hazard_t *h = hazard_store_slot(i);
        if (!h || !h->active || h->msg.channel != VMESH_CH_LOCAL)
            continue;
        double d = hz_distance_m(h, pose);
        if (d < best_d) { best_d = d; best = h; }
    }
    if (!best) return;
    qa_demo_done = true;
    show_detail(best);
    qa_ask(best, "today?");
}

static void vote_cb(lv_event_t *e)
{
    uint8_t verdict = (uint8_t)(uintptr_t)lv_event_get_user_data(e);
    if (!detail_hz || !detail_hz->active || detail_hz->my_vote) return;

    vmesh_msg_t m;
    scenario_make_own_attest(&m, &detail_hz->msg, verdict);
    vmesh_feed_publish(&m); /* loops back -> tallies update via ingest */
    detail_hz->my_vote = (verdict == VMESH_VERDICT_CONFIRM) ? 1 : -1;

    hazard_t *h = detail_hz;
    detail_close_cb(NULL); /* rebuild the sheet with updated counts */
    show_detail(h);
}

static void show_detail(hazard_t *h)
{
    detail_close_cb(NULL);
    detail_hz = h;
    qa_answer_lbl = NULL;

    vmesh_pose_t pose;
    vmesh_pose_get(&pose);
    uint32_t now = vmesh_time_s();

    int dist_m = (int)hz_distance_m(h, &pose);
    int age_s = (int)(now - h->msg.created_s);
    int left_s = (int)(h->msg.created_s + h->msg.ttl_s - now);

    detail_panel = lv_obj_create(lv_screen_active());
    lv_obj_set_size(detail_panel, lv_pct(60), LV_SIZE_CONTENT);
    lv_obj_align(detail_panel, LV_ALIGN_BOTTOM_MID, 0, -16);
    lv_obj_set_style_bg_color(detail_panel, lv_color_hex(0x22262E), 0);
    lv_obj_set_style_border_color(detail_panel, lv_color_hex(0x3B4252), 0);
    lv_obj_set_style_radius(detail_panel, 12, 0);
    lv_obj_add_event_cb(detail_panel, detail_close_cb, LV_EVENT_CLICKED, NULL);

    lv_obj_t *title = lv_label_create(detail_panel);
    if (h->msg.channel == VMESH_CH_GROUP)
        lv_label_set_text(title, "Convoy message");
    else if (h->msg.channel == VMESH_CH_LOCAL)
        lv_label_set_text_fmt(title, "%s  (local notice)",
                              vmesh_msg_name(h->msg.channel,
                                             h->msg.hazard_type));
    else
        lv_label_set_text_fmt(title, "%s  (severity %d)",
                              vmesh_hz_name(h->msg.hazard_type),
                              h->msg.severity);
    lv_obj_set_style_text_font(title, &lv_font_montserrat_20, 0);
    lv_obj_set_style_text_color(title, lv_color_white(), 0);

    lv_obj_t *body = lv_label_create(detail_panel);
    const char *who = names_get(h->msg.origin_id, now);
    char wholine[32] = "";
    if (who)
        snprintf(wholine, sizeof(wholine), "\nreported by %s", who);
    lv_label_set_text_fmt(body,
        "%s\n%d m away - reported %d min %02d s ago\n"
        "expires in %d min - heard at hop %d%s%s",
        h->msg.note[0] ? h->msg.note : "(no details)",
        dist_m, age_s / 60, age_s % 60,
        left_s > 0 ? left_s / 60 : 0, h->msg.hops,
        wholine,
        h->echoed ? "\n" LV_SYMBOL_OK " heard by town" : "");
    lv_obj_set_style_text_color(body, lv_color_hex(0xADB5BD), 0);
    lv_obj_align_to(body, title, LV_ALIGN_OUT_BOTTOM_LEFT, 0, 8);

    /* Tier 2.5: ask the place (its beacon answers over the mesh) */
    lv_obj_t *chips_anchor = body;
    if (h->msg.channel == VMESH_CH_LOCAL) {
        static const char *qs[] = { "hours?", "wait?", "today?" };
        lv_obj_t *prev = NULL;
        for (int i = 0; i < 3; i++) {
            lv_obj_t *c = lv_button_create(detail_panel);
            lv_obj_set_size(c, lv_pct(29), 46);
            if (!prev)
                lv_obj_align_to(c, body, LV_ALIGN_OUT_BOTTOM_LEFT, 0, 10);
            else
                lv_obj_align_to(c, prev, LV_ALIGN_OUT_RIGHT_MID, 10, 0);
            lv_obj_set_style_bg_color(c, lv_color_hex(0x1B4332), 0);
            lv_obj_add_event_cb(c, ask_chip_cb, LV_EVENT_CLICKED,
                                (void *)qs[i]);
            lv_obj_t *cl = lv_label_create(c);
            lv_label_set_text_fmt(cl, LV_SYMBOL_GPS " %s", qs[i]);
            lv_obj_set_style_text_font(cl, &lv_font_montserrat_16, 0);
            lv_obj_center(cl);
            prev = c;
        }
        qa_answer_lbl = lv_label_create(detail_panel);
        lv_label_set_text(qa_answer_lbl, "ask the place - it answers "
                                         "over the mesh");
        lv_obj_set_style_text_font(qa_answer_lbl,
                                   &lv_font_montserrat_16, 0);
        lv_obj_set_style_text_color(qa_answer_lbl,
                                    lv_color_hex(0x868E96), 0);
        lv_obj_align_to(qa_answer_lbl, detail_panel,
                        LV_ALIGN_TOP_LEFT, 0, 0); /* re-anchored below */
        /* anchor answer under the first chip row */
        lv_obj_update_layout(detail_panel);
        lv_obj_align_to(qa_answer_lbl, body,
                        LV_ALIGN_OUT_BOTTOM_LEFT, 0, 66);
        chips_anchor = qa_answer_lbl;
    }

    /* corroboration (§7¾): what the mesh says, and my say */
    lv_obj_t *tally = lv_label_create(detail_panel);
    if (h->confirms || h->denies)
        lv_label_set_text_fmt(tally, "%d confirmed - %d disputed%s",
                              h->confirms, h->denies,
                              h->my_vote > 0   ? "  (incl. you)"
                              : h->my_vote < 0 ? "  (you disputed)"
                                               : "");
    else
        lv_label_set_text(tally, h->my_vote ? "your vote is out"
                                            : "no corroboration yet");
    lv_obj_set_style_text_color(tally,
        (h->denies > h->confirms) ? lv_color_hex(0xF59F00)
                                  : lv_color_hex(0x51CF66), 0);
    lv_obj_align_to(tally, chips_anchor, LV_ALIGN_OUT_BOTTOM_LEFT, 0, 10);

    if (!h->my_vote) {
        lv_obj_t *yes = lv_button_create(detail_panel);
        lv_obj_set_size(yes, lv_pct(44), 54);
        lv_obj_align_to(yes, tally, LV_ALIGN_OUT_BOTTOM_LEFT, 0, 10);
        lv_obj_set_style_bg_color(yes, lv_color_hex(0x2B8A3E), 0);
        lv_obj_add_event_cb(yes, vote_cb, LV_EVENT_CLICKED,
                            (void *)(uintptr_t)VMESH_VERDICT_CONFIRM);
        lv_obj_t *yl = lv_label_create(yes);
        lv_label_set_text(yl, LV_SYMBOL_OK "  Confirmed");
        lv_obj_center(yl);

        lv_obj_t *no = lv_button_create(detail_panel);
        lv_obj_set_size(no, lv_pct(44), 54);
        lv_obj_align_to(no, yes, LV_ALIGN_OUT_RIGHT_MID, 14, 0);
        lv_obj_set_style_bg_color(no, lv_color_hex(0x66341A), 0);
        lv_obj_add_event_cb(no, vote_cb, LV_EVENT_CLICKED,
                            (void *)(uintptr_t)VMESH_VERDICT_DENY);
        lv_obj_t *nl = lv_label_create(no);
        lv_label_set_text(nl, LV_SYMBOL_CLOSE "  Not there");
        lv_obj_center(nl);
    }
}

/* ---------------- toast ---------------- */

static void toast_del_cb(lv_timer_t *t)
{
    lv_obj_delete(lv_timer_get_user_data(t));
    lv_timer_delete(t);
}

static void toast(const char *text)
{
    lv_obj_t *tst = lv_label_create(lv_layer_top());
    lv_label_set_text(tst, text);
    lv_obj_set_style_bg_color(tst, lv_color_hex(0x2B8A3E), 0);
    lv_obj_set_style_bg_opa(tst, LV_OPA_COVER, 0);
    lv_obj_set_style_text_color(tst, lv_color_white(), 0);
    lv_obj_set_style_pad_all(tst, 12, 0);
    lv_obj_set_style_radius(tst, 8, 0);
    lv_obj_align(tst, LV_ALIGN_BOTTOM_MID, 0, -100);
    lv_timer_t *t = lv_timer_create(toast_del_cb, 3000, tst);
    lv_timer_set_repeat_count(t, 1);
}

/* ---------------- S2: two-tap report flow ---------------- */
/* Tap 1 = the Report button. Tap 2 = one of four large type buttons.
 * TODO(plan §2/S2): 5-second undo on the toast. */

static const uint8_t REPORT_TYPES[4] = {
    VMESH_HZ_CRASH, VMESH_HZ_SLOWDOWN, VMESH_HZ_DEBRIS, VMESH_HZ_WEATHER,
};

static void report_close(void)
{
    if (report_overlay) {
        lv_obj_delete(report_overlay);
        report_overlay = NULL;
    }
}

static void report_pick_cb(lv_event_t *e)
{
    uint8_t hz = (uint8_t)(uintptr_t)lv_event_get_user_data(e);
    vmesh_msg_t m;
    scenario_make_own_report(&m, hz);
    vmesh_feed_publish(&m);
    report_close();
    toast("Reported - broadcasting to nearby vehicles");
}

static void report_cancel_cb(lv_event_t *e)
{
    (void)e;
    report_close();
}

static void report_open_cb(lv_event_t *e)
{
    (void)e;
    if (report_overlay) return;

    report_overlay = lv_obj_create(lv_screen_active());
    lv_obj_set_size(report_overlay, lv_pct(100), lv_pct(100));
    lv_obj_set_style_bg_color(report_overlay, lv_color_hex(0x14161B), 0);
    lv_obj_set_style_bg_opa(report_overlay, LV_OPA_90, 0);
    lv_obj_set_style_border_width(report_overlay, 0, 0);
    lv_obj_clear_flag(report_overlay, LV_OBJ_FLAG_SCROLLABLE);

    lv_obj_t *title = lv_label_create(report_overlay);
    lv_label_set_text(title, "Report what?");
    lv_obj_set_style_text_font(title, &lv_font_montserrat_28, 0);
    lv_obj_set_style_text_color(title, lv_color_white(), 0);
    lv_obj_align(title, LV_ALIGN_TOP_MID, 0, 40);

    /* four large driver-safe buttons, 2x2 */
    for (int i = 0; i < 4; i++) {
        uint8_t hz = REPORT_TYPES[i];
        lv_obj_t *btn = lv_button_create(report_overlay);
        lv_obj_set_size(btn, lv_pct(44), 160);
        lv_obj_align(btn, LV_ALIGN_CENTER,
                     (i % 2 == 0) ? lv_pct(-24) : lv_pct(24),
                     (i / 2 == 0) ? -110 : 80);
        lv_obj_set_style_bg_color(btn, lv_color_hex(0x343A46), 0);
        lv_obj_set_style_radius(btn, 16, 0);
        lv_obj_add_event_cb(btn, report_pick_cb, LV_EVENT_CLICKED,
                            (void *)(uintptr_t)hz);
        lv_obj_t *lbl = lv_label_create(btn);
        lv_label_set_text(lbl, vmesh_hz_name(hz));
        lv_obj_set_style_text_font(lbl, &lv_font_montserrat_20, 0);
        lv_obj_center(lbl);
    }

    lv_obj_t *cancel = lv_button_create(report_overlay);
    lv_obj_set_size(cancel, lv_pct(60), 56);
    lv_obj_align(cancel, LV_ALIGN_BOTTOM_MID, 0, -30);
    lv_obj_set_style_bg_color(cancel, lv_color_hex(0x22262E), 0);
    lv_obj_add_event_cb(cancel, report_cancel_cb, LV_EVENT_CLICKED, NULL);
    lv_obj_t *cl = lv_label_create(cancel);
    lv_label_set_text(cl, "Cancel");
    lv_obj_center(cl);
}

/* wrappers for town_square.c (S2): reuse this file's static overlays */
void ui_show_detail(hazard_t *h) { show_detail(h); }
void ui_open_report(void) { report_open_cb(NULL); }

/* ---------------- S5: about / radio disclosure ----------------
 * The §3 trust story as UI: the user can WATCH the off-grid guarantee
 * hold. Every radio the hardware has, and its state, on demand. */

static void about_close_cb(lv_event_t *e)
{
    (void)e;
    if (about_overlay) {
        lv_obj_delete(about_overlay);
        about_overlay = NULL;
    }
}

static lv_obj_t *about_line(lv_obj_t *parent, int32_t y, const char *txt,
                            uint32_t color)
{
    lv_obj_t *l = lv_label_create(parent);
    lv_label_set_text(l, txt);
    lv_obj_set_style_text_font(l, &lv_font_montserrat_16, 0);
    lv_obj_set_style_text_color(l, lv_color_hex(color), 0);
    lv_obj_align(l, LV_ALIGN_TOP_LEFT, 40, y);
    return l;
}

static void about_open_cb(lv_event_t *e)
{
    (void)e;
    if (about_overlay) return;

    about_overlay = lv_obj_create(lv_screen_active());
    lv_obj_set_size(about_overlay, lv_pct(100), lv_pct(100));
    lv_obj_set_style_bg_color(about_overlay, lv_color_hex(0x14161B), 0);
    lv_obj_set_style_border_width(about_overlay, 0, 0);
    lv_obj_clear_flag(about_overlay, LV_OBJ_FLAG_SCROLLABLE);

    lv_obj_t *title = lv_label_create(about_overlay);
    lv_label_set_text(title, "This device's radios");
    lv_obj_set_style_text_font(title, &lv_font_montserrat_28, 0);
    lv_obj_set_style_text_color(title, lv_color_white(), 0);
    lv_obj_align(title, LV_ALIGN_TOP_LEFT, 40, 28);

    about_line(about_overlay, 90,
               LV_SYMBOL_WIFI "  SIM feed - active (simulated mesh, "
               "phase 0)", 0x51CF66);
    about_line(about_overlay, 126,
               "LoRa (SX1262, US915) - not fitted yet", 0xF59F00);
    about_line(about_overlay, 162, "Bluetooth LE - off", 0x868E96);
    {
        const vmesh_wifi_ops_t *w = vmesh_wifi_ops();
        char ip[20] = "", txt[80];
        vmesh_wifi_state_t ws =
            w ? w->status(ip, sizeof(ip)) : VMESH_WIFI_OFF;
        if (ws == VMESH_WIFI_CONNECTED)
            snprintf(txt, sizeof(txt),
                     "Wi-Fi - on, %s (user-enabled: maps/updates)", ip);
        else
            snprintf(txt, sizeof(txt),
                     "Wi-Fi - off (user-enabled for updates only)");
        about_line(about_overlay, 198, txt,
                   ws == VMESH_WIFI_CONNECTED ? 0x51CF66 : 0x868E96);
    }
    about_line(about_overlay, 252,
               "Cellular - NONE. No modem, no SIM slot, no carrier "
               "path.", 0x51CF66);
    about_line(about_overlay, 282,
               "Everything this device knows arrived peer-to-peer.",
               0x51CF66);

    about_line(about_overlay, 336,
               "Region profile: US915 (FCC Part 15)", 0xADB5BD);
    {   /* real version from the fw seam (device); sim falls back */
        const vmesh_fw_ops_t *f = vmesh_fw_ops();
        char fwl[64];
        snprintf(fwl, sizeof(fwl), "Firmware: vmesh %s - open source",
                 f ? f->version() : VMESH_FW_VERSION);
        about_line(about_overlay, 368, fwl, 0xADB5BD);
    }

    lv_obj_t *setb = lv_button_create(about_overlay);
    lv_obj_set_size(setb, lv_pct(30), 56);
    lv_obj_align(setb, LV_ALIGN_BOTTOM_LEFT, 40, -28);
    lv_obj_set_style_bg_color(setb, lv_color_hex(0x2B6CB0), 0);
    lv_obj_add_event_cb(setb, (lv_event_cb_t)settings_open,
                        LV_EVENT_CLICKED, NULL);
    lv_obj_t *sbl = lv_label_create(setb);
    lv_label_set_text(sbl, LV_SYMBOL_WIFI "  Wi-Fi & maps");
    lv_obj_center(sbl);

    lv_obj_t *close = lv_button_create(about_overlay);
    lv_obj_set_size(close, lv_pct(30), 56);
    lv_obj_align(close, LV_ALIGN_BOTTOM_RIGHT, -40, -28);
    lv_obj_set_style_bg_color(close, lv_color_hex(0x22262E), 0);
    lv_obj_add_event_cb(close, about_close_cb, LV_EVENT_CLICKED, NULL);
    lv_obj_t *cl = lv_label_create(close);
    lv_label_set_text(cl, "Close");
    lv_obj_center(cl);
}

/* dev hook: open the first LOCAL place's detail and ask a question
 * through the real publish path (sim --open qa screenshots) */
bool ui_debug_ask(void)
{
    for (int i = 0; i < HAZARD_MAX; i++) {
        hazard_t *h = hazard_store_slot(i);
        if (!h || !h->active || h->msg.channel != VMESH_CH_LOCAL)
            continue;
        show_detail(h);
        qa_ask(h, "today?");
        return true;
    }
    return false;
}

/* dev hook: open S5 directly (sim --open s5 screenshots) */
void ui_open_about(void) { about_open_cb(NULL); }

/* ---------------- info panel ---------------- */

static void card_click_cb(lv_event_t *e)
{
    (void)e;
    if (nearest_hz && nearest_hz->active) show_detail(nearest_hz);
}

/* parked LOCAL header tap -> the town square; while moving, the
 * request routes through the passenger-consent modal */
static void local_header_cb(lv_event_t *e)
{
    (void)e;
    town_square_request_open();
}

static void gear_btn_cb(lv_event_t *e)
{
    (void)e;
    settings_open();
}

static void square_btn_cb(lv_event_t *e)
{
    (void)e;
    town_square_request_open();
}

static void update_info_panel(const vmesh_pose_t *pose, uint32_t now)
{
    /* collect active messages sorted by distance, split by channel
     * (static: 2KB that must NOT live on the LVGL task stack) */
    static struct { hazard_t *h; double d; } act[HAZARD_MAX], loc[HAZARD_MAX];
    int n = 0, nl = 0;
    for (int i = 0; i < HAZARD_MAX; i++) {
        hazard_t *h = hazard_store_slot(i);
        if (!h || !h->active) continue;
        double d = hz_distance_m(h, pose);
        if (h->msg.channel == VMESH_CH_LOCAL) {
            loc[nl].h = h;
            loc[nl].d = d;
            nl++;
        } else {
            act[n].h = h;
            act[n].d = d;
            n++;
        }
    }
    for (int i = 1; i < n; i++) { /* insertion sort; n is tiny */
        int j = i;
        while (j > 0 && act[j].d < act[j - 1].d) {
            __typeof__(act[0]) tmp = act[j];
            act[j] = act[j - 1];
            act[j - 1] = tmp;
            j--;
        }
    }
    for (int i = 1; i < nl; i++) {
        int j = i;
        while (j > 0 && loc[j].d < loc[j - 1].d) {
            __typeof__(loc[0]) tmp = loc[j];
            loc[j] = loc[j - 1];
            loc[j - 1] = tmp;
            j--;
        }
    }

    /* NEXT AHEAD card */
    nearest_hz = n ? act[0].h : NULL;
    if (nearest_hz) {
        char db[16];
        fmt_dist(db, sizeof(db), act[0].d);
        int age_s = (int)(now - nearest_hz->msg.created_s);

        char tally[40] = "";
        if (nearest_hz->denies > nearest_hz->confirms)
            snprintf(tally, sizeof(tally), " - DISPUTED (%d)",
                     nearest_hz->denies);
        else if (nearest_hz->confirms)
            snprintf(tally, sizeof(tally), " - %d confirmed",
                     nearest_hz->confirms);
        lv_label_set_text(card_title, vmesh_hz_name(nearest_hz->msg.hazard_type));
        lv_label_set_text(card_dist, db);
        lv_label_set_text_fmt(card_sub, "%s\nreported %d min %02d s ago%s",
                              nearest_hz->msg.note[0] ? nearest_hz->msg.note
                                                      : "-",
                              age_s / 60, age_s % 60, tally);

        lv_color_t c = severity_color(nearest_hz->msg.severity);
        lv_obj_set_style_border_color(card, c, 0);
        lv_obj_set_style_bg_color(card, lv_color_mix(c, lv_color_hex(0x191C22),
                                                     50),
                                  0);
        float age = hazard_age_frac(nearest_hz, now);
        lv_obj_set_style_opa(card, (lv_opa_t)(255 - (int)(age * 130)), 0);
        lv_label_set_text(card_kicker, "NEXT AHEAD");
    } else {
        lv_label_set_text(card_kicker, "ALL CLEAR");
        lv_label_set_text(card_title, "No hazards nearby");
        lv_label_set_text(card_dist, "");
        lv_label_set_text(card_sub, "listening on the mesh...");
        lv_obj_set_style_border_color(card, lv_color_hex(0x2B8A3E), 0);
        lv_obj_set_style_bg_color(card, lv_color_hex(0x191C22), 0);
        lv_obj_set_style_opa(card, LV_OPA_COVER, 0);
    }

    /* the rest of the list */
    lv_obj_set_style_opa(lbl_also, n > 1 ? LV_OPA_COVER : LV_OPA_40, 0);
    for (int r = 0; r < LIST_ROWS; r++) {
        if (r + 1 < n) {
            char db[16];
            fmt_dist(db, sizeof(db), act[r + 1].d);
            lv_label_set_text_fmt(list_row[r], "%s %s - %s",
                                  LV_SYMBOL_WARNING,
                                  vmesh_hz_name(act[r + 1].h->msg.hazard_type),
                                  db);
            lv_obj_set_style_text_color(
                list_row[r], severity_color(act[r + 1].h->msg.severity), 0);
            float age = hazard_age_frac(act[r + 1].h, now);
            lv_obj_set_style_opa(list_row[r],
                                 (lv_opa_t)(255 - (int)(age * 165)), 0);
        } else {
            lv_label_set_text(list_row[r], "");
        }
    }

    /* LOCAL channel: speed-gated. Moving = a dim counter only;
     * stopped = the notices themselves. */
    bool parked = pose->speed_mps < PARKED_MPS;
    if (nl == 0) {
        lv_label_set_text(lbl_local, "LOCAL - quiet");
        lv_obj_set_style_opa(lbl_local, LV_OPA_40, 0);
    } else if (!parked) {
        lv_label_set_text_fmt(lbl_local,
                              "LOCAL - %d notice%s (stop for the square)",
                              nl, nl == 1 ? "" : "s");
        lv_obj_set_style_opa(lbl_local, LV_OPA_60, 0);
    } else {
        lv_label_set_text_fmt(lbl_local,
                              LV_SYMBOL_HOME "  TOWN SQUARE - %d note%s - "
                              "tap to open", nl, nl == 1 ? "" : "s");
        lv_obj_set_style_opa(lbl_local, LV_OPA_COVER, 0);
    }
    for (int r = 0; r < LOCAL_ROWS; r++) {
        if (parked && r < nl) {
            char db[16];
            fmt_dist(db, sizeof(db), loc[r].d);
            lv_label_set_text_fmt(local_row[r], "%s %s - %s",
                                  vmesh_local_name(loc[r].h->msg.hazard_type),
                                  loc[r].h->msg.note[0] ? loc[r].h->msg.note
                                                        : "",
                                  db);
        } else {
            lv_label_set_text(local_row[r], "");
        }
    }
}

/* ---------------- feed pump + per-tick refresh ---------------- */

/* ---------------- convoy sheet ---------------- */

static void convoy_sheet_close(lv_event_t *e)
{
    (void)e;
    if (convoy_sheet) { lv_obj_delete(convoy_sheet); convoy_sheet = NULL; }
}

static void convoy_say_cb(lv_event_t *e)
{
    const char *phrase = (const char *)lv_event_get_user_data(e);
    vmesh_msg_t m;
    if (convoy_make_message(&m, phrase)) vmesh_feed_publish(&m);
    convoy_sheet_close(NULL);
}

static void convoy_leave_cb(lv_event_t *e)
{
    (void)e;
    convoy_leave();
    convoy_sheet_close(NULL);
}

static void convoy_sheet_open(lv_event_t *e)
{
    (void)e;
    if (convoy_sheet || !convoy_active()) return;
    uint32_t now = vmesh_time_s();
    vmesh_pose_t pose;
    vmesh_pose_get(&pose);

    convoy_sheet = lv_obj_create(lv_screen_active());
    lv_obj_set_size(convoy_sheet, lv_pct(56), LV_SIZE_CONTENT);
    lv_obj_align(convoy_sheet, LV_ALIGN_CENTER, 0, 0);
    lv_obj_set_style_bg_color(convoy_sheet, lv_color_hex(0x22262E), 0);
    lv_obj_set_style_border_color(convoy_sheet, lv_color_hex(0x3B4252), 0);
    lv_obj_set_style_radius(convoy_sheet, 12, 0);
    lv_obj_clear_flag(convoy_sheet, LV_OBJ_FLAG_SCROLLABLE);

    lv_obj_t *title = lv_label_create(convoy_sheet);
    lv_label_set_text_fmt(title, "Convoy \"%s\"", convoy_phrase());
    lv_obj_set_style_text_font(title, &lv_font_montserrat_20, 0);
    lv_obj_set_style_text_color(title, lv_color_white(), 0);

    /* who's out there: name (if we've heard their HELLO) + distance */
    lv_obj_t *anchor = title;
    int shown = 0;
    for (int i = 0; i < CONVOY_MAX; i++) {
        convoy_member_t *mb = convoy_slot(i);
        if (!mb || !mb->origin) continue;
        double dy = (mb->lat - pose.lat) * 110540.0;
        double dx = (mb->lon - pose.lon) * 111320.0 *
                    cos(pose.lat * M_PI / 180.0);
        char dist[16];
        fmt_dist(dist, sizeof(dist), sqrt(dx * dx + dy * dy));
        const char *nm = names_get(mb->origin, now);
        char idbuf[12];
        if (!nm) {
            snprintf(idbuf, sizeof(idbuf), "%06X",
                     (unsigned)(mb->origin & 0xFFFFFF));
            nm = idbuf;
        }
        lv_obj_t *row = lv_label_create(convoy_sheet);
        uint32_t ago = lv_tick_get() / 1000u - mb->heard_s;
        lv_label_set_text_fmt(row, LV_SYMBOL_GPS "  %s - %s (%us ago)",
                              nm, dist, (unsigned)ago);
        lv_obj_set_style_text_color(row, lv_color_hex(0xADB5BD), 0);
        lv_obj_align_to(row, anchor, LV_ALIGN_OUT_BOTTOM_LEFT, 0,
                        shown ? 6 : 12);
        anchor = row;
        shown++;
    }
    if (!shown) {
        lv_obj_t *none = lv_label_create(convoy_sheet);
        lv_label_set_text(none, "nobody else heard yet - same code word?");
        lv_obj_set_style_text_color(none, lv_color_hex(0x868E96), 0);
        lv_obj_align_to(none, title, LV_ALIGN_OUT_BOTTOM_LEFT, 0, 12);
        anchor = none;
    }

    /* four taps, never typing: the whole convoy vocabulary.
     * Two columns — each row's LEFT button anchors the next row (a
     * chain off the right-hand button walks the layout off-sheet). */
    lv_obj_t *row_left = NULL;
    for (int i = 0; i < CONVOY_PHRASES; i++) {
        lv_obj_t *b = lv_button_create(convoy_sheet);
        lv_obj_set_size(b, lv_pct(44), 46);
        if (i % 2 == 0) {
            if (!row_left)
                lv_obj_align_to(b, anchor, LV_ALIGN_OUT_BOTTOM_LEFT, 0, 16);
            else
                lv_obj_align_to(b, row_left, LV_ALIGN_OUT_BOTTOM_LEFT, 0, 8);
            row_left = b;
        } else {
            lv_obj_align_to(b, row_left, LV_ALIGN_OUT_RIGHT_MID, 8, 0);
        }
        lv_obj_set_style_bg_color(b, lv_color_hex(0x2B3A55), 0);
        lv_obj_add_event_cb(b, convoy_say_cb, LV_EVENT_CLICKED,
                            (void *)convoy_phrases[i]);
        lv_obj_t *l = lv_label_create(b);
        lv_label_set_text(l, convoy_phrases[i]);
        lv_obj_center(l);
    }

    lv_obj_t *lv_btn = lv_button_create(convoy_sheet);
    lv_obj_set_size(lv_btn, lv_pct(44), 44);
    lv_obj_align_to(lv_btn, row_left, LV_ALIGN_OUT_BOTTOM_LEFT, 0, 16);
    lv_obj_set_style_bg_color(lv_btn, lv_color_hex(0x3B2226), 0);
    lv_obj_add_event_cb(lv_btn, convoy_leave_cb, LV_EVENT_CLICKED, NULL);
    lv_obj_t *ll = lv_label_create(lv_btn);
    lv_label_set_text(ll, "Leave convoy");
    lv_obj_center(ll);

    lv_obj_t *cl = lv_button_create(convoy_sheet);
    lv_obj_set_size(cl, lv_pct(44), 44);
    lv_obj_align_to(cl, lv_btn, LV_ALIGN_OUT_RIGHT_MID, 8, 0);
    lv_obj_set_style_bg_color(cl, lv_color_hex(0x22262E), 0);
    lv_obj_add_event_cb(cl, convoy_sheet_close, LV_EVENT_CLICKED, NULL);
    lv_obj_t *cll = lv_label_create(cl);
    lv_label_set_text(cll, "Close");
    lv_obj_center(cll);
}

/* ---- dash clock timezone ----------------------------------------
 * No tz database on board; a solar zone (round lon/15) plus one DST
 * hour in northern summer is right almost everywhere Waycast nodes
 * live, almost all year. Longitude comes from the GPS pose or, indoors,
 * from the town anchor's beacon — same sources that set the clock. */
static float s_tz_lon, s_tz_lat;
static bool  s_tz_known;

static int tz_offset_min(uint32_t utc)
{
    float lon = s_tz_known ? s_tz_lon : 0.0f;
    int off = (int)(lon / 15.0f + (lon >= 0 ? 0.5f : -0.5f)) * 60;
    /* month via civil-from-days (Hinnant); DST at day granularity —
     * the boundary Sundays are off by design, it's a dash clock */
    int32_t  z   = (int32_t)(utc / 86400u) + 719468;
    int32_t  era = (z >= 0 ? z : z - 146096) / 146097;
    uint32_t doe = (uint32_t)(z - era * 146097);
    uint32_t yoe = (doe - doe / 1460 + doe / 36524 - doe / 146096) / 365;
    uint32_t doy = doe - (365 * yoe + yoe / 4 - yoe / 100);
    uint32_t mp  = (5 * doy + 2) / 153;
    uint32_t mon = mp < 10 ? mp + 3 : mp - 9;
    if (s_tz_known && s_tz_lat > 0 && mon >= 4 && mon <= 10)
        off += 60;
    return off;
}

static void ui_tick_cb(lv_timer_t *t)
{
    (void)t;
    uint32_t now = vmesh_time_s();

    vmesh_msg_t m;
    while (vmesh_feed_poll(&m)) {
        /* ANCHOR-AS-TIMESERVER (Peter, July 26): town beacons are
         * stamped with the node's GNSS/NTP clock. A device with no
         * GPS lock adopts it — read BEFORE normalization rewrites
         * the stamp with our own (possibly sim-epoch) clock. Live
         * GPS always wins; sanity floor rejects clockless anchors. */
        if (m.msg_type == VMESH_MT_BEACON &&
            vmesh_gps_state() != 2 && m.created_s > 1700000000u) {
            vmesh_time_set_live(m.created_s);
            now = vmesh_time_s();
        }

        /* clock-skew repair before anything judges ages (see
         * vmesh_clock_normalize — receivers trust reception time
         * over implausible sender stamps) */
        vmesh_clock_normalize(&m, now);
        if (m.msg_type == VMESH_MT_HELLO) {
            names_note(m.origin_id, m.note, now);
            continue;
        }
        if (m.msg_type == VMESH_MT_CONVOY) {
            convoy_note(&m, scenario_own_origin(), now);
            if (convoy_is_ours(&m) &&
                m.hazard_type == CONVOY_SUB_MESSAGE &&
                m.origin_id != scenario_own_origin()) {
                const char *who = names_get(m.origin_id, now);
                snprintf(s_convoy_ticker, sizeof(s_convoy_ticker),
                         "%s: %.20s", who ? who : "convoy", m.note + 8);
                s_convoy_ticker_ms = lv_tick_get();
            }
            /* positions live in the member table; only our convoy's
             * MESSAGES continue on to the store (chips + detail) */
            if (!(convoy_is_ours(&m) &&
                  m.hazard_type == CONVOY_SUB_MESSAGE))
                continue;
            /* drop the 8-hex group prefix — chips show clean text */
            memmove(m.note, m.note + 8, sizeof(m.note) - 8);
        }
        if (m.msg_type == VMESH_MT_BEACON) {
            /* anchor presence: refresh the "town near" indicator.
             * Deliberately no clock/radius gates — presence is about
             * hearing the RF now, not about timestamps. */
            s_anchor_heard_ms = lv_tick_get();
            snprintf(s_anchor_name, sizeof(s_anchor_name), "%.23s",
                     m.note[0] ? m.note : "town");
            if (m.lat_e7 || m.lon_e7) { /* tz from the anchor, indoors */
                s_tz_lon = m.lon_e7 / 1e7f;
                s_tz_lat = m.lat_e7 / 1e7f;
                s_tz_known = true;
            }
            continue;
        }
        if (m.origin_id == scenario_own_origin() && m.hops > 0) {
            /* our own report coming back off the air = somebody
             * accepted and rebroadcast it. Mark the receipt. */
            for (int i = 0; i < HAZARD_MAX; i++) {
                hazard_t *h = hazard_store_slot(i);
                if (h && h->active &&
                    h->msg.origin_id == m.origin_id &&
                    h->msg.seq == m.seq && !h->echoed) {
                    h->echoed = true;
                    break;
                }
            }
            continue; /* dedup would drop it anyway */
        }
        if (m.msg_type == VMESH_MT_REPLY) {
            if (qa_answer_lbl && detail_hz &&
                m.ref_origin == qa_pending_origin &&
                m.ref_seq == qa_pending_seq &&
                m.origin_id == detail_hz->msg.origin_id) {
                lv_label_set_text_fmt(qa_answer_lbl,
                    LV_SYMBOL_DOWNLOAD "  %s", m.note);
                lv_obj_set_style_text_color(qa_answer_lbl,
                    lv_color_hex(0x51CF66), 0);
            }
            continue;
        }
        hazard_store_ingest(&m);
    }

    hazard_store_prune(now, map_view_drop_chip);

    /* convoy: own position on the air on a slow cadence, and age out
     * members (and the convoy itself) that have gone quiet */
    convoy_prune(now);
    if (convoy_beacon_due(now)) {
        vmesh_msg_t cb;
        if (convoy_make_beacon(&cb)) vmesh_feed_publish(&cb);
    }

    /* HELLO cadence: our handle onto the air shortly after boot (radio
     * is up by then), then every 10 min. Silent if no handle is set. */
    {
        static uint32_t last_hello_ms;
        uint32_t up_ms = lv_tick_get();
        bool due = last_hello_ms == 0 ? up_ms > 15000
                                      : lv_tick_elaps(last_hello_ms) > 600000;
        if (due && vmesh_own_handle()[0]) {
            last_hello_ms = up_ms ? up_ms : 1;
            vmesh_msg_t hello;
            scenario_make_own_hello(&hello);
            vmesh_feed_publish(&hello);
        }
    }

    /* OTA download shroud: while flash is being written, big map
     * redraws collide with erase stalls and tear the panel — a near-
     * static dark screen (tiny text updates only) keeps the frame-
     * buffer calm. Lives here (not settings) so it survives closing
     * the settings overlay mid-download. The IRAM route
     * (LCD_DSI_ISR_CACHE_SAFE) bricked boot — see sdkconfig.defaults. */
    {
        static lv_obj_t *fw_shroud, *fw_shroud_sub;
        const vmesh_fw_ops_t *f = vmesh_fw_ops();
        if (f && f->state() == VMESH_FW_DOWNLOADING) {
            if (!fw_shroud) {
                fw_shroud = lv_obj_create(lv_layer_top());
                lv_obj_set_size(fw_shroud, lv_pct(100), lv_pct(100));
                lv_obj_set_style_bg_color(fw_shroud,
                                          lv_color_hex(0x0C0F13), 0);
                lv_obj_set_style_bg_opa(fw_shroud, LV_OPA_COVER, 0);
                lv_obj_set_style_border_width(fw_shroud, 0, 0);
                lv_obj_clear_flag(fw_shroud, LV_OBJ_FLAG_SCROLLABLE);
                lv_obj_t *tt = lv_label_create(fw_shroud);
                lv_label_set_text(tt,
                                  LV_SYMBOL_DOWNLOAD "  Updating firmware");
                lv_obj_set_style_text_font(tt, &lv_font_montserrat_28, 0);
                lv_obj_set_style_text_color(tt, lv_color_white(), 0);
                lv_obj_align(tt, LV_ALIGN_CENTER, 0, -30);
                fw_shroud_sub = lv_label_create(fw_shroud);
                lv_label_set_text(fw_shroud_sub, "");
                lv_obj_set_style_text_color(fw_shroud_sub,
                                            lv_color_hex(0x8B95A3), 0);
                lv_obj_align(fw_shroud_sub, LV_ALIGN_CENTER, 0, 16);
                lv_obj_t *wl = lv_label_create(fw_shroud);
                lv_label_set_text(wl,
                    "keep power on - the mesh keeps running");
                lv_obj_set_style_text_color(wl, lv_color_hex(0x5C6672), 0);
                lv_obj_align(wl, LV_ALIGN_CENTER, 0, 52);
            }
            {   /* only invalidate on REAL text changes — every label
                 * flush during flash writes is one flicker event */
                static char prev[96];
                const char *st = f->status();
                if (strcmp(prev, st) != 0) {
                    snprintf(prev, sizeof(prev), "%s", st);
                    lv_label_set_text(fw_shroud_sub, st);
                }
            }
        } else if (fw_shroud) {
            lv_obj_delete(fw_shroud);
            fw_shroud = NULL;
        }
    }

    vmesh_pose_t pose;
    vmesh_pose_get(&pose);
    map_view_update(&pose, now);
    update_info_panel(&pose, now);
    town_square_tick(&pose, now);
    /* the parked auto-QA is demo choreography (it opens a place sheet
     * and queries over the radio unbidden) — real mode leaves the
     * screen alone; tapping a marker still opens its detail */
    if (scenario_demo() && !town_square_is_open()) qa_demo_tick(&pose);

    lv_label_set_text_fmt(lbl_speed_big, "%d",
                          (int)(pose.speed_mps * 2.23694f));
    if (vmesh_pose_is_live()) { /* GPS beats the anchor for tz too */
        s_tz_lon = (float)pose.lon;
        s_tz_lat = (float)pose.lat;
        s_tz_known = true;
    }
    if (vmesh_time_live()) { /* wall clock (Peter: convenience) */
        int64_t loc = (int64_t)vmesh_time_s() +
                      60 * tz_offset_min(vmesh_time_s());
        uint32_t hm = (uint32_t)(((loc % 86400) + 86400) % 86400) / 60u;
        lv_label_set_text_fmt(lbl_clock, "%u:%02u",
                              (unsigned)(hm / 60), (unsigned)(hm % 60));
    } else { /* no real time source yet: uptime, as before */
        float c = scenario_clock_s();
        lv_label_set_text_fmt(lbl_clock, "%d:%02d",
                              (int)c / 60, (int)c % 60);
    }

    /* status-bar badges: GPS (pose source) and Wi-Fi, each with its own
     * icon + color; change-detected so labels only invalidate on real
     * transitions */
    {
        /* GPS badge — doubles as the GNSS diagnostic: silent /
         * acquiring / locked, no console needed. In demo mode the pose
         * comes from the scenario player, and the badge says so. */
        int gs = vmesh_gps_state();
        char gtxt[40];
        uint32_t gcolor;
        if (gs == 2)      { snprintf(gtxt, sizeof(gtxt), LV_SYMBOL_GPS " GPS live");     gcolor = 0x51CF66; }
        else if (gs == 1) { snprintf(gtxt, sizeof(gtxt), LV_SYMBOL_GPS " acquiring");    gcolor = 0xF59F00; }
        else if (scenario_demo())
                          { snprintf(gtxt, sizeof(gtxt), LV_SYMBOL_PLAY " SIM feed");    gcolor = 0x5C7CFA; }
        else              { snprintf(gtxt, sizeof(gtxt), LV_SYMBOL_GPS " no GPS");       gcolor = 0x868E96; }
        static char gprev[40];
        if (strcmp(gprev, gtxt) != 0) {
            strcpy(gprev, gtxt);
            lv_label_set_text(lbl_radios, gtxt);
            lv_obj_set_style_text_color(lbl_radios, lv_color_hex(gcolor), 0);
        }

        /* Wi-Fi badge — independent life, independent color */
        const vmesh_wifi_ops_t *w = vmesh_wifi_ops();
        char wtxt[40];
        uint32_t wcolor = 0x868E96;
        if (w) {
            char ip[20] = "", ssid[16] = "";
            vmesh_wifi_state_t ws = w->status(ip, sizeof(ip));
            if (w->ssid) w->ssid(ssid, sizeof(ssid));
            switch (ws) {
            case VMESH_WIFI_CONNECTED:
                snprintf(wtxt, sizeof(wtxt), LV_SYMBOL_WIFI " %s", ssid);
                wcolor = 0x51CF66;
                break;
            case VMESH_WIFI_CONNECTING:
                snprintf(wtxt, sizeof(wtxt), LV_SYMBOL_WIFI " %s ...", ssid);
                wcolor = 0xF59F00;
                break;
            case VMESH_WIFI_FAILED:
                snprintf(wtxt, sizeof(wtxt), LV_SYMBOL_WIFI " join failed");
                wcolor = 0xFF6B6B;
                break;
            default:
                snprintf(wtxt, sizeof(wtxt), LV_SYMBOL_WIFI " --");
            }
        } else {
            snprintf(wtxt, sizeof(wtxt), LV_SYMBOL_WIFI " --");
        }
        /* town-near indicator: green while beacons are fresh
         * (<3 min = ~3 beacon periods), silent otherwise */
        {
            bool near = s_anchor_heard_ms &&
                        lv_tick_elaps(s_anchor_heard_ms) < 180000;
            static char town_prev[32];
            char town_now[32];
            snprintf(town_now, sizeof(town_now), "%s",
                     near ? s_anchor_name : "");
            if (strcmp(town_now, town_prev) != 0) {
                snprintf(town_prev, sizeof(town_prev), "%s", town_now);
                if (near)
                    lv_label_set_text_fmt(lbl_town, LV_SYMBOL_HOME " %s",
                                          town_now);
                else
                    lv_label_set_text(lbl_town, "");
            }
        }

        /* convoy chip: members heard, tap for the sheet */
        {
            char ctxt[56] = "";
            bool fresh_msg = s_convoy_ticker_ms &&
                             lv_tick_elaps(s_convoy_ticker_ms) < 60000;
            if (convoy_active() && fresh_msg)
                snprintf(ctxt, sizeof(ctxt), LV_SYMBOL_SHUFFLE " %s",
                         s_convoy_ticker);
            else if (convoy_active())
                snprintf(ctxt, sizeof(ctxt), LV_SYMBOL_SHUFFLE " convoy %d",
                         convoy_count());
            static char cprev[56];
            if (strcmp(cprev, ctxt) != 0) {
                snprintf(cprev, sizeof(cprev), "%s", ctxt);
                lv_label_set_text(lbl_convoy, ctxt);
            }
        }

        static char wprev[40];
        if (strcmp(wprev, wtxt) != 0) {
            strcpy(wprev, wtxt);
            lv_label_set_text(lbl_wifi, wtxt);
            lv_obj_set_style_text_color(lbl_wifi, lv_color_hex(wcolor), 0);
        }
    }
}

/* ---------------- init ---------------- */

void ui_init(void)
{
    lv_obj_t *scr = lv_screen_active();
    lv_obj_set_style_bg_color(scr, lv_color_hex(0x14161B), 0);

    const int32_t W = lv_display_get_horizontal_resolution(NULL);
    const int32_t H = lv_display_get_vertical_resolution(NULL);
    const int32_t BAR_H = 44;
    const int32_t MAP_W = W * MAP_PCT / 100;

    /* status bar: the §3 trust story made visible — which radios are
     * alive right now. In the simulator that's the SIM feed badge;
     * on hardware it reads "LoRa - BLE". */
    status_bar = lv_obj_create(scr);
    lv_obj_set_size(status_bar, W, BAR_H);
    lv_obj_set_pos(status_bar, 0, 0);
    lv_obj_set_style_bg_color(status_bar, lv_color_hex(0x14161B), 0);
    lv_obj_set_style_border_width(status_bar, 0, 0);
    lv_obj_set_style_radius(status_bar, 0, 0);
    lv_obj_set_style_pad_hor(status_bar, 12, 0);
    lv_obj_clear_flag(status_bar, LV_OBJ_FLAG_SCROLLABLE);

    /* two SEPARATE badges (July 23): GPS and Wi-Fi are unrelated radios
     * — one label with one color couldn't tell their states apart */
    lbl_radios = lv_label_create(status_bar); /* GPS / pose source */
    lv_label_set_text(lbl_radios, LV_SYMBOL_GPS " --");
    lv_obj_set_style_text_color(lbl_radios, lv_color_hex(0x868E96), 0);
    lv_obj_align(lbl_radios, LV_ALIGN_LEFT_MID, 0, 0);
    /* tap either badge -> S5 radio disclosure */
    lv_obj_add_flag(lbl_radios, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_set_ext_click_area(lbl_radios, 12);
    lv_obj_add_event_cb(lbl_radios, about_open_cb, LV_EVENT_CLICKED, NULL);

    lbl_convoy = lv_label_create(status_bar); /* convoy chip */
    lv_label_set_text(lbl_convoy, "");
    lv_obj_set_style_text_color(lbl_convoy, lv_color_hex(0x8AB4F8), 0);
    lv_obj_align(lbl_convoy, LV_ALIGN_LEFT_MID, 560, 0);
    lv_obj_add_flag(lbl_convoy, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_set_ext_click_area(lbl_convoy, 14);
    lv_obj_add_event_cb(lbl_convoy, convoy_sheet_open, LV_EVENT_CLICKED, NULL);

    lbl_town = lv_label_create(status_bar); /* anchor presence */
    lv_label_set_text(lbl_town, "");
    lv_obj_set_style_text_color(lbl_town, lv_color_hex(0x51CF66), 0);
    lv_obj_align(lbl_town, LV_ALIGN_LEFT_MID, 380, 0);

    lbl_wifi = lv_label_create(status_bar); /* Wi-Fi, own icon + color */
    lv_label_set_text(lbl_wifi, LV_SYMBOL_WIFI " --");
    lv_obj_set_style_text_color(lbl_wifi, lv_color_hex(0x868E96), 0);
    lv_obj_align(lbl_wifi, LV_ALIGN_LEFT_MID, 190, 0);
    lv_obj_add_flag(lbl_wifi, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_set_ext_click_area(lbl_wifi, 12);
    lv_obj_add_event_cb(lbl_wifi, about_open_cb, LV_EVENT_CLICKED, NULL);

    lbl_clock = lv_label_create(status_bar);
    lv_label_set_text(lbl_clock, "0:00");
    lv_obj_set_style_text_color(lbl_clock, lv_color_hex(0xADB5BD), 0);
    lv_obj_align(lbl_clock, LV_ALIGN_RIGHT_MID, 0, 0);

    /* the door between the two postures: always visible on the map
     * side; the square's own "Map" button is the way back */
    lv_obj_t *sqb = lv_button_create(status_bar);
    lv_obj_set_size(sqb, 160, 34);
    lv_obj_align(sqb, LV_ALIGN_RIGHT_MID, -64, 0);
    lv_obj_set_style_bg_color(sqb, lv_color_hex(0x22262E), 0);
    lv_obj_set_style_border_width(sqb, 1, 0);
    lv_obj_set_style_border_color(sqb, lv_color_hex(0x3B4252), 0);
    lv_obj_set_style_radius(sqb, 17, 0);
    lv_obj_add_event_cb(sqb, square_btn_cb, LV_EVENT_CLICKED, NULL);
    lv_obj_t *sql = lv_label_create(sqb);
    lv_label_set_text(sql, LV_SYMBOL_HOME "  Town square");
    lv_obj_set_style_text_font(sql, &lv_font_montserrat_14, 0);
    lv_obj_center(sql);

    /* gear: one tap straight to Wi-Fi & maps — the wiki had to explain
     * "tap the unlabeled badges, then find the button" twice; when the
     * manual apologizes for the UI, the UI is wrong */
    lv_obj_t *gearb = lv_button_create(status_bar);
    lv_obj_set_size(gearb, 48, 34);
    lv_obj_align(gearb, LV_ALIGN_RIGHT_MID, -240, 0);
    lv_obj_set_style_bg_color(gearb, lv_color_hex(0x22262E), 0);
    lv_obj_set_style_border_width(gearb, 1, 0);
    lv_obj_set_style_border_color(gearb, lv_color_hex(0x3B4252), 0);
    lv_obj_set_style_radius(gearb, 17, 0);
    lv_obj_add_event_cb(gearb, gear_btn_cb, LV_EVENT_CLICKED, NULL);
    lv_obj_t *gl = lv_label_create(gearb);
    lv_label_set_text(gl, LV_SYMBOL_SETTINGS);
    lv_obj_set_style_text_color(gl, lv_color_hex(0xADB5BD), 0);
    lv_obj_center(gl);

    /* left: the map */
    lv_obj_t *map_wrap = lv_obj_create(scr);
    lv_obj_set_pos(map_wrap, 0, BAR_H);
    lv_obj_set_size(map_wrap, MAP_W, H - BAR_H);
    lv_obj_set_style_pad_all(map_wrap, 0, 0);
    lv_obj_set_style_border_width(map_wrap, 0, 0);
    lv_obj_set_style_radius(map_wrap, 0, 0);
    lv_obj_clear_flag(map_wrap, LV_OBJ_FLAG_SCROLLABLE);
    map_view_create(map_wrap);
    map_view_set_tap_cb(show_detail);

    /* right: the info column */
    info_panel = lv_obj_create(scr);
    lv_obj_set_pos(info_panel, MAP_W, BAR_H);
    lv_obj_set_size(info_panel, W - MAP_W, H - BAR_H);
    lv_obj_set_style_bg_color(info_panel, lv_color_hex(0x14161B), 0);
    lv_obj_set_style_border_width(info_panel, 0, 0);
    lv_obj_set_style_radius(info_panel, 0, 0);
    lv_obj_set_style_pad_all(info_panel, 18, 0);
    lv_obj_clear_flag(info_panel, LV_OBJ_FLAG_SCROLLABLE);

    /* big speed readout */
    lbl_speed_big = lv_label_create(info_panel);
    lv_label_set_text(lbl_speed_big, "0");
    lv_obj_set_style_text_font(lbl_speed_big, &lv_font_montserrat_28, 0);
    lv_obj_set_style_text_color(lbl_speed_big, lv_color_white(), 0);
    lv_obj_align(lbl_speed_big, LV_ALIGN_TOP_LEFT, 0, 0);
    lv_obj_t *mph = lv_label_create(info_panel);
    lv_label_set_text(mph, "mph");
    lv_obj_set_style_text_color(mph, lv_color_hex(0x868E96), 0);
    lv_obj_align_to(mph, lbl_speed_big, LV_ALIGN_OUT_RIGHT_BOTTOM, 14, -4);

    /* NEXT AHEAD card */
    card = lv_obj_create(info_panel);
    lv_obj_set_size(card, lv_pct(100), 190);
    lv_obj_align(card, LV_ALIGN_TOP_MID, 0, 48);
    lv_obj_set_style_radius(card, 14, 0);
    lv_obj_set_style_border_width(card, 2, 0);
    lv_obj_set_style_bg_color(card, lv_color_hex(0x191C22), 0);
    lv_obj_set_style_pad_all(card, 14, 0);
    lv_obj_clear_flag(card, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_add_flag(card, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_add_event_cb(card, card_click_cb, LV_EVENT_CLICKED, NULL);

    card_kicker = lv_label_create(card);
    lv_label_set_text(card_kicker, "ALL CLEAR");
    lv_obj_set_style_text_font(card_kicker, &lv_font_montserrat_12, 0);
    lv_obj_set_style_text_color(card_kicker, lv_color_hex(0xADB5BD), 0);
    lv_obj_align(card_kicker, LV_ALIGN_TOP_LEFT, 0, 0);

    card_title = lv_label_create(card);
    lv_label_set_text(card_title, "No hazards nearby");
    lv_obj_set_style_text_font(card_title, &lv_font_montserrat_28, 0);
    lv_obj_set_style_text_color(card_title, lv_color_white(), 0);
    lv_obj_align(card_title, LV_ALIGN_TOP_LEFT, 0, 24);

    card_dist = lv_label_create(card);
    lv_label_set_text(card_dist, "");
    lv_obj_set_style_text_font(card_dist, &lv_font_montserrat_28, 0);
    lv_obj_set_style_text_color(card_dist, lv_color_white(), 0);
    lv_obj_align(card_dist, LV_ALIGN_TOP_RIGHT, 0, 24);

    card_sub = lv_label_create(card);
    lv_label_set_text(card_sub, "listening on the mesh...");
    lv_obj_set_style_text_font(card_sub, &lv_font_montserrat_14, 0);
    lv_obj_set_style_text_color(card_sub, lv_color_hex(0xADB5BD), 0);
    lv_obj_align(card_sub, LV_ALIGN_BOTTOM_LEFT, 0, 0);

    /* the rest of the active hazards */
    lbl_also = lv_label_create(info_panel);
    lv_label_set_text(lbl_also, "ALSO ACTIVE");
    lv_obj_set_style_text_font(lbl_also, &lv_font_montserrat_12, 0);
    lv_obj_set_style_text_color(lbl_also, lv_color_hex(0x868E96), 0);
    lv_obj_align(lbl_also, LV_ALIGN_TOP_LEFT, 0, 258);

    for (int r = 0; r < LIST_ROWS; r++) {
        list_row[r] = lv_label_create(info_panel);
        lv_label_set_text(list_row[r], "");
        lv_obj_set_style_text_font(list_row[r], &lv_font_montserrat_16, 0);
        lv_obj_align(list_row[r], LV_ALIGN_TOP_LEFT, 0, 284 + r * 30);
    }

    /* LOCAL channel section (speed-gated; see update_info_panel).
     * Parked, the header becomes the door to the town square (S2). */
    lbl_local = lv_label_create(info_panel);
    lv_label_set_text(lbl_local, "LOCAL - quiet");
    lv_obj_set_style_text_font(lbl_local, &lv_font_montserrat_12, 0);
    lv_obj_set_style_text_color(lbl_local, lv_color_hex(0x0CA678), 0);
    lv_obj_align(lbl_local, LV_ALIGN_TOP_LEFT, 0, 392);
    lv_obj_add_flag(lbl_local, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_set_ext_click_area(lbl_local, 14);
    lv_obj_add_event_cb(lbl_local, local_header_cb, LV_EVENT_CLICKED, NULL);

    for (int r = 0; r < LOCAL_ROWS; r++) {
        local_row[r] = lv_label_create(info_panel);
        lv_label_set_text(local_row[r], "");
        lv_obj_set_style_text_font(local_row[r], &lv_font_montserrat_16, 0);
        lv_obj_set_style_text_color(local_row[r], lv_color_hex(0x63E6BE), 0);
        lv_obj_set_width(local_row[r], lv_pct(100));
        lv_label_set_long_mode(local_row[r], LV_LABEL_LONG_DOT);
        lv_obj_align(local_row[r], LV_ALIGN_TOP_LEFT, 0, 418 + r * 30);
    }

    /* big fixed Report button — never moves, driver muscle memory */
    report_btn = lv_button_create(info_panel);
    lv_obj_set_size(report_btn, lv_pct(100), 68);
    lv_obj_align(report_btn, LV_ALIGN_BOTTOM_MID, 0, 0);
    lv_obj_set_style_radius(report_btn, 14, 0);
    lv_obj_set_style_bg_color(report_btn, lv_color_hex(0xE8590C), 0);
    lv_obj_add_event_cb(report_btn, report_open_cb, LV_EVENT_CLICKED, NULL);
    lv_obj_t *rl = lv_label_create(report_btn);
    lv_label_set_text(rl, LV_SYMBOL_PLUS "  Report hazard");
    lv_obj_set_style_text_font(rl, &lv_font_montserrat_20, 0);
    lv_obj_center(rl);

    lv_timer_create(ui_tick_cb, 100, NULL);
}
