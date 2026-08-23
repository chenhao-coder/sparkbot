#include <stdio.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "esp_log.h"
#include "lcd_st7789.h"
#include "lcd_st7789_test.h"

static const char *TAG = "LCD_TEST";

/* 颜色轮播测试 */
static void test_color_cycle(void)
{
    uint16_t colors[] = {
        COLOR_WHITE, COLOR_RED, COLOR_GREEN, COLOR_BLUE,
        COLOR_YELLOW, COLOR_CYAN, COLOR_MAGENTA,
        COLOR_BLACK, COLOR_GRAY,
    };
    const char *color_names[] = {
        "白色", "红色", "绿色", "蓝色",
        "黄色", "青色", "洋红",
        "黑色", "灰色",
    };
    int num_colors = sizeof(colors) / sizeof(colors[0]);

    for (int i = 0; i < num_colors; i++) {
        ESP_LOGI(TAG, "显示颜色: %s (0x%04X)", color_names[i], colors[i]);
        lcd_fill_screen(colors[i]);
        vTaskDelay(pdMS_TO_TICKS(500));
    }
    ESP_LOGI(TAG, "颜色轮播测试完成 ✓");
}

/* 彩色条纹测试 */
static void test_color_stripes(void)
{
    ESP_LOGI(TAG, "绘制彩色条纹...");
    int stripe_width = LCD_H_RES / 8;  // 8条等宽条纹

    uint16_t stripe_colors[] = {
        COLOR_RED, COLOR_GREEN, COLOR_BLUE, COLOR_YELLOW,
        COLOR_CYAN, COLOR_MAGENTA, COLOR_WHITE, COLOR_BLACK
    };

    for (int i = 0; i < 8; i++) {
        lcd_fill_area(i * stripe_width, 0,
                      (i + 1) * stripe_width - 1, LCD_V_RES - 1,
                      stripe_colors[i]);
    }
    vTaskDelay(pdMS_TO_TICKS(1500));
    ESP_LOGI(TAG, "彩色条纹测试完成 ✓");
}

/* 棋盘格测试 */
static void test_checkerboard(void)
{
    ESP_LOGI(TAG, "绘制棋盘格...");
    int cell_size = 30;  // 每格30x30像素

    for (int y = 0; y < LCD_V_RES; y += cell_size) {
        for (int x = 0; x < LCD_H_RES; x += cell_size) {
            int is_white = ((x / cell_size) + (y / cell_size)) % 2;
            uint16_t color = is_white ? COLOR_WHITE : COLOR_BLACK;

            int end_x = (x + cell_size - 1) < LCD_H_RES ? (x + cell_size - 1) : (LCD_H_RES - 1);
            int end_y = (y + cell_size - 1) < LCD_V_RES ? (y + cell_size - 1) : (LCD_V_RES - 1);
            lcd_fill_area(x, y, end_x, end_y, color);
        }
    }
    vTaskDelay(pdMS_TO_TICKS(1500));
    ESP_LOGI(TAG, "棋盘格测试完成 ✓");
}

/* RGB三色框测试 */
static void test_rgb_frames(void)
{
    ESP_LOGI(TAG, "绘制RGB三色框...");

    // 全屏填充为黑色
    lcd_fill_screen(COLOR_BLACK);
    vTaskDelay(pdMS_TO_TICKS(200));

    int frame_thickness = 10;

    // 外框 - 红色
    lcd_fill_area(0, 0, LCD_H_RES - 1, frame_thickness, COLOR_RED);                      // 上
    lcd_fill_area(0, LCD_V_RES - frame_thickness, LCD_H_RES - 1, LCD_V_RES - 1, COLOR_RED); // 下
    lcd_fill_area(0, 0, frame_thickness, LCD_V_RES - 1, COLOR_RED);                      // 左
    lcd_fill_area(LCD_H_RES - frame_thickness, 0, LCD_H_RES - 1, LCD_V_RES - 1, COLOR_RED); // 右

    // 中框 - 绿色 (缩小30px)
    lcd_fill_area(30, 30, LCD_H_RES - 1 - 30, 30 + frame_thickness, COLOR_GREEN);
    lcd_fill_area(30, LCD_V_RES - 30 - frame_thickness, LCD_H_RES - 1 - 30, LCD_V_RES - 1 - 30, COLOR_GREEN);
    lcd_fill_area(30, 30, 30 + frame_thickness, LCD_V_RES - 1 - 30, COLOR_GREEN);
    lcd_fill_area(LCD_H_RES - 30 - frame_thickness, 30, LCD_H_RES - 1 - 30, LCD_V_RES - 1 - 30, COLOR_GREEN);

    // 内框 - 蓝色 (再缩小30px)
    lcd_fill_area(60, 60, LCD_H_RES - 1 - 60, 60 + frame_thickness, COLOR_BLUE);
    lcd_fill_area(60, LCD_V_RES - 60 - frame_thickness, LCD_H_RES - 1 - 60, LCD_V_RES - 1 - 60, COLOR_BLUE);
    lcd_fill_area(60, 60, 60 + frame_thickness, LCD_V_RES - 1 - 60, COLOR_BLUE);
    lcd_fill_area(LCD_H_RES - 60 - frame_thickness, 60, LCD_H_RES - 1 - 60, LCD_V_RES - 1 - 60, COLOR_BLUE);

    vTaskDelay(pdMS_TO_TICKS(2000));
    ESP_LOGI(TAG, "RGB三色框测试完成 ✓");
}

