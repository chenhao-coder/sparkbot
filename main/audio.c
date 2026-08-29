/*
 * ES8311 audio codec + I2S initialization for SparkBot (ESP32-S3).
 *
 * Uses the IDF 6 new drivers (i2c_master / i2s_std) together with the
 * esp_codec_dev framework. The legacy I2S/I2C API has been removed in IDF 6.
 *
 * Board wiring (ES8311 codec):
 *   SDA  -> GPIO4   (I2C data)
 *   SCL  -> GPIO5   (I2C clock)
 *   MCLK -> GPIO45  (I2S master clock output)
 *   BCLK -> GPIO39  (I2S bit clock)
 *   WS   -> GPIO41  (I2S word select / LRCK)
 *   DOUT -> GPIO40  (ESP32 data out -> module DIN, playback)
 *   DIN  -> GPIO42  (module DOUT -> ESP32 data in, capture)
 *
 * DOUT/DIN below are named from the ESP32 perspective. The Luxiaoban
 * ES8311 module labels the same signals from the codec perspective.
 */
#include <string.h>
#include "esp_log.h"
#include "driver/gpio.h"
#include "driver/i2c_master.h"
#include "driver/i2s_std.h"
#include "esp_codec_dev.h"
#include "esp_codec_dev_defaults.h"
#include "audio.h"

static const char *TAG = "AUDIO";

#define I2S_PORT         I2S_NUM_0
#define I2C_PORT         I2C_NUM_0
#define I2C_CLOCK_HZ     100000

#define I2C_SDA_PIN      4
#define I2C_SCL_PIN      5

#define MCLK_PIN         45
#define BCLK_PIN         39
#define WS_PIN           41
#define DOUT_PIN         40
#define DIN_PIN          42

static esp_codec_dev_handle_t  s_codec_dev = NULL;
static i2c_master_bus_handle_t s_i2c_bus   = NULL;
static i2s_chan_handle_t       s_tx_chan   = NULL;
static i2s_chan_handle_t       s_rx_chan   = NULL;

static esp_err_t detect_es8311(uint8_t *codec_addr)
{
    static const uint8_t candidates[] = {0x18, 0x19};

    for (size_t i = 0; i < sizeof(candidates); ++i) {
        uint8_t addr = candidates[i];
        esp_err_t ret = i2c_master_probe(s_i2c_bus, addr, 50);
        if (ret == ESP_OK) {
            /* esp_codec_dev uses the legacy 8-bit I2C address form. */
            *codec_addr = (uint8_t)(addr << 1);
            ESP_LOGI(TAG, "ES8311 detected at 7-bit I2C address 0x%02x", addr);
            return ESP_OK;
        }

        ESP_LOGW(TAG, "no I2C response at 0x%02x: %s",
                 addr, esp_err_to_name(ret));
    }

    ESP_LOGE(TAG, "ES8311 not found; SDA(GPIO%d)=%d, SCL(GPIO%d)=%d",
             I2C_SDA_PIN, gpio_get_level(I2C_SDA_PIN),
             I2C_SCL_PIN, gpio_get_level(I2C_SCL_PIN));
    return ESP_ERR_NOT_FOUND;
}

