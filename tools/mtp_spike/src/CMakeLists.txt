idf_component_register(
    SRCS "main.c" "fake_vfs.c"
    INCLUDE_DIRS "."
    PRIV_REQUIRES esp_driver_gpio esp_system esp_hw_support esp_rom esp_timer nvs_flash vfs esp_hal_usb)
