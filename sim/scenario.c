/* scenario.c — JSON scenario player behind the feed.h seam. */

#include "scenario.h"
#include "feed.h"

#include "cJSON/cJSON.h"

#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define MAX_WAYPOINTS 256
#define MAX_EVENTS    64
#define QUEUE_LEN     32

/* Deterministic epoch so hazard aging is reproducible run-to-run. */
#define SIM_EPOCH 1750000000u

#define OWN_ORIGIN_ID 0x00000001u /* default until set per-node */
static uint32_t s_own_origin = OWN_ORIGIN_ID;

/* Each physical node MUST have a unique origin_id, or two nodes stamp
 * their own reports identically and the mesh dedups one as the other's
 * echo (device + sim both defaulted to 1 -> cross-drops). Device sets
 * this from its efuse MAC; the sim from its PID. */
void scenario_set_own_origin(uint32_t id) { if (id) s_own_origin = id; }
uint32_t scenario_own_origin(void) { return s_own_origin; }

static uint16_t next_own_seq(void); /* defined with the seq persistence */

/* Own handle (§7¾ pseudonym): a label riding HELLO frames. Persisted
 * by the device target (NVS); weak no-op in the sim. */
static char s_own_handle[16];
void __attribute__((weak)) waycast_save_handle(const char *h) { (void)h; }
void vmesh_set_own_handle(const char *h)
{
    snprintf(s_own_handle, sizeof(s_own_handle), "%s", h ? h : "");
}
const char *vmesh_own_handle(void) { return s_own_handle; }

void scenario_make_own_hello(vmesh_msg_t *m)
{
    memset(m, 0, sizeof(*m));
    m->version = VMESH_PROTO_VERSION;
    m->msg_type = VMESH_MT_HELLO;
    m->channel = VMESH_CH_SAFETY;
    m->origin_id = s_own_origin;
    m->seq = next_own_seq();
    vmesh_pose_t p;
    vmesh_pose_get(&p);
    m->lat_e7 = (int32_t)(p.lat * 1e7);
    m->lon_e7 = (int32_t)(p.lon * 1e7);
    m->created_s = vmesh_time_s();
    m->ttl_s = 900;
    m->radius_m_x10 = 500; /* 5 km — names are local knowledge */
    snprintf(m->note, sizeof(m->note), "%s", s_own_handle);
}

/* Own message seq persists across reboots (a boot-reset seq lets the
 * network's dedup silently eat a fresh report that reuses an already-
 * seen origin/seq pair). Device target persists via NVS; sim no-op. */
void __attribute__((weak)) waycast_save_seq(uint16_t seq) { (void)seq; }
void scenario_set_own_seq(uint16_t seq);

typedef struct {
    float  t;
    double lat, lon;
} waypoint_t;

typedef struct {
    float       fire_t;
    bool        fired;
    vmesh_msg_t msg;
} sim_event_t;

static struct {
    char        name[64];
    waypoint_t  wp[MAX_WAYPOINTS];
    int         n_wp;
    sim_event_t ev[MAX_EVENTS];
    int         n_ev;

    /* incoming-message ring buffer (the "radio RX queue") */
    vmesh_msg_t q[QUEUE_LEN];
    int         q_head, q_tail;

    float    clock_s;
    float    duration_s; /* last track waypoint time; 0 = no looping */
    uint16_t lap;        /* wraps completed (offsets seq per lap)    */
    uint16_t own_seq;
} S;

static bool s_demo; /* product default OFF: real radio traffic only */

/* ---- place agent: scripted Q&A (Tier 2.5 prototype) ----
 * When a QUERY targets a scripted LOCAL event's origin, the "agent"
 * answers after a beat — same feed path a radio reply will use. */
#define PENDING_QA 4
static struct {
    bool        used;
    float       fire_t;
    vmesh_msg_t reply;
} qa[PENDING_QA];

/* ---------------- queue ---------------- */

static void q_push(const vmesh_msg_t *m)
{
    int next = (S.q_head + 1) % QUEUE_LEN;
    if (next == S.q_tail) return; /* full: drop (it's best-effort, after all) */
    S.q[S.q_head] = *m;
    S.q_head = next;
}

