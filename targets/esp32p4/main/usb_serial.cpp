/* usb_serial.cpp — USB-host transports for the solderless prototype
 * (HARDWARE_SHORTLIST.md §0): u-blox USB GPS mouse (CDC-ACM, NMEA) and
 * Waveshare USB-TO-LoRa dongle (CH34x VCP, vmesh frames over the DTU
 * byte pipe). Devices are optional and hot-pluggable; without them the
 * scenario feed keeps running.
 *
 * COMPILE-VERIFIED ONLY until the dongles arrive — enumeration IDs,
 * the DTU AT dialogue, and dongle framing are flagged in lora_dtu.h.
 */

#include "esp_log.h"
#include "esp_timer.h"
#include <string.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "usb/cdc_acm_host.h"
#include "usb/usb_host.h"
#include "usb/vcp.hpp"
#include "usb/vcp_ch34x.hpp"
#include "usb/vcp_cp210x.hpp"
#include "usb/vcp_ftdi.hpp"

extern "C" {
#include "feed.h"
#include "lora_dtu.h"

#include "soc/usb_dwc_struct.h"   /* USB_DWC_HS — FS-only root-port hack */
#include "hal/usb_dwc_ll.h"
#include "nmea.h"
#include "vmesh_wire.h"
}

extern "C" void waycast_save_loc(double lat, double lon); /* main.c (NVS) */

static const char *TAG = "vmesh-usb";

/* on-screen diagnostic: the console is unreadable on this board, so the
 * USB bus truth goes to the display. probe_task/open_task write here;
 * main renders it on the top layer. */
static char s_status[96] = "USB: starting...";
static volatile uint32_t s_gps_bytes, s_gps_rmc;
static volatile uint32_t s_lora_tx_n, s_lora_rx_n, s_lora_txerr_n;
static volatile uint32_t s_lora_raw_n; /* serial bytes from dongle, any */
static volatile esp_err_t s_lora_txerr_last;
extern "C" bool usb_lora_open(void); /* below: either VCP or CDC handle */
extern "C" char usb_lora_path(void); /* below: 'V' vcp / 'C' cdc / '-' */
extern "C" const char *usb_status_str(void)
{
    extern cdc_acm_dev_hdl_t usb_gps_handle(void);
    static char g[192];
    /* always show BOTH transports — GPS masking the dongle state cost a
     * bench session (July 23) */
    char lora[48];
    if (usb_lora_open()) {
        char path = usb_lora_path(); /* which driver claimed it */
        if (s_lora_txerr_n)
            snprintf(lora, sizeof(lora), "LoRa(%c): tx%u rx%u raw%u ERR%u(0x%x)",
                     path, (unsigned)s_lora_tx_n, (unsigned)s_lora_rx_n,
                     (unsigned)s_lora_raw_n,
                     (unsigned)s_lora_txerr_n, s_lora_txerr_last);
        else
            snprintf(lora, sizeof(lora), "LoRa(%c): open tx%u rx%u raw%u",
                     path, (unsigned)s_lora_tx_n, (unsigned)s_lora_rx_n,
                     (unsigned)s_lora_raw_n);
    } else
        snprintf(lora, sizeof(lora), "LoRa: none");
    if (usb_gps_handle()) {
        snprintf(g, sizeof(g), "USB GPS: %u bytes, %u RMC -- %s | %s",
                 (unsigned)s_gps_bytes, (unsigned)s_gps_rmc,
                 s_gps_rmc ? "parsing OK" :
                 s_gps_bytes ? "bytes flow, no valid RMC"
                             : "OPEN but NO bytes (wrong data iface?)",
                 lora);
        return g;
    }
    snprintf(g, sizeof(g), "%s | %s", s_status, lora);
    return g;
}

/* u-blox USB GPS receivers (VK-162 class) */
#define UBLOX_VID 0x1546
/* Waveshare USB-TO-LoRa dongle: WCH CH343 — CDC-ACM compliant, so the
 * plain CDC host drives it (the ch34x VCP driver only knows 340/341).
 * Verified against the real dongle July 18 2026: VID 0x1A86 PID 0x55D3. */
#define WCH_VID   0x1A86
#define CH343_PID 0x55D3

