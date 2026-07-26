/* vmesh_peer — Waycast mesh peer for Raspberry Pi + SX1302 concentrator
 * (RAK2287 SPI / similar), via Semtech's sx1302_hal (libloragw).
 *
 * Runs the SAME geo-ephemeral flooding core (mesh/vmesh_mesh.c) and
 * wire format (msg/vmesh_wire.c) as the device firmware and the
 * meshsim harness. A concentrator hears every SF on 8 channels, so a
 * peer is also the ultimate development sniffer.
 *
 *   ./vmesh_peer --lat 37.33 --lon -122.03 [--sniff] [--report N]
 *
 *   --sniff       log frames only; never relay or transmit
 *   --report N    emit a test debris hazard every N seconds
 *
 * PHY profile matches the node side (bonnet_pins.h / USB dongle):
 * 915.0 MHz, SF7 tx / multi-SF rx, BW125 (DTU stream framing on-air), CR4/5, sync PRIVATE (lorawan_public=false ->
 * 0x12), CRC on, IQ normal both directions (invert_pol=false).
 *
 * VERIFY-ON-PI: TX gain table below is a single conservative entry;
 * if TX power is off, copy the txlut values for your RAK2287 from
 * RAK's global_conf.json. Reset the card first with sx1302_hal's
 * tools/reset_lgw.sh (RAK Pi HAT: reset on GPIO17).
 */

#include "vmesh_mesh.h"
#include "vmesh_msg.h"
#include "vmesh_wire.h"
#include "lora_dtu.h"

#include <loragw_hal.h>

#include <fcntl.h>
#include <netinet/in.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <time.h>
#include <unistd.h>

#define FREQ_HZ 915000000u
#define SPI_PATH "/dev/spidev0.0"

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
    tx.rf_power = 17;
    tx.modulation = MOD_LORA;
    tx.bandwidth = BW_125KHZ;
    tx.datarate = DR_LORA_SF7; /* fleet dongle profile — CONFIRMED off the air July 23: [air] DR7 BW125 */
    tx.coderate = CR_LORA_4_5;
    tx.invert_pol = false; /* node-style TX, not LoRaWAN downlink */
    tx.preamble = 8;
    tx.no_crc = false;
    tx.no_header = false;
    /* wrap in the DTU stream framing [56 4D len payload crc16] so the
     * USB dongles (transparent stream mode) and the HAT interop */
    /* DTU air header (sniffed off a real dongle TX, July 25): the
     * dongle firmware prepends [addr_hi addr_lo netid 0x11] and its
     * RECEIVER silently drops frames without it — why every raw
     * SX1302 downlink was invisible to the fleet while dongle-to-
     * dongle worked. addr 0 = fleet default, netid 0, 0x11 static. */
    static const uint8_t dtu_hdr[4] = { 0x00, 0x00, 0x00, 0x11 };
    int wn = lora_dtu_frame(buf, len, tx.payload + sizeof(dtu_hdr),
                            sizeof(tx.payload) - sizeof(dtu_hdr));
    if (wn <= 0) return -1;
    memcpy(tx.payload, dtu_hdr, sizeof(dtu_hdr));
    tx.size = (uint16_t)(wn + sizeof(dtu_hdr));
    int rc = lgw_send(&tx);
    if (rc != 0) fprintf(stderr, "lgw_send FAILED rc=%d\n", rc);
    return rc;
}

static bool g_sniff = false;
static double g_lat = 37.33, g_lon = -122.03;
static char g_name[24] = "anchor"; /* town name, beacon payload */
static vmesh_mesh_t g_mesh;

