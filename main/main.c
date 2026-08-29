#include <stdio.h>
#include <esp_event.h>
#include "esp_log.h"
#include "nvs_flash.h"
#include "ui.h"
#include "audio.h"
#include "opus_audio.h"
#include "yay_wav.h"
#include "wifi.h"

static const char *TAG = "MAIN";

void app_main(void)
{
    ESP_LOGI(TAG, "app_main 启动");

    esp_err_t ret = nvs_flash_init();
    if (ret == ESP_ERR_NVS_NO_FREE_PAGES ||
        ret == ESP_ERR_NVS_NEW_VERSION_FOUND) {
        ESP_ERROR_CHECK(nvs_flash_erase());
        ret = nvs_flash_init();
    }
    ESP_ERROR_CHECK(ret);

    ESP_ERROR_CHECK(esp_event_loop_create_default());

    app_ui_start();

    oai_init_audio_capture();
    oai_init_audio_decoder();

    /* 临时测试: 播放 yay.wav 验证喇叭通路 */
    oai_play_test_audio();
    oai_wifi();
}
