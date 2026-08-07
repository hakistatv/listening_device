idf_component_register(SRCS "recorder.c" "listening_device.c" "device_settings.c" "mdns_service.c" "web_server.c" "sd_card.c"
                             "epaper.c" "status_display.c"
                    INCLUDE_DIRS "."
                    REQUIRES esp_driver_gpio esp_driver_i2c esp_driver_i2s esp_codec_dev
                             esp_wifi esp_event esp_netif esp_http_server nvs_flash mdns
                             esp_driver_sdmmc sdmmc fatfs esp_driver_spi
                    PRIV_REQUIRES spi_flash
                    EMBED_TXTFILES "pages/home.html" "pages/listening.html" "pages/settings.html" "pages/restart.html"
                                   "pages/recordings_no_card.html" "pages/recordings_header.html"
                                   "pages/recordings_item.html" "pages/recordings_empty.html"
                                   "pages/recordings_footer.html")
