idf_component_register(
    SRCS "main.c" "ads1115.c" "io.c" "led_sr.c" "cmd.c"
    INCLUDE_DIRS "."
    PRIV_REQUIRES pfs esp_driver_gpio esp_driver_i2c esp_driver_usb_serial_jtag esp_timer esp_rom)
