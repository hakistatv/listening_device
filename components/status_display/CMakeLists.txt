idf_component_register(SRCS "status_display.c"
                    INCLUDE_DIRS "."
                    PRIV_REQUIRES esp_driver_gpio device_settings epaper recorder)
