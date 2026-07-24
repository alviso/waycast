# vmesh_peer — Pi + RAK2287 mesh peer / sniffer

Turns a Raspberry Pi with a RAK2287 (SX1302 concentrator, SPI) into a
full Waycast mesh node running the same flooding core and wire format
as the device firmware — or, with `--sniff`, into an all-SF 8-channel
development sniffer.

## Setup (on the Pi)

```sh
sudo raspi-config           # enable SPI
git clone https://github.com/Lora-net/sx1302_hal.git ~/sx1302_hal
(cd ~/sx1302_hal && make libloragw)
~/sx1302_hal/tools/reset_lgw.sh start    # RAK Pi HAT reset = GPIO17

git clone <this repo> ~/waycast
cd ~/waycast/peer && make
```

## Run

```sh
sudo ./vmesh_peer --lat 37.331 --lon -122.030               # relay peer
sudo ./vmesh_peer --sniff                                    # sniffer
sudo ./vmesh_peer --lat 37.34 --lon -122.03 --report 30     # + test hazard/30s
```

## First real mesh test (two Pis, no other hardware)

1. Pi A: `sudo ./vmesh_peer --lat 37.33 --lon -122.03 --report 20`
2. Pi B: `sudo ./vmesh_peer --lat 37.34 --lon -122.03`
3. Watch Pi B: `[rx OK] Debris ...` then `[relay] ...` — that's
   geo-ephemeral flooding on real RF.
4. Optional Pi C in sniff mode sees both the original and the relay.

## PHY profile (must match all nodes)

915.0 MHz · SF9 · BW125 · CR4/5 · sync private (0x12) · CRC on ·
IQ normal both directions.

## VERIFY-ON-PI notes

- TX gain LUT in vmesh_peer.c is one conservative entry; if TX power
  misbehaves, copy the `tx_lut` values for RAK2287 from RAK's
  `global_conf.json`.
- `rssi_offset` -215.4 is the RAK2287 typical; sniffer RSSI absolute
  values depend on it (relative behavior does not).
- sx1302_hal API drift: built against the current master; if struct
  fields moved, compare with `util_tx_test.c` / `util_pkt_logger.c`
  in the same repo — this daemon is deliberately shaped like them.
