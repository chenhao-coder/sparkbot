#pragma once

#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

/* Start the Doubao Seeduplex WebSocket session and audio worker tasks. */
esp_err_t doubao_realtime_start(void);

/* Stop the session and release its WebSocket/audio resources. */
esp_err_t doubao_realtime_stop(void);

#ifdef __cplusplus
}
#endif
