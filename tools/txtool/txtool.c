/* txtool — send one Waycast hazard frame out a fleet LoRa dongle.
 * Reuses the repo's own wire encoder + DTU framing so the bytes match
 * the P4 exactly. Usage: txtool /dev/cu.usbmodemXXXX <seq> "<note>" */
#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <termios.h>
#include <time.h>
#include <unistd.h>

#include "vmesh_msg.h"
#include "vmesh_wire.h"
#include "lora_dtu.h"

int main(int argc, char **argv)
{
    if (argc < 3) { fprintf(stderr, "usage: %s <port> <seq> [note]\n", argv[0]); return 2; }
    const char *port = argv[1];
    int seq = atoi(argv[2]);
    const char *note = argc > 3 ? argv[3] : "mac dongle test";

    vmesh_msg_t m;
    memset(&m, 0, sizeof(m));
    m.version = VMESH_PROTO_VERSION;
    m.msg_type = VMESH_MT_HAZARD;
    m.hazard_type = VMESH_HZ_DEBRIS;
    m.severity = 2;
    m.channel = VMESH_CH_SAFETY;
    m.origin_id = 0x02BEEF01u;          /* unique test origin, no dedup clash */
    m.seq = (uint16_t)seq;
    m.lat_e7 = (int32_t)(45.52 * 1e7);
    m.lon_e7 = (int32_t)(-122.89 * 1e7);
    m.created_s = (uint32_t)time(NULL);
    m.ttl_s = 600;
    m.radius_m_x10 = 500;               /* 5 km */
    snprintf(m.note, sizeof(m.note), "%s", note);

    uint8_t wire[VMESH_WIRE_MAX];
    int wn = vmesh_wire_encode(&m, wire, sizeof(wire));
    if (wn <= 0) { fprintf(stderr, "encode failed\n"); return 1; }

    uint8_t framed[LORA_DTU_MAX_PAYLOAD + LORA_DTU_OVERHEAD];
    int fn = lora_dtu_frame(wire, (size_t)wn, framed, sizeof(framed));
    if (fn <= 0) { fprintf(stderr, "frame failed (wire %d bytes)\n", wn); return 1; }

    int fd = open(port, O_RDWR | O_NOCTTY);
    if (fd < 0) { perror("open"); return 1; }
    struct termios t;
    tcgetattr(fd, &t);
    cfmakeraw(&t);
    cfsetspeed(&t, B115200);
    tcsetattr(fd, TCSANOW, &t);

    ssize_t w = write(fd, framed, (size_t)fn);
    tcdrain(fd);
    close(fd);
    printf("sent %zd/%d bytes (wire %d) seq=%d note=\"%s\"\n", w, fn, wn, seq, note);
    return w == fn ? 0 : 1;
}
