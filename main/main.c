#include <esp_event.h>
#include <stdio.h>

#include "audio.h"
#include "camera.h"
#include "doubao_realtime.h"
#include "esp_log.h"
#include "nvs_flash.h"
#include "ui.h"
#include "wifi.h"

static const char *TAG = "MAIN";

void app_main(void) {
    ESP_LOGI(TAG, "app_main 启动");

    esp_err_t ret = nvs_flash_init();
    if (ret == ESP_ERR_NVS_NO_FREE_PAGES || ret == ESP_ERR_NVS_NEW_VERSION_FOUND) {
        ESP_ERROR_CHECK(nvs_flash_erase());
        ret = nvs_flash_init();
    }
    ESP_ERROR_CHECK(ret);

    ESP_ERROR_CHECK(esp_event_loop_create_default());

    ret = app_ui_start();
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "UI initialization failed: %s", esp_err_to_name(ret));
        return;
    }

    /* Keep the ATK-OV2640 quiet while ES8311 is configured on shared I2C0. */
    ret = sparkbot_camera_hold_reset();
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "摄像头复位引脚初始化失败: %s", esp_err_to_name(ret));
        return;
    }

    ret = oai_init_audio_capture();
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "音频初始化失败，停止启动实时语音: %s", esp_err_to_name(ret));
        return;
    }

    ret = sparkbot_camera_init();
    if (ret == ESP_OK) {
        ret = sparkbot_camera_capture_probe();
    }
    if (ret == ESP_OK) {
        ret = sparkbot_camera_start_preview();
    }
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "摄像头探测/预览启动失败: %s", esp_err_to_name(ret));
    }

    oai_wifi();

    ret = doubao_realtime_start();
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "豆包实时语音启动失败: %s", esp_err_to_name(ret));
    }
}
