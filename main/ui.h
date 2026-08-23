#pragma once

#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

/**
 * @brief 启动 UI (初始化显示 + LVGL, 对应 BSP 例子的 app_ui_start)
 */
esp_err_t app_ui_start(void);

#ifdef __cplusplus
}
#endif
