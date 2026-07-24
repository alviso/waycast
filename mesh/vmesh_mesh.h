/**
 * vmesh_mesh.h — the geo-ephemeral flooding core (spec §2).
 *
 * The whole forwarding decision, exactly as the spec states it:
 *   1. seen this (origin, seq) already?          -> drop
 *   2. past its expiry?                          -> drop
 *   3. am I outside its relevance radius?        -> drop
 *   4. otherwise: rebroadcast ONCE — after a random jitter, and only
 *      if fewer than N neighbors were overheard relaying it first
 *      (counter-based suppression, §8 broadcast-storm mitigation).
 *
 * Pure portable C. No OS, no clock, no radio: the caller feeds it
 * time, position, and frames; it calls back when something should be
 * transmitted. The same code runs on the P4, in the desktop simulator,
 * in the multi-node harness (tools/meshsim), and someday on a co-MCU —
 * which is the §5 "small auditable mesh firmware" promise kept.
 */
#pragma once

#include "vmesh_msg.h"

#include <stdbool.h>
#include <stdint.h>

#define MESH_SEEN_SIZE 128    /* dedup cache entries                */
#define MESH_PENDING_SIZE 16  /* relays waiting out their jitter    */
#define MESH_MAX_HOPS 8       /* safety valve on top of the radius  */

typedef struct {
    uint32_t jitter_min_ms;      /* relay delay window (must exceed   */
    uint32_t jitter_max_ms;      /* one airtime slot; default 100-800)*/
    uint8_t  suppress_threshold; /* cancel relay after overhearing    */
                                 /* this many copies (0 = never)      */
} mesh_config_t;

typedef struct {
    uint32_t rx_total;      /* frames handed to mesh_rx                */
    uint32_t delivered;     /* frames passed up to the application     */
    uint32_t dup_dropped;   /* dedup hits                              */
    uint32_t expired_drop;  /* stale on arrival                        */
    uint32_t radius_drop;   /* outside relevance radius (not relayed)  */
    uint32_t hop_drop;      /* hop-cap exceeded                        */
    uint32_t relayed;       /* rebroadcasts actually transmitted       */
    uint32_t suppressed;    /* relays cancelled by overheard copies    */
} mesh_stats_t;

typedef struct {
    vmesh_msg_t msg;
    uint32_t    due_ms;
    uint8_t     overheard;
    bool        used;
} mesh_pending_t;

typedef struct {
    mesh_config_t cfg;
    uint32_t      seen_key[MESH_SEEN_SIZE]; /* origin ^ (seq<<?) mix   */
    uint64_t      seen_id[MESH_SEEN_SIZE];  /* full (origin,seq)       */
    int           seen_next;
    mesh_pending_t pending[MESH_PENDING_SIZE];
    uint32_t      rng;
    mesh_stats_t  stats;
} vmesh_mesh_t;

typedef void (*mesh_tx_cb_t)(const vmesh_msg_t *m, void *user);

void mesh_init(vmesh_mesh_t *m, const mesh_config_t *cfg, uint32_t seed);

/* A frame arrived off the radio. Returns true if the application
 * should see it (fresh, first copy, relevant here). Relay scheduling
 * happens internally.
 *
 * proximity: how close the SENDER seems, 0.0 (edge of range) to 1.0
 * (on top of us) — derive from RSSI on hardware; pass 0.5 if unknown.
 * Far receivers relay sooner (contention-based forwarding): the relay
 * that makes the most geographic progress usually wins, and everyone
 * nearer hears it and suppresses. */
/* Clock-skew normalization (July 25 — the strict-clock gate silently
 * deafened the mesh four times in one week). A flood mesh propagates
 * in seconds, so at RECEPTION: a stamp within the sanity window is
 * trusted (ages/expiry as stamped); an implausible stamp (clock-less
 * sender, decades of skew) is re-anchored to now, ttl running as a
 * duration from here. Relays re-encode the normalized stamp, so one
 * clocked hop repairs the frame for everyone downstream. Replay risk
 * is bounded by dedup + the hop cap + corroboration (§7¾).
 * Call on every received frame BEFORE mesh_rx/ingest. */
#define VMESH_CLOCK_SKEW_S 600
void vmesh_clock_normalize(vmesh_msg_t *m, uint32_t now_s);

bool mesh_rx(vmesh_mesh_t *m, const vmesh_msg_t *msg, float proximity,
             double my_lat, double my_lon, uint32_t now_ms,
             uint32_t now_unix_s);

/* Advance time: transmit any relays whose jitter has elapsed. Call
 * every few tens of ms. */
void mesh_tick(vmesh_mesh_t *m, uint32_t now_ms,
               mesh_tx_cb_t tx, void *user);

/* Register a locally-originated message (so our own report echoed
 * back by a neighbor isn't relayed again). Caller transmits it. */
void mesh_note_own(vmesh_mesh_t *m, const vmesh_msg_t *msg);
