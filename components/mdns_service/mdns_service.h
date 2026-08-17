#pragma once

/* Advertises the board as <current_hostname>.local over mDNS (see
 * device_settings.h) so clients that support Bonjour/mDNS (macOS, iOS,
 * Linux w/ avahi; Windows needs Bonjour installed) can browse to it by
 * name instead of the AP's IP. Call after Wi-Fi is up and
 * load_runtime_config() has run. */
void start_mdns(void);
