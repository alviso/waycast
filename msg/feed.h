/**
 * feed.h — the transport seam (handoff-spec §5).
 *
 * The UI talks ONLY to this interface. Behind it, phase 0 wires the
 * scenario player (sim/); phase 1 wires the real mesh stack — on the P4
 * or on a co-MCU across a UART — without the UI noticing.
 */
#pragma once

#include "vmesh_msg.h"

/* Pull the next incoming message. Returns false when the queue is empty.
 * Called by the UI tick; non-blocking. */
bool vmesh_feed_poll(vmesh_msg_t *out);

/* Push a locally-created message out (user hazard report).
 * Phase 0: the scenario player loops it straight back into the incoming
 * queue, so own reports arrive through the same path as everyone else's. */
void vmesh_feed_publish(const vmesh_msg_t *m);

/* Own-vehicle pose. Scenario track playback now, GPS later. */
void vmesh_pose_get(vmesh_pose_t *out);

/* Current time (seconds, unix-ish). Simulated clock in phase 0 so demos
 * are deterministic and hazard aging matches the scenario script. */
uint32_t vmesh_time_s(void);

/* Real UTC from GPS (RMC date+time): once set, own-reports carry a true
 * timestamp so a real-clock town node accepts them. Refresh each fix. */
void vmesh_time_set_live(uint32_t unix_s);

/* True once any real time source (GPS or an anchor beacon) has set the
 * clock. Gate for anything that shows wall time to a human. */
bool vmesh_time_live(void);

/* ---- live-hardware hooks (USB GPS / USB LoRa transports) ----
 * Callable from a different task than the UI: single-producer,
 * single-consumer semantics per hook. */

/* Real GPS fix: overrides scenario-track pose from first call on. */
void vmesh_pose_set_live(const vmesh_pose_t *p);

/* Last-known position from a prior session (persisted GPS): shown
 * static + stale until the first live fix. Real mode only. */
void vmesh_pose_set_boot(double lat, double lon);

/* True once a live fix has taken over the pose (UI shows the source). */
bool vmesh_pose_is_live(void);

/* GNSS receiver state for the UI badge: 0 = silent (no NMEA at all),
 * 1 = hearing NMEA but no fix yet (acquiring), 2 = fix / locked. */
void vmesh_gps_state_set(int s);
int  vmesh_gps_state(void);

/* Message received off the air: joins the same RX queue the UI polls. */
void vmesh_feed_inject(const vmesh_msg_t *m);

/* Radio TX tap: publish() also hands the message here when set, so
 * user reports/votes leave the device as well as looping back. */
typedef void (*vmesh_tx_hook_t)(const vmesh_msg_t *m);
void vmesh_feed_set_tx_hook(vmesh_tx_hook_t hook);
