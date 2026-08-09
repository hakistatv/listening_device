#include <stdio.h>
#include <stdlib.h>
#include <stdbool.h>
#include <string.h>
#include <strings.h>
#include <ctype.h>
#include <inttypes.h>
#include <dirent.h>
#include <sys/stat.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "esp_log.h"
#include "esp_system.h"
#include "device_settings.h"
#include "recorder.h"
#include "sd_card.h"
#include "status_display.h"
#include "web_server.h"

static const char *TAG = "web_server";

/* HTML pages (and the shared style.css) live under main/pages/ and are
 * embedded into the binary at build time (EMBED_TXTFILES in
 * main/CMakeLists.txt), null-terminated, so they can be used directly as C
 * strings. settings.html and recordings_item.html are printf-style
 * templates -- see their use below for the placeholder meanings. The
 * Settings page intentionally leaves the password blank rather than echoing
 * the current value back into the page source. The Recordings page is
 * assembled from several small pieces (header/item/empty/footer) since its
 * item list has a variable length. Every page's <head> links to /style.css
 * for consistent, mobile-sized text and buttons -- see style_get_handler. */
extern const char home_html_start[] asm("_binary_home_html_start");
extern const char listening_html_start[] asm("_binary_listening_html_start");
extern const char settings_html_start[] asm("_binary_settings_html_start");
extern const char restart_html_start[] asm("_binary_restart_html_start");
extern const char recordings_no_card_html_start[] asm("_binary_recordings_no_card_html_start");
extern const char recordings_header_html_start[] asm("_binary_recordings_header_html_start");
extern const char recordings_item_html_start[] asm("_binary_recordings_item_html_start");
extern const char recordings_empty_html_start[] asm("_binary_recordings_empty_html_start");
extern const char recordings_footer_html_start[] asm("_binary_recordings_footer_html_start");
extern const char style_css_start[] asm("_binary_style_css_start");

/* Decodes an application/x-www-form-urlencoded value in place: '+' -> space,
 * '%XX' -> byte XX. esp_http_server's httpd_query_key_value() only splits
 * out the raw substring; it doesn't do this decoding for you. */
static void url_decode(char *str)
{
    char *src = str;
    char *dst = str;
    while (*src) {
        if (src[0] == '%' && isxdigit((unsigned char)src[1]) && isxdigit((unsigned char)src[2])) {
            char hex[3] = { src[1], src[2], 0 };
            *dst++ = (char)strtol(hex, NULL, 16);
            src += 3;
        } else if (*src == '+') {
            *dst++ = ' ';
            src++;
        } else {
            *dst++ = *src++;
        }
    }
    *dst = '\0';
}

/* Escapes a string for safe use inside an HTML attribute value. Truncates
 * (rather than overflows) if dst_size is too small for the escaped result. */
static void html_escape(const char *src, char *dst, size_t dst_size)
{
    size_t di = 0;
    for (size_t si = 0; src[si] != '\0'; si++) {
        const char *rep = NULL;
        switch (src[si]) {
            case '&':  rep = "&amp;";  break;
            case '<':  rep = "&lt;";   break;
            case '>':  rep = "&gt;";   break;
            case '"':  rep = "&quot;"; break;
            default: break;
        }
        size_t add_len = rep ? strlen(rep) : 1;
        if (di + add_len >= dst_size) {
            break;
        }
        if (rep) {
            memcpy(dst + di, rep, add_len);
        } else {
            dst[di] = src[si];
        }
        di += add_len;
    }
    dst[di] = '\0';
}

/* Percent-encodes a string for safe use as a URL query value. Leaves '/'
 * unescaped for readability (fine here since we only ever split query
 * strings on '&', never '/'). Truncates safely if dst_size is too small. */
