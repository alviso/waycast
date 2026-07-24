#!/usr/bin/env python3
"""waycast injector — public alert data -> vmesh town-node frames.

Polls api.weather.gov active CAP alerts for the node's location and
feeds them to the local vmesh_peer relay via its UDP inject listener
(line proto, loopback :7788). Each active alert is re-injected every
REFRESH_S seconds — the mesh is ephemeral by design, so TTL-refresh
keeps an alert alive exactly as long as its source says it is active,
and silence lets it fade. No state survives a restart; none needed.

CAP -> vmesh mapping:
  severity Extreme/Severe -> 3, Moderate -> 2, Minor/other -> 2/1
  polygon  -> centroid + max-vertex-distance radius (capped 60 km)
  zone     -> node-centred 20 km blanket
  expires  -> ttl_s (clamped 300..65535; refresh covers longer alerts)
  event    -> note[39] + hazard type (WEATHER, road CLOSURE, EMERGENCY)

Legal basis: NWS/NOAA alerts are U.S. government works — public
domain, 17 USC 105. api.weather.gov asks for a contact User-Agent.
"""

import datetime
import json
import math
import os
import socket
import time
import urllib.request

LAT = float(os.environ.get("NODE_LAT", "45.52"))
LON = float(os.environ.get("NODE_LON", "-122.89"))
# peer-start.sh publishes the GPS-or-conf resolved location here; we
# re-read it every poll so a late GPS fix wins without a restart.
LOC_FILE = os.environ.get("LOC_FILE", "/run/waycast/location")
INJECT_PORT = int(os.environ.get("INJECT_PORT", "7788"))
POLL_S = int(os.environ.get("POLL_S", "120"))
REFRESH_S = int(os.environ.get("REFRESH_S", "600"))
USER_AGENT = os.environ.get(
    "NWS_UA", "waycast-town-node/0.1 (waycast.io; ops@waycast.io)")

# vmesh_msg.h hazard ids
HZ_WEATHER, HZ_CLOSURE, HZ_EMERGENCY = 4, 5, 6

CAP_SEV = {"Extreme": 3, "Severe": 3, "Moderate": 2, "Minor": 1}


def fetch_alerts():
    url = f"https://api.weather.gov/alerts/active?point={LAT},{LON}"
    req = urllib.request.Request(
        url, headers={"User-Agent": USER_AGENT,
                      "Accept": "application/geo+json"})
    with urllib.request.urlopen(req, timeout=20) as r:
        return json.load(r).get("features", [])


def dist_m(a1, o1, a2, o2):
    dx = (o2 - o1) * 111320.0 * math.cos(math.radians((a1 + a2) / 2))
    dy = (a2 - a1) * 110540.0
    return math.hypot(dx, dy)


def centroid_radius(geom):
    """CAP polygon -> (lat, lon, radius_m); zone alerts get a node-
    centred blanket (the node only reaches its own neighbourhood)."""
    if not geom or geom.get("type") != "Polygon":
        return LAT, LON, 20000
    pts = geom["coordinates"][0]
    la = sum(p[1] for p in pts) / len(pts)
    lo = sum(p[0] for p in pts) / len(pts)
    r = max(dist_m(la, lo, p[1], p[0]) for p in pts)
    return la, lo, min(int(r), 60000)


def hazard_for(event):
    e = event.lower()
    if any(w in e for w in ("evacuation", "civil", "hazardous materials",
                            "shelter", "911", "law enforcement")):
        return HZ_EMERGENCY
    if any(w in e for w in ("closure", "road", "travel")):
        return HZ_CLOSURE
    return HZ_WEATHER


def ttl_for(props):
    exp = props.get("expires") or props.get("ends")
    if exp:
        try:
            t = datetime.datetime.fromisoformat(exp)
            left = (t - datetime.datetime.now(t.tzinfo)).total_seconds()
            return max(300, min(65535, int(left)))
        except ValueError:
            pass
    return 3600


def refresh_location():
    global LAT, LON
    try:
        parts = open(LOC_FILE).read().split()
        la, lo = float(parts[0]), float(parts[1])
    except (OSError, ValueError, IndexError):
        return
    if (la, lo) != (LAT, LON):
        src = parts[2] if len(parts) > 2 else "?"
        print(f"location update: {la},{lo} ({src})", flush=True)
        LAT, LON = la, lo


def main():
    sock = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
    last_sent = {}  # alert id -> monotonic ts of last injection
    print(f"injector up: point {LAT},{LON} -> udp :{INJECT_PORT}, "
          f"poll {POLL_S}s refresh {REFRESH_S}s", flush=True)
    while True:
        refresh_location()
        try:
            feats = fetch_alerts()
        except Exception as e:  # noqa: BLE001 — keep polling through outages
            print(f"poll failed: {e}", flush=True)
            feats = []
        now = time.monotonic()
        active_ids = set()
        for f in feats:
            props = f.get("properties", {})
            aid = props.get("id") or f.get("id")
            active_ids.add(aid)
            if now - last_sent.get(aid, -1e12) < REFRESH_S:
                continue
            event = props.get("event") or "NWS alert"
            la, lo, rm = centroid_radius(f.get("geometry"))
            line = (f"HAZ {hazard_for(event)} "
                    f"{CAP_SEV.get(props.get('severity'), 2)} "
                    f"{la:.5f} {lo:.5f} {rm} {ttl_for(props)} {event[:39]}")
            sock.sendto(line.encode(), ("127.0.0.1", INJECT_PORT))
            last_sent[aid] = now
            print(f"inject: {line}", flush=True)
        # forget expired alerts so the dict doesn't grow forever
        for aid in list(last_sent):
            if aid not in active_ids:
                del last_sent[aid]
        time.sleep(POLL_S)


if __name__ == "__main__":
    main()
