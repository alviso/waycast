/**
 * vmesh_wire.h — the over-the-air encoding of vmesh_msg_t (spec §7).
 *
 * v0 layout: little-endian, byte-aligned, fixed 29-byte base + a
 * length-prefixed note (0..40 bytes):
 *
 *   [0]     version            [1]     msg_type
 *   [2]     channel            [3]     hazard_type / LOCAL category
 *   [4]     severity           [5]     hops
 *   [6-9]   origin_id  u32     [10-11] seq        u16
 *   [12-15] lat_e7     i32     [16-19] lon_e7     i32
 *   [20-23] created_s  u32     [24-25] ttl_s      u16
 *   [26-27] radius_m_x10 u16   [28]    note_len
 *   [29..]  note (note_len bytes, no terminator)
 *
 * ATTEST frames (msg_type 4) have no position of their own, so the
 * position slots are reused: [12-15] = ref_origin, [24-25] = ref_seq;
 * hazard_type carries the verdict. Same 29-byte base, no note.
 *
 * Sizing vs. FCC 400 ms dwell (125 kHz): a note-less hazard (29 B)
 * fits up to ~SF9; chatter with a full note (69 B) needs SF7/SF8.
 * Future versions append an auth trailer — the version byte governs,
 * and decode rejects unknown versions rather than guessing.
 */
#pragma once

#include "vmesh_msg.h"
#include <stddef.h>

#define VMESH_WIRE_BASE 29
#define VMESH_WIRE_NOTE_MAX 40
#define VMESH_WIRE_MAX (VMESH_WIRE_BASE + VMESH_WIRE_NOTE_MAX)

/* Returns encoded length (>= VMESH_WIRE_BASE), or -1 if cap is too
 * small. The note is truncated to VMESH_WIRE_NOTE_MAX. */
int vmesh_wire_encode(const vmesh_msg_t *m, uint8_t *buf, size_t cap);

/* Returns 0 on success, -1 on malformed input (short buffer,
 * inconsistent note length), -2 on unsupported protocol version. */
int vmesh_wire_decode(const uint8_t *buf, size_t len, vmesh_msg_t *out);