static void relay_tx(const vmesh_msg_t *m, void *user)
{
    (void)user;
    uint8_t buf[VMESH_WIRE_MAX];
    int n = vmesh_wire_encode(m, buf, sizeof(buf));
    if (n > 0) {
        radio_send(buf, (uint8_t)n);
        printf("[relay] %08X/%u hop %u\n", m->origin_id, m->seq, m->hops);
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
        : m->msg_type == VMESH_MT_HELLO ? "Hello"
        : m->msg_type == VMESH_MT_CONVOY
            ? (m->hazard_type ? "ConvoyMsg" : "ConvoyPos")
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
        else if (!strcmp(argv[i], "--name") && i + 1 < argc)
            snprintf(g_name, sizeof(g_name), "%s", argv[++i]);
    }

    signal(SIGINT, on_sig);
    signal(SIGTERM, on_sig);
    setvbuf(stdout, NULL, _IOLBF, 0); /* live lines under systemd/journal */

    /* ---- concentrator bring-up (sx1302_hal core API) ---- */
    /* RAK2287/SX1302: fresh HW reset immediately before start (GPIO17) */
    if (system("/home/waycast/reset_lgw_rak2287.sh") != 0)
        fprintf(stderr, "warning: reset script returned nonzero\n");
    usleep(100000);

    struct lgw_conf_board_s board;
    memset(&board, 0, sizeof(board));
    board.lorawan_public = false; /* sync word 0x12 = our private net */
    board.clksrc = getenv("CLKSRC") ? atoi(getenv("CLKSRC")) : 0; /* RAK2287 often 1 */
    board.full_duplex = false;
    board.com_type = LGW_COM_SPI;
    const char *spidev = getenv("SPIDEV") ? getenv("SPIDEV") : SPI_PATH;
    strncpy(board.com_path, spidev, sizeof(board.com_path) - 1);
    if (lgw_board_setconf(&board) != LGW_HAL_SUCCESS) {
        fprintf(stderr, "board conf failed\n");
        return 1;
    }

    struct lgw_conf_rxrf_s rf;
    memset(&rf, 0, sizeof(rf));
    rf.enable = true;
    rf.freq_hz = getenv("RF0") ? (uint32_t)strtoul(getenv("RF0"),0,10) : FREQ_HZ;
    rf.rssi_offset = -215.4f; /* RAK2287 typical */
    rf.type = LGW_RADIO_TYPE_SX1250;
    rf.tx_enable = !g_sniff; /* sniffer never transmits */
    if (lgw_rxrf_setconf(0, &rf) != LGW_HAL_SUCCESS) {
        fprintf(stderr, "rf0 conf failed\n");
        return 1;
    }
    rf.tx_enable = false;
    rf.freq_hz = FREQ_HZ + 800000;
    rf.enable = (board.clksrc == 1); /* radio 1 only when it is the clock source */
    lgw_rxrf_setconf(1, &rf);

    /* one multi-SF IF channel centered on our frequency */
    struct lgw_conf_rxif_s ifc;
    memset(&ifc, 0, sizeof(ifc));
    ifc.enable = true;
    ifc.rf_chain = 0;
    ifc.freq_hz = 0; /* offset from rf0 center */
    ifc.bandwidth = BW_125KHZ;
    ifc.datarate = 0; /* 0 = default = multi-SF demod (SF5-12) */
    if (getenv("SKIPIF") == NULL && lgw_rxif_setconf(0, &ifc) != LGW_HAL_SUCCESS) {
        fprintf(stderr, "if0 conf failed\n");
        return 1;
    }

    /* SX1250 TX gain LUT — RAK2287 official US915 table
     * (rak_common_for_gateway lora/rak2287/global_conf_i2c/
     *  global_conf.us_902_928.json, radio_0.tx_gain_lut) */
    static const struct { int8_t dbm; uint8_t idx; } rak_lut[] = {
        {12, 6},  {13, 7},  {14, 8},  {15, 9},  {16, 10}, {17, 11},
        {18, 12}, {19, 13}, {20, 14}, {21, 15}, {22, 16}, {23, 17},
        {24, 18}, {25, 19}, {26, 21}, {27, 22},
    };
    struct lgw_tx_gain_lut_s lut;
    memset(&lut, 0, sizeof(lut));
    lut.size = sizeof(rak_lut) / sizeof(rak_lut[0]);
    for (unsigned li = 0; li < lut.size; li++) {
        lut.lut[li].rf_power = rak_lut[li].dbm;
        lut.lut[li].pa_gain = 1;
        lut.lut[li].pwr_idx = rak_lut[li].idx;
    }
    if (!g_sniff) lgw_txgain_setconf(0, &lut);

    if (lgw_start() != LGW_HAL_SUCCESS) {
        fprintf(stderr, "lgw_start failed (reset_lgw.sh run? spidev "
                        "enabled? card seated?)\n");
        return 1;
    }
    printf("vmesh_peer up: %.1f MHz SF7/125k sync=private DTU-framed %s @%.5f,%.5f\n",
           FREQ_HZ / 1e6, g_sniff ? "[sniffer]" : "[relay peer]", g_lat,
           g_lon);

    mesh_config_t mcfg = { .jitter_min_ms = 100,
                           .jitter_max_ms = 800,
                           .suppress_threshold = 2 };
    mesh_init(&g_mesh, &mcfg, (uint32_t)time(NULL));

    uint32_t own_origin = 0x50490000u | (uint32_t)(getpid() & 0xFFFF);
    uint16_t own_seq = 0;
    time_t last_report = time(NULL);

    /* ---- UDP inject listener (town-node injector service) ----
     * line proto: HAZ <hz 0-6> <sev 1-3> <lat> <lon> <radius_m> <ttl_s> [note]
     * loopback-only; the injector daemon (NWS/ODOT poller) feeds it. */
    int inj_fd = -1;
    if (!g_sniff) {
        inj_fd = socket(AF_INET, SOCK_DGRAM, 0);
        if (inj_fd >= 0) {
            struct sockaddr_in ia;
            memset(&ia, 0, sizeof(ia));
            ia.sin_family = AF_INET;
            ia.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
            ia.sin_port = htons(getenv("INJECT_PORT")
                                    ? atoi(getenv("INJECT_PORT")) : 7788);
            if (bind(inj_fd, (struct sockaddr *)&ia, sizeof(ia)) != 0 ||
                fcntl(inj_fd, F_SETFL, O_NONBLOCK) != 0) {
                close(inj_fd);
                inj_fd = -1;
                fprintf(stderr, "warning: inject listener disabled\n");
            } else {
                printf("[inject] listening udp 127.0.0.1:%d\n",
                       ntohs(ia.sin_port));
            }
        }
    }

    while (running) {
        struct lgw_pkt_rx_s rxpkt[8];
        int n = lgw_receive(8, rxpkt);
        for (int i = 0; i < n; i++) {
            struct lgw_pkt_rx_s *p = &rxpkt[i];
            if (p->status != STAT_CRC_OK) continue;
            /* dongles are byte-pipes: their LoRa payload carries our
             * DTU framing. Unwrap when present, else try raw. */
            uint8_t inner[VMESH_WIRE_MAX];
            const uint8_t *fr = p->payload; size_t fn = p->size;
            {   /* RAW payload dump: what the DTU actually puts on air
                 * (hunting its address/netid header — July 25) */
                char hx[3 * 64 + 8]; int hn = 0;
                for (uint16_t bi = 0; bi < p->size && bi < 64; bi++)
                    hn += snprintf(hx + hn, sizeof(hx) - hn, "%02X ",
                                   p->payload[bi]);
                printf("[raw %uB] %s\n", p->size, hx);
            }
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
                           p->rssis, p->snr);
                    for (uint16_t bi = 0; bi < p->size && bi < 40; bi++) {
                        uint8_t c = p->payload[bi];
                        putchar((c >= 32 && c < 127) ? c : '.');
                    }
                    printf("\n"); fflush(stdout);
                }
                continue;
            }
            vmesh_clock_normalize(&m, (uint32_t)time(NULL));
            if (m.msg_type == VMESH_MT_BEACON) {
                /* presence beacons are point-in-time, never relayed —
                 * also guards against the concentrator hearing its
                 * own TX and echoing it forever */
                continue;
            }
            float prox = (p->rssis + 130.0f) / 90.0f;
            bool deliver =
                g_sniff ? true
                        : mesh_rx(&g_mesh, &m, prox, g_lat, g_lon,
                                  now_ms(), (uint32_t)time(NULL));
            printf("[air] %.3fMHz DR%u BW%u ", p->freq_hz / 1e6, p->datarate, p->bandwidth);
            print_msg(&m, p->rssis, p->snr, deliver);
        }

        /* drain injector lines -> own hazards on the mesh */
        if (inj_fd >= 0) {
            char line[200];
            ssize_t ln;
            while ((ln = recv(inj_fd, line, sizeof(line) - 1, 0)) > 0) {
                line[ln] = 0;
                unsigned hz, sev, rm, ttl;
                double la, lo;
                char note[64] = "";
                int k = sscanf(line, "HAZ %u %u %lf %lf %u %u %63[^\n]",
                               &hz, &sev, &la, &lo, &rm, &ttl, note);
                if (k < 6 || hz >= VMESH_HZ_COUNT) {
                    printf("[inject] bad line: %s\n", line);
                    continue;
                }
                vmesh_msg_t m;
                memset(&m, 0, sizeof(m));
                m.version = VMESH_PROTO_VERSION;
                m.msg_type = VMESH_MT_HAZARD;
                m.channel = VMESH_CH_SAFETY;
                m.hazard_type = (uint8_t)hz;
                m.severity = (uint8_t)(sev < 1 ? 1 : sev > 3 ? 3 : sev);
                m.origin_id = own_origin;
                m.seq = ++own_seq;
                m.lat_e7 = (int32_t)(la * 1e7);
                m.lon_e7 = (int32_t)(lo * 1e7);
                m.created_s = (uint32_t)time(NULL);
                m.ttl_s = (uint16_t)(ttl > 65535 ? 65535 : ttl);
                m.radius_m_x10 =
                    (uint16_t)(rm / 10 > 65535 ? 65535 : rm / 10);
                strncpy(m.note, note, sizeof(m.note) - 1);
                mesh_note_own(&g_mesh, &m);
                uint8_t ib[VMESH_WIRE_MAX];
                int wn = vmesh_wire_encode(&m, ib, sizeof(ib));
                if (wn > 0) {
                    radio_send(ib, (uint8_t)wn);
                    printf("[inject] tx %s sev%u @%.5f,%.5f r=%.1fkm "
                           "ttl=%us \"%s\"\n",
                           vmesh_hz_name((uint8_t)hz), m.severity, la, lo,
                           rm / 1000.0, m.ttl_s, m.note);
                }
            }
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
            }
        }

        /* ANCHOR PRESENCE BEACON (July 25): once a minute, tell the
         * town "your node is here". Devices show a 'town near'
         * indicator that ages out — the answer to talking into the
         * ether. MT_BEACON (reserved since phase 0) finally earns its
         * keep. Not relayed (see beacon gate in the rx path). */
        {
            static time_t last_beacon;
            if (!g_sniff && time(NULL) - last_beacon >= 60) {
                last_beacon = time(NULL);
                vmesh_msg_t b;
                memset(&b, 0, sizeof(b));
                b.version = VMESH_PROTO_VERSION;
                b.msg_type = VMESH_MT_BEACON;
                b.channel = VMESH_CH_SAFETY;
                b.origin_id = own_origin;
                b.seq = ++own_seq;
                b.lat_e7 = (int32_t)(g_lat * 1e7);
                b.lon_e7 = (int32_t)(g_lon * 1e7);
                b.created_s = (uint32_t)time(NULL);
                b.ttl_s = 150;          /* ~2 missed beacons then stale */
                b.radius_m_x10 = 500;   /* the town, 5 km */
                snprintf(b.note, sizeof(b.note), "%s", g_name);
                uint8_t bb[VMESH_WIRE_MAX];
                int bn = vmesh_wire_encode(&b, bb, sizeof(bb));
                if (bn > 0) radio_send(bb, (uint8_t)bn);
            }
        }

        usleep(20000); /* 20 ms poll — matches device tick granularity */
    }

    lgw_stop();
    printf("stats: rx=%u delivered=%u dup=%u relayed=%u suppressed=%u\n",
           g_mesh.stats.rx_total, g_mesh.stats.delivered,
           g_mesh.stats.dup_dropped, g_mesh.stats.relayed,
           g_mesh.stats.suppressed);
    return 0;
}
