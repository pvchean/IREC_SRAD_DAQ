idf_component_register(
    SRCS "main.c" "ads1115.c"
    INCLUDE_DIRS "."
    PRIV_REQUIRES pfs esp_driver_gpio esp_driver_i2c esp_timer)
