/* tile_fetch.c — on-device offline-map download: fetch OSM raster
 * tiles for a bbox (z13..16) straight onto the TF card. The card
 * never has to leave the slot again (the DSI ribbon blocks it).
 *
 * Politeness (OSM tile policy): proper UA, ~150 ms between requests,
 * skip tiles already on disk, hard cap per download. This is dev-
 * scale use; the product path is self-hosted tiles (PMTiles-style).
 */

#include <math.h>
#include <stdio.h>
#include <string.h>
#include <sys/stat.h>

#include "esp_crt_bundle.h"
#include "esp_http_client.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

#include "provision.h"

/* optional local tile server (server/tileserver.py) — gitignored
 * header with e.g. #define TILE_SERVER_BASE "http://192.168.68.53:8484"
 * Cache-hits spare OSM during repeated local testing; every tile
 * falls back to OSM if the server is unreachable. */
#if __has_include("tile_server.h")
#include "tile_server.h"
#endif

static const char *TAG = "vmesh-tiledl";

#define ZMIN 13
#define ZMAX 16
#define TILE_CAP 1500       /* legacy bbox mode ("this screen") */
#define RING_TILE_CAP 40000 /* tier mode — WIDE is ~38k */
#define UA "vehicular-mesh-dev/0.1 (prototype; contact wmobil@gmail.com)"

static char s_root[64]; /* e.g. /sdcard/tiles — writable card only */
static volatile int s_done, s_total;
static volatile int s_new, s_have, s_fail; /* per-run outcome counts */
static volatile vmesh_tiledl_state_t s_state = VMESH_TILEDL_IDLE;
static volatile bool s_cancel;
static double s_bbox[4]; /* latmin, latmax, lonmin, lonmax */

/* ring mode: radius per zoom (km), ZMIN..ZMAX order; 0 = bbox mode */
static double s_clat, s_clon;
static double s_ring_km[ZMAX - ZMIN + 1];
static bool s_ring_mode;

/* per-tier radius tables, {z13, z14, z15, z16} km */
static const double TIER_KM[3][4] = {
    [VMESH_TILES_NEAR]   = { 15, 15, 15, 15 },
    [VMESH_TILES_REGION] = { 60, 60, 30, 15 },
    [VMESH_TILES_WIDE]   = { 120, 120, 50, 20 },
};

static int tile_x(double lon, int z)
{
    return (int)((lon + 180.0) / 360.0 * (double)(1 << z));
}

static int tile_y(double lat, int z)
{
    double r = lat * M_PI / 180.0;
    return (int)((1.0 - asinh(tan(r)) / M_PI) / 2.0 * (double)(1 << z));
}

/* is tile (z,x,y)'s center within radius_km of (s_clat, s_clon)?
 * equirectangular distance — plenty at <=120 km scales */
static bool tile_in_ring(int z, int x, int y, double radius_km)
{
    double n = (double)(1 << z);
    double tlon = (x + 0.5) / n * 360.0 - 180.0;
    double tlat = atan(sinh(M_PI * (1.0 - 2.0 * (y + 0.5) / n)))
                  * 180.0 / M_PI;
    double kx = (tlon - s_clon) * 111.32 * cos(s_clat * M_PI / 180.0);
    double ky = (tlat - s_clat) * 110.57;
    /* 3/4-tile slack so edge tiles aren't clipped mid-screen */
    double slack = 28092.0 / n * 0.75;
    double r = radius_km + slack;
    return kx * kx + ky * ky <= r * r;
}

/* bbox of a ring at zoom z (out: x0,x1,y0,y1) */
static void ring_bbox(int z, double radius_km, int *x0, int *x1,
                      int *y0, int *y1)
{
    double dlat = radius_km / 110.57;
    double dlon = radius_km / (111.32 * cos(s_clat * M_PI / 180.0));
    *x0 = tile_x(s_clon - dlon, z); *x1 = tile_x(s_clon + dlon, z);
    *y0 = tile_y(s_clat + dlat, z); *y1 = tile_y(s_clat - dlat, z);
}

static int ring_count(int z, double radius_km)
{
    int x0, x1, y0, y1, n = 0;
    ring_bbox(z, radius_km, &x0, &x1, &y0, &y1);
    for (int x = x0; x <= x1; x++)
        for (int y = y0; y <= y1; y++)
            if (tile_in_ring(z, x, y, radius_km)) n++;
    return n;
}

static const char *OSM_BASE = "https://tile.openstreetmap.org";

/* one HTTP client kept alive across tiles: TLS handshakes once per
 * source instead of once per tile (was the dominant cost — a REGION
 * download is 13k tiles) */
