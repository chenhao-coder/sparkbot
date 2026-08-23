#pragma once

#include "esp_lcd_panel_io.h"
#include "esp_lcd_panel_ops.h"

#ifdef __cplusplus
extern "C" {
#endif

#define DISP_H_RES  240
#define DISP_V_RES  240

/**
 * @brief 初始化 ST7789 显示 (基于官方 esp_lcd 框架)
 *
 * 对应 BSP 例子里的 bsp_display_lcd_init(), 产出 esp_lcd 的 panel/io handle,
 * 供 esp_lvgl_adapter 注册显示使用。
 *
 * @param[out] ret_panel 返回 LCD panel handle
 * @param[out] ret_io    返回 LCD panel IO handle
 * @return ESP_OK 成功, 否则失败
 */
esp_err_t display_init(esp_lcd_panel_handle_t *ret_panel,
                       esp_lcd_panel_io_handle_t *ret_io);

/**
 * @brief 设置背光亮度
 * @param duty 占空比 0~8191 (13位, 0最暗, 8191最亮)
 */
void display_backlight_set(uint32_t duty);

#ifdef __cplusplus
}
#endif
