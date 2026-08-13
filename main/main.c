/*
 * SPDX-FileCopyrightText: 2022 Espressif Systems (Shanghai) CO LTD
 *
 * SPDX-License-Identifier: CC0-1.0
 */

#include <inttypes.h>
#include <string.h>
#include "esp_log.h"
#include "nvs_flash.h"
#include "esp_sparkbot_bsp.h"
#include "iot_button.h"
#include "esp_mmap_assets.h"
#include "mmap_generate_audio.h"
#include "lvgl.h"
#include "ui/ui.h"
#include "app_wifi.h"

#define TAG "ESP-EXAMPLE"

typedef struct {
    const uint8_t *pcm;   /* 指向 mmap 的 flash 数据, 不拷贝不 free */
    size_t len;
    uint32_t sample_rate;
    uint16_t channels;
    uint16_t bits_per_sample;
} audio_data_t;

typedef struct {
    const uint8_t *data;
    size_t len;
    uint32_t sample_rate;
    uint16_t channels;
    uint16_t bits_per_sample;
} wav_pcm_view_t;

static mmap_assets_handle_t     asset_audio;
static esp_codec_dev_handle_t   spk_codec_dev       = NULL;
static QueueHandle_t            g_queue_audio_play  = NULL;

/*******************************************************************************
* Private functions
*******************************************************************************/

static uint16_t read_le16(const uint8_t *data)
{
    return (uint16_t)data[0] | ((uint16_t)data[1] << 8);
}

static uint32_t read_le32(const uint8_t *data)
{
    return (uint32_t)data[0] |
           ((uint32_t)data[1] << 8) |
           ((uint32_t)data[2] << 16) |
           ((uint32_t)data[3] << 24);
}

static bool wav_get_pcm_view(const uint8_t *wav, size_t len, wav_pcm_view_t *out)
{
    if (wav == NULL || out == NULL || len < 12 ||
            memcmp(wav, "RIFF", 4) != 0 ||
            memcmp(wav + 8, "WAVE", 4) != 0) {
        return false;
    }

    bool has_fmt = false;
    bool has_data = false;
    uint16_t audio_format = 0;
    size_t offset = 12;

    memset(out, 0, sizeof(*out));

    while (offset + 8 <= len) {
        const uint8_t *chunk = wav + offset;
        uint32_t chunk_size = read_le32(chunk + 4);
        size_t chunk_data_offset = offset + 8;
        size_t next_offset = chunk_data_offset + chunk_size + (chunk_size & 1);

        if (chunk_data_offset + chunk_size > len || next_offset < offset) {
            return false;
        }

        if (memcmp(chunk, "fmt ", 4) == 0 && chunk_size >= 16) {
            audio_format = read_le16(wav + chunk_data_offset);
            out->channels = read_le16(wav + chunk_data_offset + 2);
            out->sample_rate = read_le32(wav + chunk_data_offset + 4);
            out->bits_per_sample = read_le16(wav + chunk_data_offset + 14);
            has_fmt = true;
        } else if (memcmp(chunk, "data", 4) == 0) {
            out->data = wav + chunk_data_offset;
            out->len = chunk_size;
            has_data = true;
        }

        offset = next_offset;
    }

    return has_fmt && has_data &&
           audio_format == 1 &&
           out->channels == 1 &&
           out->bits_per_sample == 16 &&
           out->sample_rate > 0 &&
           out->len > 0;
}

/* 鹿小班板的 I2S 引脚与官方 SparkBot 相反: DOUT=40, DIN=42 */
static esp_err_t audio_i2s_init(void)
{
    const i2s_std_config_t i2s_config = {
        .clk_cfg = I2S_STD_CLK_DEFAULT_CONFIG(16000),
        .slot_cfg = I2S_STD_PHILIP_SLOT_DEFAULT_CONFIG(I2S_DATA_BIT_WIDTH_16BIT, I2S_SLOT_MODE_MONO),
        .gpio_cfg = {
            .mclk = GPIO_NUM_45,
            .bclk = GPIO_NUM_39,
            .ws = GPIO_NUM_41,
            .dout = GPIO_NUM_40,
            .din = GPIO_NUM_42,
            .invert_flags = {
                .mclk_inv = false,
                .bclk_inv = false,
                .ws_inv = false,
            },
        },
    };

    ESP_LOGI(TAG, "Audio I2S pins: MCLK=45 BCLK=39 LRCK=41 DOUT=40 DIN=42");
    return bsp_audio_init(&i2s_config);
}

