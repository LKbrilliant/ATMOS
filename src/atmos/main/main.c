#include <stdio.h>
#include <math.h>
#include <time.h>

#include "sdkconfig.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/semphr.h"
#include "driver/gpio.h"
#include "esp_log.h"
#include "esp_http_client.h"
#include "esp_wifi.h"
#include "esp_sleep.h"
#include "cJSON.h"

#include "TCA9554PWR.h"
#include "PCF85063.h"
#include "QMI8658.h"
#include "ST7701S.h"
#include "SD_MMC.h"
#include "LVGL_Driver.h"
#include "LVGL_Example.h"
#include "Wireless.h"
#include "BAT_Driver.h"
#include "wifi_provisioner.h"
#include "nvs_flash.h"

/* --- LOG TAG --- */
static const char *TAG = "ATMOS_MAIN";

/* --- FONTS --- */
LV_FONT_DECLARE(roboto_condensed_light_150)
LV_FONT_DECLARE(roboto_condensed_light_60)

/* --- CONFIGURATION & CONSTANTS --- */
#define UPDATE_INTERVAL_MIN    15
#define MAX_HTTP_RECV_BUFFER   4096

#define COLOR_APP_BACKGROUND   0x0F172A // Dark Blue
#define COLOR_OUTER_RING       0x06B6D4 // Cyan / Bright Blue

#define RESET_BUTTON_GPIO      GPIO_NUM_0
#define HOLD_TIME_MS           3000

#define BACKLIGHT_PIN          GPIO_NUM_6
#define BACKLIGHT_SPEED_MODE   LEDC_LOW_SPEED_MODE
#define BACKLIGHT_CHANNEL      LEDC_CHANNEL_0
#define BACKLIGHT_TIMER        LEDC_TIMER_0
#define BACKLIGHT_DUTY_RES     LEDC_TIMER_10_BIT

/* --- FALLBACK VALUES --- */
static float g_temp_low_end = -30.0f;
static float g_temp_high_end = 35.0f;
static char g_latitude[16]   = "49.826";
static char g_longitude[16]  = "-97.143";

static SemaphoreHandle_t lvgl_mux = NULL;
static lv_obj_t *wifi_status_label = NULL;
static lv_obj_t *wifi_screen = NULL;
static lv_fs_drv_t my_sd_drv;

/* --- DATA STRUCTURES --- */
typedef struct {
    float temp;
    float feels_like;
    int weather_code;
    int is_day;
} current_weather_t;

typedef struct {
    char date[8][16];
    float temp_max[8];
    float temp_min[8];
    int weather_code[8];
    float moon_phase[8];
} forecast_weather_t;

static current_weather_t current_weather;
static forecast_weather_t forecast_weather;

/* --- LVGL MUTEX LOCK HELPER FUNCTIONS --- */
void lvgl_lock(void) 
{
    if (lvgl_mux != NULL) {
        xSemaphoreTake(lvgl_mux, portMAX_DELAY);
    }
}

void lvgl_unlock(void) 
{
    if (lvgl_mux != NULL) {
        xSemaphoreGive(lvgl_mux);
    }
}

/* --- HARDWARE & BACKLIGHT HANDLERS --- */
void init_backlight_pwm(void) 
{
    ledc_timer_config_t ledc_timer = {
        .speed_mode       = BACKLIGHT_SPEED_MODE,
        .timer_num        = BACKLIGHT_TIMER,
        .duty_resolution  = BACKLIGHT_DUTY_RES,
        .freq_hz          = 5000,
        .clk_cfg          = LEDC_AUTO_CLK
    };
    ledc_timer_config(&ledc_timer);

    ledc_channel_config_t ledc_channel = {
        .speed_mode     = BACKLIGHT_SPEED_MODE,
        .channel        = BACKLIGHT_CHANNEL,
        .timer_sel      = BACKLIGHT_TIMER,
        .intr_type      = LEDC_INTR_DISABLE,
        .gpio_num       = BACKLIGHT_PIN,
        .duty           = 1023,
        .hpoint         = 0
    };
    ledc_channel_config(&ledc_channel);
}

