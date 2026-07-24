# targets/esp32p4 — ESP-IDF project for the Waveshare ESP32-P4-Module-DEV-KIT.
# Build:  idf.py set-target esp32p4 && idf.py build flash monitor
# Panel:  idf.py menuconfig -> Board Support Package(ESP32-P4) -> Display
#         -> select your kit's LCD (7" 720x1280 or 10.1" 800x1280).
cmake_minimum_required(VERSION 3.16)

include($ENV{IDF_PATH}/tools/cmake/project.cmake)

# Version = git describe (hash-based until we start tagging releases).
# Lands in esp_app_get_description()->version — the identity OTA
# compares against the published manifest (docs/OTA.md).
execute_process(
    COMMAND git describe --tags --always --dirty
    WORKING_DIRECTORY ${CMAKE_CURRENT_SOURCE_DIR}
    OUTPUT_VARIABLE PROJECT_VER
    OUTPUT_STRIP_TRAILING_WHITESPACE
    ERROR_QUIET)
if(NOT PROJECT_VER)
    set(PROJECT_VER "0.0.0-nogit")
endif()

project(vmesh)