/* ---- live-hardware side doors (see feed.h) ----
 * liveq: SPSC ring — producer is the USB task, consumer is the UI
 * task; 32-bit index writes are atomic on our targets. live_pose:
 * double-buffered with an index flip to avoid torn reads. */
static vmesh_msg_t liveq[8];
static volatile unsigned lq_head, lq_tail;
static vmesh_pose_t live_pose[2];
static volatile unsigned live_idx; /* which slot is valid to read */
static volatile bool live_pose_on;
static vmesh_tx_hook_t tx_hook;

void vmesh_feed_inject(const vmesh_msg_t *m)
{
    unsigned next = (lq_head + 1) % 8;
    if (next == lq_tail) return; /* full: drop, it's best-effort */
    liveq[lq_head] = *m;
    lq_head = next;
}

bool vmesh_pose_is_live(void) { return live_pose_on; }

/* last-known position (persisted GPS from a prior session): shown,
 * static and marked stale, until a fresh fix takes over. */
static vmesh_pose_t s_boot_pose;
static bool s_boot_pose_on;
void vmesh_pose_set_boot(double lat, double lon)
{
    s_boot_pose.lat = lat;
    s_boot_pose.lon = lon;
    s_boot_pose.speed_mps = 0;
    s_boot_pose.heading_deg = 0;
    s_boot_pose_on = true;
}

static volatile int gps_state;
void vmesh_gps_state_set(int st) { gps_state = st; }
int vmesh_gps_state(void) { return gps_state; }

void vmesh_pose_set_live(const vmesh_pose_t *p)
{
    unsigned w = 1 - live_idx;
    live_pose[w] = *p;
    live_idx = w;
    live_pose_on = true;
    gps_state = 2;
}

void vmesh_feed_set_tx_hook(vmesh_tx_hook_t hook) { tx_hook = hook; }

bool vmesh_feed_poll(vmesh_msg_t *out)
{
    if (lq_tail != lq_head) { /* radio traffic first */
        *out = liveq[lq_tail];
        lq_tail = (lq_tail + 1) % 8;
        return true;
    }
    if (S.q_tail == S.q_head) return false;
    *out = S.q[S.q_tail];
    S.q_tail = (S.q_tail + 1) % QUEUE_LEN;
    return true;
}

/* canned agent answers: category x question keyword -> <=40 chars */
static const char *agent_answer(uint8_t cat, const char *q)
{
    bool hours = strstr(q, "our") || strstr(q, "pen");
    bool wait  = strstr(q, "ait");
    bool today = strstr(q, "oday") || strstr(q, "pecial");
    switch (cat) {
    case VMESH_LC_FUEL:
        if (hours) return "open 24h, pay at pump after 10pm";
        if (wait)  return "no line right now";
        if (today) return "reg $3.89 - E85 $3.19";
        return "2 stalls free, cash discount 5c";
    case VMESH_LC_LODGING:
        if (hours) return "front desk till 11pm";
        if (wait)  return "2 rooms left tonight";
        if (today) return "queen $89 - incl breakfast";
        return "pets ok, quiet rooms in back";
    case VMESH_LC_EVENT:
        if (hours) return "till 9pm tonight";
        if (wait)  return "short line, moving fast";
        if (today) return "live music from 7";
        return "free entry, food trucks on site";
    case VMESH_LC_AID:
        if (hours) return "staffed until 20:00";
        if (wait)  return "walk-ins ok, ~15 min";
        return "water + first aid available";
    default:
        if (hours) return "open till 18:00 today";
        if (wait)  return "about 10 min right now";
        if (today) return "fresh sourdough till it lasts";
        return "ask at the counter - we're here";
    }
}

