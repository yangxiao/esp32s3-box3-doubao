#include "doubao_ws_client.h"
#include "doubao_protocol.h"
#include <stdlib.h>
#include <string.h>
#include <stdio.h>
#include <time.h>
#include "esp_log.h"
#include "esp_heap_caps.h"
#include "esp_websocket_client.h"
#include "esp_event.h"

static const char *TAG = "doubao_ws";

#define WS_HOST         "openspeech.bytedance.com"
#define WS_PORT         443
#define WS_PATH         "/api/v3/realtime/dialogue"
#define WS_RESOURCE_ID  "volc.speech.dialog"

#define RECV_BUF_INIT_SIZE  (64 * 1024)

/* ---- Helpers ---- */

static void write_u32_be(uint8_t *buf, uint32_t val) {
    buf[0] = (val >> 24) & 0xFF;
    buf[1] = (val >> 16) & 0xFF;
    buf[2] = (val >> 8) & 0xFF;
    buf[3] = val & 0xFF;
}

static void generate_uuid(char *buf, size_t len) {
    static const char hex[] = "0123456789abcdef";
    static const int groups[] = {8, 4, 4, 4, 12};
    int pos = 0;
    for (int g = 0; g < 5; g++) {
        if (g > 0 && pos < (int)len - 1) buf[pos++] = '-';
        for (int i = 0; i < groups[g] && pos < (int)len - 1; i++) {
            buf[pos++] = hex[esp_random() % 16];
        }
    }
    buf[pos] = '\0';
}

/**
 * Build start_session JSON payload.
 */
static int build_start_session_json(char *json_buf, size_t json_buf_size,
                                     const doubao_ws_config_t *config) {
    char asr_block[256];
    if (config->asr_audio_format && strlen(config->asr_audio_format) > 0) {
        snprintf(asr_block, sizeof(asr_block),
            "\"asr\":{"
                "\"audio_info\":{"
                    "\"format\":\"%s\","
                    "\"sample_rate\":16000,"
                    "\"channel\":1"
                "},"
                "\"extra\":{\"end_smooth_window_ms\":1500}"
            "},",
            config->asr_audio_format);
    } else {
        snprintf(asr_block, sizeof(asr_block),
            "\"asr\":{\"extra\":{\"end_smooth_window_ms\":1500}},");
    }

    return snprintf(json_buf, json_buf_size,
        "{"
            "%s"
            "\"tts\":{"
                "\"speaker\":\"%s\","
                "\"audio_config\":{"
                    "\"channel\":1,"
                    "\"format\":\"ogg_opus\","
                    "\"sample_rate\":24000"
                "}"
            "},"
            "\"dialog\":{"
                "\"bot_name\":\"豆包\","
                "\"system_role\":\"你使用活泼灵动的女声，性格开朗，热爱生活。\","
                "\"speaking_style\":\"你的说话风格简洁明了，语速适中，语调自然。\","
                "\"location\":{\"city\":\"北京\"},"
                "\"extra\":{"
                    "\"strict_audit\":false,"
                    "\"audit_response\":\"支持客户自定义安全审核回复话术。\","
                    "\"recv_timeout\":%d,"
                    "\"input_mod\":\"%s\""
                "}"
            "}"
        "}",
        asr_block,
        config->tts_speaker ? config->tts_speaker : "zh_female_vv_jupiter_bigtts",
        config->recv_timeout,
        config->input_mod ? config->input_mod : "audio");
}

/**
 * Build a protocol command message.
 * Returns malloc'd buffer with message data. Sets *out_len.
 * Caller must free.
 */
