# Build a town node

The anchor of a Waycast town: it hears every report in range, relays
valid ones, injects official National Weather Service alerts into the
mesh, and announces itself once a minute so passing cars show
**⌂ your town's name**. Runs unattended — it's a set-and-forget
appliance.

## 1. Order the parts

| Part | Where | ~Price |
|---|---|---|
| **Raspberry Pi 4 or 5** (2 GB is plenty) + SD card + PSU | [raspberrypi.com](https://www.raspberrypi.com/products/) | $50–80 |
| **RAK2287 concentrator module** — SPI version, **US915**, with GPS | [RAK store](https://store.rakwireless.com/products/rak2287-lpwan-gateway-concentrator-module) | $80–100 |
| **RAK2287 Pi HAT** (the adapter board) | same page, "Pi HAT" option | $15–20 |
| **915 MHz antenna** — mount it high; this sets your town's radius | any 915 MHz fiberglass/whip with the right connector | $15–50 |
| Optional: **VK-172 USB GPS** | Amazon/AliExpress | $10 |

> Why a concentrator instead of a $25 dongle? The SX1302 hears **all
> spreading factors on 8 channels simultaneously** — a town node
> should hear everyone, always. The GPS variant also gives the node
> its own time and position.

## 2. Prepare the Pi

Standard Raspberry Pi OS (64-bit, Lite is fine). Enable SPI:

```sh
sudo raspi-config nonint do_spi 0
```

Seat the RAK2287 on its Pi HAT, the HAT on the Pi's 40-pin header,
antenna on the RF port. **Never power the concentrator without an
antenna attached.**

## 3. Install Waycast

```sh
git clone https://github.com/alviso/waycast.git
cd waycast/peer
./setup.sh
```

`setup.sh` builds the mesh peer (patching the Semtech HAL for the
RAK2287's quirks), installs two systemd services, and enables them at
boot:

- `waycast-peer` — the radio: receive, validate, relay, beacon
- `waycast-injector` — polls NWS and injects active weather alerts

## 4. Configure your town

```sh
sudo cp node.conf.example /etc/waycast/node.conf
sudo nano /etc/waycast/node.conf
```

Set three lines:

```
NODE_LAT=45.52          # your node's location
NODE_LON=-122.89
NODE_NAME=YourTown      # what car screens will show: ⌂ YourTown
```

(With a USB GPS plugged in, position comes from the GPS instead and
the config is the fallback.) Then:

```sh
sudo systemctl restart waycast-peer waycast-injector
```

## 5. How you know it's working

```sh
journalctl -u waycast-peer -f
```

Within a minute you'll see the startup banner
(`vmesh_peer up: 915.0 MHz SF7/125k …`). Then:

- **Every frame it hears** is logged with frequency, RSSI, and the
  decoded report: `[rx OK ] Crash 01E1902F/7 … rssi=-77`
- **`[relay]` lines** mean it's re-broadcasting valid reports
- **`[inject] tx Weather …`** appears when NWS has an active alert
  for your area
- A car device (or a [bench transmitter](workstation.html)) in range
  shows **⌂ YourTown** within a minute — that's the presence beacon

## 6. Placement notes

Range is antenna height and quality, little else. A node on a shelf
hears a neighborhood; the same node with a fiberglass antenna at roof
height hears a town. Start indoors to validate, then move it up.
