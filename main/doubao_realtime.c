/*
 * Doubao Realtime Voice 3.0 (Seeduplex) full-duplex client.
 *
 * Protocol:
 *   wss://openspeech.bytedance.com/api/v3/duplex/realtime/dialogue
 *   WebSocket text frames containing Realtime JSON events
 *
 * Audio:
 *   microphone: ES8311 24 kHz PCM -> 16 kHz PCM -> Base64 JSON
 *   speaker:    Base64 JSON -> 24 kHz signed 16-bit little-endian PCM -> ES8311
 */
#include "doubao_realtime.h"

#include <inttypes.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>

#include "audio.h"
#include "cJSON.h"
#include "doubao_secrets.h"
#include "esp_crt_bundle.h"
#include "esp_heap_caps.h"
#include "esp_log.h"
#include "esp_random.h"
#include "esp_websocket_client.h"
#include "freertos/FreeRTOS.h"
#include "freertos/event_groups.h"
#include "freertos/queue.h"
#include "freertos/task.h"
#include "mbedtls/base64.h"

static const char *TAG = "DOUBAO";

#define DOUBAO_WS_URI "wss://openspeech.bytedance.com/api/v3/duplex/realtime/dialogue"
#define DOUBAO_MODEL_ID "1.2.6.1"
#define DOUBAO_INPUT_AUDIO_FORMAT "pcm"
#define DOUBAO_OUTPUT_AUDIO_FORMAT "pcm_s16le"

#define DOUBAO_FRAME_MS 20
#define CODEC_FRAME_SAMPLES (AUDIO_CODEC_SAMPLE_RATE * DOUBAO_FRAME_MS / 1000)
#define DOUBAO_INPUT_FRAME_SAMPLES (DOUBAO_INPUT_SAMPLE_RATE * DOUBAO_FRAME_MS / 1000)
#define CODEC_FRAME_BYTES (CODEC_FRAME_SAMPLES * sizeof(int16_t))
#define DOUBAO_INPUT_FRAME_BYTES (DOUBAO_INPUT_FRAME_SAMPLES * sizeof(int16_t))
#define DOUBAO_INPUT_BASE64_SIZE ((((DOUBAO_INPUT_FRAME_BYTES) + 2) / 3) * 4 + 1)
#define DOUBAO_UPLINK_JSON_SIZE (DOUBAO_INPUT_BASE64_SIZE + 160)

#define DOUBAO_WS_BUFFER_SIZE 8192
#define DOUBAO_MAX_MESSAGE_SIZE (64 * 1024)
#define DOUBAO_PLAYBACK_QUEUE_LENGTH 8
#define DOUBAO_I2S_WRITE_CHUNK_BYTES \
    (AUDIO_CODEC_SAMPLE_RATE * sizeof(int16_t) * DOUBAO_FRAME_MS / 1000)
#define DOUBAO_SEND_TIMEOUT_MS 2000
#define DOUBAO_SESSION_TIMEOUT_MS 10000
#define DOUBAO_TASK_CORE 0

#define DOUBAO_CONNECTED_BIT BIT0
#define DOUBAO_SESSION_SENT_BIT BIT1
#define DOUBAO_SESSION_READY_BIT BIT2

typedef struct {
    uint8_t *data;
    size_t length;
} playback_chunk_t;

static EventGroupHandle_t s_state = NULL;
static QueueHandle_t s_playback_queue = NULL;
static TaskHandle_t s_uplink_task = NULL;
static TaskHandle_t s_playback_task = NULL;
static esp_websocket_client_handle_t s_client = NULL;

static char *s_rx_message = NULL;
static size_t s_rx_capacity = 0;
static size_t s_rx_expected = 0;
static size_t s_rx_received = 0;

/* WebSocket audio deltas may split a 16-bit PCM sample between two events. */
static bool s_output_pcm_has_carry = false;
static uint8_t s_output_pcm_carry = 0;
static uint32_t s_output_pcm_odd_delta_count = 0;
static bool s_output_pcm_info_logged = false;

static const char s_auth_headers[] = "X-Api-Key: " DOUBAO_API_KEY "\r\n";

