/* radio_bonnet.cpp — raw-SPI LoRa transport: Adafruit LoRa Radio
 * Bonnet (RFM95W / SX1276-class) stacked on the 40-pin header, driven
 * by RadioLib through a minimal ESP-IDF HAL.
 *
 * vmesh_wire frames ARE the LoRa payload (LoRa has real packet
 * boundaries — no extra framing needed, unlike the USB DTU pipe).
 * RX -> vmesh_feed_inject; TX <- feed publish hook via a queue so the
 * UI task never blocks on airtime.
 *
 * Refuses to start until bonnet_pins.h is filled in (bench day).
 */

#include "driver/gpio.h"
#include "driver/i2c_master.h"
#include "driver/spi_master.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/queue.h"
#include "freertos/task.h"
#include "rom/ets_sys.h"

#include <RadioLib.h>

#include "bonnet_pins.h"
#include "hat_pins.h"

extern "C" {
#include "feed.h"
#include "vmesh_mesh.h"
#include "vmesh_wire.h"
#include "lora_dtu.h"
}

/* NOTE: this must be defined HERE — a #define in main.c is a different
 * translation unit and never reaches this file. Default OFF: the scan
 * drives header GPIOs at boot and starves the SD controller (0x107
 * tile-read timeouts). Set to 1 only for a one-shot HAT pin discovery. */
#ifndef VMESH_HAT_AUTOSCAN
#define VMESH_HAT_AUTOSCAN 0
#endif

/* diagnostic: 1 = radio up but TX frames dropped (SD-interference isolation) */
#ifndef VMESH_HAT_INIT_STAGE1
#define VMESH_HAT_INIT_STAGE1 0
#endif

#ifndef VMESH_HAT_INIT_STAGE2
#define VMESH_HAT_INIT_STAGE2 0
#endif

#ifndef VMESH_HAT_DIAG
#define VMESH_HAT_DIAG 0
#endif

#ifndef VMESH_HAT_TXBEACON
#define VMESH_HAT_TXBEACON 0
#endif

#ifndef VMESH_HAT_RFSCAN
#define VMESH_HAT_RFSCAN 0
#endif

#ifndef VMESH_HAT_TX_MUTE
#define VMESH_HAT_TX_MUTE 0
#endif

static const char *TAG = "vmesh-bonnet";

/* Live seating-tuning probe: bit-bang an SX1262 GetStatus + ReadRegister
 * (0x0740/0x0741, POR defaults 0x14/0x24) a few times a second on the
 * vendor-verified pins, and log each read. Runs as its own task so the
 * UI/tiles keep going; the user wiggles the HAT angle and watches for
 * the line to change from "FF FF" (no contact) to "14 24 <<LOCKED". */
#ifndef VMESH_HAT_LIVEPROBE
#define VMESH_HAT_LIVEPROBE 0
#endif
#if VMESH_HAT_LIVEPROBE
/* MISO-hunt live probe: drive SCK/MOSI/CS/RST on the vendor-verified
 * pins, clock an SX1262 ReadRegister(0x0740) — and sample EVERY other
 * header pin as a candidate MISO on each bit. If the chip is alive but
 * MISO is on a GPIO we mismapped, one candidate returns 0x14; if NO
 * candidate ever returns real data, the chip isn't responding at all
 * (power/clock not reaching it) regardless of which pin is MISO. */
static const int MISO_CAND[] = {
    36, 21, 22, 33, 26, 2, 48, 46, 27, 45, 53, 47, 6, 3, 5, 4, 23
};
#define MC_N ((int)(sizeof(MISO_CAND)/sizeof(int)))
static void hat_liveprobe_task(void *)
{
    const int SCK = HAT_PIN_SCLK, MOSI = HAT_PIN_MOSI,
              CS = HAT_PIN_CS, RST = HAT_PIN_RST;
    gpio_reset_pin((gpio_num_t)SCK);  gpio_reset_pin((gpio_num_t)MOSI);
    gpio_reset_pin((gpio_num_t)CS);   gpio_reset_pin((gpio_num_t)RST);
    gpio_set_direction((gpio_num_t)SCK, GPIO_MODE_OUTPUT);
    gpio_set_direction((gpio_num_t)MOSI, GPIO_MODE_OUTPUT);
    gpio_set_direction((gpio_num_t)CS, GPIO_MODE_OUTPUT);
    gpio_set_direction((gpio_num_t)RST, GPIO_MODE_OUTPUT);
    for (int i = 0; i < MC_N; i++) {
        gpio_reset_pin((gpio_num_t)MISO_CAND[i]);
        gpio_set_direction((gpio_num_t)MISO_CAND[i], GPIO_MODE_INPUT);
    }
    gpio_set_level((gpio_num_t)SCK, 0);
    gpio_set_level((gpio_num_t)CS, 1);
    gpio_set_level((gpio_num_t)RST, 1);
    /* send a byte on MOSI while accumulating a byte per candidate MISO */
    auto xfer = [&](uint8_t out, uint8_t *acc /*[MC_N] or NULL*/) {
        for (int b = 7; b >= 0; b--) {
            gpio_set_level((gpio_num_t)MOSI, (out >> b) & 1);
            ets_delay_us(4);
            gpio_set_level((gpio_num_t)SCK, 1);
            ets_delay_us(4);
            if (acc)
                for (int i = 0; i < MC_N; i++)
                    acc[i] = (uint8_t)((acc[i] << 1) |
                             gpio_get_level((gpio_num_t)MISO_CAND[i]));
            gpio_set_level((gpio_num_t)SCK, 0);
        }
    };
    ESP_LOGW(TAG, "MISO-HUNT probe — clocking ReadReg on all %d candidates", MC_N);
    for (unsigned n = 0; ; n++) {
        gpio_set_level((gpio_num_t)RST, 0); ets_delay_us(1000);
        gpio_set_level((gpio_num_t)RST, 1); vTaskDelay(pdMS_TO_TICKS(5));
        uint8_t acc[MC_N]; for (int i = 0; i < MC_N; i++) acc[i] = 0;
        gpio_set_level((gpio_num_t)CS, 0); ets_delay_us(2);
        xfer(0x1D, NULL); xfer(0x07, NULL); xfer(0x40, NULL);
        xfer(0x00, NULL); xfer(0x00, acc);   /* reg 0x0740 -> acc[] */
        gpio_set_level((gpio_num_t)CS, 1);
        /* report only pins that returned real data (not FF/00) */
        char line[256]; int p = 0; int interesting = 0;
        for (int i = 0; i < MC_N; i++) {
            if (acc[i] != 0xFF && acc[i] != 0x00) {
                p += snprintf(line + p, sizeof(line) - p, " G%d=%02X%s",
                              MISO_CAND[i], acc[i],
                              acc[i] == 0x14 ? "<<LOCK" : "");
                interesting++;
            }
        }
        if (interesting)
            ESP_LOGW(TAG, "[%3u] DATA:%s", n, line);
        else
            ESP_LOGW(TAG, "[%3u] all candidates FF/00 (no chip response)", n);
        vTaskDelay(pdMS_TO_TICKS(600));
    }
}
#endif

/* GPS-TXD hunt: the L76K emits NMEA continuously at 9600 baud (its LEDs
 * confirm it's powered and running). That's the ONE header line we KNOW
 * carries a live signal — so count logic transitions on every header pin
 * over a window. The GPS TXD stands out (hundreds of edges, idle-high);
 * every static/disconnected pin shows ~0. This (a) proves whether ANY
 * data pin mates HAT<->P4 at all, and (b) reveals which GPIO the signal
 * actually lands on — calibrating the whole phys->GPIO map from a known
 * source instead of guessing. */