static void agent_consider(const vmesh_msg_t *m)
{
    if (!s_demo) return; /* no scripted place-agents in real mode */
    if (m->msg_type != VMESH_MT_QUERY) return;
    for (int e = 0; e < S.n_ev; e++) {
        const vmesh_msg_t *pm = &S.ev[e].msg;
        if (pm->channel != VMESH_CH_LOCAL) continue;
        if (pm->origin_id != m->ref_origin) continue;
        for (int i = 0; i < PENDING_QA; i++) {
            if (qa[i].used) continue;
            qa[i].used = true;
            qa[i].fire_t = S.clock_s + 1.6f; /* radio-ish beat */
            vmesh_msg_t *r = &qa[i].reply;
            memset(r, 0, sizeof(*r));
            r->version    = VMESH_PROTO_VERSION;
            r->msg_type   = VMESH_MT_REPLY;
            r->channel    = VMESH_CH_LOCAL;
            r->origin_id  = pm->origin_id;
            r->seq        = (uint16_t)(1000u + i + S.lap * 100u);
            r->ref_origin = m->origin_id;
            r->ref_seq    = m->seq;
            r->created_s  = vmesh_time_s();
            snprintf(r->note, sizeof(r->note), "%s",
                     agent_answer(pm->hazard_type, m->note));
            return;
        }
    }
}

void vmesh_feed_publish(const vmesh_msg_t *m)
{
    /* Phase 0: loop own reports straight back in, so the UI hears itself
     * through the same path it hears everyone else. */
    q_push(m);
    agent_consider(m); /* scripted place agents (Tier 2.5 demo) */
    if (tx_hook) tx_hook(m); /* and out over the radio when present */
}

/* Real wall-clock from GPS (RMC date+time). Once set, it replaces the
 * fixed SIM_EPOCH so own-reports carry a true timestamp — otherwise a
 * real-clock town node rejects them as long-expired. Refreshed every
 * RMC (~1 Hz); 0 = no GPS time yet, fall back to the sim epoch. */
static volatile uint32_t s_gps_unix;
void vmesh_time_set_live(uint32_t unix_s) { s_gps_unix = unix_s; }

uint32_t vmesh_time_s(void)
{
    if (s_gps_unix) return s_gps_unix;
    return SIM_EPOCH + (uint32_t)S.clock_s;
}

void scenario_qa_tick(void)
{
    for (int i = 0; i < PENDING_QA; i++) {
        if (!qa[i].used || S.clock_s < qa[i].fire_t) continue;
        qa[i].used = false;
        qa[i].reply.created_s = vmesh_time_s();
        q_push(&qa[i].reply);
    }
}

float scenario_clock_s(void) { return S.clock_s; }
const char *scenario_name(void) { return S.name; }

/* ---------------- pose from track ---------------- */

void vmesh_pose_get(vmesh_pose_t *out)
{
    if (live_pose_on) { /* real GPS wins from its first fix onward */
        *out = live_pose[live_idx];
        return;
    }

    /* real mode, no live fix yet: sit still at the last-known position
     * (or the map's home waypoint), never drift along the demo track */
    if (!s_demo) {
        memset(out, 0, sizeof(*out));
        if (s_boot_pose_on) *out = s_boot_pose;
        else if (S.n_wp > 0) { out->lat = S.wp[0].lat; out->lon = S.wp[0].lon; }
        return;
    }

    memset(out, 0, sizeof(*out));
    if (S.n_wp == 0) return;

    if (S.n_wp == 1 || S.clock_s <= S.wp[0].t) {
        out->lat = S.wp[0].lat;
        out->lon = S.wp[0].lon;
    }

    /* find the active segment */
    int i = 0;
    while (i < S.n_wp - 2 && S.wp[i + 1].t <= S.clock_s) i++;
    const waypoint_t *a = &S.wp[i], *b = &S.wp[i + 1];

    float span = b->t - a->t;
    float f = (span > 0.f) ? (S.clock_s - a->t) / span : 1.f;
    if (f < 0.f) f = 0.f;
    if (f > 1.f) f = 1.f; /* past the end: park at the last waypoint */

    out->lat = a->lat + (b->lat - a->lat) * f;
    out->lon = a->lon + (b->lon - a->lon) * f;

    /* heading + speed from the segment */
    double mlat = (a->lat + b->lat) / 2.0;
    double dy_m = (b->lat - a->lat) * 110540.0;
    double dx_m = (b->lon - a->lon) * 111320.0 * cos(mlat * M_PI / 180.0);
    out->heading_deg = (float)(atan2(dx_m, dy_m) * 180.0 / M_PI);
    if (out->heading_deg < 0) out->heading_deg += 360.f;
    out->speed_mps = (span > 0.f && f < 1.f)
                         ? (float)(sqrt(dx_m * dx_m + dy_m * dy_m) / span)
                         : 0.f;
}

