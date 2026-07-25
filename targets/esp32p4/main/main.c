/* targets/esp32p4/main/main.c — device target: same shape as the SDL
 * main, but the display comes from the board BSP and the scenario JSON
 * is embedded in flash. All product logic is in ui/ + msg/ + sim/. */

#include "bsp/esp-bsp.h"
#include "esp_ldo_regulator.h"
#include "esp_vfs_fat.h"
#include "driver/sdmmc_host.h"
#include "sdmmc_cmd.h"
#include "sd_pwr_ctrl_by_on_chip_ldo.h"
#include "esp_ota_ops.h"
#include "esp_log.h"
#include "esp_mac.h"
#include "esp_efuse.h"
#include "esp_timer.h"
#include "nvs.h"
#include "lvgl.h"

#include <dirent.h>

#include "map_view.h"
#include "feed.h"
#include "scenario.h"
#include "ui.h"


#define VMESH_BENCH_POSE 0 /* RF bench test: pin pose (1 = pin, 0 = driving demo) */
#define VMESH_HAT_GPS    0 /* RETIRED July 21: the kit's header routes
                            * neither GNSS UART direction usefully
                            * (phys10 -> nothing; phys8 -> pad 37 would
                            * need a bodge jumper that also breaks
                            * flashing). Device GPS = USB G-mouse
                            * (VK-172) via usb_serial.cpp. HAT = LoRa
                            * only. Full story: hat_pins.h. */
#define VMESH_TILES_SD   1 /* TF-card tiles (works with the HAT removed) */
#define VMESH_HAT_RADIO  0 /* July 23: HAT-off ground truth — SD card init
                            * FAILS (0x108) whenever the HAT is on the
                            * header, at every clock/width, two different
                            * cards, radio software never started. With
                            * the HAT off: mount OK. Electrical, not
                            * firmware. So: HAT off, radio = USB LoRa
                            * dongle (usb_serial.cpp takes TX when
                            * radio_bonnet_active is false). NOTE: GPS
                            * mouse + dongle on the single USB-A port
                            * needs a hub. */
#define VMESH_HAT_AUTOSCAN 0 /* one-shot pin discovery for the SX1262 HAT */ /* 1 = start L76K GPS (steals console pins); 0 = keep console on bridge for radio bring-up */

static const char *TAG = "vmesh";
static char g_sd_diag[96] = ""; /* SD experiment result, shown on screen */

/* embedded by EMBED_TXTFILES in main/CMakeLists.txt */
extern const char scenario_json_start[] asm("_binary_highway_demo_json_start");
extern const char scenario_json_end[]   asm("_binary_highway_demo_json_end");

/* Pump the simulation clock from the LVGL task (an lv_timer runs in the
 * esp_lvgl_port task, so no extra locking is needed around sim state). */
static void scenario_pump_cb(lv_timer_t *t)
{
    (void)t;
    static int64_t last_us, last_log_us;
    static int calls;
    int64_t now_us = esp_timer_get_time();
    if (last_us != 0)
        scenario_update((float)(now_us - last_us) / 1e6f);
    last_us = now_us;

    /* perf heartbeat: pump cadence tells us whether the LVGL task is
     * cycling healthily (~20 Hz) or stuck in multi-second frames */
    calls++;
    if (now_us - last_log_us > 5 * 1000 * 1000) {
        if (last_log_us != 0)
            ESP_LOGI(TAG, "pump %.1f Hz | sim t=%.0fs | heap %u KB free",
                     calls / ((now_us - last_log_us) / 1e6f),
                     (double)scenario_clock_s(),
                     (unsigned)(esp_get_free_heap_size() / 1024));
        calls = 0;
        last_log_us = now_us;
    }
}

/* persisted demo-mode flag (settings toggle writes it; boot reads it).
 * Product default OFF — real GPS + real radio only. NVS is up by here
 * (wifi_mgr_start ran nvs_flash_init). */
void waycast_save_demo(bool on)
{
    nvs_handle_t h;
    if (nvs_open("waycast", NVS_READWRITE, &h) != ESP_OK) return;
    nvs_set_u8(h, "demo", on ? 1 : 0);
    nvs_commit(h);
    nvs_close(h);
}
static bool waycast_load_demo(void)
{
    nvs_handle_t h;
    uint8_t v = 0;
    if (nvs_open("waycast", NVS_READONLY, &h) == ESP_OK) {
        nvs_get_u8(h, "demo", &v);
        nvs_close(h);
    }
    return v != 0;
}