#ifndef VMESH_HAT_GPSHUNT
#define VMESH_HAT_GPSHUNT 0
#endif
#if VMESH_HAT_GPSHUNT
static void hat_gpshunt_task(void *)
{
    /* Broad sweep of every header GPIO that's safe to reset+read.
     * Excludes only the system-critical pins (7/8 I2C, 14-19 C6 SDIO,
     * 24/25 USB, 37/38 console, 39-44 SD card, 54 C6 reset) — driving
     * those would kill touch/wifi/console/tiles. The GPS TXD (BCM14 or
     * BCM15) and every LoRa signal live outside that set. */
    static const int C[] = {
        0,1,2,3,4,5,6,9,10,11,12,13,20,21,22,23,26,27,28,29,30,31,
        32,33,34,35,36,45,46,47,48,49,50,51,52,53
    };
    const int N = (int)(sizeof(C)/sizeof(int));
    for (int i = 0; i < N; i++) {
        gpio_reset_pin((gpio_num_t)C[i]);
        gpio_set_direction((gpio_num_t)C[i], GPIO_MODE_INPUT);
    }
    ESP_LOGW(TAG, "GPS-TXD hunt: scanning every header pin for the live "
                  "9600-baud NMEA line (expect ~hundreds of edges)");
    for (unsigned n = 0; ; n++) {
        int last[64], edges[64]; long ones[64]; long samples = 0;
        for (int i = 0; i < N; i++) {
            last[i] = gpio_get_level((gpio_num_t)C[i]); edges[i] = 0; ones[i] = 0;
        }
        int64_t t0 = esp_timer_get_time();
        /* 2 s CONTINUOUS window: the L76K bursts NMEA ~1x/s, so this
         * always spans several bursts — a real signal can't hide in a
         * gap. yield briefly so the watchdog/UI still breathe. */
        while (esp_timer_get_time() - t0 < 2000000) {
            for (int j = 0; j < 2000; j++) {
                for (int i = 0; i < N; i++) {
                    int v = gpio_get_level((gpio_num_t)C[i]);
                    if (v != last[i]) { edges[i]++; last[i] = v; }
                    ones[i] += v;
                }
                samples++;
            }
            vTaskDelay(1);
        }
        /* always report the 3 busiest pins so we see the noise floor */
        int order[64]; for (int i = 0; i < N; i++) order[i] = i;
        for (int a = 0; a < N; a++)
            for (int b = a + 1; b < N; b++)
                if (edges[order[b]] > edges[order[a]]) {
                    int t = order[a]; order[a] = order[b]; order[b] = t;
                }
        char line[200]; int p = 0;
        for (int k = 0; k < 3 && k < N; k++) {
            int i = order[k];
            p += snprintf(line + p, sizeof(line) - p, " G%d=%dedg/%ld%%hi",
                          C[i], edges[i], samples ? ones[i]*100/samples : 0);
        }
        bool live = edges[order[0]] > 200;
        ESP_LOGW(TAG, "[%3u] top:%s  %s", n, line,
                 live ? "<<< LIVE NMEA LINE FOUND" : "(all flat — no GPS data)");
    }
}
#endif

/* SPI pin discovery: contact is confirmed (GPS found), but our P4
 * phys->GPIO map was wrong. Find the real SCK/MOSI/MISO/CS empirically:
 * classify every safe pin by pull signature (MISO/CS/RST have pull-ups
 * = driven-high; DIO1/BUSY idle low; SCK/MOSI float), then brute-force
 * CS x SCK x MOSI over the plausible sets, clocking an SX1262
 * ReadRegister(0x0740) and watching every driven-high pin as MISO. The
 * combo that returns 0x14 (POR sync-word MSB) is the real pinout. */
#ifndef VMESH_HAT_SPISCAN
#define VMESH_HAT_SPISCAN 0
#endif
#if VMESH_HAT_SPISCAN
static const int SAFE_G[] = {
    0,1,2,3,4,5,6,9,10,11,12,13,20,21,22,23,26,27,28,29,30,31,
    32,33,34,35,36,45,46,47,48,49,50,51,52,53
};
#define SG_N ((int)(sizeof(SAFE_G)/sizeof(int)))
static int rp(int g, int pull) {
    gpio_config_t c = {}; c.pin_bit_mask = 1ULL << g; c.mode = GPIO_MODE_INPUT;
    c.pull_up_en = pull ? GPIO_PULLUP_ENABLE : GPIO_PULLUP_DISABLE;
    c.pull_down_en = pull ? GPIO_PULLDOWN_DISABLE : GPIO_PULLDOWN_ENABLE;
    gpio_config(&c); ets_delay_us(60); return gpio_get_level((gpio_num_t)g);
}
static void hat_spiscan_task(void *)
{
    for (int i = 0; i < SG_N; i++) gpio_reset_pin((gpio_num_t)SAFE_G[i]);
    /* Candidates for CS/SCK/MOSI/MISO = everything EXCEPT driven-low
     * pins (those are the DIO1/BUSY chip outputs). MISO reads floating
     * when deselected (high-Z), so it lives in this set too — the prior
     * bug was only checking pull-up pins as MISO. */
    int cand[40], nC = 0, dh[40], nDH = 0;
    for (int i = 0; i < SG_N; i++) {
        int g = SAFE_G[i], up = rp(g, 1), dn = rp(g, 0);
        if (!(up == 0 && dn == 0)) cand[nC++] = g;   /* skip driven-low */
        if (up == 1 && dn == 1) dh[nDH++] = g;       /* pull-up: RST lives here */
    }
    ESP_LOGW(TAG, "SPI-SCAN: %d candidates, %d pull-up (reset via all of "
                  "them since RST is one)", nC, nDH);
    /* NRESET has a pull-up -> it's in dh[]. Pulse all pull-up pins low
     * then high to hardware-reset the chip without knowing which is RST
     * (driving CS/others low briefly is harmless). */
    auto resetChip = [&]() {
        for (int i = 0; i < nDH; i++) {
            gpio_set_direction((gpio_num_t)dh[i], GPIO_MODE_OUTPUT);
            gpio_set_level((gpio_num_t)dh[i], 0);
        }
        vTaskDelay(pdMS_TO_TICKS(2));
        for (int i = 0; i < nDH; i++) gpio_set_level((gpio_num_t)dh[i], 1);
        vTaskDelay(pdMS_TO_TICKS(10));
        for (int i = 0; i < nDH; i++)
            gpio_set_direction((gpio_num_t)dh[i], GPIO_MODE_INPUT);
    };
    auto clockReg = [&](int SCK, int MOSI, int CS, uint8_t *acc, uint8_t *acc2) {
        gpio_set_level((gpio_num_t)CS, 0); ets_delay_us(2);
        const uint8_t seq[6] = {0x1D,0x07,0x40,0x00,0x00,0x00};
        for (int k = 0; k < 6; k++) {
            for (int b = 7; b >= 0; b--) {
                gpio_set_level((gpio_num_t)MOSI, (seq[k] >> b) & 1);
                ets_delay_us(3); gpio_set_level((gpio_num_t)SCK, 1); ets_delay_us(3);
                uint8_t *dst = (k == 4) ? acc : (k == 5) ? acc2 : nullptr;
                if (dst) for (int i = 0; i < nC; i++)
                    dst[i] = (uint8_t)((dst[i] << 1) |
                             gpio_get_level((gpio_num_t)cand[i]));
                gpio_set_level((gpio_num_t)SCK, 0);
            }
        }
        gpio_set_level((gpio_num_t)CS, 1);
    };
    for (unsigned pass = 0; ; pass++) {
        int matches = 0, tried = 0;
        resetChip();  /* clean state before each sweep */
        for (int ci = 0; ci < nC; ci++) {
            int CS = cand[ci];
            for (int si = 0; si < nC; si++) {
                if (si == ci) continue;
                int SCK = cand[si];
                for (int mi = 0; mi < nC; mi++) {
                    if (mi == ci || mi == si) continue;
                    int MOSI = cand[mi];
                    /* all candidates -> input; drive SCK/MOSI/CS */
                    for (int i = 0; i < nC; i++)
                        gpio_set_direction((gpio_num_t)cand[i], GPIO_MODE_INPUT);
                    gpio_set_direction((gpio_num_t)SCK, GPIO_MODE_OUTPUT);
                    gpio_set_direction((gpio_num_t)MOSI, GPIO_MODE_OUTPUT);
                    gpio_set_direction((gpio_num_t)CS, GPIO_MODE_OUTPUT);
                    gpio_set_level((gpio_num_t)SCK, 0);
                    gpio_set_level((gpio_num_t)CS, 1);
                    uint8_t acc[40], acc2[40];
                    for (int i = 0; i < nC; i++) { acc[i] = 0; acc2[i] = 0; }
                    clockReg(SCK, MOSI, CS, acc, acc2);
                    for (int i = 0; i < nC; i++)
                        if (i != ci && i != si && i != mi &&
                            acc[i] == 0x14 && acc2[i] == 0x24) {
                            ESP_LOGW(TAG, "*** MATCH! SCK=G%d MOSI=G%d "
                                     "CS=G%d MISO=G%d  regs=0x14,0x24 ***",
                                     SCK, MOSI, CS, cand[i]);
                            matches++;
                        }
                    if ((++tried & 0x3FF) == 0) vTaskDelay(1);
                }
            }
        }
        ESP_LOGW(TAG, "SPI-SCAN pass %u: %d combos, %d matches",
                 pass, tried, matches);
        vTaskDelay(pdMS_TO_TICKS(2000));
    }
}
#endif