/* ---------------- scenario loading ---------------- */

static uint8_t local_cat_from_str(const char *s)
{
    static const char *ids[VMESH_LC_COUNT] = {
        "lodging", "fuel", "event", "aid", "info",
    };
    for (uint8_t i = 0; i < VMESH_LC_COUNT; i++)
        if (strcasecmp(s, ids[i]) == 0) return i;
    return VMESH_LC_INFO;
}

static uint8_t hz_type_from_str(const char *s)
{
    for (uint8_t i = 0; i < VMESH_HZ_COUNT; i++)
        if (strcasecmp(s, VMESH_HZ_INFO[i].name) == 0) return i;
    /* also accept lowercase short ids used in scenario files */
    static const char *ids[VMESH_HZ_COUNT] = {
        "crash", "slowdown", "debris", "pothole",
        "weather", "closure", "emergency",
    };
    for (uint8_t i = 0; i < VMESH_HZ_COUNT; i++)
        if (strcasecmp(s, ids[i]) == 0) return i;
    return VMESH_HZ_DEBRIS;
}

/* tolerant field access: absent/malformed JSON numbers become a
 * default instead of a NULL-deref (a scenario file must never be able
 * to crash the app) */
static double num_field(const cJSON *item, const char *name, double dflt)
{
    const cJSON *v = cJSON_GetObjectItem(item, name);
    return cJSON_IsNumber(v) ? v->valuedouble : dflt;
}

static uint32_t origin_hash(const char *s)
{
    uint32_t h = 2166136261u; /* FNV-1a */
    while (*s) { h ^= (uint8_t)*s++; h *= 16777619u; }
    return h ? h : 0xFFFFFFFFu;
}

bool scenario_load(const char *json_path)
{
    FILE *fp = fopen(json_path, "rb");
    if (!fp) {
        fprintf(stderr, "scenario: cannot open %s\n", json_path);
        return false;
    }
    fseek(fp, 0, SEEK_END);
    long len = ftell(fp);
    fseek(fp, 0, SEEK_SET);
    char *buf = malloc((size_t)len + 1);
    fread(buf, 1, (size_t)len, fp);
    buf[len] = 0;
    fclose(fp);

    bool ok = scenario_load_buf(buf, (unsigned)len);
    free(buf);
    return ok;
}