esp_err_t audio_play(const uint8_t *wav, size_t len)
{
    wav_pcm_view_t pcm_view = {0};
    if (!wav_get_pcm_view(wav, len, &pcm_view)) {
        ESP_LOGE(TAG, "Unsupported WAV. Need PCM, 16-bit, mono WAV.");
        return ESP_ERR_INVALID_ARG;
    }

    /* 直接引用 mmap 的 flash 数据, 不拷贝到堆(避免 60KB/387KB 的 malloc 失败) */
    audio_data_t audio_data = {
        .len = pcm_view.len,
        .pcm = pcm_view.data,
        .sample_rate = pcm_view.sample_rate,
        .channels = pcm_view.channels,
        .bits_per_sample = pcm_view.bits_per_sample,
    };

    if (xQueueSend(g_queue_audio_play, &audio_data, portMAX_DELAY) != pdTRUE) {
        return ESP_FAIL;
    }
    return ESP_OK;
}

void button_handler(touch_button_handle_t out_handle, touch_button_message_t *out_message, void *arg)
{
    (void) out_handle; //Unused
    lv_obj_t *current_screen = lv_disp_get_scr_act(NULL);
    int button = (int)arg;

    if (asset_audio == NULL) {
        return; // mmap 还没初始化, 启动瞬间触摸误触发时忽略
    }

    if (out_message->event == TOUCH_BUTTON_EVT_ON_PRESS) {
        // ESP_LOGI(TAG, "Button[%d] Press", (int)arg);
        if (button == 1) {
            void *audio = (void *)mmap_assets_get_mem(asset_audio, MMAP_AUDIO_YAY_WAV);
            uint32_t len = mmap_assets_get_size(asset_audio, MMAP_AUDIO_YAY_WAV);
            audio_play(audio, len);
        }
    } else if (out_message->event == TOUCH_BUTTON_EVT_ON_RELEASE) {
        // ESP_LOGI(TAG, "Button[%d] Release", (int)arg);
    } else if (out_message->event == TOUCH_BUTTON_EVT_ON_LONGPRESS) {
        // ESP_LOGI(TAG, "Button[%d] LongPress", (int)arg);
        if (button == 1) {
            void *audio = (void *)mmap_assets_get_mem(asset_audio, MMAP_AUDIO_CLAPPING_WAV);
            uint32_t len = mmap_assets_get_size(asset_audio, MMAP_AUDIO_CLAPPING_WAV);
            audio_play(audio, len);
        }
    }
}

static void button_long_press_cb(void *arg, void *usr_data)
{
    ESP_LOGI(TAG, "BUTTON_LONG_PRESS_START");
    nvs_flash_erase();
    esp_restart();
}

static void mmap_audio_init()
{
    const mmap_assets_config_t config = {
        .partition_label = "storage",
        .max_files = MMAP_AUDIO_FILES,
        .checksum = MMAP_AUDIO_CHECKSUM,
        .flags = {
            .mmap_enable = true,
            .app_bin_check = true,
        },
    };

    mmap_assets_new(&config, &asset_audio);
    ESP_LOGI(TAG, "stored_files:%d", mmap_assets_get_stored_files(asset_audio));
}

