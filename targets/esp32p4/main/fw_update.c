/* fw_update.c — OTA firmware update client (docs/OTA.md).
 *
 * Flow: GET manifest.json (static file on the existing waycast.io
 * host) -> compare "version" against the running app's version ->
 * esp_https_ota streams the image into the inactive A/B slot ->
 * FW_READY, user reboots when convenient. The freshly-booted image
 * runs pending-verify until main.c's self-check gate marks it valid;
 * a crash before that and the bootloader reverts (rollback).
 *
 * Trust model v1: TLS + the cert bundle authenticate the server (same
 * scheme the tile fetch uses). App-layer ed25519 manifest signature +
 * Secure Boot v2 are the production hardening steps — see docs/OTA.md.
 *
 * Car-safety: refuses to start while moving; the mesh keeps running
 * during the download; nothing reboots without the user asking. */

#include <stdio.h>
#include <string.h>

#include "esp_app_desc.h"
#include "esp_crt_bundle.h"
#include "esp_http_client.h"
#include "esp_https_ota.h"
#include "esp_log.h"
#include "esp_system.h" /* esp_restart */
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

#include "cJSON.h"
#include "feed.h" /* vmesh_pose_get — the parked check */
#include "provision.h"

#include "fw_update.h"

/* per-hardware manifest: each board type has its own release
 * channel; one git tag releases all boards (docs/OTA.md) */
#define MANIFEST_URL \
    "https://waycast.io/fw/manifest-" CONFIG_VMESH_HW ".json"
#define MANIFEST_MAX 1024

static const char *TAG = "vmesh-fw";

static volatile fw_state_t s_state = FW_IDLE;
static char s_status[96] = "";
static char s_new_version[32];

const char *fw_update_version(void)
{
    return esp_app_get_description()->version;
}

fw_state_t fw_update_state(void) { return s_state; }

const char *fw_update_status(void)
{
    return s_status;
}

static void set_status(fw_state_t st, const char *fmt, const char *arg)
{
    s_state = st;
    snprintf(s_status, sizeof(s_status), fmt, arg);
}

/* fetch the manifest into buf (NUL-terminated); false on any failure */
static bool fetch_manifest(char *buf, size_t cap)
{
    esp_http_client_config_t cfg = {
        .url = MANIFEST_URL,
        .timeout_ms = 10000,
        .crt_bundle_attach = esp_crt_bundle_attach,
    };
    esp_http_client_handle_t cli = esp_http_client_init(&cfg);
    if (!cli) return false;
    bool ok = false;
    if (esp_http_client_open(cli, 0) == ESP_OK &&
        esp_http_client_fetch_headers(cli) >= 0 &&
        esp_http_client_get_status_code(cli) == 200) {
        int n = esp_http_client_read_response(cli, buf, (int)cap - 1);
        if (n > 0) {
            buf[n] = 0;
            ok = true;
        }
    }
    esp_http_client_cleanup(cli);
    return ok;
}