static void url_encode(const char *src, char *dst, size_t dst_size)
{
    static const char hex[] = "0123456789ABCDEF";
    size_t di = 0;
    for (size_t si = 0; src[si] != '\0'; si++) {
        unsigned char c = (unsigned char)src[si];
        bool safe = isalnum(c) || c == '-' || c == '_' || c == '.' || c == '~' || c == '/';
        size_t need = safe ? 1 : 3;
        if (di + need >= dst_size) {
            break;
        }
        if (safe) {
            dst[di++] = (char)c;
        } else {
            dst[di++] = '%';
            dst[di++] = hex[c >> 4];
            dst[di++] = hex[c & 0xF];
        }
    }
    dst[di] = '\0';
}

/* FATFS/SD entries can have names up to 255 chars (struct dirent.d_name is
 * char[256]); names longer than this are truncated for display/download
 * links only -- fine for the recordings this device creates itself, and
 * stat()/fopen() below always use the untruncated real name. */
#define RECORDING_NAME_MAX 128

static esp_err_t root_get_handler(httpd_req_t *req)
{
    httpd_resp_set_type(req, "text/html");
    return httpd_resp_send(req, home_html_start, HTTPD_RESP_USE_STRLEN);
}

/* Shared stylesheet every page's <head> links to -- one static file, so the
 * browser caches it after the first page load instead of it being resent
 * (or duplicated) per page. */
static esp_err_t style_get_handler(httpd_req_t *req)
{
    httpd_resp_set_type(req, "text/css");
    return httpd_resp_send(req, style_css_start, HTTPD_RESP_USE_STRLEN);
}

/* Builds "<p>Last recording: <a href="/download?file=...">name</a></p>", or
 * an empty string if there's no completed recording yet this boot. */
static void build_last_recording_link(char *out, size_t out_size)
{
    char filename[RECORDING_NAME_MAX + 1] = {0};
    recorder_get_last_filename(filename, sizeof(filename));
    if (filename[0] == '\0') {
        out[0] = '\0';
        return;
    }

    char name_esc[RECORDING_NAME_MAX * 3];
    html_escape(filename, name_esc, sizeof(name_esc));
    char name_enc[RECORDING_NAME_MAX * 3];
    url_encode(filename, name_enc, sizeof(name_enc));

    snprintf(out, out_size, "<p>Last recording: <a href=\"/download?file=%s\">%s</a></p>", name_enc, name_esc);
}

static esp_err_t listening_get_handler(httpd_req_t *req)
{
    const char *button_label = status_display_is_listening() ? "Stop" : "Listen";

    char link_html[RECORDING_NAME_MAX * 6 + 64];
    build_last_recording_link(link_html, sizeof(link_html));

    /* Worst case: template (~380 bytes incl. the <head> meta/stylesheet
     * tags) + button_label (<=6) + link_html (up to RECORDING_NAME_MAX * 6 +
     * 64 = 832, if the last recording's filename is all HTML-escaped/
     * percent-encoded characters) -- comfortably under 2048. */
    char page[2048];
    int len = snprintf(page, sizeof(page), listening_html_start, button_label, link_html);
    if (len < 0 || (size_t)len >= sizeof(page)) {
        httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "Page too large");
        return ESP_FAIL;
    }

    httpd_resp_set_type(req, "text/html");
    return httpd_resp_send(req, page, len);
}

/* The button posts with no fields, but drain any body defensively before
 * responding, same as every other POST handler here. */
static esp_err_t listening_post_handler(httpd_req_t *req)
{
    char discard[64];
    int remaining = req->content_len;
    while (remaining > 0) {
        int to_read = remaining < (int)sizeof(discard) ? remaining : (int)sizeof(discard);
        int ret = httpd_req_recv(req, discard, to_read);
        if (ret <= 0) {
            break;
        }
        remaining -= ret;
    }

    status_display_toggle();

    /* Redirect back to GET /listening (POST/redirect/GET) so a page reload
     * doesn't resubmit the form and toggle again. */
    httpd_resp_set_status(req, "303 See Other");
    httpd_resp_set_hdr(req, "Location", "/listening");
    return httpd_resp_send(req, NULL, 0);
}

