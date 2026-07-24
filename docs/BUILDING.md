# Building Waycast hardware

Everything here is off-the-shelf. Three builds, in increasing effort:
the desktop simulator (no hardware), the car device, and a town node.

---

## 0. Desktop simulator (no hardware)

```sh
brew install sdl2        # macOS; apt install libsdl2-dev on Linux
make deps && make -j8 && make run
```

The full device UI on a scripted demo scenario. This is where UI
development happens day-to-day.

---

## 1. The car device

### Parts

| Part | Notes |
|---|---|
| Waveshare **ESP32-P4-Module-DEV-KIT** | the 7" 720×1280 DSI touch variant |
| Waveshare **USB-TO-LoRa-xF** dongle (SX1262) | HF version for 915 MHz regions |
| **VK-172** USB GPS (u-blox 7) | any u-blox CDC GPS mouse should work |
| **microSD card** (any size ≥ 1 GB) | FAT32 — offline map tiles |
| USB-C power + the kit's stand | |

### One-time hardware prep

1. **USB hub jumper.** The kit's four USB-A ports sit behind an
   onboard CH334F hub, but the factory jumper routes the P4's one OTG
   port straight to a single USB-A port. Move the **H3 jumper** (3-pin
   header near the FSUSB42UMX mux) to the *other* position: `SEL=L`
   routes OTG → hub → three live USB-A ports, so GPS and LoRa dongle
   work simultaneously. (The firmware forces the root port to
   Full-Speed to work around missing split-transaction support in
   ESP-IDF — already handled, nothing to configure.)
2. **Dongle radio profile.** Fleet dongles must share one profile. The
   firmware assumes **channel 65 = 915.0 MHz, SF7, BW125** (the AT
   factory default is CH18 = 868 MHz!). Configure each dongle once over
   its serial port (115200, commands end CRLF):
   ```
   +++            (enter AT mode)
   AT+TXCH=65
   AT+RXCH=65
   AT+EXIT
   ```
   Do **not** hold the dongle's KEY button — 2 seconds restores factory
   settings (back to 868 MHz, silently).

### Flash the firmware

ESP-IDF **v5.5** required.

```sh
cd targets/esp32p4
idf.py set-target esp32p4
idf.py build
idf.py -p <port> flash        # first flash over USB
```

The partition table is A/B (OTA): after this first USB flash, updates
arrive over the air (Settings → Check update) — see [OTA.md](OTA.md).

### Offline map tiles

Two ways to fill the card:

- **On-device** (easiest): join Wi-Fi in Settings, then tap a coverage
  tier — *Near* (15 km, ~140 MB), *Region* (60 km, ~330 MB), or *Wide*
  (120 km, ~1 GB). Tiles download straight to the card.
- **Pre-seed from a computer**: `scripts/fetch_area_tiles.py --center
  <lat,lon> --preset region`, then copy `assets/tiles/` to the card's
  `/tiles/`. Respect the OSM tile policy: keep the throttle, set your
  own contact e-mail in the UA string.

---

## 2. The town node (Anchor)

### Parts

| Part | Notes |
|---|---|
| Raspberry Pi (4 or later) | Raspberry Pi OS |
| **RAK2287** SX1302 concentrator | SPI variant, on a Pi HAT adapter |
| Optional: USB GPS | position + time (falls back to config) |
| A decent 915 MHz antenna, mounted high | the single best range upgrade |

### Provision

```sh
cd peer
./setup.sh          # builds vmesh_peer, installs systemd services
sudo cp node.conf.example /etc/waycast/node.conf
sudo nano /etc/waycast/node.conf    # set NODE_LAT/NODE_LON/NODE_NAME
sudo systemctl restart waycast-peer waycast-injector
```

The node then: relays valid reports (geo-ephemeral flooding), injects
NWS weather alerts for its location, transmits a presence beacon once
a minute (devices show "⌂ <your town>"), and logs every frame it hears
(`journalctl -u waycast-peer -f`).

The RAK2287 reset quirk, the SX1302 HAL patch, and the DTU air-header
interop are all handled inside `peer/` — read the sources if you're
curious; the commit history was a battlefield.

---

## 3. Fleet notes

- All radios must share: frequency (CH65/915.0), SF7, BW125, private
  sync word. The concentrator *hears* every SF simultaneously but
  *transmits* SF7 — keep dongles on SF7.
- Range is antenna-dominated. Stock whips ≈ neighborhood scale; a
  proper 915 MHz antenna on the town node is the highest-leverage
  upgrade.
- Clock skew is handled at the receiver (`vmesh_clock_normalize`):
  devices without GPS lock still participate fully.

## Maintainer: publishing firmware

```sh
git tag vX.Y.Z && idf.py reconfigure build
python3 scripts/publish_firmware.py       # refuses stale/-dirty builds
```

Publishes a hash-verified image + manifest that devices pick up via
Settings → Check update.
