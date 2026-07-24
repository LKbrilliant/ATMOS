/*
 * SPDX-FileCopyrightText: 2026 Michael Teeuw
 * SPDX-License-Identifier: GPL-3.0-or-later
 *
 * Captive portal HTTP server: serves the config page and handles form submissions.
 */

#include "wifi_prov_internal.h"
#include "esp_wifi.h"
#include "esp_http_server.h"
#include "esp_event.h"
#include "wifi_provisioner.h"

#include <stdlib.h>
#include <string.h>

static const char *TAG = "wifi_prov_http";

static httpd_handle_t s_server = NULL;
static const wifi_prov_config_t *s_page_config = NULL;

/* ── Event posted when the user submits credentials ─────────────────── */

ESP_EVENT_DECLARE_BASE(WIFI_PROV_EVENT);
ESP_EVENT_DEFINE_BASE(WIFI_PROV_EVENT);

enum {
    WIFI_PROV_EVENT_CREDENTIALS_SET,
};

typedef struct {
    char ssid[33];
    char password[65];
} wifi_prov_creds_t;

/* ── Embedded HTML (see src/portal.html) ─────────────────────────────── */

extern const uint8_t portal_html_start[]    asm("_binary_portal_html_start");
extern const uint8_t portal_html_end[]      asm("_binary_portal_html_end");

/* ── URL decoding & Parameter Extraction ────────────────────────────── */

static int hex_val(char c)
{
    if (c >= '0' && c <= '9') return c - '0';
    if (c >= 'a' && c <= 'f') return c - 'a' + 10;
    if (c >= 'A' && c <= 'F') return c - 'A' + 10;
    return -1;
}

static void url_decode(char *dst, size_t dst_len, const char *src, size_t src_len)
{
    size_t di = 0;
    for (size_t si = 0; si < src_len && di < dst_len - 1; si++) {
        if (src[si] == '+') {
            dst[di++] = ' ';
        } else if (src[si] == '%' && si + 2 < src_len) {
            int hi = hex_val(src[si + 1]);
            int lo = hex_val(src[si + 2]);
            if (hi >= 0 && lo >= 0) {
                dst[di++] = (char)((hi << 4) | lo);
                si += 2;
            } else {
                dst[di++] = src[si];
            }
        } else {
            dst[di++] = src[si];
        }
    }
    dst[di] = '\0';
}

static void get_param_val(const char *buf, const char *param, char *out, size_t max_len) {
    if (!buf || !param || !out || max_len == 0) return;
    out[0] = '\0';

    size_t param_len = strlen(param);
    const char *p = buf;

    while ((p = strstr(p, param)) != NULL) {
        // Check if key is preceded by start-of-string, '&', or '?'
        if (p == buf || *(p - 1) == '&' || *(p - 1) == '?') {
            if (p[param_len] == '=') {
                p += param_len + 1;
                size_t i = 0;
                while (*p && *p != '&' && i < max_len - 1) {
                    out[i++] = *p++;
                }
                out[i] = '\0';
                return;
            }
        }
        p += param_len;
    }
}

/* ── Handlers ───────────────────────────────────────────────────────── */

static esp_err_t config_handler(httpd_req_t *req)
{
    char json[512];
    snprintf(json, sizeof(json),
             "{\"title\":\"%s\","
             "\"portal_header\":\"%s\","
             "\"portal_subheader\":\"%s\","
             "\"connected_header\":\"%s\","
             "\"connected_subheader\":\"%s\","
             "\"footer\":\"%s\"}",
             s_page_config->page_title,
             s_page_config->portal_header,
             s_page_config->portal_subheader,
             s_page_config->connected_header,
             s_page_config->connected_subheader,
             s_page_config->page_footer);

    httpd_resp_set_type(req, "application/json");
    return httpd_resp_send(req, json, HTTPD_RESP_USE_STRLEN);
}

static esp_err_t root_handler(httpd_req_t *req)
{
    httpd_resp_set_type(req, "text/html");
    const size_t len = portal_html_end - portal_html_start;
    return httpd_resp_send(req, (const char *)portal_html_start, len);
}