static void *allocate_audio_memory(size_t size) {
    void *memory = heap_caps_malloc(size, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    if (memory == NULL) {
        memory = malloc(size);
    }
    return memory;
}

static void clear_playback_queue(void) {
    if (s_playback_queue == NULL) {
        return;
    }

    playback_chunk_t chunk;
    while (xQueueReceive(s_playback_queue, &chunk, 0) == pdTRUE) {
        free(chunk.data);
    }
}

static void reset_output_pcm_stream(void) {
    s_output_pcm_has_carry = false;
    s_output_pcm_carry = 0;
    s_output_pcm_odd_delta_count = 0;
    s_output_pcm_info_logged = false;
}

static void finish_output_pcm_stream(void) {
    if (s_output_pcm_has_carry) {
        ESP_LOGW(TAG, "output PCM ended with an incomplete 16-bit sample; one byte dropped");
    }
    if (s_output_pcm_odd_delta_count > 0) {
        ESP_LOGI(TAG, "reassembled %" PRIu32 " PCM deltas split on an odd-byte boundary",
                 s_output_pcm_odd_delta_count);
    }
    s_output_pcm_has_carry = false;
    s_output_pcm_carry = 0;
}

static void log_output_pcm_info(const uint8_t *data, size_t length) {
    if (s_output_pcm_info_logged || length < sizeof(int16_t)) {
        return;
    }
    s_output_pcm_info_logged = true;

    int16_t minimum = INT16_MAX;
    int16_t maximum = INT16_MIN;
    uint64_t absolute_sum = 0;
    size_t sample_count = length / sizeof(int16_t);
    for (size_t index = 0; index < sample_count; ++index) {
        int16_t sample;
        memcpy(&sample, data + index * sizeof(sample), sizeof(sample));
        if (sample < minimum) {
            minimum = sample;
        }
        if (sample > maximum) {
            maximum = sample;
        }
        absolute_sum += sample < 0 ? (uint32_t)(-(int32_t)sample) : (uint32_t)sample;
    }

    ESP_LOGI(TAG,
             "output PCM S16LE first delta: %u bytes, head=%02x %02x %02x %02x, "
             "min=%d max=%d mean_abs=%u",
             (unsigned)length, data[0], data[1], length > 2 ? data[2] : 0,
             length > 3 ? data[3] : 0, minimum, maximum,
             (unsigned)(absolute_sum / sample_count));

    if ((length >= 12 && memcmp(data, "RIFF", 4) == 0 && memcmp(data + 8, "WAVE", 4) == 0) ||
        (length >= 4 && memcmp(data, "OggS", 4) == 0) ||
        (length >= 3 && memcmp(data, "ID3", 3) == 0)) {
        ESP_LOGE(TAG, "output audio has a container/compressed header, but playback expects raw PCM");
    }
}

static const char *json_string(const cJSON *object, const char *name) {
    const cJSON *item = cJSON_GetObjectItemCaseSensitive(object, name);
    return cJSON_IsString(item) ? item->valuestring : NULL;
}

static esp_err_t websocket_send_text(const char *payload) {
    if (s_client == NULL || !esp_websocket_client_is_connected(s_client)) {
        return ESP_ERR_INVALID_STATE;
    }

    int length = (int)strlen(payload);
    int sent = esp_websocket_client_send_text(s_client, payload, length,
                                              pdMS_TO_TICKS(DOUBAO_SEND_TIMEOUT_MS));
    return sent == length ? ESP_OK : ESP_FAIL;
}

static esp_err_t send_session_create(void) {
    esp_err_t result = ESP_ERR_NO_MEM;
    cJSON *root = cJSON_CreateObject();
    if (root == NULL) {
        return result;
    }

    cJSON *session = cJSON_AddObjectToObject(root, "session");
    cJSON *audio = session == NULL ? NULL : cJSON_AddObjectToObject(session, "audio");
    cJSON *input = audio == NULL ? NULL : cJSON_AddObjectToObject(audio, "input");
    cJSON *input_format = input == NULL ? NULL : cJSON_AddObjectToObject(input, "format");
    cJSON *output = audio == NULL ? NULL : cJSON_AddObjectToObject(audio, "output");
    cJSON *output_format = output == NULL ? NULL : cJSON_AddObjectToObject(output, "format");
    cJSON *extension = cJSON_AddObjectToObject(root, "extension");

    if (session == NULL || audio == NULL || input == NULL || input_format == NULL ||
        output == NULL || output_format == NULL || extension == NULL) {
        cJSON_Delete(root);
        return result;
    }

    cJSON_AddStringToObject(root, "type", "session.create");
    cJSON_AddStringToObject(session, "model", DOUBAO_MODEL_ID);
    cJSON_AddStringToObject(session, "instructions", DOUBAO_SYSTEM_INSTRUCTIONS);

    cJSON_AddStringToObject(input_format, "type", DOUBAO_INPUT_AUDIO_FORMAT);
    cJSON_AddNumberToObject(input_format, "rate", DOUBAO_INPUT_SAMPLE_RATE);

    cJSON_AddStringToObject(output_format, "type", DOUBAO_OUTPUT_AUDIO_FORMAT);
    cJSON_AddNumberToObject(output_format, "rate", AUDIO_CODEC_SAMPLE_RATE);
    cJSON_AddNumberToObject(output, "speed", 0);
    cJSON_AddNumberToObject(output, "loudness", 0);
    cJSON_AddStringToObject(output, "voice", DOUBAO_VOICE_ID);

    cJSON_AddObjectToObject(extension, "asr");
    cJSON_AddObjectToObject(extension, "tts");
    cJSON_AddObjectToObject(extension, "dialog");

    char *payload = cJSON_PrintUnformatted(root);
    if (payload != NULL) {
        result = websocket_send_text(payload);
        cJSON_free(payload);
    }
    cJSON_Delete(root);

    if (result == ESP_OK) {
        ESP_LOGI(TAG, "session.create sent (model=%s, voice=%s, output=%s/%dHz)",
                 DOUBAO_MODEL_ID, DOUBAO_VOICE_ID, DOUBAO_OUTPUT_AUDIO_FORMAT,
                 AUDIO_CODEC_SAMPLE_RATE);
    }
    return result;
}

static void enqueue_audio_delta(const char *base64_audio) {
    size_t base64_length = strlen(base64_audio);
    size_t decoded_capacity = 0;
    int rc = mbedtls_base64_decode(NULL, 0, &decoded_capacity, (const unsigned char *)base64_audio,
                                   base64_length);
    if (rc != MBEDTLS_ERR_BASE64_BUFFER_TOO_SMALL || decoded_capacity == 0) {
        ESP_LOGW(TAG, "invalid output audio Base64: rc=%d", rc);
        return;
    }

    size_t prefix_length = s_output_pcm_has_carry ? 1 : 0;
    uint8_t *decoded = allocate_audio_memory(decoded_capacity + prefix_length);
    if (decoded == NULL) {
        ESP_LOGE(TAG, "no memory for %u-byte audio delta", (unsigned)decoded_capacity);
        return;
    }

    if (prefix_length != 0) {
        decoded[0] = s_output_pcm_carry;
    }

    size_t decoded_length = 0;
    rc = mbedtls_base64_decode(decoded + prefix_length, decoded_capacity, &decoded_length,
                               (const unsigned char *)base64_audio, base64_length);
    if (rc != 0 || decoded_length == 0) {
        ESP_LOGW(TAG, "output audio Base64 decode failed: rc=%d", rc);
        free(decoded);
        s_output_pcm_has_carry = false;
        return;
    }

    decoded_length += prefix_length;
    s_output_pcm_has_carry = false;
    if ((decoded_length & 1U) != 0) {
        s_output_pcm_carry = decoded[decoded_length - 1];
        s_output_pcm_has_carry = true;
        ++s_output_pcm_odd_delta_count;
        --decoded_length;
    }
    if (decoded_length == 0) {
        free(decoded);
        return;
    }

    log_output_pcm_info(decoded, decoded_length);
    playback_chunk_t chunk = {
        .data = decoded,
        .length = decoded_length,
    };

    if (xQueueSend(s_playback_queue, &chunk, 0) == pdTRUE) {
        return;
    }

    /* Keep latency bounded: discard the oldest queued audio if necessary. */
    playback_chunk_t stale;
    if (xQueueReceive(s_playback_queue, &stale, 0) == pdTRUE) {
        free(stale.data);
    }
    if (xQueueSend(s_playback_queue, &chunk, 0) != pdTRUE) {
        free(chunk.data);
        ESP_LOGW(TAG, "playback queue full; audio delta dropped");
    }
}

static void handle_server_message(const char *message, size_t length) {
    cJSON *root = cJSON_ParseWithLength(message, length);
    if (root == NULL) {
        ESP_LOGW(TAG, "invalid server JSON (%u bytes)", (unsigned)length);
        return;
    }

    const char *type = json_string(root, "type");
    if (type == NULL) {
        cJSON_Delete(root);
        return;
    }

    if (strcmp(type, "session.created") == 0) {
        xEventGroupSetBits(s_state, DOUBAO_SESSION_READY_BIT);
        ESP_LOGI(TAG, "Doubao session ready");
    } else if (strcmp(type, "conversation.item.input_audio_transcription.completed") == 0) {
        const char *transcript = json_string(root, "transcript");
        if (transcript != NULL) {
            ESP_LOGI(TAG, "user: %s", transcript);
        }
    } else if (strcmp(type, "response.output_text.done") == 0) {
        const char *text = json_string(root, "text");
        if (text != NULL) {
            ESP_LOGI(TAG, "assistant: %s", text);
        }
    } else if (strcmp(type, "response.output_audio.started") == 0 ||
               strcmp(type, "response.audio.started") == 0) {
        clear_playback_queue();
        reset_output_pcm_stream();
        ESP_LOGI(TAG, "assistant audio started");
    } else if (strcmp(type, "response.output_audio.delta") == 0 ||
               strcmp(type, "response.audio.delta") == 0) {
        const char *delta = json_string(root, "delta");
        if (delta != NULL && s_playback_queue != NULL) {
            enqueue_audio_delta(delta);
        }
    } else if (strcmp(type, "response.output_audio.done") == 0 ||
               strcmp(type, "response.audio.done") == 0) {
        finish_output_pcm_stream();
        ESP_LOGI(TAG, "assistant audio finished");
    } else if (strcmp(type, "response.canceled") == 0) {
        clear_playback_queue();
        reset_output_pcm_stream();
        ESP_LOGI(TAG, "assistant response canceled");
    } else if (strcmp(type, "error") == 0) {
        const cJSON *error = cJSON_GetObjectItemCaseSensitive(root, "error");
        const char *code = cJSON_IsObject(error) ? json_string(error, "code") : NULL;
        const char *error_message =
            cJSON_IsObject(error) ? json_string(error, "message") : json_string(root, "message");
        ESP_LOGE(TAG, "server error: code=%s, message=%s", code != NULL ? code : "unknown",
                 error_message != NULL ? error_message : "unknown");
    } else {
        ESP_LOGD(TAG, "server event: %s", type);
    }

    cJSON_Delete(root);
}

static bool prepare_rx_buffer(size_t total_length) {
    if (total_length == 0 || total_length > DOUBAO_MAX_MESSAGE_SIZE) {
        ESP_LOGW(TAG, "invalid WebSocket message length: %u", (unsigned)total_length);
        return false;
    }
    if (s_rx_capacity >= total_length + 1) {
        return true;
    }

    free(s_rx_message);
    s_rx_message = allocate_audio_memory(total_length + 1);
    if (s_rx_message == NULL) {
        s_rx_capacity = 0;
        ESP_LOGE(TAG, "no memory for WebSocket message (%u bytes)", (unsigned)total_length);
        return false;
    }
    s_rx_capacity = total_length + 1;
    return true;
}

static void handle_websocket_data(const esp_websocket_event_data_t *data) {
    if (data->op_code == 0x08) {
        ESP_LOGW(TAG, "server sent WebSocket close frame: code=%d", data->close_status_code);
        return;
    }
    if (data->op_code != 0x01 && data->op_code != 0x00) {
        return;
    }

    size_t total_length =
        data->payload_len > 0 ? (size_t)data->payload_len : (size_t)data->data_len;
    size_t offset = data->payload_offset > 0 ? (size_t)data->payload_offset : 0;

    if (offset == 0) {
        s_rx_expected = total_length;
        s_rx_received = 0;
        if (!prepare_rx_buffer(total_length)) {
            s_rx_expected = 0;
            return;
        }
    }

    if (s_rx_message == NULL || total_length != s_rx_expected || offset != s_rx_received ||
        offset + (size_t)data->data_len > s_rx_expected) {
        ESP_LOGW(TAG, "unexpected WebSocket fragment: total=%u offset=%u len=%d",
                 (unsigned)total_length, (unsigned)offset, data->data_len);
        s_rx_expected = 0;
        s_rx_received = 0;
        return;
    }

    memcpy(s_rx_message + offset, data->data_ptr, (size_t)data->data_len);
    s_rx_received += (size_t)data->data_len;
    if (s_rx_received == s_rx_expected) {
        s_rx_message[s_rx_received] = '\0';
        handle_server_message(s_rx_message, s_rx_received);
        s_rx_expected = 0;
        s_rx_received = 0;
    }
}

static void websocket_event_handler(void *handler_args, esp_event_base_t event_base,
                                    int32_t event_id, void *event_data) {
    (void)handler_args;
    (void)event_base;
    esp_websocket_event_data_t *data = (esp_websocket_event_data_t *)event_data;

    switch (event_id) {
        case WEBSOCKET_EVENT_CONNECTED:
            xEventGroupClearBits(s_state, DOUBAO_SESSION_SENT_BIT | DOUBAO_SESSION_READY_BIT);
            xEventGroupSetBits(s_state, DOUBAO_CONNECTED_BIT);
            ESP_LOGI(TAG, "WebSocket connected");
            break;

#if WS_TRANSPORT_HEADER_CALLBACK_SUPPORT
        case WEBSOCKET_EVENT_HEADER_RECEIVED:
            if (data->data_ptr != NULL) {
                ESP_LOGD(TAG, "handshake header: %.*s", data->data_len, data->data_ptr);
            }
            break;
#endif

        case WEBSOCKET_EVENT_DATA:
            handle_websocket_data(data);
            break;

        case WEBSOCKET_EVENT_ERROR:
            ESP_LOGE(TAG, "WebSocket error: type=%d http=%d tls=0x%x socket_errno=%d",
                     data->error_handle.error_type, data->error_handle.esp_ws_handshake_status_code,
                     data->error_handle.esp_tls_last_esp_err,
                     data->error_handle.esp_transport_sock_errno);
            break;

        case WEBSOCKET_EVENT_DISCONNECTED:
        case WEBSOCKET_EVENT_CLOSED:
            xEventGroupClearBits(
                s_state, DOUBAO_CONNECTED_BIT | DOUBAO_SESSION_SENT_BIT | DOUBAO_SESSION_READY_BIT);
            clear_playback_queue();
            reset_output_pcm_stream();
            ESP_LOGW(TAG, "WebSocket disconnected (http=%d)",
                     data->error_handle.esp_ws_handshake_status_code);
            break;

        default:
            break;
    }
}

/**
 * @brief 将24kHz音频降采样为16kHz，采用简单的3:2比例转换
 *
 * 此函数每处理3个输入样本生成2个输出样本，实现24kHz到16kHz的采样率转换。
 * 第一个输出样本直接取自第一个输入样本，第二个输出样本取剩余两个输入样本的平均值。
 *
 * @note 此为低复杂度、低质量的降采样实现，未使用抗混叠滤波器，可能会引入混叠失真
 *
 * @param input  输入缓冲区指针，包含24kHz 16位PCM音频数据
 *               至少需要有 CODEC_FRAME_SAMPLES 个样本
 * @param output 输出缓冲区指针，用于存放16kHz 16位PCM音频数据
 *               输出缓冲区大小应为 (CODEC_FRAME_SAMPLES * 2 / 3) 个样本
 */
static void downsample_24k_to_16k(const int16_t *input, int16_t *output) {
    size_t output_index = 0;
    for (size_t input_index = 0; input_index + 2 < CODEC_FRAME_SAMPLES; input_index += 3) {
        output[output_index++] = input[input_index];
        output[output_index++] =
            (int16_t)(((int32_t)input[input_index + 1] + (int32_t)input[input_index + 2]) / 2);
    }
}

static void doubao_uplink_task(void *argument) {
    (void)argument;

    ESP_LOGI(TAG, "microphone uplink task started on CPU%d", xPortGetCoreID());
    int16_t capture[CODEC_FRAME_SAMPLES];
    int16_t resampled[DOUBAO_INPUT_FRAME_SAMPLES];
    unsigned char base64_audio[DOUBAO_INPUT_BASE64_SIZE];
    char json[DOUBAO_UPLINK_JSON_SIZE];

    while (true) {
        EventBits_t bits =
            xEventGroupWaitBits(s_state, DOUBAO_CONNECTED_BIT, pdFALSE, pdTRUE, portMAX_DELAY);
        if ((bits & DOUBAO_CONNECTED_BIT) == 0) {
            continue;
        }

        bits = xEventGroupGetBits(s_state);
        if ((bits & DOUBAO_SESSION_SENT_BIT) == 0) {
            if (send_session_create() != ESP_OK) {
                ESP_LOGW(TAG, "session.create send failed; retrying");
                vTaskDelay(pdMS_TO_TICKS(1000));
                continue;
            }
            xEventGroupSetBits(s_state, DOUBAO_SESSION_SENT_BIT);
        }

        bits = xEventGroupWaitBits(s_state, DOUBAO_CONNECTED_BIT | DOUBAO_SESSION_READY_BIT,
                                   pdFALSE, pdTRUE, pdMS_TO_TICKS(DOUBAO_SESSION_TIMEOUT_MS));
        if ((bits & (DOUBAO_CONNECTED_BIT | DOUBAO_SESSION_READY_BIT)) !=
            (DOUBAO_CONNECTED_BIT | DOUBAO_SESSION_READY_BIT)) {
            if ((bits & DOUBAO_CONNECTED_BIT) != 0) {
                ESP_LOGW(TAG, "session.create timed out; retrying");
                xEventGroupClearBits(s_state, DOUBAO_SESSION_SENT_BIT);
            }
            continue;
        }

        int rc = oai_audio_read(capture, (int)CODEC_FRAME_BYTES);
        if (rc != 0) {
            ESP_LOGE(TAG, "microphone read failed: rc=%d", rc);
            vTaskDelay(pdMS_TO_TICKS(DOUBAO_FRAME_MS));
            continue;
        }

        downsample_24k_to_16k(capture, resampled);

        size_t base64_length = 0;
        rc = mbedtls_base64_encode(base64_audio, sizeof(base64_audio), &base64_length,
                                   (const unsigned char *)resampled, DOUBAO_INPUT_FRAME_BYTES);
        if (rc != 0) {
            ESP_LOGE(TAG, "microphone Base64 encode failed: rc=%d", rc);
            continue;
        }
        base64_audio[base64_length] = '\0';

        int json_length = snprintf(json, sizeof(json),
                                   "{\"event_id\":\"event_%08" PRIx32
                                   "\",\"type\":\"input_audio_buffer.append\",\"audio\":\"%s\"}",
                                   esp_random(), (const char *)base64_audio);
        if (json_length <= 0 || json_length >= (int)sizeof(json)) {
            ESP_LOGE(TAG, "uplink JSON buffer is too small");
            continue;
        }

        if (websocket_send_text(json) != ESP_OK) {
            ESP_LOGW(TAG, "microphone audio send failed");
            vTaskDelay(pdMS_TO_TICKS(100));
        }
    }
}

static void doubao_playback_task(void *argument) {
    (void)argument;
    playback_chunk_t chunk;

    ESP_LOGI(TAG, "speaker playback task started on CPU%d", xPortGetCoreID());

    while (true) {
        if (xQueueReceive(s_playback_queue, &chunk, portMAX_DELAY) != pdTRUE) {
            continue;
        }
        for (size_t offset = 0; offset < chunk.length; offset += DOUBAO_I2S_WRITE_CHUNK_BYTES) {
            size_t remaining = chunk.length - offset;
            size_t write_length = remaining < DOUBAO_I2S_WRITE_CHUNK_BYTES
                                      ? remaining
                                      : DOUBAO_I2S_WRITE_CHUNK_BYTES;
            int rc = oai_audio_write(chunk.data + offset, (int)write_length);
            if (rc != 0) {
                ESP_LOGE(TAG, "speaker write failed: rc=%d, offset=%u/%u", rc,
                         (unsigned)offset, (unsigned)chunk.length);
                break;
            }
        }
        free(chunk.data);
    }
}

esp_err_t doubao_realtime_start(void) {
    if (s_client != NULL) {
        return ESP_ERR_INVALID_STATE;
    }
    if (DOUBAO_API_KEY[0] == '\0' ||
        strcmp(DOUBAO_API_KEY, "PASTE_YOUR_DOUBAO_API_KEY_HERE") == 0) {
        ESP_LOGE(TAG, "API Key not configured; edit main/doubao_secrets.h first");
        return ESP_ERR_INVALID_ARG;
    }

    s_state = xEventGroupCreate();
    s_playback_queue = xQueueCreate(DOUBAO_PLAYBACK_QUEUE_LENGTH, sizeof(playback_chunk_t));
    if (s_state == NULL || s_playback_queue == NULL) {
        doubao_realtime_stop();
        return ESP_ERR_NO_MEM;
    }

    esp_websocket_client_config_t websocket_config = {
        .uri = DOUBAO_WS_URI,
        .headers = s_auth_headers,
        .disable_auto_reconnect = false,
        .task_name = "doubao_ws",
        .task_stack = 8192,
        .task_prio = 6,
        .task_core_id_set = true,
        .task_core_id = DOUBAO_TASK_CORE,
        .buffer_size = DOUBAO_WS_BUFFER_SIZE,
        .crt_bundle_attach = esp_crt_bundle_attach,
        .ping_interval_sec = 10,
        .pingpong_timeout_sec = 30,
        .reconnect_timeout_ms = 5000,
        .network_timeout_ms = 10000,
    };

    s_client = esp_websocket_client_init(&websocket_config);
    if (s_client == NULL) {
        doubao_realtime_stop();
        return ESP_ERR_NO_MEM;
    }

    esp_err_t error =
        esp_websocket_register_events(s_client, WEBSOCKET_EVENT_ANY, websocket_event_handler, NULL);
    if (error != ESP_OK) {
        ESP_LOGE(TAG, "WebSocket event registration failed: %s", esp_err_to_name(error));
        doubao_realtime_stop();
        return error;
    }

    if (xTaskCreatePinnedToCore(doubao_playback_task, "doubao_play", 4096, NULL, 6,
                                &s_playback_task, DOUBAO_TASK_CORE) != pdPASS ||
        xTaskCreatePinnedToCore(doubao_uplink_task, "doubao_uplink", 8192, NULL, 6,
                                &s_uplink_task, DOUBAO_TASK_CORE) != pdPASS) {
        ESP_LOGE(TAG, "failed to create Doubao audio tasks");
        doubao_realtime_stop();
        return ESP_ERR_NO_MEM;
    }

    error = esp_websocket_client_start(s_client);
    if (error != ESP_OK) {
        ESP_LOGE(TAG, "WebSocket start failed: %s", esp_err_to_name(error));
        doubao_realtime_stop();
        return error;
    }

    ESP_LOGI(TAG, "connecting to Doubao Seeduplex 3.0");
    return ESP_OK;
}

esp_err_t doubao_realtime_stop(void) {
    if (s_uplink_task != NULL) {
        vTaskDelete(s_uplink_task);
        s_uplink_task = NULL;
    }
    if (s_playback_task != NULL) {
        vTaskDelete(s_playback_task);
        s_playback_task = NULL;
    }

    clear_playback_queue();
    reset_output_pcm_stream();

    if (s_client != NULL) {
        if (esp_websocket_client_is_connected(s_client)) {
            const char *close_event = "{\"type\":\"session.close\",\"event_id\":\"event_close\"}";
            websocket_send_text(close_event);
            esp_websocket_client_close(s_client, pdMS_TO_TICKS(2000));
        } else {
            esp_websocket_client_stop(s_client);
        }
        esp_websocket_unregister_events(s_client, WEBSOCKET_EVENT_ANY, websocket_event_handler);
        esp_websocket_client_destroy(s_client);
        s_client = NULL;
    }

    free(s_rx_message);
    s_rx_message = NULL;
    s_rx_capacity = 0;
    s_rx_expected = 0;
    s_rx_received = 0;

    if (s_playback_queue != NULL) {
        vQueueDelete(s_playback_queue);
        s_playback_queue = NULL;
    }
    if (s_state != NULL) {
        vEventGroupDelete(s_state);
        s_state = NULL;
    }
    return ESP_OK;
}
