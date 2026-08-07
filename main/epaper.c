#include <string.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "driver/gpio.h"
#include "driver/spi_master.h"
#include "esp_log.h"
#include "esp_heap_caps.h"
#include "epaper.h"

static const char *TAG = "epaper";

#define EPD_PIN_CS   11
#define EPD_PIN_DC   10
#define EPD_PIN_RST  9
#define EPD_PIN_BUSY 8
#define EPD_PIN_MOSI 13
#define EPD_PIN_SCK  12
#define EPD_PIN_PWR  6 /* active-LOW: 0 = panel powered on, see epaper.h */
#define EPD_SPI_HOST SPI2_HOST

#define EPD_BYTES_PER_ROW (EPD_WIDTH / 8)
#define EPD_BUFFER_LEN    (EPD_BYTES_PER_ROW * EPD_HEIGHT) /* 5000 bytes */

static spi_device_handle_t s_spi;
static uint8_t *s_buffer;

/* Full-refresh waveform LUT, copied verbatim from Waveshare's
 * epaper_driver_bsp.cpp (WF_Full_1IN54[159]). */
static const uint8_t WF_FULL_1IN54[159] = {
    0x80, 0x48, 0x40, 0x0, 0x0, 0x0, 0x0, 0x0, 0x0, 0x0, 0x0, 0x0,
    0x40, 0x48, 0x80, 0x0, 0x0, 0x0, 0x0, 0x0, 0x0, 0x0, 0x0, 0x0,
    0x80, 0x48, 0x40, 0x0, 0x0, 0x0, 0x0, 0x0, 0x0, 0x0, 0x0, 0x0,
    0x40, 0x48, 0x80, 0x0, 0x0, 0x0, 0x0, 0x0, 0x0, 0x0, 0x0, 0x0,
    0x0, 0x0, 0x0, 0x0, 0x0, 0x0, 0x0, 0x0, 0x0, 0x0, 0x0, 0x0,
    0xA, 0x0, 0x0, 0x0, 0x0, 0x0, 0x0,
    0x8, 0x1, 0x0, 0x8, 0x1, 0x0, 0x2,
    0xA, 0x0, 0x0, 0x0, 0x0, 0x0, 0x0,
    0x0, 0x0, 0x0, 0x0, 0x0, 0x0, 0x0,
    0x0, 0x0, 0x0, 0x0, 0x0, 0x0, 0x0,
    0x0, 0x0, 0x0, 0x0, 0x0, 0x0, 0x0,
    0x0, 0x0, 0x0, 0x0, 0x0, 0x0, 0x0,
    0x0, 0x0, 0x0, 0x0, 0x0, 0x0, 0x0,
    0x0, 0x0, 0x0, 0x0, 0x0, 0x0, 0x0,
    0x0, 0x0, 0x0, 0x0, 0x0, 0x0, 0x0,
    0x0, 0x0, 0x0, 0x0, 0x0, 0x0, 0x0,
    0x0, 0x0, 0x0, 0x0, 0x0, 0x0, 0x0,
    0x22, 0x22, 0x22, 0x22, 0x22, 0x22, 0x0, 0x0, 0x0,
    0x22, 0x17, 0x41, 0x0, 0x32, 0x20
};

static void set_cs(int level)  { gpio_set_level(EPD_PIN_CS, level); }
static void set_dc(int level)  { gpio_set_level(EPD_PIN_DC, level); }
static void set_rst(int level) { gpio_set_level(EPD_PIN_RST, level); }

static void read_busy(void)
{
    while (gpio_get_level(EPD_PIN_BUSY) == 1) { /* LOW: idle, HIGH: busy */
        vTaskDelay(pdMS_TO_TICKS(5));
    }
}

static void spi_send_byte(uint8_t data)
{
    spi_transaction_t t = {0};
    t.length = 8;
    t.tx_buffer = &data;
    ESP_ERROR_CHECK(spi_device_polling_transmit(s_spi, &t));
}

static void epd_send_data(uint8_t data)
{
    set_dc(1);
    set_cs(0);
    spi_send_byte(data);
    set_cs(1);
}

static void epd_send_command(uint8_t command)
{
    set_dc(0);
    set_cs(0);
    spi_send_byte(command);
    set_cs(1);
}

static void write_bytes(const uint8_t *buf, int len)
{
    set_dc(1);
    set_cs(0);
    spi_transaction_t t = {0};
    t.length = 8 * len;
    t.tx_buffer = buf;
    ESP_ERROR_CHECK(spi_device_polling_transmit(s_spi, &t));
    set_cs(1);
}

