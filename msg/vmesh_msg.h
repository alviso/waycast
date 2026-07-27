/**
 * vmesh_msg.h — the message model (handoff-spec §7).
 *
 * This struct is THE seam. It crosses three boundaries with one definition:
 *   1. LoRa over-the-air (packed wire encoding, later)
 *   2. mesh-processor <-> app-processor (feed.h — scenario player today,
 *      real mesh stack later, co-MCU possibly after that)
 *   3. device <-> phone over BLE (later)
 *
 * Phase 0 uses it unpacked between the scenario player and the UI.
 */
#pragma once

#include <stdint.h>
#include <stdbool.h>

#define VMESH_PROTO_VERSION 0

/* ---- message types ---- */
enum {
    VMESH_MT_BEACON = 0,   /* position beacon (not used in phase 0) */
    VMESH_MT_HAZARD = 1,
    VMESH_MT_TRAFFIC = 2,
    VMESH_MT_TEXT = 3,
    VMESH_MT_QUERY  = 5,   /* ask a place a question (ref_origin =
                              target place, note = the question) */
    VMESH_MT_REPLY  = 6,   /* place agent's answer (ref = the query,
                              note = the answer) */
    VMESH_MT_ATTEST = 4,   /* corroboration vote about another message
                              (plan §7¾): ref_origin/ref_seq name the
                              target, hazard_type carries the verdict */
    VMESH_MT_CONVOY = 8,   /* trip-scoped group traffic (ui/convoy.h):
                              note = 8-hex group id prefix [+ text],
                              hazard_type = 0 position / 1 message */
    VMESH_MT_HELLO  = 7,   /* identity: note = the sender's handle
                              (§7¾ locally persistent pseudonym; a
                              label on origin_id, never a key). Sent
                              on boot + every ~10 min; receivers cache
                              origin→handle and FORGET after 24 h —
                              ephemerality applies to names too. */
};

/* ATTEST verdicts (carried in hazard_type) */
enum {
    VMESH_VERDICT_DENY = 0,    /* "not there" */
    VMESH_VERDICT_CONFIRM = 1, /* "confirmed" */
};

/* ---- channels (plan §7.5: hyperlocal intelligence, not just hazards).
 * Safety interrupts the driver; LOCAL is browsed when stopped. ---- */
enum {
    VMESH_CH_SAFETY = 0,
    VMESH_CH_TRAFFIC = 1,
    VMESH_CH_LOCAL = 2,    /* events, lodging, fuel, aid — local chatter */
    VMESH_CH_GROUP = 3,    /* convoy traffic: above LOCAL in the
                              interrupt ladder, below SAFETY */
};

/* ---- LOCAL-channel categories ---- */
enum {
    VMESH_LC_LODGING = 0,
    VMESH_LC_FUEL,
    VMESH_LC_EVENT,
    VMESH_LC_AID,
    VMESH_LC_INFO,
    VMESH_LC_COUNT,
};

/* ---- hazard taxonomy v0 (PRODUCT_PLAN.md §3) ---- */
enum {
    VMESH_HZ_CRASH = 0,
    VMESH_HZ_SLOWDOWN,
    VMESH_HZ_DEBRIS,
    VMESH_HZ_POTHOLE,
    VMESH_HZ_WEATHER,
    VMESH_HZ_CLOSURE,
    VMESH_HZ_EMERGENCY,
    VMESH_HZ_COUNT,
};

/* severity: 1 = info, 2 = caution, 3 = danger */

/* types whose wire position slots carry a message reference instead */
/* NOTE: CONVOY is deliberately NOT here — HAS_REF types overlay the
 * ref fields onto the position/ttl bytes, and a convoy POSITION
 * beacon without a position is a haiku, not a protocol (v0.8.1
 * shipped that way; the town node's journal caught it: @0,0 ttl=0).
 * Convoy group id rides as an 8-hex-char prefix in note instead. */
#define VMESH_MT_HAS_REF(t) ((t) == VMESH_MT_ATTEST || \
                             (t) == VMESH_MT_QUERY  || \
                             (t) == VMESH_MT_REPLY)

