#!/usr/bin/env python3
"""Stage the device tile subset (assets_device/tiles).

The full fetched set (assets/tiles) is nice for the simulator but too
big for the device's flash partition; this copies just the demo-track
bbox (small per-zoom pad) into a staging tree that the esp32p4 build
bakes into the spiffs image. Re-run after fetch_demo_tiles.py.
"""

import math
import pathlib
import shutil

ZOOMS = [13, 14, 15, 16]

import json as _json
_track = _json.load(open(pathlib.Path(__file__).resolve().parent.parent
                          / "sim" / "scenarios" / "highway_demo.json"))["track"]
LAT_MIN = min(w["lat"] for w in _track)
LAT_MAX = max(w["lat"] for w in _track)
LON_MIN = min(w["lon"] for w in _track)
LON_MAX = max(w["lon"] for w in _track)

# Pad must cover at least the half-viewport at each zoom, or the screen
# edges go dark while driving (1280x720 view; z16 ~1.9 m/px at lat 37).
# z16 gets full coverage; lower zooms get slimmer pads (they're for
# zoom-out overview; running out of map at the edge there is OK).
# (z15-z13 pads kept slim so the whole set + spiffs overhead fits the
# 13.25MB partition — spiffsgen hard-fails on overflow)
PAD_LON = {16: 0.016, 15: 0.010, 14: 0.008, 13: 0.008}
PAD_LAT = {16: 0.012, 15: 0.008, 14: 0.008, 13: 0.008}

ROOT = pathlib.Path(__file__).resolve().parent.parent
SRC = ROOT / "assets" / "tiles"
DST = ROOT / "assets_device" / "tiles"


def tile_x(lon, z):
    return int((lon + 180.0) / 360.0 * (1 << z))


def tile_y(lat, z):
    r = math.radians(lat)
    return int((1.0 - math.asinh(math.tan(r)) / math.pi) / 2.0 * (1 << z))


def main():
    if DST.parent.exists():
        shutil.rmtree(DST.parent)
    n = missing = 0
    for z in ZOOMS:
        plon, plat = PAD_LON[z], PAD_LAT[z]
        x0, x1 = tile_x(LON_MIN - plon, z), tile_x(LON_MAX + plon, z)
        y0, y1 = tile_y(LAT_MAX + plat, z), tile_y(LAT_MIN - plat, z)
        for x in range(x0, x1 + 1):
            for y in range(y0, y1 + 1):
                s = SRC / str(z) / str(x) / f"{y}.png"
                if not s.exists():
                    missing += 1
                    continue
                d = DST / str(z) / str(x) / f"{y}.png"
                d.parent.mkdir(parents=True, exist_ok=True)
                shutil.copy2(s, d)
                n += 1
    size = sum(f.stat().st_size for f in DST.rglob("*.png"))
    print(f"staged {n} tiles ({size/1e6:.1f} MB, {missing} missing) -> {DST}")


if __name__ == "__main__":
    main()
