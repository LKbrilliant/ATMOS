#include <stdio.h>
#include "sdkconfig.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/semphr.h"
#include "TCA9554PWR.h"
#include "PCF85063.h"
#include "QMI8658.h"
#include "ST7701S.h"
#include "SD_MMC.h"
#include "LVGL_Driver.h"
#include "LVGL_Example.h"
#include "Wireless.h"
#include "BAT_Driver.h"

#include <math.h>
#include <time.h>
#include "esp_log.h"
#include "esp_http_client.h"
#include "cJSON.h"
#include "wifi_provisioner.h"
#include "driver/gpio.h"
#include "esp_wifi.h"
#include "esp_sleep.h"

LV_FONT_DECLARE(roboto_condensed_light_150)
LV_FONT_DECLARE(roboto_condensed_light_60)
// LV_FONT_DECLARE(roboto_condensed_light_16)
// LV_FONT_DECLARE(roboto_condensed_light_12)

// Weather Fetch Sync Interval in Minutes
#define UPDATE_INTERVAL_MIN    15

// Temperature Range Boundaries
#define TEMP_LOW_END      -40.0f   // Purple
#define TEMP_HIGH_END      40.0f   // Red
#define TEMP_GREEN_TARGET  21.0f   // Green (Ideal Room Temp)
#define TEMP_BLUE_MID     (TEMP_LOW_END + (TEMP_GREEN_TARGET - TEMP_LOW_END) / 2.0f)

#define COLOR_APP_BACKGROUND  0x0F172A // dark blue
#define COLOR_OUTER_RING      0x06B6D4 // bright blue/cyan 

#define MAX_HTTP_RECV_BUFFER 4096 // 4KB is perfect for this parsed JSON string
// #define WEATHER_URL "http://api.open-meteo.com/v1/forecast?latitude=49.8844&longitude=-97.147&daily=weather_code,temperature_2m_max,temperature_2m_min&current=temperature_2m,apparent_temperature,weather_code,is_day&timezone=auto"

// Geolocation Coordinates
#define VAL_LATITUDE   "49.8844"
#define VAL_LONGITUDE  "-97.147"

#define RESET_BUTTON_GPIO    GPIO_NUM_0  // BOOT button on most ESP32 boards
#define HOLD_TIME_MS         3000        // Hold for 3 seconds to trigger reset


// API Endpoint Architecture Construction
#define WEATHER_URL \
    "http://api.open-meteo.com/v1/forecast?" \
    "latitude=" VAL_LATITUDE \
    "&longitude=" VAL_LONGITUDE \
    "&daily=weather_code,temperature_2m_max,temperature_2m_min" \
    "&current=temperature_2m,apparent_temperature,weather_code,is_day" \
    "&timezone=auto"

#define BACKLIGHT_PIN          GPIO_NUM_6           // Onboard hardware backlight control pin
#define BACKLIGHT_SPEED_MODE   LEDC_LOW_SPEED_MODE
#define BACKLIGHT_CHANNEL      LEDC_CHANNEL_0
#define BACKLIGHT_TIMER        LEDC_TIMER_0
#define BACKLIGHT_DUTY_RES     LEDC_TIMER_10_BIT    // 10-bit resolution = range from 0 to 1023

static SemaphoreHandle_t lvgl_mux = NULL;

static lv_obj_t *wifi_status_label = NULL;
static lv_obj_t *wifi_screen = NULL;

static lv_fs_drv_t my_sd_drv;

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

    // esp_restart();
    esp_sleep_enable_timer_wakeup(100 * 1000); // 100 ms
    esp_deep_sleep_start();
}

static void reset_button_task(void *pvParameters)
{
    // Configure GPIO 0 as input with internal pull-up
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
        // Active LOW: Button is pressed when level is 0
        if (gpio_get_level(RESET_BUTTON_GPIO) == 0) {
            press_duration += 100;
            
            if (press_duration >= HOLD_TIME_MS) {
                ESP_LOGI("RESET", "Reset button hold detected!");
                reset_wifi_and_restart();
            }
        } else {
            press_duration = 0; // Reset counter if released early
        }

        vTaskDelay(pdMS_TO_TICKS(100)); // Check every 100ms
    }
}

void reset_button_init(void)
{
    xTaskCreate(reset_button_task, "reset_button_task", 4096, NULL, 10, NULL);
}

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