static esp_err_t settings_get_handler(httpd_req_t *req)
{
    char ssid_esc[SSID_MAX_LEN * 6 + 1];
    char host_esc[HOST_MAX_LEN * 6 + 1];
    html_escape(current_ssid, ssid_esc, sizeof(ssid_esc));
    html_escape(current_hostname, host_esc, sizeof(host_esc));

    char duration_str[11]; /* uint32_t max is 10 digits */
    snprintf(duration_str, sizeof(duration_str), "%" PRIu32, current_recording_max_duration_sec);

    /* Worst case: template (~1100 bytes incl. the <head> meta/stylesheet
     * tags) + ssid_esc/host_esc (up to SSID_MAX_LEN/HOST_MAX_LEN * 6 + 1 =
     * 193 each, if every character needs HTML-escaping) + duration_str
     * (<=10) + "checked" (<=8) -- comfortably under 2048. */
    char page[2048];
    int len = snprintf(page, sizeof(page), settings_html_start, ssid_esc, host_esc, duration_str,
                        current_stealth_mode ? "checked" : "");
    if (len < 0 || (size_t)len >= sizeof(page)) {
        httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "Page too large");
        return ESP_FAIL;
    }

    httpd_resp_set_type(req, "text/html");
    return httpd_resp_send(req, page, len);
}

/* Restarting from inside the HTTP handler that sent the response would race
 * the socket close, so hand it to a task that waits for the response to
 * flush first. */
static void restart_task(void *arg)
{
    (void)arg;
    vTaskDelay(pdMS_TO_TICKS(1500));
    esp_restart();
}

static esp_err_t settings_post_handler(httpd_req_t *req)
{
    if (req->content_len <= 0 || req->content_len >= 256) {
        httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "Form data missing or too large");
        return ESP_FAIL;
    }

    char body[256];
    int received = 0;
    while (received < req->content_len) {
        int ret = httpd_req_recv(req, body + received, req->content_len - received);
        if (ret <= 0) {
            if (ret == HTTPD_SOCK_ERR_TIMEOUT) {
                continue;
            }
            return ESP_FAIL;
        }
        received += ret;
    }
    body[received] = '\0';

    char ssid_val[SSID_MAX_LEN + 1] = {0};
    char pass_val[PASS_MAX_LEN + 1] = {0};
    char host_val[HOST_MAX_LEN + 1] = {0};
    char duration_val[16] = {0};
    char stealth_val[8] = {0};

    bool has_ssid = httpd_query_key_value(body, "ssid", ssid_val, sizeof(ssid_val)) == ESP_OK;
    bool has_pass = httpd_query_key_value(body, "password", pass_val, sizeof(pass_val)) == ESP_OK;
    bool has_host = httpd_query_key_value(body, "hostname", host_val, sizeof(host_val)) == ESP_OK;
    bool has_duration = httpd_query_key_value(body, "max_duration", duration_val, sizeof(duration_val)) == ESP_OK;
    /* Checkboxes are simply absent from the submitted form when unchecked
     * -- presence (regardless of value) means checked, no "required" check
     * needed like the other fields. */
    bool stealth_mode = httpd_query_key_value(body, "stealth_mode", stealth_val, sizeof(stealth_val)) == ESP_OK;
    url_decode(ssid_val);
    url_decode(pass_val);
    url_decode(host_val);
    url_decode(duration_val);

    if (!has_ssid || strlen(ssid_val) == 0) {
        httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "SSID is required");
        return ESP_FAIL;
    }
    if (!has_host || strlen(host_val) == 0) {
        httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "Device name is required");
        return ESP_FAIL;
    }
    if (has_pass && strlen(pass_val) > 0 && strlen(pass_val) < 8) {
        httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST,
                             "Password must be at least 8 characters, or left blank to keep the current one");
        return ESP_FAIL;
    }

    char *duration_endptr = NULL;
    unsigned long duration_parsed = has_duration ? strtoul(duration_val, &duration_endptr, 10) : 0;
    if (!has_duration || duration_val[0] == '\0' || duration_endptr == duration_val || *duration_endptr != '\0'
        || duration_parsed > 86400) {
        httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST,
                             "Max recording duration must be a whole number of seconds, 0-86400 (0 = no limit)");
        return ESP_FAIL;
    }

    const char *new_pass = (has_pass && strlen(pass_val) > 0) ? pass_val : current_pass;
    esp_err_t err = save_runtime_config(ssid_val, new_pass, host_val, (uint32_t)duration_parsed, stealth_mode);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "Failed to save settings: %s", esp_err_to_name(err));
        httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "Failed to save settings");
        return ESP_FAIL;
    }

    httpd_resp_set_type(req, "text/html");
    httpd_resp_send(req, restart_html_start, HTTPD_RESP_USE_STRLEN);

    ESP_LOGI(TAG, "Settings saved (SSID=\"%s\" hostname=\"%s\" max_duration=%lus stealth_mode=%s) -- restarting",
             ssid_val, host_val, duration_parsed, stealth_mode ? "on" : "off");
    xTaskCreate(restart_task, "restart", 2048, NULL, 5, NULL);
    return ESP_OK;
}