esp_err_t oai_init_audio_capture(void)
{
    esp_err_t ret;
    uint8_t codec_i2c_addr = 0;

    /* 1. I2C master bus for ES8311 register control */
    i2c_master_bus_config_t i2c_bus_cfg = {
        .clk_source = I2C_CLK_SRC_DEFAULT,
        .i2c_port   = I2C_PORT,
        .scl_io_num = I2C_SCL_PIN,
        .sda_io_num = I2C_SDA_PIN,
        .glitch_ignore_cnt = 7,
        .flags.enable_internal_pullup = true,
    };
    ret = i2c_new_master_bus(&i2c_bus_cfg, &s_i2c_bus);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "i2c_new_master_bus failed: %s", esp_err_to_name(ret));
        return ret;
    }
    ret = detect_es8311(&codec_i2c_addr);
    if (ret != ESP_OK) {
        return ret;
    }

    /* 2. I2S channel (TX playback + RX capture), ESP32 as master */
    i2s_chan_config_t chan_cfg = I2S_CHANNEL_DEFAULT_CONFIG(I2S_PORT, I2S_ROLE_MASTER);
    chan_cfg.auto_clear = true;
    ret = i2s_new_channel(&chan_cfg, &s_tx_chan, &s_rx_chan);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "i2s_new_channel failed: %s", esp_err_to_name(ret));
        return ret;
    }

    i2s_std_config_t std_cfg = {
        .clk_cfg  = I2S_STD_CLK_DEFAULT_CONFIG(AUDIO_CODEC_SAMPLE_RATE),
        .slot_cfg = I2S_STD_PHILIPS_SLOT_DEFAULT_CONFIG(I2S_DATA_BIT_WIDTH_16BIT, I2S_SLOT_MODE_MONO),
        .gpio_cfg = {
            .mclk = MCLK_PIN,
            .bclk = BCLK_PIN,
            .ws   = WS_PIN,
            .dout = DOUT_PIN,
            .din  = DIN_PIN,
            .invert_flags = {
                .mclk_inv = false,
                .bclk_inv = false,
                .ws_inv   = false,
            },
        },
    };
    ESP_LOGI(TAG, "I2S pins: MCLK=%d BCLK=%d LRCK=%d DOUT=%d DIN=%d",
             MCLK_PIN, BCLK_PIN, WS_PIN, DOUT_PIN, DIN_PIN);
    ret = i2s_channel_init_std_mode(s_tx_chan, &std_cfg);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "i2s_channel_init_std_mode(tx) failed: %s", esp_err_to_name(ret));
        return ret;
    }
    ret = i2s_channel_init_std_mode(s_rx_chan, &std_cfg);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "i2s_channel_init_std_mode(rx) failed: %s", esp_err_to_name(ret));
        return ret;
    }
    /* TX drives the shared clock in full-duplex mode */
    ret = i2s_channel_enable(s_tx_chan);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "i2s_channel_enable(tx) failed: %s", esp_err_to_name(ret));
        return ret;
    }
    ret = i2s_channel_enable(s_rx_chan);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "i2s_channel_enable(rx) failed: %s", esp_err_to_name(ret));
        return ret;
    }

    /* 3. I2S data interface for esp_codec_dev */
    audio_codec_i2s_cfg_t i2s_cfg = {
        .rx_handle = s_rx_chan,
        .tx_handle = s_tx_chan,
    };
    const audio_codec_data_if_t *data_if = audio_codec_new_i2s_data(&i2s_cfg);
    if (data_if == NULL) {
        ESP_LOGE(TAG, "audio_codec_new_i2s_data failed");
        return ESP_ERR_NO_MEM;
    }

    /* 4. I2C control interface */
    audio_codec_i2c_cfg_t i2c_ctrl_cfg = {
        .addr           = codec_i2c_addr,
        .bus_handle     = s_i2c_bus,
        .clock_speed_hz = I2C_CLOCK_HZ,
    };
    const audio_codec_ctrl_if_t *ctrl_if = audio_codec_new_i2c_ctrl(&i2c_ctrl_cfg);
    if (ctrl_if == NULL) {
        ESP_LOGE(TAG, "audio_codec_new_i2c_ctrl failed");
        return ESP_ERR_NO_MEM;
    }

    /* 5. GPIO interface (PA enable etc.) */
    const audio_codec_gpio_if_t *gpio_if = audio_codec_new_gpio();
    if (gpio_if == NULL) {
        ESP_LOGE(TAG, "audio_codec_new_gpio failed");
        return ESP_ERR_NO_MEM;
    }

    /* 6. ES8311 codec interface (input + output) */
    es8311_codec_cfg_t es8311_cfg = {
        .codec_mode  = ESP_CODEC_DEV_WORK_MODE_BOTH,
        .ctrl_if     = ctrl_if,
        .gpio_if     = gpio_if,
        .pa_pin      = -1,     /* no dedicated PA enable pin */
        .pa_reverted = false,
        .master_mode = false,  /* ESP32 is I2S master, codec is slave */
        .use_mclk    = true,   /* MCLK supplied by I2S on GPIO45 */
        .digital_mic = false,  /* analog microphone */
        .invert_mclk = false,
        .invert_sclk = false,
        .hw_gain = {
            .pa_voltage        = 5.0,
            .codec_dac_voltage = 3.3,
        },
        .mclk_div    = 256,
    };
    const audio_codec_if_t *codec_if = es8311_codec_new(&es8311_cfg);
    if (codec_if == NULL) {
        ESP_LOGE(TAG, "es8311_codec_new failed");
        return ESP_FAIL;
    }

    /* 7. Codec device */
    esp_codec_dev_cfg_t dev_cfg = {
        .dev_type = ESP_CODEC_DEV_TYPE_IN_OUT,
        .codec_if = codec_if,
        .data_if  = data_if,
    };
    s_codec_dev = esp_codec_dev_new(&dev_cfg);
    if (s_codec_dev == NULL) {
        ESP_LOGE(TAG, "esp_codec_dev_new failed");
        return ESP_ERR_NO_MEM;
    }

    /* 8. Open codec with sample config (24 kHz mono 16-bit) */
    esp_codec_dev_sample_info_t fs = {
        .sample_rate     = AUDIO_CODEC_SAMPLE_RATE,
        .channel         = 1,
        .bits_per_sample = 16,
    };
    int rc = esp_codec_dev_open(s_codec_dev, &fs);
    if (rc != ESP_CODEC_DEV_OK) {
        ESP_LOGE(TAG, "esp_codec_dev_open failed: %d", rc);
        return ESP_FAIL;
    }

    /* 9. Output volume (0..100) and microphone gain (dB) */
    esp_codec_dev_set_out_vol(s_codec_dev, 60);
    esp_codec_dev_set_in_gain(s_codec_dev, 30.0);

    ESP_LOGI(TAG, "audio codec ready (sample_rate=%d, 1ch, 16bit)",
             AUDIO_CODEC_SAMPLE_RATE);
    return ESP_OK;
}

int oai_audio_read(void *data, int len)
{
    if (s_codec_dev == NULL) {
        return ESP_ERR_INVALID_STATE;
    }
    return esp_codec_dev_read(s_codec_dev, data, len);
}

int oai_audio_write(void *data, int len)
{
    if (s_codec_dev == NULL) {
        return ESP_ERR_INVALID_STATE;
    }
    return esp_codec_dev_write(s_codec_dev, data, len);
}
