/* vmesh_peer_sx1301 — Waycast mesh peer for Raspberry Pi + SX1301
 * concentrator (RAK2245 / RAK831 / iC880A-class), via Semtech's legacy
 * lora_gateway HAL (libloragw). SX1301 boards use SX1257 radios and a
 * different HAL from the SX1302 (RAK2287) variant in vmesh_peer.c.
 *
 * Confirmed on Peter's Crankk gateway: the concentrator reads VERSION
 * register = 103 (0x67) = SX1301 (NOT SX1302 — that's why sx1302_hal
 * saw only 0x00). Same mesh core + DTU-framed 915/SF7 fleet profile.
 *
 *   sudo ./vmesh_peer_sx1301 --lat .. --lon .. [--sniff] [--report N]
 *
 * Reset the SX1301 first (RAK Pi HAT reset = GPIO17, held low):
 *   sudo pinctrl set 17 op dh; sleep .1; sudo pinctrl set 17 op dl
 *
 * PHY: 915.0 MHz, SF7, BW125, CR4/5, sync PRIVATE (lorawan_public=false
 * -> 0x12), CRC on, IQ normal. RF0 center 914.7 + IF0 offset +300k =
 * 915.0 (kept off the SX1301 DC spur). clksrc=1: radio 1 clocks the
 * concentrator, so RF1 is enabled too.
 *
 * VERIFY-ON-PI: TX gain LUT is a conservative single entry for SX1257;
 * copy RAK2245 values from its global_conf.json if TX power is off.
 */

#include "vmesh_mesh.h"
#include "vmesh_msg.h"
#include "vmesh_wire.h"
#include "lora_dtu.h"

#include <loragw_hal.h>

#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>

#define FREQ_HZ    915000000u          /* target on-air frequency */
#define RF0_CENTER 914700000u          /* radio 0 center */
#define IF0_OFFSET (int32_t)(FREQ_HZ - RF0_CENTER) /* +300 kHz -> 915.0 */
#define RF1_CENTER 915600000u          /* radio 1 (clock source) */

static volatile int running = 1;
static void on_sig(int s) { (void)s; running = 0; }

static uint32_t now_ms(void)
{
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (uint32_t)(ts.tv_sec * 1000 + ts.tv_nsec / 1000000);
}

static int radio_send(const uint8_t *buf, uint8_t len)
{
    struct lgw_pkt_tx_s tx;
    memset(&tx, 0, sizeof(tx));
    tx.freq_hz = FREQ_HZ;
    tx.tx_mode = IMMEDIATE;
    tx.rf_chain = 0;
    tx.rf_power = 14;              /* matches the LUT entry below */
    tx.modulation = MOD_LORA;
    tx.bandwidth = BW_125KHZ;
    tx.datarate = DR_LORA_SF7;
    tx.coderate = CR_LORA_4_5;
    tx.invert_pol = false;        /* node-style TX, not LoRaWAN downlink */
    tx.preamble = 8;
    tx.no_crc = false;
    tx.no_header = false;
    /* DTU stream framing so the USB dongles / HAT interop */
    int wn = lora_dtu_frame(buf, len, tx.payload, sizeof(tx.payload));
    if (wn <= 0) return -1;
    tx.size = (uint16_t)wn;
    return lgw_send(tx);          /* SX1301 HAL: by value */
}

static bool g_sniff = false;
static double g_lat = 37.33, g_lon = -122.03;
static vmesh_mesh_t g_mesh;

static void relay_tx(const vmesh_msg_t *m, void *user)
{
    (void)user;
    uint8_t buf[VMESH_WIRE_MAX];
    int n = vmesh_wire_encode(m, buf, sizeof(buf));
    if (n > 0) {
        radio_send(buf, (uint8_t)n);
        printf("[relay] %08X/%u hop %u\n", m->origin_id, m->seq, m->hops);
        fflush(stdout);
    }
}

