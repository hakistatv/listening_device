#pragma once

#include <stddef.h>
#include <stdint.h>
#include <stdbool.h>

#define SD_MOUNT_POINT     "/sdcard"
#define SD_RECORDINGS_DIR  SD_MOUNT_POINT "/recordings"

/* Mounts the TF/SD card over 1-bit SDMMC (pins per the Waveshare
 * ESP32-S3-ePaper-1.54 demo: CLK=GPIO39, CMD=GPIO41, D0=GPIO40) and, if a
 * card is present, creates SD_RECORDINGS_DIR if it doesn't already exist.
 * Safe to call with no card inserted -- logs a warning and leaves
 * sd_card_is_mounted() false rather than failing. */
void sd_card_init(void);

bool sd_card_is_mounted(void);

/* Formats "<prefix>YYYYMMDDHHMMSS.<ext>" from the current system time into
 * out (e.g. prefix="rec_", ext="wav" -> "rec_20260806143000.wav"). prefix
 * may be NULL/empty. Uses whatever the system clock currently reads --
 * accurate once a real time source (RTC, NTP, etc.) is wired up; until
 * then the clock defaults to the Unix epoch at boot, so names will look
 * like "rec_19700101000003.wav" (counting up from boot) instead of a real
 * date. Same format either way, so callers don't need to change once time
 * sync exists. */
void sd_card_format_timestamped_name(const char *prefix, const char *ext, char *out, size_t out_size);

/* Standard PCM WAV header. Shared so anyone streaming their own WAV file
 * (e.g. a recorder that doesn't know the final size up front) can build/
 * rewrite one instead of hand-rolling the struct. */
typedef struct __attribute__((packed)) {
    char     riff[4];
    uint32_t chunk_size;
    char     wave[4];
    char     fmt[4];
    uint32_t fmt_size;
    uint16_t audio_format;
    uint16_t num_channels;
    uint32_t sample_rate;
    uint32_t byte_rate;
    uint16_t block_align;
    uint16_t bits_per_sample;
    char     data[4];
    uint32_t data_size;
} wav_header_t;

/* Builds a PCM WAV header for the given format and (already-known)
 * data_size. To finalize a WAV file whose size wasn't known up front:
 * write a placeholder header (data_size=0) before streaming samples, then
 * once done, fseek(f, 0, SEEK_SET) and fwrite() a header built with the
 * real data_size over it. */
wav_header_t sd_card_make_wav_header(uint32_t sample_rate, uint16_t channels, uint16_t bits_per_sample, uint32_t data_size);
