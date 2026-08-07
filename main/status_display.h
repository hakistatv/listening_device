#pragma once

#include <stdbool.h>

/* Draws the initial "Stop" screen and starts a task that watches the BOOT
 * button (GPIO0): each press calls status_display_toggle(). Call after
 * epd_init(). */
void status_display_init(void);

/* Toggles between Stop/Listening: updates the e-paper screen and starts/
 * stops the recorder (see recorder.h) to match. Thread-safe -- this is the
 * single source of truth for that state, callable from the BOOT button
 * task or an HTTP handler (e.g. the web "Listening" page) without the two
 * racing each other. */
void status_display_toggle(void);

bool status_display_is_listening(void);
