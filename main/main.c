#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#include "adsb.h"
#include "board.h"
#include "config_store.h"
#include "encoder.h"
#include "esp_log.h"
#include "esp_netif_sntp.h"
#include "esp_ota_ops.h"
#include "esp_system.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "nvs_flash.h"
#include "portal.h"
#include "radar_ui.h"
#include "status_led.h"
#include "wifi_mgr.h"

static const char *TAG = "main";

#define WIFI_CONNECT_TIMEOUT_MS 20000
#define SETUP_RETRY_AFTER_S 180   /* leave setup mode after this long if WiFi credentials exist */
#define SETTINGS_MSG_TIMEOUT_US (10LL * 1000000)
#define NIGHT_WAKE_US (60LL * 1000000)  /* at night, stay bright this long after the last input */
#define LED_HARD_FAILS 3                /* failed fetches in a row (with stale data) that count as a hard error */

static void apply_tz(const radar_config_t *c)
{
    setenv("TZ", c->tz[0] ? c->tz : "UTC0", 1);
    tzset();
}

static bool night_now(const radar_config_t *c)
{
    if (!c->night || c->night_start_h == c->night_end_h) {
        return false;
    }
    time_t t = time(NULL);
    struct tm tm;
    localtime_r(&t, &tm);
    if (tm.tm_year < 2024 - 1900) {
        return false; /* clock not set by SNTP yet */
    }
    int h = tm.tm_hour;
    if (c->night_start_h < c->night_end_h) {
        return h >= c->night_start_h && h < c->night_end_h;
    }
    return h >= c->night_start_h || h < c->night_end_h;
}

/* Dim after dim_after_min without input, or after NIGHT_WAKE_US during the night hours; an active
 * proximity alert always wakes the display. */
static bool backlight_dimmed(const radar_config_t *c, int64_t idle_us)
{
    if (radar_ui_alert_active()) {
        return false;
    }
    if (c->dim_after_min && idle_us >= c->dim_after_min * 60LL * 1000000) {
        return true;
    }
    return idle_us >= NIGHT_WAKE_US && night_now(c);
}

/* Status LED colour from the WiFi and data state. */
static led_status_t led_status(bool setup_mode, const radar_config_t *c)
{
    if (setup_mode) {
        return LED_HARD; /* no usable WiFi: the setup access point needs the user */
    }
    if (!wifi_mgr_is_connected()) {
        return LED_CONNECTING; /* rejoining */
    }
    adsb_health_t h;
    adsb_get_health(&h);
    /* a 4xx other than 429 (bad API key, wrong tar1090 URL ...) will not fix itself */
    if (h.state == ADSB_ST_HTTP_ERR && h.http_status >= 400 && h.http_status < 500 && h.http_status != 429) {
        return LED_HARD;
    }
    if (h.state == ADSB_ST_RATE_LIMITED) {
        return LED_SOFT;
    }
    bool stale = !h.t_us || esp_timer_get_time() - h.t_us > (int64_t)(c->refresh_s * 3 + 5) * 1000000;
    if (stale && h.fails >= LED_HARD_FAILS) {
        return LED_HARD;
    }
    if (!h.t_us) {
        return LED_CONNECTING; /* waiting for the first data */
    }
    return (stale || h.state != ADSB_ST_OK) ? LED_SOFT : LED_OK;
}

