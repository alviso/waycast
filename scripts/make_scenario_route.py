#!/usr/bin/env python3
"""Scenario compiler: build highway_demo.json from a real street route.

Queries OSRM's public demo server for driving geometry between waypoints
(Cupertino: north on De Anza Blvd, west on Homestead Rd), resamples it
into timed track waypoints at city speed, and places the demo events at
computed positions ALONG the route (a hazard "700 m ahead at t=15" is
literally 700 m of road ahead). Attestation refs stay index-stable.

Needs SSL_CERT_FILE (this machine's python CA quirk). Route stays inside
the fetched tile bbox (37.305..37.370 / -122.065..-122.015).
"""

import json
import math
import os
import pathlib
import ssl
import urllib.request

# Hillsboro / Orenco-Quatama loop (starts ~400m from home, not at it)
WAYPOINTS = [(-122.8901, 45.5220), (-122.8795, 45.5220),
             (-122.8795, 45.5305), (-122.9005, 45.5305),
             (-122.9005, 45.5220), (-122.8901, 45.5220)]
SPEED = 18.0          # m/s (~40 mph arterial)
TRACK_STEP_S = 10.0   # emit a waypoint every 10 s
PARKED_TAIL_S = 30.0  # brief parked beat, then the next lap

OUT = (pathlib.Path(__file__).resolve().parent.parent
       / "sim" / "scenarios" / "highway_demo.json")


def osrm_route():
    coords = ";".join(f"{lon},{lat}" for lon, lat in WAYPOINTS)
    url = (f"https://router.project-osrm.org/route/v1/driving/{coords}"
           f"?overview=full&geometries=geojson")
    ctx = ssl.create_default_context(cafile=os.environ.get("SSL_CERT_FILE"))
    with urllib.request.urlopen(url, context=ctx, timeout=30) as r:
        data = json.load(r)
    assert data["code"] == "Ok", data
    geom = data["routes"][0]["geometry"]["coordinates"]  # [[lon,lat],..]
    return [(lat, lon) for lon, lat in geom]


def dist_m(a, b):
    dy = (b[0] - a[0]) * 110540.0
    dx = (b[1] - a[1]) * 111320.0 * math.cos(math.radians(a[0]))
    return math.hypot(dx, dy)


