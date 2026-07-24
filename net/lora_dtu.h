/**
 * lora_dtu.h — transport adapter for the Waveshare USB-TO-LoRa-xF
 * dongle (SX1262 DTU firmware, AT-configurable, byte-pipe payload).
 *
 * The dongle moves opaque bytes; framing is OURS: since a serial pipe
 * has no packet boundaries, vmesh wire frames travel as
 *
 *   [0x56 0x4D][len u8][payload len bytes][crc16-ccitt over len+payload]
 *
 * so the deframer can resync mid-stream after garbage/partial reads.
 *
 * VERIFY-ON-HARDWARE (flagged, not risky-guessed): the exact AT
 * response strings and whether the DTU injects RSSI trailer bytes
 * (AT+RSSI=1 — we keep it OFF until verified). Pure C; unit-tested.
 */
#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#define LORA_DTU_SYNC0 0x56 /* 'V' */
#define LORA_DTU_SYNC1 0x4D /* 'M' */
#define LORA_DTU_MAX_PAYLOAD 96
#define LORA_DTU_OVERHEAD 5 /* sync2 + len1 + crc2 */

/* Build the AT init sequence for our profile into buf (NUL-terminated,
 * one command per line). Returns length, or -1 if cap too small. */
int lora_dtu_init_cmds(char *buf, size_t cap,
                       int sf, int bw_idx, int channel, int power_dbm);

/* Wrap payload for the pipe. Returns frame length or -1 (cap/size). */
int lora_dtu_frame(const uint8_t *payload, size_t n,
                   uint8_t *out, size_t cap);

/* streaming deframer */
typedef struct {
    uint8_t buf[LORA_DTU_MAX_PAYLOAD + LORA_DTU_OVERHEAD];
    int     have;
} lora_dtu_rx_t;

/* Feed one byte; returns payload length (>0) when a checksummed frame
 * completed (payload copied to *payload, cap >= LORA_DTU_MAX_PAYLOAD),
 * else 0. Garbage between frames is skipped automatically. */
int lora_dtu_rx_feed(lora_dtu_rx_t *rx, uint8_t c,
                     uint8_t *payload, size_t cap);