bool scenario_load_buf(const char *json, unsigned len)
{
    memset(&S, 0, sizeof(S));

    cJSON *root = cJSON_ParseWithLength(json, len);
    if (!root) {
        fprintf(stderr, "scenario: JSON parse error\n");
        return false;
    }

    const cJSON *j = cJSON_GetObjectItem(root, "name");
    snprintf(S.name, sizeof(S.name), "%s",
             cJSON_IsString(j) ? j->valuestring : "unnamed");

    const cJSON *track = cJSON_GetObjectItem(root, "track");
    const cJSON *item;
    cJSON_ArrayForEach(item, track) {
        if (S.n_wp >= MAX_WAYPOINTS) break;
        waypoint_t *w = &S.wp[S.n_wp++];
        w->t   = (float)num_field(item, "t", 0);
        w->lat = num_field(item, "lat", 0);
        w->lon = num_field(item, "lon", 0);
    }

    if (S.n_wp >= 2) S.duration_s = S.wp[S.n_wp - 1].t;

    const cJSON *events = cJSON_GetObjectItem(root, "events");
    uint16_t seq = 100;
    cJSON_ArrayForEach(item, events) {
        if (S.n_ev >= MAX_EVENTS) break;
        sim_event_t *e = &S.ev[S.n_ev++];
        vmesh_msg_t *m = &e->msg;

        e->fire_t = (float)num_field(item, "t", 0);

        const cJSON *type = cJSON_GetObjectItem(item, "type");
        const char *type_s =
            cJSON_IsString(type) ? type->valuestring : "debris";

        /* attestation events: {"type":"attest","ref":<event index>,
         * "verdict":"confirm"|"deny","origin":...} */
        if (strcasecmp(type_s, "attest") == 0) {
            const cJSON *ref = cJSON_GetObjectItem(item, "ref");
            int ri = cJSON_IsNumber(ref) ? ref->valueint : -1;
            if (ri < 0 || ri >= S.n_ev - 1) { S.n_ev--; continue; }
            const cJSON *vd = cJSON_GetObjectItem(item, "verdict");
            m->version     = VMESH_PROTO_VERSION;
            m->msg_type    = VMESH_MT_ATTEST;
            m->hazard_type = (cJSON_IsString(vd) &&
                              strcasecmp(vd->valuestring, "deny") == 0)
                                 ? VMESH_VERDICT_DENY
                                 : VMESH_VERDICT_CONFIRM;
            m->ref_origin = S.ev[ri].msg.origin_id;
            m->ref_seq    = S.ev[ri].msg.seq;
            m->seq        = seq++;
            const cJSON *o = cJSON_GetObjectItem(item, "origin");
            m->origin_id = cJSON_IsString(o) ? origin_hash(o->valuestring)
                                             : origin_hash("SIM-UNKNOWN");
            continue;
        }

        const cJSON *ch = cJSON_GetObjectItem(item, "channel");
        bool is_local =
            cJSON_IsString(ch) && strcasecmp(ch->valuestring, "local") == 0;

        const vmesh_hz_info_t *info;
        m->version = VMESH_PROTO_VERSION;
        if (is_local) {
            uint8_t lc = local_cat_from_str(type_s);
            info           = &VMESH_LOCAL_INFO[lc];
            m->channel     = VMESH_CH_LOCAL;
            m->msg_type    = VMESH_MT_TEXT;
            m->hazard_type = lc;
        } else {
            uint8_t hz = hz_type_from_str(type_s);
            info           = &VMESH_HZ_INFO[hz];
            m->channel     = VMESH_CH_SAFETY;
            m->msg_type    = VMESH_MT_HAZARD;
            m->hazard_type = hz;
        }
        m->severity = info->severity;
        m->lat_e7 = (int32_t)(num_field(item, "lat", 0) * 1e7);
        m->lon_e7 = (int32_t)(num_field(item, "lon", 0) * 1e7);
        m->ttl_s        = info->ttl_s_default;
        m->radius_m_x10 = info->radius_m_x10_default;
        m->seq          = seq++;
        m->hops         = 1;

        const cJSON *v;
        if ((v = cJSON_GetObjectItem(item, "severity")) && cJSON_IsNumber(v))
            m->severity = (uint8_t)v->valueint;
        if ((v = cJSON_GetObjectItem(item, "expiry_s")) && cJSON_IsNumber(v))
            m->ttl_s = (uint16_t)v->valueint;
        if ((v = cJSON_GetObjectItem(item, "radius_m")) && cJSON_IsNumber(v))
            m->radius_m_x10 = (uint16_t)(v->valueint / 10);
        if ((v = cJSON_GetObjectItem(item, "hops")) && cJSON_IsNumber(v))
            m->hops = (uint8_t)v->valueint;
        if ((v = cJSON_GetObjectItem(item, "origin")) && cJSON_IsString(v))
            m->origin_id = origin_hash(v->valuestring);
        else
            m->origin_id = origin_hash("SIM-UNKNOWN");
        if ((v = cJSON_GetObjectItem(item, "note")) && cJSON_IsString(v))
            snprintf(m->note, sizeof(m->note), "%s", v->valuestring);
    }

    cJSON_Delete(root);
    printf("scenario: \"%s\" — %d waypoints, %d events\n",
           S.name, S.n_wp, S.n_ev);
    return S.n_wp >= 2;
}

