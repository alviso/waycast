#!/usr/bin/env python3
"""Waycast tile server — deployable at tiles.waycast.io, runnable on a
laptop. Zero dependencies (stdlib only).

Endpoints:
  GET /{z}/{x}/{y}.png    single tile, OSM path shape. Cache-on-miss
                          (throttled, UA'd upstream), served from disk
                          forever after.
  GET /area.tar?lat=..&lon=..[&half_lat=0.015][&half_lon=0.020]
                          region archive: every z13..16 tile covering
                          the bbox, one tar stream. The device (or
                          anything) sends a coordinate; the server
                          does the math. Misses are fetched upstream
                          first, so a warm region is one round trip.
  GET /healthz            "ok" + cache stats.

Config (env or flags): PORT, CACHE_DIR, UPSTREAM.

Politeness/product notes:
- Upstream fetches are globally throttled + capped per client IP per
  hour: this is a CACHE in front of OSM for a small fleet, not a
  mass proxy. The product path replaces UPSTREAM with self-rendered
  tiles (planetiler/openmaptiles) behind the exact same paths.
- Attribution: (c) OpenStreetMap contributors — surfaced in an
  X-Attribution header here and in the device UI's map corner.

Deploy: see server/DEPLOY.md (Docker + Caddy auto-TLS).
"""

import argparse
import http.server
import io
import math
import os
import pathlib
import re
import socketserver
import tarfile
import threading
import time
import urllib.parse
import urllib.request

UA = "waycast-tileserver/0.1 (contact wmobil@gmail.com)"
ATTR = "(c) OpenStreetMap contributors"
TILE_RE = re.compile(r"^/(\d{1,2})/(\d+)/(\d+)\.png$")
ZMIN, ZMAX = 13, 16
AREA_TILE_CAP = 1500
UPSTREAM_PER_IP_PER_HOUR = 4000  # misses only; hits are free

_fetch_lock = threading.Lock()
_last_fetch = [0.0]
_ip_budget = {}  # ip -> [window_start, count]
_ip_lock = threading.Lock()


def tile_x(lon, z):
    return int((lon + 180.0) / 360.0 * (1 << z))


def tile_y(lat, z):
    r = math.radians(lat)
    return int((1.0 - math.asinh(math.tan(r)) / math.pi) / 2.0 * (1 << z))


def upstream_allowed(ip):
    now = time.time()
    with _ip_lock:
        w = _ip_budget.get(ip)
        if not w or now - w[0] > 3600:
            _ip_budget[ip] = [now, 0]
            w = _ip_budget[ip]
        if w[1] >= UPSTREAM_PER_IP_PER_HOUR:
            return False
        w[1] += 1
        return True