/* radio profile for the prototype (SF9/125k: 29B hazard fits dwell) */
#define LORA_SF 9
#define LORA_BW_IDX 0 /* 125 kHz */
#define LORA_CHANNEL 65 /* DTU CH65 = 850+65 = 915.0 MHz (US ISM); dongles AT-set to this */
#define LORA_PWR_DBM 22

static nmea_parser_t s_nmea;
static lora_dtu_rx_t s_dtu_rx;
static CdcAcmDevice *s_lora;      /* VCP handle (global-ns class) */
static cdc_acm_dev_hdl_t s_lora_cdc; /* CH343: plain CDC handle */
static cdc_acm_dev_hdl_t s_gps;
extern "C" cdc_acm_dev_hdl_t usb_gps_handle(void) { return s_gps; }

/* ---- RX paths (run in USB host context) ---- */

static bool gps_rx(const uint8_t *data, size_t len, void *arg)
{
    (void)arg;
    s_gps_bytes += len;
    nmea_fix_t fix;
    for (size_t i = 0; i < len; i++) {
        bool rmc = nmea_feed_char(&s_nmea, (char)data[i], &fix);
        if (rmc) s_gps_rmc++;
        if (rmc && !fix.valid) {
            extern void vmesh_gps_state_set(int);
            vmesh_gps_state_set(1); /* USB GPS talking, hunting for sky */
        }
        if (rmc && fix.valid) {
            vmesh_pose_t pose = {
                .lat = fix.lat,
                .lon = fix.lon,
                .heading_deg = fix.course_deg,
                .speed_mps = fix.speed_mps,
            };
            vmesh_pose_set_live(&pose);
            if (fix.unix_s) vmesh_time_set_live(fix.unix_s); /* real clock */
            /* persist last-known position, throttled (NVS wear) */
            static int64_t last_save;
            int64_t nowt = esp_timer_get_time();
            if (nowt - last_save > 60000000LL) { /* 60 s */
                last_save = nowt;
                waycast_save_loc(fix.lat, fix.lon);
            }
        }
    }
    return true;
}

extern "C" bool usb_lora_open(void)
{
    return s_lora != nullptr || s_lora_cdc != nullptr;
}

extern "C" char usb_lora_path(void)
{
    return s_lora ? 'V' : s_lora_cdc ? 'C' : '-';
}

static bool lora_rx(const uint8_t *data, size_t len, void *arg)
{
    (void)arg;
    s_lora_raw_n += (uint32_t)len; /* ALL serial bytes, valid or garbage */
    uint8_t payload[LORA_DTU_MAX_PAYLOAD];
    for (size_t i = 0; i < len; i++) {
        int n = lora_dtu_rx_feed(&s_dtu_rx, data[i], payload,
                                 sizeof(payload));
        if (n > 0) {
            vmesh_msg_t m;
            if (vmesh_wire_decode(payload, (size_t)n, &m) == 0) {
                s_lora_rx_n++;
                m.hops++;
                vmesh_feed_inject(&m);
            }
        }
    }
    return true;
}

/* ---- TX path (feed publish hook, runs in UI task) ---- */

static void lora_tx(const vmesh_msg_t *m)
{
    if (!s_lora && !s_lora_cdc) return;
    uint8_t wire[VMESH_WIRE_MAX];
    int wn = vmesh_wire_encode(m, wire, sizeof(wire));
    if (wn <= 0) return;
    uint8_t framed[LORA_DTU_MAX_PAYLOAD + LORA_DTU_OVERHEAD];
    int fn = lora_dtu_frame(wire, (size_t)wn, framed, sizeof(framed));
    if (fn <= 0) return;
    esp_err_t te;
    if (s_lora)
        te = s_lora->tx_blocking(framed, (size_t)fn, 500);
    else
        te = cdc_acm_host_data_tx_blocking(s_lora_cdc, framed, (size_t)fn, 500);
    if (te == ESP_OK) s_lora_tx_n++; else s_lora_txerr_n++;
    s_lora_txerr_last = te;
}

/* ---- device bring-up ---- */

static void usb_lib_task(void *arg)
{
    (void)arg;
    while (true) {
        uint32_t flags;
        usb_host_lib_handle_events(portMAX_DELAY, &flags);
        if (flags & USB_HOST_LIB_EVENT_FLAGS_NO_CLIENTS)
            usb_host_device_free_all();
    }
}