/* Lists SD_RECORDINGS_DIR (flat -- no subfolder navigation) with the
 * filename itself as the download link. */
static esp_err_t recordings_get_handler(httpd_req_t *req)
{
    httpd_resp_set_type(req, "text/html");

    if (!sd_card_is_mounted()) {
        return httpd_resp_send(req, recordings_no_card_html_start, HTTPD_RESP_USE_STRLEN);
    }

    DIR *dir = opendir(SD_RECORDINGS_DIR);
    if (!dir) {
        httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "Could not open recordings folder");
        return ESP_FAIL;
    }

    httpd_resp_sendstr_chunk(req, recordings_header_html_start);

    char chunk[1024];
    bool any = false;
    struct dirent *ent;
    while ((ent = readdir(dir)) != NULL) {
        if (strcmp(ent->d_name, ".") == 0 || strcmp(ent->d_name, "..") == 0) {
            continue;
        }

        char entry_full[sizeof(SD_RECORDINGS_DIR) + 1 + sizeof(ent->d_name)];
        snprintf(entry_full, sizeof(entry_full), "%s/%s", SD_RECORDINGS_DIR, ent->d_name);
        struct stat st;
        if (stat(entry_full, &st) != 0 || S_ISDIR(st.st_mode)) {
            continue; /* recordings is meant to stay flat; ignore subfolders */
        }
        any = true;

        char name_buf[RECORDING_NAME_MAX + 1];
        strlcpy(name_buf, ent->d_name, sizeof(name_buf));

        char name_esc[RECORDING_NAME_MAX * 3];
        html_escape(name_buf, name_esc, sizeof(name_esc));
        char name_enc[RECORDING_NAME_MAX * 3];
        url_encode(name_buf, name_enc, sizeof(name_enc));

        snprintf(chunk, sizeof(chunk), recordings_item_html_start, name_enc, name_esc);
        httpd_resp_sendstr_chunk(req, chunk);
    }
    closedir(dir);

    if (!any) {
        httpd_resp_sendstr_chunk(req, recordings_empty_html_start);
    }

    httpd_resp_sendstr_chunk(req, recordings_footer_html_start);
    httpd_resp_sendstr_chunk(req, NULL); /* end the chunked response */
    return ESP_OK;
}

static const char *content_type_for_filename(const char *filename)
{
    const char *dot = strrchr(filename, '.');
    if (dot != NULL) {
        if (strcasecmp(dot, ".wav") == 0) {
            return "audio/wav";
        }
        if (strcasecmp(dot, ".mp3") == 0) {
            return "audio/mpeg";
        }
    }
    return "application/octet-stream";
}

/* Streams a single file out of SD_RECORDINGS_DIR as a download.
 * GET /download?file=<name> -- name must be a plain filename (no '/' or
 * ".."), since recordings is a flat directory. */
