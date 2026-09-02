#pragma once

#include <stddef.h>
#include <stdint.h>

#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

/**
 * @brief 启动 UI (初始化显示 + LVGL, 对应 BSP 例子的 app_ui_start)
 */
esp_err_t app_ui_start(void);

/**
 * @brief Copy one 240x240 RGB565 camera frame into the LVGL preview buffer.
 *
 * The OV2640 frame uses high-byte-first RGB565. The UI descriptor marks it as
 * RGB565_SWAPPED so LVGL converts it to the display's native RGB565 format.
 */
esp_err_t app_ui_show_camera_frame(const uint8_t *frame_data, size_t frame_size);

#ifdef __cplusplus
}
#endif
