#include "convoy.h"

#include <stdio.h>
#include <string.h>

#include "feed.h"
#include "scenario.h" /* scenario_stamp_own — own identity + seq */

const char *const convoy_phrases[CONVOY_PHRASES] = {
    "stop ahead", "fuel stop", "wait up", "all good",
};

/* persisted by the device target (NVS); weak no-op in the sim */
void __attribute__((weak)) waycast_save_convoy(const char *phrase)
{
    (void)phrase;
}

static uint32_t s_group;              /* 0 = not in a convoy */
static char     s_phrase[24];
static uint32_t s_last_traffic_s;     /* any frame of ours or theirs */
static uint32_t s_last_beacon_s;
static convoy_member_t s_mem[CONVOY_MAX];

uint32_t convoy_code_id(const char *phrase)
{
    /* FNV-1a over the phrase with spaces dropped and case folded, so
     * "Coast Run" and "coastrun" join the same convoy. Never returns
     * 0 (0 means "no convoy"). */
    uint32_t h = 2166136261u;
    for (const char *p = phrase; p && *p; p++) {
        char c = *p;
        if (c == ' ' || c == '-' || c == '_') continue;
        if (c >= 'A' && c <= 'Z') c = (char)(c - 'A' + 'a');
        h ^= (uint8_t)c;
        h *= 16777619u;
    }
    return h ? h : 1u;
}

void convoy_join(const char *phrase, uint32_t now_s)
{
    if (!phrase || !phrase[0]) { convoy_leave(); return; }
    s_group = convoy_code_id(phrase);
    snprintf(s_phrase, sizeof(s_phrase), "%s", phrase);
    memset(s_mem, 0, sizeof(s_mem));
    s_last_traffic_s = now_s;
    s_last_beacon_s = 0; /* beacon immediately: announce yourself */
    waycast_save_convoy(s_phrase);
}

void convoy_leave(void)
{
    s_group = 0;
    s_phrase[0] = 0;
    memset(s_mem, 0, sizeof(s_mem));
    waycast_save_convoy("");
}

bool convoy_active(void) { return s_group != 0; }
const char *convoy_phrase(void) { return s_phrase; }

bool convoy_note(const vmesh_msg_t *m, uint32_t own_origin, uint32_t now_s)
{
    if (!s_group || m->msg_type != VMESH_MT_CONVOY) return false;
    if (m->ref_origin != s_group) return false; /* someone else's convoy */
    if (m->origin_id == own_origin) return true; /* our own loopback */

    s_last_traffic_s = now_s;
    if (m->hazard_type != CONVOY_SUB_POSITION)
        return false; /* a group message — caller shows it */

    int slot = -1, oldest = 0;
    for (int i = 0; i < CONVOY_MAX; i++) {
        if (s_mem[i].origin == m->origin_id) { slot = i; break; }
        if (!s_mem[i].origin && slot < 0) slot = i;
        if (s_mem[i].heard_s < s_mem[oldest].heard_s) oldest = i;
    }
    if (slot < 0) slot = oldest;
    s_mem[slot].origin = m->origin_id;
    s_mem[slot].lat = m->lat_e7 / 1e7;
    s_mem[slot].lon = m->lon_e7 / 1e7;
    s_mem[slot].heard_s = now_s;
    return true;
}

static void stamp_convoy(vmesh_msg_t *m, uint8_t sub)
{
    scenario_stamp_own(m);
    m->msg_type = VMESH_MT_CONVOY;
    m->channel = VMESH_CH_GROUP;
    m->hazard_type = sub;
    m->ref_origin = s_group;
    m->severity = 1;
    m->radius_m_x10 = 2000;  /* 20 km: a convoy can string out          */
    m->ttl_s = 600;          /* positions are stale fast; messages too  */
}

bool convoy_make_beacon(vmesh_msg_t *out)
{
    if (!s_group) return false;
    stamp_convoy(out, CONVOY_SUB_POSITION);
    out->note[0] = 0;
    return true;
}

bool convoy_make_message(vmesh_msg_t *out, const char *text)
{
    if (!s_group) return false;
    stamp_convoy(out, CONVOY_SUB_MESSAGE);
    snprintf(out->note, sizeof(out->note), "%s", text ? text : "");
    s_last_traffic_s = out->created_s;
    return true;
}

bool convoy_beacon_due(uint32_t now_s)
{
    if (!s_group) return false;
    if (s_last_beacon_s && now_s - s_last_beacon_s < CONVOY_BEACON_S)
        return false;
    s_last_beacon_s = now_s ? now_s : 1;
    return true;
}

void convoy_prune(uint32_t now_s)
{
    if (!s_group) return;
    for (int i = 0; i < CONVOY_MAX; i++)
        if (s_mem[i].origin && now_s - s_mem[i].heard_s > CONVOY_MEMBER_TTL)
            memset(&s_mem[i], 0, sizeof(s_mem[i]));
    /* a trip, not a club: a convoy nobody has spoken in dissolves */
    if (s_last_traffic_s && now_s - s_last_traffic_s > CONVOY_IDLE_TTL)
        convoy_leave();
}

int convoy_count(void)
{
    int n = 0;
    for (int i = 0; i < CONVOY_MAX; i++)
        if (s_mem[i].origin) n++;
    return n;
}

convoy_member_t *convoy_slot(int i)
{
    return (i >= 0 && i < CONVOY_MAX) ? &s_mem[i] : 0;
}

bool convoy_is_ours(const vmesh_msg_t *m)
{
    return s_group && m->msg_type == VMESH_MT_CONVOY &&
           m->ref_origin == s_group;
}