bool radio_bonnet_active = false; /* checked by usb_serial.cpp */
bool radio_hat_found = false;     /* SX1262 GNSS HAT: gates gps_uart */

/* ---------------- minimal ESP-IDF RadioLibHal ---------------- */

class P4Hal : public RadioLibHal {
public:
    P4Hal()
        : RadioLibHal(GPIO_MODE_INPUT, GPIO_MODE_OUTPUT, 0, 1,
                      GPIO_INTR_POSEDGE, GPIO_INTR_NEGEDGE) {}

    void init() override
    {
        /* called once per RadioLib Module — the HAT probe and the
         * bonnet fallback share this HAL, so guard the one-time
         * bus/ISR setup (second spi_bus_initialize aborts the boot) */
        static bool inited;
        if (inited) return;
        inited = true;
        gpio_install_isr_service(0);

        /* Detach default peripheral routing from the HAT pins first —
         * GPIO0/1/32 come up muxed to JTAG/strap functions, so
         * spi_bus_initialize's matrix routing never reaches the pads
         * (verified: they wouldn't drive until gpio_reset_pin). */
        static const int hp[] = { HAT_PIN_SCLK, HAT_PIN_MOSI,
            HAT_PIN_MISO, HAT_PIN_CS, HAT_PIN_RST, HAT_PIN_BUSY,
            HAT_PIN_DIO1 };
        for (unsigned i = 0; i < sizeof(hp)/sizeof(hp[0]); i++)
            if (hp[i] >= 0) gpio_reset_pin((gpio_num_t)hp[i]);

        spi_bus_config_t bus = {};
        /* HAT pins (primary path). The bonnet fallback's matrix probe
         * bit-bangs its own pins separately, so this is fine. */
        bus.sclk_io_num = HAT_PIN_SCLK;
        bus.mosi_io_num = HAT_PIN_MOSI;
        bus.miso_io_num = HAT_PIN_MISO;
        bus.quadwp_io_num = -1;
        bus.quadhd_io_num = -1;
        bus.max_transfer_sz = 256;
        ESP_ERROR_CHECK(spi_bus_initialize(SPI2_HOST, &bus,
                                           SPI_DMA_DISABLED));

        spi_device_interface_config_t dev = {};
        dev.clock_speed_hz = 2 * 1000 * 1000; /* SX127x max 10 MHz */
        dev.mode = 0;
        dev.spics_io_num = -1; /* RadioLib toggles CS as a GPIO */
        dev.queue_size = 1;
        ESP_ERROR_CHECK(spi_bus_add_device(SPI2_HOST, &dev, &spi_));

        /* weakest drive on the bus lines too — SCLK/MOSI edges at CAP_2
         * contribute to the SD-block crosstalk (see pinMode note) */
        gpio_set_drive_capability((gpio_num_t)HAT_PIN_SCLK, GPIO_DRIVE_CAP_0);
        gpio_set_drive_capability((gpio_num_t)HAT_PIN_MOSI, GPIO_DRIVE_CAP_0);
    }

    void term() override {}

    void pinMode(uint32_t pin, uint32_t mode) override
    {
        if (pin == RADIOLIB_NC) return;
        gpio_config_t c = {};
        c.pin_bit_mask = 1ULL << pin;
        c.mode = (gpio_mode_t)mode;
        c.pull_up_en = GPIO_PULLUP_DISABLE;
        c.pull_down_en = GPIO_PULLDOWN_DISABLE;
        c.intr_type = GPIO_INTR_DISABLE;
        gpio_config(&c);
        /* Weakest drive on every radio output: CS=GPIO45 sits right next
         * to the SD block (GPIO39-44) and full-strength edges during
         * begin()'s SPI burst crosstalk-wedge the card (0x107 forever,
         * only a power dip revives it). Radio SPI is 2 MHz — CAP_0 is
         * plenty. (gpio_config resets drive to CAP_2, so re-apply here.) */
        gpio_set_drive_capability((gpio_num_t)pin, GPIO_DRIVE_CAP_0);
    }

    void digitalWrite(uint32_t pin, uint32_t value) override
    {
        if (pin == RADIOLIB_NC) return;
        gpio_set_level((gpio_num_t)pin, value);
    }

    uint32_t digitalRead(uint32_t pin) override
    {
        if (pin == RADIOLIB_NC) return 0;
        return (uint32_t)gpio_get_level((gpio_num_t)pin);
    }

    void attachInterrupt(uint32_t pin, void (*cb)(void),
                         uint32_t mode) override
    {
        if (pin == RADIOLIB_NC) return;
        gpio_set_intr_type((gpio_num_t)pin, (gpio_int_type_t)mode);
        gpio_isr_handler_add((gpio_num_t)pin, (gpio_isr_t)cb, nullptr);
        gpio_intr_enable((gpio_num_t)pin);
    }

    void detachInterrupt(uint32_t pin) override
    {
        if (pin == RADIOLIB_NC) return;
        gpio_isr_handler_remove((gpio_num_t)pin);
        gpio_intr_disable((gpio_num_t)pin);
    }

    void delay(RadioLibTime_t ms) override
    {
        if (ms == 0) { taskYIELD(); return; }
        vTaskDelay(pdMS_TO_TICKS(ms ? ms : 1));
    }

    void delayMicroseconds(RadioLibTime_t us) override
    {
        ets_delay_us((uint32_t)us);
    }

    /* RadioLib's blocking waits (transmit() spinning on DIO1 for the
     * whole ~300ms airtime, standby transitions, ...) poll via yield().
     * The base-class yield() is a no-op -> pure busy-spin at this
     * task's priority, which starved the SDMMC driver during every TX
     * burst (0x107 tile-read timeouts whenever the scenario published).
     * Sleep a tick instead: TX-done detection lags <=10ms, and the SD
     * card / LVGL keep breathing through transmissions. */
    void yield() override { vTaskDelay(1); }

    RadioLibTime_t millis() override
    {
        return (RadioLibTime_t)(esp_timer_get_time() / 1000ULL);
    }

    RadioLibTime_t micros() override
    {
        return (RadioLibTime_t)esp_timer_get_time();
    }

    long pulseIn(uint32_t pin, uint32_t state,
                 RadioLibTime_t timeout) override
    {
        (void)pin; (void)state; (void)timeout;
        return 0; /* unused by SX127x driver */
    }

    void spiBegin() override {}
    void spiBeginTransaction() override {}

    void spiTransfer(uint8_t *out, size_t len, uint8_t *in) override
    {
        spi_transaction_t t = {};
        t.length = len * 8;
        t.tx_buffer = out;
        t.rx_buffer = in;
        spi_device_transmit(spi_, &t);
    }

    void spiEndTransaction() override {}
    void spiEnd() override {}

private:
    spi_device_handle_t spi_ = nullptr;
};

/* ---------------- driver task ---------------- */

static P4Hal *s_hal;
static PhysicalLayer *s_radio; /* SX1262 (HAT) or SX1276 (bonnet) */
static int s_irq_pin = -1;     /* DIO1 (HAT) or DIO0 (bonnet) */
static TaskHandle_t s_task;
static QueueHandle_t s_txq; /* wire frames waiting for airtime */
static vmesh_mesh_t s_mesh; /* geo-ephemeral flooding core — owned by
                               the radio task exclusively */

typedef struct {
    uint8_t len;
    bool own; /* locally originated: register in dedup before tx */
    uint8_t buf[VMESH_WIRE_MAX];
} tx_item_t;

static uint32_t now_ms(void)
{
    return (uint32_t)(esp_timer_get_time() / 1000ULL);
}

static volatile bool s_rx_flag;
static volatile uint32_t s_irq_count; /* DIO1 fire diagnostics */

