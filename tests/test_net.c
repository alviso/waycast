/* unit tests for the USB-transport building blocks — `make test` */
#include "lora_dtu.h"
#include "nmea.h"
#include "vmesh_wire.h"

#include <assert.h>
#include <math.h>
#include <stdio.h>
#include <string.h>

static void test_nmea(void)
{
    nmea_parser_t p = {0};
    nmea_fix_t fix;

    /* real-world RMC (checksum-correct), fed with leading garbage */
    const char *s =
        "xx$$GPRMC,123519,A,4807.038,N,01131.000,E,022.4,084.4,230394,"
        "003.1,W*6A\r\n";
    bool got = false;
    for (const char *c = s; *c; c++)
        if (nmea_feed_char(&p, *c, &fix)) got = true;
    assert(got);
    assert(fix.valid);
    assert(fabs(fix.lat - 48.1173) < 1e-3);
    assert(fabs(fix.lon - 11.5167) < 1e-3);
    assert(fabs(fix.speed_mps - 22.4 * 0.514444) < 1e-2);
    assert(fabs(fix.course_deg - 84.4) < 1e-2);

    /* corrupted checksum is rejected */
    const char *bad =
        "$GPRMC,123519,A,4807.038,N,01131.000,E,022.4,084.4,230394,"
        "003.1,W*6B\r\n";
    got = false;
    for (const char *c = bad; *c; c++)
        if (nmea_feed_char(&p, *c, &fix)) got = true;
    assert(!got);

    /* southern/western hemisphere signs */
    const char *sw =
        "$GNRMC,000001,A,3352.000,S,15112.000,W,000.0,000.0,010100,,*29";
    /* compute checksum ourselves to keep the fixture honest */
    unsigned cs = 0;
    for (const char *c = sw + 1; *c != '*'; c++) cs ^= (unsigned char)*c;
    char sw2[128];
    snprintf(sw2, sizeof(sw2), "%.*s%02X\r\n", (int)(strchr(sw, '*') - sw + 1),
             sw, cs);
    got = false;
    for (const char *c = sw2; *c; c++)
        if (nmea_feed_char(&p, *c, &fix)) got = true;
    assert(got);
    assert(fix.lat < 0 && fix.lon < 0);

    printf("test_net: nmea ok\n");
}

static void test_dtu(void)
{
    /* a vmesh wire frame rides the pipe and comes back intact */
    vmesh_msg_t m = {0};
    m.version = VMESH_PROTO_VERSION;
    m.msg_type = VMESH_MT_HAZARD;
    m.hazard_type = VMESH_HZ_DEBRIS;
    m.severity = 2;
    m.origin_id = 0xAABBCCDD;
    m.seq = 99;
    m.lat_e7 = 373310000;
    m.lon_e7 = -1220300000;
    m.created_s = 1750000789;
    m.ttl_s = 600;
    m.radius_m_x10 = 300;
    snprintf(m.note, sizeof(m.note), "pipe survivor");

    uint8_t wire[VMESH_WIRE_MAX];
    int wn = vmesh_wire_encode(&m, wire, sizeof(wire));
    assert(wn > 0);

    uint8_t framed[LORA_DTU_MAX_PAYLOAD + LORA_DTU_OVERHEAD];
    int fn = lora_dtu_frame(wire, (size_t)wn, framed, sizeof(framed));
    assert(fn == wn + LORA_DTU_OVERHEAD);

    /* deframe with garbage before, between sync-like bytes, and after */
    lora_dtu_rx_t rx = {0};
    uint8_t out[LORA_DTU_MAX_PAYLOAD];
    int got_len = 0;
    uint8_t noise[] = {0x00, 0x56, 0x00, 0xFF, 0x56, 0x4D, 0xFF};
    for (size_t i = 0; i < sizeof(noise); i++)
        assert(lora_dtu_rx_feed(&rx, noise[i], out, sizeof(out)) == 0);
    /* the 0x56 0x4D 0xFF above starts a bogus frame with len 0xFF ->
     * deframer must recover and still catch the real frame */
    for (int i = 0; i < fn; i++) {
        int r = lora_dtu_rx_feed(&rx, framed[i], out, sizeof(out));
        if (r > 0) got_len = r;
    }
    /* one full resync pass may consume the first real frame as bogus
     * payload; send it again to prove recovery */
    if (!got_len) {
        for (int i = 0; i < fn; i++) {
            int r = lora_dtu_rx_feed(&rx, framed[i], out, sizeof(out));
            if (r > 0) got_len = r;
        }
    }
    assert(got_len == wn);

    vmesh_msg_t back;
    assert(vmesh_wire_decode(out, (size_t)got_len, &back) == 0);
    assert(back.origin_id == m.origin_id);
    assert(strcmp(back.note, m.note) == 0);

    /* corrupt a payload byte: frame must be dropped */
    framed[5] ^= 0x40;
    lora_dtu_rx_t rx2 = {0};
    int r2 = 0;
    for (int i = 0; i < fn; i++)
        if (lora_dtu_rx_feed(&rx2, framed[i], out, sizeof(out)) > 0) r2 = 1;
    assert(r2 == 0);

    /* AT init block sanity */
    char cmds[256];
    int cl = lora_dtu_init_cmds(cmds, sizeof(cmds), 9, 0, 10, 22);
    assert(cl > 0);
    assert(strstr(cmds, "AT+SF=9"));
    assert(strstr(cmds, "AT+PWR=22"));
    assert(strstr(cmds, "AT+RSSI=0"));

    printf("test_net: lora_dtu ok\n");
}

int main(void)
{
    test_nmea();
    test_dtu();
    printf("test_net: all passed\n");
    return 0;
}
