/**
 * provision.h — Wi-Fi + map-download backend seam (feed-seam style).
 *
 * The UI (ui/settings.c) never talks to a network stack: it polls
 * these ops from the LVGL tick, so backends stay free-threaded.
 * Simulator registers fakes (whole flow demoable on the desk);
 * the device registers esp_wifi_remote + esp_http_client backends.
 *
 * Wi-Fi here is user-enabled infrastructure (map downloads, updates)
 * per the S5 trust story — the mesh never depends on it.
 */
#pragma once

#include <stdbool.h>
#include <stddef.h>

typedef struct {
    char ssid[33];
    int rssi;
    bool secured;
    bool known; /* credentials stored — joins without password prompt */
} vmesh_wifi_net_t;

typedef enum {
    VMESH_WIFI_OFF = 0,
    VMESH_WIFI_CONNECTING,
    VMESH_WIFI_CONNECTED,
    VMESH_WIFI_FAILED,
} vmesh_wifi_state_t;

typedef struct {
    void (*scan_start)(void);
    /* -1 = scan still running, else number of results copied */
    int (*scan_results)(vmesh_wifi_net_t *out, int max);
    /* pass == NULL: use stored credentials (known network) */
    void (*connect)(const char *ssid, const char *pass);
    vmesh_wifi_state_t (*status)(char *ip, size_t ipsz);
    /* current/target SSID ("" if none) — for the status-bar badge */
    void (*ssid)(char *out, size_t sz);
} vmesh_wifi_ops_t;

typedef enum {
    VMESH_TILEDL_IDLE = 0,
    VMESH_TILEDL_RUNNING,
    VMESH_TILEDL_DONE,
    VMESH_TILEDL_ERROR,
} vmesh_tiledl_state_t;

/* coverage tiers: zoom rings around a center — full z16 detail where
 * you park, z13-14 where you cruise (sizes at ~26 KB/tile avg) */
typedef enum {
    VMESH_TILES_NEAR = 0, /* all zooms to 15 km   (~5.4k tiles, ~140 MB) */
    VMESH_TILES_REGION,   /* 16@15 15@30 13-14@60 (~13k tiles, ~330 MB)  */
    VMESH_TILES_WIDE,     /* 16@20 15@50 13-14@120 (~38k tiles, ~1 GB)   */
} vmesh_tiles_tier_t;

typedef struct {
    /* fetch tiles covering the bbox (z13..16) into the tile root.
     * false = cannot start (no Wi-Fi, no writable card, busy). */
    bool (*start)(double lat_min, double lat_max,
                  double lon_min, double lon_max);
    /* ring fetch around a center per the tier table above */
    bool (*start_tier)(double clat, double clon, vmesh_tiles_tier_t tier);
    vmesh_tiledl_state_t (*progress)(int *done, int *total);
    void (*cancel)(void);
} vmesh_tiledl_ops_t;

/* ---- OTA firmware update (docs/OTA.md; device-only, sim = NULL) ---- */
typedef enum {
    VMESH_FW_IDLE = 0,
    VMESH_FW_CHECKING,
    VMESH_FW_UPTODATE,
    VMESH_FW_DOWNLOADING,
    VMESH_FW_READY, /* staged in the inactive slot — reboot to run */
    VMESH_FW_ERROR,
} vmesh_fw_state_t;

typedef struct {
    bool (*start)(void);              /* check + download (parked only) */
    vmesh_fw_state_t (*state)(void);
    const char *(*status)(void);      /* one-liner for the UI */
    const char *(*version)(void);     /* running firmware version */
    void (*reboot)(void);             /* boot the staged image */
} vmesh_fw_ops_t;

void vmesh_wifi_set_ops(const vmesh_wifi_ops_t *ops);
const vmesh_wifi_ops_t *vmesh_wifi_ops(void);
void vmesh_tiledl_set_ops(const vmesh_tiledl_ops_t *ops);
const vmesh_tiledl_ops_t *vmesh_tiledl_ops(void);
void vmesh_fw_set_ops(const vmesh_fw_ops_t *ops);
const vmesh_fw_ops_t *vmesh_fw_ops(void);