static void IRAM_ATTR dio0_isr(void *arg)
{
    (void)arg;
    s_rx_flag = true;
    s_irq_count++;
    BaseType_t hp = pdFALSE;
    if (s_task) vTaskNotifyGiveFromISR(s_task, &hp);
    portYIELD_FROM_ISR(hp);
}

static void bonnet_tx_hook(const vmesh_msg_t *m)
{
    /* runs in the UI task — no mesh state here; the radio task
     * registers own-messages in the dedup cache before transmit */
    tx_item_t item;
    int n = vmesh_wire_encode(m, item.buf, sizeof(item.buf));
    if (n <= 0) return;
    item.len = (uint8_t)n;
    item.own = true;
    xQueueSend(s_txq, &item, 0); /* full queue: drop, best-effort */
}

/* mesh relay decision fired -> queue the rebroadcast */
static void mesh_relay_cb(const vmesh_msg_t *m, void *user)
{
    (void)user;
    tx_item_t item;
    int n = vmesh_wire_encode(m, item.buf, sizeof(item.buf));
    if (n <= 0) return;
    item.len = (uint8_t)n;
    item.own = false;
    xQueueSend(s_txq, &item, 0);
}

static void radio_task(void *arg)
{
    (void)arg;
    int64_t last_diag = 0;
    uint32_t last_irqs = 0, rx_reads = 0;
    for (;;) {
        ulTaskNotifyTake(pdTRUE, pdMS_TO_TICKS(50));

        /* diagnostics: DIO1 fire rate (storm detection) */
        int64_t now = esp_timer_get_time();
        if (now - last_diag > 5 * 1000 * 1000) {
            if (last_diag != 0)
                ESP_LOGI(TAG, "diag: %u DIO1 irq / %u rx-reads in 5s",
                         (unsigned)(s_irq_count - last_irqs), (unsigned)rx_reads);
            last_irqs = s_irq_count; rx_reads = 0; last_diag = now;
        }

        vmesh_pose_t pose;
        vmesh_pose_get(&pose);

        if (s_rx_flag) {
            s_rx_flag = false;
            rx_reads++;
            /* The USB dongles run STREAM mode: their over-the-air
             * payload is the UART byte-pipe verbatim, i.e. OUR DTU
             * framing [56 4D len payload crc16]. Unwrap it when
             * present so HAT nodes interop with dongle nodes; fall
             * back to raw for HAT<->HAT frames. */
            uint8_t buf[LORA_DTU_MAX_PAYLOAD + LORA_DTU_OVERHEAD];
            size_t len = s_radio->getPacketLength();
            int rst = (len > 0 && len <= sizeof(buf))
                          ? s_radio->readData(buf, len) : -999;
            ESP_LOGI(TAG, "rx: len=%u readData=%d rssi=%.0f snr=%.1f",
                     (unsigned)len, rst,
                     s_radio->getRSSI(), s_radio->getSNR());
            if (rst == RADIOLIB_ERR_NONE) {
                uint8_t inner[VMESH_WIRE_MAX];
                const uint8_t *fr = buf; size_t fn = len;
                lora_dtu_rx_t d; memset(&d, 0, sizeof(d));
                for (size_t i = 0; i < len; i++) {
                    int r = lora_dtu_rx_feed(&d, buf[i], inner,
                                             sizeof(inner));
                    if (r > 0) { fr = inner; fn = (size_t)r; break; }
                }
                ESP_LOGI(TAG, "rx: %s frame, %uB",
                         fr == inner ? "DTU-unwrapped" : "raw", (unsigned)fn);
                vmesh_msg_t m;
                int dec = vmesh_wire_decode(fr, fn, &m);
                if (dec != 0)
                    ESP_LOGW(TAG, "rx: vmesh decode failed (%d)", dec);
                if (dec == 0) {
                    /* RSSI -> proximity hint for contention-based
                     * forwarding (−130 dBm edge .. −40 dBm on top) */
                    float rssi = s_radio->getRSSI();
                    float prox = (rssi + 130.0f) / 90.0f;
                    bool acc = mesh_rx(&s_mesh, &m, prox, pose.lat,
                                       pose.lon, now_ms(), vmesh_time_s());
                    ESP_LOGI(TAG, "rx: origin=%08X seq=%u %s",
                             (unsigned)m.origin_id, (unsigned)m.seq,
                             acc ? "INJECTED" : "dropped (dedup/radius/age)");
                    if (acc)
                        vmesh_feed_inject(&m);
                }
            }
            s_radio->startReceive();
        }

        /* relays whose contention window elapsed */
        mesh_tick(&s_mesh, now_ms(), mesh_relay_cb, nullptr);

        tx_item_t item;
        while (xQueueReceive(s_txq, &item, 0) == pdTRUE) {
            if (item.own) {
                vmesh_msg_t m;
                if (vmesh_wire_decode(item.buf, item.len, &m) == 0)
                    mesh_note_own(&s_mesh, &m);
            }
            /* blocking transmit is fine here — this task owns airtime */
#if VMESH_HAT_TX_MUTE
            /* diagnostic: radio fully up (begin+RX) but drop TX frames —
             * discriminates "TX event breaks SD" from "SPI/RX breaks SD" */
            ESP_LOGI(TAG, "tx muted (%u bytes dropped)", item.len);
#else
            /* wrap in DTU framing so dongle nodes (stream mode: LoRa
             * payload -> UART verbatim) deframe it on their serial side */
            uint8_t wire[LORA_DTU_MAX_PAYLOAD + LORA_DTU_OVERHEAD];
            int wn = lora_dtu_frame(item.buf, item.len, wire, sizeof(wire));
            if (wn > 0) {
                /* RTC6603SP switch: TXEN LOW during transmit (RXEN=DIO2
                 * auto-HIGH), HIGH after to return the switch to RX. */
                gpio_set_level((gpio_num_t)HAT_PIN_TXEN, 0);
                ets_delay_us(60);
                int st = s_radio->transmit(wire, (size_t)wn);
                gpio_set_level((gpio_num_t)HAT_PIN_TXEN, 1);
                if (st != RADIOLIB_ERR_NONE)
                    ESP_LOGW(TAG, "tx failed: %d", st);
                else
                    ESP_LOGI(TAG, "tx %dB ota", wn);
            }
#endif
            s_radio->startReceive();
        }
    }
}

/* Try the Waveshare SX1262 LoRaWAN/GNSS HAT (§0c): raw SPI, DIO2
 * drives the RF switch's RX pole, HAT_PIN_RXEN gates the other
 * (their "TXEN", active-high FOR RECEIVE — see hat_pins.h). */