/* 背光呼吸灯测试 */
static void test_backlight_breathing(void)
{
    ESP_LOGI(TAG, "背光呼吸灯测试...");

    lcd_fill_screen(COLOR_WHITE);

    // 呼吸效果: 渐变3个周期
    for (int cycle = 0; cycle < 3; cycle++) {
        // 渐亮
        for (int duty = 0; duty <= 8191; duty += 100) {
            lcd_set_backlight(duty);
            vTaskDelay(pdMS_TO_TICKS(1));
        }
        // 渐暗
        for (int duty = 8191; duty >= 0; duty -= 100) {
            lcd_set_backlight(duty);
            vTaskDelay(pdMS_TO_TICKS(1));
        }
    }

    lcd_set_backlight(4095);  // 恢复50%亮度
    ESP_LOGI(TAG, "背光呼吸灯测试完成 ✓");
}

/* 绘制渐变填充测试 */
static void test_gradient(void)
{
    ESP_LOGI(TAG, "绘制RGB渐变...");

    // 从红色渐变到蓝色 (水平方向)
    for (int x = 0; x < LCD_H_RES; x++) {
        uint8_t r = (uint8_t)(255 * (LCD_H_RES - 1 - x) / LCD_H_RES);
        uint8_t b = (uint8_t)(255 * x / LCD_H_RES);
        uint16_t color = ((r & 0xF8) << 8) | (0x00) | ((b >> 3) & 0x1F);
        lcd_fill_area(x, 0, x, LCD_V_RES / 2, color);
    }

    // 从绿色渐变到蓝色 (垂直下半)
    for (int y = LCD_V_RES / 2; y < LCD_V_RES; y++) {
        uint8_t g = (uint8_t)(255 * (LCD_V_RES - 1 - y) / LCD_V_RES * 2);
        uint8_t b = (uint8_t)(255 * (y - LCD_V_RES / 2) / (LCD_V_RES / 2));
        uint16_t color = ((g & 0xE0) << 3) | (b >> 3);
        lcd_fill_area(0, y, LCD_H_RES - 1, y, color);
    }

    vTaskDelay(pdMS_TO_TICKS(2000));
    ESP_LOGI(TAG, "RGB渐变测试完成 ✓");
}

void lcd_st7789_test_run(void)
{
    ESP_LOGI(TAG, "╔══════════════════════════════════════════════╗");
    ESP_LOGI(TAG, "║   ESP32-S3 + ST7789 LCD 通路验证测试         ║");
    ESP_LOGI(TAG, "╠══════════════════════════════════════════════╣");
    ESP_LOGI(TAG, "║  引脚连接:                                   ║");
    ESP_LOGI(TAG, "║    SCLK (SCL)  → GPIO_%02d                    ║", PIN_SCLK);
    ESP_LOGI(TAG, "║    MOSI (SDA)  → GPIO_%02d                    ║", PIN_MOSI);
    ESP_LOGI(TAG, "║    CS   (CS)   → GPIO_%02d                    ║", PIN_CS);
    ESP_LOGI(TAG, "║    DC   (DC)   → GPIO_%02d                    ║", PIN_DC);
    ESP_LOGI(TAG, "║    BLK  (BLK)  → GPIO_%02d                    ║", PIN_BLK);
    ESP_LOGI(TAG, "║    RES  (RES)  → GPIO_%02d                    ║", PIN_RST);
    ESP_LOGI(TAG, "╠══════════════════════════════════════════════╣");
    ESP_LOGI(TAG, "║   分辨率: 240 x 240                          ║");
    ESP_LOGI(TAG, "║   驱动IC: ST7789                             ║");
    ESP_LOGI(TAG, "║   接口:   4线SPI (模式3)                     ║");
    ESP_LOGI(TAG, "╚══════════════════════════════════════════════╝");

    /* 初始化LCD */
    esp_err_t ret = lcd_init();
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "LCD初始化失败, 程序退出!");
        return;
    }

    while (1) {
        ESP_LOGI(TAG, "==================== 测试开始 ====================");

        /* 测试1: 颜色循环 */
        test_color_cycle();

        /* 测试2: 彩色条纹 */
        test_color_stripes();

        /* 测试3: 棋盘格 */
        test_checkerboard();

        /* 测试4: RGB三色框 */
        test_rgb_frames();

        /* 测试5: 渐变填充 */
        test_gradient();

        /* 测试6: 背光呼吸灯 */
        test_backlight_breathing();

        ESP_LOGI(TAG, "==================== 测试完成 ====================");
        ESP_LOGI(TAG, "所有测试通过! 2秒后重新开始...");
        vTaskDelay(pdMS_TO_TICKS(2000));
    }
}
