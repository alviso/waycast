#!/bin/bash
# Waycast town-node provisioning — vanilla Raspberry Pi OS Lite (trixie)
# -> appliance running the mesh peer + NWS injector under systemd.
#
# Run ON the Pi, as the login user (needs sudo), with the repo synced
# to ~/waycast (rsync from the dev machine: mesh/ msg/ net/ peer/):
#
#   rsync -a --exclude .git mesh msg net peer waycast@<pi>:~/waycast/
#   ssh waycast@<pi> 'bash ~/waycast/peer/setup.sh'
#
# Idempotent: safe to re-run after a repo sync to rebuild + restart.
set -euo pipefail

REPO=${REPO:-$HOME/waycast}
HAL=${HAL:-$HOME/sx1302_hal}
PEER=$REPO/peer
CONFIG_TXT=/boot/firmware/config.txt
NEED_REBOOT=0

say() { echo "== $*"; }

# --- 1. packages ------------------------------------------------------
say "packages"
need=""
for p in git build-essential python3 gpsd gpsd-clients chrony; do
    dpkg -s "$p" >/dev/null 2>&1 || need="$need $p"
done
if [ -n "$need" ]; then
    sudo apt-get update -qq
    # shellcheck disable=SC2086
    sudo apt-get install -y -qq $need
fi

# --- 2. SPI overlay ---------------------------------------------------
say "SPI"
if ! grep -q "^dtparam=spi=on" $CONFIG_TXT; then
    echo "dtparam=spi=on" | sudo tee -a $CONFIG_TXT >/dev/null
    NEED_REBOOT=1
fi

# --- 3. survivability (SD-card-friendly unattended node) --------------
say "survivability: journald->RAM, swap off, hw watchdog"
sudo mkdir -p /etc/systemd/journald.conf.d
printf '[Journal]\nStorage=volatile\nRuntimeMaxUse=32M\n' |
    sudo tee /etc/systemd/journald.conf.d/waycast.conf >/dev/null
if systemctl is-enabled dphys-swapfile >/dev/null 2>&1; then
    sudo systemctl disable --now dphys-swapfile >/dev/null 2>&1 || true
fi
sudo mkdir -p /etc/systemd/system.conf.d
printf '[Manager]\nRuntimeWatchdogSec=15\nRebootWatchdogSec=2min\n' |
    sudo tee /etc/systemd/system.conf.d/waycast-watchdog.conf >/dev/null

# --- 3b. GPS: HAT u-blox on the Pi UART + gpsd + GPS-disciplined time
say "GPS: UART, console eviction, gpsd, chrony refclock"
CMDLINE=/boot/firmware/cmdline.txt
if ! grep -q "^enable_uart=1" $CONFIG_TXT; then
    echo "enable_uart=1" | sudo tee -a $CONFIG_TXT >/dev/null
    NEED_REBOOT=1
fi
# the serial console spams the GPS with login prompts — evict it
if grep -qE "console=(serial0|ttyS0|ttyAMA0),[0-9]+ ?" $CMDLINE; then
    sudo sed -i -E "s/console=(serial0|ttyS0|ttyAMA0),[0-9]+ ?//g" $CMDLINE
    NEED_REBOOT=1
fi
sudo systemctl disable serial-getty@ttyS0.service \
    serial-getty@serial0.service >/dev/null 2>&1 || true
# gpsd owns the port (-n: read without waiting for clients)
printf 'START_DAEMON="true"\nUSBAUTO="false"\nDEVICES="/dev/ttyS0"\nGPSD_OPTIONS="-n"\n' |
    sudo tee /etc/default/gpsd >/dev/null
sudo systemctl enable gpsd >/dev/null 2>&1
# chrony: GPS NMEA time via gpsd SHM — NTP wins online, GPS carries
# the clock offline, no fix = chrony coasts. No PPS wire, so ~0.1 s.
printf 'refclock SHM 0 refid GPS precision 1e-1 offset 0.1 delay 0.2\n' |
    sudo tee /etc/chrony/conf.d/waycast-gps.conf >/dev/null

# --- 4. sx1302_hal with RAK2287 fixes --------------------------------
say "sx1302_hal"
if [ ! -d "$HAL" ]; then
    git clone --depth 1 https://github.com/Lora-net/sx1302_hal "$HAL"
fi
# patch: no-temp-sensor tolerance (RAK2287 has no STTS751)
if ! grep -q "RAK2287 has none" "$HAL/libloragw/src/loragw_hal.c"; then
    (cd "$HAL/libloragw/src" && patch loragw_hal.c <"$PEER/rak2287_hal.patch")
fi
make -C "$HAL/libloragw" -j"$(nproc)" >/dev/null

# --- 5. reset script (trixie: pinctrl, not /sys/class/gpio) -----------
say "reset script"
install -m 0755 "$PEER/reset_lgw_rak2287.sh" "$HOME/reset_lgw_rak2287.sh"

# --- 6. peer ----------------------------------------------------------
say "peer build"
make -C "$PEER" SX1302_HAL="$HAL" vmesh_peer >/dev/null

# --- 7. node config ---------------------------------------------------
say "node config"
sudo mkdir -p /etc/waycast
if [ ! -f /etc/waycast/node.conf ]; then
    sudo install -m 0644 "$PEER/node.conf.example" /etc/waycast/node.conf
    echo "   >> EDIT /etc/waycast/node.conf with this node's lat/lon <<"
fi

# --- 8. systemd units -------------------------------------------------
say "systemd units"
sudo install -m 0644 "$PEER/waycast-peer.service" \
    "$PEER/waycast-injector.service" /etc/systemd/system/
sudo systemctl daemon-reload
sudo systemctl enable waycast-peer waycast-injector >/dev/null

if [ "$NEED_REBOOT" = 1 ]; then
    say "boot config changed (SPI/UART/console) — REBOOT REQUIRED, then services start alone"
else
    sudo systemctl restart gpsd chrony waycast-peer waycast-injector
    say "services (re)started"
fi

systemctl --no-pager --plain status waycast-peer waycast-injector |
    grep -E "waycast-|Active:" || true
say "done"