void set_backlight_brightness(uint8_t brightness_percent) 
{
    if (brightness_percent > 100) brightness_percent = 100;
    uint32_t duty = (uint32_t)((brightness_percent / 100.0f) * 1023.0f);

    ledc_set_duty(BACKLIGHT_SPEED_MODE, BACKLIGHT_CHANNEL, duty);
    ledc_update_duty(BACKLIGHT_SPEED_MODE, BACKLIGHT_CHANNEL);
}

void reset_wifi_and_restart(void)
{
    ESP_LOGW("RESET", "Erasing Wi-Fi credentials via wifi_provisioner...");
    esp_err_t err = wifi_prov_erase_credentials();
    if (err == ESP_OK) {
        ESP_LOGI("RESET", "Wi-Fi credentials erased successfully.");
    } else {
        ESP_LOGE("RESET", "Failed to erase credentials: %s", esp_err_to_name(err));
    }

    vTaskDelay(pdMS_TO_TICKS(500));
    esp_sleep_enable_timer_wakeup(100 * 1000);
    esp_deep_sleep_start();
}

static void reset_button_task(void *pvParameters)
{
    gpio_config_t io_conf = {
        .pin_bit_mask = (1ULL << RESET_BUTTON_GPIO),
        .mode = GPIO_MODE_INPUT,
        .pull_up_en = GPIO_PULLUP_ENABLE,
        .pull_down_en = GPIO_PULLDOWN_DISABLE,
        .intr_type = GPIO_INTR_DISABLE,
    };
    gpio_config(&io_conf);

    uint32_t press_duration = 0;

    while (1) {
        if (gpio_get_level(RESET_BUTTON_GPIO) == 0) {
            press_duration += 100;
            if (press_duration >= HOLD_TIME_MS) {
                ESP_LOGI("RESET", "Reset button hold detected!");
                reset_wifi_and_restart();
            }
        } else {
            press_duration = 0;
        }
        vTaskDelay(pdMS_TO_TICKS(100));
    }
}

void reset_button_init(void)
{
    xTaskCreate(reset_button_task, "reset_button_task", 4096, NULL, 10, NULL);
}

/* --- LVGL CUSTOM SD CARD VFS CALLBACKS --- */
static void * fs_open_cb(struct _lv_fs_drv_t * drv, const char * path, lv_fs_mode_t mode) 
{
    const char * flags = "";
    if(mode == LV_FS_MODE_WR) flags = "wb";
    else if(mode == LV_FS_MODE_RD) flags = "rb";
    else if(mode == (LV_FS_MODE_WR | LV_FS_MODE_RD)) flags = "r+b";

    char full_path[128];
    if (path[0] == '/') {
        snprintf(full_path, sizeof(full_path), "/sdcard%s", path);
    } else {
        snprintf(full_path, sizeof(full_path), "/sdcard/%s", path);
    }

    FILE *f = fopen(full_path, flags);
    if (f == NULL) {
        printf("[LVGL FS ERROR] Failed to open: %s\n", full_path);
    }
    return (void *)f;
}

static lv_fs_res_t fs_close_cb(struct _lv_fs_drv_t * drv, void * file_p) 
{
    if(!file_p) return LV_FS_RES_INV_PARAM;
    fclose((FILE *)file_p);
    return LV_FS_RES_OK;
}

static lv_fs_res_t fs_read_cb(struct _lv_fs_drv_t * drv, void * file_p, void * buf, uint32_t btr, uint32_t * br) 
{
    if(!file_p || !buf) return LV_FS_RES_INV_PARAM;
    *br = fread(buf, 1, btr, (FILE *)file_p);
    return LV_FS_RES_OK;
}