/* last-known GPS position across reboots (lat/lon as 1e7 fixed point).
 * gps_rx saves it (throttled); boot restores it as the stale pose so
 * the map opens where you were, not at 0,0 off Africa. */
void waycast_save_loc(double lat, double lon)
{
    nvs_handle_t h;
    if (nvs_open("waycast", NVS_READWRITE, &h) != ESP_OK) return;
    nvs_set_i32(h, "lat", (int32_t)(lat * 1e7));
    nvs_set_i32(h, "lon", (int32_t)(lon * 1e7));
    nvs_commit(h);
    nvs_close(h);
}
/* pseudonym handle (§7¾): saved by the settings editor, restored at
 * boot. 15 chars + NUL, matching the names cache. */
void waycast_save_handle(const char *hd)
{
    nvs_handle_t h;
    if (nvs_open("waycast", NVS_READWRITE, &h) != ESP_OK) return;
    nvs_set_str(h, "handle", hd ? hd : "");
    nvs_commit(h);
    nvs_close(h);
}
static void waycast_load_handle(char *out, size_t cap)
{
    nvs_handle_t h;
    out[0] = 0;
    if (nvs_open("waycast", NVS_READONLY, &h) == ESP_OK) {
        size_t len = cap;
        nvs_get_str(h, "handle", out, &len);
        nvs_close(h);
    }
}

/* own message seq across reboots — a reset seq collides with the
 * network's dedup (origin/seq pairs already seen get dropped). Saved
 * at bump time, BEFORE the frame is transmitted. */
void waycast_save_seq(uint16_t seq)
{
    nvs_handle_t h;
    if (nvs_open("waycast", NVS_READWRITE, &h) != ESP_OK) return;
    nvs_set_u16(h, "seq", seq);
    nvs_commit(h);
    nvs_close(h);
}
static uint16_t waycast_load_seq(void)
{
    nvs_handle_t h;
    uint16_t v = 0;
    if (nvs_open("waycast", NVS_READONLY, &h) == ESP_OK) {
        nvs_get_u16(h, "seq", &v);
        nvs_close(h);
    }
    return v;
}

static bool waycast_load_loc(double *lat, double *lon)
{
    nvs_handle_t h;
    int32_t la = 0, lo = 0;
    bool ok = false;
    if (nvs_open("waycast", NVS_READONLY, &h) == ESP_OK) {
        if (nvs_get_i32(h, "lat", &la) == ESP_OK &&
            nvs_get_i32(h, "lon", &lo) == ESP_OK && (la || lo)) {
            *lat = la / 1e7;
            *lon = lo / 1e7;
            ok = true;
        }
        nvs_close(h);
    }
    return ok;
}

/* USB diagnostic label updater (LVGL task context) */
void usb_diag_tick(lv_timer_t *t)
{
    extern const char *usb_status_str(void);
    lv_obj_t *lbl = (lv_obj_t *)lv_timer_get_user_data(t);
    if (g_sd_diag[0])
        lv_label_set_text_fmt(lbl, "%s\n%s", usb_status_str(), g_sd_diag);
    else
        lv_label_set_text(lbl, usb_status_str());
}

#if VMESH_TILES_SD
/* Faithful copy of bsp_sdcard_mount()'s LDO4 power flow (the piece the
 * old custom mount got wrong), but with width/clock as parameters so we
 * can trade the 40 MHz 4-bit default down to something the HAT-loaded
 * SD pads can actually clock. Populates the BSP's global bsp_sdcard so
 * unmount and the tile reader keep working. Releases LDO4 on failure so
 * the next tier can re-acquire it. */
