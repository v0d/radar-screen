#include "board.h"

#include "driver/gpio.h"
#include "driver/ledc.h"
#include "driver/spi_master.h"
#include "esp_check.h"
#include "esp_lcd_gc9a01.h"
#include "esp_lcd_panel_io.h"
#include "esp_lcd_panel_ops.h"
#include "esp_log.h"
#include "esp_lvgl_port.h"

static const char *TAG = "board";

#define LCD_SPI_HOST SPI2_HOST
#define LCD_PIXEL_CLOCK_HZ (40 * 1000 * 1000)
#define LCD_DRAW_BUF_LINES 30

#define BL_LEDC_TIMER LEDC_TIMER_0
#define BL_LEDC_CHANNEL LEDC_CHANNEL_0
#define BL_LEDC_BITS 10
#define BL_LEDC_FREQ_HZ 20000 /* above the audible range */

void board_backlight_set_pct(int pct)
{
    if (pct < 0) {
        pct = 0;
    } else if (pct > 100) {
        pct = 100;
    }
    /* squared so that the percentage roughly follows perceived brightness */
    const uint32_t max = (1u << BL_LEDC_BITS) - 1;
    uint32_t duty = max * (uint32_t)(pct * pct) / 10000;
    if (pct > 0 && duty == 0) {
        duty = 1;
    }
    ledc_set_duty(LEDC_LOW_SPEED_MODE, BL_LEDC_CHANNEL, duty);
    ledc_update_duty(LEDC_LOW_SPEED_MODE, BL_LEDC_CHANNEL);
}

void board_backlight_set(bool on)
{
    board_backlight_set_pct(on ? 100 : 0);
}

esp_err_t board_display_init(lv_disp_t **out_disp)
{
    /* Backlight (active high), dimmable via LEDC PWM; starts off */
    const ledc_timer_config_t bl_timer = {
        .speed_mode = LEDC_LOW_SPEED_MODE,
        .duty_resolution = BL_LEDC_BITS,
        .timer_num = BL_LEDC_TIMER,
        .freq_hz = BL_LEDC_FREQ_HZ,
        .clk_cfg = LEDC_AUTO_CLK,
    };
    ESP_RETURN_ON_ERROR(ledc_timer_config(&bl_timer), TAG, "backlight timer");
    const ledc_channel_config_t bl_chan = {
        .gpio_num = BOARD_LCD_BL,
        .speed_mode = LEDC_LOW_SPEED_MODE,
        .channel = BL_LEDC_CHANNEL,
        .timer_sel = BL_LEDC_TIMER,
        .duty = 0,
    };
    ESP_RETURN_ON_ERROR(ledc_channel_config(&bl_chan), TAG, "backlight channel");

    const spi_bus_config_t buscfg = {
        .sclk_io_num = BOARD_LCD_SCLK,
        .mosi_io_num = BOARD_LCD_MOSI,
        .miso_io_num = -1,
        .quadwp_io_num = -1,
        .quadhd_io_num = -1,
        .max_transfer_sz = BOARD_LCD_W * LCD_DRAW_BUF_LINES * sizeof(uint16_t),
    };
    ESP_RETURN_ON_ERROR(spi_bus_initialize(LCD_SPI_HOST, &buscfg, SPI_DMA_CH_AUTO), TAG, "spi bus");

    esp_lcd_panel_io_handle_t io = NULL;
    const esp_lcd_panel_io_spi_config_t io_cfg = {
        .dc_gpio_num = BOARD_LCD_DC,
        .cs_gpio_num = BOARD_LCD_CS,
        .pclk_hz = LCD_PIXEL_CLOCK_HZ,
        .lcd_cmd_bits = 8,
        .lcd_param_bits = 8,
        .spi_mode = 0,
        .trans_queue_depth = 10,
    };
    ESP_RETURN_ON_ERROR(esp_lcd_new_panel_io_spi((esp_lcd_spi_bus_handle_t)LCD_SPI_HOST, &io_cfg, &io), TAG,
                        "panel io");

    esp_lcd_panel_handle_t panel = NULL;
    const esp_lcd_panel_dev_config_t panel_cfg = {
        .reset_gpio_num = -1, /* RST is not wired on the LCDkit */
        .rgb_ele_order = LCD_RGB_ELEMENT_ORDER_BGR,
        .bits_per_pixel = 16,
    };
    ESP_RETURN_ON_ERROR(esp_lcd_new_panel_gc9a01(io, &panel_cfg, &panel), TAG, "panel");

    const bool mirror_x = (bool)(
#if CONFIG_RADAR_LCD_MIRROR_X
        1
#else
        0
#endif
    );
    const bool mirror_y = (bool)(
#if CONFIG_RADAR_LCD_MIRROR_Y
        1
#else
        0
#endif
    );
    esp_lcd_panel_reset(panel);
    esp_lcd_panel_init(panel);
    esp_lcd_panel_invert_color(panel, true);
    esp_lcd_panel_swap_xy(panel, false);
    esp_lcd_panel_mirror(panel, mirror_x, mirror_y);
    esp_lcd_panel_disp_on_off(panel, true);

    lvgl_port_cfg_t lvgl_cfg = ESP_LVGL_PORT_INIT_CONFIG();
    lvgl_cfg.task_stack = 6144;
    ESP_RETURN_ON_ERROR(lvgl_port_init(&lvgl_cfg), TAG, "lvgl port");

    const lvgl_port_display_cfg_t disp_cfg = {
        .io_handle = io,
        .panel_handle = panel,
        .buffer_size = BOARD_LCD_W * LCD_DRAW_BUF_LINES,
        .double_buffer = true,
        .hres = BOARD_LCD_W,
        .vres = BOARD_LCD_H,
        .monochrome = false,
        .rotation = {
            .swap_xy = false,
            .mirror_x = mirror_x,
            .mirror_y = mirror_y,
        },
        .flags = {
            .buff_dma = true,
        },
    };
    lv_disp_t *disp = lvgl_port_add_disp(&disp_cfg);
    ESP_RETURN_ON_FALSE(disp, ESP_FAIL, TAG, "lvgl_port_add_disp failed");

    if (out_disp) {
        *out_disp = disp;
    }
    return ESP_OK;
}
