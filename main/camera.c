#include "camera.h"

#include <inttypes.h>

#include "driver/gpio.h"
#include "driver/i2c_master.h"
#include "driver/i2c_types.h"
#include "driver/pulse_cnt.h"
#include "esp_camera.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

static const char *TAG = "CAMERA";

/* ATK-OV2640-V1.2 pinout. The module has its own 12 MHz oscillator. */
#define CAMERA_PIN_RESET  15
#define CAMERA_PIN_PCLK   13
#define CAMERA_PIN_VSYNC   6
#define CAMERA_PIN_HREF    7
#define CAMERA_PIN_D0     11
#define CAMERA_PIN_D1      9
#define CAMERA_PIN_D2      8
#define CAMERA_PIN_D3     10
#define CAMERA_PIN_D4     12
#define CAMERA_PIN_D5     18
#define CAMERA_PIN_D6     17
#define CAMERA_PIN_D7     16

/* OV2640 DSP bank (bank 0), R_DVP_SP register. Keep auto mode and halve PCLK. */
#define OV2640_REG_R_DVP_SP       0x00D3
#define OV2640_PCLK_DIV_MASK      0x7F
#define OV2640_PCLK_DIV_SLOW      16
#define OV2640_REG_CLKRC          0x0111
#define OV2640_CLK_DIV_MASK       0x3F
#define OV2640_CLK_DIV_SLOW       7

#define CAMERA_INIT_MAX_ATTEMPTS   3
#define CAMERA_I2C_RETRY_DELAY_MS 100

static bool s_camera_initialized;

static void recover_camera_i2c_bus(int next_attempt)
{
    /* Hold the sensor quiet while the shared I2C0 controller clears the bus. */
    esp_err_t reset_pin_ret = gpio_set_level(CAMERA_PIN_RESET, 0);

    i2c_master_bus_handle_t bus_handle = NULL;
    esp_err_t ret = i2c_master_get_bus_handle(I2C_NUM_0, &bus_handle);
    if (ret == ESP_OK) {
        ret = i2c_master_bus_reset(bus_handle);
    }

    if (reset_pin_ret == ESP_OK && ret == ESP_OK) {
        ESP_LOGW(TAG,
                 "camera init failed; sensor reset and I2C bus recovered, retrying (%d/%d)",
                 next_attempt, CAMERA_INIT_MAX_ATTEMPTS);
    } else {
        ESP_LOGW(TAG,
                 "camera init failed; recovery incomplete (reset=%s, i2c=%s), retrying (%d/%d)",
                 esp_err_to_name(reset_pin_ret), esp_err_to_name(ret),
                 next_attempt, CAMERA_INIT_MAX_ATTEMPTS);
    }
    vTaskDelay(pdMS_TO_TICKS(CAMERA_I2C_RETRY_DELAY_MS));
}

