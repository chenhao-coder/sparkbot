#include "display.h"
#include "esp_log.h"
#include "esp_lcd_panel_vendor.h"   // esp_lcd_new_panel_st7789 + esp_lcd_panel_dev_config_t
#include "esp_lcd_io_spi.h"         // esp_lcd_new_panel_io_spi + esp_lcd_panel_io_spi_config_t
#include "driver/gpio.h"
#include "driver/spi_master.h"

static const char *TAG = "DISPLAY";

/* 为相机腾出 GPIO8 和 GPIO6。
 * 保留已验证的 LCD DC 接线，CS 迁移到空闲 GPIO14，
 * RST 迁移到空闲 GPIO2，保留该外接 LCD 模块所需的硬件复位；
 * 避开可能仍与 USB-UART 芯片电气相连的 GPIO43/GPIO44。 */
#define PIN_SCLK  GPIO_NUM_21 // SCL - 时钟
#define PIN_MOSI  GPIO_NUM_47 // SDA - 数据
#define PIN_DC    GPIO_NUM_38 // DC  - 数据/命令选择
#define PIN_CS    GPIO_NUM_14 // CS  - 片选
#define PIN_RST   GPIO_NUM_2  // RES - 硬件复位
#define PIN_BLK   GPIO_NUM_46 // BLK - 背光

#define LCD_PIXEL_CLOCK (20 * 1000 * 1000)  // 20MHz

/* 1.3寸 ST7789: 控制器 RAM 为 240x320, 可见区 240x240 只是其中一个 240 行的窗口。
 * 本模组的可见窗口位于 RAM 第 0~239 行, 所以偏移 0。
 * 若换成可见区居中的模组(第 80~319 行), 这里要改成 80。 */
#define LCD_Y_GAP  0

esp_err_t display_init(esp_lcd_panel_handle_t *ret_panel,
                       esp_lcd_panel_io_handle_t *ret_io)
{
    esp_err_t ret;

    /* 1. 初始化 SPI 总线 */
    spi_bus_config_t bus_cfg = {
        .mosi_io_num     = PIN_MOSI,
        .miso_io_num     = -1,
        .sclk_io_num     = PIN_SCLK,
        .quadwp_io_num   = -1,
        .quadhd_io_num   = -1,
        .max_transfer_sz = DISP_H_RES * DISP_V_RES * 2,
    };
    ret = spi_bus_initialize(SPI2_HOST, &bus_cfg, SPI_DMA_CH_AUTO);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "SPI 总线初始化失败: %d", ret);
        return ret;
    }

    /* 2. 创建 panel IO (DC/CS 由 esp_lcd 框架自动管理) */
    esp_lcd_panel_io_handle_t io = NULL;
    esp_lcd_panel_io_spi_config_t io_cfg = {
        .dc_gpio_num       = PIN_DC,
        .cs_gpio_num       = PIN_CS,
        .pclk_hz           = LCD_PIXEL_CLOCK,
        .lcd_cmd_bits      = 8,
        .lcd_param_bits    = 8,
        .spi_mode          = 0,          // ST7789 同时支持 mode0/mode3
        .trans_queue_depth = 10,
    };
    ret = esp_lcd_new_panel_io_spi(SPI2_HOST, &io_cfg, &io);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "创建 panel IO 失败: %d", ret);
        return ret;
    }

    /* 3. 创建 ST7789 panel (内置默认初始化序列) */
    esp_lcd_panel_handle_t panel = NULL;
    esp_lcd_panel_dev_config_t panel_cfg = {
        .reset_gpio_num = PIN_RST,
        .rgb_ele_order  = LCD_RGB_ELEMENT_ORDER_RGB,
        .data_endian    = LCD_RGB_DATA_ENDIAN_BIG,
        .bits_per_pixel = 16,           // RGB565
    };
    ret = esp_lcd_new_panel_st7789(io, &panel_cfg, &panel);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "创建 ST7789 panel 失败: %d", ret);
        return ret;
    }

    /* 4. 复位 + 初始化 */
    esp_lcd_panel_reset(panel);
    esp_lcd_panel_init(panel);

    /* This 1.3-inch ST7789 panel uses the inverted drive polarity. Without
     * INVON, commanded black appears white/gray and the whole image is color
     * inverted. This matches the original SparkBot BSP initialization. */
    ret = esp_lcd_panel_invert_color(panel, true);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "LCD 颜色反相配置失败: %d", ret);
        return ret;
    }

    /* 5. 240x240 可见区在 240x320 RAM 中的偏移 */
    esp_lcd_panel_set_gap(panel, 0, LCD_Y_GAP);

    /* 6. 开显示 */
    esp_lcd_panel_disp_on_off(panel, true);

    /* 7. 背光只需要开/关，直接使用 GPIO，完整保留 LEDC0 给相机 XCLK。 */
    gpio_config_t backlight_cfg = {
        .pin_bit_mask = 1ULL << PIN_BLK,
        .mode = GPIO_MODE_OUTPUT,
        .pull_up_en = GPIO_PULLUP_DISABLE,
        .pull_down_en = GPIO_PULLDOWN_DISABLE,
        .intr_type = GPIO_INTR_DISABLE,
    };
    ret = gpio_config(&backlight_cfg);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "背光 GPIO 初始化失败: %d", ret);
        return ret;
    }
    display_backlight_set(8191);  // 打开背光

    *ret_panel = panel;
    *ret_io    = io;

    ESP_LOGI(TAG, "ST7789 显示初始化完成 (%dx%d)", DISP_H_RES, DISP_V_RES);
    return ESP_OK;
}

void display_backlight_set(uint32_t duty)
{
    gpio_set_level(PIN_BLK, duty > 0 ? 1 : 0);
}
