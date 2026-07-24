/**
 * scenario.h — scenario player: implements the feed.h seam from a JSON
 * scenario file (PRODUCT_PLAN.md §5). A scenario file is, in effect, a
 * recorded mesh session: a GPS track for the own vehicle plus timed
 * "heard from the mesh" events.
 */
#pragma once

#include <stdbool.h>
#include <stdint.h>

bool scenario_load(const char *json_path);

/* Parse from an in-memory JSON string (embedded in flash on the device
 * target, which has no scenario files on disk). */
bool scenario_load_buf(const char *json, unsigned len);

/* Advance the simulation clock. Call once per main-loop iteration. */
void scenario_update(float dt_s);

/* Demo mode: when OFF (the product default), the scenario injects no
 * fake events and the scripted place-agents / auto-QA stay silent —
 * the device shows only what really arrives over the radio. ON
 * replays the demo. The SDL sim turns it on; the device persists it. */
void scenario_set_demo(bool on);
bool scenario_demo(void);

const char *scenario_name(void);
float scenario_clock_s(void);

/* Build an own-vehicle hazard report at the current pose (used by the
 * UI's two-tap report flow); caller then vmesh_feed_publish()es it. */
#include "vmesh_msg.h"
void scenario_make_own_report(vmesh_msg_t *m, uint8_t hz_type);

/* Build an own attestation about another message (§7¾ confirm/deny). */
/* set this node's unique origin_id (device: efuse MAC; sim: PID) */
void scenario_set_own_origin(uint32_t id);
uint32_t scenario_own_origin(void);
void scenario_set_own_seq(uint16_t seq); /* boot restore (NVS) */
uint32_t scenario_own_origin(void);
void scenario_set_own_seq(uint16_t seq); /* boot restore (NVS) */

void scenario_make_own_query(vmesh_msg_t *m, uint32_t place_origin,
                             const char *question);
void scenario_qa_tick(void);
void scenario_make_own_attest(vmesh_msg_t *m, const vmesh_msg_t *target,
                              uint8_t verdict);
