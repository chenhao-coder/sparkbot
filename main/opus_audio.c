/*
 * Opus decoder + playback path for SparkBot (ESP32-S3).
 *
 * Receives compressed Opus packets from the peer, decodes them to 16 kHz
 * mono PCM, and writes the samples to the codec via oai_audio_write().
 *
 * Replaces the legacy i2s_write() playback path from the original sparkbot
 * code. The codec itself (I2S + ES8311) is initialized in audio.c.
 */
#include <stdlib.h>
#include "esp_log.h"
#include "opus.h"
#include "audio.h"
#include "opus_audio.h"

static const char *TAG = "OPUS";

/* One 20 ms frame at 16 kHz mono => 320 samples. */
#define OPUS_DECODE_FRAME_SAMPLES (OAI_SAMPLE_RATE / 50)

static OpusDecoder *opus_decoder = NULL;
static opus_int16   *output_buffer = NULL;

void oai_init_audio_decoder(void)
{
    int err = 0;
    opus_decoder = opus_decoder_create(OAI_SAMPLE_RATE, 1, &err);
    if (err != OPUS_OK) {
        ESP_LOGE(TAG, "opus_decoder_create failed: %d", err);
        return;
    }

    output_buffer = (opus_int16 *)malloc(OPUS_DECODE_FRAME_SAMPLES * sizeof(opus_int16));
    if (output_buffer == NULL) {
        ESP_LOGE(TAG, "malloc for output_buffer failed");
        return;
    }

    ESP_LOGI(TAG, "opus decoder ready (%d Hz, 1ch)", OAI_SAMPLE_RATE);
}

void oai_audio_decode(uint8_t *data, size_t size)
{
    if (opus_decoder == NULL || output_buffer == NULL) {
        return;
    }

    int decoded = opus_decode(opus_decoder,
                              data, (opus_int32)size,
                              output_buffer, OPUS_DECODE_FRAME_SAMPLES,
                              0);
    if (decoded < 0) {
        ESP_LOGE(TAG, "opus_decode error: %d", decoded);
        return;
    }
    if (decoded > 0) {
        oai_audio_write(output_buffer, decoded * (int)sizeof(opus_int16));
    }
}
