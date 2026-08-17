# listening_device

ESP-IDF firmware for the Waveshare **ESP32-S3-ePaper-1.54** (V1: ESP32-S3FH4R2,
4MB Flash / 2MB PSRAM). Captures mic audio via the onboard ES8311 codec and
serves a small web page over its own Wi-Fi access point.

## Demo

[![Watch a walkthrough of the features on YouTube](https://img.youtube.com/vi/R5Ie84ewBdo/maxresdefault.jpg)](https://youtu.be/R5Ie84ewBdo?si=l8--JXbw3XhU6gSn)

## Hardware

- Board: Waveshare ESP32-S3-ePaper-1.54, V1
- Docs: https://docs.waveshare.com/ESP32-S3-ePaper-1.54 (no pinout table there
  — the pin map below came from Waveshare's own ESP-IDF demo repo,
  `github.com/waveshareteam/ESP32-S3-ePaper-1.54`,
  `02_Example/ESP-IDF/V1/08_Audio_Test/`)

| Function              | Pin(s) |
|------------------------|--------|
| I2C (codec control)    | SDA=GPIO47, SCL=GPIO48 |
| I2S (mic/codec data)   | BCLK=GPIO15, WS=GPIO38, DOUT=GPIO45, DIN=GPIO16, MCLK=GPIO14 |
| Codec power rail       | GPIO42 |
| Speaker PA enable      | GPIO46 (not driven by this firmware yet) |
| E-paper (SPI2)         | DC=GPIO10, CS=GPIO11, SCK=GPIO12, MOSI=GPIO13, RST=GPIO9, BUSY=GPIO8, PWR=GPIO6 (**active-LOW**) |
| TF/SD card (SDMMC 1-bit) | CLK=GPIO39, CMD=GPIO41, D0=GPIO40 |
| Buttons                | BOOT=GPIO0, PWR=GPIO18 |

Codec: ES8311 (mic in only, for now — no speaker output wired up). SD card
and e-paper pinouts sourced the same way, from `02_Example/ESP-IDF/V1/04_SD_Card/`
and `.../08_Audio_Test/components/epaper_driver_bsp/` in Waveshare's demo repo.

**Power-enable pin polarity, worth knowing:** Waveshare's own
`board_power_bsp.cpp` shows `EPD_PWR` (GPIO6) is **active-LOW**
(`POWEER_EPD_ON()` drives it to `0`) — confirmed and implemented that way in
`epaper.c`. The same file also shows the **audio codec's power pin
(GPIO42) is active-LOW too** (`POWEER_Audio_ON()` also drives it to `0`),
but this project's existing `audio_power_on()` in `listening_device.c`
drives GPIO42 **HIGH** to enable it — the opposite polarity. That code was
written before this was discovered and has not been changed or verified
against real mic audio on hardware. If mic capture doesn't work, this is
the first thing to check/flip.

## Features

- Boots and logs chip info (target, cores, flash size, free heap)
- Initializes the ES8311 codec in mic-capture mode over I2S/I2C. A single
  background task continuously reads mic frames (required — only one
  reader may ever hold the I2S/codec stream) and logs an RMS/peak "mic
  alive" heartbeat roughly once a second
- Starts a Wi-Fi access point (SoftAP) — connect directly to the board, no
  router needed
- Starts an HTTP server: a Home page at `/` linking to a Settings page at
  `/settings`, where you can change the Wi-Fi SSID, password, and device
  (mDNS) name from the browser — no reflash needed. Saving restarts the
  board so the new settings take effect
- Advertises itself over mDNS so the AP can be reached by hostname instead of
  IP
- Mounts the TF/SD card (FAT, 1-bit SDMMC) if one is present and creates
  `/sdcard/recordings` if it doesn't already exist, plus a 5-second 440Hz
  test-tone WAV in there the first time (filename `sample_YYYYMMDDHHMMSS.wav`),
  so there's something real to try downloading before mic-to-SD recording
  is wired up. A "Recordings" page at `/recordings` lists that folder's
  files, each filename itself being the download link; shows "No SD card
  detected" gracefully if the slot is empty. Filenames are timestamped
  using the system clock (`sd_card_format_timestamped_name()` in
  `sd_card.h`) — this board has no RTC/NTP time sync wired up yet, so
  until that exists the clock defaults to the Unix epoch at boot and
  names will look like `rec_19700101000012.wav` (counting up from boot)
  rather than a real date; same format either way, so nothing needs to
  change once time sync exists
- Drives the onboard 1.54" e-paper panel and shows "Stop" at boot.
  Pressing the **BOOT** button toggles the screen between "Stop" and
  "Listening" — see `epaper.c`/`status_display.c`. Only full-refresh is
  implemented (no partial refresh), so each toggle flashes the panel a
  few times and takes roughly 1-2 seconds — normal for e-paper, just
  worth expecting
- Pressing BOOT to go to "Listening" also starts recording mic audio to a
  new `rec_YYYYMMDDHHMMSS.wav` file in `/sdcard/recordings`; pressing it
  again to go back to "Stop" finalizes and closes that file (correct WAV
  header written after the fact, since the length isn't known up front)
  — see `recorder.c`. Shows up on the Recordings page like any other file
- A "Listening" page at `/listening` mirrors the BOOT button from the
  browser: a button reading "Listen"/"Stop" that starts/stops the same
  recording (`status_display_toggle()` is the single source of truth, so
  the physical button and this page can't desync or race each other —
  either one updates the e-paper screen and recorder state together).
  Once a recording's been stopped, its download link appears right there
  on the page, no need to go find it on Recordings
- Recordings auto-stop after a configurable duration (default 30s, 0 =
  unlimited) — a guardrail against an accidentally-forgotten recording
  filling the SD card. Enforced in `recorder.c` by byte-counting against
  the known sample rate; when it fires, `status_display_toggle()` runs the
  same way a manual Stop would, so the BOOT button, the web page, and the
  e-paper screen all reflect that it actually stopped
- Stealth Mode (default off): when on, the e-paper screen is always blanked
  instead of showing "Stop"/"Listening" — recording and everything else
  works exactly the same, the screen just doesn't advertise it. Panel is
  still actively refreshed to blank rather than left untouched, so no
  stale text lingers from before Stealth Mode was turned on

## Configuring

There are two ways to set the Wi-Fi SSID/password, device (mDNS) name, max
recording duration, and Stealth Mode:

1. **Before you build** — edit the defaults in
   [`components/device_settings/device_config.h`](components/device_settings/device_config.h):

   ```c
   #define WIFI_AP_SSID     "hakista"
   #define WIFI_AP_PASS     "hak1sta!"
   #define WIFI_AP_CHANNEL  1
   #define WIFI_AP_MAX_CONN 4

   #define MDNS_HOSTNAME    "hakista"

   #define RECORDING_MAX_DURATION_SEC 30  // 0 = no limit

   #define STEALTH_MODE_DEFAULT false
   ```

   Edit that file, then build/flash as usual — no menuconfig step needed.

2. **After flashing, from the browser** — open `/settings` on the device
   (see below) and change the SSID, password, device name, max recording
   duration (seconds; 0 = no limit), and Stealth Mode there. These are saved to
   NVS flash and take priority over `device_config.h` from then on
   (survives reflashing the app, but not `idf.py erase-flash`). Saving
   restarts the board immediately. Leave the password field blank to keep
   the current password unchanged.

   Note: settings saved this way are stored **unencrypted** in NVS — fine
   for a local device you control, but don't expose this AP's `/settings`
   endpoint beyond that.

## Quick Flash (no build tools needed)

Flash the latest prebuilt firmware straight from your browser using
[ESP Web Tools](https://esphome.github.io/esp-web-tools/) — built and
published automatically by GitHub Actions on every push to `main`.

1. Open **https://hakistatv.github.io/listening_device/** in **Chrome or
   Edge on desktop** (Web Serial isn't supported in Firefox/Safari, or on
   mobile browsers).
2. Connect the board via USB **while holding the BOOT button down** — hold
   BOOT, plug in the USB cable, then release BOOT after a second or two.
   This puts the ESP32-S3 into its serial bootloader (download) mode; without
   it, the board boots straight into whatever firmware is already on it
   instead of exposing itself for flashing, and the browser either won't see
   a usable port or the flash will fail partway through.
3. Click **Connect**, select the board's serial port, then click
   **Install**.
4. Wait for the flash to finish (roughly 30s-1min); the board reboots into
   the firmware automatically once it's done.

Then connect to the `hakista` Wi-Fi network (default password `hak1sta!`)
and browse to `http://192.168.4.1/` — see
[Connecting to the device](#connecting-to-the-device) below. Building from
source (next section) is only needed if you want to change the code.

## Building & flashing

This project uses ESP-IDF v6.0.2. Activate the toolchain, then use `idf.py`
as normal:

```sh
source ~/.espressif/tools/activate_idf_v6.0.2.sh
idf.py set-target esp32s3   # first time only
idf.py build
idf.py -p /dev/tty.usbmodem101 flash monitor
```

## Connecting to the device

1. Connect your phone/laptop's Wi-Fi to the SSID set in
   `components/device_settings/device_config.h`
   (default `hakista`), using the configured password (default `hak1sta!`).
2. Browse to either:
   - `http://192.168.4.1/` (always works — the SoftAP's fixed gateway IP), or
   - `http://<MDNS_HOSTNAME>.local/` (default `http://hakista.local/` — works
     out of the box on macOS/iOS/Linux; Windows needs Bonjour installed)

You should see the Home page, with links to Listening, Settings, and
Recordings. On
Recordings, the `sample_YYYYMMDDHHMMSS.wav` file should be downloadable and
playable (e.g. AirDrop/save-to-Files then open in a player, or Safari plays
it inline) as a quick end-to-end check that SD card + web server + your
phone's audio stack all agree with each other.

## Project layout

Each piece besides the app entry point lives in its own ESP-IDF component
under `components/`, with its own `CMakeLists.txt` declaring exactly what it
requires:

```
main/
  listening_device.c        -- app_main, mic capture, Wi-Fi AP setup
  idf_component.yml         -- managed component deps (esp_codec_dev, mdns)
  CMakeLists.txt
components/
  device_settings/          -- runtime SSID/password/hostname/max-recording-
    device_settings.c/.h       duration/stealth-mode storage (NVS-backed)
    device_config.h          -- edit Wi-Fi/mDNS defaults here before building
    CMakeLists.txt
  mdns_service/              -- mDNS advertisement (<hostname>.local)
    mdns_service.c/.h
    CMakeLists.txt
  web_server/                -- HTTP server: Home, Listening (GET+POST),
    web_server.c/.h             Settings (GET+POST), Recordings + Download, restart
    pages/                   -- every page's HTML lives here (embedded into
                                  the binary at build time, see CMakeLists.txt):
                                    home.html, listening.html, settings.html,
                                    restart.html, recordings_header.html,
                                    recordings_item.html (printf template, one
                                      row per file), recordings_empty.html,
                                    recordings_footer.html, recordings_no_card.html
    CMakeLists.txt
  sd_card/                    -- mounts the TF/SD card, creates /sdcard/recordings
    sd_card.c/.h                 and a sample WAV in it; shared wav_header_t +
                                  timestamped-filename helpers
    CMakeLists.txt
  recorder/                   -- the one task that reads the mic stream; writes
    recorder.c/.h                frames to a WAV file while recording is active
    CMakeLists.txt
  epaper/                     -- SSD1681 e-paper driver (full-refresh) + a small
    epaper.c/.h                  built-in bitmap font, ported from Waveshare's demo
    CMakeLists.txt
  status_display/             -- single source of truth for Stop/Listening state;
    status_display.c/.h          called from the BOOT button task AND the web
                                  Listening page, updates the e-paper screen and
                                  starts/stops recorder either way
    CMakeLists.txt
partitions.csv               -- custom 3MB app partition (see note below)
sdkconfig.defaults           -- flash size (4MB), PSRAM (2MB Quad), partition
                                 table for this board
```

**Partition table:** this project uses a custom `partitions.csv` (3MB app
partition) instead of ESP-IDF's default 1MB single-app table — the default
was down to 4% free after adding FAT/SDMMC support, and this board has 4MB
of flash to use. If `idf.py build` ever reports a stale/wrong partition
size after pulling changes, delete `sdkconfig` and rebuild so it
regenerates from `sdkconfig.defaults` (`sdkconfig` is a local cache, not
checked in).

