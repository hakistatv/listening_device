#pragma once

#include <stdint.h>

#define EPD_WIDTH  200
#define EPD_HEIGHT 200

typedef enum {
    EPD_COLOR_WHITE = 0xFF,
    EPD_COLOR_BLACK = 0x00,
} epd_color_t;

/*
 * SSD1681-class 1.54" 200x200 e-paper panel driver, ported to C from
 * Waveshare's own C++ demo (github.com/waveshareteam/ESP32-S3-ePaper-1.54,
 * 02_Example/ESP-IDF/V1/08_Audio_Test/components/epaper_driver_bsp) -- SPI
 * setup, the SSD1681 command sequence, and the waveform LUT table are
 * copied verbatim from that reference. Only full-refresh is implemented
 * here (no partial refresh) -- a full update takes roughly 1-2 seconds and
 * flashes the panel black/white a few times, which is normal for e-paper.
 *
 * Pins (SPI2_HOST): DC=GPIO10, CS=GPIO11, SCK=GPIO12, MOSI=GPIO13,
 * RST=GPIO9, BUSY=GPIO8, power-enable=GPIO6. The power-enable pin is
 * active-LOW -- confirmed from Waveshare's board_power_bsp.cpp, where
 * POWEER_EPD_ON() drives it to 0, POWEER_EPD_OFF() drives it to 1.
 */

/* Powers the panel, brings up SPI + the SSD1681 controller, and allocates
 * the internal 5000-byte (200x200/8) framebuffer. Call once at startup. */
void epd_init(void);

/* Fills the framebuffer white. Doesn't touch the panel -- call epd_display()
 * after to push it. */
void epd_clear(void);

void epd_draw_pixel(uint16_t x, uint16_t y, epd_color_t color);

/* Draws text using a built-in 5x7 bitmap font that only covers the
 * characters this project's status strings actually use
 * (S,t,o,p,L,i,s,e,n,g -- see epaper.c). Any other character is silently
 * skipped. Horizontally and vertically centered on the panel. */
void epd_draw_text_centered(const char *text, epd_color_t color, uint8_t scale);

/* Pushes the framebuffer to the panel with a full refresh. */
void epd_display(void);