typedef struct {
    uint8_t  version;      /* protocol evolution (§7)                    */
    uint8_t  msg_type;     /* VMESH_MT_*                                 */
    uint8_t  hazard_type;  /* VMESH_HZ_* when msg_type == HAZARD         */
    uint8_t  severity;     /* 1..3                                       */
    uint32_t origin_id;    /* node id      ┐                             */
    uint16_t seq;          /*              ┘ -> dedup id                 */
    int32_t  lat_e7;       /* fixed-point 1e-7 deg (§7 compact encoding) */
    int32_t  lon_e7;
    uint32_t created_s;    /* unix time                                  */
    uint16_t ttl_s;        /* expiry = created + ttl                     */
    uint16_t radius_m_x10; /* relevance radius, 10 m units               */
    uint8_t  hops;         /* hop count when heard                       */
    uint8_t  channel;      /* VMESH_CH_*; for LOCAL, hazard_type holds
                              the VMESH_LC_* category                    */
    uint32_t ref_origin;   /* ATTEST only: target message's origin ┐    */
    uint16_t ref_seq;      /*                          and seq     ┘    */
    uint8_t  _reserved;    /* auth headroom (v1 adds a wire trailer)    */
    char     note[40];     /* short text payload (chatter needs it OTA;
                              fits any LoRa DR payload budget)           */
} vmesh_msg_t;

/* Own-vehicle pose. Scenario playback in phase 0, GPS/NMEA later. */
typedef struct {
    double lat;
    double lon;
    float  heading_deg;    /* 0 = north, clockwise */
    float  speed_mps;
} vmesh_pose_t;

/* ---- taxonomy defaults (used by the two-tap report flow) ---- */
typedef struct {
    const char *name;
    uint8_t     severity;
    uint16_t    ttl_s_default;
    uint16_t    radius_m_x10_default;
} vmesh_hz_info_t;

static const vmesh_hz_info_t VMESH_HZ_INFO[VMESH_HZ_COUNT] = {
    [VMESH_HZ_CRASH]     = { "Crash",     3, 1800,  500 },
    [VMESH_HZ_SLOWDOWN]  = { "Slowdown",  2,  900,  500 },
    [VMESH_HZ_DEBRIS]    = { "Debris",    2, 3600,  300 },
    [VMESH_HZ_POTHOLE]   = { "Pothole",   1, 43200, 200 }, /* ttl capped u16 */
    [VMESH_HZ_WEATHER]   = { "Weather",   2, 3600,  800 },
    [VMESH_HZ_CLOSURE]   = { "Closure",   2, 14400, 1000 },
    [VMESH_HZ_EMERGENCY] = { "Emergency", 2, 1200,  400 },
};

static inline const char *vmesh_hz_name(uint8_t t)
{
    return (t < VMESH_HZ_COUNT) ? VMESH_HZ_INFO[t].name : "?";
}

/* LOCAL-channel category defaults: long TTLs, wide radii — "rooms
 * available" lives hours; still geo-ephemeral, just slower. */
static const vmesh_hz_info_t VMESH_LOCAL_INFO[VMESH_LC_COUNT] = {
    [VMESH_LC_LODGING] = { "Lodging", 1, 14400, 1000 },
    [VMESH_LC_FUEL]    = { "Fuel",    1,  7200,  800 },
    [VMESH_LC_EVENT]   = { "Event",   1, 28800,  800 },
    [VMESH_LC_AID]     = { "Aid",     1, 14400, 1500 },
    [VMESH_LC_INFO]    = { "Info",    1,  7200,  500 },
};

static inline const char *vmesh_local_name(uint8_t c)
{
    return (c < VMESH_LC_COUNT) ? VMESH_LOCAL_INFO[c].name : "?";
}

/* channel-aware display name for any message */
static inline const char *vmesh_msg_name(uint8_t channel, uint8_t type)
{
    return (channel == VMESH_CH_LOCAL) ? vmesh_local_name(type)
                                       : vmesh_hz_name(type);
}