static uint8_t *build_command(uint8_t msg_type, uint8_t flags, uint8_t serial,
                              uint8_t compress, uint32_t cmd_id,
                              const char *session_id,
                              const uint8_t *payload, size_t payload_len,
                              bool include_session, size_t *out_len) {
    /* Compress payload if needed */
    uint8_t *compressed = NULL;
    size_t compressed_len = 0;
    if (compress == COMPRESS_GZIP && payload && payload_len > 0) {
        compressed = gzip_compress(payload, payload_len, &compressed_len);
        if (!compressed) return NULL;
    } else if (payload && payload_len > 0) {
        compressed_len = payload_len;
        compressed = malloc(payload_len);
        if (!compressed) return NULL;
        memcpy(compressed, payload, payload_len);
    }

    size_t sid_len = session_id ? strlen(session_id) : 0;
    size_t total = 4;           /* header */
    total += 4;                 /* cmd_id */
    if (include_session) {
        total += 4 + sid_len;   /* session_id_len + session_id */
    }
    total += 4 + compressed_len; /* payload_len + payload */

    uint8_t *buf = malloc(total);
    if (!buf) { free(compressed); return NULL; }
    uint8_t *p = buf;

    /* Header */
    protocol_generate_header(p, 4, msg_type, flags, serial, compress);
    p += 4;

    /* Command ID */
    write_u32_be(p, cmd_id);
    p += 4;

    /* Session ID */
    if (include_session) {
        write_u32_be(p, (uint32_t)sid_len);
        p += 4;
        if (sid_len > 0) {
            memcpy(p, session_id, sid_len);
            p += sid_len;
        }
    }

    /* Payload */
    write_u32_be(p, (uint32_t)compressed_len);
    p += 4;
    if (compressed_len > 0) {
        memcpy(p, compressed, compressed_len);
    }

    free(compressed);
    *out_len = total;
    return buf;
}

static int send_command(doubao_ws_client_t *client, uint8_t *buf, size_t msg_len) {
    if (!buf) return -1;
    if (!client->connected) {
        free(buf);
        return -1;
    }
    int ret = esp_websocket_client_send_bin(client->ws_handle, (const char *)buf,
                                             (int)msg_len, pdMS_TO_TICKS(10000));
    free(buf);
    return (ret < 0) ? -1 : 0;
}

/* ---- WebSocket event handler ---- */

