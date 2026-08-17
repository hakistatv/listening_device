#include <stdio.h>
#include <string.h>
#include <math.h>
#include <inttypes.h>
#include <time.h>
#include <dirent.h>
#include <sys/stat.h>
#include "esp_log.h"
#include "esp_vfs_fat.h"
#include "sdmmc_cmd.h"
#include "driver/sdmmc_host.h"
#include "sd_card.h"

static const char *TAG = "sd_card";

/* TF card slot pinout, sourced from Waveshare's own ESP-IDF demo
 * (github.com/waveshareteam/ESP32-S3-ePaper-1.54,
 * 02_Example/ESP-IDF/V1/04_SD_Card/components/sdcard_bsp/sdcard_bsp.c). */
#define SD_CLK_PIN 39
#define SD_CMD_PIN 41
#define SD_D0_PIN  40

static sdmmc_card_t *s_card = NULL;

bool sd_card_is_mounted(void)
{
    return s_card != NULL;
}

void sd_card_format_timestamped_name(const char *prefix, const char *ext, char *out, size_t out_size)
{
    time_t now = time(NULL);
    struct tm tm_now;
    localtime_r(&now, &tm_now);
    char ts[16];
    strftime(ts, sizeof(ts), "%Y%m%d%H%M%S", &tm_now);
    snprintf(out, out_size, "%s%s.%s", prefix ? prefix : "", ts, ext);
}

/* True if dir has no entries other than "." and "..". Used to decide
 * whether it's safe to drop in the bootstrap sample recording -- once
 * anything's in there (the sample itself, or a real recording), don't add
 * more on every boot. */
static bool dir_is_empty(const char *path)
{
    DIR *d = opendir(path);
    if (!d) {
        return true; /* couldn't open it; sd_card_init() already tried to create it */
    }
    bool empty = true;
    struct dirent *ent;
    while ((ent = readdir(d)) != NULL) {
        if (strcmp(ent->d_name, ".") == 0 || strcmp(ent->d_name, "..") == 0) {
            continue;
        }
        empty = false;
        break;
    }
    closedir(d);
    return empty;
}

#define SAMPLE_WAV_SAMPLE_RATE 16000
#define SAMPLE_WAV_SECONDS     5
#define SAMPLE_WAV_FREQ_HZ     440.0f /* A4 -- an audible, unambiguous test tone */

wav_header_t sd_card_make_wav_header(uint32_t sample_rate, uint16_t channels, uint16_t bits_per_sample, uint32_t data_size)
{
    uint16_t block_align = (uint16_t)(channels * (bits_per_sample / 8));
    wav_header_t hdr = {
        .riff = {'R', 'I', 'F', 'F'},
        .chunk_size = 36 + data_size,
        .wave = {'W', 'A', 'V', 'E'},
        .fmt = {'f', 'm', 't', ' '},
        .fmt_size = 16,
        .audio_format = 1, /* PCM */
        .num_channels = channels,
        .sample_rate = sample_rate,
        .byte_rate = sample_rate * block_align,
        .block_align = block_align,
        .bits_per_sample = bits_per_sample,
        .data = {'d', 'a', 't', 'a'},
        .data_size = data_size,
    };
    return hdr;
}

/* Writes a 5-second, 16kHz/16-bit mono WAV of a 440Hz tone into
 * SD_RECORDINGS_DIR, purely so there's something real to download and play
 * from the Recordings web page before actual mic capture is wired up to
 * the SD card. Filename is a timestamp (see sd_card_format_timestamped_name()
 * in sd_card.h for the caveat on what that means without RTC/NTP time sync).
 * Skipped if the folder already has anything in it -- a prior sample, or a
 * real recording. */
