#include <string.h>
#include <inttypes.h>
#include "esp_log.h"
#include "nvs.h"
#include "nvs_flash.h"
#include "device_config.h"
#include "device_settings.h"

static const char *TAG = "device_settings";

#define CFG_NVS_NAMESPACE "dev_cfg"
#define CFG_KEY_SSID      "wifi_ssid"
#define CFG_KEY_PASS      "wifi_pass"
#define CFG_KEY_HOST      "mdns_host"
#define CFG_KEY_REC_DUR   "rec_max_dur"
#define CFG_KEY_STEALTH   "stealth_mode"

char current_ssid[SSID_MAX_LEN + 1];
char current_pass[PASS_MAX_LEN + 1];
char current_hostname[HOST_MAX_LEN + 1];
uint32_t current_recording_max_duration_sec;
bool current_stealth_mode;

void init_nvs(void)
{
    esp_err_t ret = nvs_flash_init();
    if (ret == ESP_ERR_NVS_NO_FREE_PAGES || ret == ESP_ERR_NVS_NEW_VERSION_FOUND) {
        ESP_ERROR_CHECK(nvs_flash_erase());
        ret = nvs_flash_init();
    }
    ESP_ERROR_CHECK(ret);
}

void load_runtime_config(void)
{
    strncpy(current_ssid, WIFI_AP_SSID, sizeof(current_ssid) - 1);
    strncpy(current_pass, WIFI_AP_PASS, sizeof(current_pass) - 1);
    strncpy(current_hostname, MDNS_HOSTNAME, sizeof(current_hostname) - 1);
    current_recording_max_duration_sec = RECORDING_MAX_DURATION_SEC;
    current_stealth_mode = STEALTH_MODE_DEFAULT;

    nvs_handle_t nvs;
    if (nvs_open(CFG_NVS_NAMESPACE, NVS_READONLY, &nvs) != ESP_OK) {
        ESP_LOGI(TAG, "No saved settings in NVS yet -- using device_config.h defaults");
        return;
    }

    size_t len = sizeof(current_ssid);
    nvs_get_str(nvs, CFG_KEY_SSID, current_ssid, &len);
    len = sizeof(current_pass);
    nvs_get_str(nvs, CFG_KEY_PASS, current_pass, &len);
    len = sizeof(current_hostname);
    nvs_get_str(nvs, CFG_KEY_HOST, current_hostname, &len);
    nvs_get_u32(nvs, CFG_KEY_REC_DUR, &current_recording_max_duration_sec);

    uint8_t stealth_u8 = current_stealth_mode ? 1 : 0;
    if (nvs_get_u8(nvs, CFG_KEY_STEALTH, &stealth_u8) == ESP_OK) {
        current_stealth_mode = (stealth_u8 != 0);
    }

    nvs_close(nvs);
    ESP_LOGI(TAG, "Loaded settings from NVS: SSID=\"%s\" hostname=\"%s\" recording_max_duration_sec=%" PRIu32
             " stealth_mode=%s",
             current_ssid, current_hostname, current_recording_max_duration_sec,
             current_stealth_mode ? "on" : "off");
}

esp_err_t save_runtime_config(const char *ssid, const char *pass, const char *hostname,
                               uint32_t recording_max_duration_sec, bool stealth_mode)
{
    nvs_handle_t nvs;
    esp_err_t err = nvs_open(CFG_NVS_NAMESPACE, NVS_READWRITE, &nvs);
    if (err != ESP_OK) {
        return err;
    }

    err = nvs_set_str(nvs, CFG_KEY_SSID, ssid);
    if (err == ESP_OK) {
        err = nvs_set_str(nvs, CFG_KEY_PASS, pass);
    }
    if (err == ESP_OK) {
        err = nvs_set_str(nvs, CFG_KEY_HOST, hostname);
    }
    if (err == ESP_OK) {
        err = nvs_set_u32(nvs, CFG_KEY_REC_DUR, recording_max_duration_sec);
    }
    if (err == ESP_OK) {
        err = nvs_set_u8(nvs, CFG_KEY_STEALTH, stealth_mode ? 1 : 0);
    }
    if (err == ESP_OK) {
        err = nvs_commit(nvs);
    }

    nvs_close(nvs);
    return err;
}
