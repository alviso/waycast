/**
 * town_square.c — S2, the parked town square (DEVICE_UI2.md).
 *
 * A new rendering over the existing hazard_store: no new plumbing.
 * Card anatomy per the design doc: channel color bar, title +
 * distance, provenance in plain words, age + expiry, trust tally.
 * Cards visibly fade as they age; expired cards are simply gone.
 * Tap a card -> the existing detail sheet (votes, ask-the-place).
 */
#include "town_square.h"
#include "hazard_store.h"
#include "feed.h"

#include "lvgl.h"

#include <math.h>
#include <stdio.h>

#define PARKED_MPS 2.5f /* same gate as ui.c */

/* provided by ui.c (wrappers around its static overlays) */
void ui_show_detail(hazard_t *h);
void ui_open_report(void);

static lv_obj_t *square;    /* the full-screen overlay; NULL = closed */
static lv_obj_t *feed_list; /* scrollable card column */
static lv_obj_t *lbl_count;
static lv_obj_t *chip_btn[3];
static lv_obj_t *passenger_btn;

enum { FILT_ALL, FILT_NOTICES, FILT_ALERTS };
static int filt = FILT_ALL;
/* passenger consent: granted via the modal, expires on its own */
#define PASSENGER_TTL_S 600
static uint32_t passenger_until; /* vmesh_time_s deadline; 0 = none */
static lv_obj_t *consent; /* the consent modal; NULL = closed */
static uint32_t last_sig;

static bool passenger_valid(uint32_t now) { return now < passenger_until; }

static double dist_m(const hazard_t *h, const vmesh_pose_t *pose)
{
    double dy = (h->msg.lat_e7 / 1e7 - pose->lat) * 110540.0;
    double dx = (h->msg.lon_e7 / 1e7 - pose->lon) * 111320.0 *
                cos(pose->lat * M_PI / 180.0);
    return sqrt(dx * dx + dy * dy);
}

static bool filt_match(const hazard_t *h)
{
    if (filt == FILT_NOTICES) return h->msg.channel == VMESH_CH_LOCAL;
    if (filt == FILT_ALERTS) return h->msg.channel != VMESH_CH_LOCAL;
    return true;
}

static lv_color_t channel_color(const hazard_t *h)
{
    if (h->msg.channel == VMESH_CH_LOCAL) return lv_color_hex(0x0CA678);
    switch (h->msg.severity) {
    case 3:  return lv_color_hex(0xE03131);
    case 2:  return lv_color_hex(0xF59F00);
    default: return lv_color_hex(0x5C7CFA);
    }
}

/* provenance in plain words (DEVICE_UI2: trust lives in provenance +
 * votes, never identity). Town nodes stamp 0x5049xxxx origins. */
static const char *provenance(const hazard_t *h)
{
    if ((h->msg.origin_id & 0xFFFF0000u) == 0x50490000u)
        return "town node";
    if (h->msg.channel == VMESH_CH_LOCAL) return "place beacon";
    return "another driver";
}

static void card_click_cb(lv_event_t *e)
{
    hazard_t *h = lv_event_get_user_data(e);
    if (h && h->active) ui_show_detail(h);
}

