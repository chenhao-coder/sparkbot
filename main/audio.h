#pragma once

#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

/*
 * ES8311 capture and playback share one I2S clock.  The Doubao full-duplex
 * endpoint returns 24 kHz PCM, so the codec runs at 24 kHz in both directions.
 * Microphone audio is resampled to 16 kHz before it is sent to Doubao.
 */
#define AUDIO_CODEC_SAMPLE_RATE 24000
#define DOUBAO_INPUT_SAMPLE_RATE 16000

/*
 * Initialize ES8311 codec (I2C) + I2S (new IDF 6 driver) via esp_codec_dev.
 * Configures both capture (mic) and playback (speaker).
 */
esp_err_t oai_init_audio_capture(void);

/*
 * Read captured audio samples (blocking until `len` bytes are filled).
 * Returns ESP_CODEC_DEV_OK (0) on success, otherwise a negative error code.
 */
int oai_audio_read(void *data, int len);

/*
 * Write samples to the speaker.
 * Returns ESP_CODEC_DEV_OK (0) on success, otherwise a negative error code.
 */
int oai_audio_write(void *data, int len);

#ifdef __cplusplus
}
#endif
