#!/usr/bin/env python3
"""Fetch OSM raster tiles covering the demo scenario area.

Writes assets/tiles/{z}/{x}/{y}.png — the layout map_view expects, both
for the simulator (repo-relative) and for copying onto the device's TF
card (/sdcard/tiles). Skips tiles already on disk, throttles requests,
and sends a proper User-Agent per the OSM tile usage policy.

Dev-scale use only (a few hundred tiles). A real tile pipeline
(OSM extract -> pre-rendered tiles, PRODUCT_PLAN.md M2) replaces this.
"""

import math
import pathlib
import sys
import time
import urllib.request

ZOOMS = [13, 14, 15, 16]  # map supports zoom-out to z13

# bbox derives from the generated scenario track — change the route,
# rerun make_scenario_route.py, then this script: no manual sync.
import json as _json
_track = _json.load(open(pathlib.Path(__file__).resolve().parent.parent
                          / "sim" / "scenarios" / "highway_demo.json"))["track"]
LAT_MIN = min(w["lat"] for w in _track)
LAT_MAX = max(w["lat"] for w in _track)
LON_MIN = min(w["lon"] for w in _track)
LON_MAX = max(w["lon"] for w in _track)

UA = "vehicular-mesh-dev/0.1 (prototype; contact wmobil@gmail.com)"
OUT = pathlib.Path(__file__).resolve().parent.parent / "assets" / "tiles"


def tile_x(lon, z):
    return int((lon + 180.0) / 360.0 * (1 << z))


def tile_y(lat, z):
    r = math.radians(lat)
    return int((1.0 - math.asinh(math.tan(r)) / math.pi) / 2.0 * (1 << z))


def main():
    done = fetched = 0
    for z in ZOOMS:
        # wider margin at lower zooms so zoom-out still has coverage
        pad = 0.02 * (16 - z + 1)
        x0, x1 = tile_x(LON_MIN - pad, z), tile_x(LON_MAX + pad, z)
        y0, y1 = tile_y(LAT_MAX + pad, z), tile_y(LAT_MIN - pad, z)
        total = (x1 - x0 + 1) * (y1 - y0 + 1)
        print(f"z{z}: x {x0}..{x1}, y {y0}..{y1} -> {total} tiles")
        for x in range(x0, x1 + 1):
            for y in range(y0, y1 + 1):
                done += 1
                p = OUT / str(z) / str(x) / f"{y}.png"
                if p.exists():
                    continue
                p.parent.mkdir(parents=True, exist_ok=True)
                url = f"https://tile.openstreetmap.org/{z}/{x}/{y}.png"
                req = urllib.request.Request(url, headers={"User-Agent": UA})
                try:
                    with urllib.request.urlopen(req, timeout=15) as r:
                        p.write_bytes(r.read())
                    fetched += 1
                except Exception as e:
                    print(f"FAIL {url}: {e}", file=sys.stderr)
                time.sleep(0.15)  # be polite
                if done % 50 == 0:
                    print(f"...{done}")

    print(f"done ({fetched} newly fetched) -> {OUT}")


if __name__ == "__main__":
    main()
