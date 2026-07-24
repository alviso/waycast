set(REPO ${CMAKE_CURRENT_LIST_DIR}/../../..)

idf_component_register(
    SRCS
        main.c
        usb_serial.cpp
        radio_bonnet.cpp
        gps_uart.c
        gps_sniff.c
        wifi_mgr.c
        tile_fetch.c
        fw_update.c
        ${REPO}/net/provision.c
        ${REPO}/ui/settings.c
        ${REPO}/net/nmea.c
        ${REPO}/net/lora_dtu.c
        ${REPO}/ui/ui.c
        ${REPO}/ui/town_square.c
        ${REPO}/ui/map_view.c
        ${REPO}/ui/hazard_store.c
        ${REPO}/msg/vmesh_wire.c
        ${REPO}/mesh/vmesh_mesh.c
        ${REPO}/sim/scenario.c
        ${REPO}/third_party/cJSON/cJSON.c
        ${REPO}/third_party/lodepng/lodepng.c
    INCLUDE_DIRS
        ${REPO}/msg
        ${REPO}/mesh
        ${REPO}/net
        ${REPO}/ui
        ${REPO}/sim
        ${REPO}/third_party
    EMBED_TXTFILES
        ${REPO}/sim/scenarios/highway_demo.json
)

# vendored lodepng trips GCC-14 array-bounds false positives at -O2
set_source_files_properties(${REPO}/third_party/lodepng/lodepng.c
    PROPERTIES COMPILE_OPTIONS "-Wno-array-bounds;-Wno-stringop-overflow")

# Bake the demo tiles into the "storage" partition so the device demos
# with zero setup (no TF card). assets_device/ (the trimmed demo-bbox
# subset — scripts/make_device_tiles.py) becomes the FS root ->
# /spiffs/tiles/{z}/{x}/{y}.png at runtime.
if(EXISTS ${REPO}/assets_device/tiles)
    spiffs_create_partition_image(storage ${REPO}/assets_device
                                  FLASH_IN_PROJECT)
else()
    message(WARNING
        "assets_device/tiles missing (run scripts/fetch_demo_tiles.py "
        "then scripts/make_device_tiles.py) — flashing without embedded "
        "map tiles")
endif()
