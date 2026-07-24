# targets/esp32p4 — milestone M1 (Waveshare ESP32-P4-Module-DEV-KIT)

Full ESP-IDF project. The whole `ui/` + `msg/` + `sim/` tree compiles
as-is; only `main/main.c` is device-specific (BSP display + embedded
scenario instead of SDL + a JSON file on disk).

## Build & flash

```sh
# This machine's python.org Python 3.13 has a broken default CA path, which
# breaks IDF's downloads (toolchains, managed components). Fix per shell:
export SSL_CERT_FILE=$(python3 -m certifi) REQUESTS_CA_BUNDLE=$SSL_CERT_FILE

source ~/esp/esp-idf-v5.5/export.sh      # needs IDF >= 5.3 (P4 support)
cd targets/esp32p4
idf.py set-target esp32p4
idf.py menuconfig    # Board Support Package(ESP32-P4) -> Display ->
                     #   pick your kit's panel (7" 720x1280 / 10.1" 800x1280)
idf.py build flash monitor
```

Board support comes from the registry component
[`cfscn/esp32_p4_module_dev_kit`](https://components.espressif.com/components/cfscn/esp32_p4_module_dev_kit)
(DSI panel + GT9271 touch + `esp_lvgl_port`, pulls LVGL 9). It is fetched
automatically on first build via `main/idf_component.yml`.

## Notes

- LVGL here is configured via Kconfig (`sdkconfig.defaults`), NOT the
  repo-root `lv_conf.h` (that file is SDL-target only). If the UI adds a
  font/feature, enable it in both places.
- The demo scenario is embedded in flash (`EMBED_TXTFILES`); swap the
  file in `main/CMakeLists.txt` to demo a different script.
- Expectation per handoff-spec §4: the P4+DSI+LVGL path is young. If the
  BSP misbehaves, fall back to Waveshare's wiki examples to isolate
  board vs. our code.
