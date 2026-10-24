#include "wifi_mgr.h"

#include <stdlib.h>
#include <string.h>

#include "cJSON.h"
#include "esp_check.h"
#include "esp_event.h"
#include "esp_log.h"
#include "esp_netif.h"
#include "esp_wifi.h"
#include "freertos/FreeRTOS.h"
#include "freertos/event_groups.h"
#include "freertos/semphr.h"

static const char *TAG = "wifi";

#define BIT_CONNECTED BIT0
#define MAX_SCAN_RESULTS 20

static EventGroupHandle_t s_ev;
static esp_netif_t *s_sta_netif;
static esp_netif_t *s_ap_netif;
static volatile wifi_mgr_state_t s_state = WIFI_MGR_IDLE;
static volatile bool s_auto_reconnect;
static SemaphoreHandle_t s_scan_lock;

static void on_wifi_event(void *arg, esp_event_base_t base, int32_t id, void *data)
{
    (void)arg;
    (void)data;
    if (base == WIFI_EVENT && id == WIFI_EVENT_STA_DISCONNECTED) {
        xEventGroupClearBits(s_ev, BIT_CONNECTED);
        if (s_auto_reconnect) {
            if (s_state == WIFI_MGR_CONNECTED) {
                s_state = WIFI_MGR_CONNECTING;
            }
            esp_wifi_connect();
        }
    } else if (base == IP_EVENT && id == IP_EVENT_STA_GOT_IP) {
        if (s_state != WIFI_MGR_PORTAL) {
            s_state = WIFI_MGR_CONNECTED;
        }
        xEventGroupSetBits(s_ev, BIT_CONNECTED);
    }
}

esp_err_t wifi_mgr_init(void)
{
    s_ev = xEventGroupCreate();
    s_scan_lock = xSemaphoreCreateMutex();

    ESP_RETURN_ON_ERROR(esp_netif_init(), TAG, "netif");
    ESP_RETURN_ON_ERROR(esp_event_loop_create_default(), TAG, "event loop");
    s_sta_netif = esp_netif_create_default_wifi_sta();
    s_ap_netif = esp_netif_create_default_wifi_ap();
    esp_netif_set_hostname(s_sta_netif, CONFIG_RADAR_HOSTNAME);

    wifi_init_config_t cfg = WIFI_INIT_CONFIG_DEFAULT();
    ESP_RETURN_ON_ERROR(esp_wifi_init(&cfg), TAG, "wifi init");
    ESP_RETURN_ON_ERROR(esp_wifi_set_storage(WIFI_STORAGE_RAM), TAG, "storage");
    ESP_RETURN_ON_ERROR(esp_event_handler_register(WIFI_EVENT, ESP_EVENT_ANY_ID, on_wifi_event, NULL), TAG, "reg");
    ESP_RETURN_ON_ERROR(esp_event_handler_register(IP_EVENT, IP_EVENT_STA_GOT_IP, on_wifi_event, NULL), TAG, "reg");
    ESP_RETURN_ON_ERROR(esp_wifi_set_mode(WIFI_MODE_STA), TAG, "mode");
    ESP_RETURN_ON_ERROR(esp_wifi_start(), TAG, "start");
    return ESP_OK;
}

bool wifi_mgr_connect(const char *ssid, const char *pass, int timeout_ms)
{
    wifi_config_t sta = {0};
    strlcpy((char *)sta.sta.ssid, ssid, sizeof(sta.sta.ssid));
    strlcpy((char *)sta.sta.password, pass ? pass : "", sizeof(sta.sta.password));
    sta.sta.threshold.authmode = (pass && pass[0]) ? WIFI_AUTH_WPA_PSK : WIFI_AUTH_OPEN;
    sta.sta.pmf_cfg.capable = true;
    sta.sta.pmf_cfg.required = false;

    s_state = WIFI_MGR_CONNECTING;
    s_auto_reconnect = true;
    xEventGroupClearBits(s_ev, BIT_CONNECTED);

    if (esp_wifi_set_config(WIFI_IF_STA, &sta) != ESP_OK || esp_wifi_connect() != ESP_OK) {
        s_auto_reconnect = false;
        s_state = WIFI_MGR_IDLE;
        return false;
    }

    EventBits_t bits = xEventGroupWaitBits(s_ev, BIT_CONNECTED, pdFALSE, pdFALSE, pdMS_TO_TICKS(timeout_ms));
    if (bits & BIT_CONNECTED) {
        return true;
    }
    s_auto_reconnect = false;
    esp_wifi_disconnect();
    s_state = WIFI_MGR_IDLE;
    return false;
}

esp_err_t wifi_mgr_start_ap(void)
{
    wifi_config_t ap = {0};
    strlcpy((char *)ap.ap.ssid, CONFIG_RADAR_AP_SSID, sizeof(ap.ap.ssid));
    strlcpy((char *)ap.ap.password, CONFIG_RADAR_AP_PASSWORD, sizeof(ap.ap.password));
    ap.ap.ssid_len = strlen(CONFIG_RADAR_AP_SSID);
    ap.ap.channel = 1;
    ap.ap.max_connection = 4;
    ap.ap.authmode = strlen(CONFIG_RADAR_AP_PASSWORD) >= 8 ? WIFI_AUTH_WPA2_PSK : WIFI_AUTH_OPEN;

    s_auto_reconnect = false;
    s_state = WIFI_MGR_PORTAL;
    ESP_RETURN_ON_ERROR(esp_wifi_set_mode(WIFI_MODE_APSTA), TAG, "mode");
    ESP_RETURN_ON_ERROR(esp_wifi_set_config(WIFI_IF_AP, &ap), TAG, "ap cfg");
    return ESP_OK;
}

bool wifi_mgr_is_connected(void)
{
    return (xEventGroupGetBits(s_ev) & BIT_CONNECTED) != 0;
}

wifi_mgr_state_t wifi_mgr_state(void)
{
    return s_state;
}

void wifi_mgr_get_ip(char *buf, size_t len)
{
    buf[0] = 0;
    esp_netif_ip_info_t ip;
    if (s_sta_netif && esp_netif_get_ip_info(s_sta_netif, &ip) == ESP_OK && ip.ip.addr) {
        snprintf(buf, len, IPSTR, IP2STR(&ip.ip));
    }
}

char *wifi_mgr_scan_json(void)
{
    char *out = NULL;
    xSemaphoreTake(s_scan_lock, portMAX_DELAY);

    if (esp_wifi_scan_start(NULL, true) == ESP_OK) {
        uint16_t n = MAX_SCAN_RESULTS;
        wifi_ap_record_t *recs = calloc(MAX_SCAN_RESULTS, sizeof(wifi_ap_record_t));
        if (recs && esp_wifi_scan_get_ap_records(&n, recs) == ESP_OK) {
            cJSON *arr = cJSON_CreateArray();
            for (int i = 0; arr && i < n; i++) {  /* records come sorted by RSSI */
                const char *ssid = (const char *)recs[i].ssid;
                if (!ssid[0]) {
                    continue;
                }
                bool dup = false;
                for (int j = 0; j < i && !dup; j++) {
                    dup = strcmp(ssid, (const char *)recs[j].ssid) == 0;
                }
                if (!dup) {
                    cJSON_AddItemToArray(arr, cJSON_CreateString(ssid));
                }
            }
            if (arr) {
                out = cJSON_PrintUnformatted(arr);
                cJSON_Delete(arr);
            }
        }
        free(recs);
    }
    xSemaphoreGive(s_scan_lock);
    return out;
}
