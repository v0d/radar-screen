#pragma once

#include <stdbool.h>
#include "esp_err.h"

typedef enum {
    LED_OFF = 0,
    LED_OK,          /* green: all set, data is current */
    LED_SOFT,        /* yellow: stale data, rate limited, occasional fetch errors */
    LED_HARD,        /* red: needs attention (setup mode, rejected requests, source unreachable) */
    LED_CONNECTING,  /* pulsing red: joining WiFi / waiting for the first data */
} led_status_t;

/* Drives the WS2812B RGB LED of the LCDkit from a small background task. */
esp_err_t status_led_init(void);

/* Thread-safe; cheap when nothing changes. */
void status_led_set(led_status_t st);

/* Follow the display backlight: dimmed = errors only, at low brightness (no green at night). */
void status_led_set_dimmed(bool dimmed);
