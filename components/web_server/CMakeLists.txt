idf_component_register(SRCS "web_server.c"
                    INCLUDE_DIRS "."
                    REQUIRES esp_http_server
                    PRIV_REQUIRES device_settings recorder sd_card status_display build_info
                    EMBED_TXTFILES "pages/home.html" "pages/listening.html" "pages/settings.html" "pages/restart.html"
                                   "pages/recordings_no_card.html" "pages/recordings_header.html"
                                   "pages/recordings_item.html" "pages/recordings_empty.html"
                                   "pages/recordings_footer.html" "pages/style.css")
