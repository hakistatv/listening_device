#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <inttypes.h>
#include "sdkconfig.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "esp_chip_info.h"
#include "esp_flash.h"
#include "esp_system.h"
#include "esp_log.h"
#include "driver/gpio.h"
#include "driver/i2c_master.h"
#include "driver/i2s_std.h"
#include "esp_codec_dev.h"
#include "esp_codec_dev_defaults.h"
#include "es8311_codec.h"
#include "esp_event.h"
#include "esp_netif.h"
#include "esp_wifi.h"
#include "device_config.h"
#include "device_settings.h"
#include "epaper.h"
#include "mdns_service.h"
#include "recorder.h"
#include "sd_card.h"
#include "status_display.h"
#include "web_server.h"
#include "build_info.h"

/*
 * Pinout for the Waveshare ESP32-S3-ePaper-1.54 (V1 & V2 share the same
 * pin map). Sourced from Waveshare's own ESP-IDF demo
 * (github.com/waveshareteam/ESP32-S3-ePaper-1.54,
 * 02_Example/ESP-IDF/V1/08_Audio_Test/components/codec_board/board_cfg.txt
 * and main/user_config.h) -- the product docs page has no pinout table.
 */
#define AUDIO_I2C_SDA_PIN   47
#define AUDIO_I2C_SCL_PIN   48
#define AUDIO_I2S_MCLK_PIN  14
#define AUDIO_I2S_BCLK_PIN  15
#define AUDIO_I2S_WS_PIN    38
#define AUDIO_I2S_DIN_PIN   16 /* mic data into the MCU */
#define AUDIO_I2S_DOUT_PIN  45 /* speaker data out of the MCU (unused for now) */
#define AUDIO_PWR_PIN       42 /* ES8311 codec power rail enable */
#define AUDIO_PA_PIN        (-1) /* speaker PA left undriven -- this firmware only records */

#define MIC_SAMPLE_RATE     16000
#define MIC_BITS_PER_SAMPLE 16
#define MIC_CHANNELS        1

static const char *TAG = "listening_device";

static void audio_power_on(void)
{
    gpio_config_t io_conf = {
        .pin_bit_mask = 1ULL << AUDIO_PWR_PIN,
        .mode = GPIO_MODE_OUTPUT,
    };
    ESP_ERROR_CHECK(gpio_config(&io_conf));
    ESP_ERROR_CHECK(gpio_set_level(AUDIO_PWR_PIN, 1));
    vTaskDelay(pdMS_TO_TICKS(50)); /* let the codec power rail settle */
}

/* Bring up the I2C control bus, I2S RX channel, and ES8311 codec in
 * mic-capture-only mode. Returns NULL on failure. */
