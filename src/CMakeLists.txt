idf_component_register(
    SRCS "main.c" "ads1115.c" "io.c" "led_sr.c" "cmd.c" "sd_log.c" "stream.c" "chain.c" "nau7802.c" "profile.c" "nand_test.c" "cal.c" "telem.c" "mtp_store_pfs.c" "mtp_store_sd.c"
    INCLUDE_DIRS "."
    PRIV_INCLUDE_DIRS "../server"
    PRIV_REQUIRES pfs nvs_flash esp_driver_spi esp_hw_support esp_system esp_driver_gpio esp_driver_i2c esp_driver_usb_serial_jtag esp_timer esp_rom esp_driver_uart fatfs esp_driver_sdmmc sdmmc esp_wifi esp_netif esp_event lwip)