static void audio_play_task(void *arg)
{
    ESP_ERROR_CHECK(audio_i2s_init());
    spk_codec_dev = bsp_audio_codec_speaker_init();
    if (spk_codec_dev == NULL) {
        ESP_LOGE(TAG, "Speaker codec init failed");
        vTaskDelete(NULL);
        return;
    }

    ESP_LOGI(TAG, "esp_codec_dev_set_out_vol: %d", esp_codec_dev_set_out_vol(spk_codec_dev, 60));

    audio_data_t audio_data = {0};
    esp_codec_dev_sample_info_t opened_fs = {0};
    bool codec_opened = false;

    while (xQueueReceive(g_queue_audio_play, &audio_data, portMAX_DELAY) == pdTRUE) {
        esp_codec_dev_sample_info_t fs = {
            .sample_rate = audio_data.sample_rate,
            .channel = audio_data.channels,
            .bits_per_sample = audio_data.bits_per_sample,
        };

        if (!codec_opened ||
                opened_fs.sample_rate != fs.sample_rate ||
                opened_fs.channel != fs.channel ||
                opened_fs.bits_per_sample != fs.bits_per_sample) {
            if (codec_opened) {
                esp_codec_dev_close(spk_codec_dev);
            }

            int ret = esp_codec_dev_open(spk_codec_dev, &fs);
            if (ret != ESP_CODEC_DEV_OK) {
                ESP_LOGE(TAG, "esp_codec_dev_open failed: %d", ret);
                continue; // pcm 指向 mmap flash, 无需 free
            }

            opened_fs = fs;
            codec_opened = true;
            ESP_LOGI(TAG, "Opened codec: %"PRIu32" Hz, %u ch, %u bit",
                     fs.sample_rate, fs.channel, fs.bits_per_sample);
        }

        /* 分块从 mmap 拷贝到 RAM 再写: 只占 4KB, 避免 I2S DMA 直接读 flash */
        static uint8_t chunk[4096];
        const uint8_t *p = audio_data.pcm;
        size_t remaining = audio_data.len;
        int res = ESP_CODEC_DEV_OK;
        while (remaining > 0 && res == ESP_CODEC_DEV_OK) {
            size_t n = (remaining > sizeof(chunk)) ? sizeof(chunk) : remaining;
            memcpy(chunk, p, n);
            res = esp_codec_dev_write(spk_codec_dev, chunk, n);
            p += n;
            remaining -= n;
        }
        if (res != ESP_CODEC_DEV_OK) {
            ESP_LOGE(TAG, "esp_codec_dev_write failed: %d", res);
        }
        /* 无需 free: pcm 指向 mmap flash */
    }

    if (codec_opened) {
        esp_codec_dev_close(spk_codec_dev);
    }
    vTaskDelete(NULL);
}

// *INDENT-OFF*
void app_lvgl_display(void)
{
    bsp_display_lock(0);

    ui_init();

    bsp_display_unlock();
}

void app_main(void)
{
    /* Initialize I2C (for audio codec ES8311) */
    bsp_i2c_init();

    /* Create GPIO button */
    button_config_t gpio_btn_cfg = {
        .type = BUTTON_TYPE_GPIO,
        .long_press_time = CONFIG_BUTTON_LONG_PRESS_TIME_MS,
        .short_press_time = CONFIG_BUTTON_SHORT_PRESS_TIME_MS,
        .gpio_button_config = {
            .gpio_num = 0,
            .active_level = 0,
        },
    };
    button_handle_t gpio_btn = iot_button_create(&gpio_btn_cfg);

    if (NULL == gpio_btn) {
        ESP_LOGE(TAG, "Button create failed");
    }

    iot_button_register_cb(gpio_btn, BUTTON_LONG_PRESS_START, button_long_press_cb, NULL);

    /* Create touch button */
    bsp_touch_button_create(button_handler);

    /* Initialize display and LVGL */
    bsp_display_cfg_t custom_cfg = {
        .lvgl_port_cfg = ESP_LVGL_PORT_INIT_CONFIG(),
        .buffer_size = BSP_LCD_H_RES * 24,
        .trans_size = BSP_LCD_H_RES * 10, // in SRAM, DMA-capable
        .double_buffer = 0,
        .flags = {
            .buff_dma = false,
            .buff_spiram = false,
        }
    };
    custom_cfg.lvgl_port_cfg.task_stack = 1024 * 30;
    custom_cfg.lvgl_port_cfg.task_affinity = 1;
    bsp_display_start_with_config(&custom_cfg);

    /* Turn on display backlight */
    bsp_display_backlight_on();

    /* Add and show objects on display */
    app_lvgl_display();

    /* Start audio task */
    g_queue_audio_play = xQueueCreate(1, sizeof(audio_data_t));
    mmap_audio_init();
    xTaskCreate(audio_play_task, "audio_play_task", 1024 * 5, NULL, 15, NULL);

    /* Init network */
    app_wifi_init();

    ESP_LOGI(TAG, "Example initialization done.");
}
// *INDENT-ON*
