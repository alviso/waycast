#include "lora_dtu.h"

#include <stdio.h>
#include <string.h>

static uint16_t crc16_ccitt(const uint8_t *d, size_t n)
{
    uint16_t crc = 0xFFFF;
    for (size_t i = 0; i < n; i++) {
        crc ^= (uint16_t)d[i] << 8;
        for (int b = 0; b < 8; b++)
            crc = (crc & 0x8000) ? (uint16_t)((crc << 1) ^ 0x1021)
                                 : (uint16_t)(crc << 1);
    }
    return crc;
}

int lora_dtu_init_cmds(char *buf, size_t cap,
                       int sf, int bw_idx, int channel, int power_dbm)
{
    /* per the USB-TO-LoRa-xF AT set: SF 7..12, BW 0..2, CH 0..80,
     * PWR 10..22; TX and RX channel kept equal; RSSI off (see .h) */
    int n = snprintf(buf, cap,
                     "AT+SF=%d\r\n"
                     "AT+BW=%d\r\n"
                     "AT+TXCH=%d\r\n"
                     "AT+RXCH=%d\r\n"
                     "AT+PWR=%d\r\n"
                     "AT+RSSI=0\r\n",
                     sf, bw_idx, channel, channel, power_dbm);
    return (n > 0 && (size_t)n < cap) ? n : -1;
}

int lora_dtu_frame(const uint8_t *payload, size_t n,
                   uint8_t *out, size_t cap)
{
    if (n == 0 || n > LORA_DTU_MAX_PAYLOAD) return -1;
    size_t total = n + LORA_DTU_OVERHEAD;
    if (cap < total) return -1;

    out[0] = LORA_DTU_SYNC0;
    out[1] = LORA_DTU_SYNC1;
    out[2] = (uint8_t)n;
    memcpy(out + 3, payload, n);
    uint16_t crc = crc16_ccitt(out + 2, n + 1); /* len + payload */
    out[3 + n] = (uint8_t)(crc >> 8);
    out[4 + n] = (uint8_t)crc;
    return (int)total;
}

int lora_dtu_rx_feed(lora_dtu_rx_t *rx, uint8_t c,
                     uint8_t *payload, size_t cap)
{
    /* sync hunt */
    if (rx->have == 0) {
        if (c == LORA_DTU_SYNC0) rx->buf[rx->have++] = c;
        return 0;
    }
    if (rx->have == 1) {
        if (c == LORA_DTU_SYNC1) {
            rx->buf[rx->have++] = c;
        } else {
            rx->have = (c == LORA_DTU_SYNC0) ? 1 : 0; /* resync */
        }
        return 0;
    }

    rx->buf[rx->have++] = c;

    int len = rx->buf[2];
    if (rx->have == 3 &&
        (len == 0 || len > LORA_DTU_MAX_PAYLOAD)) {
        rx->have = 0; /* nonsense length: back to hunting */
        return 0;
    }
    if (rx->have < 3 + len + 2) return 0;

    /* complete frame: verify */
    uint16_t want = (uint16_t)((rx->buf[3 + len] << 8) | rx->buf[4 + len]);
    uint16_t got = crc16_ccitt(rx->buf + 2, (size_t)len + 1);
    rx->have = 0;
    if (want != got || cap < (size_t)len) return 0;

    memcpy(payload, rx->buf + 3, (size_t)len);
    return len;
}
