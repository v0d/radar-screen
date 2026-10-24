#include "status_led.h"

#include "board.h"
#include "esp_check.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "led_strip.h"

static const char *TAG = "led";

#define LED_MAX 10           /* of 255: the bare WS2812B is very bright */
#define PULSE_PERIOD_MS 1200
#define PULSE_STEP_MS 30
#define DIM_DIV 4

static led_strip_handle_t s_strip;
static TaskHandle_t s_task;
static volatile led_status_t s_status = LED_OFF;
static volatile bool s_dimmed;

static void show(uint32_t r, uint32_t g, uint32_t b)
{
    if (s_dimmed) {
        r /= DIM_DIV;
        g /= DIM_DIV;
        b /= DIM_DIV;
    }
    led_strip_set_pixel(s_strip, 0, r, g, b);
    led_strip_refresh(s_strip);
}

static void led_task(void *arg)
{
    (void)arg;
    for (;;) {
        led_status_t st = s_status;
        if (st == LED_CONNECTING) {
            /* triangle wave between a faint glow and full red */
            uint32_t ph = (uint32_t)(esp_timer_get_time() / 1000) % PULSE_PERIOD_MS;
            uint32_t tri = ph < PULSE_PERIOD_MS / 2 ? ph : PULSE_PERIOD_MS - ph;
            show(2 + (LED_MAX - 2) * tri / (PULSE_PERIOD_MS / 2), 0, 0);
            ulTaskNotifyTake(pdTRUE, pdMS_TO_TICKS(PULSE_STEP_MS));
            continue;
        }
        switch (st) {
        case LED_OK:
            if (s_dimmed) {
                show(0, 0, 0); /* nothing to report: stay dark while the display is dimmed */
            } else {
                show(0, LED_MAX, 0);
            }
            break;
        case LED_SOFT:
            show(LED_MAX, LED_MAX * 2 / 3, 0);
            break;
        case LED_HARD:
            show(LED_MAX, 0, 0);
            break;
        default:
            show(0, 0, 0);
            break;
        }
        ulTaskNotifyTake(pdTRUE, portMAX_DELAY); /* static colour: sleep until something changes */
    }
}

esp_err_t status_led_init(void)
{
    const led_strip_config_t strip_cfg = {
        .strip_gpio_num = BOARD_RGB_LED,
        .max_leds = 1,
        .led_pixel_format = LED_PIXEL_FORMAT_GRB,
        .led_model = LED_MODEL_WS2812,
    };
    const led_strip_rmt_config_t rmt_cfg = {
        .resolution_hz = 10 * 1000 * 1000,
    };
    ESP_RETURN_ON_ERROR(led_strip_new_rmt_device(&strip_cfg, &rmt_cfg, &s_strip), TAG, "led strip");
    led_strip_clear(s_strip);
    BaseType_t ok = xTaskCreate(led_task, "led", 2048, NULL, 2, &s_task);
    ESP_RETURN_ON_FALSE(ok == pdPASS, ESP_ERR_NO_MEM, TAG, "task");
    return ESP_OK;
}

void status_led_set(led_status_t st)
{
    if (st != s_status) {
        s_status = st;
        if (s_task) {
            xTaskNotifyGive(s_task);
        }
    }
}

void status_led_set_dimmed(bool dimmed)
{
    if (dimmed != s_dimmed) {
        s_dimmed = dimmed;
        if (s_task) {
            xTaskNotifyGive(s_task);
        }
    }
}
