# OTA firmware updates

Status: **BUILT and field-proven July 24 2026** — first OTA ran
v0.4.1 → v0.4.2 on the bench device (download over home Wi-Fi, A/B slot
swap, rollback gate passed). Items 1–4 of the build order below are
done; remaining: companion-app delivery, auto-check-when-parked,
ed25519 manifest signing, Secure Boot v2 + anti-rollback (production).
Download flicker: SOLVED (v0.4.10, third attempt) — root cause was
flash-erase cache blackouts starving the DSI refresh (content-
independent; a fully static screen still flickered). Fix:
CONFIG_SPI_FLASH_AUTO_SUSPEND (erases suspend whenever the CPU needs
cache; stalls us-scale). Failed attempts, for the record:
LCD_DSI_ISR_CACHE_SAFE (bricked boot — requires all display callbacks
in IRAM; first live rollback save) and flush-rate reduction (wrong
theory — flicker was never render-correlated). A dark near-static
"Updating firmware" shroud remains as good UX during downloads.

OTA is the same architectural shape as offline maps: a heavy-ish payload
(~2 MB app image) that flows to the device over whatever **oasis** it can
reach — home Wi-Fi, the phone companion, an Anchor hotspot, HaLoWave —
and *never* a thing the device depends on to function. It reuses almost
all the plumbing already built for tile fetch (C6 Wi-Fi station mode,
`esp_http_client` + cert bundle, a configurable server base).

---

## 1. The blocker: our partition table has no OTA slots

Current layout (`targets/esp32p4/partitions.csv`) is single-app:

```
factory,   app,  factory, 0x20000,  3M
storage,   data, spiffs,  0x320000, 0xCE0000   # ~12.9 MB embedded tiles
```

OTA needs **two app slots** (A/B) plus an `otadata` partition that records
which slot boots. You cannot OTA into a `factory` partition. So step one
is a partition redesign — and it frees up naturally, because the SD-card
tileset (shipped July 2026) demoted the embedded SPIFFS tiles to a
fallback demo set (~5 MB actual, not 13 MB).

Proposed OTA-ready layout (16 MB flash; app is currently 1.97 MB, fits a
3 MB slot with headroom):

```
# Name,   Type, SubType,  Offset,    Size
nvs,      data, nvs,      0x11000,   0x6000
otadata,  data, ota,      0x17000,   0x2000    # 2 sectors, power-fail safe
phy_init, data, phy,      0x19000,   0x1000
ota_0,    app,  ota_0,    0x20000,   0x300000  # 3 MB
ota_1,    app,  ota_1,    0x320000,  0x300000  # 3 MB
storage,  data, spiffs,   0x620000,  0x9E0000  # ~9.9 MB fallback tiles
```

Note: **anti-rollback (below) forbids a `factory` partition entirely** —
the table must be ota_0/ota_1 only, which this is. A device flashed with
this table for the first time over USB lands in ota_0.

One-time cost: existing bench units need a full `idf.py flash` on the new
table once (the SD tiles survive — flashing never touches the card). After
that, every future update is OTA.

---

## 2. Device side (ESP-IDF)

Mechanism: **`esp_https_ota`** streams the new image straight into the
inactive slot, validates it, flips `otadata`, reboots. Config that makes
it safe for a device in a car we can't easily recover:

- **`CONFIG_BOOTLOADER_APP_ROLLBACK_ENABLE`** — a freshly-OTA'd app boots
  in a *pending-verify* state. It must call
  `esp_ota_mark_app_valid_cancel_rollback()` after passing a self-check
  (display up, Wi-Fi/mesh init OK); if it crashes or resets first, the
  bootloader automatically reverts to the previous slot. A bad flash
  **cannot brick the device.** This is non-negotiable for the field.
- **`CONFIG_BOOTLOADER_APP_ANTI_ROLLBACK`** (later) — eFuse security
  version prevents downgrading to an image with a known vulnerability.
  Heavier (burns eFuses, no going back), so Phase-later, not first.
- Version identity comes from `esp_app_get_description()->version` — set
  it from git via `PROJECT_VER` in the top CMakeLists so every build is
  self-describing.