static void ws_event_handler(void *handler_args, esp_event_base_t base,
                              int32_t event_id, void *event_data) {
    doubao_ws_client_t *client = (doubao_ws_client_t *)handler_args;
    esp_websocket_event_data_t *data = (esp_websocket_event_data_t *)event_data;

    switch (event_id) {
    case WEBSOCKET_EVENT_CONNECTED:
        ESP_LOGI(TAG, "WebSocket connected");
        client->connected = true;
        xEventGroupSetBits(client->event_group, WS_EVENT_CONNECTED);
        break;

    case WEBSOCKET_EVENT_DISCONNECTED:
        ESP_LOGW(TAG, "WebSocket disconnected");
        client->connected = false;
        xEventGroupSetBits(client->event_group, WS_EVENT_DISCONNECTED);
        break;

    case WEBSOCKET_EVENT_DATA:
        if (data->op_code == 0x02 || data->op_code == 0x00) {
            /* Binary frame or continuation */
            size_t needed = client->recv_buf_len + data->data_len;
            if (needed > client->recv_buf_cap) {
                size_t new_cap = needed * 2;
                if (new_cap < 8192) new_cap = 8192;
                uint8_t *tmp = heap_caps_realloc(client->recv_buf, new_cap,
                                                  MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
                if (!tmp) {
                    ESP_LOGE(TAG, "Failed to realloc recv buffer");
                    client->recv_buf_len = 0;
                    break;
                }
                client->recv_buf = tmp;
                client->recv_buf_cap = new_cap;
            }
            memcpy(client->recv_buf + client->recv_buf_len, data->data_ptr, data->data_len);
            client->recv_buf_len += data->data_len;

            /* Check if this is the final fragment */
            if (data->payload_offset + data->data_len >= data->payload_len) {
                /* Complete message */
                if (client->on_recv && client->recv_buf_len > 0) {
                    parsed_response_t resp;
                    if (protocol_parse_response(client->recv_buf, client->recv_buf_len, &resp) == 0) {
                        client->on_recv(&resp, client->recv_userdata);
                        free(resp.payload_data);
                    } else {
                        ESP_LOGE(TAG, "Failed to parse response (%d bytes)", (int)client->recv_buf_len);
                    }
                }
                client->recv_buf_len = 0;
            }
        }
        break;

    case WEBSOCKET_EVENT_ERROR:
        ESP_LOGE(TAG, "WebSocket error");
        break;

    default:
        break;
    }
}

/* ---- Public API ---- */

int doubao_ws_init(doubao_ws_client_t *client, const doubao_ws_config_t *config) {
    memset(client, 0, sizeof(*client));
    memcpy(&client->config, config, sizeof(doubao_ws_config_t));

    generate_uuid(client->session_id, sizeof(client->session_id));
    generate_uuid(client->connect_id, sizeof(client->connect_id));

    client->recv_buf = heap_caps_malloc(RECV_BUF_INIT_SIZE, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    if (!client->recv_buf) {
        client->recv_buf = malloc(RECV_BUF_INIT_SIZE);
        if (!client->recv_buf) return -1;
    }
    client->recv_buf_len = 0;
    client->recv_buf_cap = RECV_BUF_INIT_SIZE;

    client->event_group = xEventGroupCreate();
    if (!client->event_group) {
        free(client->recv_buf);
        return -1;
    }

    client->connected = false;
    client->running = true;
    return 0;
}

void doubao_ws_set_recv_callback(doubao_ws_client_t *client,
                                  doubao_ws_recv_cb_t cb, void *userdata) {
    client->on_recv = cb;
    client->recv_userdata = userdata;
}

int doubao_ws_connect(doubao_ws_client_t *client) {
    /* Build custom headers string */
    char headers[1024];
    snprintf(headers, sizeof(headers),
        "X-Api-App-ID: %s\r\n"
        "X-Api-Access-Key: %s\r\n"
        "X-Api-Resource-Id: %s\r\n"
        "X-Api-App-Key: %s\r\n"
        "X-Api-Connect-Id: %s\r\n",
        client->config.app_id,
        client->config.access_key,
        client->config.resource_id ? client->config.resource_id : WS_RESOURCE_ID,
        client->config.app_key ? client->config.app_key : "PlgvMymc7f3tQnJ6",
        client->connect_id);

    /* Build URI */
    char uri[512];
    snprintf(uri, sizeof(uri), "wss://%s:%d%s", WS_HOST, WS_PORT, WS_PATH);

    esp_websocket_client_config_t ws_cfg = {
        .uri = uri,
        .headers = headers,
        .buffer_size = 16384,
        .task_stack = 8192,
        .reconnect_timeout_ms = 5000,
        .network_timeout_ms = 10000,
    };

    client->ws_handle = esp_websocket_client_init(&ws_cfg);
    if (!client->ws_handle) {
        ESP_LOGE(TAG, "Failed to init WebSocket client");
        return -1;
    }

    esp_websocket_register_events(client->ws_handle, WEBSOCKET_EVENT_ANY,
                                   ws_event_handler, client);

    esp_err_t err = esp_websocket_client_start(client->ws_handle);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "Failed to start WebSocket client: %s", esp_err_to_name(err));
        esp_websocket_client_destroy(client->ws_handle);
        client->ws_handle = NULL;
        return -1;
    }

    /* Wait for connection */
    EventBits_t bits = xEventGroupWaitBits(client->event_group,
                                            WS_EVENT_CONNECTED | WS_EVENT_DISCONNECTED,
                                            pdTRUE, pdFALSE,
                                            pdMS_TO_TICKS(15000));
    if (!(bits & WS_EVENT_CONNECTED)) {
        ESP_LOGE(TAG, "WebSocket connection timeout");
        esp_websocket_client_stop(client->ws_handle);
        esp_websocket_client_destroy(client->ws_handle);
        client->ws_handle = NULL;
        return -1;
    }

    ESP_LOGI(TAG, "Connected to %s", uri);
    return 0;
}