static void open_task(void *arg)
{
    (void)arg;
    const cdc_acm_host_device_config_t gps_cfg = {
        .connection_timeout_ms = 5000,
        .out_buffer_size = 64,
        .in_buffer_size = 512,
        .event_cb = nullptr,
        .data_cb = gps_rx,
        .user_arg = nullptr,
    };
    const cdc_acm_host_device_config_t lora_cfg = {
        .connection_timeout_ms = 5000,
        .out_buffer_size = 256,
        .in_buffer_size = 512,
        .event_cb = nullptr,
        .data_cb = lora_rx,
        .user_arg = nullptr,
    };

    while (true) {
        /* GPS: any u-blox CDC device */
        if (!s_gps) {
            esp_err_t err = cdc_acm_host_open(
                UBLOX_VID, CDC_HOST_ANY_PID, 0, &gps_cfg, &s_gps);
            if (err == ESP_OK) {
                cdc_acm_line_coding_t lc = {
                    .dwDTERate = 9600, .bCharFormat = 0,
                    .bParityType = 0, .bDataBits = 8,
                };
                cdc_acm_host_line_coding_set(s_gps, &lc);
                snprintf(s_status, sizeof(s_status),
                         "USB GPS open OK (u-blox) — waiting for NMEA");
                ESP_LOGI(TAG, "USB GPS attached (u-blox)");
            }
        }

        /* LoRa dongle: VCP chips first, then the CH343 as plain CDC */
        if (!s_lora && !s_lora_cdc) {
            bool up = false;
            s_lora = esp_usb::VCP::open(&lora_cfg);
            if (s_lora) {
                cdc_acm_line_coding_t lc = {
                    .dwDTERate = 115200, .bCharFormat = 0,
                    .bParityType = 0, .bDataBits = 8,
                };
                s_lora->line_coding_set(&lc);
                up = true;
            } else if (cdc_acm_host_open(WCH_VID, CH343_PID, 0,
                                         &lora_cfg,
                                         &s_lora_cdc) == ESP_OK) {
                cdc_acm_line_coding_t lc = {
                    .dwDTERate = 115200, .bCharFormat = 0,
                    .bParityType = 0, .bDataBits = 8,
                };
                cdc_acm_host_line_coding_set(s_lora_cdc, &lc);
                up = true;
            }
            if (up) {
                char cmds[256];
                int n = lora_dtu_init_cmds(cmds, sizeof(cmds), LORA_SF,
                                           LORA_BW_IDX, LORA_CHANNEL,
                                           LORA_PWR_DBM);
                if (n > 0) {
                    if (s_lora)
                        s_lora->tx_blocking((uint8_t *)cmds, (size_t)n,
                                            1000);
                    else
                        cdc_acm_host_data_tx_blocking(
                            s_lora_cdc, (uint8_t *)cmds, (size_t)n, 1000);
                }
                /* the raw-SPI bonnet outranks the DTU dongle as TX path */
                extern bool radio_bonnet_active;
                if (!radio_bonnet_active) vmesh_feed_set_tx_hook(lora_tx);
                ESP_LOGI(TAG, "USB LoRa dongle attached (%s), profile "
                              "SF%d/BW%d/CH%d",
                         s_lora ? "VCP" : "CH343 CDC",
                         LORA_SF, LORA_BW_IDX, LORA_CHANNEL);
            }
        }

        vTaskDelay(pdMS_TO_TICKS(3000)); /* poll for late plug-ins */
    }
}

/* diagnostic: announce EVERY device that enumerates (VID/PID) — this
 * is the first proof the CH334F hub + host stack see anything at all */
static void new_dev_cb(usb_device_handle_t dev)
{
    const usb_device_desc_t *desc = nullptr;
    if (usb_host_get_device_descriptor(dev, &desc) == ESP_OK && desc)
        ESP_LOGI(TAG, "USB device enumerated: VID 0x%04X PID 0x%04X",
                 desc->idVendor, desc->idProduct);
}

/* raw bus truth-teller: lists EVERY enumerated device (any class),
 * so we learn whether the hub/port sees anything at all — independent
 * of the CDC driver. Logs each device's VID/PID/class once. */
