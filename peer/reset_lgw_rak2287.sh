#!/bin/sh
# RAK2287 (SX1302) reset for Debian trixie via pinctrl.
# RAK recipe: GPIO17 reset, sequence 0->1->0, NO power-enable pin.
RST=${RESET_PIN:-17}
pinctrl set $RST op dl; sleep 0.1
pinctrl set $RST op dh; sleep 0.1
pinctrl set $RST op dl; sleep 0.1