static esp_err_t __attribute__((unused)) sd_try_mount(int width, int khz)
{
    const esp_vfs_fat_sdmmc_mount_config_t mcfg = {
        .format_if_mount_failed = false,
        .max_files = 5,
        .allocation_unit_size = 64 * 1024,
    };
    sdmmc_host_t host = SDMMC_HOST_DEFAULT();
    host.slot = SDMMC_HOST_SLOT_0;
    host.max_freq_khz = khz;

    sd_pwr_ctrl_ldo_config_t ldo_cfg = { .ldo_chan_id = 4 };
    sd_pwr_ctrl_handle_t pwr = NULL;
    esp_err_t e = sd_pwr_ctrl_new_on_chip_ldo(&ldo_cfg, &pwr);
    if (e != ESP_OK) return e;
    host.pwr_ctrl_handle = pwr;

    const sdmmc_slot_config_t slot = {
        .cd = SDMMC_SLOT_NO_CD, .wp = SDMMC_SLOT_NO_WP,
        .width = width, .flags = 0,
    };
    e = esp_vfs_fat_sdmmc_mount(BSP_SD_MOUNT_POINT, &host, &slot, &mcfg,
                                &bsp_sdcard);
    if (e != ESP_OK) sd_pwr_ctrl_del_on_chip_ldo(pwr); /* free LDO4 */
    return e;
}
#endif