static void write_sample_recording(void)
{
    if (!dir_is_empty(SD_RECORDINGS_DIR)) {
        return;
    }

    char filename[32];
    sd_card_format_timestamped_name("sample_", "wav", filename, sizeof(filename));
    char path[sizeof(SD_RECORDINGS_DIR) + 1 + sizeof(filename)];
    snprintf(path, sizeof(path), "%s/%s", SD_RECORDINGS_DIR, filename);

    FILE *f = fopen(path, "wb");
    if (!f) {
        ESP_LOGW(TAG, "Failed to create sample recording at %s", path);
        return;
    }

    const uint32_t num_samples = SAMPLE_WAV_SAMPLE_RATE * SAMPLE_WAV_SECONDS;
    const uint32_t data_size = num_samples * sizeof(int16_t);
    wav_header_t hdr = sd_card_make_wav_header(SAMPLE_WAV_SAMPLE_RATE, 1, 16, data_size);
    fwrite(&hdr, sizeof(hdr), 1, f);

    const int16_t amplitude = 8000; /* comfortable volume, not full-scale */
    const uint32_t fade_samples = SAMPLE_WAV_SAMPLE_RATE / 10; /* 100ms fade in/out, avoids a click at the edges */
    int16_t buf[512];
    uint32_t written = 0;
    while (written < num_samples) {
        uint32_t chunk_samples = num_samples - written;
        if (chunk_samples > sizeof(buf) / sizeof(buf[0])) {
            chunk_samples = sizeof(buf) / sizeof(buf[0]);
        }
        for (uint32_t i = 0; i < chunk_samples; i++) {
            uint32_t t = written + i;
            float phase = 2.0f * (float)M_PI * SAMPLE_WAV_FREQ_HZ * (float)t / SAMPLE_WAV_SAMPLE_RATE;
            float fade = 1.0f;
            if (t < fade_samples) {
                fade = (float)t / (float)fade_samples;
            } else if (t > num_samples - fade_samples) {
                fade = (float)(num_samples - t) / (float)fade_samples;
            }
            buf[i] = (int16_t)((float)amplitude * fade * sinf(phase));
        }
        fwrite(buf, sizeof(int16_t), chunk_samples, f);
        written += chunk_samples;
    }

    fclose(f);
    ESP_LOGI(TAG, "Wrote sample recording: %s (%" PRIu32 " bytes)", path,
             data_size + (uint32_t)sizeof(hdr));
}

void sd_card_init(void)
{
    sdmmc_host_t host = SDMMC_HOST_DEFAULT();
    host.max_freq_khz = SDMMC_FREQ_HIGHSPEED;

    sdmmc_slot_config_t slot_config = SDMMC_SLOT_CONFIG_DEFAULT();
    slot_config.width = 1; /* only D0 is wired up on this board -- 1-bit mode */
    slot_config.clk = SD_CLK_PIN;
    slot_config.cmd = SD_CMD_PIN;
    slot_config.d0 = SD_D0_PIN;

    esp_vfs_fat_sdmmc_mount_config_t mount_config = {
        .format_if_mount_failed = false,
        .max_files = 5,
        .allocation_unit_size = 16 * 1024,
    };

    esp_err_t err = esp_vfs_fat_sdmmc_mount(SD_MOUNT_POINT, &host, &slot_config, &mount_config, &s_card);
    if (err != ESP_OK) {
        ESP_LOGW(TAG, "No SD card mounted (%s) -- Recordings page will report it as absent", esp_err_to_name(err));
        s_card = NULL;
        return;
    }

    ESP_LOGI(TAG, "SD card mounted at %s (%.2f GB)", SD_MOUNT_POINT,
             (double)s_card->csd.capacity / 2048.0 / 1024.0);

    struct stat st;
    if (stat(SD_RECORDINGS_DIR, &st) != 0) {
        if (mkdir(SD_RECORDINGS_DIR, 0755) == 0) {
            ESP_LOGI(TAG, "Created %s", SD_RECORDINGS_DIR);
        } else {
            ESP_LOGE(TAG, "Failed to create %s", SD_RECORDINGS_DIR);
        }
    }

    write_sample_recording();
}