static bool try_sx1262_hat(void)
{
    SX1262 *sx = new SX1262(
        new Module(s_hal, HAT_PIN_CS, HAT_PIN_DIO1,
                   HAT_PIN_RST, HAT_PIN_BUSY));

    /* XTAL only. The Waveshare LoRaRF demo never touches DIO3/TCXO and
     * leaves the XTAL-trim write commented -> this module has a plain
     * crystal. Passing a TCXO voltage makes begin() wait on a crystal
     * that isn't there and stall in calibration; don't.
     * Retry: radio start now overlaps the C6 SDIO bring-up (it must
     * precede the TF mount — see main.c), and the first attempt can
     * transiently fail there. */
    int st = RADIOLIB_ERR_NONE;
    for (int attempt = 0; attempt < 3; attempt++) {
        if (attempt) vTaskDelay(pdMS_TO_TICKS(400));
        st = sx->begin(HAT_FREQ_MHZ, HAT_BW_KHZ, HAT_SF, HAT_CR,
                       HAT_SYNC_WORD, HAT_POWER_DBM, HAT_PREAMBLE,
                       0.0f);
        if (st == RADIOLIB_ERR_NONE) break;
        ESP_LOGW(TAG, "SX1262 begin attempt %d: %d", attempt + 1, st);
    }
    if (st == RADIOLIB_ERR_NONE) {
        ESP_LOGI(TAG, "SX1262 HAT: XTAL config accepted");
        /* Keep RadioLib's default DC-DC regulator: the HP PA draws
         * ~100mA at 17dBm and the LDO can't source it — forcing LDO
         * keys the PA but radiates ~nothing (RX unaffected). That was
         * the dead-TX cause. (LDO was a failed SD-interference attempt;
         * SD coexistence is handled by boot ordering instead.) */
    }
    if (st != RADIOLIB_ERR_NONE) {
        ESP_LOGI(TAG, "SX1262 HAT: XTAL begin=%d", st);
        ESP_LOGW(TAG, "SX1262 HAT not found (begin=%d)", st);
#if VMESH_HAT_SPISCAN
        delete sx;
        xTaskCreate(hat_spiscan_task, "spiscan", 6144, NULL, 3, NULL);
        return false; /* empirical pin discovery owns the pins now */
#endif
#if VMESH_HAT_GPSHUNT
        delete sx;
        xTaskCreate(hat_gpshunt_task, "gpshunt", 4096, NULL, 3, NULL);
        return false; /* signal hunt owns the pins now */
#endif
#if VMESH_HAT_LIVEPROBE
        delete sx;
        xTaskCreate(hat_liveprobe_task, "hatprobe", 4096, NULL, 3, NULL);
        return false; /* live tuning owns the pins now */
#endif
#if !VMESH_HAT_DIAG
        /* Pins are KNOWN-correct now (verified working map). The wiring
         * diagnostics below bit-bang garbage SPI that makes the chip
         * respond — the exact activity that wedges the SD card — so on
         * a transient begin() failure just bail quietly and leave the
         * card its clean init. Re-enable with VMESH_HAT_DIAG=1. */
        delete sx;
        return false;
#endif
        /* cheap wiring hint: after a reset pulse BUSY should go idle-
         * low once the chip is ready; stuck high = power/seating */
        s_hal->pinMode(HAT_PIN_RST, GPIO_MODE_OUTPUT);
        s_hal->digitalWrite(HAT_PIN_RST, 0);
        vTaskDelay(pdMS_TO_TICKS(2));
        s_hal->digitalWrite(HAT_PIN_RST, 1);
        vTaskDelay(pdMS_TO_TICKS(10));
        s_hal->pinMode(HAT_PIN_BUSY, GPIO_MODE_INPUT);
        ESP_LOGW(TAG, "  BUSY after reset: %d (0=chip ready, 1=stuck)",
                 (int)s_hal->digitalRead(HAT_PIN_BUSY));
        /* Is BUSY really the chip's BUSY? A live SX1262 holds BUSY HIGH
         * during its post-reset boot (~1-2ms) then drops it LOW. A
         * floating/wrong pin won't show that transition. */
        {
            gpio_reset_pin((gpio_num_t)HAT_PIN_RST);
            gpio_reset_pin((gpio_num_t)HAT_PIN_BUSY);
            gpio_set_direction((gpio_num_t)HAT_PIN_RST, GPIO_MODE_OUTPUT);
            gpio_set_direction((gpio_num_t)HAT_PIN_BUSY, GPIO_MODE_INPUT);
            gpio_set_level((gpio_num_t)HAT_PIN_RST, 0);
            ets_delay_us(500);
            int during = gpio_get_level((gpio_num_t)HAT_PIN_BUSY);
            gpio_set_level((gpio_num_t)HAT_PIN_RST, 1);
            ets_delay_us(200);
            int t200us = gpio_get_level((gpio_num_t)HAT_PIN_BUSY);
            ets_delay_us(1500);
            int t1700us = gpio_get_level((gpio_num_t)HAT_PIN_BUSY);
            vTaskDelay(pdMS_TO_TICKS(10));
            int t10ms = gpio_get_level((gpio_num_t)HAT_PIN_BUSY);
            ESP_LOGW(TAG, "  BUSY(GPIO%d) thru reset: during=%d +200us=%d "
                          "+1.7ms=%d +10ms=%d  %s", HAT_PIN_BUSY,
                     during, t200us, t1700us, t10ms,
                     (t200us == 1 && t10ms == 0) ? "<-- REAL BUSY (chip alive!)"
                                                 : "(no transition — pin may be wrong)");
        }

        /* SX1262 GetStatus(0xC0) bit-bang on the CORRECT HAT pins —
         * a live chip returns a non-0x00/0xFF status byte. This is
         * the real liveness test (the SX1276 matrix below is bogus
         * for a 1262). Also read reg 0x0740 (LoRa sync MSB, def 0x14). */
        {
            const int SCK = HAT_PIN_SCLK, MOSI = HAT_PIN_MOSI,
                      MISO = HAT_PIN_MISO, CS = HAT_PIN_CS,
                      BUSY = HAT_PIN_BUSY;
            /* detach SPI2 (spi_bus_initialize owns these) so we can
             * bit-bang with real GPIO control */
            gpio_reset_pin((gpio_num_t)SCK);
            gpio_reset_pin((gpio_num_t)MOSI);
            gpio_reset_pin((gpio_num_t)MISO);
            gpio_reset_pin((gpio_num_t)CS);
            gpio_set_direction((gpio_num_t)SCK, GPIO_MODE_OUTPUT);
            gpio_set_direction((gpio_num_t)MOSI, GPIO_MODE_OUTPUT);
            gpio_set_direction((gpio_num_t)CS, GPIO_MODE_OUTPUT);
            gpio_set_direction((gpio_num_t)MISO, GPIO_MODE_INPUT);
            gpio_set_direction((gpio_num_t)BUSY, GPIO_MODE_INPUT);
            s_hal->digitalWrite(CS, 1);
            s_hal->digitalWrite(SCK, 0);
            auto xfer = [&](uint8_t out)->uint8_t {
                uint8_t in = 0;
                for (int b = 7; b >= 0; b--) {
                    s_hal->digitalWrite(MOSI, (out >> b) & 1);
                    ets_delay_us(4);
                    s_hal->digitalWrite(SCK, 1);
                    ets_delay_us(4);
                    in = (uint8_t)((in << 1) | s_hal->digitalRead(MISO));
                    s_hal->digitalWrite(SCK, 0);
                }
                return in;
            };
            /* wait BUSY low */
            int guard = 1000;
            while (s_hal->digitalRead(BUSY) && guard-- > 0)
                ets_delay_us(10);
            s_hal->digitalWrite(CS, 0); ets_delay_us(2);
            uint8_t stat0 = xfer(0xC0);  /* GetStatus opcode */
            uint8_t stat1 = xfer(0x00);  /* status byte */
            s_hal->digitalWrite(CS, 1);
            ets_delay_us(20);
            /* ReadRegister 0x0740 */
            s_hal->digitalWrite(CS, 0); ets_delay_us(2);
            xfer(0x1D); xfer(0x07); xfer(0x40); xfer(0x00);
            uint8_t reg = xfer(0x00);
            s_hal->digitalWrite(CS, 1);
            ESP_LOGW(TAG, "  SX1262 probe: GetStatus=0x%02X/0x%02X "
                          "reg0x0740=0x%02X %s",
                     stat0, stat1, reg,
                     (stat1 && stat1 != 0xFF) ? "<-- CHIP ALIVE"
                                              : "(no response)");
            /* toggle-test each SPI output: can the P4 actually drive it?
             * gpio_get_level on an output returns the driven level. */
            int tst[] = { SCK, MOSI, CS };
            const char *nm[] = { "SCLK", "MOSI", "CS" };
            for (int i = 0; i < 3; i++) {
                gpio_reset_pin((gpio_num_t)tst[i]); /* detach any peripheral */
                gpio_set_direction((gpio_num_t)tst[i], GPIO_MODE_INPUT_OUTPUT);
                gpio_set_level((gpio_num_t)tst[i], 0);
                ets_delay_us(20);
                int lo = gpio_get_level((gpio_num_t)tst[i]);
                gpio_set_level((gpio_num_t)tst[i], 1);
                ets_delay_us(20);
                int hi = gpio_get_level((gpio_num_t)tst[i]);
                ESP_LOGW(TAG, "  drive GPIO%d (%s) after reset_pin: "
                              "low=%d high=%d %s",
                         tst[i], nm[i], lo, hi,
                         (lo == 0 && hi == 1) ? "OK"
                                              : "<-- WON'T DRIVE");
            }
            /* MISO float test */
            gpio_config_t up = {}; up.pin_bit_mask = 1ULL << MISO;
            up.mode = GPIO_MODE_INPUT; up.pull_up_en = GPIO_PULLUP_ENABLE;
            gpio_config(&up); ets_delay_us(50);
            int mu = gpio_get_level((gpio_num_t)MISO);
            up.pull_up_en = GPIO_PULLUP_DISABLE;
            up.pull_down_en = GPIO_PULLDOWN_ENABLE;
            gpio_config(&up); ets_delay_us(50);
            int md = gpio_get_level((gpio_num_t)MISO);
            ESP_LOGW(TAG, "  MISO GPIO%d float: pullup=%d pulldn=%d %s",
                     MISO, mu, md,
                     (mu == 1 && md == 0) ? "floating (chip not driving)"
                                          : "driven/held");
        }
        delete sx;
        return false;
    }

    /* RTC6603SP SPDT antenna switch (per HAT wiki + datasheet truth
     * table): its two controls V1/V2 must be COMPLEMENTARY — both-high
     * or both-low = isolation (nothing routes). V1=RXEN=SX1262 DIO2
     * (auto), V2=TXEN=GPIO26 (host). TX needs RXEN(DIO2)=H & TXEN=L;
     * RX needs RXEN(DIO2)=L & TXEN=H. So DIO2-as-rf-switch handles RXEN,
     * and TXEN must be driven as the INVERSE of tx/rx: LOW on transmit,
     * HIGH on receive. (Driving TXEN high-on-tx — what we did before —
     * put the switch in both-high isolation → dead TX; and idle-low RX
     * was both-low isolation → the weak -51dBm coupling-only RX.) */
    sx->setDio2AsRfSwitch(true);
    gpio_reset_pin((gpio_num_t)HAT_PIN_TXEN);
    gpio_set_direction((gpio_num_t)HAT_PIN_TXEN, GPIO_MODE_OUTPUT);
    gpio_set_level((gpio_num_t)HAT_PIN_TXEN, 1); /* idle = RX: TXEN HIGH */

#if VMESH_HAT_RFSCAN
    /* RF-profile hunt: a beacon hammers the dongle's serial side every
     * 300ms; sweep freq x sync x SF until the HAT hears it. Poll the
     * IRQ register directly — no ISR needed. */
    {
        static const float FREQS[] = { 868.125f, 915.0f, 868.0f, 850.125f, 915.125f };
        static const float BWS[]   = { 125.0f, 250.0f, 500.0f };
        static const int   SFS[]   = { 7, 8, 9, 10, 11, 12 };
        ESP_LOGW(TAG, "RFSCAN-RX: wide receiver sweep vs the dongle beacon "
                      "(~2.2s/combo, syncword 0x12)");
        sx->setSyncWord(0x12);
        for (unsigned pass = 0; ; pass++)   /* loop until reflashed */
        for (unsigned fi = 0; fi < 5; fi++)
        for (unsigned bi = 0; bi < 3; bi++)
        for (unsigned pi = 0; pi < 6; pi++) {
            sx->standby();
            sx->setFrequency(FREQS[fi]);
            sx->setBandwidth(BWS[bi]);
            sx->setSpreadingFactor(SFS[pi]);
            sx->startReceive();
            int64_t t0 = esp_timer_get_time();
            bool hit = false;
            while (esp_timer_get_time() - t0 < 2200000) {
                if (sx->getIrqFlags() & RADIOLIB_SX126X_IRQ_RX_DONE) {
                    uint8_t b[64]; size_t l = sx->getPacketLength();
                    int st = sx->readData(b, l > 63 ? 63 : l);
                    if (st == RADIOLIB_ERR_NONE && l > 0) {
                        b[l > 63 ? 63 : l] = 0;
                        ESP_LOGW(TAG, "  *** HIT %.3fMHz BW%.0f SF%d: %uB "
                                 "rssi=%.0f snr=%.1f \"%.24s\" ***",
                                 FREQS[fi], BWS[bi], SFS[pi], (unsigned)l,
                                 sx->getRSSI(), sx->getSNR(), (char *)b);
                        hit = true;
                    }
                    sx->startReceive();
                }
                vTaskDelay(pdMS_TO_TICKS(8));
            }
            if (!hit && pi == 0)
                ESP_LOGI(TAG, "  %.3fMHz BW%.0f: SF7 quiet...",
                         FREQS[fi], BWS[bi]);
        }
        ESP_LOGW(TAG, "RFSCAN DONE");
        delete sx;
        return false;
    }
#endif
#if VMESH_HAT_TXBEACON
    /* Isolate HAT->dongle: transmit a plain marker every 1.5s at the
     * matched profile. Read the dongle's RAW serial: bytes out => RF
     * link fine (framing/parse is the problem); silence => TX RF path
     * (switch / antenna) is the problem. */
    /* Switch confirmed responsive (RX -51->-31). Brute-force the 4
     * DIO2-rf-switch x TXEN states during TX, combo # in the payload so
     * the dongle's raw serial names the winner. */
    sx->setOutputPower(17);
    for (unsigned n = 0; ; n++) {
        int combo = (n / 3) % 4;      /* 3 tx per combo */
        bool rfsw = combo & 1;        /* setDio2AsRfSwitch */
        bool txen = combo & 2;        /* TXEN level during tx */
        sx->standby();
        sx->setDio2AsRfSwitch(rfsw);
        char msg[48];
        int len = snprintf(msg, sizeof(msg), "WCTX c=%d rfsw=%d txen=%d n=%u",
                           combo, rfsw, txen, n);
        gpio_set_level((gpio_num_t)HAT_PIN_TXEN, txen ? 1 : 0);
        ets_delay_us(60);
        int st = sx->transmit((uint8_t *)msg, (size_t)len);
        gpio_set_level((gpio_num_t)HAT_PIN_TXEN, 1); /* idle back to RX */
        ESP_LOGI(TAG, "  c=%d rfsw=%d txen=%d st=%d", combo, rfsw, txen, st);
        vTaskDelay(pdMS_TO_TICKS(1200));
    }
#endif
    s_radio = sx;
    s_irq_pin = HAT_PIN_DIO1;
    radio_hat_found = true;
    ESP_LOGI(TAG, "SX1262 HAT up: %.1f MHz SF%d BW%.0f",
             HAT_FREQ_MHZ, HAT_SF, HAT_BW_KHZ);
    return true;
}

