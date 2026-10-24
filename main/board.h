#pragma once

#include <stdbool.h>
#include "esp_err.h"
#include "lvgl.h"

/* ESP32-C3-LCDkit v1.1 pin map */
#define BOARD_LCD_SCLK 1
#define BOARD_LCD_MOSI 0
#define BOARD_LCD_DC 2
#define BOARD_LCD_CS 7
#define BOARD_LCD_BL 5
#define BOARD_ENC_A 10
#define BOARD_ENC_B 6
#define BOARD_ENC_SW 9
#define BOARD_RGB_LED 8 /* WS2812B */

#define BOARD_LCD_W 240
#define BOARD_LCD_H 240

/* Initialises SPI, the GC9A01 panel, backlight and LVGL (via esp_lvgl_port). */
esp_err_t board_display_init(lv_disp_t **out_disp);

void board_backlight_set(bool on);

/* Backlight brightness 0..100 % (PWM). */
void board_backlight_set_pct(int pct);
