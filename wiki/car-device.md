# Build the car device

The in-car unit: 7" touchscreen, offline map, GPS navigation, two-tap
hazard reports over LoRa. No soldering; one jumper cap moves.

## 1. Order the parts

| Part | Where | ~Price |
|---|---|---|
| **Waveshare ESP32-P4-Module-DEV-KIT** (7" 720×1280 touch) | [Waveshare product page](https://www.waveshare.com/esp32-p4-module-dev-kit.htm) | $60–75 |
| **Waveshare USB-TO-LoRa-xF** dongle — **HF version** (915 MHz), SX1262, TCXO | [Waveshare product page](https://www.waveshare.com/product/iot-communication/long-range-wireless/nb-iot-lora/usb-to-lora.htm) ([docs](https://www.waveshare.com/wiki/USB-TO-LoRa-xF)) | $25 |
| **VK-172 USB GPS** (u-blox 7) | search "VK-172" on Amazon/AliExpress | $10–15 |
| **microSD card**, 8 GB+ (any brand) | anywhere | $8 |
| USB-C power supply (car: any 15 W+ USB-C adapter) | anywhere | — |

> **HF vs LF:** the dongle comes in HF (850–930 MHz) and LF (410–490
> MHz) versions. For the US mesh you want **HF**. Two dongles make a
> bench pair — worth ordering a spare.

## 2. The one hardware step: the USB hub jumper

Out of the box, only ONE of the kit's four USB-A ports works — the
factory jumper bypasses the onboard USB hub. You need two ports (GPS +
LoRa), so:

1. Find the **H3** 3-pin header with the **yellow jumper cap**, near
   the USB-A stack (next to the FSUSB42UMX chip).
2. Move the cap to the **DEVICE side** of the header — yes, DEVICE,
   counterintuitive as that reads when you're enabling more host
   ports. (Electrically: SEL=L → hub upstream enabled.)
3. Result: **three** live USB-A ports through the hub. (The single
   port that worked before goes dead in this position — expected.)

No firmware change needed — the firmware already handles the hub.

## 3. Configure the LoRa dongle (once, on any computer)

The dongle ships tuned to **868 MHz (channel 18)** — the mesh lives at
**915 MHz (channel 65)**. Fix it over the dongle's serial port
(115200 baud, every command ends with CRLF):

```
+++
AT+TXCH=65
AT+RXCH=65
AT+EXIT
```

Any serial terminal works (`screen /dev/cu.usbmodem* 115200`, PuTTY,
the Waveshare SSCOM tool). You should see `OK` after each command.

> ⚠️ Never hold the dongle's **KEY button** — 2 seconds silently
> restores factory settings (back to 868 MHz).

## 4. Install ESP-IDF v5.5

The firmware builds with Espressif's ESP-IDF, version **5.5**:

```sh
git clone -b v5.5 --recursive https://github.com/espressif/esp-idf.git ~/esp/esp-idf
cd ~/esp/esp-idf && ./install.sh esp32p4
```

(Full official guide: [ESP-IDF Get Started](https://docs.espressif.com/projects/esp-idf/en/v5.5/esp32p4/get-started/).)

## 5. Build and flash

Connect the kit's **USB-C debug port** to your computer, then:

```sh
git clone https://github.com/alviso/waycast.git
cd waycast/targets/esp32p4
. ~/esp/esp-idf/export.sh
idf.py set-target esp32p4
idf.py build
idf.py flash
```

First flash takes a few minutes (it writes the map-tile fallback
partition too). **This is the only USB flash you'll ever need** — the
partition table is A/B and every later update arrives over the air —
tap the **gear (⚙) in the top bar** → **Check update** — with
automatic rollback if an update misbehaves.

## 6. First boot checklist

Insert the microSD, plug GPS + LoRa dongles into the hub ports, power
on. You should see:

- the map (grid or demo tiles at first — real tiles come next)
- bottom-left diagnostics: `USB GPS: … | LoRa(C): open …`
- top-left: `GPS acquiring` (amber) → `GPS live` (green) once it sees
  sky — first fix takes 1–2 minutes outdoors

## 7. Load offline maps

Easiest: on the device. Tap the **gear (⚙) in the top bar** → join
your Wi-Fi → tap a coverage tier:

| Tier | Coverage | Size | Time |
|---|---|---|---|
| Near | 15 km, full detail | ~140 MB | ~15 min |
| Region | 60 km, detail tapers | ~330 MB | ~30 min |
| Wide | 120 km | ~1 GB | hours |

Tiles land on the card and stay there. (Map data © OpenStreetMap
contributors.)

## 8. Done — now what?

- Report something (the orange button, two taps). If a town node is in
  range you'll see **⌂ its name** in the status bar, and your report's
  chip gets a **green dot** when the town confirms it heard you.
- Build your town a [town node](town-node.html) — a mesh with memory
  beats two cars shouting.
- Something weird? [Troubleshooting](troubleshooting.html).
