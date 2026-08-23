#pragma once

#include "driver/spi_master.h"
#include "driver/gpio.h"
#include "driver/ledc.h"
#include "esp_lcd_panel_io.h"
#include "esp_lcd_panel_vendor.h"
#include "esp_lcd_panel_ops.h"
#include "esp_log.h"

#ifdef __cplusplus
extern "C" {
#endif

/* ===================== 引脚定义 ===================== */
#define LCD_HOST        SPI2_HOST
#define LCD_SPI_MODE    3           // CPOL=1, CPHA=1 (ST7789: 空闲SCL=H, 上升沿采样)

#define PIN_SCLK        GPIO_NUM_21 // SCL - 时钟
#define PIN_MOSI        GPIO_NUM_47 // SDA - 数据
#define PIN_DC          GPIO_NUM_5  // DC  - 数据/命令选择
#define PIN_CS          GPIO_NUM_8  // CS  - 片选
#define PIN_RST         GPIO_NUM_6  // RES - 硬件复位
#define PIN_BLK         GPIO_NUM_46 // BLK - 背光

/* ===================== LCD参数 ===================== */
#define LCD_H_RES       240
#define LCD_V_RES       240
#define LCD_PIXEL_CLOCK (20 * 1000 * 1000)  // 20MHz (降低速率确保稳定性)

/* ===================== 颜色定义 (RGB565) ===================== */
#define COLOR_BLACK     0x0000
#define COLOR_WHITE     0xFFFF
#define COLOR_RED       0xF800
#define COLOR_GREEN     0x07E0
#define COLOR_BLUE      0x001F
#define COLOR_YELLOW    0xFFE0
#define COLOR_CYAN      0x07FF
#define COLOR_MAGENTA   0xF81F
#define COLOR_GRAY      0x8410

/* ===================== 函数声明 ===================== */

/**
 * @brief 初始化LCD (SPI + ST7789 + 背光)
 * @return ESP_OK 成功, 否则失败
 */
esp_err_t lcd_init(void);

/**
 * @brief 设置背光亮度
 * @param duty 占空比 0~8191 (13位分辨率, 0最暗, 8191最亮)
 */
void lcd_set_backlight(uint32_t duty);

/**
 * @brief 填充整个屏幕为指定颜色
 * @param color RGB565颜色值
 */
void lcd_fill_screen(uint16_t color);

/**
 * @brief 在指定区域填充颜色
 * @param x_start 起始X坐标
 * @param y_start 起始Y坐标
 * @param x_end   结束X坐标
 * @param y_end   结束Y坐标
 * @param color   RGB565颜色值
 */
void lcd_fill_area(uint16_t x_start, uint16_t y_start,
                   uint16_t x_end, uint16_t y_end, uint16_t color);

/**
 * @brief 画一个像素点
 * @param x     X坐标
 * @param y     Y坐标
 * @param color RGB565颜色值
 */
void lcd_draw_pixel(uint16_t x, uint16_t y, uint16_t color);

/**
 * @brief 写入图像数据到指定区域
 * @param x_start 起始X坐标
 * @param y_start 起始Y坐标
 * @param x_end   结束X坐标
 * @param y_end   结束Y坐标
 * @param data    像素数据缓冲区 (RGB565格式)
 * @param len     数据长度 (字节数)
 */
void lcd_write_image(uint16_t x_start, uint16_t y_start,
                     uint16_t x_end, uint16_t y_end,
                     const uint8_t *data, size_t len);

#ifdef __cplusplus
}
#endif
