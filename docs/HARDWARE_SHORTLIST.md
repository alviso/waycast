# Hardware shortlist — LoRa + GPS for the P4 dev kit (July 2026)

> **CHOSEN FOR THE PROTOTYPE (rev 2, July 2026): the Adafruit LoRa
> Radio Bonnet already on hand (§0a) — raw SPI AND solderless AND $0.**
> The USB path (§0b) stays as the GPS plan + radio fallback; the
> soldered/SPI path below remains the reference for the real product.

## 0a. Bonnet path — Adafruit LoRa Radio Bonnet #4284 (RFM95W, 915 MHz)

User has several on Raspberry Pis; the P4 dev kit's 40-pin header is
Pi-compatible, so the bonnet **stacks straight on**. This beats both
other paths: raw SX1276-class PHY (spec §10 satisfied — CAD, sync
words, airtime control all available), no soldering, no purchase, and
the Pi+bonnet fleet doubles as **ready-made mesh peer nodes** (a small
Python script speaking vmesh_wire makes each Pi a full node —
multi-node flood testing with zero new hardware).

Bonnet pinout (Pi GPIO → physical header pin):
| Signal | Pi GPIO | phys |
|---|---|---|
| SPI SCLK / MOSI / MISO | 11 / 10 / 9 | 23 / 19 / 21 |
| Radio CS (CE1) | 7 | 26 |
| Radio RST | 25 | 22 |
| Radio DIO0 (IRQ) | 22 | 15 |
| OLED I2C (bonus debug display) | 2 / 3 | 3 / 5 |
| Buttons ×3 (bonus: physical Report/Confirm!) | 5 / 6 / 12 | 29 / 31 / 32 |

**Bench checks before first use** (5 minutes with the kit schematic):
1. Which P4 GPIOs sit on physical pins 15/19/21/22/23/26 of THIS kit,
   and that none are claimed by the Ethernet PHY → fill in
   `targets/esp32p4/main/bonnet_pins.h`.
2. The RFM95W sticker says 915 MHz (RadioFruit 868/915 variant).

Driver: RadioLib (registry component) with a custom ESP-IDF HAL —
SX1276 and SX1262 sit behind the same RadioLib API, so the eventual
product-path SX1262 swap is a constructor change.

Notes: no GNSS on the bonnet — GPS stays on the USB G-mouse plan
(§0b). SX1276 max +20 dBm via PA_BOOST; we run +17 for headroom.

## 0c. Waveshare SX1262 868/915M LoRaWAN/GNSS HAT (~$30) — STRONG CANDIDATE (July 2026)

wiki: SX1262_XXXM_LoRaWAN/GNSS_HAT. Despite the LoRaWAN name it is a
**bare SX1262 on raw SPI** (CS/MOSI/MISO/CLK + BUSY + DIO1 + RESET,
18 MHz max — no AT MCU) → satisfies spec §10, unlike the USB dongles.
GNSS variant adds an **L76K (GPS+BD, NMEA)** on the Pi UART pins +
ML1220 hot-start battery. Radio + GPS, one solderless HAT, stacks on
the P4 kit AND on Pis (instant mesh peers, simpler than RAK2287).

Pin map (verified against the kit board drawing, July 2026):
| HAT | BCM | phys | P4 GPIO |
|---|---|---|---|
| SCLK/MOSI/MISO | 11/10/9 | 23/19/21 | 0 / 3 / 2 (same as bonnet) |
| CS (CE0) | 8 | 24 | 36 |
| RESET | 18 | 12 | 22 |
| BUSY | 20 | 38 | 27 |
| DIO1 | 16 | 36 | 46 |
| TXEN | 6 | 31 | 26 |
| GNSS UART | 14/15 | 8/10 | 37/38 — kit console UART0! |

Notes / VERIFY-ON-HW:
- Console must move to USB-Serial-JTAG (one sdkconfig line; that's
  the port we log from anyway) to free UART0 for the L76K.
