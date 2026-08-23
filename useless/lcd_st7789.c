#include "lcd_st7789.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

static const char *TAG = "LCD_ST7789";

static spi_device_handle_t g_spi_handle = NULL;

/*
 * 1.3寸ST7789屏幕: RAM为240x320, 可见区域为240x240
 * 行地址需要加80偏移, 否则数据写到屏幕外(黑屏)
 */
#define LCD_Y_OFFSET    80

/* ---------- 内部辅助: 发送命令 ---------- */
static void lcd_send_cmd(uint8_t cmd)
{
    esp_err_t ret;
    spi_transaction_t t = {
        .length    = 8,
        .tx_buffer = &cmd,
        .user      = (void *)0,       // DC=0
    };
    ret = spi_device_polling_transmit(g_spi_handle, &t);
    assert(ret == ESP_OK);
}

/* ---------- 内部辅助: 发送单字节数据 ---------- */
static void lcd_send_data(uint8_t data)
{
    esp_err_t ret;
    spi_transaction_t t = {
        .length    = 8,
        .tx_buffer = &data,
        .user      = (void *)1,       // DC=1
    };
    ret = spi_device_polling_transmit(g_spi_handle, &t);
    assert(ret == ESP_OK);
}

/* ---------- 内部辅助: 发送多字节数据 ---------- */
static void lcd_send_data_multi(const uint8_t *data, size_t len)
{
    esp_err_t ret;
    spi_transaction_t t = {
        .length    = len * 8,
        .tx_buffer = data,
        .user      = (void *)1,       // DC=1
    };
    ret = spi_device_polling_transmit(g_spi_handle, &t);
    assert(ret == ESP_OK);
}

/* ---------- 内部辅助: 发送16位数据(高字节先) ---------- */
static inline void lcd_send_data16(uint16_t data)
{
    lcd_send_data(data >> 8);
    lcd_send_data(data & 0xFF);
}

/* ---------- SPI传输前回调: 设置DC引脚 ---------- */
static void lcd_spi_pre_transfer_callback(spi_transaction_t *t)
{
    int dc_level = (int)(t->user);
    gpio_set_level(PIN_DC, dc_level);
}

/* ---------- 设置窗口地址(Y轴带偏移) ---------- */
static void lcd_set_window(uint16_t x1, uint16_t y1,
                           uint16_t x2, uint16_t y2)
{
    lcd_send_cmd(0x2A);  // CASET 列地址
    lcd_send_data16(x1);
    lcd_send_data16(x2);

    lcd_send_cmd(0x2B);  // RASET 行地址 (+80偏移)
    lcd_send_data16(y1 + LCD_Y_OFFSET);
    lcd_send_data16(y2 + LCD_Y_OFFSET);

    lcd_send_cmd(0x2C);  // RAMWR
}

/* ===================================================================
 *  LCD初始化 — 参数严格参考已验证的ST7789 1.3寸模组驱动
 * =================================================================== */
