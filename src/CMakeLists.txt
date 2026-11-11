set(daq_srcs "main.c" "ads1115.c" "io.c" "led_sr.c" "cmd.c" "sd_log.c" "stream.c" "chain.c" "nau7802.c" "profile.c" "nand_test.c" "cal.c" "telem.c" "mtp_store_pfs.c" "mtp_store_sd.c" "console_io.c" "files_mode.c" "nor_ro.c")
set(daq_reqs pfs nvs_flash esp_driver_spi esp_hw_support esp_system esp_driver_gpio esp_driver_i2c esp_driver_usb_serial_jtag esp_timer esp_rom esp_driver_uart joltwallet__littlefs fatfs esp_driver_sdmmc sdmmc esp_wifi esp_netif esp_event lwip)

# Env daq_mtp only (top CMakeLists.txt sets the build property DAQ_MTP_USB): the USB files-mode code and the component mtp_usb (esp_tinyusb fork).
# In the other envs the list stays exactly as it was.
idf_build_get_property(daq_mtp_usb DAQ_MTP_USB)       # also valid in the early expansion, where plain variables do not exist
if(daq_mtp_usb)
    list(APPEND daq_srcs "usb_files.c" "mtp_vfs.c")
    list(APPEND daq_reqs mtp_usb esp_tinyusb vfs esp_hal_usb)
endif()

idf_component_register(
    SRCS ${daq_srcs}
    INCLUDE_DIRS "."
    PRIV_INCLUDE_DIRS "../server"
    PRIV_REQUIRES ${daq_reqs})