Self-check gate (the app decides it's healthy):

```c
if (esp_ota_get_state_partition(running, &st) == ESP_OK
    && st == ESP_OTA_IMG_PENDING_VERIFY) {
    if (self_check_passed())  esp_ota_mark_app_valid_cancel_rollback();
    else                      esp_ota_mark_app_invalid_rollback_and_reboot();
}
```

**Car-safety policy — when the device is allowed to update:**
- never while moving (`speed_mps > 0` → defer);
- only parked, on a trusted oasis, with power ideally present;
- user consent by default (a Settings toggle can opt into silent
  auto-update when parked at a known network);
- the mesh keeps running throughout — OTA is a background download, and
  the actual swap happens on the next safe reboot.

---

## 3. Server side (fits the existing box)

Lives on the same Hetzner host as `tiles.waycast.io` / the site — a
static manifest + signed images behind Caddy. No new infrastructure.

```
GET  https://fw.waycast.io/manifest.json
GET  https://fw.waycast.io/vmesh-<version>.bin
```

`manifest.json` (the device polls this, compares to its running version):

```json
{
  "version": "0.4.0",
  "url": "https://fw.waycast.io/vmesh-0.4.0.bin",
  "size": 1998848,
  "sha256": "…",
  "min_from": "0.1.0",
  "hw": "esp32p4-devkit",
  "notes": "SF9 downlink, town beacon",
  "sig": "ed25519(base64 over the fields above)"
}
```

- **Integrity/authenticity — two layers.** App-layer: the device carries
  an ed25519 *public* key, verifies `sig` over the manifest, then verifies
  the downloaded image against `sha256`. Hardware layer (later): enable
  **Secure Boot v2** (the P4 SoC supports RSA and ECC — confirmed in
  sdkconfig) so the bootloader itself refuses an unsigned image. App-layer
  gets us safe delivery now; Secure Boot makes it tamper-proof for
  production. `min_from` blocks illegal jumps; `hw` guards against
  flashing the wrong board when we have more than one.
- **Channels:** `manifest.json` = stable; `manifest-beta.json` = the
  bench units, so we can dogfood without risking a fleet.
- **Build/publish:** a `scripts/publish_firmware.py` (to write) stamps the
  git version, uploads the `.bin`, signs and writes the manifest — the
  firmware equivalent of `fetch_area_tiles.py`.

---

## 4. Delivery modes (the oasis ladder, reused verbatim)

The image is ~2 MB — trivial next to a 187 MB tileset — so every oasis
already validated for tiles carries firmware for free, same transport:

1. **Home Wi-Fi (oasis #1):** device checks the manifest on boot / once a
   day, self-updates when parked. The direct path.
2. **Phone companion (oasis #0):** phone pulls the image over LTE, serves
   it to the device over local Wi-Fi — *exactly* the LAN-serve flow proven
   during the July 2026 tile seed (device C6 STA → local HTTP). The
   companion app is the natural place for "update available → install now
   / when parked."
3. **Anchor-relayed (future):** a town node with backhaul caches firmware
   and offers it to passing cars — the mesh updating itself, no user
   internet at all.

Discipline (same line as tiles and the HaLoWave call): **an oasis
enriches, never gates.** A device that never meets one runs its current
firmware forever; OTA just keeps it fresh when the world offers a pipe.

---

## 5. Build order when we pick this up

1. Partition-table redesign + one-time re-flash of bench units (unblocks
   everything; do this first, it's the irreversible-in-the-field bit).
2. `PROJECT_VER` from git; `esp_https_ota` + rollback self-check gate.
3. `fw.waycast.io` static host + `publish_firmware.py` + ed25519 signing.
4. Settings UI: "check for updates", channel, parked-auto-update toggle.
5. Companion-app delivery (folds into the companion-app effort).
6. (Later) Secure Boot v2 + anti-rollback for production hardware.

Reuses from tile fetch: Wi-Fi STA bring-up, `esp_http_client` + cert
bundle, the configurable server base, the "only when parked" safety idea.
The new code is the OTA writer + rollback gate + manifest/signature check.
```
