/**
 * convoy.h — trip-scoped groups (plan: convoy-first groups).
 *
 * Join by CODE WORD: both drivers type the same phrase, it hashes to a
 * group id, and that's the whole handshake — no server, no invitations,
 * no accounts. Works at a gas station in twenty seconds.
 *
 * A convoy is a TRIP, not a club: members age out after a few minutes
 * of silence, and the convoy itself dissolves after half a day without
 * group traffic. Nothing to clean up, nothing to leave behind.
 *
 * Wire: VMESH_MT_CONVOY on VMESH_CH_GROUP, group id in ref_origin,
 * hazard_type as subtype (0 = position beacon, 1 = group message).
 * v1 is CLEARTEXT — anyone with a dongle and the code word (or a
 * little patience) can follow a convoy. Per-group keys ride the
 * reserved auth trailer later; see docs/MESSAGING.md.
 */
#pragma once

#include <stdbool.h>
#include <stdint.h>

#include "vmesh_msg.h"

#define CONVOY_MAX        8
#define CONVOY_MEMBER_TTL 300u        /* drop a silent member (5 min)  */
#define CONVOY_IDLE_TTL   (12u * 3600u) /* dissolve a dead convoy      */
#define CONVOY_BEACON_S   30u         /* own position cadence          */

/* convoy subtypes (carried in hazard_type) */
enum {
    CONVOY_SUB_POSITION = 0,
    CONVOY_SUB_MESSAGE  = 1,
};

typedef struct {
    uint32_t origin;
    double   lat, lon;
    uint32_t heard_s;
} convoy_member_t;

/* code word -> group id (case/space insensitive, so "Coast Run" and
 * "coastrun" are the same convoy) */
uint32_t convoy_code_id(const char *phrase);

void convoy_join(const char *phrase, uint32_t now_s);
void convoy_leave(void);
bool convoy_active(void);
const char *convoy_phrase(void);

/* a heard convoy frame: position beacons update the member table,
 * messages are left for the caller to show. Returns true if the frame
 * belonged to our convoy (and was consumed as a position). */
bool convoy_note(const vmesh_msg_t *m, uint32_t own_origin, uint32_t now_s);

/* build own frames; false when not in a convoy */
bool convoy_make_beacon(vmesh_msg_t *out);
bool convoy_make_message(vmesh_msg_t *out, const char *text);

/* true every CONVOY_BEACON_S while in a convoy — drives the tick */
bool convoy_beacon_due(uint32_t now_s);

void convoy_prune(uint32_t now_s);      /* ages members + the convoy   */
int  convoy_count(void);                /* live members (not counting us) */
convoy_member_t *convoy_slot(int i);    /* NULL-safe, may be inactive  */

/* canned group phrases (driver-safe: taps, never typing) */
#define CONVOY_PHRASES 4
extern const char *const convoy_phrases[CONVOY_PHRASES];

/* is this frame addressed to the convoy we're in? */
bool convoy_is_ours(const vmesh_msg_t *m);