static lv_fs_res_t fs_seek_cb(struct _lv_fs_drv_t * drv, void * file_p, uint32_t pos, lv_fs_whence_t whence) 
{
    if(!file_p) return LV_FS_RES_INV_PARAM;
    int w = SEEK_SET;
    if(whence == LV_FS_SEEK_CUR) w = SEEK_CUR;
    else if(whence == LV_FS_SEEK_END) w = SEEK_END;
    
    fseek((FILE *)file_p, pos, w);
    return LV_FS_RES_OK;
}

static lv_fs_res_t fs_tell_cb(struct _lv_fs_drv_t * drv, void * file_p, uint32_t * pos_p) 
{
    if(!file_p || !pos_p) return LV_FS_RES_INV_PARAM;
    *pos_p = ftell((FILE *)file_p);
    return LV_FS_RES_OK;
}

void register_custom_sd_driver(void) 
{
    lv_fs_drv_init(&my_sd_drv);
    my_sd_drv.letter = 'S';
    my_sd_drv.open_cb = fs_open_cb;
    my_sd_drv.close_cb = fs_close_cb;
    my_sd_drv.read_cb = fs_read_cb;
    my_sd_drv.seek_cb = fs_seek_cb;
    my_sd_drv.tell_cb = fs_tell_cb;
    lv_fs_drv_register(&my_sd_drv);
    ESP_LOGI(TAG, "LVGL Driver S: mapped to /sdcard/");
}

/* --- CALCULATIONS & UTILITIES --- */
static float calculate_local_moon_phase(const char* date_str) 
{
    int year, month, day;
    if (sscanf(date_str, "%d-%d-%d", &year, &month, &day) != 3) {
        return 0.25f;
    }

    struct tm t = {0};
    t.tm_year = year - 1900;
    t.tm_mon = month - 1;
    t.tm_mday = day;
    time_t target_time = mktime(&t);

    const time_t known_new_moon = 947116800; // Jan 6, 2000
    const double synodic_month = 2551443;   // 29.53059 days

    if (target_time < known_new_moon) return 0.0f;

    double diff_seconds = difftime(target_time, known_new_moon);
    double cycle_position = fmod(diff_seconds, synodic_month);
    return (float)(cycle_position / synodic_month);
}

const char* get_weekday_from_date(const char* date_str) 
{
    int y, m, d;
    if (sscanf(date_str, "%d-%d-%d", &y, &m, &d) != 3) return "???";

    if (m < 3) {
        m += 12;
        y--;
    }
    int k = y % 100;
    int j = y / 100;
    int h = (d + 13 * (m + 1) / 5 + k + k / 4 + j / 4 + 5 * j) % 7;

    const char* days[] = {"SAT", "SUN", "MON", "TUE", "WED", "THU", "FRI"};
    return days[h];
}

static lv_color_t get_temp_color(float temp) 
{
    if (temp < g_temp_low_end)  temp = g_temp_low_end;
    if (temp > g_temp_high_end) temp = g_temp_high_end;

    float temp_green_target = 21.0f;
    float temp_blue_mid = g_temp_low_end + (temp_green_target - g_temp_low_end) / 2.0f;

    lv_color_t purple_cold = lv_color_hex(0xA855F7);
    lv_color_t blue_mid    = lv_color_hex(0x3498DB);
    lv_color_t green_nice  = lv_color_hex(0x2ECC71);
    lv_color_t red_hot     = lv_color_hex(0xEF4444);

    if (temp < temp_blue_mid) {
        float range_span = temp_blue_mid - g_temp_low_end;
        uint8_t ratio = (uint8_t)(((temp - g_temp_low_end) / range_span) * 255.0f);
        return lv_color_mix(blue_mid, purple_cold, ratio);
    } else if (temp < temp_green_target) {
        float range_span = temp_green_target - temp_blue_mid;
        uint8_t ratio = (uint8_t)(((temp - temp_blue_mid) / range_span) * 255.0f);
        return lv_color_mix(green_nice, blue_mid, ratio);
    } else {
        float range_span = g_temp_high_end - temp_green_target;
        uint8_t ratio = (uint8_t)(((temp - temp_green_target) / range_span) * 255.0f);
        return lv_color_mix(red_hot, green_nice, ratio);
    }
}