/* ---------------- HAT pin auto-discovery ----------------
 * The pin map couldn't be read reliably off the schematic. The SX1262
 * drives BUSY low at idle and pulses it HIGH during a reset. Sweep
 * every header GPIO as a RESET candidate; whichever GPIO shows the
 * high-then-low blip in response is BUSY, and the pulsed pin is RST.
 * Then MISO = a pin that floats high (pull-up wins) but is NOT in the
 * driven set. Outputs everything to the log; changes no product state. */
/* SAFE candidate set: excludes GPIO7/8 (I2C codec+touch — driving
 * breaks touch/boot), 24/25 (native USB D-/D+), 37/38 (console UART),
 * 54 (C6 SDIO reset). The SX1262's 7 signals are all outside those. */
static const int HDR_GPIOS[] = {
    23, 21, 20, 6, 3, 2, 0, 33, 26, 48, 53, 47,
    22, 5, 4, 1, 36, 32, 46, 27, 45
};
#define HDR_N ((int)(sizeof(HDR_GPIOS)/sizeof(HDR_GPIOS[0])))

static int read_pulled(int g, int pull /*1=up,0=down*/)
{
    gpio_config_t c = {};
    c.pin_bit_mask = 1ULL << g;
    c.mode = GPIO_MODE_INPUT;
    c.pull_up_en = pull ? GPIO_PULLUP_ENABLE : GPIO_PULLUP_DISABLE;
    c.pull_down_en = pull ? GPIO_PULLDOWN_DISABLE : GPIO_PULLDOWN_ENABLE;
    gpio_config(&c);
    ets_delay_us(60);
    return gpio_get_level((gpio_num_t)g);
}

