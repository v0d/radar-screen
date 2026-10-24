#include "config_store.h"

#include <stddef.h>
#include <string.h>
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "nvs.h"

static const char *TAG = "cfg";

#define CFG_NS "radar"
#define CFG_KEY "cfg"
#define CFG_VERSION 8
#define CFG_V3_SIZE 224
#define CFG_V4_SIZE 352
#define CFG_V5_SIZE 424

/* v1-3 blobs are a prefix of the current layout: their last field (sweep) is followed only by padding. */
_Static_assert(offsetof(radar_config_t, trails) == CFG_V3_SIZE - 1, "v3 prefix layout changed");
/* likewise for v4, whose last field (ref) ends 4 bytes before its size */
_Static_assert(offsetof(radar_config_t, api_key) == CFG_V4_SIZE - 4, "v4 prefix layout changed");
_Static_assert(offsetof(radar_config_t, rotation) == CFG_V5_SIZE, "v5 prefix layout changed");
/* v6 has the current size: label_fields took a padding byte, so it is reset on migration */
_Static_assert(offsetof(radar_config_t, label_fields) == CFG_V5_SIZE + 1, "v6 prefix layout changed");
_Static_assert(offsetof(radar_config_t, show_count) == CFG_V5_SIZE + 2, "v7 prefix layout changed");
_Static_assert(sizeof(radar_config_t) == 432, "radar_config_t size changed: add a migration");

static radar_config_t s_cfg;
static SemaphoreHandle_t s_lock;
static volatile uint32_t s_gen;

static void set_defaults(radar_config_t *c)
{
    memset(c, 0, sizeof(*c));
    c->version = CFG_VERSION;
    /* Amsterdam Schiphol */
    c->lat = 52.3105;
    c->lon = 4.7683;
    c->source = SRC_ADSBLOL;
    c->units = UNITS_AVIATION;
    c->default_zoom = 2;
    c->lookup_routes = 1;
    c->refresh_s = 5;
    c->sweep = 0;
    strcpy(c->base_url, "http://tar1090.local/tar1090");
    c->trails = 1;
    c->leader_s = 60;
    c->brightness = 100;
    c->dim_level = 20;
    c->night_start_h = 22;
    c->night_end_h = 7;
    strcpy(c->tz, "CET-1CEST,M3.5.0,M10.5.0/3");
    c->alert_dist_nm10 = 30;
    c->alert_alt_ft = 10000;
    strlcpy(c->api_key, CONFIG_RADAR_ADSBLOL_API_KEY, sizeof(c->api_key));
    c->label_fields = LABEL_CALLSIGN;
    c->show_count = 0;
}

void cfg_init(void)
{
    s_lock = xSemaphoreCreateMutex();
    set_defaults(&s_cfg);

    nvs_handle_t h;
    if (nvs_open(CFG_NS, NVS_READONLY, &h) != ESP_OK) {
        ESP_LOGI(TAG, "no stored config, using defaults");
        return;
    }
    radar_config_t tmp;
    size_t len = sizeof(tmp);
    esp_err_t err = nvs_get_blob(h, CFG_KEY, &tmp, &len);
    nvs_close(h);
    bool v3 = err == ESP_OK && len == CFG_V3_SIZE && tmp.version >= 1 && tmp.version <= 3;
    bool v4 = err == ESP_OK && len == CFG_V4_SIZE && tmp.version == 4;
    bool v5 = err == ESP_OK && len == CFG_V5_SIZE && tmp.version == 5;
    bool v67 = err == ESP_OK && len == sizeof(tmp) && (tmp.version == 6 || tmp.version == 7);
    if (err == ESP_OK && (v3 || v4 || v5 || v67 || (len == sizeof(tmp) && tmp.version == CFG_VERSION))) {
        if (tmp.version < 2) {
            tmp.sweep = 0; /* v1 had no sweep flag; its padding byte is undefined */
        }
        if (tmp.version < CFG_VERSION) { /* fields the stored version lacks get their defaults */
            radar_config_t d;
            set_defaults(&d);
            const size_t o = tmp.version < 4   ? offsetof(radar_config_t, trails)
                             : tmp.version < 5 ? offsetof(radar_config_t, api_key)
                             : tmp.version < 6 ? offsetof(radar_config_t, rotation)
                             : tmp.version < 7 ? offsetof(radar_config_t, label_fields)
                                               : offsetof(radar_config_t, show_count);
            memcpy((uint8_t *)&tmp + o, (const uint8_t *)&d + o, sizeof(tmp) - o);
        }
        tmp.version = CFG_VERSION;
        tmp.sweep = tmp.sweep ? 1 : 0;
        /* make sure strings are terminated */
        tmp.wifi_ssid[sizeof(tmp.wifi_ssid) - 1] = 0;
        tmp.wifi_pass[sizeof(tmp.wifi_pass) - 1] = 0;
        tmp.base_url[sizeof(tmp.base_url) - 1] = 0;
        tmp.tz[sizeof(tmp.tz) - 1] = 0;
        tmp.api_key[sizeof(tmp.api_key) - 1] = 0;
        tmp.rotation &= 3;
        tmp.label_fields &= LABEL_CALLSIGN | LABEL_TYPE | LABEL_ALT;
        tmp.show_count = tmp.show_count ? 1 : 0;
        for (int i = 0; i < CFG_REF_POINTS; i++) {
            tmp.ref[i].name[sizeof(tmp.ref[i].name) - 1] = 0;
        }
        s_cfg = tmp;
        ESP_LOGI(TAG, "config loaded (%.4f, %.4f)", s_cfg.lat, s_cfg.lon);
    } else {
        ESP_LOGW(TAG, "stored config invalid (%s), using defaults", esp_err_to_name(err));
    }
}

void cfg_get(radar_config_t *out)
{
    xSemaphoreTake(s_lock, portMAX_DELAY);
    *out = s_cfg;
    xSemaphoreGive(s_lock);
}

esp_err_t cfg_set(const radar_config_t *in)
{
    nvs_handle_t h;
    esp_err_t err = nvs_open(CFG_NS, NVS_READWRITE, &h);
    if (err != ESP_OK) {
        return err;
    }
    radar_config_t tmp = *in;
    tmp.version = CFG_VERSION;
    err = nvs_set_blob(h, CFG_KEY, &tmp, sizeof(tmp));
    if (err == ESP_OK) {
        err = nvs_commit(h);
    }
    nvs_close(h);
    if (err != ESP_OK) {
        return err;
    }
    xSemaphoreTake(s_lock, portMAX_DELAY);
    s_cfg = tmp;
    s_gen++;
    xSemaphoreGive(s_lock);
    return ESP_OK;
}

uint32_t cfg_generation(void)
{
    return s_gen;
}

bool cfg_has_wifi(void)
{
    bool r;
    xSemaphoreTake(s_lock, portMAX_DELAY);
    r = s_cfg.wifi_ssid[0] != 0;
    xSemaphoreGive(s_lock);
    return r;
}