static esp_http_client_handle_t s_cli;
static const char *s_cli_base;
static bool s_last_was_osm;

static void keepalive_drop(void)
{
    if (s_cli) {
        esp_http_client_cleanup(s_cli);
        s_cli = NULL;
        s_cli_base = NULL;
    }
}

/* GET url -> path via the kept-alive client bound to base. On any
 * imperfection the client is dropped (connection state unknown) and
 * the next call rebuilds it. */
static bool http_get_to_file(const char *base, const char *url,
                             const char *path, char *buf, size_t bufsz)
{
    if (s_cli && s_cli_base != base) keepalive_drop();
    if (!s_cli) {
        esp_http_client_config_t cfg = {
            .url = url,
            .user_agent = UA,
            .timeout_ms = 15000,
            .crt_bundle_attach = esp_crt_bundle_attach,
            .keep_alive_enable = true,
        };
        s_cli = esp_http_client_init(&cfg);
        if (!s_cli) return false;
        s_cli_base = base;
    } else {
        esp_http_client_set_url(s_cli, url);
    }

    bool ok = false;
    if (esp_http_client_open(s_cli, 0) == ESP_OK &&
        esp_http_client_fetch_headers(s_cli) >= 0 &&
        esp_http_client_get_status_code(s_cli) == 200) {
        char tmp[136];
        snprintf(tmp, sizeof(tmp), "%s.part", path);
        FILE *f = fopen(tmp, "wb");
        if (f) {
            int r;
            ok = true;
            while ((r = esp_http_client_read(s_cli, buf, (int)bufsz)) > 0) {
                if (fwrite(buf, 1, (size_t)r, f) != (size_t)r) {
                    ok = false;
                    break;
                }
            }
            if (r < 0) ok = false;
            fclose(f);
            /* rename is the commit: a tile either fully exists or
             * doesn't — a reboot mid-write can't poison the resume */
            if (ok && rename(tmp, path) != 0) ok = false;
            if (!ok) remove(tmp);
        }
    }
    if (!ok) keepalive_drop();
    return ok;
}

/* 2 = already on card, 1 = fetched, 0 = failed */
static int fetch_one(int z, int x, int y, char *buf, size_t bufsz)
{
    char path[128], url[96];
    snprintf(path, sizeof(path), "%s/%d/%d/%d.png", s_root, z, x, y);
    struct stat st;
    if (stat(path, &st) == 0 && st.st_size > 0) return 2; /* have it */

    /* mkdir -p {root}/{z}/{x} */
    char dir[128];
    snprintf(dir, sizeof(dir), "%s/%d", s_root, z);
    mkdir(dir, 0777);
    snprintf(dir, sizeof(dir), "%s/%d/%d", s_root, z, x);
    mkdir(dir, 0777);

    const char *sources[2];
    int nsrc = 0;
#ifdef TILE_SERVER_BASE
    sources[nsrc++] = TILE_SERVER_BASE;
#endif
    sources[nsrc++] = OSM_BASE;

    for (int si = 0; si < nsrc; si++) {
        snprintf(url, sizeof(url), "%s/%d/%d/%d.png", sources[si], z, x, y);
        if (http_get_to_file(sources[si], url, path, buf, bufsz)) {
            s_last_was_osm = (sources[si] == OSM_BASE);
            return 1;
        }
    }
    return 0;
}

static void dl_task(void *arg)
{
    (void)arg;
    static char buf[4096];
    int fails = 0;

    for (int z = ZMIN; z <= ZMAX && !s_cancel; z++) {
        int x0, x1, y0, y1;
        double ring = s_ring_mode ? s_ring_km[z - ZMIN] : 0;
        if (s_ring_mode) {
            ring_bbox(z, ring, &x0, &x1, &y0, &y1);
        } else {
            x0 = tile_x(s_bbox[2], z); x1 = tile_x(s_bbox[3], z);
            y0 = tile_y(s_bbox[1], z); y1 = tile_y(s_bbox[0], z);
        }
        for (int x = x0; x <= x1 && !s_cancel; x++) {
            for (int y = y0; y <= y1 && !s_cancel; y++) {
                if (s_ring_mode && !tile_in_ring(z, x, y, ring))
                    continue; /* outside the circle, not counted */
                int r = fetch_one(z, x, y, buf, sizeof(buf));
                if (r == 0) { fails++; s_fail++; }
                else if (r == 1) s_new++;
                else s_have++;
                s_done++;
                if (s_done % 250 == 0)
                    ESP_LOGI(TAG, "%d/%d: %d new %d on-card %d failed",
                             s_done, s_total, s_new, s_have, s_fail);
                /* politeness is owed to OSM; our own server sets its
                 * own pace. already-on-card tiles cost nothing. */
                if (r == 0 || (r == 1 && s_last_was_osm))
                    vTaskDelay(pdMS_TO_TICKS(150));
                else if (r == 1)
                    vTaskDelay(pdMS_TO_TICKS(10));
            }
        }
    }
    keepalive_drop();

    ESP_LOGI(TAG, "download finished: %d/%d tiles, %d failed",
             s_done, s_total, fails);
    /* mostly-failed = something structural (no net, card gone) */
    s_state = (fails * 2 > s_total) ? VMESH_TILEDL_ERROR
                                    : VMESH_TILEDL_DONE;
    vTaskDelete(NULL);
}

