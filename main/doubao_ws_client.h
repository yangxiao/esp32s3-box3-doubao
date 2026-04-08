#ifndef DOUBAO_WS_CLIENT_H
#define DOUBAO_WS_CLIENT_H

#include <stdint.h>
#include <stddef.h>
#include <stdbool.h>
#include "doubao_protocol.h"
#include "freertos/FreeRTOS.h"
#include "freertos/event_groups.h"

/* WebSocket events posted to the event group */
#define WS_EVENT_CONNECTED      BIT0
#define WS_EVENT_DISCONNECTED   BIT1
#define WS_EVENT_DATA_RECEIVED  BIT2

/* Receive callback: called from WS event handler context */
typedef void (*doubao_ws_recv_cb_t)(const parsed_response_t *resp, void *userdata);

/* Configuration */
typedef struct {
    const char *app_id;
    const char *access_key;
    const char *app_key;
    const char *resource_id;
    const char *tts_speaker;
    const char *input_mod;        /* "audio" or "text" */
    const char *asr_audio_format; /* "speech_opus" or NULL for PCM */
    int recv_timeout;             /* 10-120 */
} doubao_ws_config_t;

/* Client state */
typedef struct {
    void *ws_handle;              /* esp_websocket_client_handle_t */
    char session_id[64];
    char connect_id[64];
    doubao_ws_config_t config;

    /* Receive fragment buffer (PSRAM) */
    uint8_t *recv_buf;
    size_t recv_buf_len;
    size_t recv_buf_cap;

    /* Callback */
    doubao_ws_recv_cb_t on_recv;
    void *recv_userdata;

    /* State */
    EventGroupHandle_t event_group;
    volatile bool connected;
    volatile bool running;
} doubao_ws_client_t;

/**
 * Initialize client. Reads APP_ID and ACCESS_KEY from config.
 */
int doubao_ws_init(doubao_ws_client_t *client, const doubao_ws_config_t *config);

/**
 * Set receive callback for parsed responses.
 */
void doubao_ws_set_recv_callback(doubao_ws_client_t *client,
                                  doubao_ws_recv_cb_t cb, void *userdata);

/**
 * Connect to Doubao WebSocket server. Blocks until connected or timeout.
 */
int doubao_ws_connect(doubao_ws_client_t *client);

/* High-level protocol commands */
int doubao_ws_start_connection(doubao_ws_client_t *client);
int doubao_ws_start_session(doubao_ws_client_t *client);
int doubao_ws_say_hello(doubao_ws_client_t *client);
int doubao_ws_send_audio(doubao_ws_client_t *client, const uint8_t *opus_data, size_t len);
int doubao_ws_finish_session(doubao_ws_client_t *client);
int doubao_ws_finish_connection(doubao_ws_client_t *client);

/**
 * Check if connected.
 */
bool doubao_ws_is_connected(doubao_ws_client_t *client);

/**
 * Destroy client and free resources.
 */
void doubao_ws_destroy(doubao_ws_client_t *client);

#endif /* DOUBAO_WS_CLIENT_H */
