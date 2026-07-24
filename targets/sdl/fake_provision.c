/* fake_provision.c — desk-demo Wi-Fi + tile-download backends for the
 * simulator: the whole S6 flow works on the Mac with no radio and no
 * network. Scan "finds" canned networks, join always succeeds after a
 * moment, downloads tick a progress bar. UI-first, as ever. */

#include <stdio.h>
#include <string.h>
#include <time.h>

#include "provision.h"

static struct timespec t0;
static double now_s(void)
{
    struct timespec t;
    clock_gettime(CLOCK_MONOTONIC, &t);
    return (t.tv_sec - t0.tv_sec) + (t.tv_nsec - t0.tv_nsec) / 1e9;
}

/* ---- wifi ---- */

static double scan_started = -1, join_started = -1;
static bool joined;
static char joined_ssid[33];

static void fw_scan_start(void) { scan_started = now_s(); }

static int fw_scan_results(vmesh_wifi_net_t *out, int max)
{
    if (scan_started < 0 || now_s() - scan_started < 1.5) return -1;
    static const vmesh_wifi_net_t nets[] = {
        { "DECODDOG",       -48, true,  true  }, /* known: no pw prompt */
        { "xfinitywifi",    -71, false, false },
        { "Orenco-Guest",   -76, true,  false },
        { "NETGEAR-5G",     -82, true,  false },
    };
    int n = (int)(sizeof(nets) / sizeof(nets[0]));
    if (n > max) n = max;
    memcpy(out, nets, (size_t)n * sizeof(*out));
    scan_started = -1;
    return n;
}

static void fw_connect(const char *ssid, const char *pass)
{
    (void)pass;
    strncpy(joined_ssid, ssid, sizeof(joined_ssid) - 1);
    join_started = now_s();
    joined = false;
}

static vmesh_wifi_state_t fw_status(char *ip, size_t ipsz)
{
    if (join_started >= 0 && now_s() - join_started > 2.0) {
        joined = true;
        join_started = -1;
    }
    if (joined) {
        snprintf(ip, ipsz, "10.0.0.42");
        return VMESH_WIFI_CONNECTED;
    }
    if (join_started >= 0) return VMESH_WIFI_CONNECTING;
    return VMESH_WIFI_OFF;
}

static void fw_ssid(char *out, size_t sz)
{
    snprintf(out, sz, "%s", joined_ssid);
}

static const vmesh_wifi_ops_t fake_wifi = {
    .scan_start = fw_scan_start,
    .scan_results = fw_scan_results,
    .connect = fw_connect,
    .status = fw_status,
    .ssid = fw_ssid,
};

/* ---- tile download ---- */

#define FAKE_TOTAL 470
static double dl_started = -1;

static bool ft_start(double a, double b, double c, double d)
{
    (void)a; (void)b; (void)c; (void)d;
    if (!joined) return false; /* mirrors the device: Wi-Fi first */
    dl_started = now_s();
    return true;
}

static vmesh_tiledl_state_t ft_progress(int *done, int *total)
{
    *total = FAKE_TOTAL;
    if (dl_started < 0) { *done = 0; return VMESH_TILEDL_IDLE; }
    int d = (int)((now_s() - dl_started) * 60.0); /* ~8s to finish */
    if (d >= FAKE_TOTAL) { *done = FAKE_TOTAL; return VMESH_TILEDL_DONE; }
    *done = d;
    return VMESH_TILEDL_RUNNING;
}

static void ft_cancel(void) { dl_started = -1; }

static const vmesh_tiledl_ops_t fake_tiledl = {
    .start = ft_start,
    .progress = ft_progress,
    .cancel = ft_cancel,
};

void fake_provision_init(void)
{
    clock_gettime(CLOCK_MONOTONIC, &t0);
    vmesh_wifi_set_ops(&fake_wifi);
    vmesh_tiledl_set_ops(&fake_tiledl);
}