static esp_err_t download_get_handler(httpd_req_t *req)
{
    char name[RECORDING_NAME_MAX + 1] = {0};
    size_t qs_len = httpd_req_get_url_query_len(req);
    if (qs_len > 0 && qs_len < 512) {
        char *qs = malloc(qs_len + 1);
        if (qs != NULL) {
            if (httpd_req_get_url_query_str(req, qs, qs_len + 1) == ESP_OK) {
                httpd_query_key_value(qs, "file", name, sizeof(name));
                url_decode(name);
            }
            free(qs);
        }
    }

    if (name[0] == '\0' || strchr(name, '/') != NULL || strstr(name, "..") != NULL) {
        httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "Invalid filename");
        return ESP_FAIL;
    }

    char full_path[sizeof(SD_RECORDINGS_DIR) + 1 + sizeof(name)];
    snprintf(full_path, sizeof(full_path), "%s/%s", SD_RECORDINGS_DIR, name);

    FILE *f = fopen(full_path, "rb");
    if (!f) {
        httpd_resp_send_err(req, HTTPD_404_NOT_FOUND, "File not found");
        return ESP_FAIL;
    }

    httpd_resp_set_type(req, content_type_for_filename(name));
    char disposition[sizeof(name) + 32];
    snprintf(disposition, sizeof(disposition), "attachment; filename=\"%s\"", name);
    httpd_resp_set_hdr(req, "Content-Disposition", disposition);

    char buf[1024];
    size_t n;
    esp_err_t ret = ESP_OK;
    while ((n = fread(buf, 1, sizeof(buf), f)) > 0) {
        if (httpd_resp_send_chunk(req, buf, n) != ESP_OK) {
            ret = ESP_FAIL;
            break;
        }
    }
    fclose(f);
    httpd_resp_send_chunk(req, NULL, 0); /* end the chunked response */
    return ret;
}

static const httpd_uri_t root_uri = {
    .uri = "/",
    .method = HTTP_GET,
    .handler = root_get_handler,
};

static const httpd_uri_t style_uri = {
    .uri = "/style.css",
    .method = HTTP_GET,
    .handler = style_get_handler,
};

static const httpd_uri_t listening_get_uri = {
    .uri = "/listening",
    .method = HTTP_GET,
    .handler = listening_get_handler,
};

static const httpd_uri_t listening_post_uri = {
    .uri = "/listening",
    .method = HTTP_POST,
    .handler = listening_post_handler,
};

static const httpd_uri_t settings_get_uri = {
    .uri = "/settings",
    .method = HTTP_GET,
    .handler = settings_get_handler,
};

static const httpd_uri_t settings_post_uri = {
    .uri = "/settings",
    .method = HTTP_POST,
    .handler = settings_post_handler,
};

static const httpd_uri_t recordings_get_uri = {
    .uri = "/recordings",
    .method = HTTP_GET,
    .handler = recordings_get_handler,
};

static const httpd_uri_t download_get_uri = {
    .uri = "/download",
    .method = HTTP_GET,
    .handler = download_get_handler,
};

httpd_handle_t start_webserver(void)
{
    httpd_handle_t server = NULL;
    httpd_config_t config = HTTPD_DEFAULT_CONFIG();
    config.stack_size = 8192; /* recordings/download handlers' path/HTML buffers need more than the 4KB default */

    esp_err_t ret = httpd_start(&server, &config);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "httpd_start failed: %s", esp_err_to_name(ret));
        return NULL;
    }

    httpd_register_uri_handler(server, &root_uri);
    httpd_register_uri_handler(server, &style_uri);
    httpd_register_uri_handler(server, &listening_get_uri);
    httpd_register_uri_handler(server, &listening_post_uri);
    httpd_register_uri_handler(server, &settings_get_uri);
    httpd_register_uri_handler(server, &settings_post_uri);
    httpd_register_uri_handler(server, &recordings_get_uri);
    httpd_register_uri_handler(server, &download_get_uri);
    ESP_LOGI(TAG, "Web server started");
    return server;
}