static float calculate_local_moon_phase(const char* date_str) 
{
    int year, month, day;
    if (sscanf(date_str, "%d-%d-%d", &year, &month, &day) != 3) {
        return 0.25f; // Safe quarter-moon fallback on parse failure
    }

    struct tm t = {0};
    t.tm_year = year - 1900;
    t.tm_mon = month - 1;
    t.tm_mday = day;
    time_t target_time = mktime(&t);

    // Reference New Moon: January 6, 2000 (Seconds since Unix Epoch)
    const time_t known_new_moon = 947116800; 
    const double synodic_month = 2551443; // 29.53059 days in seconds

    if (target_time < known_new_moon) return 0.0f;

    double diff_seconds = difftime(target_time, known_new_moon);
    double cycle_position = fmod(diff_seconds, synodic_month);
    
    // Normalize to a fraction value between 0.00 and 1.00
    return (float)(cycle_position / synodic_month);
}

// Custom callbacks to map LVGL to standard C standard library (VFS /sdcard)
static void * fs_open_cb(struct _lv_fs_drv_t * drv, const char * path, lv_fs_mode_t mode) 
{
    const char * flags = "";
    if(mode == LV_FS_MODE_WR) flags = "wb";
    else if(mode == LV_FS_MODE_RD) flags = "rb";
    else if(mode == (LV_FS_MODE_WR | LV_FS_MODE_RD)) flags = "r+b";

    char full_path[128];
    // Strip leading slash if LVGL includes it to prevent double '//'
    if (path[0] == '/') {
        snprintf(full_path, sizeof(full_path), "/sdcard%s", path);
    } else {
        snprintf(full_path, sizeof(full_path), "/sdcard/%s", path);
    }

    printf("[LVGL FS] Trying to open: %s\n", full_path);

    FILE *f = fopen(full_path, flags);
    if (f == NULL) {
        printf("[LVGL FS ERROR] Failed to open: %s\n", full_path);
    } else {
        printf("[LVGL FS SUCCESS] Opened file: %s\n", full_path);
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
    
    my_sd_drv.letter = 'S'; // Register 'S' drive -> "S:/..."
    my_sd_drv.open_cb = fs_open_cb;
    my_sd_drv.close_cb = fs_close_cb;
    my_sd_drv.read_cb = fs_read_cb;
    my_sd_drv.seek_cb = fs_seek_cb;
    my_sd_drv.tell_cb = fs_tell_cb;
    
    lv_fs_drv_register(&my_sd_drv);
    printf("[LVGL FS] Registered Drive S: directly mapped to /sdcard/\n");
}

// Data structure for "Today" (Current conditions)
typedef struct {
    float temp;
    float feels_like;
    int weather_code;
    int is_day;         // 1 = Day, 0 = Night
} current_weather_t;

// Data structure for the 7-day forecast lists
typedef struct {
    char date[8][16]; // 7 days (index 0 to 6) plus null-terminators
    float temp_max[8];
    float temp_min[8];
    int weather_code[8];
    float moon_phase[8];
} forecast_weather_t;

static current_weather_t current_weather;
static forecast_weather_t forecast_weather;


static lv_color_t get_temp_color(float temp) 
{
    // Clamp input strictly between the absolute system limits
    if (temp < TEMP_LOW_END)  temp = TEMP_LOW_END;
    if (temp > TEMP_HIGH_END) temp = TEMP_HIGH_END;

    // High-vibrancy hex anchors for your UI layout
    lv_color_t purple_cold = lv_color_hex(0xA855F7); // Coldest limit
    lv_color_t blue_mid    = lv_color_hex(0x3498DB); // Center of cold-to-comfortable
    lv_color_t green_nice  = lv_color_hex(0x2ECC71); // Perfect comfortable zone (21C)
    lv_color_t red_hot     = lv_color_hex(0xEF4444); // Hottest limit

    // Dynamic multi-stage interpolation math
    if (temp < TEMP_BLUE_MID) {
        // Range 1: Purple -> Blue
        float range_span = TEMP_BLUE_MID - TEMP_LOW_END;
        uint8_t ratio = (uint8_t)(((temp - TEMP_LOW_END) / range_span) * 255.0f);
        return lv_color_mix(blue_mid, purple_cold, ratio);
        
    } else if (temp < TEMP_GREEN_TARGET) {
        // Range 2: Blue -> Green
        float range_span = TEMP_GREEN_TARGET - TEMP_BLUE_MID;
        uint8_t ratio = (uint8_t)(((temp - TEMP_BLUE_MID) / range_span) * 255.0f);
        return lv_color_mix(green_nice, blue_mid, ratio);
        
    } else {
        // Range 3: Green -> Red
        float range_span = TEMP_HIGH_END - TEMP_GREEN_TARGET;
        uint8_t ratio = (uint8_t)(((temp - TEMP_GREEN_TARGET) / range_span) * 255.0f);
        return lv_color_mix(red_hot, green_nice, ratio);
    }
}

const char* get_weekday_from_date(const char* date_str) 
{
    int y, m, d;
    if (sscanf(date_str, "%d-%d-%d", &y, &m, &d) != 3) {
        return "???";
    }

    // Zeller's Congruence algorithm
    if (m < 3) {
        m += 12;
        y--;
    }
    int k = y % 100;
    int j = y / 100;
    int h = (d + 13 * (m + 1) / 5 + k + k / 4 + j / 4 + 5 * j) % 7;

    // Map Zeller's output (0 = Saturday, 1 = Sunday, ...) to names
    const char* days[] = {"SAT", "SUN", "MON", "TUE", "WED", "THU", "FRI"};
    return days[h];
}

static void get_main_icon_path(char *buffer, size_t max_len) 
{
    int code = current_weather.weather_code;

    // Check if it's currently night time
    if (current_weather.is_day == 0) {
        
        // Code 0 = Perfectly Clear Sky
        if (code == 0) {
            // Compute current moon phase using today's forecast date string
            float moon = calculate_local_moon_phase(forecast_weather.date[0]);
            
            // New Moon boundary checking (Near 0.0 or 1.0 with 5% tolerance window)
            if (moon < 0.05f || moon > 0.95f) {
                snprintf(buffer, max_len, "S:/0_new_moon_100.bin");
                return;
            }
            // Full Moon boundary checking (Near 0.5 center mark)
            else if (moon > 0.45f && moon < 0.55f) {
                snprintf(buffer, max_len, "S:/0_full_moon_100.bin");
                return;
            }
            // Standard fallback clear night icon
            else {
                snprintf(buffer, max_len, "S:/0_night_100.bin");
                return;
            }
        }
        
        // Check for specific night code variations available on your SD card
        if (code == 1 || code == 45 || code == 48 || code == 51 || code == 53 || code == 55) {
            snprintf(buffer, max_len, "S:/%d_night_100.bin", code);
            return;
        }
    }

    // Default Fallback: If it is day, or a heavy precipitation code, use the standard asset
    snprintf(buffer, max_len, "S:/%d_100.bin", code);
}

void build_weather_ui(void) 
{
    lv_obj_set_style_bg_color(lv_scr_act(), lv_color_hex(COLOR_APP_BACKGROUND), 0);

    // Disable the scrollbar
    lv_obj_clear_flag(lv_scr_act(), LV_OBJ_FLAG_SCROLLABLE);

    // --- MAIN WEATHER ICON ---
    char main_icon_path[64];
    get_main_icon_path(main_icon_path, sizeof(main_icon_path));

    // ESP_LOGI("UI_DEBUG", "Loading main weather icon path: %s", main_icon_path); // To see which image is being loaded

    lv_obj_t * main_icon = lv_img_create(lv_scr_act());
    lv_img_set_src(main_icon, main_icon_path);
    lv_obj_align(main_icon, LV_ALIGN_TOP_MID, 0, 40); 

    // --- CURRENT TEMP (Using Aligned Labels for a True Superscript) ---
    
    // The main temperature number
    lv_obj_t * temp_num_lbl = lv_label_create(lv_scr_act());
    lv_obj_set_style_text_font(temp_num_lbl, &roboto_condensed_light_150, 0); 
    lv_obj_set_style_text_color(temp_num_lbl, lv_color_white(), 0);
    lv_label_set_text_fmt(temp_num_lbl, "%d", (int)current_weather.temp);
    
    // Position it slightly left (-15px) of center to leave room for the symbol
    lv_obj_align(temp_num_lbl, LV_ALIGN_TOP_MID, -15, 160); 

    // The smaller superscript unit label (aligned directly to the main label!)
    lv_obj_t * temp_unit_lbl = lv_label_create(lv_scr_act());
    lv_obj_set_style_text_font(temp_unit_lbl, &roboto_condensed_light_60, 0);
    lv_obj_set_style_text_color(temp_unit_lbl, lv_color_hex(0xCBD5E1), 0); // Slate-300 color
    lv_label_set_text(temp_unit_lbl, "°C");

    // This puts the unit on the top-right outside edge of the number label
    lv_obj_align_to(temp_unit_lbl, temp_num_lbl, LV_ALIGN_OUT_RIGHT_TOP, 2, 4);

    // --- FEELS LIKE (Cast to int to fix "f" bug) ---
    lv_obj_t * feels_label = lv_label_create(lv_scr_act());
    lv_obj_set_style_text_font(feels_label, &lv_font_montserrat_16, 0);
    // lv_obj_set_style_text_font(feels_label, &roboto_condensed_light_16, 0);
    lv_obj_set_style_text_color(feels_label, lv_color_hex(0x94A3B8), 0);
    lv_label_set_text_fmt(feels_label, "FEELS LIKE %d°", (int)current_weather.feels_like); // Fixed!
    lv_obj_align(feels_label, LV_ALIGN_TOP_MID, 0, 285);

    // --- TEMPERATURE RANGE BAR ---
    lv_obj_t * range_bar = lv_bar_create(lv_scr_act());
    lv_obj_set_size(range_bar, 180, 8);
    lv_obj_align(range_bar, LV_ALIGN_TOP_MID, 0, 315);
    
    lv_bar_set_range(range_bar, (int32_t)TEMP_LOW_END, (int32_t)TEMP_HIGH_END);
    
    // Clamp structural value calculations safely
    int32_t bar_val = (int32_t)current_weather.feels_like;
    if (bar_val < (int32_t)TEMP_LOW_END)  bar_val = (int32_t)TEMP_LOW_END;
    if (bar_val > (int32_t)TEMP_HIGH_END) bar_val = (int32_t)TEMP_HIGH_END;
    
    // purple always stays visible on the track.
    if (bar_val == (int32_t)TEMP_LOW_END) {
        // Pads the value up by roughly 5% of the total 80-degree span (-40 to +40)
        bar_val = (int32_t)TEMP_LOW_END + 4; 
    }
    
    lv_bar_set_value(range_bar, bar_val, LV_ANIM_OFF);

    // Keep color math tied to the *true* value so it stays pure purple at the limit
    lv_color_t active_color = get_temp_color(current_weather.feels_like);
    
    lv_obj_set_style_bg_color(range_bar, lv_color_hex(0x758DAD), LV_PART_MAIN); 
    lv_obj_set_style_bg_color(range_bar, active_color, LV_PART_INDICATOR);      
    lv_obj_set_style_bg_opa(range_bar, LV_OPA_COVER, LV_PART_INDICATOR); 
    lv_obj_set_style_radius(range_bar, 4, 0);

    // --- 5-DAY FORECAST ROW WITH RADIAL DISTRIBUTION ---
    int items_count = 5;
    
    // We want the 5 items to sweep from start degrees to end degrees 
    // (with 90 degrees being straight down at the bottom)
    float start_angle_deg = 140.0f;
    float end_angle_deg   = 40.0f;
    float angle_step = (end_angle_deg - start_angle_deg) / (items_count - 1);
    
    // Radius from the screen center (adjust this to push icons closer to or further from the edge)
    float radius = 185.0f; 

    for (int i = 0; i < items_count; i++) {
        // Calculate the specific angle for this item in radians
        float current_angle_deg = start_angle_deg + (i * angle_step);
        float angle_rad = current_angle_deg * (M_PI / 180.0f);

        // Map polar coordinates to Cartesian (relative to screen center 0,0)
        int x_pos = (int)(radius * cosf(angle_rad));
        int y_pos = (int)(radius * sinf(angle_rad));

        // Create invisible layout column container
        lv_obj_t * col = lv_obj_create(lv_scr_act());
        lv_obj_set_size(col, 75, 120);
        lv_obj_clear_flag(col, LV_OBJ_FLAG_SCROLLABLE);
        lv_obj_set_style_bg_opa(col, LV_OPA_TRANSP, 0);
        lv_obj_set_style_border_width(col, 0, 0);
        lv_obj_set_style_pad_all(col, 0, 0);

        // Align the container relative to the SCREEN CENTER using our calculated offset coordinates!
        lv_obj_align(col, LV_ALIGN_CENTER, x_pos, y_pos);

        // A. Small Weather Icon (60x60)
        char sub_icon_path[32];
        snprintf(sub_icon_path, sizeof(sub_icon_path), "S:/%d_60.bin", forecast_weather.weather_code[i]);
        
        lv_obj_t * sub_icon = lv_img_create(col);
        lv_img_set_src(sub_icon, sub_icon_path);
        lv_obj_align(sub_icon, LV_ALIGN_TOP_MID, 0, 0);

        // B. Day Text
        lv_obj_t * day_lbl = lv_label_create(col);
        lv_obj_set_style_text_font(day_lbl, &lv_font_montserrat_12, 0);
        // lv_obj_set_style_text_font(day_lbl, &roboto_condensed_light_12, 0);
        lv_obj_set_style_text_color(day_lbl, lv_color_hex(0x94A3B8), LV_PART_MAIN);
        lv_label_set_text(day_lbl, get_weekday_from_date(forecast_weather.date[i]));
        lv_obj_align(day_lbl, LV_ALIGN_TOP_MID, 0, 65);

        // C. Min / Max Temp
        lv_obj_t * min_max_lbl = lv_label_create(col);
        lv_obj_set_style_text_font(min_max_lbl, &lv_font_montserrat_12, 0);
        // lv_obj_set_style_text_font(min_max_lbl, &roboto_condensed_light_12, 0);
        lv_obj_set_style_text_color(min_max_lbl, lv_color_white(), 0);
        lv_label_set_text_fmt(min_max_lbl, "%d/%d°", (int)forecast_weather.temp_min[i], (int)forecast_weather.temp_max[i]);
        lv_obj_align(min_max_lbl, LV_ALIGN_TOP_MID, 0, 85);
    }

    // --- EDGE CIRCLE BORDER ---
    lv_obj_t * edge_ring = lv_obj_create(lv_scr_act());
    
    // Size it to exactly match your 480x480 circular screen dimensions
    lv_obj_set_size(edge_ring, 480, 480);
    lv_obj_center(edge_ring);
    
    // Strip background styles and layout rules so it acts strictly as an overlay ring
    lv_obj_clear_flag(edge_ring, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_add_flag(edge_ring, LV_OBJ_FLAG_IGNORE_LAYOUT);
    lv_obj_set_style_bg_opa(edge_ring, LV_OPA_TRANSP, 0);
    lv_obj_set_style_pad_all(edge_ring, 0, 0);
    
    // Make the object a perfect circle
    lv_obj_set_style_radius(edge_ring, LV_RADIUS_CIRCLE, 0);
    
    // Define the ring border properties (Using the vibrant Cyan/Blue accent color)
    lv_obj_set_style_border_color(edge_ring, lv_color_hex(COLOR_OUTER_RING), 0); 
    
    lv_obj_set_style_border_width(edge_ring, 4, 0);         // 4px thick line around the bezel
    lv_obj_set_style_border_opa(edge_ring, LV_OPA_80, 0);   // Slightly translucent (80%) for a clean glow
}

static void parse_weather_json(const char *json_string)
{
    cJSON *root = cJSON_Parse(json_string);
    if (root == NULL) {
        ESP_LOGE("WEATHER", "Failed to parse JSON");
        return;
    }

    // Parse Current Weather
    cJSON *current = cJSON_GetObjectItemCaseSensitive(root, "current");

    cJSON *is_day_val = cJSON_GetObjectItemCaseSensitive(current, "is_day");
    if (is_day_val) current_weather.is_day = is_day_val->valueint;

    if (current != NULL) {
        cJSON *temp = cJSON_GetObjectItemCaseSensitive(current, "temperature_2m");
        cJSON *apparent = cJSON_GetObjectItemCaseSensitive(current, "apparent_temperature");
        cJSON *code = cJSON_GetObjectItemCaseSensitive(current, "weather_code");

        if (temp) current_weather.temp = temp->valuedouble;
        if (apparent) current_weather.feels_like = apparent->valuedouble;
        if (code) current_weather.weather_code = code->valueint;

        ESP_LOGI("WEATHER", "Current Temp: %.1f C | Feels Like: %.1f C | Code: %d", 
                 current_weather.temp, current_weather.feels_like, current_weather.weather_code);
    }

    // Parse 7-day Forecast (inside arrays)
    cJSON *daily = cJSON_GetObjectItemCaseSensitive(root, "daily");
    if (daily != NULL) {
        cJSON *time_arr = cJSON_GetObjectItemCaseSensitive(daily, "time");
        cJSON *temp_max_arr = cJSON_GetObjectItemCaseSensitive(daily, "temperature_2m_max");
        cJSON *temp_min_arr = cJSON_GetObjectItemCaseSensitive(daily, "temperature_2m_min");
        cJSON *code_arr = cJSON_GetObjectItemCaseSensitive(daily, "weather_code");

        int days_count = cJSON_GetArraySize(time_arr);
        if (days_count > 7) days_count = 7; // Clamp to exactly 7 days

        ESP_LOGI("WEATHER", "--- 7-Day Forecast ---");
        for (int i = 0; i < days_count; i++) {
            cJSON *time_val = cJSON_GetArrayItem(time_arr, i);
            cJSON *max_val = cJSON_GetArrayItem(temp_max_arr, i);
            cJSON *min_val = cJSON_GetArrayItem(temp_min_arr, i);
            cJSON *code_val = cJSON_GetArrayItem(code_arr, i);

            if (time_val) snprintf(forecast_weather.date[i], sizeof(forecast_weather.date[i]), "%s", time_val->valuestring);
            if (max_val) forecast_weather.temp_max[i] = max_val->valuedouble;
            if (min_val) forecast_weather.temp_min[i] = min_val->valuedouble;
            if (code_val) forecast_weather.weather_code[i] = code_val->valueint;

            ESP_LOGI("WEATHER", "Day %d [%s]: Max: %.1f C | Min: %.1f C | Code: %d", 
                     i+1, forecast_weather.date[i], forecast_weather.temp_max[i], 
                     forecast_weather.temp_min[i], forecast_weather.weather_code[i]);
        }
    }

    cJSON_Delete(root); // clean up allocated cJSON memory!
}

static void weather_fetch_task(void *pvParameters)
{
    // Define how often want to refresh
    const TickType_t xDelay = pdMS_TO_TICKS(UPDATE_INTERVAL_MIN * 60 * 1000);

    // Move the buffer allocation inside or keep it clean
    char *response_buffer = malloc(MAX_HTTP_RECV_BUFFER);
    if (response_buffer == NULL) {
        ESP_LOGE("WEATHER", "Failed to allocate memory for HTTP buffer");
        vTaskDelete(NULL);
        return;
    }

    esp_http_client_config_t config = {
        .url = WEATHER_URL,
        .timeout_ms = 5000,
    };

    // --- INFINITE LOOP KEEPS THE TASK ALIVE ---
    while(1) {
        memset(response_buffer, 0, MAX_HTTP_RECV_BUFFER);
        esp_http_client_handle_t client = esp_http_client_init(&config);
        
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
                        ESP_LOGI("WEATHER", "Weather payload fetched successfully!");
                        parse_weather_json(response_buffer);
                        
                        // --- SAFE CORE SCREEN UPDATE ---
                        lvgl_lock(); // <-- Grab the lock
                        
                        // Clean up previous UI elements to avoid creating massive memory leaks over time
                        lv_obj_clean(lv_scr_act()); 
                        
                        build_weather_ui(); 
                        
                        lvgl_unlock(); // <-- Release the lock
                    }
                }
            }
            esp_http_client_cleanup(client); // Always clean up at the end of the loop iteration
        }

        ESP_LOGI("WEATHER", "Sleeping for %d minutes before next update...", UPDATE_INTERVAL_MIN);
        vTaskDelay(xDelay); // Puts the task to sleep cleanly, freeing up the CPU
    }

    // This line is unreachable, but kept for compiler
    free(response_buffer);
    vTaskDelete(NULL); 
}

