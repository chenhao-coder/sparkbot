#pragma once

#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

/** Initialize the SparkBot OV2640 and keep it ready for later inference. */
esp_err_t sparkbot_camera_init(void);

/** Hold the camera in hardware reset while other shared-I2C devices start. */
esp_err_t sparkbot_camera_hold_reset(void);

/** Capture and validate one RGB565 240x240 frame. */
esp_err_t sparkbot_camera_capture_probe(void);

#ifdef __cplusplus
}
#endif
