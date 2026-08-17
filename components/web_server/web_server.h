#pragma once

#include "esp_http_server.h"

/* Starts the HTTP server: "/" (Home) and "/settings" (GET to view, POST to
 * change Wi-Fi SSID/password and mDNS hostname -- saves to NVS via
 * device_settings.h and restarts the board). Returns NULL on failure. */
httpd_handle_t start_webserver(void);
