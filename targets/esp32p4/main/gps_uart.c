/* gps_uart.c — L76K GNSS on the SX1262 LoRaWAN/GNSS HAT: NMEA 9600
 * on the header UART pins (P4 GPIO37/38, freed from console duty by
 * CONFIG_ESP_CONSOLE_USB_SERIAL_JTAG — see hat_pins.h for the
 * USB-TO-UART bridge contention caveat).
 *
 * Same seam as the USB G-mouse path: valid RMC fix -> live pose.
 * Harmless if no HAT is fitted (the UART just stays silent). */

#include "driver/uart.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

#include "feed.h"
#include "hat_pins.h"
#include "nmea.h"

static const char *TAG = "vmesh-gps";

#define GPS_UART UART_NUM_1

static void gps_task(void *arg)
{
    (void)arg;
    nmea_parser_t parser = { 0 };
    uint8_t buf[256];
    uint32_t fixes = 0, last_log = 0;

    for (;;) {
        int n = uart_read_bytes(GPS_UART, buf, sizeof(buf),
                                pdMS_TO_TICKS(200));
        for (int i = 0; i < n; i++) {
            nmea_fix_t fix;
            bool rmc = nmea_feed_char(&parser, (char)buf[i], &fix);
            if (rmc && !fix.valid)
                vmesh_gps_state_set(1); /* talking, hunting for sky */
            if (rmc && fix.valid) {
                vmesh_pose_t pose = {
                    .lat = fix.lat,
                    .lon = fix.lon,
                    .heading_deg = fix.course_deg,
                    .speed_mps = fix.speed_mps,
                };
                vmesh_pose_set_live(&pose);
                fixes++;
                uint32_t now = (uint32_t)(xTaskGetTickCount() *
                                          portTICK_PERIOD_MS);
                if (now - last_log > 30000) {
                    ESP_LOGI(TAG, "L76K fix %.5f,%.5f (%lu fixes)",
                             fix.lat, fix.lon, (unsigned long)fixes);
                    last_log = now;
                }
            }
        }
    }
}

void gps_uart_start(void)
{
    uart_config_t cfg = {
        .baud_rate = HAT_GNSS_BAUD,
        .data_bits = UART_DATA_8_BITS,
        .parity = UART_PARITY_DISABLE,
        .stop_bits = UART_STOP_BITS_1,
        .flow_ctrl = UART_HW_FLOWCTRL_DISABLE,
        .source_clk = UART_SCLK_DEFAULT,
    };
    if (uart_driver_install(GPS_UART, 2048, 0, 0, NULL, 0) != ESP_OK ||
        uart_param_config(GPS_UART, &cfg) != ESP_OK ||
        uart_set_pin(GPS_UART, UART_PIN_NO_CHANGE, HAT_GNSS_UART_RX,
                     UART_PIN_NO_CHANGE, UART_PIN_NO_CHANGE) != ESP_OK) {
        ESP_LOGW(TAG, "UART setup failed — HAT GNSS idle");
        return;
    }
    xTaskCreate(gps_task, "hat_gps", 4096, NULL, 5, NULL);
    ESP_LOGI(TAG, "listening for L76K NMEA on UART1 rx=%d @ %d",
             HAT_GNSS_UART_RX, HAT_GNSS_BAUD);
}
