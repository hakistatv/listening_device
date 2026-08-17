#pragma once

/*
 * Central place to configure the device before building.
 * Edit these values, then run `idf.py build flash`.
 */

/* --- Wi-Fi access point --- */
#define WIFI_AP_SSID     "hakista"
#define WIFI_AP_PASS     "hak1sta!"
#define WIFI_AP_CHANNEL  1
#define WIFI_AP_MAX_CONN 4

/* --- mDNS --- */
/* Resolves as http://<MDNS_HOSTNAME>.local -- plain "hakista" (no .local)
 * isn't a valid browser hostname. */
#define MDNS_HOSTNAME    "hakista"

/* --- Recording guardrail --- */
/* Recordings auto-stop after this many seconds (see recorder.c). 0 means
 * no limit -- recording only stops when the user presses Stop. */
#define RECORDING_MAX_DURATION_SEC 30

/* --- Stealth mode --- */
/* When true, the e-paper screen stays blank (no "Stop"/"Listening" text)
 * regardless of recording state -- see status_display.c. */
#define STEALTH_MODE_DEFAULT false
