/* wifi_mgr.c — device Wi-Fi backend for the provision seam.
 *
 * esp_wifi_remote -> ESP-Hosted -> onboard C6 over SDIO (the stack
 * proven on this kit by the p4-netdisplay project, including the
 * PSRAM-mempool boot-loop fix in sdkconfig.defaults).
 *
 * Credentials persist in NVS ("wifi": ssid/pass); if present we join
 * at boot. Wi-Fi is infrastructure only (tile downloads, updates) —
 * the mesh never depends on it (S5 trust story).
 */

#include <string.h>

#include "esp_event.h"
#include "esp_log.h"
#include "esp_netif.h"
#include "esp_wifi.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "nvs_flash.h"

#include "provision.h"

static const char *TAG = "vmesh-wifi";

static volatile vmesh_wifi_state_t s_state = VMESH_WIFI_OFF;
static char s_ip[20];
static volatile bool s_scanning;
static vmesh_wifi_net_t s_nets[12];
static volatile int s_nnets = -1;
static bool s_started;

static const char *known_pass(const char *ssid); /* fwd (used in evt) */

static void evt(void *arg, esp_event_base_t base, int32_t id, void *data)
{
    (void)arg;
    if (base == WIFI_EVENT && id == WIFI_EVENT_STA_DISCONNECTED) {
        if (s_state == VMESH_WIFI_CONNECTING) {
            /* one retry happens inside esp_wifi; call it failed */
            s_state = VMESH_WIFI_FAILED;
        } else if (s_state == VMESH_WIFI_CONNECTED) {
            s_state = VMESH_WIFI_CONNECTING; /* roam/retry */
            esp_wifi_connect();
        }
    } else if (base == WIFI_EVENT && id == WIFI_EVENT_SCAN_DONE) {
        uint16_t n = 0;
        wifi_ap_record_t recs[12];
        uint16_t want = 12;
        esp_wifi_scan_get_ap_num(&n);
        if (n > want) n = want;
        if (esp_wifi_scan_get_ap_records(&n, recs) == ESP_OK) {
            for (int i = 0; i < n; i++) {
                strncpy(s_nets[i].ssid, (const char *)recs[i].ssid,
                        sizeof(s_nets[i].ssid) - 1);
                s_nets[i].ssid[sizeof(s_nets[i].ssid) - 1] = 0;
                s_nets[i].rssi = recs[i].rssi;
                s_nets[i].secured =
                    recs[i].authmode != WIFI_AUTH_OPEN;
                s_nets[i].known =
                    known_pass(s_nets[i].ssid) != NULL;
            }
            s_nnets = n;
        } else {
            s_nnets = 0;
        }
        s_scanning = false;
    } else if (base == IP_EVENT && id == IP_EVENT_STA_GOT_IP) {
        ip_event_got_ip_t *e = data;
        snprintf(s_ip, sizeof(s_ip), IPSTR, IP2STR(&e->ip_info.ip));
        s_state = VMESH_WIFI_CONNECTED;
        ESP_LOGI(TAG, "connected, ip %s", s_ip);
    }
}

static char s_ssid[33];

/* ---- known networks (Mac/Windows-style): up to 8, recency-ordered,
 * persisted in NVS as slot keys s0/p0..s7/p7 + count n ---- */
#define KNOWN_MAX 8
typedef struct { char ssid[33]; char pass[65]; } known_t;
static known_t s_known[KNOWN_MAX];
static int s_nknown;

static void known_load(void)
{
    nvs_handle_t h;
    if (nvs_open("wifi", NVS_READONLY, &h) != ESP_OK) return;
    uint8_t n = 0;
    nvs_get_u8(h, "n", &n);
    if (n > KNOWN_MAX) n = KNOWN_MAX;
    for (int i = 0; i < n; i++) {
        char k[12];
        size_t sl = sizeof(s_known[i].ssid), pl = sizeof(s_known[i].pass);
        snprintf(k, sizeof(k), "s%d", i);
        if (nvs_get_str(h, k, s_known[i].ssid, &sl) != ESP_OK) break;
        snprintf(k, sizeof(k), "p%d", i);
        if (nvs_get_str(h, k, s_known[i].pass, &pl) != ESP_OK) break;
        s_nknown = i + 1;
    }
    /* migrate the old single-network keys, if present */
    if (s_nknown == 0) {
        char ssid[33] = "", pass[65] = "";
        size_t sl = sizeof(ssid), pl = sizeof(pass);
        if (nvs_get_str(h, "ssid", ssid, &sl) == ESP_OK && ssid[0]) {
            nvs_get_str(h, "pass", pass, &pl);
            strcpy(s_known[0].ssid, ssid);
            strcpy(s_known[0].pass, pass);
            s_nknown = 1;
        }
    }
    nvs_close(h);
}

static void known_persist(void)
{
    nvs_handle_t h;
    if (nvs_open("wifi", NVS_READWRITE, &h) != ESP_OK) return;
    nvs_set_u8(h, "n", (uint8_t)s_nknown);
    for (int i = 0; i < s_nknown; i++) {
        char k[12];
        snprintf(k, sizeof(k), "s%d", i);
        nvs_set_str(h, k, s_known[i].ssid);
        snprintf(k, sizeof(k), "p%d", i);
        nvs_set_str(h, k, s_known[i].pass);
    }
    nvs_commit(h);
    nvs_close(h);
}

static const char *known_pass(const char *ssid)
{
    for (int i = 0; i < s_nknown; i++)
        if (strcmp(s_known[i].ssid, ssid) == 0) return s_known[i].pass;
    return NULL;
}