void app_main(void)
{
    ESP_LOGI(TAG, "vehicular-mesh phase 0 — scenario-driven UI");

    {
        uint8_t mac[6] = {0};
        esp_efuse_mac_get_default(mac); /* factory chip MAC (P4 has no
                                         * wifi MAC of its own) */
        uint32_t id = 0x01000000u |
            ((uint32_t)mac[3] << 16) | ((uint32_t)mac[4] << 8) | mac[5];
        scenario_set_own_origin(id);
        ESP_LOGI(TAG, "node origin_id 0x%08X", (unsigned)id);
    }

#if VMESH_BENCH_POSE
    /* RF bench test without GPS: pin the pose so this node and a
     * co-located sim node share coordinates (else the relevance radius
     * drops each other's reports — their SIMULATED positions diverge on
     * the loop). Real GPS (HAT) makes this unnecessary. */
    {
        extern void vmesh_pose_set_live(const vmesh_pose_t *);
        vmesh_pose_t p = { .lat = 45.52, .lon = -122.89,
                           .heading_deg = 0, .speed_mps = 0 };
        vmesh_pose_set_live(&p);
        ESP_LOGI(TAG, "BENCH: pose pinned to 45.52,-122.89");
    }
#endif

    if (!scenario_load_buf(scenario_json_start,
                           (unsigned)(scenario_json_end - scenario_json_start))) {
        ESP_LOGE(TAG, "embedded scenario failed to parse");
        return;
    }

    /* Wi-Fi first: the C6-over-SDIO link must be brought up close to
     * esp_hosted's constructor-time reset of the C6 — 25 s later (after
     * the radio probe) its SDIO init times out (0x107). Also predates
     * the TF mount so the two SDMMC slots init in a known order. */
    bool sd_ok = false;
#if VMESH_TILES_SD
    /* Mount the TF card on a QUIET bus — BEFORE Wi-Fi brings up the C6
     * over the neighbouring SDIO slot. July 22: with the mount AFTER
     * wifi_mgr_start(), init failed at every clock/width (INVALID_RESPONSE
     * then TIMEOUT, varying boot-to-boot — the fingerprint of cross-slot
     * SDIO contention, not a fixed bug). Mounting first also powers LDO4
     * for the radio pads. */
    ESP_LOGI(TAG, "start: TF mount (pre-wifi)");
    {
        /* Full-speed BSP mount (4-bit, 40 MHz): with the HAT off the
         * card initializes normally — the slow-clock experiments were
         * only ever probing the HAT interference, which is gone. */
        esp_err_t mret = bsp_sdcard_mount();
        sd_ok = (mret == ESP_OK);
        snprintf(g_sd_diag, sizeof(g_sd_diag), "SD: mount 0x%x (%s)",
                 mret, esp_err_to_name(mret));
    }
    if (!sd_ok) {
        /* a failed sd_try_mount deletes its pwr handle, RELEASING LDO4 —
         * but the radio's CS/DIO1 pads (GPIO45/46) live in that domain.
         * Re-acquire it or the radio runs on unpowered pads. */
        esp_ldo_channel_config_t ldo = { .chan_id = 4, .voltage_mv = 3300 };
        static esp_ldo_channel_handle_t s_fail_ldo;
        esp_ldo_acquire_channel(&ldo, &s_fail_ldo);
        vTaskDelay(pdMS_TO_TICKS(10));
    }
#endif

    extern void wifi_mgr_start(void);
    ESP_LOGI(TAG, "start: wifi_mgr");
    wifi_mgr_start();

    /* DISPLAY BEFORE RADIO (July 23): radio bring-up blocks indefinitely
     * when the HAT is absent, which used to leave the screen black and
     * made the HAT-off SD ground-truth test unreadable. The UI depends
     * on nothing the radio provides — bring it up first, always. */

    /* Like bsp_display_start(), but with a bigger LVGL task stack: the
     * default 7 KB intermittently overflows (PNG inflate + rendering +
     * UI tick all run on it) -> panic -> the "sometimes it restarts". */
    bsp_display_cfg_t disp_cfg = {
        .lvgl_port_cfg = ESP_LVGL_PORT_INIT_CONFIG(),
        .buffer_size = BSP_LCD_DRAW_BUFF_SIZE,
        .double_buffer = BSP_LCD_DRAW_BUFF_DOUBLE,
        .flags = {
            .buff_dma = true,
            .buff_spiram = false,
            .sw_rotate = true, /* PPA does the work (Kconfig) */
        },
    };
    disp_cfg.lvgl_port_cfg.task_stack = 16384;
    bsp_display_start_with_config(&disp_cfg);
    bsp_display_backlight_on();

    bsp_display_lock(0);
    /* panel is 720x1280 portrait-native; the device mounts landscape
     * (270°, not 90°: USB connector ergonomics on the dev-kit stand) */
    lv_display_set_rotation(lv_display_get_default(), LV_DISPLAY_ROTATION_270);
    scenario_set_demo(waycast_load_demo()); /* real mode unless saved on */
    scenario_set_own_seq(waycast_load_seq() + 4); /* +4: NVS-commit slack */
    {
        char hd[16];
        waycast_load_handle(hd, sizeof(hd));
        vmesh_set_own_handle(hd);
    }
    {   /* open at the last-known position (stale) until GPS locks */
        double la, lo;
        if (waycast_load_loc(&la, &lo)) vmesh_pose_set_boot(la, lo);
    }
    ui_init();
    lv_timer_create(scenario_pump_cb, 50, NULL);
    /* DIAGNOSTIC: USB bus state on the top layer (console is unreadable
     * on this board). Remove once USB GPS is confirmed. */
    {
        lv_obj_t *ul = lv_label_create(lv_layer_top());
        lv_obj_set_style_text_color(ul, lv_color_hex(0xFAC775), 0);
        lv_obj_set_style_bg_color(ul, lv_color_hex(0x14161B), 0);
        lv_obj_set_style_bg_opa(ul, LV_OPA_80, 0);
        lv_obj_set_style_pad_all(ul, 4, 0);
        lv_obj_align(ul, LV_ALIGN_BOTTOM_LEFT, 4, -2); /* clear of the +/- zoom buttons */
        lv_timer_create(usb_diag_tick, 700, ul);
    }
    bsp_display_unlock();

    /* Radio AFTER the display: with no HAT, begin() blocks forever — but
     * the screen (and the SD diag line) are already alive by now.
     * Radio-vs-SD sequencing history: SX1262 begin()'s calibration burst
     * reliably wedges an already-initialized card (0x107 on every read);
     * an unmount/remount can't recover because the SDMMC host teardown
     * doesn't survive coexisting with the C6 SDIO slot. Current tactic:
     * mount early on a quiet bus, then re-init just the CARD in place on
     * the live host after the radio has done its damage. */
#if VMESH_HAT_RADIO
    extern bool radio_bonnet_start(void);
#if !VMESH_TILES_SD
    /* No TF mount in this build — but the radio's CS/DIO1 pads
     * (GPIO45/46) live in the SD pad domain (LDO4), so acquire it. */
    {
        esp_ldo_channel_config_t ldo = { .chan_id = 4, .voltage_mv = 3300 };
        static esp_ldo_channel_handle_t s_pad_ldo;
        esp_ldo_acquire_channel(&ldo, &s_pad_ldo);
        vTaskDelay(pdMS_TO_TICKS(10));
    }
#endif
    ESP_LOGI(TAG, "start: radio");
    radio_bonnet_start();
#else
    ESP_LOGI(TAG, "HAT radio disabled — LoRa via USB dongle when present");
#endif

#if VMESH_TILES_SD && VMESH_HAT_RADIO
    /* only the HAT radio's begin() burst wedges the card — without it
     * the in-place re-init is unnecessary */
    if (sd_ok && bsp_sdcard) {
        esp_err_t r = sdmmc_card_init(&bsp_sdcard->host, bsp_sdcard);
        int reads = -1;
        if (r == ESP_OK) {
            /* real data reads off the card, not just command responses */
            DIR *d = opendir(BSP_SD_MOUNT_POINT);
            if (d) { reads = 0; while (readdir(d) && reads < 3) reads++; closedir(d); }
        }
        size_t n = strlen(g_sd_diag); /* keep the mount result visible */
        snprintf(g_sd_diag + n, sizeof(g_sd_diag) - n,
                 " | reinit=%s(0x%x) root %s",
                 r == ESP_OK ? "OK" : "FAIL", r,
                 reads >= 0 ? "readable" : "UNREADABLE");
        if (r != ESP_OK || reads < 0) sd_ok = false; /* fall back to spiffs */
    }
#endif

    /* Map tiles ({root}/{z}/{x}/{y}.png): prefer the TF card (big
     * regions), else the demo tileset embedded in the flash "storage"
     * partition, else the grid fallback. Chosen only now that the card's
     * post-radio fate is known. */
    const char *tile_root = NULL;
    const char *writable_root = NULL; /* card only: download target */
    if (sd_ok) {
        tile_root = BSP_SD_MOUNT_POINT "/tiles";
        writable_root = tile_root;
        ESP_LOGI(TAG, "tiles: TF card");
    } else if (bsp_spiffs_mount() == ESP_OK) {
        tile_root = BSP_SPIFFS_MOUNT_POINT "/tiles";
        ESP_LOGI(TAG, "tiles: embedded demo tileset");
    } else {
        ESP_LOGW(TAG, "no tile source — map falls back to grid");
    }
    if (tile_root) {
        bsp_display_lock(0);
        map_view_set_tile_root(tile_root);
        bsp_display_unlock();
    }

    /* USB transports (GPS mouse + LoRa dongle fallback); the raw-SPI
     * HAT radio already started BEFORE the TF mount (see above).
     * All optional — the scenario feed keeps running without them. */
    extern void usb_serial_start(void);
    extern void gps_uart_start(void);
    extern void tile_fetch_init(const char *writable_root);
    /* USB host LAST: its bring-up stalls the USB-Serial-JTAG console
     * (under investigation) — everything after it would be skipped */
    /* GPIO37/38 double as the UART0 console pins, and the kit's
     * USB-TO-UART bridge (how we monitor!) hangs off them — only
     * claim them for the L76K when the GNSS HAT is actually there. */
    extern bool radio_hat_found;
#if VMESH_HAT_GPS
    if (radio_hat_found) {
        ESP_LOGI(TAG, "start: gps_uart (HAT present — console pins "
                      "handed to the L76K; logs continue on USB-JTAG)");
        gps_uart_start();
    } else {
        ESP_LOGI(TAG, "start: gps_uart skipped (no GNSS HAT)");
    }
#else
    (void)gps_uart_start;
    ESP_LOGI(TAG, "start: gps_uart DISABLED (radio bring-up: keep "
                  "console on the UART bridge)");
#endif
    ESP_LOGI(TAG, "start: tile_fetch");
    tile_fetch_init(writable_root); /* offline-map download to card */
    ESP_LOGI(TAG, "start: usb_serial");
    usb_serial_start();

    /* OTA (docs/OTA.md): register the update ops, then the ROLLBACK
     * GATE — reaching this line means display, UI, SD/tiles, Wi-Fi and
     * USB all came up, which is the self-check. If this boot is a
     * freshly-OTA'd image (pending-verify), mark it good; had it
     * crashed before this point, the bootloader would auto-revert to
     * the previous slot on the next reset. */
    {
        extern void fw_update_init(void);
        fw_update_init();
        const esp_partition_t *run = esp_ota_get_running_partition();
        esp_ota_img_states_t ost;
        if (esp_ota_get_state_partition(run, &ost) == ESP_OK &&
            ost == ESP_OTA_IMG_PENDING_VERIFY) {
            esp_ota_mark_app_valid_cancel_rollback();
            ESP_LOGI(TAG, "OTA image passed self-check — marked valid");
        }
    }
    ESP_LOGI(TAG, "start: done");

    ESP_LOGI(TAG, "UI up, scenario \"%s\" playing", scenario_name());
}