static void on_captive_portal_start(void)
{
    ESP_LOGI("ATMOS", "Captive portal started");

    // Create a temporary full-screen container over everything else
    wifi_screen = lv_obj_create(lv_scr_act());
    lv_obj_set_size(wifi_screen, LV_PCT(100), LV_PCT(100));
    lv_obj_clear_flag(wifi_screen, LV_OBJ_FLAG_SCROLLABLE); 

    // --- STRIP DEFAULT STYLES TO FIX CORNERS AND BORDERS ---
    lv_obj_set_style_radius(wifi_screen, 0, 0);          // Remove rounded corners (make it sharp)
    lv_obj_set_style_border_width(wifi_screen, 0, 0);    // Remove the default white/gray border outline
    lv_obj_set_style_pad_all(wifi_screen, 0, 0);         // Remove internal padding
    // -------------------------------------------------------

    // Set the solid dark background color
    lv_obj_set_style_bg_color(wifi_screen, lv_color_hex(0x111111), 0); 

    // Create the main text label
    wifi_status_label = lv_label_create(wifi_screen);
    lv_label_set_long_mode(wifi_status_label, LV_LABEL_LONG_WRAP); 
    lv_obj_set_width(wifi_status_label, LV_PCT(85));               
    lv_obj_align(wifi_status_label, LV_ALIGN_CENTER, 0, 0);
    lv_obj_set_style_text_align(wifi_status_label, LV_TEXT_ALIGN_CENTER, 0);
    lv_obj_set_style_text_color(wifi_status_label, lv_color_hex(0xFFFFFF), 0);

    // Prompt user to connect to the AP
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
    ESP_LOGI("ATMOS", "WiFi connected!");

    if (wifi_status_label != NULL) {
        lv_label_set_text(wifi_status_label, "Connected successfully!\nBooting Atmos...");
    }
}