static void camera_log_parallel_signal_edges(void)
{
    enum { SIGNAL_COUNT = 3 };
    static const int pins[SIGNAL_COUNT] = {
        CAMERA_PIN_PCLK,
        CAMERA_PIN_HREF,
        CAMERA_PIN_VSYNC,
    };
    static const char *const names[SIGNAL_COUNT] = {
        "PCLK",
        "HREF",
        "VSYNC",
    };

    pcnt_unit_handle_t units[SIGNAL_COUNT] = { NULL };
    pcnt_channel_handle_t channels[SIGNAL_COUNT] = { NULL };
    bool enabled[SIGNAL_COUNT] = { false };
    bool started[SIGNAL_COUNT] = { false };
    int counts[SIGNAL_COUNT] = { 0 };
    esp_err_t ret = ESP_OK;

    for (int i = 0; i < SIGNAL_COUNT; ++i) {
        const pcnt_unit_config_t unit_config = {
            .low_limit = -1,
            .high_limit = 30000,
        };
        ret = pcnt_new_unit(&unit_config, &units[i]);
        if (ret != ESP_OK) {
            ESP_LOGE(TAG, "PCNT unit for %s failed: %s", names[i], esp_err_to_name(ret));
            goto cleanup;
        }

        const pcnt_chan_config_t channel_config = {
            .edge_gpio_num = pins[i],
            .level_gpio_num = -1,
        };
        ret = pcnt_new_channel(units[i], &channel_config, &channels[i]);
        if (ret != ESP_OK) {
            ESP_LOGE(TAG, "PCNT channel for %s failed: %s", names[i], esp_err_to_name(ret));
            goto cleanup;
        }

        ret = pcnt_channel_set_edge_action(channels[i],
                                           PCNT_CHANNEL_EDGE_ACTION_INCREASE,
                                           PCNT_CHANNEL_EDGE_ACTION_HOLD);
        if (ret != ESP_OK) {
            ESP_LOGE(TAG, "PCNT edge action for %s failed: %s", names[i], esp_err_to_name(ret));
            goto cleanup;
        }

        ret = pcnt_channel_set_level_action(channels[i],
                                            PCNT_CHANNEL_LEVEL_ACTION_KEEP,
                                            PCNT_CHANNEL_LEVEL_ACTION_KEEP);
        if (ret != ESP_OK) {
            ESP_LOGE(TAG, "PCNT level action for %s failed: %s", names[i], esp_err_to_name(ret));
            goto cleanup;
        }

        ret = pcnt_unit_enable(units[i]);
        if (ret != ESP_OK) {
            ESP_LOGE(TAG, "PCNT enable for %s failed: %s", names[i], esp_err_to_name(ret));
            goto cleanup;
        }
        enabled[i] = true;

        ret = pcnt_unit_clear_count(units[i]);
        if (ret != ESP_OK) {
            ESP_LOGE(TAG, "PCNT clear for %s failed: %s", names[i], esp_err_to_name(ret));
            goto cleanup;
        }

        ret = pcnt_unit_start(units[i]);
        if (ret != ESP_OK) {
            ESP_LOGE(TAG, "PCNT start for %s failed: %s", names[i], esp_err_to_name(ret));
            goto cleanup;
        }
        started[i] = true;
    }

    vTaskDelay(pdMS_TO_TICKS(200));

    for (int i = 0; i < SIGNAL_COUNT; ++i) {
        pcnt_unit_stop(units[i]);
        started[i] = false;
        ret = pcnt_unit_get_count(units[i], &counts[i]);
        if (ret != ESP_OK) {
            ESP_LOGE(TAG, "PCNT read for %s failed: %s", names[i], esp_err_to_name(ret));
            goto cleanup;
        }
    }

    ESP_LOGI(TAG,
             "camera signal edges/200ms: PCLK(GPIO%d)=%d, HREF(GPIO%d)=%d, VSYNC(GPIO%d)=%d",
             CAMERA_PIN_PCLK, counts[0],
             CAMERA_PIN_HREF, counts[1],
             CAMERA_PIN_VSYNC, counts[2]);

cleanup:
    for (int i = SIGNAL_COUNT - 1; i >= 0; --i) {
        if (started[i]) {
            pcnt_unit_stop(units[i]);
        }
        if (enabled[i]) {
            pcnt_unit_disable(units[i]);
        }
        if (channels[i] != NULL) {
            pcnt_del_channel(channels[i]);
        }
        if (units[i] != NULL) {
            pcnt_del_unit(units[i]);
        }
    }
}

esp_err_t sparkbot_camera_hold_reset(void)
{
    const gpio_config_t reset_config = {
        .pin_bit_mask = 1ULL << CAMERA_PIN_RESET,
        .mode = GPIO_MODE_OUTPUT,
        .pull_up_en = GPIO_PULLUP_DISABLE,
        .pull_down_en = GPIO_PULLDOWN_DISABLE,
        .intr_type = GPIO_INTR_DISABLE,
    };

    esp_err_t ret = gpio_config(&reset_config);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "failed to configure reset GPIO: %s", esp_err_to_name(ret));
        return ret;
    }

    ret = gpio_set_level(CAMERA_PIN_RESET, 0);
    if (ret == ESP_OK) {
        ESP_LOGI(TAG, "camera held in reset on GPIO%d", CAMERA_PIN_RESET);
    }
    return ret;
}