static void hat_autoscan(void)
{
    for (int i = 0; i < HDR_N; i++) gpio_reset_pin((gpio_num_t)HDR_GPIOS[i]);

    ESP_LOGW(TAG, "=== HAT AUTOSCAN ===");
    bool drivenLow[64] = {false};
    for (int i = 0; i < HDR_N; i++) {
        int g = HDR_GPIOS[i];
        int up = read_pulled(g, 1), dn = read_pulled(g, 0);
        const char *cls = (up == 0 && dn == 0) ? "driven-LOW (BUSY/DIO?)"
                        : (up == 1 && dn == 1) ? "driven-HIGH"
                        : "floating";
        if (up == 0 && dn == 0) drivenLow[g] = true;
        ESP_LOGW(TAG, "  GPIO%-2d  up=%d dn=%d  %s", g, up, dn, cls);
    }

    (void)drivenLow;
    /* The official HAT pinout gives the SPI pins directly: SCK=GPIO32,
     * MOSI=GPIO1, MISO=GPIO36 (all confirmed contacting). RST(phys12)
     * and BUSY(phys38) do NOT seat -- but a raw ReadRegister needs
     * NEITHER: the SX1262 answers register reads in its POR standby
     * state. So fix the SPI trio and sweep CS over every other header
     * pin, reading two registers whose POR defaults are known
     * (0x0740=0x14, 0x0741=0x24 -> sync word 0x1424). A pin that
     * returns both is unambiguously the chip select of a live chip. */
    const int SCK = 32, MOSI = 1, MISO = 36;
    gpio_reset_pin((gpio_num_t)SCK);  gpio_reset_pin((gpio_num_t)MOSI);
    gpio_reset_pin((gpio_num_t)MISO);
    gpio_set_direction((gpio_num_t)SCK, GPIO_MODE_OUTPUT);
    gpio_set_direction((gpio_num_t)MOSI, GPIO_MODE_OUTPUT);
    gpio_set_direction((gpio_num_t)MISO, GPIO_MODE_INPUT);
    gpio_set_level((gpio_num_t)SCK, 0);
    ESP_LOGW(TAG, "--- SPI CS scan (SCK=32 MOSI=1 MISO=36, no RST/BUSY) ---");
    for (int i = 0; i < HDR_N; i++) {
        int CS = HDR_GPIOS[i];
        if (CS == SCK || CS == MOSI || CS == MISO) continue;
        gpio_reset_pin((gpio_num_t)CS);
        gpio_set_direction((gpio_num_t)CS, GPIO_MODE_OUTPUT);
        gpio_set_level((gpio_num_t)CS, 1);
        ets_delay_us(20);
        auto xfer = [&](uint8_t out)->uint8_t {
            uint8_t in = 0;
            for (int b = 7; b >= 0; b--) {
                gpio_set_level((gpio_num_t)MOSI, (out >> b) & 1);
                ets_delay_us(4);
                gpio_set_level((gpio_num_t)SCK, 1);
                ets_delay_us(4);
                in = (uint8_t)((in << 1) | gpio_get_level((gpio_num_t)MISO));
                gpio_set_level((gpio_num_t)SCK, 0);
            }
            return in;
        };
        gpio_set_level((gpio_num_t)CS, 0); ets_delay_us(2);
        uint8_t st = xfer(0xC0); uint8_t st1 = xfer(0x00);
        gpio_set_level((gpio_num_t)CS, 1); ets_delay_us(10);
        gpio_set_level((gpio_num_t)CS, 0); ets_delay_us(2);
        xfer(0x1D); xfer(0x07); xfer(0x40); xfer(0x00);
        uint8_t r40 = xfer(0x00), r41 = xfer(0x00);
        gpio_set_level((gpio_num_t)CS, 1);
        (void)st;
        bool alive = (r40 == 0x14 && r41 == 0x24);
        ESP_LOGW(TAG, "  CS=GPIO%-2d  status=0x%02X reg40=0x%02X reg41=0x%02X %s",
                 CS, st1, r40, r41,
                 alive ? "<<<<< CHIP SELECT (sync 0x1424, chip is ALIVE)"
                       : (st1 && st1 != 0xFF && st1 != 0x00) ? "(spi activity?)" : "");
        gpio_reset_pin((gpio_num_t)CS);
    }
    ESP_LOGW(TAG, "=== AUTOSCAN DONE ===");
}