static esp_codec_dev_handle_t init_mic(void)
{
    i2c_master_bus_config_t i2c_bus_cfg = {
        .clk_source = I2C_CLK_SRC_DEFAULT,
        .i2c_port = I2C_NUM_0,
        .scl_io_num = AUDIO_I2C_SCL_PIN,
        .sda_io_num = AUDIO_I2C_SDA_PIN,
        .glitch_ignore_cnt = 7,
        .flags.enable_internal_pullup = true,
    };
    i2c_master_bus_handle_t i2c_bus_handle;
    esp_err_t err = i2c_new_master_bus(&i2c_bus_cfg, &i2c_bus_handle);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "i2c_new_master_bus failed: %s", esp_err_to_name(err));
        return NULL;
    }

    i2s_chan_config_t chan_cfg = I2S_CHANNEL_DEFAULT_CONFIG(I2S_NUM_0, I2S_ROLE_MASTER);
    i2s_chan_handle_t rx_handle;
    err = i2s_new_channel(&chan_cfg, NULL, &rx_handle); /* record-only: no tx handle */
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "i2s_new_channel failed: %s", esp_err_to_name(err));
        return NULL;
    }

    i2s_std_config_t std_cfg = {
        .clk_cfg = I2S_STD_CLK_DEFAULT_CONFIG(MIC_SAMPLE_RATE),
        .slot_cfg = I2S_STD_PHILIPS_SLOT_DEFAULT_CONFIG(MIC_BITS_PER_SAMPLE, I2S_SLOT_MODE_MONO),
        .gpio_cfg = {
            .mclk = AUDIO_I2S_MCLK_PIN,
            .bclk = AUDIO_I2S_BCLK_PIN,
            .ws = AUDIO_I2S_WS_PIN,
            .dout = I2S_GPIO_UNUSED,
            .din = AUDIO_I2S_DIN_PIN,
        },
    };
    err = i2s_channel_init_std_mode(rx_handle, &std_cfg);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "i2s_channel_init_std_mode failed: %s", esp_err_to_name(err));
        return NULL;
    }

    audio_codec_i2s_cfg_t i2s_cfg = {
        .rx_handle = rx_handle,
    };
    const audio_codec_data_if_t *data_if = audio_codec_new_i2s_data(&i2s_cfg);

    audio_codec_i2c_cfg_t i2c_cfg = {
        .addr = ES8311_CODEC_DEFAULT_ADDR,
        .bus_handle = i2c_bus_handle,
        .clock_speed_hz = 400000,
    };
    const audio_codec_ctrl_if_t *ctrl_if = audio_codec_new_i2c_ctrl(&i2c_cfg);

    const audio_codec_gpio_if_t *gpio_if = audio_codec_new_gpio();

    es8311_codec_cfg_t es8311_cfg = {
        .codec_mode = ESP_CODEC_DEV_WORK_MODE_ADC,
        .ctrl_if = ctrl_if,
        .gpio_if = gpio_if,
        .pa_pin = AUDIO_PA_PIN,
        .use_mclk = true,
    };
    const audio_codec_if_t *codec_if = es8311_codec_new(&es8311_cfg);
    if (!data_if || !ctrl_if || !gpio_if || !codec_if) {
        ESP_LOGE(TAG, "Failed to build one or more ES8311 interfaces");
        return NULL;
    }

    esp_codec_dev_cfg_t dev_cfg = {
        .codec_if = codec_if,
        .data_if = data_if,
        .dev_type = ESP_CODEC_DEV_TYPE_IN,
    };
    esp_codec_dev_handle_t mic_dev = esp_codec_dev_new(&dev_cfg);
    if (!mic_dev) {
        ESP_LOGE(TAG, "esp_codec_dev_new failed");
        return NULL;
    }

    esp_codec_dev_set_in_gain(mic_dev, 30.0f);

    esp_codec_dev_sample_info_t fs = {
        .sample_rate = MIC_SAMPLE_RATE,
        .channel = MIC_CHANNELS,
        .bits_per_sample = MIC_BITS_PER_SAMPLE,
    };
    int ret = esp_codec_dev_open(mic_dev, &fs);
    if (ret != ESP_CODEC_DEV_OK) {
        ESP_LOGE(TAG, "esp_codec_dev_open failed: %d", ret);
        return NULL;
    }

    ESP_LOGI(TAG, "Mic ready: ES8311 @ %dHz %d-bit %dch",
             MIC_SAMPLE_RATE, MIC_BITS_PER_SAMPLE, MIC_CHANNELS);
    return mic_dev;
}

/* Brings up Wi-Fi in access-point mode using current_ssid/current_pass
 * (see device_settings.h) so a phone/laptop can connect directly to the
 * board (no home router needed). Call init_nvs() + load_runtime_config()
 * before this. */