esp_err_t sparkbot_camera_init(void)
{
    if (s_camera_initialized) {
        return ESP_OK;
    }

    /*
     * GPIO4/GPIO5 and I2C0 are initialized by audio.c before this function.
     * Setting the SCCB pins to -1 makes esp32-camera reuse that existing bus.
     */
    const camera_config_t config = {
        /* PWDN is held low externally; GPIO15 drives the active-low reset. */
        .pin_pwdn = -1,
        .pin_reset = CAMERA_PIN_RESET,
        .pin_xclk = -1,
        .pin_sccb_sda = -1,
        .pin_sccb_scl = -1,
        .pin_d7 = CAMERA_PIN_D7,
        .pin_d6 = CAMERA_PIN_D6,
        .pin_d5 = CAMERA_PIN_D5,
        .pin_d4 = CAMERA_PIN_D4,
        .pin_d3 = CAMERA_PIN_D3,
        .pin_d2 = CAMERA_PIN_D2,
        .pin_d1 = CAMERA_PIN_D1,
        .pin_d0 = CAMERA_PIN_D0,
        .pin_vsync = CAMERA_PIN_VSYNC,
        .pin_href = CAMERA_PIN_HREF,
        .pin_pclk = CAMERA_PIN_PCLK,
        /* Tell the driver the module's actual onboard clock frequency. */
        .xclk_freq_hz = 12000000,
        .ledc_timer = LEDC_TIMER_0,
        .ledc_channel = LEDC_CHANNEL_0,
        .pixel_format = PIXFORMAT_RGB565,
        .frame_size = FRAMESIZE_240X240,
        .jpeg_quality = 12,
        /* RGB565 uses single-buffer capture; continuous multi-buffer mode is intended for JPEG. */
        .fb_count = 1,
        .fb_location = CAMERA_FB_IN_PSRAM,
        .grab_mode = CAMERA_GRAB_WHEN_EMPTY,
        .sccb_i2c_port = I2C_NUM_0,
    };

    ESP_LOGI(TAG, "initializing ATK-OV2640 (RGB565 240x240, onboard XCLK 12 MHz)");
    esp_err_t ret = ESP_FAIL;
    for (int attempt = 1; attempt <= CAMERA_INIT_MAX_ATTEMPTS; ++attempt) {
        ret = esp_camera_init(&config);
        if (ret == ESP_OK) {
            if (attempt > 1) {
                ESP_LOGI(TAG, "camera initialized on attempt %d/%d",
                         attempt, CAMERA_INIT_MAX_ATTEMPTS);
            }
            break;
        }
        ESP_LOGW(TAG, "esp_camera_init attempt %d/%d failed: %s (0x%x)",
                 attempt, CAMERA_INIT_MAX_ATTEMPTS,
                 esp_err_to_name(ret), (unsigned int)ret);
        if (attempt < CAMERA_INIT_MAX_ATTEMPTS) {
            recover_camera_i2c_bus(attempt + 1);
        }
    }
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "esp_camera_init failed after %d attempts: %s (0x%x)",
                 CAMERA_INIT_MAX_ATTEMPTS,
                 esp_err_to_name(ret), (unsigned int)ret);
        return ret;
    }

    sensor_t *sensor = esp_camera_sensor_get();
    if (sensor == NULL) {
        ESP_LOGE(TAG, "camera initialized but sensor handle is unavailable");
        esp_camera_deinit();
        return ESP_FAIL;
    }

    /* Match the orientation used by the SparkBot YOLO example. */
    sensor->set_vflip(sensor, 1);
    sensor->set_hmirror(sensor, 0);

    /*
     * ATK-OV2640-V1.2 has a fixed onboard 12 MHz oscillator. Slow its DVP
     * output through R_DVP_SP instead of pretending that XCLK is adjustable.
     */
    const int old_dvp_sp = sensor->get_reg(sensor, OV2640_REG_R_DVP_SP, 0xFF);
    const int reg_ret = sensor->set_reg(sensor,
                                        OV2640_REG_R_DVP_SP,
                                        OV2640_PCLK_DIV_MASK,
                                        OV2640_PCLK_DIV_SLOW);
    const int new_dvp_sp = sensor->get_reg(sensor, OV2640_REG_R_DVP_SP, 0xFF);
    if (old_dvp_sp < 0 || reg_ret != 0 || new_dvp_sp < 0) {
        ESP_LOGE(TAG,
                 "failed to configure OV2640 PCLK divider (old=%d, set=%d, new=%d)",
                 old_dvp_sp, reg_ret, new_dvp_sp);
        esp_camera_deinit();
        return ESP_FAIL;
    }
    ESP_LOGI(TAG,
             "OV2640 R_DVP_SP: 0x%02x -> 0x%02x (PCLK divider=%d)",
             old_dvp_sp, new_dvp_sp, OV2640_PCLK_DIV_SLOW);

    const int old_clkrc = sensor->get_reg(sensor, OV2640_REG_CLKRC, 0xFF);
    const int clkrc_ret = sensor->set_reg(sensor,
                                          OV2640_REG_CLKRC,
                                          OV2640_CLK_DIV_MASK,
                                          OV2640_CLK_DIV_SLOW);
    const int new_clkrc = sensor->get_reg(sensor, OV2640_REG_CLKRC, 0xFF);
    if (old_clkrc < 0 || clkrc_ret != 0 || new_clkrc < 0) {
        ESP_LOGE(TAG,
                 "failed to configure OV2640 CLKRC (old=%d, set=%d, new=%d)",
                 old_clkrc, clkrc_ret, new_clkrc);
        esp_camera_deinit();
        return ESP_FAIL;
    }
    ESP_LOGI(TAG,
             "OV2640 CLKRC: 0x%02x -> 0x%02x (clock divider=%d)",
             old_clkrc, new_clkrc, OV2640_CLK_DIV_SLOW);
    vTaskDelay(pdMS_TO_TICKS(20));

    s_camera_initialized = true;
    ESP_LOGI(TAG, "sensor ready: PID=0x%04x", sensor->id.PID);
    return ESP_OK;
}