static esp_err_t scan_handler(httpd_req_t *req)
{
    uint16_t ap_count = 0;
    wifi_ap_record_t *ap_records = NULL;

    wifi_scan_config_t scan_cfg = {
        .show_hidden = false,
    };
    esp_err_t err = esp_wifi_scan_start(&scan_cfg, true);
    if (err != ESP_OK) {
        httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "Scan failed");
        return ESP_FAIL;
    }

    esp_wifi_scan_get_ap_num(&ap_count);
    if (ap_count == 0) {
        httpd_resp_set_type(req, "application/json");
        return httpd_resp_send(req, "[]", 2);
    }

    ap_records = malloc(sizeof(wifi_ap_record_t) * ap_count);
    if (!ap_records) {
        httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "Out of memory");
        return ESP_ERR_NO_MEM;
    }
    esp_wifi_scan_get_ap_records(&ap_count, ap_records);

    /* Deduplicate by SSID, keeping the strongest signal */
    uint16_t unique_count = 0;
    for (int i = 0; i < ap_count; i++) {
        if (ap_records[i].ssid[0] == '\0') continue; /* skip hidden */
        bool dup = false;
        for (int j = 0; j < i; j++) {
            if (strcmp((char *)ap_records[i].ssid, (char *)ap_records[j].ssid) == 0) {
                dup = true;
                if (ap_records[i].rssi > ap_records[j].rssi) {
                    ap_records[j].rssi = ap_records[i].rssi;
                }
                break;
            }
        }
        if (!dup) {
            if (unique_count != i) {
                ap_records[unique_count] = ap_records[i];
            }
            unique_count++;
        }
    }

    /* Build JSON array */
    char *json = malloc(unique_count * 80 + 4);
    if (!json) {
        free(ap_records);
        httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "Out of memory");
        return ESP_ERR_NO_MEM;
    }

    char *p = json;
    *p++ = '[';
    for (int i = 0; i < unique_count; i++) {
        if (i > 0) *p++ = ',';
        p += sprintf(p, "{\"ssid\":\"%s\",\"rssi\":%d,\"auth\":%d}",
                     (char *)ap_records[i].ssid, ap_records[i].rssi,
                     ap_records[i].authmode);
    }
    *p++ = ']';
    *p   = '\0';

    free(ap_records);

    httpd_resp_set_type(req, "application/json");
    esp_err_t ret = httpd_resp_send(req, json, HTTPD_RESP_USE_STRLEN);
    free(json);
    return ret;
}

static esp_err_t save_handler(httpd_req_t *req)
{
    char buf[512]; 
    int received = httpd_req_recv(req, buf, sizeof(buf) - 1);
    if (received <= 0) {
        httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "No data");
        return ESP_FAIL;
    }
    buf[received] = '\0';

    ESP_LOGI(TAG, "RAW Received Form Body: %s", buf);

    wifi_prov_creds_t creds = {0};
    char raw_ssid[64] = {0}, raw_pass[64] = {0};

    get_param_val(buf, "ssid", raw_ssid, sizeof(raw_ssid));
    get_param_val(buf, "password", raw_pass, sizeof(raw_pass));

    if (strlen(raw_ssid) == 0) {
        httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "Missing SSID");
        return ESP_FAIL;
    }

    url_decode(creds.ssid, sizeof(creds.ssid), raw_ssid, strlen(raw_ssid));
    url_decode(creds.password, sizeof(creds.password), raw_pass, strlen(raw_pass));

    /* --- Parse Application Parameters --- */
    app_config_t app_cfg = {
        .latitude = "49.8844",
        .longitude = "-97.147",
        .temp_min = -40.0f,
        .temp_max = 40.0f
    };

    char raw_lat[32] = {0}, raw_lon[32] = {0};
    char raw_tmin[32] = {0}, raw_tmax[32] = {0};

    get_param_val(buf, "latitude", raw_lat, sizeof(raw_lat));
    get_param_val(buf, "longitude", raw_lon, sizeof(raw_lon));
    get_param_val(buf, "temp_min", raw_tmin, sizeof(raw_tmin));
    get_param_val(buf, "temp_max", raw_tmax, sizeof(raw_tmax));

    if (strlen(raw_lat) > 0) {
        url_decode(app_cfg.latitude, sizeof(app_cfg.latitude), raw_lat, strlen(raw_lat));
    }
    if (strlen(raw_lon) > 0) {
        url_decode(app_cfg.longitude, sizeof(app_cfg.longitude), raw_lon, strlen(raw_lon));
    }
    if (strlen(raw_tmin) > 0) {
        char decoded[32] = {0};
        url_decode(decoded, sizeof(decoded), raw_tmin, strlen(raw_tmin));
        app_cfg.temp_min = (float)atof(decoded);
    }
    if (strlen(raw_tmax) > 0) {
        char decoded[32] = {0};
        url_decode(decoded, sizeof(decoded), raw_tmax, strlen(raw_tmax));
        app_cfg.temp_max = (float)atof(decoded);
    }

    ESP_LOGI(TAG, "Parsed SSID: \"%s\"", creds.ssid);
    ESP_LOGI(TAG, "Parsed App Config -> Lat: %s | Lon: %s | Min: %.1f | Max: %.1f", 
             app_cfg.latitude, app_cfg.longitude, app_cfg.temp_min, app_cfg.temp_max);

    /* SAVE CONFIG TO NVS IMMEDIATELY BEFORE TRYING TO CONNECT */
    esp_err_t nvs_err = wifi_prov_save_app_config(&app_cfg);
    if (nvs_err != ESP_OK) {
        ESP_LOGE(TAG, "Failed to persist app_config to NVS: %s", esp_err_to_name(nvs_err));
    } else {
        ESP_LOGI(TAG, "App config successfully persisted to NVS.");
    }

    /* Try Wi-Fi Connection */
    esp_err_t err = wifi_sta_try_connect(creds.ssid, creds.password);

    httpd_resp_set_type(req, "application/json");

    if (err == ESP_OK) {
        nvs_store_save(creds.ssid, creds.password);
        httpd_resp_send(req, "{\"success\":true}", HTTPD_RESP_USE_STRLEN);

        esp_event_post(WIFI_PROV_EVENT, WIFI_PROV_EVENT_CREDENTIALS_SET,
                       &creds, sizeof(creds), pdMS_TO_TICKS(100));
    } else {
        httpd_resp_send(req, "{\"success\":false}", HTTPD_RESP_USE_STRLEN);
    }

    return ESP_OK;
}