esp_err_t lcd_init(void)
{
    esp_err_t ret;

    ESP_LOGI(TAG, "========== ST7789 LCD 初始化开始 ==========");

    /* ---------- 1. 配置GPIO (RES + DC) ---------- */
    gpio_config_t io_cfg = {
        .pin_bit_mask = (1ULL << PIN_DC) | (1ULL << PIN_RST),
        .mode         = GPIO_MODE_OUTPUT,
        .pull_up_en   = GPIO_PULLUP_DISABLE,
        .pull_down_en = GPIO_PULLDOWN_DISABLE,
        .intr_type    = GPIO_INTR_DISABLE,
    };
    gpio_config(&io_cfg);
    gpio_set_level(PIN_DC, 0);

    // 硬件复位 (参考已验证驱动)
    ESP_LOGI(TAG, "硬件复位 RES → GPIO_%d", PIN_RST);
    gpio_set_level(PIN_RST, 1);
    vTaskDelay(pdMS_TO_TICKS(50));
    gpio_set_level(PIN_RST, 0);
    vTaskDelay(pdMS_TO_TICKS(50));
    gpio_set_level(PIN_RST, 1);
    vTaskDelay(pdMS_TO_TICKS(120));

    /* ---------- 2. 初始化SPI总线 ---------- */
    ESP_LOGI(TAG, "SPI总线: MOSI=GPIO_%d SCLK=GPIO_%d CS=GPIO_%d",
             PIN_MOSI, PIN_SCLK, PIN_CS);

    spi_bus_config_t bus_cfg = {
        .mosi_io_num     = PIN_MOSI,
        .miso_io_num     = -1,
        .sclk_io_num     = PIN_SCLK,
        .quadwp_io_num   = -1,
        .quadhd_io_num   = -1,
        .max_transfer_sz = LCD_H_RES * LCD_V_RES * 2,
    };
    ret = spi_bus_initialize(LCD_HOST, &bus_cfg, SPI_DMA_CH_AUTO);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "SPI总线初始化失败: %d", ret);
        return ret;
    }

    /* ---------- 3. 添加SPI设备 ---------- */
    ESP_LOGI(TAG, "SPI设备: 模式%d (CPOL=%d CPHA=%d) %dMHz",
             LCD_SPI_MODE,
             (LCD_SPI_MODE >> 1) & 1,
             LCD_SPI_MODE & 1,
             LCD_PIXEL_CLOCK / 1000000);

    spi_device_interface_config_t dev_cfg = {
        .mode           = LCD_SPI_MODE,
        .clock_speed_hz = LCD_PIXEL_CLOCK,
        .spics_io_num   = PIN_CS,
        .queue_size     = 7,
        .pre_cb         = lcd_spi_pre_transfer_callback,
        .flags          = SPI_DEVICE_NO_DUMMY,
    };
    ret = spi_bus_add_device(LCD_HOST, &dev_cfg, &g_spi_handle);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "添加SPI设备失败: %d", ret);
        return ret;
    }

    /* ---------- 4. ST7789初始化序列 (参考已验证驱动) ---------- */
    ESP_LOGI(TAG, "发送ST7789初始化序列...");

    // SWRESET 软件复位
    lcd_send_cmd(0x01);
    vTaskDelay(pdMS_TO_TICKS(150));

    // SLPOUT 退出休眠
    lcd_send_cmd(0x11);
    vTaskDelay(pdMS_TO_TICKS(120));

    // MADCTL 内存访问控制
    lcd_send_cmd(0x36);
    lcd_send_data(0x00);

    // COLMOD 像素格式: 16-bit RGB565
    lcd_send_cmd(0x3A);
    lcd_send_data(0x05);

    // PORCTRL Porch控制
    lcd_send_cmd(0xB2);
    lcd_send_data(0x1F);
    lcd_send_data(0x1F);
    lcd_send_data(0x00);
    lcd_send_data(0x33);
    lcd_send_data(0x33);

    // GCTRL Gate控制
    lcd_send_cmd(0xB7);
    lcd_send_data(0x00);

    // VCOMS VCOM设置
    lcd_send_cmd(0xBB);
    lcd_send_data(0x3F);

    // LCMCTRL LCM控制
    lcd_send_cmd(0xC0);
    lcd_send_data(0x2C);

    // VDVVRHEN VDV和VRH命令使能
    lcd_send_cmd(0xC2);
    lcd_send_data(0x01);

    // FRCTRL2 帧率控制
    lcd_send_cmd(0xC3);
    lcd_send_data(0x0F);

    // PWCTRL1 电源控制1
    lcd_send_cmd(0xC4);
    lcd_send_data(0x20);

    // 电源控制2
    lcd_send_cmd(0xC6);
    lcd_send_data(0x13);

    // 电源电压设置
    lcd_send_cmd(0xD0);
    lcd_send_data(0xA4);
    lcd_send_data(0xA1);

    // sleep in后gate输出为GND
    lcd_send_cmd(0xD6);
    lcd_send_data(0xA1);

    // 正电压Gamma控制
    lcd_send_cmd(0xE0);
    lcd_send_data(0xF0);
    lcd_send_data(0x06);
    lcd_send_data(0x0D);
    lcd_send_data(0x0B);
    lcd_send_data(0x0A);
    lcd_send_data(0x07);
    lcd_send_data(0x2E);
    lcd_send_data(0x43);
    lcd_send_data(0x45);
    lcd_send_data(0x38);
    lcd_send_data(0x14);
    lcd_send_data(0x13);
    lcd_send_data(0x25);
    lcd_send_data(0x29);

    // 负电压Gamma控制
    lcd_send_cmd(0xE1);
    lcd_send_data(0xF0);
    lcd_send_data(0x07);
    lcd_send_data(0x0A);
    lcd_send_data(0x08);
    lcd_send_data(0x07);
    lcd_send_data(0x23);
    lcd_send_data(0x2E);
    lcd_send_data(0x33);
    lcd_send_data(0x44);
    lcd_send_data(0x3A);
    lcd_send_data(0x16);
    lcd_send_data(0x17);
    lcd_send_data(0x26);
    lcd_send_data(0x2C);

    // Gate数量: (0x1D+1)*8 = 240条gate线
    lcd_send_cmd(0xE4);
    lcd_send_data(0x1D);
    lcd_send_data(0x00);
    lcd_send_data(0x00);

    // INVON 显示反转
    lcd_send_cmd(0x21);

    // 再次退出休眠
    lcd_send_cmd(0x11);
    vTaskDelay(pdMS_TO_TICKS(120));

    // DISPON 显示开启
    lcd_send_cmd(0x29);
    vTaskDelay(pdMS_TO_TICKS(50));

    ESP_LOGI(TAG, "ST7789初始化序列完成");

    /* ---------- 5. 配置背光 ---------- */
    ESP_LOGI(TAG, "配置背光PWM: GPIO_%d", PIN_BLK);

    ledc_timer_config_t ledc_timer = {
        .speed_mode       = LEDC_LOW_SPEED_MODE,
        .duty_resolution  = LEDC_TIMER_13_BIT,
        .timer_num        = LEDC_TIMER_0,
        .freq_hz          = 5000,
        .clk_cfg          = LEDC_AUTO_CLK,
    };
    ledc_timer_config(&ledc_timer);

    ledc_channel_config_t ledc_channel = {
        .gpio_num       = PIN_BLK,
        .speed_mode     = LEDC_LOW_SPEED_MODE,
        .channel        = LEDC_CHANNEL_0,
        .timer_sel      = LEDC_TIMER_0,
        .duty           = 0,
        .hpoint         = 0,
    };
    ledc_channel_config(&ledc_channel);

    lcd_set_backlight(8191);  // 100%亮度
    ESP_LOGI(TAG, "背光: 100%%");

    ESP_LOGI(TAG, "========== LCD初始化完成 ==========");
    return ESP_OK;
}