static void fw_task(void *arg)
{
    (void)arg;
    static char manifest[MANIFEST_MAX];

    set_status(FW_CHECKING, "checking...", NULL);
    if (!fetch_manifest(manifest, sizeof(manifest))) {
        set_status(FW_ERROR, "can't reach update server", NULL);
        vTaskDelete(NULL);
        return;
    }

    cJSON *j = cJSON_Parse(manifest);
    const cJSON *jver = j ? cJSON_GetObjectItem(j, "version") : NULL;
    const cJSON *jurl = j ? cJSON_GetObjectItem(j, "url") : NULL;
    if (!cJSON_IsString(jver) || !cJSON_IsString(jurl)) {
        cJSON_Delete(j);
        set_status(FW_ERROR, "bad manifest", NULL);
        vTaskDelete(NULL);
        return;
    }

    /* hardware gate: an image for another board would boot, pass the
     * self-check, and leave this panel dark forever */
    const cJSON *jhw = cJSON_GetObjectItem(j, "hw");
    if (cJSON_IsString(jhw) &&
        strcmp(jhw->valuestring, CONFIG_VMESH_HW) != 0) {
        set_status(FW_UPTODATE, "no build for this hardware yet", NULL);
        cJSON_Delete(j);
        vTaskDelete(NULL);
        return;
    }

    const char *cur = fw_update_version();
    if (strcmp(jver->valuestring, cur) == 0) {
        set_status(FW_UPTODATE, "up to date (%s)", cur);
        cJSON_Delete(j);
        vTaskDelete(NULL);
        return;
    }
    snprintf(s_new_version, sizeof(s_new_version), "%s",
             jver->valuestring);

    static char url[160]; /* outlives the cJSON tree */
    snprintf(url, sizeof(url), "%s", jurl->valuestring);
    cJSON_Delete(j);
    ESP_LOGI(TAG, "update %s -> %s (%s)", cur, s_new_version, url);

    /* stream into the inactive slot */
    esp_http_client_config_t http = {
        .url = url,
        .timeout_ms = 20000,
        .crt_bundle_attach = esp_crt_bundle_attach,
        .keep_alive_enable = true,
    };
    esp_https_ota_config_t ota = { .http_config = &http };
    esp_https_ota_handle_t h = NULL;
    if (esp_https_ota_begin(&ota, &h) != ESP_OK) {
        set_status(FW_ERROR, "download start failed", NULL);
        vTaskDelete(NULL);
        return;
    }
    int total = esp_https_ota_get_image_size(h);
    esp_err_t e;
    while ((e = esp_https_ota_perform(h)) ==
           ESP_ERR_HTTPS_OTA_IN_PROGRESS) {
        int got = esp_https_ota_get_image_len_read(h);
        s_state = FW_DOWNLOADING;
        /* coarse 20%% buckets: every status change repaints the shroud
         * label, and every repaint during flash writes is a visible
         * flicker event — five per download is the budget */
        int pct = total > 0 ? got * 100 / total : 0;
        snprintf(s_status, sizeof(s_status), "downloading %d%%",
                 pct - pct % 20);
    }
    if (e != ESP_OK || !esp_https_ota_is_complete_data_received(h)) {
        esp_https_ota_abort(h);
        set_status(FW_ERROR, "download failed", NULL);
        vTaskDelete(NULL);
        return;
    }
    if (esp_https_ota_finish(h) != ESP_OK) { /* validates + sets boot */
        set_status(FW_ERROR, "image rejected", NULL);
        vTaskDelete(NULL);
        return;
    }

    set_status(FW_READY, "v%s staged - tap to reboot", s_new_version);
    ESP_LOGI(TAG, "OTA staged; reboot to run %s", s_new_version);
    vTaskDelete(NULL);
}

bool fw_update_start(void)
{
    if (s_state == FW_CHECKING || s_state == FW_DOWNLOADING)
        return false;

    /* never while moving — updates are a parked activity */
    vmesh_pose_t p;
    vmesh_pose_get(&p);
    if (p.speed_mps > 0.5f) {
        set_status(FW_ERROR, "not while driving", NULL);
        return false;
    }

    if (xTaskCreate(fw_task, "fw_ota", 8192, NULL, 5, NULL) != pdPASS) {
        set_status(FW_ERROR, "no memory", NULL);
        return false;
    }
    return true;
}

/* ---- provision-seam registration (settings UI is target-agnostic) ---- */

static vmesh_fw_state_t op_state(void)
{
    return (vmesh_fw_state_t)s_state; /* enums mirror each other */
}

static void op_reboot(void) { esp_restart(); }

static const vmesh_fw_ops_t fw_ops = {
    .start = fw_update_start,
    .state = op_state,
    .status = fw_update_status,
    .version = fw_update_version,
    .reboot = op_reboot,
};

void fw_update_init(void)
{
    vmesh_fw_set_ops(&fw_ops);
}