static void wifi_init_softap(void)
{
    ESP_ERROR_CHECK(esp_netif_init());
    ESP_ERROR_CHECK(esp_event_loop_create_default());
    esp_netif_create_default_wifi_ap();

    wifi_init_config_t cfg = WIFI_INIT_CONFIG_DEFAULT();
    ESP_ERROR_CHECK(esp_wifi_init(&cfg));

    wifi_config_t wifi_config = {
        .ap = {
            .channel = WIFI_AP_CHANNEL,
            .max_connection = WIFI_AP_MAX_CONN,
            .authmode = WIFI_AUTH_WPA2_PSK,
            .pmf_cfg = {
                .required = true,
            },
        },
    };
    strlcpy((char *)wifi_config.ap.ssid, current_ssid, sizeof(wifi_config.ap.ssid));
    wifi_config.ap.ssid_len = strlen(current_ssid);
    strlcpy((char *)wifi_config.ap.password, current_pass, sizeof(wifi_config.ap.password));

    ESP_ERROR_CHECK(esp_wifi_set_mode(WIFI_MODE_AP));
    ESP_ERROR_CHECK(esp_wifi_set_config(WIFI_IF_AP, &wifi_config));
    ESP_ERROR_CHECK(esp_wifi_start());

    ESP_LOGI(TAG, "Wi-Fi AP started: SSID=\"%s\" channel=%d", current_ssid, WIFI_AP_CHANNEL);
    ESP_LOGI(TAG, "Connect, then browse to http://192.168.4.1/ or http://%s.local/", current_hostname);
}

void app_main(void)
{
    ESP_LOGI(TAG, "listening_device starting up");
    /* Backup copy of the /origin route's provenance info (see
     * web_server.c, build_info.h) -- keeps FW_ORIGIN_MARK in the compiled
     * binary even if a fork strips the web server out entirely. */
    ESP_LOGI(TAG, "%s / %s (%s)", FW_ORIGIN_AUTHOR, FW_ORIGIN_REPO, FW_ORIGIN_MARK);

    esp_chip_info_t chip_info;
    uint32_t flash_size;
    esp_chip_info(&chip_info);
    ESP_LOGI(TAG, "This is %s chip with %d CPU core(s), %s%s%s%s",
             CONFIG_IDF_TARGET,
             chip_info.cores,
             (chip_info.features & CHIP_FEATURE_WIFI_BGN) ? "WiFi/" : "",
             (chip_info.features & CHIP_FEATURE_BT) ? "BT" : "",
             (chip_info.features & CHIP_FEATURE_BLE) ? "BLE" : "",
             (chip_info.features & CHIP_FEATURE_IEEE802154) ? ", 802.15.4 (Zigbee/Thread)" : "");

    unsigned major_rev = chip_info.revision / 100;
    unsigned minor_rev = chip_info.revision % 100;
    ESP_LOGI(TAG, "Silicon revision v%d.%d", major_rev, minor_rev);

    if (esp_flash_get_size(NULL, &flash_size) == ESP_OK) {
        ESP_LOGI(TAG, "%" PRIu32 "MB %s flash", flash_size / (uint32_t)(1024 * 1024),
                 (chip_info.features & CHIP_FEATURE_EMB_FLASH) ? "embedded" : "external");
    }
    ESP_LOGI(TAG, "Minimum free heap size: %" PRIu32 " bytes", esp_get_minimum_free_heap_size());

    init_nvs();
    load_runtime_config(); /* must run before status_display_init() -- it draws the initial screen right away and needs current_stealth_mode already loaded */

    epd_init();
    status_display_init(); /* draws "Stop" (or blank, if stealth mode is on) and starts watching the BOOT button */

    wifi_init_softap();
    start_mdns();
    sd_card_init();
    start_webserver();

    audio_power_on();
    esp_codec_dev_handle_t mic_dev = init_mic();
    if (mic_dev) {
        recorder_init(mic_dev);
        /* If a recording hits the configured max duration, make the BOOT
         * button/web "Listening" state and e-paper screen reflect that it
         * actually stopped -- status_display_toggle() is the single
         * source of truth for that (see status_display.h). */
        recorder_set_auto_stop_callback(status_display_toggle);
    } else {
        ESP_LOGE(TAG, "Mic init failed -- check wiring/pin map before retrying");
    }

    while (1) {
        vTaskDelay(pdMS_TO_TICKS(5000));
    }
}
