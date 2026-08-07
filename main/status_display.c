#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/semphr.h"
#include "driver/gpio.h"
#include "esp_log.h"
#include "device_settings.h"
#include "epaper.h"
#include "recorder.h"
#include "status_display.h"

static const char *TAG = "status_display";

/* BOOT button: active-low with internal pull-up, same as Waveshare's own
 * button_bsp. Safe to reconfigure as a plain input after boot -- GPIO0's
 * bootstrap role only matters during power-on/reset, not once the app is
 * already running. */
#define BOOT_BUTTON_PIN 0
#define DEBOUNCE_MS     50
#define POLL_MS         20
#define TEXT_SCALE      3

typedef enum {
    STATUS_STOPPED,
    STATUS_LISTENING,
} status_state_t;

static SemaphoreHandle_t s_lock;
static status_state_t s_state = STATUS_STOPPED;

/* Caller must hold s_lock. Always blanks and refreshes the panel -- when
 * stealth mode is on, that means pushing an actually-blank screen (not
 * just skipping the update), so no stale "Listening" text is left showing
 * from before stealth mode was turned on. */
static void render_locked(void)
{
    epd_clear();
    if (!current_stealth_mode) {
        epd_draw_text_centered(s_state == STATUS_LISTENING ? "Listening" : "Stop", EPD_COLOR_BLACK, TEXT_SCALE);
    }
    epd_display();
}

/* Caller must hold s_lock. */
static void apply_state_locked(status_state_t new_state)
{
    if (new_state == s_state) {
        return;
    }
    s_state = new_state;
    if (s_state == STATUS_LISTENING) {
        recorder_start();
    } else {
        recorder_stop();
    }
    render_locked();
    ESP_LOGI(TAG, "Status -> %s", s_state == STATUS_LISTENING ? "Listening" : "Stop");
}

void status_display_toggle(void)
{
    xSemaphoreTake(s_lock, portMAX_DELAY);
    apply_state_locked(s_state == STATUS_STOPPED ? STATUS_LISTENING : STATUS_STOPPED);
    xSemaphoreGive(s_lock);
}

bool status_display_is_listening(void)
{
    return s_state == STATUS_LISTENING;
}

static void button_task(void *arg)
{
    (void)arg;

    gpio_config_t io_conf = {
        .pin_bit_mask = 1ULL << BOOT_BUTTON_PIN,
        .mode = GPIO_MODE_INPUT,
        .pull_up_en = GPIO_PULLUP_ENABLE,
    };
    ESP_ERROR_CHECK(gpio_config(&io_conf));

    xSemaphoreTake(s_lock, portMAX_DELAY);
    render_locked(); /* draw the initial "Stop" screen */
    xSemaphoreGive(s_lock);

    int last_level = 1;
    while (1) {
        int level = gpio_get_level(BOOT_BUTTON_PIN);
        if (level == 0 && last_level == 1) {
            /* Falling edge -- debounce, then confirm it's still held. */
            vTaskDelay(pdMS_TO_TICKS(DEBOUNCE_MS));
            if (gpio_get_level(BOOT_BUTTON_PIN) == 0) {
                ESP_LOGI(TAG, "BOOT button pressed");
                status_display_toggle();

                /* Wait for release so holding the button doesn't retrigger. */
                while (gpio_get_level(BOOT_BUTTON_PIN) == 0) {
                    vTaskDelay(pdMS_TO_TICKS(POLL_MS));
                }
            }
        }
        last_level = level;
        vTaskDelay(pdMS_TO_TICKS(POLL_MS));
    }
}

void status_display_init(void)
{
    s_lock = xSemaphoreCreateMutex();
    xTaskCreate(button_task, "status_button", 4096, NULL, 5, NULL);
}