def main():
    pts = osrm_route()
    # cumulative distance along the geometry
    cum = [0.0]
    for i in range(1, len(pts)):
        cum.append(cum[-1] + dist_m(pts[i - 1], pts[i]))
    total = cum[-1]
    dur = total / SPEED
    print(f"route: {len(pts)} pts, {total:.0f} m, {dur:.0f} s at "
          f"{SPEED*2.237:.0f} mph")

    def at_m(m):  # position at m meters along the route
        m = max(0.0, min(m, total))
        for i in range(1, len(cum)):
            if cum[i] >= m:
                f = (m - cum[i - 1]) / max(cum[i] - cum[i - 1], 1e-9)
                lat = pts[i - 1][0] + (pts[i][0] - pts[i - 1][0]) * f
                lon = pts[i - 1][1] + (pts[i][1] - pts[i - 1][1]) * f
                return lat, lon
        return pts[-1]

    # track: every TRACK_STEP_S, plus a parked tail
    track = []
    t = 0.0
    while t * SPEED < total:
        lat, lon = at_m(t * SPEED)
        track.append({"t": round(t), "lat": round(lat, 5),
                      "lon": round(lon, 5)})
        t += TRACK_STEP_S
    end_lat, end_lon = pts[-1]
    track.append({"t": round(dur), "lat": round(end_lat, 5),
                  "lon": round(end_lon, 5)})
    track.append({"t": round(dur + PARKED_TAIL_S),
                  "lat": round(end_lat, 5), "lon": round(end_lon, 5)})

    lap_s = dur + PARKED_TAIL_S

    def cap_ttl(t_fire, want):
        # die before the lap wraps (small margin) so laps don't stack
        return int(min(want, max(60, lap_s - t_fire - 5)))

    def on_route(t_fire, lead_m, jitter=(0.0, 0.0)):
        lat, lon = at_m(t_fire * SPEED + lead_m)
        return round(lat + jitter[0], 5), round(lon + jitter[1], 5)

    # events at lap FRACTIONS -> auto-spread on any route length;
    # same order (attest refs 0..3 stay stable)
    def ft(frac):
        return int(dur * frac)

    ev = []
    lat, lon = on_route(ft(.04), 700)
    ev.append({"t": ft(.04), "type": "debris", "severity": 2, "lat": lat,
               "lon": lon, "expiry_s": cap_ttl(ft(.04), 600),
               "radius_m": 3000, "origin": "SIM-A3F2",
               "note": "tire on shoulder"})
    lat, lon = on_route(ft(.26), 800)
    ev.append({"t": ft(.26), "type": "slowdown", "severity": 2, "lat": lat,
               "lon": lon, "expiry_s": cap_ttl(ft(.26), 900),
               "radius_m": 5000, "origin": "SIM-77C1",
               "note": "stop-and-go"})
    lat, lon = on_route(ft(.42), 1300)
    ev.append({"t": ft(.42), "type": "crash", "severity": 3, "lat": lat,
               "lon": lon, "expiry_s": cap_ttl(ft(.42), 1800),
               "radius_m": 5000, "origin": "SIM-B811",
               "note": "multi-vehicle, right lane"})
    lat, lon = on_route(ft(.56), 400, jitter=(0.004, -0.006))
    ev.append({"t": ft(.56), "type": "weather", "severity": 2, "lat": lat,
               "lon": lon, "expiry_s": cap_ttl(ft(.56), 3600),
               "radius_m": 8000, "origin": "SIM-4E09",
               "note": "dense fog"})

    lat, lon = on_route(ft(.12), 500, jitter=(0.0008, 0.0009))
    ev.append({"t": ft(.12), "channel": "local", "type": "event",
               "lat": lat, "lon": lon,
               "expiry_s": cap_ttl(ft(.12), 28800), "origin": "SIM-C201",
               "note": "Farmers market Sat 9-1"})
    lat, lon = on_route(ft(.33), 600, jitter=(-0.0007, 0.0008))
    ev.append({"t": ft(.33), "channel": "local", "type": "lodging",
               "lat": lat, "lon": lon,
               "expiry_s": cap_ttl(ft(.33), 14400), "origin": "SIM-9A44",
               "note": "2 rooms - inn on Cornell"})
    lat, lon = on_route(ft(.66), 700, jitter=(0.0006, -0.0009))
    ev.append({"t": ft(.66), "channel": "local", "type": "fuel",
               "lat": lat, "lon": lon,
               "expiry_s": cap_ttl(ft(.66), 7200), "origin": "SIM-1F7B",
               "note": "EV chargers open, no wait"})
    lat, lon = at_m(total - 150)
    ev.append({"t": ft(.90), "channel": "local", "type": "aid",
               "lat": round(lat + 0.0008, 5), "lon": round(lon - 0.001, 5),
               "expiry_s": cap_ttl(ft(.90), 14400), "origin": "SIM-D3E8",
               "note": "Water + first aid, Quatama"})

    ev += [
        {"t": ft(.09), "type": "attest", "ref": 0, "verdict": "confirm",
         "origin": "SIM-77C1"},
        {"t": ft(.11), "type": "attest", "ref": 0, "verdict": "confirm",
         "origin": "SIM-B811"},
        {"t": ft(.46), "type": "attest", "ref": 2, "verdict": "confirm",
         "origin": "SIM-A3F2"},
        {"t": ft(.60), "type": "attest", "ref": 3, "verdict": "deny",
         "origin": "SIM-77C1"},
        {"t": ft(.62), "type": "attest", "ref": 3, "verdict": "deny",
         "origin": "SIM-B811"},
    ]

    out = {
        "name": "highway-demo",
        "description": ("Street-following demo (OSRM route): closed circuit "
                        "(Hillsboro/Orenco-Quatama arterial loop), "
                        "loops seamlessly. Hazards placed along the real route."),
        "track": track,
        "events": ev,
    }
    OUT.write_text(json.dumps(out, indent=2) + "\n")
    print(f"wrote {OUT.name}: {len(track)} waypoints, {len(ev)} events, "
          f"demo ends parked at t={round(dur + PARKED_TAIL_S)}s")


if __name__ == "__main__":
    main()
