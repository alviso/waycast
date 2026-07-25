# txtool — bench transmitter

Sends one Waycast hazard frame out a USB LoRa dongle from your
computer — the fastest way to verify a town node hears anything at
all, no car device needed.

    cc -o txtool -I ../../msg -I ../../net txtool.c \
       ../../msg/vmesh_wire.c ../../net/lora_dtu.c
    ./txtool /dev/cu.usbmodemXXXX 1 "hello from the bench"

Watch the town node's journal for the [rx OK] line. Edit the
coordinates in txtool.c to your area first (the node drops reports
outside their relevance radius).
