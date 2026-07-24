# Waycast

**An open mesh network for cars.** Vehicles, town nodes, and shop
beacons share road reports over LoRa radio: no internet, no SIM, no
accounts, no tracking. Reports live and die by place and time —
*postcards, not chat*.

- Website: **[waycast.io](https://waycast.io)**
- Community: **[r/waycast](https://www.reddit.com/r/waycast/)**
- License: **GPLv3** (see [LICENSE](LICENSE))

> **Status: working prototype, built in the open.** One car device and
> one town node exchange real reports over the air daily — GPS
> navigation on offline maps, presence beacons, delivery receipts, and
> over-the-air firmware updates all work. It is not a finished product:
> expect sharp edges, protocol changes, and honest commit messages.

## The idea

Telling the car behind you about a pothole shouldn't require a cell
tower, an account, and a datacenter. Waycast moves road reports
car-to-car and town-to-town over LoRa:

- **Drive** — the in-car device: offline map, two-tap hazard reports,
  ambient alerts while driving, a browsable "town square" when parked.
- **Anchor** — a town node (Raspberry Pi + LoRa concentrator): gives a
  town a memory. Holds recent reports, re-broadcasts them to passing
  cars, injects official weather alerts, announces itself once a
  minute ("you are in Hillsboro's earshot").
- **Announce** — a shop/POI beacon: local notices for passing drivers
  (planned).

Every report carries a lifespan and a radius; when either runs out it
is gone everywhere. There is no archive, no server, and nothing to
create an account on.

## Hardware (current prototype)

| Role | Hardware |
|---|---|
| Car device | Waveshare ESP32-P4-Module-DEV-KIT (7" DSI touch) + USB LoRa dongle (Waveshare USB-TO-LoRa-xF, SX1262) + USB GPS (u-blox VK-172) + microSD for map tiles |
| Town node | Raspberry Pi + RAK2287 (SX1302) concentrator HAT |
| Radio profile | 915 MHz US ISM, SF7/BW125 (fleet-configurable) |

See [docs/BUILDING.md](docs/BUILDING.md) for the full build guide —
including the dev kit's USB-hub jumper mod and the dongle
configuration.

## Repository layout

```
msg/        message model + feed interface — THE seam between UI and radio
mesh/       geo-ephemeral flooding core (shared: device, town node, sim)
ui/         LVGL UI: map, hazards, town square, settings (target-agnostic)
net/        NMEA parsing, DTU framing, provisioning seams
sim/        scenario player — the same UI runs on scripted demo data
targets/
  sdl/      desktop simulator (day-to-day UI development)
  esp32p4/  ESP-IDF firmware for the car device (OTA-updatable)
peer/       town node: concentrator relay + NWS alert injector (Pi)
server/     tile cache server (deployable; runs at tiles.waycast.io)
scripts/    tile fetchers, scenario generation, firmware publishing
docs/       specs, product plan, OTA design, hardware notes
```

## Quick start: desktop simulator

No hardware needed — the full UI on scripted demo data:

```sh
brew install sdl2        # macOS; apt install libsdl2-dev on Linux
make deps                # fetch LVGL + vendored deps
make -j8
make run
```

## Quick start: real hardware

The [build guide](docs/BUILDING.md) walks through flashing the car
device (ESP-IDF v5.5), configuring the LoRa dongles, loading offline
map tiles, and provisioning a town node with `peer/setup.sh`.

## Design documents

- [docs/handoff-spec.md](docs/handoff-spec.md) — the full project spec
- [docs/PRODUCT_PLAN.md](docs/PRODUCT_PLAN.md) — phases, screens, roadmap
- [docs/OTA.md](docs/OTA.md) — A/B firmware updates with rollback
- [docs/MESSAGING.md](docs/MESSAGING.md) — the message model

## Attribution & thanks

- Map data © [OpenStreetMap](https://www.openstreetmap.org/copyright)
  contributors (shown in-app; tile fetching follows the OSM tile usage
  policy — bring your own contact UA if you fork the fetchers)
- [LVGL](https://lvgl.io) (MIT), [RadioLib](https://github.com/jgromes/RadioLib)
  (MIT), [cJSON](https://github.com/DaveGamble/cJSON) (MIT),
  [lodepng](https://github.com/lvandeve/lodepng) (zlib),
  [Semtech](https://www.semtech.com)'s libloragw (RAK2287 HAL)
- Weather alerts: [NWS public API](https://www.weather.gov/documentation/services-web-api)

## Regulatory note

LoRa transmissions must follow your region's ISM rules (frequency,
power, duty cycle). The defaults target US915/FCC Part 15; operating
elsewhere is your responsibility to configure correctly.
