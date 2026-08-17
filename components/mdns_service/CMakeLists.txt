idf_component_register(SRCS "mdns_service.c"
                    INCLUDE_DIRS "."
                    PRIV_REQUIRES mdns device_settings)
