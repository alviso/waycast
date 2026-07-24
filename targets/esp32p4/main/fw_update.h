/* fw_update.h — OTA firmware update client (docs/OTA.md).
 * Manifest-driven esp_https_ota into the inactive A/B slot; rollback
 * gate lives in main.c (mark-valid after the boot self-check). */
#pragma once

#include <stdbool.h>

typedef enum {
    FW_IDLE = 0,
    FW_CHECKING,    /* fetching/parsing the manifest      */
    FW_UPTODATE,    /* manifest version == running version */
    FW_DOWNLOADING, /* esp_https_ota in progress           */
    FW_READY,       /* new image staged — reboot to finish */
    FW_ERROR,
} fw_state_t;

/* Kick off a check-and-download (no-op if already running).
 * false = refused (moving, or already in progress). */
bool fw_update_start(void);

fw_state_t fw_update_state(void);

/* Human-readable one-liner for the settings UI. */
const char *fw_update_status(void);

/* Running firmware version (esp_app_desc). */
const char *fw_update_version(void);

/* Register the provision-seam ops (call once at boot). */
void fw_update_init(void);