extern "C" bool radio_bonnet_start(void)
{
#if VMESH_HAT_AUTOSCAN
    s_hal = new P4Hal();
    hat_autoscan();
    return false;
#endif
    if (BONNET_PIN_SCLK < 0) {
        ESP_LOGW(TAG, "bonnet pins not configured (bonnet_pins.h) — "
                      "radio transport idle");
        return false;
    }

    s_hal = new P4Hal();

#if VMESH_HAT_INIT_STAGE1
    /* BISECT stage 1 + timeline: fire each suspect action spaced 15s
     * apart; the first 0x107 timestamp names the killer.
     *   t+15s: RST pulse on GPIO22 (what RadioLib reset() does)
     *   t+30s: CS toggling on GPIO45 (what every SPI transfer does)
     *   t+45s: real SPI transactions (clock+MOSI+MISO, CS held high) */
    s_hal->init();
    ESP_LOGW(TAG, "INIT_STAGE1+TIMELINE: bus up, waiting");
    xTaskCreate([](void *) {
        vTaskDelay(pdMS_TO_TICKS(15000));
        ESP_LOGW(TAG, "EVENT A: RST pulse GPIO22");
        gpio_set_direction((gpio_num_t)HAT_PIN_RST, GPIO_MODE_OUTPUT);
        gpio_set_level((gpio_num_t)HAT_PIN_RST, 1); ets_delay_us(200);
        gpio_set_level((gpio_num_t)HAT_PIN_RST, 0); ets_delay_us(2000);
        gpio_set_level((gpio_num_t)HAT_PIN_RST, 1);
        vTaskDelay(pdMS_TO_TICKS(15000));
        ESP_LOGW(TAG, "EVENT B: CS toggle burst GPIO45");
        gpio_set_direction((gpio_num_t)HAT_PIN_CS, GPIO_MODE_OUTPUT);
        for (int i = 0; i < 200; i++) {
            gpio_set_level((gpio_num_t)HAT_PIN_CS, i & 1); ets_delay_us(50);
        }
        gpio_set_level((gpio_num_t)HAT_PIN_CS, 1);
        vTaskDelay(pdMS_TO_TICKS(15000));
        ESP_LOGW(TAG, "EVENT C: SPI transaction burst (CS high)");
        for (int i = 0; i < 50; i++) {
            uint8_t out[4] = {0x1D, 0x07, 0x40, 0x00}, in[4];
            s_hal->spiTransfer(out, 4, in);
            vTaskDelay(1);
        }
        ESP_LOGW(TAG, "TIMELINE DONE");
        vTaskDelete(NULL);
    }, "hat_timeline", 4096, NULL, 3, NULL);
    return false;
#endif

    if (!try_sx1262_hat()) {
        /* fall back to the SX1276 bonnet path */
    SX1276 *sx76 = new SX1276(
        new Module(s_hal, BONNET_PIN_CS, BONNET_PIN_DIO0,
                   BONNET_PIN_RST, RADIOLIB_NC));

    int st = sx76->begin(BONNET_FREQ_MHZ, BONNET_BW_KHZ, BONNET_SF,
                         BONNET_CR, BONNET_SYNC_WORD,
                         BONNET_POWER_DBM, BONNET_PREAMBLE);
    if (st != RADIOLIB_ERR_NONE) {
        ESP_LOGW(TAG, "SX1276 begin failed (%d) — wiring probe matrix:",
                 st);
        /* Try every plausible wiring hypothesis by bit-banging a
         * RegVersion (0x42) read: {normal, MISO<->MOSI} x {CE1, CE0}.
         * 0x12 = found the chip; all-0x00/0xFF = look at hardware. */
        struct combo { int sck, mosi, miso, cs; const char *name; };
        const combo combos[] = {
            { BONNET_PIN_SCLK, BONNET_PIN_MOSI, BONNET_PIN_MISO, 32,
              "normal, CS=CE1(32)" },
            { BONNET_PIN_SCLK, BONNET_PIN_MISO, BONNET_PIN_MOSI, 32,
              "MISO<->MOSI, CS=CE1(32)" },
            { BONNET_PIN_SCLK, BONNET_PIN_MOSI, BONNET_PIN_MISO, 36,
              "normal, CS=CE0(36)" },
            { BONNET_PIN_SCLK, BONNET_PIN_MISO, BONNET_PIN_MOSI, 36,
              "MISO<->MOSI, CS=CE0(36)" },
        };
        for (const combo &c : combos) {
            /* bit-bang: all plain GPIO, no SPI peripheral involved */
            s_hal->pinMode(c.sck, GPIO_MODE_OUTPUT);
            s_hal->pinMode(c.mosi, GPIO_MODE_OUTPUT);
            s_hal->pinMode(c.cs, GPIO_MODE_OUTPUT);
            s_hal->pinMode(c.miso, GPIO_MODE_INPUT);
            s_hal->digitalWrite(c.cs, 1);
            s_hal->digitalWrite(c.sck, 0);
            vTaskDelay(1);
            uint8_t addr = 0x42 & 0x7F, val = 0;
            s_hal->digitalWrite(c.cs, 0);
            ets_delay_us(5);
            for (int b = 7; b >= 0; b--) { /* write address, mode 0 */
                s_hal->digitalWrite(c.mosi, (addr >> b) & 1);
                ets_delay_us(3);
                s_hal->digitalWrite(c.sck, 1);
                ets_delay_us(3);
                s_hal->digitalWrite(c.sck, 0);
            }
            for (int b = 7; b >= 0; b--) { /* read value */
                ets_delay_us(3);
                s_hal->digitalWrite(c.sck, 1);
                ets_delay_us(3);
                val = (uint8_t)((val << 1) |
                                s_hal->digitalRead(c.miso));
                s_hal->digitalWrite(c.sck, 0);
            }
            s_hal->digitalWrite(c.cs, 1);
            ESP_LOGW(TAG, "  %-26s -> 0x%02X %s", c.name, val,
                     val == 0x12 ? "<-- CHIP FOUND" : "");
            vTaskDelay(pdMS_TO_TICKS(20));
        }

        /* NOTE: no I2C scan here — header pins 3/5 share the bus with
         * the kit's codec AND the GT911 touch; probing it broke touch
         * and boot-looped the system (learned the hard way).
         *
         * Float tests instead: the bonnet has pull-ups on its CS lines
         * (CE0=GPIO36, CE1=GPIO32). "held high" there = bonnet powered
         * and making contact; MISO(2)/MOSI(3)/DIO0(6) floating vs held
         * localizes any bad socket positions. */
        static const int float_pins[] = { 2, 3, 6, 32, 36 };
        for (int pin : float_pins) {
            gpio_config_t up = {};
            up.pin_bit_mask = 1ULL << pin;
            up.mode = GPIO_MODE_INPUT;
            up.pull_up_en = GPIO_PULLUP_ENABLE;
            gpio_config(&up);
            vTaskDelay(1);
            int hi = gpio_get_level((gpio_num_t)pin);
            up.pull_up_en = GPIO_PULLUP_DISABLE;
            up.pull_down_en = GPIO_PULLDOWN_ENABLE;
            gpio_config(&up);
            vTaskDelay(1);
            int lo = gpio_get_level((gpio_num_t)pin);
            ESP_LOGW(TAG, "  GPIO%d pull-test: up=%d down=%d -> %s", pin,
                     hi, lo,
                     (hi == 1 && lo == 0) ? "floating (not driven)"
                                          : "held (connected to something)");
        }

        /* v4: does the radio REACT to chip-select? A powered SX127x
         * drives MISO once NSS falls; if it does, CS+MISO+power are all
         * good and the fault is isolated to SCLK/MOSI socket contact
         * (chip inputs — invisible to pull tests). Proper RST pulse
         * first, then the select test, then one slow-clock read. */
        {
            s_hal->pinMode(BONNET_PIN_RST, GPIO_MODE_OUTPUT);
            s_hal->digitalWrite(BONNET_PIN_RST, 0); /* active-low reset */
            vTaskDelay(pdMS_TO_TICKS(2));
            s_hal->digitalWrite(BONNET_PIN_RST, 1);
            vTaskDelay(pdMS_TO_TICKS(10));

            gpio_config_t mi = {};
            mi.pin_bit_mask = 1ULL << BONNET_PIN_MISO;
            mi.mode = GPIO_MODE_INPUT;
            mi.pull_up_en = GPIO_PULLUP_ENABLE;
            gpio_config(&mi);
            s_hal->pinMode(BONNET_PIN_CS, GPIO_MODE_OUTPUT);

            s_hal->digitalWrite(BONNET_PIN_CS, 1);
            ets_delay_us(50);
            int miso_desel = gpio_get_level((gpio_num_t)BONNET_PIN_MISO);
            s_hal->digitalWrite(BONNET_PIN_CS, 0);
            ets_delay_us(50);
            int miso_sel = gpio_get_level((gpio_num_t)BONNET_PIN_MISO);
            s_hal->digitalWrite(BONNET_PIN_CS, 1);
            ESP_LOGW(TAG, "  CS-response: MISO deselected=%d selected=%d"
                          " -> %s",
                     miso_desel, miso_sel,
                     (miso_desel == 1 && miso_sel == 0)
                         ? "RADIO REACTS TO CS (fault = SCLK/MOSI contact)"
                     : (miso_desel == 1 && miso_sel == 1)
                         ? "no reaction (CS trace or chip)"
                         : "MISO stuck low");

            /* slow-clock RegVersion after clean reset (20us edges) */
            uint8_t addr = 0x42 & 0x7F, val = 0;
            s_hal->pinMode(BONNET_PIN_SCLK, GPIO_MODE_OUTPUT);
            s_hal->pinMode(BONNET_PIN_MOSI, GPIO_MODE_OUTPUT);
            s_hal->digitalWrite(BONNET_PIN_SCLK, 0);
            s_hal->digitalWrite(BONNET_PIN_CS, 0);
            ets_delay_us(20);
            for (int b = 7; b >= 0; b--) {
                s_hal->digitalWrite(BONNET_PIN_MOSI, (addr >> b) & 1);
                ets_delay_us(20);
                s_hal->digitalWrite(BONNET_PIN_SCLK, 1);
                ets_delay_us(20);
                s_hal->digitalWrite(BONNET_PIN_SCLK, 0);
            }
            for (int b = 7; b >= 0; b--) {
                ets_delay_us(20);
                s_hal->digitalWrite(BONNET_PIN_SCLK, 1);
                ets_delay_us(20);
                val = (uint8_t)((val << 1) |
                                gpio_get_level((gpio_num_t)BONNET_PIN_MISO));
                s_hal->digitalWrite(BONNET_PIN_SCLK, 0);
            }
            s_hal->digitalWrite(BONNET_PIN_CS, 1);
            ESP_LOGW(TAG, "  slow read after reset: RegVersion=0x%02X %s",
                     val, val == 0x12 ? "<-- CHIP FOUND" : "");
        }
        return false;
    }

    s_radio = sx76;
    s_irq_pin = BONNET_PIN_DIO0;
    ESP_LOGI(TAG, "SX1276 bonnet up: %.1f MHz SF%d BW%.0f",
             BONNET_FREQ_MHZ, BONNET_SF, BONNET_BW_KHZ);
    } /* end bonnet fallback */

#if VMESH_HAT_INIT_STAGE2
    /* BISECT stage 2: begin() completed (chip configured, standby) but
     * NO ISR, NO task, NO startReceive. */
    ESP_LOGW(TAG, "INIT_STAGE2: begin() only, radio in standby");
    return false;
#endif

    const mesh_config_t mesh_cfg = {
        .jitter_min_ms = 100,
        .jitter_max_ms = 800,
        .suppress_threshold = 2,
    };
    mesh_init(&s_mesh, &mesh_cfg, (uint32_t)esp_timer_get_time());

    s_txq = xQueueCreate(8, sizeof(tx_item_t));
    xTaskCreate(radio_task, "bonnet_radio", 6144, nullptr, 12, &s_task);

    /* RX-done IRQ (DIO1 on SX1262, DIO0 on SX1276); route through our
     * ISR (not RadioLib's callback machinery) so we can notify the
     * task from IRQ context */
    gpio_isr_handler_add((gpio_num_t)s_irq_pin, dio0_isr, nullptr);
    gpio_set_intr_type((gpio_num_t)s_irq_pin, GPIO_INTR_POSEDGE);
    gpio_intr_enable((gpio_num_t)s_irq_pin);

    s_radio->startReceive();
    vmesh_feed_set_tx_hook(bonnet_tx_hook);
    radio_bonnet_active = true;

    ESP_LOGI(TAG, "raw-SPI mesh transport active");
    return true;
}