/* insert/update at front (most-recent-first order drives auto-join) */
static void known_touch(const char *ssid, const char *pass)
{
    known_t e;
    memset(&e, 0, sizeof(e));
    strncpy(e.ssid, ssid, sizeof(e.ssid) - 1);
    strncpy(e.pass, pass, sizeof(e.pass) - 1);
    int at = s_nknown;
    for (int i = 0; i < s_nknown; i++)
        if (strcmp(s_known[i].ssid, ssid) == 0) { at = i; break; }
    if (at == s_nknown && s_nknown < KNOWN_MAX) s_nknown++;
    if (at >= KNOWN_MAX) at = KNOWN_MAX - 1;
    for (int i = at; i > 0; i--) s_known[i] = s_known[i - 1];
    s_known[0] = e;
    known_persist();
}

static void do_connect(const char *ssid, const char *pass)
{
    strncpy(s_ssid, ssid, sizeof(s_ssid) - 1);
    wifi_config_t sta = { 0 };
    strncpy((char *)sta.sta.ssid, ssid, sizeof(sta.sta.ssid) - 1);
    strncpy((char *)sta.sta.password, pass, sizeof(sta.sta.password) - 1);
    esp_wifi_set_config(WIFI_IF_STA, &sta);
    s_state = VMESH_WIFI_CONNECTING;
    esp_wifi_disconnect();
    esp_wifi_connect();
}

/* ---- seam ops (called from LVGL task; keep them non-blocking) ---- */

static void op_scan_start(void)
{
    if (s_scanning) return;
    s_scanning = true;
    s_nnets = -1;
    wifi_scan_config_t sc = { 0 };
    if (esp_wifi_scan_start(&sc, false) != ESP_OK) {
        s_scanning = false;
        s_nnets = 0;
    }
}

static int op_scan_results(vmesh_wifi_net_t *out, int max)
{
    if (s_scanning || s_nnets < 0) return -1;
    int n = s_nnets < max ? s_nnets : max;
    memcpy(out, s_nets, (size_t)n * sizeof(*out));
    return n;
}

static void op_connect(const char *ssid, const char *pass)
{
    if (!pass) { /* known network: use stored credentials */
        const char *stored = known_pass(ssid);
        pass = stored ? stored : "";
    }
    known_touch(ssid, pass);
    do_connect(ssid, pass);
}

static vmesh_wifi_state_t op_status(char *ip, size_t ipsz)
{
    if (s_state == VMESH_WIFI_CONNECTED) snprintf(ip, ipsz, "%s", s_ip);
    else if (ipsz) ip[0] = 0;
    return s_state;
}

static void op_ssid(char *out, size_t sz)
{
    snprintf(out, sz, "%s", s_ssid);
}

static const vmesh_wifi_ops_t ops = {
    .scan_start = op_scan_start,
    .scan_results = op_scan_results,
    .connect = op_connect,
    .status = op_status,
    .ssid = op_ssid,
};

/* Mac/Windows-style behavior: whenever we're not connected, scan and
 * join the most-recently-used known network that's in the air. Runs
 * at boot (fast first attempt) and retries while disconnected — so
 * driving back into home coverage reconnects by itself. */
static void auto_join_task(void *arg)
{
    (void)arg;
    vTaskDelay(pdMS_TO_TICKS(1500)); /* let the C6 link settle */
    for (;;) {
        if ((s_state == VMESH_WIFI_OFF || s_state == VMESH_WIFI_FAILED)
            && s_nknown > 0 && !s_scanning) {
            s_scanning = true;
            s_nnets = -1;
            wifi_scan_config_t sc = { 0 };
            if (esp_wifi_scan_start(&sc, true) == ESP_OK) {
                /* SCAN_DONE handler fills s_nets; give it a moment */
                vTaskDelay(pdMS_TO_TICKS(300));
                for (int k = 0; k < s_nknown; k++) {
                    bool seen = false;
                    for (int i = 0; i < s_nnets && !seen; i++)
                        seen = strcmp(s_nets[i].ssid,
                                      s_known[k].ssid) == 0;
                    if (seen) {
                        ESP_LOGI(TAG, "auto-joining known \"%s\"",
                                 s_known[k].ssid);
                        do_connect(s_known[k].ssid, s_known[k].pass);
                        break;
                    }
                }
            } else {
                s_scanning = false;
            }
        }
        vTaskDelay(pdMS_TO_TICKS(45000));
    }
}

void wifi_mgr_start(void)
{
    esp_err_t err = nvs_flash_init();
    if (err == ESP_ERR_NVS_NO_FREE_PAGES ||
        err == ESP_ERR_NVS_NEW_VERSION_FOUND) {
        nvs_flash_erase();
        nvs_flash_init();
    }

    if (esp_netif_init() != ESP_OK) return;
    esp_event_loop_create_default(); /* ok if it already exists */
    esp_netif_create_default_wifi_sta();

    wifi_init_config_t icfg = WIFI_INIT_CONFIG_DEFAULT();
    if (esp_wifi_init(&icfg) != ESP_OK) {
        ESP_LOGW(TAG, "wifi init failed (C6/hosted?) — Wi-Fi off");
        return;
    }
    esp_event_handler_register(WIFI_EVENT, ESP_EVENT_ANY_ID, evt, NULL);
    esp_event_handler_register(IP_EVENT, IP_EVENT_STA_GOT_IP, evt, NULL);
    esp_wifi_set_mode(WIFI_MODE_STA);
    if (esp_wifi_start() != ESP_OK) {
        ESP_LOGW(TAG, "wifi start failed — Wi-Fi off");
        return;
    }
    s_started = true;
    known_load();
    vmesh_wifi_set_ops(&ops);
    xTaskCreate(auto_join_task, "wifi_auto", 4096, NULL, 3, NULL);
    ESP_LOGI(TAG, "Wi-Fi backend up (%d known networks)", s_nknown);
}
