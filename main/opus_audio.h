#pragma once

#ifdef __cplusplus
extern "C" {
#endif

#include <stddef.h>
#include <stdint.h>

/*
 * Initialize the Opus decoder used for incoming voice playback.
 * Must be called after oai_init_audio_capture().
 * Decodes 16 kHz mono Opus packets (matching OAI_SAMPLE_RATE in audio.h).
 */
void oai_init_audio_decoder(void);

/*
 * Decode one Opus packet and play it out through the codec.
 * `data`/`size` is a single compressed Opus frame received from the peer.
 */
void oai_audio_decode(uint8_t *data, size_t size);

#ifdef __cplusplus
}
#endif