/* Redirect any unknown path to "/" for captive portal detection */
static esp_err_t redirect_handler(httpd_req_t *req)
{
    httpd_resp_set_status(req, "302 Found");
    httpd_resp_set_hdr(req, "Location", "http://192.168.4.1/");
    return httpd_resp_send(req, NULL, 0);
}

/* ── Start / Stop ───────────────────────────────────────────────────── */

esp_err_t http_server_start(uint16_t port, const wifi_prov_config_t *page_config)
{
    if (s_server) {
        return ESP_ERR_INVALID_STATE;
    }

    s_page_config = page_config;

    httpd_config_t config = HTTPD_DEFAULT_CONFIG();
    config.server_port      = port;
    config.uri_match_fn     = httpd_uri_match_wildcard;
    config.lru_purge_enable = true;

    esp_err_t err = httpd_start(&s_server, &config);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "Failed to start HTTP server (%s)", esp_err_to_name(err));
        return err;
    }

    const httpd_uri_t uri_root = {
        .uri     = "/",
        .method  = HTTP_GET,
        .handler = root_handler,
    };
    const httpd_uri_t uri_scan = {
        .uri     = "/scan",
        .method  = HTTP_GET,
        .handler = scan_handler,
    };
    const httpd_uri_t uri_save = {
        .uri     = "/save",
        .method  = HTTP_POST,
        .handler = save_handler,
    };
    const httpd_uri_t uri_config = {
        .uri     = "/config",
        .method  = HTTP_GET,
        .handler = config_handler,
    };
    const httpd_uri_t uri_catch_all_get = {
        .uri     = "/*",
        .method  = HTTP_GET,
        .handler = redirect_handler,
    };
    const httpd_uri_t uri_catch_all_post = {
        .uri     = "/*",
        .method  = HTTP_POST,
        .handler = redirect_handler,
    };

    httpd_register_uri_handler(s_server, &uri_root);
    httpd_register_uri_handler(s_server, &uri_config);
    httpd_register_uri_handler(s_server, &uri_scan);
    httpd_register_uri_handler(s_server, &uri_save);
    httpd_register_uri_handler(s_server, &uri_catch_all_get);
    httpd_register_uri_handler(s_server, &uri_catch_all_post);

    ESP_LOGI(TAG, "HTTP server started on port %d", port);
    return ESP_OK;
}

esp_err_t http_server_stop(void)
{
    if (!s_server) {
        return ESP_OK;
    }
    esp_err_t err = httpd_stop(s_server);
    s_server = NULL;
    ESP_LOGI(TAG, "HTTP server stopped");
    return err;
}