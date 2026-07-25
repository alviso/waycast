# Build Waycast

Waycast is an open mesh network for cars — road reports over LoRa
radio, no internet, no accounts. Everything here is off-the-shelf
hardware and open (GPLv3) firmware:
[github.com/alviso/waycast](https://github.com/alviso/waycast).

Pick your build:

## 🚗 [Car device](car-device.html)
The in-car unit: touchscreen, offline maps, GPS, LoRa. **~$120 in
parts, an afternoon of work.** No soldering — the only "mod" is moving
one jumper cap.

## 📡 [Town node](town-node.html)
The anchor that gives your town a memory: hears every report in range,
re-broadcasts them, injects official weather alerts, and announces
itself to passing cars. **A Raspberry Pi and a LoRa concentrator.**

## 💻 [Workstation](workstation.html)
No hardware yet? Run the full device UI as a desktop simulator,
pre-fetch map tiles for a card, or turn a $25 LoRa dongle on your
computer into a bench transmitter to test your town node.

## 🔧 [Troubleshooting](troubleshooting.html)
The traps we already fell into so you don't have to — factory radio
profiles, deceptive jumpers, silent buttons that factory-reset things.

---

### How the pieces talk

```
  car device  ⇄  car device        (direct, when in range)
  car device  ⇄  town node         (reports up, town memory + beacon down)
  town node   →  weather alerts    (NWS injected into the mesh)
```

All radios share one profile: **915.0 MHz (channel 65), SF7, BW125**,
US ISM band. Reports are 29-byte frames with a lifespan and a radius —
when either runs out, the report is gone everywhere. Postcards, not
chat.

Questions? [r/waycast](https://www.reddit.com/r/waycast/) is the
campfire.