- RXEN is internally driven by DIO2; only TXEN needs a GPIO
  (RadioLib setRfSwitchPins / DIO2-as-RF-switch config).
- TCXO not stated on wiki; module footprint = Core1262-HF (has one);
  their driver supports DIO3-TCXO. Confirm at first begin().
- MUST order the 868/915M + GNSS variant (LF and GNSS-less exist).
- SX1262 = same family as the USB DTU dongles → one PHY profile,
  full interop; and the §10 product-path chip.

## 0d. Waveshare ESP32-P4-WIFI6-Touch-LCD-7/8/10.1 (enclosed, battery) — SECOND-NODE CANDIDATE (July 2026)

Tablet-style enclosed P4 devices (SKU 30738=7", 33673=8", 33672=10.1")
with the feature the dev kit lacks: **JST battery header + onboard
charging + power switch + charge LED**. Same architecture as our kit
(P4NRW32 32MB PSRAM, C6 Wi-Fi over SDIO/esp_hosted, MIPI-DSI, TF
slot) -> firmware ports with a BSP/panel swap; all P4 lessons carry.
~2.5W measured draw -> ~6.5h on a 5Ah cell.

**Gotcha: the 40-pin header is NOT Pi-compatible** (pin1=GPIO48,
pin2=GND, USB DP/DM + I2C GPIO7/8 + UART GPIO37/38 mixed in) — the
GNSS HAT (§0c) must NOT be stacked on it. Attach radio/GPS via:
1. ~10-wire harness from header GPIOs to the HAT's edge breakout
   pads (GPIO matrix routes SPI/UART anywhere; flat-mount the HAT
   behind the board inside the case), or
2. USB DTU dongle through the USB-C OTG port (C-to-A adapter).
Case needs antenna exits (SMA drill or printed back).

Role: NOT a dev-kit replacement — it's the enclosed battery unit and
the **second P4 node** (two screens + LoRa = first honest mesh demo).

## 0b-RESULT — USB path VALIDATED July 18 2026 (over-the-air mesh works)

First OTA mesh confirmed: P4 + USB LoRa dongle ↔ Mac + USB LoRa dongle,
bidirectional, real 915 MHz. Full Tier 0–2½ stack (beacon/relay/query/
reply) over the air. Dongle = WCH **CH343** (VID 0x1A86 PID 0x55D3),
plain CDC — the DTU AT profile (SF9/BW125/CH10) worked as written.

**Gotcha that cost an evening — the USB mux:** the P4's single USB
routes through an FSUSB42UMX mux (U15) selected by the H3 HOST/DEVICE
jumper. One position → CH334F **HS hub** → 4 USB-A ports; other →
**direct** to the USBHSD1 socket on J8. ESP-IDF USB host has **no
Transaction-Translator support**, so a Full-Speed dongle behind the HS
hub is rejected ("TT not supported", port disabled). The FS CH343
dongle works ONLY via the mux DIRECT path + the USBHSD1 socket (root
port). Tape-mark that port. Symptom of wrong mux position: dongle
PWR LED on (VBUS) but zero USB enumeration.

## 0b. USB path (GPS plan + radio fallback)

**No soldering; everything plugs into the dev kit's 4× USB-A host
ports** (CH334F hub on the P4's native USB 2.0 HS OTG).

### Radio: Waveshare USB-TO-LoRa-HF (SX1262, TCXO)  ×3, ~$25 ea
- AT-configurable **SF 7–12, BW 125/250/500 kHz, power 10–22 dBm,
  channels 0–80**; per-packet **RSSI output** (`AT+RSSI=1`); **LBT**
  supported; stream/packet/relay firmware modes. Use *packet/stream*
  mode and carry our `vmesh_wire` frames as opaque payload.
- **Why 3:** one on the device, one on the Mac (the simulator gains a
  REAL radio backend through the same feed seam — a laptop becomes a
  mesh node), one as roving peer/sniffer. Multi-node flood testing
  without a second dev kit.
- **What this path genuinely loses vs. raw SPI** (accepted for the
  prototype, §10 stands for the product): CAD-level channel sensing,
  sync-word control, preamble tricks, precise airtime accounting,
  deterministic TX timing (USB-serial jitter), per-packet SF agility —
  and the firmware is closed (§3 trust story requires the raw-SPI
  path eventually). **App-level geo-ephemeral flooding (dedup, TTL,
  radius, rebroadcast decisions) is unaffected** — it lives in our
  layer either way.
- Firmware task on the P4: USB Host + serial driver from
  `espressif/esp-usb` (CH34x VCP / CDC-ACM class); the feed seam
  gets its first real transport.

### GPS: u-blox USB "G-mouse" (VK-162 class, ~$15) or GlobalSat BU-353N (~$35)
- Plugs into a USB-A port; enumerates as CDC serial, speaks plain
  NMEA — the pose provider swaps from scenario to a USB CDC stream.
  Magnetic/dash-mount housings with integral patch antennas: the
  antenna problem solves itself for free.

### Notes
- Both dongles on the Mac too → the SDL simulator can grow the same
  USB radio/GPS backends; sim-vs-device parity extends to hardware.
- Power: dongles add ~150 mA worst case — trivial next to the panel.
- The §10 raw-SPI requirement is **deferred, not repealed**: mesh
  airtime/congestion engineering (§8) ultimately needs register-level
  control; this path is for proving product practicality first.


Constraints locked by the spec (§10): SX1262-class radio, **bare SPI**
(no UART "HAT" protocol wrappers), **TCXO mandatory** (dashboard
temperature swings), US915; GPS = NMEA over UART, kept separate from
the radio. This doc picks concrete parts within those constraints and
looks ahead to an enclosure.

## LoRa radio

### Primary: Waveshare Core1262-HF  (~$13)
- SX1262, 850–930 MHz, **TCXO on board**, +22 dBm, SPI up to 18 MHz.
- Castellated module; DIO2 drives the RF switch *on-module*, so wiring
  is just NSS/SCK/MOSI/MISO + BUSY + DIO1 + RESET + 3V3/GND — the
  exact pin set the spec's driver notes assume. RadioLib supports it
  out of the box.
- Same vendor as the dev kit; the spec already named it; proven in the
  Meshtastic ecosystem.
- Antenna: SMA/IPEX (buy the HF = high-frequency variant, not 868-only
  branding — it covers 915).

### Alternate: Ebyte E22-900M22S  (~$9)
- SX1262 + TCXO, 14×20 mm SMD, u.FL, +22 dBm. Slightly smaller and
  cheaper; needs RXEN/TXEN wired (two more GPIOs) and a carrier board
  for prototyping. Fine choice if Core1262 availability slips.

### Range upgrade (later, not v1): Ebyte E22-900M30S
- Same family with a +30 dBm (1 W) PA — legal under FCC 15.247 and
  tempting for vehicle-to-vehicle range, but ~600 mA TX bursts and
  real heat in an enclosure. Decide after field range tests, not
  before; the mesh protocol must work at 22 dBm anyway.

### Also seen: Seeed Wio-SX1262 (~$10, 21×18 mm, u.FL)
- Nice module, but its B2B-connector/XIAO form factor optimizes for
  Seeed's own boards; no advantage here over Core1262.

## GPS

**Spec correction: the NEO-M8N is end-of-life.** u-blox's current
generation is the M10; recommendation updated accordingly.

### Primary: u-blox MAX-M10S on the SparkFun breakout  (~$45)
- Current-gen, 4-constellation concurrent (GPS/GLONASS/Galileo/BeiDou),
  ultra-low power (<25 mW tracking), strong urban-canyon behavior.
- SparkFun breakout: u.FL antenna input **with active-antenna bias**,
  UART broken out (NMEA — TinyGPS++/our parser unchanged), Qwiic I2C
  as a bonus, backup battery for hot starts.
- This is the "professional grade" pick; it also future-proofs the
  power budget for a battery-backed portable variant.

### Budget/runner: NEO-M9N breakout (GY-M9N-style, ~$20–25)
- Previous-but-recent gen, drop-in NMEA, widely available. Perfectly
  adequate; choose it if $45 for GPS feels heavy at prototype stage.

### Cheap-and-cheerful (bench only): ATGM336H (~$5)
- Fine for desk testing, not what you want judging city-canyon
  performance with.

## Antennas (the part that actually determines performance)

- **LoRa, in-car product**: external 915 MHz mag-mount whip on the
  roof (NMO or SMA mag base, ~$20) — per the spec, a real vehicular
  antenna beats any PCB antenna by a huge margin. For bench/portable:
  a 915 MHz stubby whip on an SMA bulkhead (~$8).
- **GPS**: 25×25 mm **active** ceramic patch with LNA+SAW
  (Taoglas AGGBP.25B class, or generic 28 dB units, ~$10–15), u.FL to
  the MAX-M10S. Lives on the dash or inside the enclosure's top face
  (RF-transparent plastic only — no metal above it).
- Adapters: u.FL→SMA bulkhead pigtails (~$5) so both antennas are
  external-connector-ready for the enclosure.

## Wiring to the dev kit's 40-pin header

28 free GPIOs; the P4's GPIO matrix routes any function to any pin, so
exact numbers are firmware config, not constraints:

| Function | Pins needed |
|---|---|
| SX1262 (Core1262) | SPI ×4 (NSS/SCK/MOSI/MISO) + BUSY + DIO1 + RESET = 7 GPIO |
| GPS | UART TX/RX (+ optional PPS) = 2–3 GPIO |
| Power | 3V3 + GND from header (both modules are 3.3 V logic — no level shifting) |

Keep the SX1262 on its **own SPI bus** (spec §5's timing-isolation
mitigation), not shared with anything chatty.

## Enclosure considerations (early notes)

- The 7" panel dictates the front face; everything else is thin.
  Target: panel + P4 board sandwich, modules on a small proto/carrier
  board on the 40-pin header, two SMA bulkheads (LoRa, GPS) on the
  rear/top edge.
- **TF slot access** (field finding, July 2026): on the dev kit the
  display ribbon cable blocks the SD slot — card swaps mean unseating
  the DSI ribbon, which is both annoying and a wear risk. The
  enclosure must expose the card slot (or relocate it via an extender)
  — and the product should treat card swaps as rare: region updates
  belong on Wi-Fi/OTA or an oversized multi-region card, not on
  fingers near a ribbon cable.
- **Separation**: keep the LoRa TX path away from the GPS antenna/LNA
  (opposite corners; the SAW-filtered active patch tolerates a lot,
  but don't make it work for nothing).
- **Power**: car 12 V → 5 V/3 A buck (USB-C PD trigger board or
  hardwired). **Measured July 2026** (MacBook battery-delta method):
  panel + P4 + SD running the demo ≈ 2.5 W ≈ 0.5 A at 5 V — half the
  earlier estimate. Add ~150 mA LoRa dongle TX + ~50 mA GPS: still
  under 0.75 A average. A 3 A buck is generous; only the 30 dBm PA
  question could change that. (Average only — TX bursts need an
  inline meter with peak-hold to characterize.)
- Production form factor stays an open §10 question (7" is big for a
  dash) — the enclosure around *this* kit is a field-test mule, and
  that's fine.

## Suggested order (one cart, ~$110)

| Part | Qty | ~Price |
|---|---|---|
| Waveshare Core1262-HF | 2 (two nodes = first mesh test!) | $26 |
| SparkFun MAX-M10S breakout | 1 | $45 |
| 25 mm active GPS patch (u.FL) | 1 | $12 |
| 915 MHz whip + SMA bulkhead pigtails | 2 + 2 | $20 |
| Proto/carrier board + headers | 1 | $8 |

Two radios on day one is deliberate: geo-ephemeral flooding can't be
tested with one node, and the second SX1262 can hang off any spare MCU
(or the second dev kit slot) as a packet sniffer/peer.
