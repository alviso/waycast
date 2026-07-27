idf_component_register(
    SRCS "esp32_p4_module_dev_kit.c"
    INCLUDE_DIRS "include"
    PRIV_INCLUDE_DIRS "priv_include"
    REQUIRES driver esp_lcd_touch_ft6x36
    PRIV_REQUIRES esp_lcd usb spiffs fatfs
)
