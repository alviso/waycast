/**
 * hat_pins.h — Waveshare SX1262 868/915M LoRaWAN/GNSS HAT on the
 * ESP32-P4-Module-DEV-KIT 40-pin header (P6).
 *
 * DEFINITIVE map (July 19 2026, confirmed by Peter reading both vendor
 * schematics together). Critical gotcha: the HAT's "Raspberry
 * Interface" drawing and the P4's P6 drawing number the two header
 * columns with OPPOSITE left/right conventions — a naive overlay
 * mirrors odd/even and lands every signal one pin off. (That mirrored
 * map — SCK=32/MOSI=1/MISO=36, and "phys38/40 = GND" — cost two bench
 * sessions. Anchor: Pi phys1=3V3/phys2=5V vs P6 pin1=5V/pin2=3V3.)
 *
 *   HAT signal | Pi phys | P4 GPIO
 *   -----------+---------+--------
 *   CLK/SCK    |   23    | GPIO0
 *   MOSI       |   19    | GPIO3
 *   MISO       |   21    | GPIO2
 *   CS/NSS     |   40    | GPIO45
 *   BUSY       |   38    | GPIO27
 *   RST        |   12    | GPIO22
 *   DIO1/IRQ   |   36    | GPIO46
 *   DIO4/TXEN  |   31    | GPIO26  (unused: DIO2 drives the RF switch)
 *   GPS TXD    |   10    | GPIO38  (NMEA out -> P4 UART0 RX / console!)
 *   GPS RXD    |    8    | GPIO37  (<- P4 UART0 TX / console!)
 *
 * GPS shares the CONSOLE UART pins (37/38) — same constraint as the old
 * gps_uart plan: only claim them when VMESH_HAT_GPS=1 (logs then move
 * to USB-JTAG). Radio bring-up keeps the console on the bridge.
 */
#pragma once

#define HAT_PIN_SCLK (0)  /* Pi phys23 */
#define HAT_PIN_MOSI (3)  /* Pi phys19 */
#define HAT_PIN_MISO (2)  /* Pi phys21 */
#define HAT_PIN_CS   (45) /* Pi phys40 */
#define HAT_PIN_RST  (22) /* Pi phys12 */
#define HAT_PIN_BUSY (27) /* Pi phys38 */
#define HAT_PIN_DIO1 (46) /* Pi phys36 */
#define HAT_PIN_RXEN (-1)
#define HAT_PIN_TXEN (26) /* DIO4/TXEN = Pi phys31 -> GPIO26. The vendor
                           * demo drives THIS high for TX (never DIO2) —
                           * the PE4259 antenna switch hangs off it. */

/* FINAL (proven by the on-screen line sniffer + the HAT's activity
 * LEDs, July 21): the kit routes ONLY phys8 <-> pad 37 of the UART
 * header pair; phys10 (the L76K's TXD) reaches no P4 pad at all.
 * Fix: a jumper bridges phys8<->phys10 (adjacent pins; P5 pins 3-4
 * work too), so the GPS's NMEA rides the one routed line onto pad
 * 37. Console lives on USB-Serial-JTAG (pad 37 must be undriven).
 * RX ONLY claimed: we never command the GPS (it hears its own echo
 * through R23 and ignores it). */
#define HAT_GNSS_UART_RX (37) /* L76K TXD -> phys10 =jumper= phys8 -> pad 37 */
#define HAT_GNSS_UART_TX (-1) /* unused: RX-only */
#define HAT_GNSS_BAUD    (9600)

/* MATCH THE DONGLES' FACTORY PROFILE (temporary): the USB-TO-LoRa
 * dongles were never actually AT-configured — command mode needs a
 * leading "+++" we never sent, so they run factory CH18=868.125MHz,
 * SF7, BW125, stream mode. Until we land a verified +++/AT reconfig
 * to move everyone to 915MHz US ISM, the HAT speaks the factory
 * profile so the fleet interoperates. */
#define HAT_FREQ_MHZ   915.0f /* US ISM = DTU CH65 (850+65); dongles AT-set to CH65 */
#define HAT_BW_KHZ     125.0f
#define HAT_SF         7 /* DTU factory */
#define HAT_CR         5
#define HAT_SYNC_WORD  0x12
#define HAT_POWER_DBM  17 /* antennas confirmed attached */
#define HAT_PREAMBLE   8