void init_backlight_pwm(void) 
{
    // 1. Configure the core PWM Timer
    ledc_timer_config_t ledc_timer = {
        .speed_mode       = BACKLIGHT_SPEED_MODE,
        .timer_num        = BACKLIGHT_TIMER,
        .duty_resolution  = BACKLIGHT_DUTY_RES,
        .freq_hz          = 5000,  // 5kHz frequency eliminates physical screen flickering
        .clk_cfg          = LEDC_AUTO_CLK
    };
    ledc_timer_config(&ledc_timer);

    // 2. Map the configuration out to GPIO6
    ledc_channel_config_t ledc_channel = {
        .speed_mode     = BACKLIGHT_SPEED_MODE,
        .channel        = BACKLIGHT_CHANNEL,
        .timer_sel      = BACKLIGHT_TIMER,
        .intr_type      = LEDC_INTR_DISABLE,
        .gpio_num       = BACKLIGHT_PIN,
        .duty           = 1023, // Initialize at 100% full brightness upon device boot
        .hpoint         = 0
    };
    ledc_channel_config(&ledc_channel);
}

void set_backlight_brightness(uint8_t brightness_percent) 
{
    // Clamp the percentage strictly to screen safety margins
    if (brightness_percent > 100) brightness_percent = 100;

    // Map 0-100% across the 0-1023 10-bit duty cycle matrix space
    uint32_t duty = (uint32_t)((brightness_percent / 100.0f) * 1023.0f);

    // Set and update the hardware state
    ledc_set_duty(BACKLIGHT_SPEED_MODE, BACKLIGHT_CHANNEL, duty);
    ledc_update_duty(BACKLIGHT_SPEED_MODE, BACKLIGHT_CHANNEL);
}

