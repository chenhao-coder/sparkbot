#pragma once

#ifdef __cplusplus
extern "C" {
#endif

/* Audio sample rate used by the whole voice pipeline (capture + playback) */
#define OAI_SAMPLE_RATE 16000

/*
 * Initialize ES8311 codec (I2C) + I2S (new IDF 6 driver) via esp_codec_dev.
 * Configures both capture (mic) and playback (speaker).
 */
void oai_init_audio_capture(void);

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