static void get_main_icon_path(char *buffer, size_t max_len) 
{
    int code = current_weather.weather_code;

    if (current_weather.is_day == 0) {
        if (code == 0) {
            float moon = calculate_local_moon_phase(forecast_weather.date[0]);
            if (moon < 0.05f || moon > 0.95f) {
                snprintf(buffer, max_len, "S:/0_new_moon_100.bin");
                return;
            } else if (moon > 0.45f && moon < 0.55f) {
                snprintf(buffer, max_len, "S:/0_full_moon_100.bin");
                return;
            } else {
                snprintf(buffer, max_len, "S:/0_night_100.bin");
                return;
            }
        }
        
        if (code == 1 || code == 45 || code == 48 || code == 51 || code == 53 || code == 55) {
            snprintf(buffer, max_len, "S:/%d_night_100.bin", code);
            return;
        }
    }

    snprintf(buffer, max_len, "S:/%d_100.bin", code);
}

/* --- UI RENDERING --- */
void build_weather_ui(void) 
{
    lv_obj_set_style_bg_color(lv_scr_act(), lv_color_hex(COLOR_APP_BACKGROUND), 0);
    lv_obj_clear_flag(lv_scr_act(), LV_OBJ_FLAG_SCROLLABLE);

    // --- MAIN ICON ---
    char main_icon_path[64];
    get_main_icon_path(main_icon_path, sizeof(main_icon_path));

    lv_obj_t * main_icon = lv_img_create(lv_scr_act());
    lv_img_set_src(main_icon, main_icon_path);
    lv_obj_align(main_icon, LV_ALIGN_TOP_MID, 0, 40); 

    // --- CURRENT TEMP ---
    lv_obj_t * temp_num_lbl = lv_label_create(lv_scr_act());
    lv_obj_set_style_text_font(temp_num_lbl, &roboto_condensed_light_150, 0); 
    lv_obj_set_style_text_color(temp_num_lbl, lv_color_white(), 0);
    lv_label_set_text_fmt(temp_num_lbl, "%d", (int)current_weather.temp);
    lv_obj_align(temp_num_lbl, LV_ALIGN_TOP_MID, -15, 160); 

    lv_obj_t * temp_unit_lbl = lv_label_create(lv_scr_act());
    lv_obj_set_style_text_font(temp_unit_lbl, &roboto_condensed_light_60, 0);
    lv_obj_set_style_text_color(temp_unit_lbl, lv_color_hex(0xCBD5E1), 0);
    lv_label_set_text(temp_unit_lbl, "°C");
    lv_obj_align_to(temp_unit_lbl, temp_num_lbl, LV_ALIGN_OUT_RIGHT_TOP, 2, 4);

    // --- FEELS LIKE ---
    lv_obj_t * feels_label = lv_label_create(lv_scr_act());
    lv_obj_set_style_text_font(feels_label, &lv_font_montserrat_16, 0);
    lv_obj_set_style_text_color(feels_label, lv_color_hex(0x94A3B8), 0);
    lv_label_set_text_fmt(feels_label, "FEELS LIKE %d°", (int)current_weather.feels_like);
    lv_obj_align(feels_label, LV_ALIGN_TOP_MID, 0, 285);

    // --- RANGE BAR ---
    lv_obj_t * range_bar = lv_bar_create(lv_scr_act());
    lv_obj_set_size(range_bar, 180, 8);
    lv_obj_align(range_bar, LV_ALIGN_TOP_MID, 0, 315);
    
    lv_bar_set_range(range_bar, (int32_t)g_temp_low_end, (int32_t)g_temp_high_end);
    
    int32_t bar_val = (int32_t)current_weather.feels_like;
    if (bar_val < (int32_t)g_temp_low_end)  bar_val = (int32_t)g_temp_low_end;
    if (bar_val > (int32_t)g_temp_high_end) bar_val = (int32_t)g_temp_high_end;
    
    if (bar_val == (int32_t)g_temp_low_end) bar_val = (int32_t)g_temp_low_end + 4; 
    
    lv_bar_set_value(range_bar, bar_val, LV_ANIM_OFF);

    lv_color_t active_color = get_temp_color(current_weather.feels_like);
    lv_obj_set_style_bg_color(range_bar, lv_color_hex(0x758DAD), LV_PART_MAIN); 
    lv_obj_set_style_bg_color(range_bar, active_color, LV_PART_INDICATOR);      
    lv_obj_set_style_bg_opa(range_bar, LV_OPA_COVER, LV_PART_INDICATOR); 
    lv_obj_set_style_radius(range_bar, 4, 0);

    // --- 5-DAY FORECAST RADIAL DISPLAY ---
    int items_count = 5;
    float start_angle_deg = 140.0f;
    float end_angle_deg   = 40.0f;
    float angle_step = (end_angle_deg - start_angle_deg) / (items_count - 1);
    float radius = 185.0f; 

    for (int i = 0; i < items_count; i++) {
        float current_angle_deg = start_angle_deg + (i * angle_step);
        float angle_rad = current_angle_deg * (M_PI / 180.0f);

        int x_pos = (int)(radius * cosf(angle_rad));
        int y_pos = (int)(radius * sinf(angle_rad));

        lv_obj_t * col = lv_obj_create(lv_scr_act());
        lv_obj_set_size(col, 75, 120);
        lv_obj_clear_flag(col, LV_OBJ_FLAG_SCROLLABLE);
        lv_obj_set_style_bg_opa(col, LV_OPA_TRANSP, 0);
        lv_obj_set_style_border_width(col, 0, 0);
        lv_obj_set_style_pad_all(col, 0, 0);
        lv_obj_align(col, LV_ALIGN_CENTER, x_pos, y_pos);

        char sub_icon_path[32];
        snprintf(sub_icon_path, sizeof(sub_icon_path), "S:/%d_60.bin", forecast_weather.weather_code[i]);
        
        lv_obj_t * sub_icon = lv_img_create(col);
        lv_img_set_src(sub_icon, sub_icon_path);
        lv_obj_align(sub_icon, LV_ALIGN_TOP_MID, 0, 0);

        lv_obj_t * day_lbl = lv_label_create(col);
        lv_obj_set_style_text_font(day_lbl, &lv_font_montserrat_12, 0);
        lv_obj_set_style_text_color(day_lbl, lv_color_hex(0x94A3B8), LV_PART_MAIN);
        lv_label_set_text(day_lbl, get_weekday_from_date(forecast_weather.date[i]));
        lv_obj_align(day_lbl, LV_ALIGN_TOP_MID, 0, 65);

        lv_obj_t * min_max_lbl = lv_label_create(col);
        lv_obj_set_style_text_font(min_max_lbl, &lv_font_montserrat_12, 0);
        lv_obj_set_style_text_color(min_max_lbl, lv_color_white(), 0);
        lv_label_set_text_fmt(min_max_lbl, "%d/%d°", (int)forecast_weather.temp_min[i], (int)forecast_weather.temp_max[i]);
        lv_obj_align(min_max_lbl, LV_ALIGN_TOP_MID, 0, 85);
    }

    // --- OUTER BEZEL RING ---
    lv_obj_t * edge_ring = lv_obj_create(lv_scr_act());
    lv_obj_set_size(edge_ring, 480, 480);
    lv_obj_center(edge_ring);
    lv_obj_clear_flag(edge_ring, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_add_flag(edge_ring, LV_OBJ_FLAG_IGNORE_LAYOUT);
    lv_obj_set_style_bg_opa(edge_ring, LV_OPA_TRANSP, 0);
    lv_obj_set_style_pad_all(edge_ring, 0, 0);
    lv_obj_set_style_radius(edge_ring, LV_RADIUS_CIRCLE, 0);
    lv_obj_set_style_border_color(edge_ring, lv_color_hex(COLOR_OUTER_RING), 0); 
    lv_obj_set_style_border_width(edge_ring, 4, 0);
    lv_obj_set_style_border_opa(edge_ring, LV_OPA_80, 0);
}

/* --- JSON PARSER & HTTP TASK --- */
static void parse_weather_json(const char *json_string)
{
    cJSON *root = cJSON_Parse(json_string);
    if (root == NULL) {
        ESP_LOGE(TAG, "Failed to parse JSON");
        return;
    }

    cJSON *current = cJSON_GetObjectItemCaseSensitive(root, "current");
    if (current != NULL) {
        cJSON *is_day_val = cJSON_GetObjectItemCaseSensitive(current, "is_day");
        cJSON *temp = cJSON_GetObjectItemCaseSensitive(current, "temperature_2m");
        cJSON *apparent = cJSON_GetObjectItemCaseSensitive(current, "apparent_temperature");
        cJSON *code = cJSON_GetObjectItemCaseSensitive(current, "weather_code");

        if (is_day_val) current_weather.is_day = is_day_val->valueint;
        if (temp) current_weather.temp = temp->valuedouble;
        if (apparent) current_weather.feels_like = apparent->valuedouble;
        if (code) current_weather.weather_code = code->valueint;

        ESP_LOGI(TAG, "Current Temp: %.1f C | Feels Like: %.1f C | Code: %d", 
                 current_weather.temp, current_weather.feels_like, current_weather.weather_code);
    }

    cJSON *daily = cJSON_GetObjectItemCaseSensitive(root, "daily");
    if (daily != NULL) {
        cJSON *time_arr = cJSON_GetObjectItemCaseSensitive(daily, "time");
        cJSON *temp_max_arr = cJSON_GetObjectItemCaseSensitive(daily, "temperature_2m_max");
        cJSON *temp_min_arr = cJSON_GetObjectItemCaseSensitive(daily, "temperature_2m_min");
        cJSON *code_arr = cJSON_GetObjectItemCaseSensitive(daily, "weather_code");

        int days_count = cJSON_GetArraySize(time_arr);
        if (days_count > 7) days_count = 7;

        for (int i = 0; i < days_count; i++) {
            cJSON *time_val = cJSON_GetArrayItem(time_arr, i);
            cJSON *max_val = cJSON_GetArrayItem(temp_max_arr, i);
            cJSON *min_val = cJSON_GetArrayItem(temp_min_arr, i);
            cJSON *code_val = cJSON_GetArrayItem(code_arr, i);

            if (time_val) snprintf(forecast_weather.date[i], sizeof(forecast_weather.date[i]), "%s", time_val->valuestring);
            if (max_val) forecast_weather.temp_max[i] = max_val->valuedouble;
            if (min_val) forecast_weather.temp_min[i] = min_val->valuedouble;
            if (code_val) forecast_weather.weather_code[i] = code_val->valueint;
        }
    }

    cJSON_Delete(root);
}

static void weather_fetch_task(void *pvParameters)
{
    char url[256];
    snprintf(url, sizeof(url),
        "http://api.open-meteo.com/v1/forecast?"
        "latitude=%s&longitude=%s"
        "&daily=weather_code,temperature_2m_max,temperature_2m_min"
        "&current=temperature_2m,apparent_temperature,weather_code,is_day"
        "&timezone=auto", g_latitude, g_longitude);

    esp_http_client_config_t http_config = {
        .url = url,
        .timeout_ms = 5000,
    };

    const TickType_t xDelay = pdMS_TO_TICKS(UPDATE_INTERVAL_MIN * 60 * 1000);

    char *response_buffer = malloc(MAX_HTTP_RECV_BUFFER);
    if (response_buffer == NULL) {
        ESP_LOGE(TAG, "Failed to allocate memory for HTTP buffer");
        vTaskDelete(NULL);
        return;
    }

    while(1) {
        memset(response_buffer, 0, MAX_HTTP_RECV_BUFFER);
        esp_http_client_handle_t client = esp_http_client_init(&http_config);
        
        if (client != NULL) {
            esp_err_t err = esp_http_client_open(client, 0);
            if (err == ESP_OK) {
                esp_http_client_fetch_headers(client); 
                int status_code = esp_http_client_get_status_code(client);
                
                if (status_code == 200) {
                    int total_read_len = 0;
                    int read_len = 0;
                    
                    while (total_read_len < (MAX_HTTP_RECV_BUFFER - 1)) {
                        read_len = esp_http_client_read(client, response_buffer + total_read_len, MAX_HTTP_RECV_BUFFER - total_read_len - 1);
                        if (read_len <= 0) break;
                        total_read_len += read_len;
                    }
                    
                    response_buffer[total_read_len] = '\0';
                    
                    if (total_read_len > 0) {
                        ESP_LOGI(TAG, "Weather payload fetched successfully!");
                        parse_weather_json(response_buffer);
                        
                        lvgl_lock();
                        lv_obj_clean(lv_scr_act()); 
                        build_weather_ui();
                        lvgl_unlock();
                    }
                }
            }
            esp_http_client_cleanup(client);
        }

        ESP_LOGI(TAG, "Sleeping for %d minutes before next update...", UPDATE_INTERVAL_MIN);
        vTaskDelay(xDelay);
    }

    free(response_buffer);
    vTaskDelete(NULL);
}

/* --- CAPTIVE PORTAL HANDLERS --- */
static void on_captive_portal_start(void)
{
    ESP_LOGI(TAG, "Captive portal started");

    wifi_screen = lv_obj_create(lv_scr_act());
    lv_obj_set_size(wifi_screen, LV_PCT(100), LV_PCT(100));
    lv_obj_clear_flag(wifi_screen, LV_OBJ_FLAG_SCROLLABLE); 
    lv_obj_set_style_radius(wifi_screen, 0, 0);
    lv_obj_set_style_border_width(wifi_screen, 0, 0);
    lv_obj_set_style_pad_all(wifi_screen, 0, 0);
    lv_obj_set_style_bg_color(wifi_screen, lv_color_hex(0x111111), 0); 

    wifi_status_label = lv_label_create(wifi_screen);
    lv_label_set_long_mode(wifi_status_label, LV_LABEL_LONG_WRAP); 
    lv_obj_set_width(wifi_status_label, LV_PCT(85));               
    lv_obj_align(wifi_status_label, LV_ALIGN_CENTER, 0, 0);
    lv_obj_set_style_text_align(wifi_status_label, LV_TEXT_ALIGN_CENTER, 0);
    lv_obj_set_style_text_color(wifi_status_label, lv_color_hex(0xFFFFFF), 0);

    lv_label_set_text(wifi_status_label, 
        "WiFi Setup Required\n\n"
        "Connect your phone to:\n\n"
        "#00FF00 Atmos-Portal#\n\n"
        "And follow instructions."
    );
    lv_label_set_recolor(wifi_status_label, true); 
}

static void on_WiFi_connected(void)
{
    ESP_LOGI(TAG, "WiFi connected!");
    if (wifi_status_label != NULL) {
        lv_label_set_text(wifi_status_label, "Connected successfully!\nBooting Atmos...");
    }
}

/* --- INITIALIZATION & DRIVER LOOPS --- */
void load_system_config(void)
{
    app_config_t cfg = {0};
    esp_err_t err = wifi_prov_get_app_config(&cfg);
    
    if (err == ESP_OK) {
        ESP_LOGI(TAG, "NVS Read OK -> Lat: '%s', Lon: '%s', Min: %.1f, Max: %.1f", 
                 cfg.latitude, cfg.longitude, cfg.temp_min, cfg.temp_max);

        if (strlen(cfg.latitude) > 0)  snprintf(g_latitude, sizeof(g_latitude), "%s", cfg.latitude);
        if (strlen(cfg.longitude) > 0) snprintf(g_longitude, sizeof(g_longitude), "%s", cfg.longitude);
        g_temp_low_end = cfg.temp_min;
        g_temp_high_end = cfg.temp_max;
    } else {
        ESP_LOGE(TAG, "Failed to load config from NVS! Error: %s (0x%X)", esp_err_to_name(err), err);
    }
}

void Driver_Loop(void *parameter)
{
    while(1)
    {
        QMI8658_Loop();
        RTC_Loop();
        BAT_Get_Volts();
        vTaskDelay(pdMS_TO_TICKS(100));
    }
    vTaskDelete(NULL);
}

void Driver_Init(void)
{
    Flash_Searching();
    BAT_Init();
    I2C_Init();
    PCF85063_Init();
    EXIO_Init();
    xTaskCreatePinnedToCore(
        Driver_Loop, 
        "Other Driver task",
        4096, 
        NULL, 
        3, 
        NULL, 
        0);
}

/* --- MAIN ENTRY POINT --- */
void app_main(void)
{   
    // Initialize Non-Volatile Storage (NVS)
    esp_err_t ret = nvs_flash_init();
    if (ret == ESP_ERR_NVS_NO_FREE_PAGES || ret == ESP_ERR_NVS_NEW_VERSION_FOUND) {
        ESP_ERROR_CHECK(nvs_flash_erase());
        ret = nvs_flash_init();
    }
    ESP_ERROR_CHECK(ret);

    lvgl_mux = xSemaphoreCreateMutex();
    if (lvgl_mux == NULL) {
        ESP_LOGE(TAG, "Failed to create LVGL Mutex Lock!");
        return;
    }

    // Load active application configuration
    load_system_config();

    // Print active configuration values to console
    ESP_LOGI(TAG, "==================================================");
    ESP_LOGI(TAG, " ACTIVE SYSTEM CONFIGURATION:");
    ESP_LOGI(TAG, "   Latitude:    %s", g_latitude);
    ESP_LOGI(TAG, "   Longitude:   %s", g_longitude);
    ESP_LOGI(TAG, "   Temp Range:  %.1f deg C to %.1f deg C", g_temp_low_end, g_temp_high_end);
    ESP_LOGI(TAG, "==================================================");

    // Hardware & display drivers initialization
    Driver_Init();
    LCD_Init();
    SD_Init();
    LVGL_Init();
    register_custom_sd_driver();

    // Wi-Fi & Captive Portal setup
    wifi_prov_config_t config = WIFI_PROV_DEFAULT_CONFIG();
    config.ap_ssid              = "Atmos-Portal";
    config.page_title           = "Atmos Network Setup";
    config.portal_header        = "Connect Atmos to your WiFi";
    config.portal_subheader     = "Select your network below.";
    config.connected_header     = "Done!";
    config.connected_subheader  = "Atmos is now connected.";
    config.page_footer          = "Copyright &copy; Atmos";

    config.on_connected   = on_WiFi_connected;
    config.on_portal_start = on_captive_portal_start;

    ESP_ERROR_CHECK(wifi_prov_start(&config));
    
    while (!wifi_prov_is_connected()) {
        lv_timer_handler();
        vTaskDelay(pdMS_TO_TICKS(10));
    }

    if (wifi_screen != NULL) {
        lv_obj_del(wifi_screen);
        wifi_screen = NULL;
        wifi_status_label = NULL;
    }

    init_backlight_pwm();
    set_backlight_brightness(100);

    xTaskCreatePinnedToCore(
        weather_fetch_task, 
        "Weather Fetch Task", 
        6144, 
        NULL, 
        5, 
        NULL, 
        1);

    reset_button_init();

    // 6. Main LVGL Execution Loop
    while (1) {
        vTaskDelay(pdMS_TO_TICKS(10));
        lvgl_lock();
        lv_timer_handler();
        lvgl_unlock();
    }
}