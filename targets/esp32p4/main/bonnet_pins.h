/**
 * bonnet_pins.h — which ESP32-P4 GPIOs sit under the Adafruit LoRa
 * Radio Bonnet (#4284) when stacked on the dev kit's 40-pin header.
 *
 * The bonnet's fixed PHYSICAL header pins (Pi numbering):
 *   SCLK=23  MOSI=19  MISO=21  CS=26  RST=22  DIO0=15
 *
 * Mapped July 2026 from the official board drawing (files.waveshare
 * .com ESP32-P4-Module-DEV-KIT.pdf); no RMII/Ethernet conflicts.
 * Previously: FILL THESE IN from the schematic (which P4
 * GPIO is routed to each physical pin) — and check none is claimed by
 * the Ethernet PHY. Until then they are -1 and the radio driver
 * politely refuses to start.
 */
#pragma once

#define BONNET_PIN_SCLK (0)  /* phys 23 */
#define BONNET_PIN_MOSI (3)  /* phys 19 */
#define BONNET_PIN_MISO (2)  /* phys 21 */
#define BONNET_PIN_CS   (32) /* phys 26 */
#define BONNET_PIN_RST  (1)  /* phys 22 */
#define BONNET_PIN_DIO0 (6)  /* phys 15 */

/* radio profile — MUST match the Pi peer nodes */
#define BONNET_FREQ_MHZ   915.0f
#define BONNET_BW_KHZ     125.0f
#define BONNET_SF         9
#define BONNET_CR         5      /* 4/5 */
#define BONNET_SYNC_WORD  0x12   /* private network */
#define BONNET_POWER_DBM  17     /* RFM95 PA_BOOST headroom */
#define BONNET_PREAMBLE   8
