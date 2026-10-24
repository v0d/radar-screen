#include "encoder.h"

#include "board.h"
#include "driver/gpio.h"
#include "esp_check.h"
#include "esp_log.h"
#include "freertos/queue.h"
#include "freertos/task.h"

static const char *TAG = "encoder";

#define STEPS_PER_DETENT CONFIG_RADAR_ENC_STEPS_PER_DETENT
#define LONG_PRESS_MS 700
#define POLL_MS 10

static QueueHandle_t s_queue;
static volatile uint8_t s_prev;
static volatile int s_acc;

/* Quadrature state machine: index = (prev << 2) | cur */
static const int8_t s_table[16] = {0, -1, 1, 0, 1, 0, 0, -1, -1, 0, 0, 1, 0, 1, -1, 0};

static void enc_isr(void *arg)
{
    (void)arg;
    uint8_t cur = (uint8_t)((gpio_get_level(BOARD_ENC_A) << 1) | gpio_get_level(BOARD_ENC_B));
    int delta = s_table[(s_prev << 2) | cur];
    s_prev = cur;
    if (!delta) {
        return;
    }
    int acc = s_acc + delta;
    input_evt_t evt;
    bool fire = false;
    if (acc >= STEPS_PER_DETENT) {
        acc = 0;
        evt = INPUT_CW;
        fire = true;
    } else if (acc <= -STEPS_PER_DETENT) {
        acc = 0;
        evt = INPUT_CCW;
        fire = true;
    }
    s_acc = acc;
    if (fire) {
#if CONFIG_RADAR_ENC_REVERSE
        evt = (evt == INPUT_CW) ? INPUT_CCW : INPUT_CW;
#endif
        BaseType_t woken = pdFALSE;
        xQueueSendFromISR(s_queue, &evt, &woken);
        if (woken) {
            portYIELD_FROM_ISR();
        }
    }
}

static void button_task(void *arg)
{
    (void)arg;
    bool down = false;
    bool long_sent = false;
    int held_ms = 0;
    int stable = 0;
    bool last_raw = false;

    for (;;) {
        vTaskDelay(pdMS_TO_TICKS(POLL_MS));
        bool raw = gpio_get_level(BOARD_ENC_SW) == 0;
        if (raw == last_raw) {
            if (stable < 1000) {
                stable++;
            }
        } else {
            stable = 0;
            last_raw = raw;
        }
        if (stable == 2 && raw != down) { /* 20 ms debounce */
            down = raw;
            held_ms = 0;
            if (!down) {
                if (!long_sent) {
                    input_evt_t evt = INPUT_PRESS;
                    xQueueSend(s_queue, &evt, 0);
                }
                long_sent = false;
            }
        }
        if (down && !long_sent) {
            held_ms += POLL_MS;
            if (held_ms >= LONG_PRESS_MS) {
                long_sent = true;
                input_evt_t evt = INPUT_LONG_PRESS;
                xQueueSend(s_queue, &evt, 0);
            }
        }
    }
}

esp_err_t encoder_init(void)
{
    s_queue = xQueueCreate(16, sizeof(input_evt_t));
    ESP_RETURN_ON_FALSE(s_queue, ESP_ERR_NO_MEM, TAG, "queue");

    const gpio_config_t ab = {
        .pin_bit_mask = (1ULL << BOARD_ENC_A) | (1ULL << BOARD_ENC_B),
        .mode = GPIO_MODE_INPUT,
        .pull_up_en = GPIO_PULLUP_ENABLE,
        .intr_type = GPIO_INTR_ANYEDGE,
    };
    ESP_RETURN_ON_ERROR(gpio_config(&ab), TAG, "gpio ab");

    const gpio_config_t sw = {
        .pin_bit_mask = 1ULL << BOARD_ENC_SW,
        .mode = GPIO_MODE_INPUT,
        .pull_up_en = GPIO_PULLUP_ENABLE,
    };
    ESP_RETURN_ON_ERROR(gpio_config(&sw), TAG, "gpio sw");

    s_prev = (uint8_t)((gpio_get_level(BOARD_ENC_A) << 1) | gpio_get_level(BOARD_ENC_B));

    esp_err_t err = gpio_install_isr_service(0);
    if (err != ESP_OK && err != ESP_ERR_INVALID_STATE) {
        return err;
    }
    ESP_RETURN_ON_ERROR(gpio_isr_handler_add(BOARD_ENC_A, enc_isr, NULL), TAG, "isr a");
    ESP_RETURN_ON_ERROR(gpio_isr_handler_add(BOARD_ENC_B, enc_isr, NULL), TAG, "isr b");

    BaseType_t ok = xTaskCreate(button_task, "btn", 2048, NULL, 5, NULL);
    ESP_RETURN_ON_FALSE(ok == pdPASS, ESP_ERR_NO_MEM, TAG, "task");
    return ESP_OK;
}

bool encoder_wait(input_evt_t *evt, TickType_t timeout)
{
    return xQueueReceive(s_queue, evt, timeout) == pdTRUE;
}

bool encoder_button_down(void)
{
    return gpio_get_level(BOARD_ENC_SW) == 0;
}