esp_err_t sparkbot_camera_capture_probe(void)
{
    if (!s_camera_initialized) {
        return ESP_ERR_INVALID_STATE;
    }

    camera_fb_t *frame = esp_camera_fb_get();
    if (frame == NULL) {
        ESP_LOGE(TAG, "failed to acquire camera frame");
        esp_camera_deinit();
        s_camera_initialized = false;
        ESP_LOGI(TAG, "camera stopped after capture failure");
        camera_log_parallel_signal_edges();
        return ESP_FAIL;
    }

    /* FNV-1a makes it easy to confirm that real frame data was received. */
    uint32_t checksum = UINT32_C(2166136261);
    for (size_t i = 0; i < frame->len; ++i) {
        checksum ^= frame->buf[i];
        checksum *= UINT32_C(16777619);
    }

    const bool valid = frame->width == 240 &&
                       frame->height == 240 &&
                       frame->format == PIXFORMAT_RGB565 &&
                       frame->len == 240U * 240U * 2U;

    ESP_LOGI(TAG,
             "frame: %ux%u, len=%u, format=%d, checksum=0x%08" PRIx32,
             (unsigned int)frame->width,
             (unsigned int)frame->height,
             (unsigned int)frame->len,
             (int)frame->format,
             checksum);

    esp_camera_fb_return(frame);

    if (!valid) {
        ESP_LOGE(TAG, "unexpected frame metadata (expected RGB565 240x240/115200 bytes)");
        esp_camera_deinit();
        s_camera_initialized = false;
        ESP_LOGI(TAG, "camera stopped after invalid frame");
        return ESP_ERR_INVALID_SIZE;
    }

    ESP_LOGI(TAG, "camera capture probe passed");
    return ESP_OK;
}