void app_main(void)
{
    esp_err_t err = nvs_flash_init();
    if (err == ESP_ERR_NVS_NO_FREE_PAGES || err == ESP_ERR_NVS_NEW_VERSION_FOUND) {
        ESP_ERROR_CHECK(nvs_flash_erase());
        err = nvs_flash_init();
    }
    ESP_ERROR_CHECK(err);
    cfg_init();
    radar_config_t cfg;
    cfg_get(&cfg);

    lv_disp_t *disp = NULL;
    ESP_ERROR_CHECK(board_display_init(&disp));
    radar_ui_init(disp);
    radar_ui_show_message("ADS-B radar\nstarting...");
    board_backlight_set_pct(cfg.brightness);

    if (status_led_init() == ESP_OK) { /* the radar works without it */
        status_led_set(LED_CONNECTING);
    }
    ESP_ERROR_CHECK(encoder_init());
    ESP_ERROR_CHECK(wifi_mgr_init());

    bool connected = false;
    if (cfg.wifi_ssid[0]) {
        char msg[96];
        snprintf(msg, sizeof(msg), "Connecting to\n%.32s", cfg.wifi_ssid);
        radar_ui_show_message(msg);
        connected = wifi_mgr_connect(cfg.wifi_ssid, cfg.wifi_pass, WIFI_CONNECT_TIMEOUT_MS);
    }

    bool setup_mode = !connected;
    if (connected) {
        ESP_LOGI(TAG, "WiFi connected");
        esp_sntp_config_t sntp = ESP_NETIF_SNTP_DEFAULT_CONFIG("pool.ntp.org");
        esp_netif_sntp_init(&sntp);
        portal_start(false);
        adsb_init();
        radar_ui_show_message(NULL);
    } else {
        ESP_LOGW(TAG, "no WiFi, starting setup access point");
        ESP_ERROR_CHECK(wifi_mgr_start_ap());
        portal_start(true);
        char msg[192];
        snprintf(msg, sizeof(msg), "Setup\njoin WiFi\n%s\npassword\n%s\nthen open\nhttp://192.168.4.1", CONFIG_RADAR_AP_SSID,
                 CONFIG_RADAR_AP_PASSWORD);
        radar_ui_show_message(msg);
    }

    /* Reaching this point proves the image boots: cancel the rollback of a fresh OTA update. */
    esp_ota_mark_app_valid_cancel_rollback();

    bool settings_msg = false;
    int64_t settings_msg_t = 0;
    int64_t last_input_t = esp_timer_get_time();
    uint32_t cfg_gen = cfg_generation();
    apply_tz(&cfg);
    int backlight = -1;

    /* app_main runs at priority 1, below the LVGL and fetch tasks; the knob must not wait for them */
    vTaskPrioritySet(NULL, 5);

    for (;;) {
        input_evt_t evt;
        bool got = encoder_wait(&evt, pdMS_TO_TICKS(500));
        int64_t now = esp_timer_get_time();
        if (cfg_generation() != cfg_gen) {
            cfg_gen = cfg_generation();
            cfg_get(&cfg);
            apply_tz(&cfg);
            backlight = -1;
        }
        bool was_dimmed = backlight >= 0 && backlight < cfg.brightness;
        if (got) {
            last_input_t = now;
        }
        int level = (!setup_mode && backlight_dimmed(&cfg, now - last_input_t)) ? cfg.dim_level : cfg.brightness;
        if (level != backlight) {
            backlight = level;
            board_backlight_set_pct(level);
        }
        status_led_set_dimmed(level < cfg.brightness);
        status_led_set(led_status(setup_mode, &cfg));

        if (got) {
            if (setup_mode || was_dimmed) { /* the first input on a dimmed display only wakes it */
                continue;
            }
            if (settings_msg) { /* any input dismisses the settings address screen */
                settings_msg = false;
                radar_ui_show_message(NULL);
                continue;
            }
            if (!radar_ui_input(evt) && evt == INPUT_LONG_PRESS) {
                char ip[16];
                char msg[128];
                wifi_mgr_get_ip(ip, sizeof(ip));
                snprintf(msg, sizeof(msg), "Settings\nhttp://%s.local\n%s%s", CONFIG_RADAR_HOSTNAME,
                         ip[0] ? "http://" : "", ip);
                radar_ui_show_message(msg);
                settings_msg = true;
                settings_msg_t = esp_timer_get_time();
            }
        }
        if (settings_msg && now - settings_msg_t > SETTINGS_MSG_TIMEOUT_US) {
            settings_msg = false;
            radar_ui_show_message(NULL);
        }
        if (setup_mode && cfg_has_wifi() && now > SETUP_RETRY_AFTER_S * 1000000LL) {
            ESP_LOGI(TAG, "setup timeout, retrying stored WiFi");
            esp_restart();
        }
    }
}