static void epd_set_windows(uint16_t xstart, uint16_t ystart, uint16_t xend, uint16_t yend)
{
    epd_send_command(0x44); /* SET_RAM_X_ADDRESS_START_END_POSITION */
    epd_send_data((xstart >> 3) & 0xFF);
    epd_send_data((xend >> 3) & 0xFF);

    epd_send_command(0x45); /* SET_RAM_Y_ADDRESS_START_END_POSITION */
    epd_send_data(ystart & 0xFF);
    epd_send_data((ystart >> 8) & 0xFF);
    epd_send_data(yend & 0xFF);
    epd_send_data((yend >> 8) & 0xFF);
}

static void epd_set_cursor(uint16_t xstart, uint16_t ystart)
{
    epd_send_command(0x4E); /* SET_RAM_X_ADDRESS_COUNTER */
    epd_send_data(xstart & 0xFF);

    epd_send_command(0x4F); /* SET_RAM_Y_ADDRESS_COUNTER */
    epd_send_data(ystart & 0xFF);
    epd_send_data((ystart >> 8) & 0xFF);
}

static void epd_set_lut(const uint8_t *lut)
{
    epd_send_command(0x32);
    write_bytes(lut, 153);
    read_busy();

    epd_send_command(0x3F);
    epd_send_data(lut[153]);

    epd_send_command(0x03);
    epd_send_data(lut[154]);

    epd_send_command(0x04);
    epd_send_data(lut[155]);
    epd_send_data(lut[156]);
    epd_send_data(lut[157]);

    epd_send_command(0x2C);
    epd_send_data(lut[158]);
}

static void epd_turn_on_display(void)
{
    epd_send_command(0x22);
    epd_send_data(0xC7);
    epd_send_command(0x20);
    read_busy();
}

static void gpio_setup(void)
{
    gpio_config_t out_conf = {
        .pin_bit_mask = (1ULL << EPD_PIN_RST) | (1ULL << EPD_PIN_DC) |
                         (1ULL << EPD_PIN_CS) | (1ULL << EPD_PIN_PWR),
        .mode = GPIO_MODE_OUTPUT,
        .pull_up_en = GPIO_PULLUP_ENABLE,
    };
    ESP_ERROR_CHECK(gpio_config(&out_conf));

    gpio_config_t busy_conf = {
        .pin_bit_mask = (1ULL << EPD_PIN_BUSY),
        .mode = GPIO_MODE_INPUT,
    };
    ESP_ERROR_CHECK(gpio_config(&busy_conf));

    gpio_set_level(EPD_PIN_PWR, 0); /* active-low: power the panel on */
    set_rst(1);
}

static void spi_setup(void)
{
    spi_bus_config_t bus_cfg = {
        .miso_io_num = -1,
        .mosi_io_num = EPD_PIN_MOSI,
        .sclk_io_num = EPD_PIN_SCK,
        .quadwp_io_num = -1,
        .quadhd_io_num = -1,
        .max_transfer_sz = EPD_WIDTH * EPD_HEIGHT,
    };
    ESP_ERROR_CHECK(spi_bus_initialize(EPD_SPI_HOST, &bus_cfg, SPI_DMA_CH_AUTO));

    spi_device_interface_config_t dev_cfg = {
        .mode = 0,
        .clock_speed_hz = 40 * 1000 * 1000, /* matches Waveshare's tested reference value */
        .spics_io_num = -1,
        .queue_size = 7,
    };
    ESP_ERROR_CHECK(spi_bus_add_device(EPD_SPI_HOST, &dev_cfg, &s_spi));
}

void epd_init(void)
{
    s_buffer = heap_caps_malloc(EPD_BUFFER_LEN, MALLOC_CAP_SPIRAM);
    if (!s_buffer) {
        ESP_LOGE(TAG, "Failed to allocate %d-byte framebuffer -- display disabled", EPD_BUFFER_LEN);
        return;
    }

    gpio_setup();
    spi_setup();

    set_rst(1);
    vTaskDelay(pdMS_TO_TICKS(50));
    set_rst(0);
    vTaskDelay(pdMS_TO_TICKS(20));
    set_rst(1);
    vTaskDelay(pdMS_TO_TICKS(50));

    read_busy();
    epd_send_command(0x12); /* SWRESET */
    read_busy();

    epd_send_command(0x01); /* driver output control */
    epd_send_data(0xC7);
    epd_send_data(0x00);
    epd_send_data(0x01);

    epd_send_command(0x11); /* data entry mode */
    epd_send_data(0x01);

    epd_set_windows(0, EPD_WIDTH - 1, EPD_HEIGHT - 1, 0);

    epd_send_command(0x3C); /* border waveform */
    epd_send_data(0x01);

    epd_send_command(0x18);
    epd_send_data(0x80);

    epd_send_command(0x22); /* load temperature + waveform setting */
    epd_send_data(0xB1);
    epd_send_command(0x20);

    epd_set_cursor(0, EPD_HEIGHT - 1);
    read_busy();

    epd_set_lut(WF_FULL_1IN54);

    ESP_LOGI(TAG, "Display initialized");
}

