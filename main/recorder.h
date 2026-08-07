#pragma once

#include <stddef.h>
#include <stdbool.h>
#include "esp_codec_dev.h"

/* Owns the single task that continuously reads frames from mic_dev (must
 * already be open -- see init_mic() in listening_device.c). It always
 * reads, whether or not a recording is in progress, so the I2S/codec
 * stream stays consistent -- exactly one task may ever read from an
 * esp_codec_dev at a time. Whether frames also get written to a WAV file
 * on the SD card is controlled by recorder_start()/recorder_stop(). Also
 * logs an RMS/peak "mic alive" heartbeat roughly once a second regardless
 * of recording state. Call once at startup. */
void recorder_init(esp_codec_dev_handle_t mic_dev);

/* Begins writing captured audio to a new timestamped WAV file in
 * SD_RECORDINGS_DIR (filename "rec_<timestamp>.wav", see
 * sd_card_format_timestamped_name()). No-op if already recording or if
 * the SD card isn't mounted/no file could be created. */
void recorder_start(void);

/* Stops writing, finalizes the WAV header with the real data size, and
 * closes the file. No-op if not currently recording. */
void recorder_stop(void);

bool recorder_is_recording(void);

/* Copies the filename (not full path -- just the basename, matching what
 * GET /download?file=<name> expects) of the most recently *completed*
 * recording into out, or an empty string if there isn't one yet this
 * boot. Only set once recorder_stop() has finished -- the file isn't a
 * valid WAV (header not finalized) until then, so a still-in-progress
 * recording's name is never exposed here. */
void recorder_get_last_filename(char *out, size_t out_size);

/* Registers a callback fired (from the internal mic-reading task, not the
 * caller of recorder_start()) exactly when a recording auto-stops because
 * it hit current_recording_max_duration_sec (see device_settings.h) --
 * NOT fired for a normal, caller-requested recorder_stop(). Typically
 * wired to status_display_toggle() so the BOOT button/web "Listening"
 * state and the e-paper screen reflect that recording actually stopped.
 * Pass NULL to clear. */
void recorder_set_auto_stop_callback(void (*callback)(void));