static void probe_task(void *arg)
{
    (void)arg;
    uint8_t seen[8] = {0};
    int last_n = -1;
    while (true) {
        uint8_t addrs[8] = {0};
        int n = 0;
        usb_host_device_addr_list_fill(8, addrs, &n);
        if (n != last_n) {
            ESP_LOGW(TAG, "PROBE: %d USB device(s) on the bus", n);
            snprintf(s_status, sizeof(s_status), "USB bus: %d device(s)", n);
            last_n = n;
        }
        for (int i = 0; i < n; i++) {
            bool known = false;
            for (int k = 0; k < 8; k++) if (seen[k] == addrs[i]) known = true;
            if (known) continue;
            usb_host_client_handle_t cl = nullptr;
            usb_host_client_config_t cc = {};
            cc.is_synchronous = false;
            cc.max_num_event_msg = 3;
            if (usb_host_client_register(&cc, &cl) != ESP_OK) continue;
            usb_device_handle_t dev;
            if (usb_host_device_open(cl, addrs[i], &dev) == ESP_OK) {
                const usb_device_desc_t *d = nullptr;
                if (usb_host_get_device_descriptor(dev, &d) == ESP_OK && d) {
                    usb_device_info_t di = {};
                    char spd = '?'; /* L/F/H = negotiated speed */
                    if (usb_host_device_info(dev, &di) == ESP_OK)
                        spd = di.speed == USB_SPEED_LOW    ? 'L'
                            : di.speed == USB_SPEED_FULL   ? 'F'
                            : di.speed == USB_SPEED_HIGH   ? 'H' : '?';
                    ESP_LOGW(TAG, "PROBE: addr %d -> VID 0x%04X PID 0x%04X "
                                  "class 0x%02X speed %c",
                             addrs[i], d->idVendor, d->idProduct,
                             d->bDeviceClass, spd);
                    size_t l = strlen(s_status);
                    snprintf(s_status + l, sizeof(s_status) - l,
                             "  [%04X:%04X cls%02X %c]",
                             d->idVendor, d->idProduct, d->bDeviceClass, spd);
                }
                usb_host_device_close(cl, dev);
                for (int k = 0; k < 8; k++)
                    if (!seen[k]) { seen[k] = addrs[i]; break; }
            }
            usb_host_client_deregister(cl);
        }
        vTaskDelay(pdMS_TO_TICKS(2000));
    }
}

extern "C" void usb_serial_start(void)
{
    usb_host_config_t host_cfg = {};
    host_cfg.skip_phy_setup = false;
    host_cfg.intr_flags = ESP_INTR_FLAG_LEVEL1;
    /* FS-ONLY ROOT PORT (July 23): the board's CH334F hub links High-
     * Speed, and IDF's DWC host has no split-transaction support — so
     * Full-Speed devices (GPS mouse, CH343 dongle) behind the HS hub
     * can never enumerate. Fix: install with the root port unpowered,
     * set HCFG.FSLSSupp (disables the HS chirp entirely — the hub then
     * links at FS, and FS-behind-FS needs no splits), then power the
     * port. IDF never exposes this on P4 ("UTMI can be HighSpeed only"
     * is their SOFTWARE limitation, the ll header has the setter). */
    host_cfg.root_port_unpowered = true;
    ESP_ERROR_CHECK(usb_host_install(&host_cfg));
    usb_dwc_ll_hcfg_set_fsls_supp_only(&USB_DWC_HS);
    ESP_ERROR_CHECK(usb_host_lib_set_root_port_power(true));
    cdc_acm_host_driver_config_t cdc_cfg = {
        .driver_task_stack_size = 4096,
        .driver_task_priority = 11,
        .xCoreID = 0,
        .new_dev_cb = new_dev_cb,
    };
    ESP_ERROR_CHECK(cdc_acm_host_install(&cdc_cfg));
    esp_usb::VCP::register_driver<esp_usb::CH34x>();
    esp_usb::VCP::register_driver<esp_usb::CP210x>();
    esp_usb::VCP::register_driver<esp_usb::FT23x>();

    xTaskCreate(usb_lib_task, "usb_events", 4096, nullptr, 10, nullptr);
    xTaskCreate(open_task, "usb_open", 6144, nullptr, 5, nullptr);
    xTaskCreate(probe_task, "usb_probe", 4096, nullptr, 4, nullptr);
    ESP_LOGI(TAG, "USB host up — waiting for GPS / LoRa dongles");
}
