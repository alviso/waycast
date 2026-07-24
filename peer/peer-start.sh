#!/bin/bash
# Resolve the node location (GPS preferred, node.conf fallback), publish
# it for other services, then exec the mesh peer.
set -u
HERE=$(dirname "$(readlink -f "$0")")
. /etc/waycast/node.conf
DIR=${RUNTIME_DIRECTORY:-/run/waycast}

if loc=$("$HERE/gpsfix.sh"); then
    # shellcheck disable=SC2086
    set -- $loc
    NODE_LAT=$1 NODE_LON=$2 SRC=gps
    echo "[location] gps fix: $NODE_LAT,$NODE_LON"
else
    SRC=conf
    echo "[location] no gps fix in ${GPS_WAIT_S:-30}s — node.conf: $NODE_LAT,$NODE_LON"
fi
mkdir -p "$DIR"
echo "$NODE_LAT $NODE_LON $SRC" >"$DIR/location"

exec "$HERE/vmesh_peer" --lat "$NODE_LAT" --lon "$NODE_LON" --name "${NODE_NAME:-anchor}"
