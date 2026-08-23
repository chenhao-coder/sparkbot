#include "ui.h"
#include "bsp_display.h"
#include "esp_log.h"
#include "lvgl.h"

static const char *TAG = "UI";

esp_err_t app_ui_start(void)
{
    /* 初始化显示和 LVGL (对应 bsp_display_start_with_config) */
    bsp_display_cfg_t custom_cfg = {
        .lvgl_port_cfg = ESP_LVGL_PORT_INIT_CONFIG(),
        .buffer_size   = BSP_LCD_H_RES * BSP_LCD_V_RES,
        .trans_size    = BSP_LCD_H_RES * 10,  // in SRAM, DMA-capable
        .double_buffer = 0,
        .flags = {
            .buff_dma    = false,
            .buff_spiram = true,
        }};
    custom_cfg.lvgl_port_cfg.task_stack    = 8 * 1024;  // 全屏图片渲染需要足够栈, 4KB 会栈溢出导致复位
    custom_cfg.lvgl_port_cfg.task_affinity = 1;
    bsp_display_start_with_config(&custom_cfg);

    /* 打开背光 */
    bsp_display_backlight_on();

    ESP_LOGI(TAG, "Display initialized");
    bsp_display_lock(0);

    /* 全屏黑色背景 */
    lv_obj_t *obj_bg = lv_obj_create(lv_screen_active());
    lv_obj_set_size(obj_bg, BSP_LCD_H_RES, BSP_LCD_V_RES);
    lv_obj_set_align(obj_bg, LV_ALIGN_CENTER);
    lv_obj_clear_flag(obj_bg, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_set_style_bg_color(obj_bg, lv_color_hex(0x000000), 0);
    lv_obj_set_style_border_width(obj_bg, 0, 0);

    /* 显示 OpenAI_logo 图片 (对应 BSP 例子的 ui_emoji) */
    extern const lv_img_dsc_t OpenAI_logo;
    lv_obj_t *ui_emoji = lv_image_create(obj_bg);
    lv_obj_set_size(ui_emoji, 240, 240);
    lv_obj_center(ui_emoji);
    lv_image_set_src(ui_emoji, &OpenAI_logo);

    bsp_display_unlock();

    return ESP_OK;
}
