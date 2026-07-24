# Vehicular LoRa Mesh — Project Handoff & Technical Spec

**Status:** Pre-implementation design brief for a coding session
**Author context:** Solo developer with prior DePIN-over-LoRa experience and a Meshtastic fork. Comfortable with embedded systems, LoRa PHY, and mesh protocol design.
**Purpose of this document:** Hand off the agreed design decisions and open questions to an AI coding session so it can begin scaffolding firmware and tooling.

---

## 1. What we're building

An **open-source, vehicle-focused local data mesh** — conceptually "Meshtastic for cars," but purpose-built rather than a fork.

Vehicles carry a dedicated device that **broadcasts and relays small, perishable, geographically-scoped messages** — traffic conditions, road hazards, and hyperlocal information — to nearby vehicles over LoRa. The value proposition is that the information is **hyperlocal and immediate**, delivered peer-to-peer with no cloud, no cellular, and no infrastructure dependency.

This is **not** a safety-critical V2V system (that's the domain of DSRC/C-V2X). It is an informational, best-effort, human-facing hazard/traffic awareness layer.

### Explicit non-goals
- No cooperative perception, no automated vehicle control, no sub-second safety guarantees.
- No cellular connectivity of any kind (this is a hard design constraint — see §3).
- No token/crypto/incentive layer. The DePIN economic machinery is deliberately dropped. Value is the local information itself.
- No guaranteed delivery, no ACKs, no long multi-hop routing to specific distant nodes.

---

## 2. Core design principle: geo-ephemeral flooding

The central architectural insight: **traffic/hazard data is tiny, perishable, local, and loss-tolerant.** This lets us discard traditional mesh routing entirely.

Meshtastic's managed-flood routing exists to deliver a message to a *specific distant node*. We have no such requirement. We have: *"everyone within N km / M hops of this event should hear about it, for the next few minutes."* That is **geocasting**, and it's a simpler and more mobility-robust primitive.

### Message structure (conceptual — see §7 for schema work)
Every message carries:
- **Origin geographic point** (lat/lon of the event)
- **Relevance radius** and/or **hop/TTL limit**
- **Timestamp / expiry** (when the info stops being useful)
- **Dedup ID** (origin ID + sequence, or a hash)
- **Payload** (hazard type, severity, short text — tens of bytes)

### Relay logic (the whole forwarding decision)
On receiving a message:
1. Have I already seen this dedup ID? → drop.
2. Is it still fresh (not past expiry)? → if no, drop.
3. Am I still within its relevance radius of the origin point? → if no, drop.
4. Otherwise: rebroadcast **once**, then remember the dedup ID.

Nodes moving away from an event naturally fall outside the relevance radius and stop relaying — the flood is **self-limiting** with no route state to maintain. Contact windows of only a few seconds (the reality at highway closing speeds) are fine, because a single successful rebroadcast is all that's needed.

### Why this beats a naive Meshtastic fork here
- No routing tables to churn as topology reshuffles every few seconds.
- No delivery guarantees to maintain under extreme mobility.
- Graceful degradation: a dropped beacon is replaced by the next one seconds later.

---

## 3. The "provably off-grid" positioning (a first-class design constraint)

This is a **trust and positioning argument**, not just an engineering one, and it drives hardware decisions.

A phone app *claims* to be local-only but can't prove it — users have no way to verify it isn't silently sending data to a server. A dedicated device with a **known, auditable, cellular-free radio set** makes "off-grid" a **physical fact**: there is no hardware path to exfiltrate data or route it through carrier infrastructure.

### Rules that protect this claim
- **No cellular modem, ever. No SIM slot.** The moment cellular hardware exists, the entire argument collapses.
- **Wi-Fi is scoped narrowly and visibly.** Wi-Fi is used *only* for local map-tile loading and OTA firmware updates, over a network the user explicitly connects to. It is **not** a silent backhaul. Strongly consider making Wi-Fi **default-off**, user-enabled only for updates, so the device's resting state is LoRa + BLE only.
- **BLE is the phone tether and nothing else.** The phone provides optional map/UI convenience; the device must work fully standalone. The phone must never become a connectivity backdoor.
- **Open, inspectable firmware is part of the claim.** Cellular-free hardware earns the physical claim; open firmware earns the "and it's not doing anything sneaky with the radios it *does* have" claim. Both are needed. A closed-firmware off-grid box invites the same suspicion as an app.
- **Consider a radio-state indicator in the UI** ("radios active: LoRa, BLE") so users can *watch* the off-grid guarantee hold.

### Product framing
Not "a cheaper Garmin." Rather: *a road-information network that physically cannot surveil you and cannot be shut off by an outage.* Regular people understand this instantly, especially post-disaster when cell networks fail. Meshtastic proved this audience exists and will pay.

---

## 4. Hardware platform (on hand)

**Board:** Waveshare ESP32-P4-Module-DEV-KIT (with DSI capacitive touch display).

### Key facts about this board (corrects earlier assumptions)
- **ESP32-P4** is the **application processor**: RISC-V dual-core, up to 32MB PSRAM (this module has 32MB in-package), 16MB onboard NOR flash. Includes a **hardware JPEG codec, Pixel Processing Accelerator (PPA), and MIPI-DSI display interface** — genuinely well-suited to smooth map rendering, better than an ESP32-S3 for this purpose.
- **ESP32-C6** is a **companion radio chip** providing **Wi-Fi 6 + Bluetooth 5/BLE**, connected to the P4 over SDIO. **The C6 is the Wi-Fi/BLE provider — it is NOT the LoRa MCU.**
- Display: MIPI-DSI IPS capacitive touch panel (kit ships with 7" 720×1280 or 10.1" 800×1280, GT9271 touch, 10-point).
- **TF card slot** over SDIO 3.0 — this is the map-tile store.
- Exposes SPI, UART, I2C, I3C, plus a 40-pin GPIO header (28 free GPIOs).
- Security features usable for signed OTA: Secure Boot, Flash Encryption, crypto accelerators, TRNG, Digital Signature Peripheral, Key Management Unit.

### What the board does NOT have (must be added externally)
- **No LoRa radio.** Add an SX1262-class module via SPI on the 40-pin header.
- **No GPS/GNSS.** Add a GPS module (u-blox NEO-class or better) via UART.
- **No battery/charge circuit** in the base dev kit — add for a portable prototype.

### Off-grid audit note
On this board the only onboard radios are Wi-Fi 6 and BLE (both from the C6). Plus whatever LoRa module is attached. **No cellular anywhere** — the §3 claim holds cleanly on this hardware.

### Platform caveat for the coder
The **P4 + MIPI-DSI + PPA** graphics stack is newer and less battle-tested than the mature ESP32-S3 + LVGL + LovyanGFX ecosystem. Display bring-up should follow **Espressif's ESP-IDF BSP for this specific board and LVGL's DSI path**, plus **Waveshare's P4 example code / Wiki** — NOT the LovyanGFX setup that S3 projects use. See §6 for what to reuse from prior art vs. what's board-specific.

---

## 5. Recommended system architecture

### Processor / task division
- **ESP32-P4 (app processor):** LVGL UI, map rendering (leveraging PPA + JPEG codec), touch input, GPS parsing, TF-card tile access, BLE tether logic, hazard overlay rendering, OTA. Runs under ESP-IDF + FreeRTOS with separate tasks for UI render, GPS parse, and mesh I/O so no task blocks another.
- **ESP32-C6 (companion):** Wi-Fi 6 + BLE transport, driven by the P4 over SDIO via ESP-IDF's hosted/co-processor model.
- **External LoRa module (SX1262):** the mesh radio, on SPI. **Decision point below.**

### Open architecture decision: where does the mesh stack run?
Two options — flag this for early resolution:

- **(A) Mesh stack on the P4 itself.** SX1262 driver + geo-ephemeral flooding run as FreeRTOS tasks on the P4 alongside LVGL. Simpler BOM, fewer parts. Risk: LoRa RX-window / airtime timing is sensitive, and a heavy map pan or SD tile-load could introduce jitter. Mitigate with task priorities and by keeping the SX1262 on a dedicated SPI bus + interrupt.
- **(B) Dedicated mesh co-MCU.** A cheap separate MCU (e.g. an RP2040 or a second ESP32-C-class chip) owns the SX1262 and runs the mesh firmware in isolation, talking to the P4 over UART/SPI with a small versioned message protocol. Benefits: hard radio-timing isolation; a **small, self-contained, easily-auditable** mesh firmware (reinforces §3); independent update cadence; reusable as a headless node module later.

**Recommendation:** Start with (A) for the fastest demoable prototype, but **design the mesh layer behind a clean transport interface** so it can be lifted onto a co-MCU (B) later without touching the app logic. The inter-processor message set (§7) should be identical whether the boundary is intra-chip or across a UART — this is the key to keeping the option open.

---

## 6. Prior art to reuse (don't build from scratch)

The navigation/display layer is largely a solved problem. Your novel work is the mesh + hazard layer on top. Reuse aggressively:

- **IceNav-v3** (`github.com/jgauchia/IceNav-v3`) — ESP32 GPS navigator with OSM offline maps, supports both raster tiles and a newer vectorized format. **Use its architecture and map logic as reference.** Note: it targets ESP32-S3 with LovyanGFX, so its *low-level display driver* won't port to the P4's DSI panel — but its map-handling, tile-cache, and nav structure are the reference model.
  - Related: its GOL/GeoDesk-based **vector map format + tile-generator repo** (`github.com/jgauchia/Tile-Generator`) — potential path for a future vector renderer, so you don't write one from scratch.
- **ESP32 map-tiles library** (LVGL 9.x, RGB565 256×256 tiles from SD, smooth pan + multi-level zoom, custom tile providers) — a drop-in **raster fast-path**.
- **Espressif ESP-IDF BSP for ESP32-P4** + **LVGL DSI examples** + **Waveshare ESP32-P4-Module-DEV-KIT Wiki/sample code** — the board-specific display/touch bring-up path.

### Map rendering strategy: **raster first**
- **v1 = raster tiles.** Pre-render OSM tiles server-side, cache PNG/RGB565 tiles on the TF card, blit the visible tiles. Minimal CPU, predictable RAM, fast to ship. Standard pipeline: OSM → QGIS/GDAL → MBTiles via tippecanoe → PNG tiles at **zoom 10–16** (road-detail sweet spot). Reference point: a whole small country ≈ ~11MB of tiles.
- **Design the nav/render layer renderer-agnostic** so vector rendering can slot in as v2 without touching the mesh or message layers.
- Hazards render as an **LVGL overlay layer** on top of the base map — trivial compared to the map itself.

### Reuse from the author's Meshtastic experience
- Reuse: LoRa PHY driver patterns, OTA update mechanism, PSK channel encryption, BLE-to-phone bridge pattern, hardware abstraction habits.
- Rebuild: the routing/forwarding layer (managed-flood → geo-ephemeral flooding), the message schema (geo + expiry as first-class fields), and the app-side UX (a moving hazard map, not a chat thread).

---

## 7. Highest-leverage early work: the data model

Get the **message schema** right before anything else — it's the seam that unifies three boundaries:
1. LoRa-over-the-air messages (between vehicles)
2. Mesh-processor ↔ app-processor messages (intra-P4 or across a UART to a co-MCU)
3. Device ↔ phone messages over BLE

All three should be **dialects of one message model.** Define it once, versioned, compact (this is going over LoRa — every byte counts).

### Schema must include (at minimum)
- Origin node ID + sequence number (→ dedup ID)
- Message type (position beacon / hazard / traffic / free-text)
- Geographic origin (lat/lon, compact fixed-point encoding)
- Relevance radius and/or hop TTL
- Creation timestamp + expiry
- Severity / category enum for hazards
- Optional short payload
- Version field (protocol evolution)

---

## 8. The real hard problem: airtime / congestion control

**Routing is not the enemy — LoRa channel saturation is.** On a busy road with hundreds of nodes beaconing position and relaying hazards, the channel saturates and collisions cascade. This is where the genuinely novel engineering effort belongs, and where the project differentiates from a naive fork.

Design focus areas:
- **Aggressive dedup + suppression:** if a hazard has already been heard rebroadcast by ≥N neighbors, stay silent (counter-based / probabilistic flooding, à la "gossip"/broadcast-storm mitigation).
- **Adaptive beacon rate:** beacon position rarely when nothing is happening; increase density only near active events.
- **Spreading-factor / channel planning:** lower SF for dense short-range urban; reserve higher SF for genuine range needs.
- **Regional profile:** first build is **US915 (FCC Part 15)** — no legal duty-cycle cap, so congestion control is capacity/LBT-driven. Region-configurable so EU868 (with its 1% duty-cycle enforcement) can be added later.

---

## 9. Suggested v0.1 build path

1. **Board bring-up:** flash ESP-IDF, get the DSI touch panel + LVGL rendering via Espressif/Waveshare P4 examples.
2. **Raster map on screen:** integrate the map-tiles library (or IceNav's tile logic adapted), tiles from TF card, no GPS yet — hardcode a location and confirm smooth pan/zoom.
3. **GPS in:** attach the NEO-M8N over UART, parse NMEA (TinyGPS++), move the map with real position.
4. **LoRa in:** attach the SX1262 (Core1262-HF) over SPI via RadioLib, US915; get raw TX/RX of the message schema between two units.
5. **Mesh logic:** implement geo-ephemeral flooding + dedup + expiry; validate self-limiting flood with 3+ units.
6. **Hazard overlay:** render received hazards as an LVGL layer on the map; add a minimal "report hazard" touch flow.
7. **BLE tether:** expose the message model to a phone over BLE (default-off Wi-Fi, per §3).
8. **Congestion control:** layer in suppression / adaptive beaconing; stress-test with many simulated nodes.
9. **OTA + signed firmware:** using the P4's secure-boot / DS peripheral.

Throughout: keep the mesh layer behind a clean transport interface (§5) so it can migrate to a co-MCU later.

---

## 10. Locked hardware choices (US / first test build)

These are decided for the initial USA test build. All are config-profiled so other regions/parts can be added later without rework.

### LoRa radio — Semtech SX1262 @ 915 MHz, SPI, with TCXO
- **Chip:** SX1262 (current-gen; more efficient and longer-range than SX127x; matches prior Meshtastic experience).
- **Band:** **US915** (902–928 MHz ISM), for FCC Part 15.247 operation.
- **Module:** a **bare SPI SX1262 breakout that exposes raw LoRa** — e.g. **Waveshare Core1262-HF** or equivalent exposing NSS/SCK/MOSI/MISO + BUSY + DIO1 + RESET + (TXEN/RXEN if present).
  - **Pick a module with a TCXO**, not a plain crystal — a dashboard device sees large temperature swings and needs stable RX frequency. Core1262 uses a TCXO.
- **IMPORTANT — avoid "LoRa HAT" style modules** (e.g. the UART "SX1262 915M LoRa HAT"). Those hide the PHY behind a UART serial module running a *private point-to-point protocol* and do **not** expose raw LoRa. Geo-ephemeral flooding requires **direct SPI register access to the SX1262**, so a bare SPI breakout is mandatory.
- **Driver:** use **RadioLib** (mature ESP-IDF/Arduino SX1262 support) rather than hand-rolling register code. Core1262 also ships STM32/Pico reference code.
- **SX1262 wiring/protocol facts for the coder:**
  - SPI max **18 MHz**.
  - **Poll the BUSY pin before every register read/write** (low = idle/OK, high = busy).
  - **DIO1** = primary interrupt line (RX-done / TX-done).
  - **RESET** pulled low ~100 µs to restore defaults; high in normal operation.
  - If the module exposes RXEN/TXEN RF-switch control, wire per its datasheet.
- **Antenna:** 915 MHz antenna suited to vehicular mounting (mag-mount CB-style or external whip preferred over a tiny PCB antenna for real range).

### GPS — u-blox NEO-M8N over UART (NMEA)
- **Default:** **u-blox NEO-M8N** — multi-GNSS (GPS + GLONASS + Galileo + BeiDou), cheap, ubiquitous, strong library support, NMEA over UART (9600 or 38400 baud).
- **Parser:** TinyGPS++ (or equivalent NMEA parser).
- **Upgrade option:** **NEO-M9N** for faster update rate and better multipath/city-canyon handling in dense urban traffic. Drop-in from a firmware perspective.
- **Keep GPS and LoRa as separate modules** — do not use a bundled LoRa+GNSS HAT, which tends to wrap both behind the UART/private-protocol layer and constrains the raw-SPI LoRa access you need.

### Region / regulatory profile — US915 (FCC Part 15)
- **Profile: US915.** 902–928 MHz. **No EU-style 1% duty-cycle cap** (that is an EU868 constraint).
- Under FCC Part 15, congestion control is therefore driven by **channel capacity + politeness (listen-before-talk / LBT / carrier-sense), not a legal airtime budget.** This is the correct engineering framing regardless (see §8).
- Make **region a config profile** so **EU868** (with duty-cycle enforcement) and others can be added later without touching core logic.

### Still open (lower priority, non-blocking for firmware start)
- **Mesh on P4 vs. dedicated co-MCU** (§5) — start on P4, keep the transport seam clean.
- **Battery/power** — portable prototype (LiPo + charger) vs. 12V-vehicle-powered for on-road testing.
- **Screen size** for a shippable unit — the dev kit's 7"/10.1" panel is large for a dashboard; a production unit likely wants smaller. Not blocking for bring-up.
- **Message schema v1 encoding** (§7) — still the top *software* priority to nail first.