class TileHandler(http.server.BaseHTTPRequestHandler):
    cache_dir = None
    upstream = None
    protocol_version = "HTTP/1.1"

    # ---- helpers ----

    def _tile_path(self, z, x, y):
        return self.cache_dir / str(z) / str(x) / f"{y}.png"

    def _get_tile(self, z, x, y, client_ip):
        """cached bytes, fetching upstream on miss. None = unavailable."""
        p = self._tile_path(z, x, y)
        if p.exists() and p.stat().st_size > 0:
            return p.read_bytes(), True
        if not upstream_allowed(client_ip):
            return None, False
        url = f"{self.upstream}/{z}/{x}/{y}.png"
        try:
            with _fetch_lock:
                wait = 0.15 - (time.time() - _last_fetch[0])
                if wait > 0:
                    time.sleep(wait)
                req = urllib.request.Request(url,
                                             headers={"User-Agent": UA})
                data = urllib.request.urlopen(req, timeout=15).read()
                _last_fetch[0] = time.time()
        except Exception as e:
            print(f"MISS {z}/{x}/{y} upstream failed: {e}", flush=True)
            return None, False
        p.parent.mkdir(parents=True, exist_ok=True)
        p.write_bytes(data)
        return data, False

    def _headers(self, code, ctype, length):
        self.send_response(code)
        self.send_header("Content-Type", ctype)
        self.send_header("Content-Length", str(length))
        self.send_header("X-Attribution", ATTR)
        self.send_header("Cache-Control", "public, max-age=86400")
        self.end_headers()

    # ---- endpoints ----

    def do_GET(self):
        parsed = urllib.parse.urlparse(self.path)

        if parsed.path == "/healthz":
            n = sum(1 for _ in self.cache_dir.rglob("*.png"))
            body = f"ok\ncached_tiles {n}\n".encode()
            self._headers(200, "text/plain", len(body))
            self.wfile.write(body)
            return

        if parsed.path == "/area.tar":
            self._area(parsed)
            return

        m = TILE_RE.match(parsed.path)
        if not m:
            self.send_error(404, "want /z/x/y.png or /area.tar")
            return
        z, x, y = (int(g) for g in m.groups())
        if not (0 <= z <= 19):
            self.send_error(400, "bad zoom")
            return
        data, hit = self._get_tile(z, x, y, self.client_address[0])
        if data is None:
            self.send_error(502, "tile unavailable")
            return
        self._headers(200, "image/png", len(data))
        self.wfile.write(data)
        print(f"{'HIT ' if hit else 'MISS'} {parsed.path}", flush=True)

    def _area(self, parsed):
        q = urllib.parse.parse_qs(parsed.query)
        try:
            lat = float(q["lat"][0])
            lon = float(q["lon"][0])
            half_lat = float(q.get("half_lat", ["0.015"])[0])
            half_lon = float(q.get("half_lon", ["0.020"])[0])
        except (KeyError, ValueError):
            self.send_error(400, "need lat=&lon= (opt half_lat, half_lon)")
            return

        tiles = []
        for z in range(ZMIN, ZMAX + 1):
            x0, x1 = tile_x(lon - half_lon, z), tile_x(lon + half_lon, z)
            y0, y1 = tile_y(lat + half_lat, z), tile_y(lat - half_lat, z)
            for x in range(x0, x1 + 1):
                for y in range(y0, y1 + 1):
                    tiles.append((z, x, y))
        if len(tiles) > AREA_TILE_CAP:
            self.send_error(413, f"{len(tiles)} tiles > cap {AREA_TILE_CAP}")
            return

        ip = self.client_address[0]
        buf = io.BytesIO()
        served = 0
        with tarfile.open(fileobj=buf, mode="w") as tar:
            for z, x, y in tiles:
                data, _ = self._get_tile(z, x, y, ip)
                if data is None:
                    continue
                info = tarfile.TarInfo(name=f"tiles/{z}/{x}/{y}.png")
                info.size = len(data)
                info.mtime = int(time.time())
                tar.addfile(info, io.BytesIO(data))
                served += 1
        body = buf.getvalue()
        self._headers(200, "application/x-tar", len(body))
        self.wfile.write(body)
        print(f"AREA {lat:.4f},{lon:.4f} -> {served}/{len(tiles)} tiles, "
              f"{len(body) // 1024} KB", flush=True)

    def log_message(self, *a):
        pass


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--port", type=int,
                    default=int(os.environ.get("PORT", 8484)))
    ap.add_argument("--cache",
                    default=os.environ.get(
                        "CACHE_DIR",
                        str(pathlib.Path(__file__).parent / "cache")))
    ap.add_argument("--upstream",
                    default=os.environ.get(
                        "UPSTREAM", "https://tile.openstreetmap.org"))
    args = ap.parse_args()

    TileHandler.cache_dir = pathlib.Path(args.cache)
    TileHandler.cache_dir.mkdir(parents=True, exist_ok=True)
    TileHandler.upstream = args.upstream

    with socketserver.ThreadingTCPServer(("", args.port),
                                         TileHandler) as srv:
        srv.allow_reuse_address = True
        print(f"waycast tileserver on :{args.port}, cache "
              f"{TileHandler.cache_dir}, upstream {args.upstream}",
              flush=True)
        try:
            srv.serve_forever()
        except KeyboardInterrupt:
            print("\nbye")


if __name__ == "__main__":
    main()