int doubao_ws_start_connection(doubao_ws_client_t *client) {
    const char *payload = "{}";
    size_t msg_len;
    uint8_t *buf = build_command(
        MSG_CLIENT_FULL_REQUEST, FLAG_MSG_WITH_EVENT,
        SERIAL_JSON, COMPRESS_GZIP,
        CMD_START_CONNECTION, NULL,
        (const uint8_t *)payload, strlen(payload),
        false, &msg_len);
    ESP_LOGI(TAG, "Sending StartConnection");
    return send_command(client, buf, msg_len);
}

int doubao_ws_start_session(doubao_ws_client_t *client) {
    char json_buf[2048];
    build_start_session_json(json_buf, sizeof(json_buf), &client->config);

    size_t msg_len;
    uint8_t *buf = build_command(
        MSG_CLIENT_FULL_REQUEST, FLAG_MSG_WITH_EVENT,
        SERIAL_JSON, COMPRESS_GZIP,
        CMD_START_SESSION, client->session_id,
        (const uint8_t *)json_buf, strlen(json_buf),
        true, &msg_len);
    ESP_LOGI(TAG, "Sending StartSession (session=%s)", client->session_id);
    return send_command(client, buf, msg_len);
}

int doubao_ws_say_hello(doubao_ws_client_t *client) {
    const char *payload = "{\"content\":\"你好，我是豆包，有什么可以帮助你的？\"}";
    size_t msg_len;
    uint8_t *buf = build_command(
        MSG_CLIENT_FULL_REQUEST, FLAG_MSG_WITH_EVENT,
        SERIAL_JSON, COMPRESS_GZIP,
        CMD_SAY_HELLO, client->session_id,
        (const uint8_t *)payload, strlen(payload),
        true, &msg_len);
    ESP_LOGI(TAG, "Sending SayHello");
    return send_command(client, buf, msg_len);
}

int doubao_ws_send_audio(doubao_ws_client_t *client, const uint8_t *opus_data, size_t len) {
    bool use_opus = (client->config.asr_audio_format &&
                     strlen(client->config.asr_audio_format) > 0);
    uint8_t compress = use_opus ? COMPRESS_NONE : COMPRESS_GZIP;

    size_t msg_len;
    uint8_t *buf = build_command(
        MSG_CLIENT_AUDIO_ONLY, FLAG_MSG_WITH_EVENT,
        SERIAL_NONE, compress,
        CMD_TASK_REQUEST, client->session_id,
        opus_data, len,
        true, &msg_len);
    return send_command(client, buf, msg_len);
}

int doubao_ws_finish_session(doubao_ws_client_t *client) {
    const char *payload = "{}";
    size_t msg_len;
    uint8_t *buf = build_command(
        MSG_CLIENT_FULL_REQUEST, FLAG_MSG_WITH_EVENT,
        SERIAL_JSON, COMPRESS_GZIP,
        CMD_FINISH_SESSION, client->session_id,
        (const uint8_t *)payload, strlen(payload),
        true, &msg_len);
    ESP_LOGI(TAG, "Sending FinishSession");
    return send_command(client, buf, msg_len);
}

int doubao_ws_finish_connection(doubao_ws_client_t *client) {
    const char *payload = "{}";
    size_t msg_len;
    uint8_t *buf = build_command(
        MSG_CLIENT_FULL_REQUEST, FLAG_MSG_WITH_EVENT,
        SERIAL_JSON, COMPRESS_GZIP,
        CMD_FINISH_CONNECTION, NULL,
        (const uint8_t *)payload, strlen(payload),
        false, &msg_len);
    ESP_LOGI(TAG, "Sending FinishConnection");
    return send_command(client, buf, msg_len);
}

bool doubao_ws_is_connected(doubao_ws_client_t *client) {
    return client->connected;
}

void doubao_ws_destroy(doubao_ws_client_t *client) {
    client->running = false;
    if (client->ws_handle) {
        esp_websocket_client_stop(client->ws_handle);
        esp_websocket_client_destroy(client->ws_handle);
        client->ws_handle = NULL;
    }
    free(client->recv_buf);
    client->recv_buf = NULL;
    if (client->event_group) {
        vEventGroupDelete(client->event_group);
        client->event_group = NULL;
    }
}
