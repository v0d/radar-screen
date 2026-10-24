#pragma once

#include <stdbool.h>
#include <stddef.h>
#include "esp_err.h"

typedef enum {
    WIFI_MGR_IDLE = 0,
    WIFI_MGR_CONNECTING,
    WIFI_MGR_CONNECTED,
    WIFI_MGR_PORTAL,
} wifi_mgr_state_t;

esp_err_t wifi_mgr_init(void);

/* Blocking: try to join the given network. Returns true when an IP has been obtained. */
bool wifi_mgr_connect(const char *ssid, const char *pass, int timeout_ms);

/* Start the setup access point (APSTA mode). The station side stays idle so scans work. */
esp_err_t wifi_mgr_start_ap(void);

bool wifi_mgr_is_connected(void);
wifi_mgr_state_t wifi_mgr_state(void);

/* Dotted IP of the station interface ("" when not connected). */
void wifi_mgr_get_ip(char *buf, size_t len);

/* Scan for networks (blocking, a few seconds). Returns a malloc'd JSON array string of unique SSIDs
 * sorted by signal strength, caller frees. NULL on failure. */
char *wifi_mgr_scan_json(void);
