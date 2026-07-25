# Troubleshooting

Every entry here is a trap we personally fell into. Learn from our
bruises.

## Radio: nothing hears anything

**The dongle factory default is 868 MHz.** Waveshare USB-TO-LoRa
dongles ship on channel 18 (868.125 MHz); the mesh lives on channel
65 (915.0 MHz). Configure every dongle once — see the
[car device guide](car-device.html#3-configure-the-lora-dongle-once-on-any-computer).

**The KEY button is a silent factory reset.** Holding a dongle's KEY
button ~2 s restores factory settings — back to 868 MHz, no
indication. If a previously-working dongle goes deaf, re-check its
channel before anything else.

**All radios must match:** channel 65, SF7, BW125. The town node's
concentrator *hears* every SF at once (so uplink may work even
mismatched!) but *transmits* SF7 — an SF-mismatched dongle hears
nothing back. If cars reach the node but never see its beacon,
suspect SF.

## Car device

**Only one USB port works.** That's the factory jumper bypassing the
onboard hub — move **H3** (see the
[build step](car-device.html#2-the-one-hardware-step-the-usb-hub-jumper)).
Note the previously-working port goes dead after the move; use the
other three.

**No GPS fix indoors.** Normal — the VK-172 wants sky. In a car it
locks in 1–2 minutes. The device works fine without a fix: reports
still send (receivers timestamp them on arrival), you just navigate
from your last known position until lock.

**Map is a grid.** No tiles for where you are. Download a coverage
tier (Settings → Wi-Fi & maps) or check the card is FAT32 with tiles
in `/tiles/{z}/{x}/{y}.png`.

**Update seemingly failed / device rebooted twice.** That's the
rollback system doing its job: a new firmware that fails its boot
self-check is automatically reverted. You're on the previous version;
nothing is broken. Check for a newer update later.

## Town node

**`journalctl -u waycast-peer` shows nothing received, ever.** Check
antenna (never run without one), SPI enabled (`raspi-config`), and
that something is actually transmitting in range — a
[bench transmitter](workstation.html#3-bench-radio-test-a-town-node-from-your-desk)
two meters away settles it in one command.

**Reports arrive but say `[rx drop]`.** The drop reason is usually the
relevance radius — the report's coordinates are too far from your
node's position. Check `NODE_LAT`/`NODE_LON` in
`/etc/waycast/node.conf` (a node that thinks it's at 0,0 drops
everything).

**Node stopped hearing after days of working.** A concentrator RX path
can wedge (rare). `sudo systemctl restart waycast-peer` clears it. An
automatic watchdog is on the roadmap.

## Serial ports on macOS

**`cat /dev/cu.usbmodem…` shows nothing** from a LoRa dongle even
though its RX LED blinks. `cat` never asserts DTR, and the dongle
won't stream without it. Use `screen /dev/cu.usbmodem… 115200`, or
any real serial terminal.

## Still stuck?

Bring the symptom (and a `journalctl` snippet or a photo of the
device's diagnostic line) to
[r/waycast](https://www.reddit.com/r/waycast/) — the diagnostic line
at the bottom of the device screen exists precisely because someone
will ask you what it says.
