#include "ui.h"

#include <stdbool.h>
#include <string.h>

#include "bsp_display.h"
#include "esp_heap_caps.h"
#include "esp_log.h"
#include "lvgl.h"

static const char *TAG = "UI";

#define CAMERA_PREVIEW_WIDTH       240U
#define CAMERA_PREVIEW_HEIGHT      240U
#define CAMERA_PREVIEW_PIXEL_BYTES 2U
#define CAMERA_PREVIEW_BUFFER_SIZE \
    (CAMERA_PREVIEW_WIDTH * CAMERA_PREVIEW_HEIGHT * CAMERA_PREVIEW_PIXEL_BYTES)
#define LCD_DRAW_BUFFER_LINES      80U

static uint8_t *s_camera_preview_buffer;
static lv_obj_t *s_camera_preview_image;
static bool s_camera_preview_visible;

/*
 * OV2640 RGB565 arrives MSB first, while LVGL's native RGB565 is stored in
 * CPU byte order. RGB565_SWAPPED lets LVGL perform that conversion while it
 * renders, avoiding an extra byte-swap pass in the camera task.
 */
static lv_image_dsc_t s_camera_preview_dsc = {
    .header = {
        .magic = LV_IMAGE_HEADER_MAGIC,
        .cf = LV_COLOR_FORMAT_RGB565_SWAPPED,
        .flags = 0,
        .w = CAMERA_PREVIEW_WIDTH,
        .h = CAMERA_PREVIEW_HEIGHT,
        .stride = CAMERA_PREVIEW_WIDTH * CAMERA_PREVIEW_PIXEL_BYTES,
        .reserved_2 = 0,
    },
    .data_size = CAMERA_PREVIEW_BUFFER_SIZE,
    .data = NULL,
    .reserved = NULL,
    .reserved_2 = NULL,
};

esp_err_t app_ui_start(void)
{
    /* 初始化显示和 LVGL (对应 bsp_display_start_with_config) */
    bsp_display_cfg_t custom_cfg = {
        .lvgl_port_cfg = ESP_LVGL_PORT_INIT_CONFIG(),
        .buffer_size   = BSP_LCD_H_RES * BSP_LCD_V_RES,
        .trans_size    = BSP_LCD_H_RES * LCD_DRAW_BUFFER_LINES,
        .double_buffer = 0,
        .flags = {
            .buff_dma    = false,
            .buff_spiram = true,
        }};
    custom_cfg.lvgl_port_cfg.task_stack    = 8 * 1024;  // 全屏图片渲染需要足够栈, 4KB 会栈溢出导致复位
    custom_cfg.lvgl_port_cfg.task_affinity = 1;
    esp_err_t ret = bsp_display_start_with_config(&custom_cfg);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "Display/LVGL initialization failed: %s", esp_err_to_name(ret));
        return ret;
    }

    /* 打开背光 */
    ret = bsp_display_backlight_on();
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "Display backlight initialization failed: %s", esp_err_to_name(ret));
        return ret;
    }

    s_camera_preview_buffer = heap_caps_malloc(CAMERA_PREVIEW_BUFFER_SIZE,
                                               MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    if (s_camera_preview_buffer == NULL) {
        ESP_LOGE(TAG, "Failed to allocate %u-byte camera preview buffer in PSRAM",
                 (unsigned int)CAMERA_PREVIEW_BUFFER_SIZE);
        return ESP_ERR_NO_MEM;
    }
    memset(s_camera_preview_buffer, 0, CAMERA_PREVIEW_BUFFER_SIZE);
    s_camera_preview_dsc.data = s_camera_preview_buffer;

    ESP_LOGI(TAG, "Display initialized");
    ret = bsp_display_lock(0);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "Failed to acquire LVGL lock: %s", esp_err_to_name(ret));
        return ret;
    }

    /* 全屏黑色背景 */
    lv_obj_t *obj_bg = lv_obj_create(lv_screen_active());
    lv_obj_set_size(obj_bg, BSP_LCD_H_RES, BSP_LCD_V_RES);
    lv_obj_set_align(obj_bg, LV_ALIGN_CENTER);
    lv_obj_clear_flag(obj_bg, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_set_style_bg_color(obj_bg, lv_color_hex(0x000000), 0);
    lv_obj_set_style_border_width(obj_bg, 0, 0);

    /* 显示 OpenAI_logo 图片 (对应 BSP 例子的 ui_emoji) */
    extern const lv_img_dsc_t OpenAI_logo;
    s_camera_preview_image = lv_image_create(obj_bg);
    lv_obj_set_size(s_camera_preview_image, 240, 240);
    lv_obj_center(s_camera_preview_image);
    lv_image_set_src(s_camera_preview_image, &OpenAI_logo);

    bsp_display_unlock();

    ESP_LOGI(TAG, "Camera preview buffer ready in PSRAM (%u bytes)",
             (unsigned int)CAMERA_PREVIEW_BUFFER_SIZE);
    return ESP_OK;
}

esp_err_t app_ui_show_camera_frame(const uint8_t *frame_data, size_t frame_size)
{
    if (frame_data == NULL || frame_size != CAMERA_PREVIEW_BUFFER_SIZE) {
        return ESP_ERR_INVALID_ARG;
    }
    if (s_camera_preview_buffer == NULL || s_camera_preview_image == NULL) {
        return ESP_ERR_INVALID_STATE;
    }

    esp_err_t ret = bsp_display_lock(0);
    if (ret != ESP_OK) {
        return ret;
    }

    /* The LVGL lock also prevents rendering from reading a half-copied frame. */
    memcpy(s_camera_preview_buffer, frame_data, CAMERA_PREVIEW_BUFFER_SIZE);
    if (!s_camera_preview_visible) {
        lv_image_set_src(s_camera_preview_image, &s_camera_preview_dsc);
        s_camera_preview_visible = true;
    }
    lv_obj_invalidate(s_camera_preview_image);

    bsp_display_unlock();
    return ESP_OK;
}