/* ---------------- tick ---------------- */

void scenario_set_demo(bool on) { s_demo = on; }
bool scenario_demo(void) { return s_demo; }

void scenario_update(float dt_s)
{
    S.clock_s += dt_s;
    if (!s_demo) return; /* real mode: no scripted events, no agents */
    scenario_qa_tick();

    /* seamless looping: the demo route is a closed circuit, so wrap
     * the clock and re-arm every event. Re-fired events get a per-lap
     * seq offset — otherwise the mesh/store dedup (correctly!) drops
     * them as duplicates of last lap. */
    if (S.duration_s > 0 && S.clock_s >= S.duration_s) {
        S.clock_s -= S.duration_s;
        S.lap++;
        for (int i = 0; i < S.n_ev; i++) S.ev[i].fired = false;
        printf("scenario: lap %u\n", (unsigned)(S.lap + 1));
    }

    for (int i = 0; i < S.n_ev; i++) {
        sim_event_t *e = &S.ev[i];
        if (e->fired || e->fire_t > S.clock_s) continue;
        e->fired = true;
        vmesh_msg_t m = e->msg;
        m.seq = (uint16_t)(m.seq + S.lap * 100u);
        if (m.msg_type == VMESH_MT_ATTEST)
            m.ref_seq = (uint16_t)(m.ref_seq + S.lap * 100u);
        m.created_s = vmesh_time_s();
        q_push(&m);
    }
}

/* Helper for the UI's report flow: build a hazard at the current pose. */
void scenario_make_own_report(vmesh_msg_t *m, uint8_t hz_type)
{
    const vmesh_hz_info_t *info = &VMESH_HZ_INFO[hz_type];
    vmesh_pose_t pose;
    vmesh_pose_get(&pose);

    memset(m, 0, sizeof(*m));
    m->version      = VMESH_PROTO_VERSION;
    m->msg_type     = VMESH_MT_HAZARD;
    m->hazard_type  = hz_type;
    m->severity     = info->severity;
    m->origin_id    = s_own_origin;
    m->seq          = next_own_seq();
    m->lat_e7       = (int32_t)(pose.lat * 1e7);
    m->lon_e7       = (int32_t)(pose.lon * 1e7);
    m->created_s    = vmesh_time_s();
    m->ttl_s        = info->ttl_s_default;
    m->radius_m_x10 = info->radius_m_x10_default;
    m->hops         = 0;
    snprintf(m->note, sizeof(m->note), "your report");
}

/* Tier 2.5: ask a place a question (parked flow) */
void scenario_make_own_query(vmesh_msg_t *m, uint32_t place_origin,
                             const char *question)
{
    memset(m, 0, sizeof(*m));
    m->version    = VMESH_PROTO_VERSION;
    m->msg_type   = VMESH_MT_QUERY;
    m->channel    = VMESH_CH_LOCAL;
    m->origin_id  = s_own_origin;
    m->seq        = next_own_seq();
    m->ref_origin = place_origin;
    m->created_s  = vmesh_time_s();
    snprintf(m->note, sizeof(m->note), "%s", question);
}

void scenario_make_own_attest(vmesh_msg_t *m, const vmesh_msg_t *target,
                              uint8_t verdict)
{
    memset(m, 0, sizeof(*m));
    m->version     = VMESH_PROTO_VERSION;
    m->msg_type    = VMESH_MT_ATTEST;
    m->hazard_type = verdict;
    m->origin_id   = s_own_origin;
    m->seq         = next_own_seq();
    m->ref_origin  = target->origin_id;
    m->ref_seq     = target->seq;
    m->created_s   = vmesh_time_s();
}

static uint16_t next_own_seq(void)
{
    uint16_t v = ++S.own_seq;
    waycast_save_seq(v);
    return v;
}

void scenario_set_own_seq(uint16_t seq)
{
    if (seq > S.own_seq) S.own_seq = seq;
}
