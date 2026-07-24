/* serial_radio.c — the simulator's real radio: a Waveshare USB-TO-LoRa
 * dongle on a POSIX serial port. Same lora_dtu framing and the same
 * mesh core as the device — plug dongle #2 into the laptop and the
 * simulator becomes a genuine mesh node.
 *
 *   ./build/vmesh-sim scenario.json --radio /dev/cu.usbserial-XXXX
 */

#include "feed.h"
#include "lora_dtu.h"
#include "vmesh_mesh.h"
#include "vmesh_wire.h"

#include <fcntl.h>
#include <pthread.h>
#include <stdio.h>
#include <string.h>
#include <termios.h>
#include <time.h>
#include <unistd.h>

static int s_fd = -1;
static pthread_mutex_t s_tx_lock = PTHREAD_MUTEX_INITIALIZER;
static vmesh_mesh_t s_mesh;
static pthread_mutex_t s_mesh_lock = PTHREAD_MUTEX_INITIALIZER;

static uint32_t rt_ms(void)
{
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (uint32_t)(ts.tv_sec * 1000 + ts.tv_nsec / 1000000);
}

static void send_framed(const uint8_t *wire, int n)
{
    uint8_t framed[LORA_DTU_MAX_PAYLOAD + LORA_DTU_OVERHEAD];
    int fn = lora_dtu_frame(wire, (size_t)n, framed, sizeof(framed));
    if (fn <= 0 || s_fd < 0) return;
    pthread_mutex_lock(&s_tx_lock);
    write(s_fd, framed, (size_t)fn);
    pthread_mutex_unlock(&s_tx_lock);
}

/* feed publish hook: our reports/votes leave over the air too */
static void radio_tx_hook(const vmesh_msg_t *m)
{
    uint8_t wire[VMESH_WIRE_MAX];
    int n = vmesh_wire_encode(m, wire, sizeof(wire));
    if (n <= 0) return;
    pthread_mutex_lock(&s_mesh_lock);
    mesh_note_own(&s_mesh, m);
    pthread_mutex_unlock(&s_mesh_lock);
    send_framed(wire, n);
    printf("[radio tx] %08X/%u\n", m->origin_id, m->seq);
}

static void relay_cb(const vmesh_msg_t *m, void *user)
{
    (void)user;
    uint8_t wire[VMESH_WIRE_MAX];
    int n = vmesh_wire_encode(m, wire, sizeof(wire));
    if (n > 0) {
        send_framed(wire, n);
        printf("[radio relay] %08X/%u hop %u\n", m->origin_id, m->seq,
               m->hops);
    }
}

static void *rx_thread(void *arg)
{
    (void)arg;
    lora_dtu_rx_t rx = {0};
    uint8_t byte, payload[LORA_DTU_MAX_PAYLOAD];
    while (read(s_fd, &byte, 1) >= 0) {
        int n = lora_dtu_rx_feed(&rx, byte, payload, sizeof(payload));
        if (n <= 0) continue;
        vmesh_msg_t m;
        if (vmesh_wire_decode(payload, (size_t)n, &m) != 0) continue;
        vmesh_pose_t pose;
        vmesh_pose_get(&pose);
        pthread_mutex_lock(&s_mesh_lock);
        /* freshness must compare in the SAME timebase the sender
         * stamped created_s with (vmesh_time_s = SIM_EPOCH+clock), NOT
         * wall-clock time(NULL) — else every frame looks ~400 days old
         * and is dropped as expired. */
        bool deliver = mesh_rx(&s_mesh, &m, 0.5f, pose.lat, pose.lon,
                               rt_ms(), vmesh_time_s());
        pthread_mutex_unlock(&s_mesh_lock);
        printf("[radio rx] %08X/%u hops=%u %s\n", m.origin_id, m.seq,
               m.hops, deliver ? "-> feed" : "(dropped)");
        if (deliver) vmesh_feed_inject(&m);
    }
    return NULL;
}

/* call from the main loop, ~every 20-50 ms */
void serial_radio_tick(void)
{
    if (s_fd < 0) return;
    pthread_mutex_lock(&s_mesh_lock);
    mesh_tick(&s_mesh, rt_ms(), relay_cb, NULL);
    pthread_mutex_unlock(&s_mesh_lock);
}

int serial_radio_start(const char *dev)
{
    s_fd = open(dev, O_RDWR | O_NOCTTY);
    if (s_fd < 0) {
        fprintf(stderr, "serial_radio: cannot open %s\n", dev);
        return -1;
    }
    struct termios t;
    tcgetattr(s_fd, &t);
    cfmakeraw(&t);
    cfsetspeed(&t, B115200);
    t.c_cc[VMIN] = 1;
    t.c_cc[VTIME] = 0;
    tcsetattr(s_fd, TCSANOW, &t);

    /* same PHY profile as the device side */
    char cmds[256];
    int n = lora_dtu_init_cmds(cmds, sizeof(cmds), 7, 0, 65, 22);
    /* CH65 = 850+65 = 915.0 MHz (legal US ISM), SF7 — matches the
     * dongles' AT-set config and the HAT. NOTE: these AT strings only
     * take effect if the dongle is in AT mode (needs a leading
     * "+++\r\n" + trailing AT+EXIT); as plain stream data they are
     * transmitted, not applied. The dongles are pre-configured over AT
     * (persisted), so this is belt-and-suspenders. */
    if (n > 0) write(s_fd, cmds, (size_t)n);

    mesh_config_t cfg = { .jitter_min_ms = 100,
                          .jitter_max_ms = 800,
                          .suppress_threshold = 2 };
    mesh_init(&s_mesh, &cfg, (uint32_t)time(NULL));
    vmesh_feed_set_tx_hook(radio_tx_hook);

    pthread_t th;
    pthread_create(&th, NULL, rx_thread, NULL);
    pthread_detach(th);
    printf("serial_radio: %s up (SF7/BW125/CH65=915MHz, DTU-framed) — the "
           "simulator is now a mesh node\n", dev);
    return 0;
}
