#pragma once

#include "encoder.h"
#include "lvgl.h"

/* Builds the radar screen on the given display. */
void radar_ui_init(lv_disp_t *disp);

/* Feed an encoder / button event (thread-safe). Returns false for a long press in radar mode, which
 * the UI does not use (the caller may use it, e.g. to show the settings address). */
bool radar_ui_input(input_evt_t evt);

/* True while an aircraft is inside the proximity alert zone (safe to call from any task). */
bool radar_ui_alert_active(void);

/* Full-screen text message (boot / setup instructions). NULL hides it again. */
void radar_ui_show_message(const char *msg);
