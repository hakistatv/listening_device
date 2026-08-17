idf_component_register(SRCS "listening_device.c"
                    INCLUDE_DIRS "."
                    REQUIRES esp_driver_gpio esp_driver_i2c esp_driver_i2s esp_codec_dev
                             esp_wifi esp_event esp_netif
                             device_settings epaper mdns_service recorder sd_card status_display web_server
                    PRIV_REQUIRES spi_flash)