void epd_clear(void)
{
    if (s_buffer) {
        memset(s_buffer, 0xFF, EPD_BUFFER_LEN);
    }
}

void epd_draw_pixel(uint16_t x, uint16_t y, epd_color_t color)
{
    if (!s_buffer || x >= EPD_WIDTH || y >= EPD_HEIGHT) {
        return;
    }
    uint16_t index = y * EPD_BYTES_PER_ROW + (x >> 3);
    uint8_t bit = 7 - (x & 0x07);
    if (color == EPD_COLOR_WHITE) {
        s_buffer[index] |= (uint8_t)(1 << bit);
    } else {
        s_buffer[index] &= (uint8_t)~(1 << bit);
    }
}

void epd_display(void)
{
    if (!s_buffer) {
        return;
    }
    epd_send_command(0x24);
    write_bytes(s_buffer, EPD_BUFFER_LEN);
    epd_turn_on_display();
}

/*
 * Minimal 5x7 bitmap font -- only covers the characters this project's two
 * status strings ("Stop"/"Listening") use. Each row is 5 bits (bit4 =
 * leftmost column .. bit0 = rightmost), 7 rows top-to-bottom. Hand-drawn
 * for legibility at small pixel counts, not tracing any existing font.
 */
typedef struct {
    char ch;
    uint8_t rows[7];
} font_glyph_t;

static const font_glyph_t FONT_5X7[] = {
    { 'S', { 0x0E, 0x11, 0x10, 0x0E, 0x01, 0x11, 0x0E } },
    { 'L', { 0x10, 0x10, 0x10, 0x10, 0x10, 0x10, 0x1F } },
    { 't', { 0x08, 0x08, 0x1E, 0x08, 0x08, 0x09, 0x06 } },
    { 'o', { 0x00, 0x00, 0x0E, 0x11, 0x11, 0x11, 0x0E } },
    { 'p', { 0x00, 0x00, 0x1E, 0x11, 0x1E, 0x10, 0x10 } },
    { 'i', { 0x08, 0x00, 0x18, 0x08, 0x08, 0x08, 0x1C } },
    { 's', { 0x00, 0x00, 0x0F, 0x10, 0x0E, 0x01, 0x1E } },
    { 'e', { 0x00, 0x00, 0x0E, 0x11, 0x1F, 0x10, 0x0E } },
    { 'n', { 0x00, 0x00, 0x16, 0x19, 0x11, 0x11, 0x11 } },
    { 'g', { 0x00, 0x00, 0x0F, 0x11, 0x0F, 0x01, 0x1E } },
};
#define FONT_GLYPH_COUNT (sizeof(FONT_5X7) / sizeof(FONT_5X7[0]))

static const font_glyph_t *find_glyph(char c)
{
    for (size_t i = 0; i < FONT_GLYPH_COUNT; i++) {
        if (FONT_5X7[i].ch == c) {
            return &FONT_5X7[i];
        }
    }
    return NULL;
}

static void draw_glyph(uint16_t x, uint16_t y, const font_glyph_t *g, epd_color_t color, uint8_t scale)
{
    for (int row = 0; row < 7; row++) {
        for (int col = 0; col < 5; col++) {
            if (g->rows[row] & (1 << (4 - col))) {
                for (int sy = 0; sy < scale; sy++) {
                    for (int sx = 0; sx < scale; sx++) {
                        epd_draw_pixel((uint16_t)(x + col * scale + sx), (uint16_t)(y + row * scale + sy), color);
                    }
                }
            }
        }
    }
}

void epd_draw_text_centered(const char *text, epd_color_t color, uint8_t scale)
{
    size_t len = strlen(text);
    if (len == 0 || scale == 0) {
        return;
    }

    uint16_t char_cell = (uint16_t)((5 + 1) * scale); /* glyph width + 1 column gap, scaled */
    uint16_t text_width = (uint16_t)(len * char_cell - scale); /* no trailing gap after the last char */
    uint16_t text_height = (uint16_t)(7 * scale);

    uint16_t x = (text_width < EPD_WIDTH) ? (uint16_t)((EPD_WIDTH - text_width) / 2) : 0;
    uint16_t y = (text_height < EPD_HEIGHT) ? (uint16_t)((EPD_HEIGHT - text_height) / 2) : 0;

    for (size_t i = 0; i < len; i++) {
        const font_glyph_t *g = find_glyph(text[i]);
        if (g) {
            draw_glyph(x, y, g, color, scale);
        }
        x = (uint16_t)(x + char_cell);
    }
}
