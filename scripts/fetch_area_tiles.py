#!/usr/bin/env python3
"""Fetch OSM raster tiles around a center point, with per-zoom radius
rings — full detail where you park, coarse detail where you cruise.

Writes {out}/{z}/{x}/{y}.png (map_view's layout — copy to the TF card's
/tiles, or serve via server/tileserver.py for on-device sync). Resumes:
existing tiles are skipped, so re-running after an interrupt is cheap.

Presets (z13..z16 = everything map_view renders):
  near    all zooms to 15 km          (~130 MB, ~45 min)
  region  z16@15, z15@30, z13-14@60   (~330 MB, ~2 h)    <- default
  wide    z16@20, z15@50, z13-14@120  (~1 GB, overnight)

Examples:
  fetch_area_tiles.py --center 45.52,-122.89
  fetch_area_tiles.py --center 45.52,-122.89 --preset wide
  fetch_area_tiles.py --center 45.52,-122.89 --rings 16:10,15:25,13:40

Polite to the OSM volunteers: throttled, proper UA, resume-not-refetch.
Bigger than `wide` belongs to the M2 pipeline (extract + self-render),
not to this script.
"""

import argparse
import math
import pathlib
import sys
import time
import urllib.request

UA = "vehicular-mesh-dev/0.1 (prototype; contact wmobil@gmail.com)"
THROTTLE_S = 0.15

PRESETS = {
    "near":   {16: 15, 15: 15, 14: 15, 13: 15},
    "region": {16: 15, 15: 30, 14: 60, 13: 60},
    "wide":   {16: 20, 15: 50, 14: 120, 13: 120},
}


def tile_x(lon, z):
    return int((lon + 180.0) / 360.0 * (1 << z))


def tile_y(lat, z):
    r = math.radians(lat)
    return int((1.0 - math.asinh(math.tan(r)) / math.pi) / 2.0 * (1 << z))


def tile_center(z, x, y):
    n = 1 << z
    lon = (x + 0.5) / n * 360.0 - 180.0
    lat = math.degrees(math.atan(math.sinh(math.pi * (1 - 2 * (y + 0.5) / n))))
    return lat, lon


def dist_km(lat1, lon1, lat2, lon2):
    r1, r2 = math.radians(lat1), math.radians(lat2)
    dlat, dlon = r2 - r1, math.radians(lon2 - lon1)
    a = (math.sin(dlat / 2) ** 2
         + math.cos(r1) * math.cos(r2) * math.sin(dlon / 2) ** 2)
    return 6371.0 * 2 * math.asin(math.sqrt(a))


def ring_tiles(clat, clon, z, radius_km):
    """All (x, y) at zoom z whose tile CENTER is within radius_km."""
    # bbox first, then the circle test (saves ~21% vs the square)
    dlat = radius_km / 111.0
    dlon = radius_km / (111.0 * math.cos(math.radians(clat)))
    x0, x1 = tile_x(clon - dlon, z), tile_x(clon + dlon, z)
    y0, y1 = tile_y(clat + dlat, z), tile_y(clat - dlat, z)
    # half a tile of slack so edge tiles aren't clipped mid-screen
    slack = 28092.0 / (1 << z) * 0.75  # ~3/4 tile width in km at 45N
    out = []
    for x in range(x0, x1 + 1):
        for y in range(y0, y1 + 1):
            tlat, tlon = tile_center(z, x, y)
            if dist_km(clat, clon, tlat, tlon) <= radius_km + slack:
                out.append((x, y))
    return out


def main():
    ap = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    ap.add_argument("--center", required=True,
                    help="lat,lon (e.g. 45.52,-122.89)")
    ap.add_argument("--preset", choices=sorted(PRESETS), default="region")
    ap.add_argument("--rings",
                    help="explicit z:radius_km list, e.g. 16:15,15:30,13:60 "
                         "(a zoom listed once covers only itself; overrides "
                         "--preset)")
    ap.add_argument("--out", default=str(pathlib.Path(__file__).resolve()
                                         .parent.parent / "assets" / "tiles"))
    args = ap.parse_args()

    clat, clon = (float(v) for v in args.center.split(","))
    if args.rings:
        rings = {}
        for part in args.rings.split(","):
            zs, rs = part.split(":")
            rings[int(zs)] = float(rs)
    else:
        rings = PRESETS[args.preset]

    out_root = pathlib.Path(args.out)
    plan = {z: ring_tiles(clat, clon, z, r) for z, r in sorted(rings.items())}
    total = sum(len(v) for v in plan.values())
    est_mb = total * 26 / 1024  # measured avg from the current set
    est_h = total * THROTTLE_S / 3600
    print(f"center {clat:.5f},{clon:.5f}  rings "
          + ", ".join(f"z{z}@{r}km" for z, r in sorted(rings.items())))
    print(f"{total} tiles, ~{est_mb:.0f} MB, >= {est_h:.1f} h at the "
          f"throttle (resume skips existing)")

    done = fetched = failed = 0
    t0 = time.time()
    for z, tiles in plan.items():
        print(f"z{z}: {len(tiles)} tiles")
        for x, y in tiles:
            done += 1
            p = out_root / str(z) / str(x) / f"{y}.png"
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
                failed += 1
                print(f"FAIL {url}: {e}", file=sys.stderr)
            time.sleep(THROTTLE_S)
            if fetched and fetched % 200 == 0:
                rate = fetched / (time.time() - t0)
                left = (total - done) / rate / 3600 if rate else 0
                print(f"...{done}/{total} ({fetched} new, ~{left:.1f} h left)")

    print(f"done: {fetched} fetched, {failed} failed, "
          f"{done - fetched - failed} already had -> {out_root}")
    if failed:
        print("re-run the same command to retry the failures (resume).")


if __name__ == "__main__":
    main()
