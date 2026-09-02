#include "bsp_display.h"
#include "display.h"
#include "esp_lv_adapter.h"
#include "esp_log.h"

static const char *TAG = "BSP_DISPLAY";

static esp_lcd_panel_handle_t s_panel = NULL;

esp_err_t bsp_display_start_with_config(const bsp_display_cfg_t *cfg)
{
    esp_err_t ret;

    /* 1. LCD panel 初始化 (对应 bsp_display_lcd_init) */
    esp_lcd_panel_io_handle_t io = NULL;
    ret = display_init(&s_panel, &io);
    if (ret != ESP_OK) {
        return ret;
    }

    /* 2. LVGL 初始化 (对应 lvgl_port_init) */
    esp_lv_adapter_config_t adapter_cfg = ESP_LV_ADAPTER_DEFAULT_CONFIG();
    adapter_cfg.task_stack_size = cfg->lvgl_port_cfg.task_stack;
    adapter_cfg.task_core_id    = cfg->lvgl_port_cfg.task_affinity;
    ret = esp_lv_adapter_init(&adapter_cfg);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "LVGL adapter 初始化失败: %d", ret);
        return ret;
    }

    /*
     * 3. 注册显示 (对应 lvgl_port_add_disp)。
     *    SPI DMA draw buffers remain in internal RAM. Camera preview pixels use
     *    a separate PSRAM buffer and are rendered into these 10-line buffers.
     */
    esp_lv_adapter_display_config_t disp_cfg =
        ESP_LV_ADAPTER_DISPLAY_SPI_WITHOUT_PSRAM_DEFAULT_CONFIG(
            s_panel, io, BSP_LCD_H_RES, BSP_LCD_V_RES, ESP_LV_ADAPTER_ROTATE_0);
    lv_display_t *disp = esp_lv_adapter_register_display(&disp_cfg);
    if (disp == NULL) {
        ESP_LOGE(TAG, "注册显示失败");
        return ESP_FAIL;
    }

    /* 4. 启动 LVGL 任务 */
    ret = esp_lv_adapter_start();
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "启动 LVGL 任务失败: %d", ret);
        return ret;
    }

    ESP_LOGI(TAG, "显示初始化完成");
    return ESP_OK;
}

esp_err_t bsp_display_backlight_on(void)
{
    display_backlight_set(8191);  // 100% 亮度
    return ESP_OK;
}

esp_err_t bsp_display_lock(uint32_t timeout_ms)
{
    /* esp-bsp 里 0 表示阻塞等待; adapter 用 -1 表示无限等待, 这里做映射 */
    int32_t t = (timeout_ms == 0) ? -1 : (int32_t)timeout_ms;
    return esp_lv_adapter_lock(t);
}

void bsp_display_unlock(void)
{
    esp_lv_adapter_unlock();
}
