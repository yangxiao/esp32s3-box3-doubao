#ifndef CLIENT_H
#define CLIENT_H

#include <libwebsockets.h>
#include <pthread.h>
#include <stdint.h>
#include <stdbool.h>
#include "protocol.h"

/* Max pending messages in send queue */
#define CLIENT_SEND_QUEUE_SIZE  64
#define CLIENT_RECV_BUF_SIZE    (256 * 1024)

/* A message to be sent over WebSocket */
typedef struct {
    uint8_t *data;
    size_t len;
} ws_message_t;

/* Received message callback */
typedef void (*recv_callback_t)(const uint8_t *data, size_t len, void *userdata);

/* Client state */
typedef struct {
    struct lws_context *context;
    struct lws *wsi;
    char host[256];
    int port;
    char path[512];

    /* Custom headers */
    char app_id[128];
    char access_key[128];
    char resource_id[128];
    char app_key[64];
    char connect_id[64];

    /* Session */
    char session_id[128];
    char output_format[32];
    char input_mod[32];
    int recv_timeout;

    /* Send queue (ring buffer) */
    ws_message_t send_queue[CLIENT_SEND_QUEUE_SIZE];
    int send_head;
    int send_tail;
    pthread_mutex_t send_mutex;

    /* Receive buffer (for fragmented messages) */
    uint8_t *recv_buf;
    size_t recv_buf_len;
    size_t recv_buf_cap;

    /* Receive callback */
    recv_callback_t on_recv;
    void *recv_userdata;

    /* State flags */
    volatile bool connected;
    volatile bool running;

    /* Log ID from server */
    char logid[256];
} doubao_client_t;

/* Initialize client. Returns 0 on success. */
int client_init(doubao_client_t *client, const char *session_id,
                const char *output_format, const char *input_mod, int recv_timeout);

/* Set receive callback */
void client_set_recv_callback(doubao_client_t *client, recv_callback_t cb, void *userdata);

/* Connect WebSocket and do StartConnection + StartSession handshake.
 * This runs the lws event loop in a background thread.
 * Returns 0 on success.
 */
int client_connect(doubao_client_t *client);

/* Run lws service loop (call in dedicated thread) */
void client_service_loop(doubao_client_t *client);

/* Queue a raw WebSocket message for sending */
int client_send_raw(doubao_client_t *client, const uint8_t *data, size_t len);

/* High-level commands - each builds the binary protocol message and queues it */
int client_start_connection(doubao_client_t *client);
int client_start_session(doubao_client_t *client);
int client_say_hello(doubao_client_t *client);
int client_task_request(doubao_client_t *client, const uint8_t *audio, size_t audio_len);
int client_chat_text_query(doubao_client_t *client, const char *content);
int client_finish_session(doubao_client_t *client);
int client_finish_connection(doubao_client_t *client);

/* Destroy client and free resources */
void client_destroy(doubao_client_t *client);

#endif /* CLIENT_H */
