#!/bin/bash
# Print "LAT LON" from the first gpsd TPV report carrying a 2D/3D fix,
# waiting at most GPS_WAIT_S (default 30). Exit 1 on no fix / no gpsd —
# callers fall back to the configured location.
W=${GPS_WAIT_S:-30}
timeout "$W" gpspipe -w 2>/dev/null | python3 -c '
import json, sys
for line in sys.stdin:
    try:
        d = json.loads(line)
    except ValueError:
        continue
    if d.get("class") == "TPV" and d.get("mode", 0) >= 2 and "lat" in d:
        print("%.6f %.6f" % (d["lat"], d["lon"]))
        sys.exit(0)
sys.exit(1)'
