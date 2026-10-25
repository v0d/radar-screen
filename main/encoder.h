#pragma once

#include <stdbool.h>
#include "esp_err.h"
#include "freertos/FreeRTOS.h"

typedef enum {
    INPUT_CW = 0,
    INPUT_CCW,
    INPUT_PRESS,       /* short press, reported on release */
    INPUT_LONG_PRESS,  /* reported as soon as the hold threshold is reached */
    INPUT_HOLD,        /* button still held after HOLD_MS (10 s), reported once */
} input_evt_t;

esp_err_t encoder_init(void);

/* Wait for the next input event. Returns false on timeout. */
bool encoder_wait(input_evt_t *evt, TickType_t timeout);

/* Raw (undebounced) state of the push button. */
bool encoder_button_down(void);
