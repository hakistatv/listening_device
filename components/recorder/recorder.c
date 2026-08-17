#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <math.h>
#include <inttypes.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/semphr.h"
#include "esp_log.h"
#include "device_settings.h"
#include "sd_card.h"
#include "recorder.h"

static const char *TAG = "recorder";

#define REC_SAMPLE_RATE     16000
#define REC_BITS_PER_SAMPLE 16
#define REC_CHANNELS        1
#define REC_FRAME_SAMPLES   512
#define REC_LOG_EVERY_N_FRAMES 32 /* ~1s at 16kHz/512-sample frames */

static esp_codec_dev_handle_t s_mic_dev;
static SemaphoreHandle_t s_lock; /* guards s_file/s_data_bytes/s_recording/s_current_filename/s_last_filename */
static volatile bool s_recording = false;
static FILE *s_file = NULL;
static uint32_t s_data_bytes = 0;
static char s_current_filename[32];
static char s_last_filename[32];
static void (*s_auto_stop_cb)(void) = NULL;

bool recorder_is_recording(void)
{
    return s_recording;
}

void recorder_set_auto_stop_callback(void (*callback)(void))
{
    s_auto_stop_cb = callback;
}

void recorder_get_last_filename(char *out, size_t out_size)
{
    if (out_size == 0) {
        return;
    }
    if (!s_lock) {
        out[0] = '\0';
        return;
    }
    xSemaphoreTake(s_lock, portMAX_DELAY);
    strlcpy(out, s_last_filename, out_size);
    xSemaphoreGive(s_lock);
}

/* Caller must hold s_lock. */
static void open_new_file_locked(void)
{
    if (!sd_card_is_mounted()) {
        ESP_LOGW(TAG, "No SD card -- can't start recording");
        return;
    }

    sd_card_format_timestamped_name("rec_", "wav", s_current_filename, sizeof(s_current_filename));
    char path[sizeof(SD_RECORDINGS_DIR) + 1 + sizeof(s_current_filename)];
    snprintf(path, sizeof(path), "%s/%s", SD_RECORDINGS_DIR, s_current_filename);

    FILE *f = fopen(path, "wb");
    if (!f) {
        ESP_LOGE(TAG, "Failed to create %s", path);
        return;
    }

    wav_header_t hdr = sd_card_make_wav_header(REC_SAMPLE_RATE, REC_CHANNELS, REC_BITS_PER_SAMPLE, 0);
    fwrite(&hdr, sizeof(hdr), 1, f);

    s_file = f;
    s_data_bytes = 0;
    ESP_LOGI(TAG, "Recording started: %s", path);
}

/* Caller must hold s_lock. */
static void close_file_locked(void)
{
    if (!s_file) {
        return;
    }
    wav_header_t hdr = sd_card_make_wav_header(REC_SAMPLE_RATE, REC_CHANNELS, REC_BITS_PER_SAMPLE, s_data_bytes);
    fseek(s_file, 0, SEEK_SET);
    fwrite(&hdr, sizeof(hdr), 1, s_file);
    fclose(s_file);
    ESP_LOGI(TAG, "Recording stopped (%" PRIu32 " bytes of audio)", s_data_bytes);
    s_file = NULL;
    s_data_bytes = 0;
    strlcpy(s_last_filename, s_current_filename, sizeof(s_last_filename));
}

void recorder_start(void)
{
    if (s_recording || !s_lock) {
        return; /* !s_lock: recorder_init() hasn't run yet (e.g. mic still initializing at boot) */
    }
    xSemaphoreTake(s_lock, portMAX_DELAY);
    open_new_file_locked();
    if (s_file) {
        s_recording = true;
    }
    xSemaphoreGive(s_lock);
}

void recorder_stop(void)
{
    if (!s_recording || !s_lock) {
        return;
    }
    xSemaphoreTake(s_lock, portMAX_DELAY);
    s_recording = false;
    close_file_locked();
    xSemaphoreGive(s_lock);
}

/* Single continuous reader of the mic stream: always reads (required for
 * the I2S/codec stream to stay healthy), conditionally writes to the open
 * WAV file, and periodically logs an RMS/peak heartbeat either way. */
static void mic_task(void *arg)
{
    (void)arg;
    int16_t *buf = malloc(REC_FRAME_SAMPLES * sizeof(int16_t));
    if (!buf) {
        ESP_LOGE(TAG, "Failed to allocate mic frame buffer");
        vTaskDelete(NULL);
        return;
    }

    uint32_t frame_count = 0;
    while (1) {
        int ret = esp_codec_dev_read(s_mic_dev, buf, REC_FRAME_SAMPLES * sizeof(int16_t));
        if (ret != ESP_CODEC_DEV_OK) {
            ESP_LOGW(TAG, "mic read failed: %d", ret);
            vTaskDelay(pdMS_TO_TICKS(100));
            continue;
        }

        bool hit_duration_limit = false;
        xSemaphoreTake(s_lock, portMAX_DELAY);
        if (s_recording && s_file) {
            size_t written = fwrite(buf, 1, REC_FRAME_SAMPLES * sizeof(int16_t), s_file);
            s_data_bytes += (uint32_t)written;

            if (current_recording_max_duration_sec > 0) {
                uint32_t byte_rate = REC_SAMPLE_RATE * REC_CHANNELS * (REC_BITS_PER_SAMPLE / 8);
                if (s_data_bytes >= current_recording_max_duration_sec * byte_rate) {
                    s_recording = false;
                    close_file_locked();
                    hit_duration_limit = true;
                }
            }
        }
        xSemaphoreGive(s_lock);

        if (hit_duration_limit) {
            ESP_LOGI(TAG, "Recording auto-stopped: reached max duration (%" PRIu32 "s)",
                     current_recording_max_duration_sec);
            if (s_auto_stop_cb) {
                s_auto_stop_cb();
            }
        }

        if (++frame_count >= REC_LOG_EVERY_N_FRAMES) {
            frame_count = 0;
            int64_t sum_sq = 0;
            int16_t peak = 0;
            for (int i = 0; i < REC_FRAME_SAMPLES; i++) {
                int16_t s = buf[i];
                sum_sq += (int32_t)s * (int32_t)s;
                int16_t abs_s = s < 0 ? (int16_t)(-s) : s;
                if (abs_s > peak) {
                    peak = abs_s;
                }
            }
            double rms = sqrt((double)sum_sq / REC_FRAME_SAMPLES);
            ESP_LOGI(TAG, "mic alive: rms=%.0f peak=%d%s", rms, peak, s_recording ? " (recording)" : "");
        }
    }
}

void recorder_init(esp_codec_dev_handle_t mic_dev)
{
    s_mic_dev = mic_dev;
    s_lock = xSemaphoreCreateMutex();
    xTaskCreate(mic_task, "recorder_mic", 4096, NULL, 5, NULL);
}
