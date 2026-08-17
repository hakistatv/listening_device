idf_component_register(SRCS "recorder.c"
                    INCLUDE_DIRS "."
                    REQUIRES esp_codec_dev
                    PRIV_REQUIRES device_settings sd_card)
