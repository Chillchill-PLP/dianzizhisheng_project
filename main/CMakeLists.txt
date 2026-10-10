idf_component_register(
    SRCS "main.c"
    INCLUDE_DIRS "."
    PRIV_REQUIRES
        esp_driver_gpio
        esp_driver_i2s
        esp_driver_spi
        esp_timer
        espressif__esp-sr
        fatfs
        esp_driver_sdspi
        sdmmc
)