void sd_card_test(void) 
{
        printf("Checking SD Card mounting...\n");
        FILE *f = fopen("/sdcard/0_100.bin", "rb");
        if (f == NULL) {
            printf("[SD TEST FAIL] ESP32 cannot open the file. Is the card mounted or folder nested correctly?\n");
        } else {
            printf("[SD TEST PASS] ESP32 can open the file!\n");
            fclose(f);
        }
}

void run_weather_ui_test(int test_case) 
{
    // Lock LVGL for thread safety before editing properties
    lvgl_lock();
    
    // Wipe the screen before drawing the test case
    lv_obj_clean(lv_scr_act());

    switch (test_case) {
        case 1: // SCENARIO 1: Crisp Midpoint Day (0°C, Clear Sky, Day)
            printf("[TEST] Running Case 1: 0C Day Midpoint\n");
            current_weather.temp = 0.0f;
            current_weather.feels_like = 0.0f;
            current_weather.weather_code = 0; // Clear sky
            current_weather.is_day = 1;       // Day
            
            // Populate 5-day forecast mock data (YYYY-MM-DD format)
            snprintf(forecast_weather.date[0], 16, "2026-07-20");
            snprintf(forecast_weather.date[1], 16, "2026-07-21");
            snprintf(forecast_weather.date[2], 16, "2026-07-22");
            snprintf(forecast_weather.date[3], 16, "2026-07-23");
            snprintf(forecast_weather.date[4], 16, "2026-07-24");
            for(int i=0; i<5; i++) {
                forecast_weather.temp_min[i] = -2.0f;
                forecast_weather.temp_max[i] = 4.0f;
                forecast_weather.weather_code[i] = 1; // Mainly clear
            }
            break;

        case 2: // SCENARIO 2: Bitter Cold (Extreme Sub-Zero -30°C)
            printf("[TEST] Running Case 2: Extreme Sub-Zero\n");
            current_weather.temp = -30.0f;
            current_weather.feels_like = -40.0f; // Windchill
            current_weather.weather_code = 45;   // Foggy
            current_weather.is_day = 0;          // Night (Should load 45_night_100.bin)
            
            snprintf(forecast_weather.date[0], 16, "2026-01-15");
            for(int i=0; i<5; i++) {
                forecast_weather.temp_min[i] = -38.0f;
                forecast_weather.temp_max[i] = -25.0f;
                forecast_weather.weather_code[i] = 45;
            }
            break;

        case 3: // SCENARIO 3: Scorching Summer Heat (+32°C, Feels like 38°C)
            printf("[TEST] Running Case 3: Intense Heat Wave\n");
            current_weather.temp = 32.0f;
            current_weather.feels_like = 38.0f; 
            current_weather.weather_code = 0;
            current_weather.is_day = 0;
            
            snprintf(forecast_weather.date[0], 16, "2026-07-17");
            for(int i=0; i<5; i++) {
                forecast_weather.temp_min[i] = 22.0f;
                forecast_weather.temp_max[i] = 36.0f;
                forecast_weather.weather_code[i] = 0;
            }
            break;

        case 4: // SCENARIO 4: Full Moon Night (Calculated via accurate historical date)
            printf("[TEST] Running Case 4: Clear Night on a Full Moon\n");
            current_weather.temp = 18.0f;
            current_weather.feels_like = 21.0f;
            current_weather.weather_code = 0; // Clear Sky
            current_weather.is_day = 0;       // Night Time!
            
            // July 29, 2026 is mathematically a verified Full Moon day
            snprintf(forecast_weather.date[0], 16, "2026-07-29"); 
            for(int i=0; i<5; i++) {
                forecast_weather.temp_min[i] = 12.0f;
                forecast_weather.temp_max[i] = 22.0f;
                forecast_weather.weather_code[i] = 0;
            }
            break;

        default:
            break;
    }

    // Force build the UI with our mock data values instantly
    build_weather_ui();
    
    // Always unlock LVGL when finished updating elements
    lvgl_unlock();
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
    // QMI8658_Init();
    EXIO_Init();                    // Example Initialize EXIO
    xTaskCreatePinnedToCore(
        Driver_Loop, 
        "Other Driver task",
        4096, 
        NULL, 
        3, 
        NULL, 
        0);
}