/* ---- seam ops ---- */

static bool op_start(double lat_min, double lat_max,
                     double lon_min, double lon_max)
{
    if (s_state == VMESH_TILEDL_RUNNING) return false;
    if (!s_root[0]) return false; /* no writable card mounted */

    const vmesh_wifi_ops_t *w = vmesh_wifi_ops();
    char ip[20];
    if (!w || w->status(ip, sizeof(ip)) != VMESH_WIFI_CONNECTED)
        return false;

    int total = 0;
    for (int z = ZMIN; z <= ZMAX; z++) {
        int nx = tile_x(lon_max, z) - tile_x(lon_min, z) + 1;
        int ny = tile_y(lat_min, z) - tile_y(lat_max, z) + 1;
        total += nx * ny;
    }
    if (total <= 0 || total > TILE_CAP) {
        ESP_LOGW(TAG, "refusing download of %d tiles (cap %d)",
                 total, TILE_CAP);
        return false;
    }

    s_bbox[0] = lat_min; s_bbox[1] = lat_max;
    s_bbox[2] = lon_min; s_bbox[3] = lon_max;
    s_ring_mode = false;
    s_done = 0;
    s_new = s_have = s_fail = 0;
    s_total = total;
    s_cancel = false;
    s_state = VMESH_TILEDL_RUNNING;
    xTaskCreate(dl_task, "tiledl", 6144, NULL, 4, NULL);
    ESP_LOGI(TAG, "downloading %d tiles -> %s", total, s_root);
    return true;
}

static bool op_start_tier(double clat, double clon,
                          vmesh_tiles_tier_t tier)
{
    if (s_state == VMESH_TILEDL_RUNNING) return false;
    if (!s_root[0]) return false;
    if (tier > VMESH_TILES_WIDE) return false;

    const vmesh_wifi_ops_t *w = vmesh_wifi_ops();
    char ip[20];
    if (!w || w->status(ip, sizeof(ip)) != VMESH_WIFI_CONNECTED)
        return false;

    s_clat = clat; s_clon = clon;
    for (int z = ZMIN; z <= ZMAX; z++)
        s_ring_km[z - ZMIN] = TIER_KM[tier][z - ZMIN];

    s_cancel = false;
    int total = 0;
    for (int z = ZMIN; z <= ZMAX; z++)
        total += ring_count(z, s_ring_km[z - ZMIN]);
    if (total <= 0 || total > RING_TILE_CAP) {
        ESP_LOGW(TAG, "refusing tier download of %d tiles (cap %d)",
                 total, RING_TILE_CAP);
        return false;
    }

    s_ring_mode = true;
    s_done = 0;
    s_new = s_have = s_fail = 0;
    s_total = total;
    s_state = VMESH_TILEDL_RUNNING;
    xTaskCreate(dl_task, "tiledl", 6144, NULL, 4, NULL);
    ESP_LOGI(TAG, "tier %d: downloading %d tiles -> %s",
             (int)tier, total, s_root);
    return true;
}

static vmesh_tiledl_state_t op_progress(int *done, int *total)
{
    *done = s_done;
    *total = s_total;
    return s_state;
}

static void op_cancel(void) { s_cancel = true; }

static const char *op_detail(void)
{
    static char buf[64];
    snprintf(buf, sizeof(buf), "%d new / %d on card / %d failed",
             s_new, s_have, s_fail);
    return buf;
}

static const vmesh_tiledl_ops_t ops = {
    .start = op_start,
    .start_tier = op_start_tier,
    .progress = op_progress,
    .cancel = op_cancel,
    .detail = op_detail,
};

/* root: the WRITABLE tile root (TF card), or NULL if only SPIFFS is
 * available — downloads stay disabled then. */
void tile_fetch_init(const char *writable_root)
{
    if (writable_root)
        snprintf(s_root, sizeof(s_root), "%s", writable_root);
    vmesh_tiledl_set_ops(&ops);
}