static void print_msg(const vmesh_msg_t *m, float rssi, float snr,
                      bool delivered)
{
    const char *kind =
        m->msg_type == VMESH_MT_HAZARD ? vmesh_hz_name(m->hazard_type)
        : m->msg_type == VMESH_MT_ATTEST
            ? (m->hazard_type == VMESH_VERDICT_CONFIRM ? "ATTEST+"
                                                       : "ATTEST-")
        : m->channel == VMESH_CH_LOCAL ? vmesh_local_name(m->hazard_type)
                                       : "msg";
    printf("[rx %s] %-9s %08X/%-5u hops=%u rssi=%.0f snr=%.1f",
           delivered ? "OK " : "drop", kind, m->origin_id, m->seq,
           m->hops, rssi, snr);
    if (m->msg_type != VMESH_MT_ATTEST)
        printf(" @%.5f,%.5f r=%.1fkm ttl=%us", m->lat_e7 / 1e7,
               m->lon_e7 / 1e7, m->radius_m_x10 / 100.0, m->ttl_s);
    if (m->note[0]) printf(" \"%s\"", m->note);
    printf("\n");
    fflush(stdout);
}

int main(int argc, char **argv)
{
    int report_every = 0;
    for (int i = 1; i < argc; i++) {
        if (!strcmp(argv[i], "--sniff")) g_sniff = true;
        else if (!strcmp(argv[i], "--lat") && i + 1 < argc)
            g_lat = atof(argv[++i]);
        else if (!strcmp(argv[i], "--lon") && i + 1 < argc)
            g_lon = atof(argv[++i]);
        else if (!strcmp(argv[i], "--report") && i + 1 < argc)
            report_every = atoi(argv[++i]);
    }

    signal(SIGINT, on_sig);
    signal(SIGTERM, on_sig);

    /* ---- concentrator bring-up (SX1301 lora_gateway API) ---- */
    struct lgw_conf_board_s board;
    memset(&board, 0, sizeof(board));
    board.lorawan_public = false;  /* private sync word 0x12 */
    board.clksrc = 1;              /* radio 1 clocks the SX1301 (RAK2245) */
    if (lgw_board_setconf(board) != LGW_HAL_SUCCESS) {
        fprintf(stderr, "board conf failed\n");
        return 1;
    }

    struct lgw_conf_rxrf_s rf;
    /* RF chain 0: our RX/TX radio */
    memset(&rf, 0, sizeof(rf));
    rf.enable = true;
    rf.freq_hz = RF0_CENTER;
    rf.rssi_offset = -166.0f;      /* SX1257 typical (RAK2245 global_conf) */
    rf.type = LGW_RADIO_TYPE_SX1257;
    rf.tx_enable = true;
    if (lgw_rxrf_setconf(0, rf) != LGW_HAL_SUCCESS) {
        fprintf(stderr, "rf0 conf failed\n");
        return 1;
    }
    /* RF chain 1: clock source (clksrc=1) — must be enabled */
    memset(&rf, 0, sizeof(rf));
    rf.enable = true;
    rf.freq_hz = RF1_CENTER;
    rf.rssi_offset = -166.0f;
    rf.type = LGW_RADIO_TYPE_SX1257;
    rf.tx_enable = false;
    if (lgw_rxrf_setconf(1, rf) != LGW_HAL_SUCCESS) {
        fprintf(stderr, "rf1 conf failed\n");
        return 1;
    }

    /* one multi-SF LoRa IF channel at 915.0 (offset from RF0 center) */
    struct lgw_conf_rxif_s ifc;
    memset(&ifc, 0, sizeof(ifc));
    ifc.enable = true;
    ifc.rf_chain = 0;
    ifc.freq_hz = IF0_OFFSET;      /* +300 kHz -> 915.0 MHz */
    ifc.bandwidth = BW_125KHZ;
    ifc.datarate = DR_LORA_MULTI;  /* SF7..SF12 multi-SF demod */
    if (lgw_rxif_setconf(0, ifc) != LGW_HAL_SUCCESS) {
        fprintf(stderr, "if0 conf failed\n");
        return 1;
    }

    /* minimal SX1257 TX gain LUT — VERIFY against RAK2245 global_conf */
    struct lgw_tx_gain_lut_s lut;
    memset(&lut, 0, sizeof(lut));
    lut.size = 1;
    lut.lut[0].rf_power = 14;
    lut.lut[0].dig_gain = 0;
    lut.lut[0].pa_gain = 2;
    lut.lut[0].dac_gain = 3;
    lut.lut[0].mix_gain = 12;
    lgw_txgain_setconf(&lut);

    if (lgw_start() != LGW_HAL_SUCCESS) {
        fprintf(stderr, "lgw_start failed (SX1301 reset done? spidev? "
                        "card seated?)\n");
        return 1;
    }
    printf("vmesh_peer_sx1301 up: %.1f MHz SF7/125k sync=private "
           "DTU-framed %s @%.5f,%.5f\n", FREQ_HZ / 1e6,
           g_sniff ? "[sniffer]" : "[relay peer]", g_lat, g_lon);
    fflush(stdout);

    mesh_config_t mcfg = { .jitter_min_ms = 100,
                           .jitter_max_ms = 800,
                           .suppress_threshold = 2 };
    mesh_init(&g_mesh, &mcfg, (uint32_t)time(NULL));

    uint32_t own_origin = 0x50490000u | (uint32_t)(getpid() & 0xFFFF);
    uint16_t own_seq = 0;
    time_t last_report = time(NULL);

    while (running) {
        struct lgw_pkt_rx_s rxpkt[8];
        int n = lgw_receive(8, rxpkt);
        for (int i = 0; i < n; i++) {
            struct lgw_pkt_rx_s *p = &rxpkt[i];
            if (p->status != STAT_CRC_OK) continue;
            /* dongle byte-pipe: LoRa payload carries our DTU framing */
            uint8_t inner[VMESH_WIRE_MAX];
            const uint8_t *fr = p->payload; size_t fn = p->size;
            lora_dtu_rx_t d; memset(&d, 0, sizeof(d));
            for (uint16_t bi = 0; bi < p->size; bi++) {
                int r = lora_dtu_rx_feed(&d, p->payload[bi], inner,
                                         sizeof(inner));
                if (r > 0) { fr = inner; fn = (size_t)r; break; }
            }
            vmesh_msg_t m;
            if (vmesh_wire_decode(fr, fn, &m) != 0) {
                if (g_sniff) {
                    printf("[raw %3uB rssi %.0f snr %.1f] ", p->size,
                           p->rssi, p->snr);
                    for (uint16_t bi = 0; bi < p->size && bi < 40; bi++) {
                        uint8_t c = p->payload[bi];
                        putchar((c >= 32 && c < 127) ? c : '.');
                    }
                    printf("\n"); fflush(stdout);
                }
                continue;
            }
            float prox = (p->rssi + 130.0f) / 90.0f;
            bool deliver =
                g_sniff ? true
                        : mesh_rx(&g_mesh, &m, prox, g_lat, g_lon,
                                  now_ms(), (uint32_t)time(NULL));
            print_msg(&m, p->rssi, p->snr, deliver);
        }

        if (!g_sniff)
            mesh_tick(&g_mesh, now_ms(), relay_tx, NULL);

        if (report_every && !g_sniff &&
            time(NULL) - last_report >= report_every) {
            last_report = time(NULL);
            vmesh_msg_t m;
            memset(&m, 0, sizeof(m));
            m.version = VMESH_PROTO_VERSION;
            m.msg_type = VMESH_MT_HAZARD;
            m.channel = VMESH_CH_SAFETY;
            m.hazard_type = VMESH_HZ_DEBRIS;
            m.severity = 2;
            m.origin_id = own_origin;
            m.seq = ++own_seq;
            m.lat_e7 = (int32_t)(g_lat * 1e7);
            m.lon_e7 = (int32_t)(g_lon * 1e7);
            m.created_s = (uint32_t)time(NULL);
            m.ttl_s = 600;
            m.radius_m_x10 = 300;
            snprintf(m.note, sizeof(m.note), "pi peer test");
            mesh_note_own(&g_mesh, &m);
            uint8_t buf[VMESH_WIRE_MAX];
            int wn = vmesh_wire_encode(&m, buf, sizeof(buf));
            if (wn > 0) {
                radio_send(buf, (uint8_t)wn);
                printf("[tx] test hazard %08X/%u\n", m.origin_id, m.seq);
                fflush(stdout);
            }
        }

        usleep(20000); /* 20 ms poll */
    }

    lgw_stop();
    printf("stats: rx=%u delivered=%u dup=%u relayed=%u suppressed=%u\n",
           g_mesh.stats.rx_total, g_mesh.stats.delivered,
           g_mesh.stats.dup_dropped, g_mesh.stats.relayed,
           g_mesh.stats.suppressed);
    return 0;
}