void app_main(void)
{   
    lvgl_mux = xSemaphoreCreateMutex();
    if (lvgl_mux == NULL) {
        printf("Failed to create LVGL Mutex Lock!\n");
        return;
    }
    // button_Init();
    Driver_Init();
    LCD_Init();
    SD_Init();
    LVGL_Init();
    register_custom_sd_driver();

    // sd_card_test();                // Optional sanity test

    //-----------------------WiFi captive portal setup-------------------//
    // wifi_prov_erase_credentials(); // remove the saved credentials (only need for debugging)

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

    //----------------Set brightness -------------------//
    set_backlight_brightness(100);
    
    //------------Weather retrieval task ---------------//
    xTaskCreatePinnedToCore(
            weather_fetch_task, 
            "Weather Fetch Task", 
            6144, // Generous stack for parsing JSON
            NULL, 
            5, 
            NULL, 
            1);

    //--------------------UI tests----------------------//
    // Change the integer argument from 1 to 4 to test each specific UI scenario:
    // run_weather_ui_test(2);
    //--------------------------------------------------//
        
    reset_button_init(); // wait for a long press for wifi credential reset

    while (1) {
        vTaskDelay(pdMS_TO_TICKS(10));
        lvgl_lock();   // <-- Grab the lock
        lv_timer_handler();
        lvgl_unlock(); // <-- Release the lock
    }
}