static void build_card(hazard_t *h, double d, uint32_t now)
{
    lv_obj_t *c = lv_obj_create(feed_list);
    lv_obj_set_size(c, lv_pct(100), LV_SIZE_CONTENT);
    lv_obj_set_style_bg_color(c, lv_color_hex(0x191C22), 0);
    lv_obj_set_style_border_width(c, 0, 0);
    lv_obj_set_style_radius(c, 12, 0);
    lv_obj_set_style_pad_all(c, 14, 0);
    lv_obj_set_style_pad_left(c, 24, 0);
    lv_obj_clear_flag(c, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_add_flag(c, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_add_event_cb(c, card_click_cb, LV_EVENT_CLICKED, h);

    /* channel color bar */
    lv_obj_t *bar = lv_obj_create(c);
    lv_obj_set_size(bar, 5, lv_pct(100));
    lv_obj_align(bar, LV_ALIGN_LEFT_MID, -18, 0);
    lv_obj_set_style_bg_color(bar, channel_color(h), 0);
    lv_obj_set_style_border_width(bar, 0, 0);
    lv_obj_set_style_radius(bar, 2, 0);

    /* title + distance */
    char db[16];
    if (d < 950) snprintf(db, sizeof(db), "%d m", (int)(d / 10) * 10);
    else         snprintf(db, sizeof(db), "%.1f km", d / 1000.0);

    lv_obj_t *title = lv_label_create(c);
    lv_label_set_text_fmt(title, "%s%s%s",
                          vmesh_msg_name(h->msg.channel, h->msg.hazard_type),
                          h->msg.note[0] ? " - " : "",
                          h->msg.note[0] ? h->msg.note : "");
    lv_obj_set_style_text_font(title, &lv_font_montserrat_20, 0);
    lv_obj_set_style_text_color(title, lv_color_white(), 0);
    lv_obj_set_width(title, lv_pct(82));
    lv_label_set_long_mode(title, LV_LABEL_LONG_DOT);
    lv_obj_align(title, LV_ALIGN_TOP_LEFT, 0, 0);

    lv_obj_t *dist = lv_label_create(c);
    lv_label_set_text(dist, db);
    lv_obj_set_style_text_font(dist, &lv_font_montserrat_16, 0);
    lv_obj_set_style_text_color(dist, lv_color_hex(0xADB5BD), 0);
    lv_obj_align(dist, LV_ALIGN_TOP_RIGHT, 0, 2);

    /* provenance - age - expiry */
    int age_min = (int)(now - h->msg.created_s) / 60;
    int left_s = (int)(h->msg.created_s + h->msg.ttl_s - now);
    lv_obj_t *meta = lv_label_create(c);
    lv_label_set_text_fmt(meta, "%s - hop %d - %d min ago - %d min left",
                          provenance(h), h->msg.hops, age_min,
                          left_s > 0 ? left_s / 60 : 0);
    lv_obj_set_style_text_font(meta, &lv_font_montserrat_14, 0);
    lv_obj_set_style_text_color(meta, lv_color_hex(0x868E96), 0);
    lv_obj_align_to(meta, title, LV_ALIGN_OUT_BOTTOM_LEFT, 0, 6);

    /* trust tally (when the mesh has spoken) */
    if (h->confirms || h->denies) {
        lv_obj_t *tally = lv_label_create(c);
        lv_label_set_text_fmt(tally, "%d confirmed - %d disputed%s",
                              h->confirms, h->denies,
                              h->my_vote ? "  (incl. you)" : "");
        lv_obj_set_style_text_font(tally, &lv_font_montserrat_14, 0);
        lv_obj_set_style_text_color(tally,
            (h->denies > h->confirms) ? lv_color_hex(0xF59F00)
                                      : lv_color_hex(0x51CF66), 0);
        lv_obj_align_to(tally, meta, LV_ALIGN_OUT_BOTTOM_LEFT, 0, 4);
    }

    /* ephemerality made visible: the card itself fades toward expiry */
    float age = hazard_age_frac(h, now);
    lv_obj_set_style_opa(c, (lv_opa_t)(255 - (int)(age * 140)), 0);
}

static void rebuild(uint32_t now)
{
    if (!square) return;
    vmesh_pose_t pose;
    vmesh_pose_get(&pose);

    int32_t scroll_y = lv_obj_get_scroll_y(feed_list);
    lv_obj_clean(feed_list);

    /* collect + sort by distance: the town, nearest first */
    static struct { hazard_t *h; double d; } items[HAZARD_MAX];
    int n = 0;
    for (int i = 0; i < HAZARD_MAX; i++) {
        hazard_t *h = hazard_store_slot(i);
        if (!h || !h->active || !filt_match(h)) continue;
        items[n].h = h;
        items[n].d = dist_m(h, &pose);
        n++;
    }
    for (int i = 1; i < n; i++) {
        int j = i;
        while (j > 0 && items[j].d < items[j - 1].d) {
            __typeof__(items[0]) tmp = items[j];
            items[j] = items[j - 1];
            items[j - 1] = tmp;
            j--;
        }
    }

    for (int i = 0; i < n; i++) build_card(items[i].h, items[i].d, now);

    if (n == 0) {
        lv_obj_t *empty = lv_label_create(feed_list);
        lv_label_set_text(empty, "The square is quiet - nothing on the "
                                 "air here right now.");
        lv_obj_set_style_text_color(empty, lv_color_hex(0x868E96), 0);
        lv_obj_set_style_text_font(empty, &lv_font_montserrat_16, 0);
    }

    lv_label_set_text_fmt(lbl_count, "%d note%s alive on the mesh - "
                          "gone when they expire, kept nowhere",
                          n, n == 1 ? "" : "s");
    lv_obj_scroll_to_y(feed_list, scroll_y, LV_ANIM_OFF);
}

static uint32_t feed_sig(uint32_t now)
{
    uint32_t sig = (uint32_t)filt * 2654435761u + now / 30;
    for (int i = 0; i < HAZARD_MAX; i++) {
        hazard_t *h = hazard_store_slot(i);
        if (!h || !h->active) continue;
        sig = sig * 31 + h->msg.origin_id + h->msg.seq * 7 +
              h->confirms * 131 + h->denies * 251;
    }
    return sig;
}

static void chip_paint(void)
{
    for (int i = 0; i < 3; i++) {
        lv_obj_t *lbl = lv_obj_get_child(chip_btn[i], 0);
        lv_obj_set_style_text_color(lbl,
            i == filt ? lv_color_hex(0xF59F00) : lv_color_hex(0x868E96), 0);
        lv_obj_set_style_border_color(chip_btn[i],
            i == filt ? lv_color_hex(0xF59F00) : lv_color_hex(0x3B4252), 0);
    }
}

static void chip_cb(lv_event_t *e)
{
    filt = (int)(uintptr_t)lv_event_get_user_data(e);
    chip_paint();
    last_sig = 0; /* force rebuild */
}

static void passenger_paint(void)
{
    if (!passenger_btn) return;
    bool ok = passenger_valid(vmesh_time_s());
    lv_obj_t *lbl = lv_obj_get_child(passenger_btn, 0);
    lv_label_set_text(lbl, ok ? LV_SYMBOL_OK " passenger"
                              : "passenger?");
    lv_obj_set_style_text_color(lbl,
        ok ? lv_color_hex(0x51CF66) : lv_color_hex(0x868E96), 0);
}

/* ---- the consent modal: honest words, big buttons, expiring grant */
static void consent_close(void)
{
    if (consent) {
        lv_obj_delete(consent);
        consent = NULL;
    }
}

static void consent_grant_cb(lv_event_t *e)
{
    (void)e;
    passenger_until = vmesh_time_s() + PASSENGER_TTL_S;
    consent_close();
    passenger_paint();
    if (!square) town_square_open();
}

static void consent_cancel_cb(lv_event_t *e)
{
    (void)e;
    consent_close();
}

static void consent_open(void)
{
    if (consent) return;
    consent = lv_obj_create(lv_screen_active());
    lv_obj_set_size(consent, lv_pct(100), lv_pct(100));
    lv_obj_set_style_bg_color(consent, lv_color_hex(0x14161B), 0);
    lv_obj_set_style_bg_opa(consent, LV_OPA_80, 0);
    lv_obj_set_style_border_width(consent, 0, 0);
    lv_obj_clear_flag(consent, LV_OBJ_FLAG_SCROLLABLE);

    lv_obj_t *panel = lv_obj_create(consent);
    lv_obj_set_size(panel, 560, LV_SIZE_CONTENT);
    lv_obj_center(panel);
    lv_obj_set_style_bg_color(panel, lv_color_hex(0x22262E), 0);
    lv_obj_set_style_border_color(panel, lv_color_hex(0x3B4252), 0);
    lv_obj_set_style_radius(panel, 14, 0);
    lv_obj_set_style_pad_all(panel, 24, 0);
    lv_obj_clear_flag(panel, LV_OBJ_FLAG_SCROLLABLE);

    lv_obj_t *t = lv_label_create(panel);
    lv_label_set_text(t, "A moving car's screen is for driving");
    lv_obj_set_style_text_font(t, &lv_font_montserrat_20, 0);
    lv_obj_set_style_text_color(t, lv_color_white(), 0);
    lv_obj_align(t, LV_ALIGN_TOP_LEFT, 0, 0);

    lv_obj_t *bdy = lv_label_create(panel);
    lv_label_set_text_fmt(bdy,
        "If you're a passenger, the square is yours for %d minutes.\n"
        "The driver shouldn't browse at speed.", PASSENGER_TTL_S / 60);
    lv_obj_set_style_text_font(bdy, &lv_font_montserrat_16, 0);
    lv_obj_set_style_text_color(bdy, lv_color_hex(0xADB5BD), 0);
    lv_obj_align_to(bdy, t, LV_ALIGN_OUT_BOTTOM_LEFT, 0, 10);

    lv_obj_t *yes = lv_button_create(panel);
    lv_obj_set_size(yes, 230, 56);
    lv_obj_align_to(yes, bdy, LV_ALIGN_OUT_BOTTOM_LEFT, 0, 18);
    lv_obj_set_style_bg_color(yes, lv_color_hex(0x2B8A3E), 0);
    lv_obj_add_event_cb(yes, consent_grant_cb, LV_EVENT_CLICKED, NULL);
    lv_obj_t *yl = lv_label_create(yes);
    lv_label_set_text(yl, LV_SYMBOL_OK "  I'm a passenger");
    lv_obj_center(yl);

    lv_obj_t *no = lv_button_create(panel);
    lv_obj_set_size(no, 150, 56);
    lv_obj_align_to(no, yes, LV_ALIGN_OUT_RIGHT_MID, 16, 0);
    lv_obj_set_style_bg_color(no, lv_color_hex(0x343A46), 0);
    lv_obj_add_event_cb(no, consent_cancel_cb, LV_EVENT_CLICKED, NULL);
    lv_obj_t *nl = lv_label_create(no);
    lv_label_set_text(nl, "Cancel");
    lv_obj_center(nl);
}

static void passenger_cb(lv_event_t *e)
{
    (void)e;
    if (passenger_valid(vmesh_time_s()))
        passenger_until = 0; /* revoke: one tap, driver-friendly */
    else
        consent_open();
    passenger_paint();
}

static void close_cb(lv_event_t *e)
{
    (void)e;
    town_square_close();
}

static void post_cb(lv_event_t *e)
{
    (void)e;
    ui_open_report();
}

void town_square_request_open(void)
{
    vmesh_pose_t pose;
    vmesh_pose_get(&pose);
    if (pose.speed_mps < PARKED_MPS || passenger_valid(vmesh_time_s()))
        town_square_open();
    else
        consent_open();
}

void town_square_close(void)
{
    if (square) {
        lv_obj_delete(square);
        square = NULL;
        feed_list = NULL;
    }
}

bool town_square_is_open(void) { return square != NULL; }

void town_square_open(void)
{
    if (square) return;

    const int32_t W = lv_display_get_horizontal_resolution(NULL);
    const int32_t H = lv_display_get_vertical_resolution(NULL);
    const int32_t FEED_W = W * 62 / 100;

    square = lv_obj_create(lv_screen_active());
    lv_obj_set_size(square, lv_pct(100), lv_pct(100));
    lv_obj_set_style_bg_color(square, lv_color_hex(0x14161B), 0);
    lv_obj_set_style_border_width(square, 0, 0);
    lv_obj_set_style_radius(square, 0, 0);
    lv_obj_set_style_pad_all(square, 0, 0);
    lv_obj_clear_flag(square, LV_OBJ_FLAG_SCROLLABLE);

    lv_obj_t *title = lv_label_create(square);
    lv_label_set_text(title, "The town square");
    lv_obj_set_style_text_font(title, &lv_font_montserrat_28, 0);
    lv_obj_set_style_text_color(title, lv_color_white(), 0);
    lv_obj_align(title, LV_ALIGN_TOP_LEFT, 28, 20);

    lbl_count = lv_label_create(square);
    lv_label_set_text(lbl_count, "");
    lv_obj_set_style_text_font(lbl_count, &lv_font_montserrat_14, 0);
    lv_obj_set_style_text_color(lbl_count, lv_color_hex(0x868E96), 0);
    lv_obj_align(lbl_count, LV_ALIGN_TOP_LEFT, 28, 58);

    /* filter chips: tabs are filters over ONE feed, never rooms */
    static const char *names[3] = { "All", "Notices", "Alerts" };
    for (int i = 0; i < 3; i++) {
        chip_btn[i] = lv_button_create(square);
        lv_obj_set_size(chip_btn[i], 120, 44);
        lv_obj_align(chip_btn[i], LV_ALIGN_TOP_LEFT, 28 + i * 132, 88);
        lv_obj_set_style_bg_color(chip_btn[i], lv_color_hex(0x191C22), 0);
        lv_obj_set_style_border_width(chip_btn[i], 1, 0);
        lv_obj_set_style_radius(chip_btn[i], 22, 0);
        lv_obj_add_event_cb(chip_btn[i], chip_cb, LV_EVENT_CLICKED,
                            (void *)(uintptr_t)i);
        lv_obj_t *cl = lv_label_create(chip_btn[i]);
        lv_label_set_text(cl, names[i]);
        lv_obj_set_style_text_font(cl, &lv_font_montserrat_16, 0);
        lv_obj_center(cl);
    }
    chip_paint();

    /* passenger affirmation: honest wording, one tap, session-scoped */
    passenger_btn = lv_button_create(square);
    lv_obj_set_size(passenger_btn, 150, 44);
    lv_obj_align(passenger_btn, LV_ALIGN_TOP_RIGHT, -200, 88);
    lv_obj_set_style_bg_color(passenger_btn, lv_color_hex(0x191C22), 0);
    lv_obj_set_style_border_width(passenger_btn, 1, 0);
    lv_obj_set_style_border_color(passenger_btn, lv_color_hex(0x3B4252), 0);
    lv_obj_set_style_radius(passenger_btn, 22, 0);
    lv_obj_add_event_cb(passenger_btn, passenger_cb, LV_EVENT_CLICKED, NULL);
    lv_obj_t *pl = lv_label_create(passenger_btn);
    lv_label_set_text(pl, "passenger?");
    lv_obj_set_style_text_font(pl, &lv_font_montserrat_16, 0);
    lv_obj_center(pl);
    passenger_paint();

    /* + Post (reuses the report flow, which publishes for real) */
    lv_obj_t *post = lv_button_create(square);
    lv_obj_set_size(post, 150, 44);
    lv_obj_align(post, LV_ALIGN_TOP_RIGHT, -28, 88);
    lv_obj_set_style_bg_color(post, lv_color_hex(0xE8590C), 0);
    lv_obj_set_style_radius(post, 22, 0);
    lv_obj_add_event_cb(post, post_cb, LV_EVENT_CLICKED, NULL);
    lv_obj_t *pol = lv_label_create(post);
    lv_label_set_text(pol, LV_SYMBOL_PLUS "  Post");
    lv_obj_set_style_text_font(pol, &lv_font_montserrat_16, 0);
    lv_obj_center(pol);

    lv_obj_t *close = lv_button_create(square);
    lv_obj_set_size(close, 130, 44);
    lv_obj_align(close, LV_ALIGN_TOP_RIGHT, -28, 20);
    lv_obj_set_style_bg_color(close, lv_color_hex(0x22262E), 0);
    lv_obj_set_style_border_width(close, 1, 0);
    lv_obj_set_style_border_color(close, lv_color_hex(0x3B4252), 0);
    lv_obj_set_style_radius(close, 22, 0);
    lv_obj_add_event_cb(close, close_cb, LV_EVENT_CLICKED, NULL);
    lv_obj_t *xl = lv_label_create(close);
    lv_label_set_text(xl, LV_SYMBOL_GPS "  Map");
    lv_obj_set_style_text_font(xl, &lv_font_montserrat_16, 0);
    lv_obj_center(xl);

    /* the feed: one scrollable column, town-width, centered */
    feed_list = lv_obj_create(square);
    lv_obj_set_size(feed_list, FEED_W, lv_display_get_vertical_resolution(NULL) - 152);
    lv_obj_align(feed_list, LV_ALIGN_TOP_MID, 0, 144);
    lv_obj_set_style_bg_opa(feed_list, LV_OPA_TRANSP, 0);
    lv_obj_set_style_border_width(feed_list, 0, 0);
    lv_obj_set_style_pad_all(feed_list, 0, 0);
    lv_obj_set_style_pad_row(feed_list, 10, 0);
    lv_obj_set_flex_flow(feed_list, LV_FLEX_FLOW_COLUMN);

    (void)H;
    last_sig = 0;
    rebuild(vmesh_time_s());
    last_sig = feed_sig(vmesh_time_s());
}

void town_square_tick(const vmesh_pose_t *pose, uint32_t now_s)
{
    if (!square) return;

    /* the posture gate: driving closes the square unless a still-
     * valid passenger grant is in effect (grants expire on their own) */
    if (pose->speed_mps >= PARKED_MPS && !passenger_valid(now_s)) {
        town_square_close();
        return;
    }
    static bool prev_valid;
    bool v = passenger_valid(now_s);
    if (v != prev_valid) { prev_valid = v; passenger_paint(); }

    uint32_t sig = feed_sig(now_s);
    if (sig != last_sig) {
        last_sig = sig;
        rebuild(now_s);
    }
}

void ui_open_town_square(void)
{
    /* dev hook: sim may be mid-drive; grant a normal timed consent */
    passenger_until = vmesh_time_s() + PASSENGER_TTL_S;
    town_square_open();
}
