# Workstation: simulator, tiles, bench radio

Three things your Mac (or Linux box) can do for Waycast — including
everything short of driving.

## 1. Run the device UI with zero hardware

The full car-device interface runs as a desktop app on a scripted demo
scenario — hazards appear from a simulated mesh, age, and fade:

```sh
git clone https://github.com/alviso/waycast.git
cd waycast
brew install sdl2          # macOS   (Linux: sudo apt install libsdl2-dev)
make deps                  # fetches LVGL
make -j8
make run
```

This is also where UI development happens — the simulator and the
device share every line of UI code.

## 2. Pre-fetch map tiles

If you'd rather fill a card from your computer than wait on the
device's Wi-Fi:

```sh
python3 scripts/fetch_area_tiles.py --center <lat>,<lon> --preset region
```

Presets: `near` (15 km, ~140 MB) / `region` (60 km, ~330 MB) / `wide`
(120 km, ~1 GB). Copy the resulting `assets/tiles/` folder to the
card as `/tiles/`. The script is throttled and resume-safe — please
keep it that way, and put **your own contact e-mail** in the `UA`
string if you fork it (OSM tile policy).

## 3. Bench radio: test a town node from your desk

A second USB-TO-LoRa dongle (configured like the
[car device's](car-device.html#3-configure-the-lora-dongle-once-on-any-computer))
turns your computer into a transmitter:

```sh
cd tools/txtool
cc -o txtool -I ../../msg -I ../../net txtool.c \
   ../../msg/vmesh_wire.c ../../net/lora_dtu.c
./txtool /dev/cu.usbmodemXXXX 1 "hello from the bench"
```

Then watch your [town node](town-node.html)'s journal:

```sh
journalctl -u waycast-peer -f
# → [rx OK ] Debris 02BEEF01/1 … "hello from the bench"
```

Edit the coordinates in `txtool.c` to your area first — the mesh
drops reports outside their relevance radius, by design.

> Reading from a dongle on macOS: plain `cat /dev/cu.usbmodem…` shows
> nothing — the dongle only streams once DTR is asserted. Use a real
> serial terminal (`screen`, minicom) or see
> [troubleshooting](troubleshooting.html).