/* ===================================================================
 *  公共API
 * =================================================================== */

void lcd_set_backlight(uint32_t duty)
{
    if (duty > 8191) duty = 8191;
    ledc_set_duty(LEDC_LOW_SPEED_MODE, LEDC_CHANNEL_0, duty);
    ledc_update_duty(LEDC_LOW_SPEED_MODE, LEDC_CHANNEL_0);
}

void lcd_fill_screen(uint16_t color)
{
    lcd_fill_area(0, 0, LCD_H_RES - 1, LCD_V_RES - 1, color);
}

void lcd_fill_area(uint16_t x_start, uint16_t y_start,
                   uint16_t x_end, uint16_t y_end, uint16_t color)
{
    lcd_set_window(x_start, y_start, x_end, y_end);

    uint32_t pixel_count = (uint32_t)(x_end - x_start + 1) *
                           (uint32_t)(y_end - y_start + 1);

    /* 预填充发送缓冲区 (RGB565 → 2 bytes/pixel) */
    #define CHUNK 2048
    static uint8_t buf[CHUNK];
    for (int i = 0; i < CHUNK; i += 2) {
        buf[i]     = color >> 8;
        buf[i + 1] = color & 0xFF;
    }

    while (pixel_count > 0) {
        size_t n = (pixel_count > (CHUNK / 2)) ? (CHUNK / 2) : pixel_count;
        lcd_send_data_multi(buf, n * 2);
        pixel_count -= n;
    }
}

void lcd_draw_pixel(uint16_t x, uint16_t y, uint16_t color)
{
    lcd_fill_area(x, y, x, y, color);
}

void lcd_write_image(uint16_t x_start, uint16_t y_start,
                     uint16_t x_end, uint16_t y_end,
                     const uint8_t *data, size_t len)
{
    lcd_set_window(x_start, y_start, x_end, y_end);
    lcd_send_data_multi(data, len);
}
