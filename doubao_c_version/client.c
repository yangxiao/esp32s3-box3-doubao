#include "client.h"
#include "config.h"
#include "protocol.h"
#include <stdlib.h>
#include <string.h>
#include <stdio.h>
#include <unistd.h>

/* ---- Internal helpers ---- */

static void write_u32_be(uint8_t *buf, uint32_t val) {
    buf[0] = (val >> 24) & 0xFF;
    buf[1] = (val >> 16) & 0xFF;
    buf[2] = (val >> 8) & 0xFF;
    buf[3] = val & 0xFF;
}

/*
 * Build a full-request command message:
 *   [header(4B)][cmd_id(4B)][session_id_len(4B)][session_id][payload_len(4B)][gzip_payload]
 *
 * If include_session is false, skip session_id fields (used for StartConnection/FinishConnection).
 */
static uint8_t *build_command(uint8_t msg_type, uint8_t flags, uint8_t serial,
                              uint8_t compress, uint32_t cmd_id,
                              const char *session_id,
                              const uint8_t *payload, size_t payload_len,
                              bool include_session, size_t *out_len) {
    /* Compress payload */
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
    /* Calculate total size */
    size_t total = 4; /* header */
    total += 4; /* cmd_id */
    if (include_session) {
        total += 4 + sid_len; /* session_id_len + session_id */
    }
    total += 4 + compressed_len; /* payload_len + payload */

    /* LWS requires LWS_PRE bytes before the data */
    uint8_t *buf = malloc(LWS_PRE + total);
    if (!buf) { free(compressed); return NULL; }
    uint8_t *p = buf + LWS_PRE;

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
    return buf; /* caller must free; actual data starts at buf + LWS_PRE */
}

/* ---- LWS callback ---- */

/* Helper to append a custom header during handshake.
 * name should NOT include trailing colon - we add it. */
static int append_header(struct lws *wsi, uint8_t **p, uint8_t *end,
                         const char *name, const char *value) {
    /* lws_add_http_header_by_name expects name with trailing colon */
    char name_colon[256];
    snprintf(name_colon, sizeof(name_colon), "%s:", name);
    if (lws_add_http_header_by_name(wsi,
            (const unsigned char *)name_colon,
            (const unsigned char *)value,
            (int)strlen(value), p, end)) {
        fprintf(stderr, "Failed to add header: %s\n", name);
        return -1;
    }
    return 0;
}

static int lws_callback(struct lws *wsi, enum lws_callback_reasons reason,
                         void *user, void *in, size_t len) {
    doubao_client_t *client = (doubao_client_t *)lws_context_user(lws_get_context(wsi));
    if (!client) return 0;

    switch (reason) {
    case LWS_CALLBACK_CLIENT_APPEND_HANDSHAKE_HEADER: {
        /* Add custom headers to the WebSocket upgrade request */
        uint8_t **p = (uint8_t **)in;
        uint8_t *end = (*p) + len;

        if (append_header(wsi, p, end, "X-Api-App-ID", client->app_id)) return -1;
        if (append_header(wsi, p, end, "X-Api-Access-Key", client->access_key)) return -1;
        if (append_header(wsi, p, end, "X-Api-Resource-Id", client->resource_id)) return -1;
        if (append_header(wsi, p, end, "X-Api-App-Key", client->app_key)) return -1;
        if (append_header(wsi, p, end, "X-Api-Connect-Id", client->connect_id)) return -1;
        break;
    }

    case LWS_CALLBACK_CLIENT_ESTABLISHED:
        client->connected = true;
        /* Check for logid header */
        {
            char logid_buf[256] = {0};
            int n = lws_hdr_custom_copy(wsi, logid_buf, sizeof(logid_buf),
                                         "x-tt-logid:", strlen("x-tt-logid:"));
            if (n > 0) {
                strncpy(client->logid, logid_buf, sizeof(client->logid) - 1);
                printf("dialog server response logid: %s\n", client->logid);
            }
        }
        lws_callback_on_writable(wsi);
        break;

    case LWS_CALLBACK_CLIENT_RECEIVE: {
        /* Accumulate fragmented messages */
        size_t needed = client->recv_buf_len + len;
        if (needed > client->recv_buf_cap) {
            size_t new_cap = needed * 2;
            if (new_cap < 8192) new_cap = 8192;
            uint8_t *tmp = realloc(client->recv_buf, new_cap);
            if (!tmp) break;
            client->recv_buf = tmp;
            client->recv_buf_cap = new_cap;
        }
        memcpy(client->recv_buf + client->recv_buf_len, in, len);
        client->recv_buf_len += len;

        if (lws_is_final_fragment(wsi)) {
            /* Complete message received */
            if (client->on_recv) {
                client->on_recv(client->recv_buf, client->recv_buf_len, client->recv_userdata);
            }
            client->recv_buf_len = 0;
        }
        break;
    }

    case LWS_CALLBACK_CLIENT_WRITEABLE: {
        pthread_mutex_lock(&client->send_mutex);
        if (client->send_head != client->send_tail) {
            ws_message_t *msg = &client->send_queue[client->send_head];
            /* msg->data has LWS_PRE bytes prepended */
            int written = lws_write(wsi, msg->data + LWS_PRE, msg->len, LWS_WRITE_BINARY);
            free(msg->data);
            msg->data = NULL;
            client->send_head = (client->send_head + 1) % CLIENT_SEND_QUEUE_SIZE;
            if (written < 0) {
                pthread_mutex_unlock(&client->send_mutex);
                return -1;
            }
            /* If more messages, request another writable callback */
            if (client->send_head != client->send_tail) {
                lws_callback_on_writable(wsi);
            }
        }
        pthread_mutex_unlock(&client->send_mutex);
        break;
    }

    case LWS_CALLBACK_CLIENT_CONNECTION_ERROR:
        fprintf(stderr, "WebSocket connection error: %s\n", in ? (const char *)in : "unknown");
        client->connected = false;
        client->running = false;
        break;

    case LWS_CALLBACK_CLIENT_CLOSED:
        printf("WebSocket connection closed\n");
        client->connected = false;
        client->running = false;
        break;

    default:
        break;
    }
    return 0;
}

static const struct lws_protocols protocols[] = {
    { "doubao-protocol", lws_callback, 0, CLIENT_RECV_BUF_SIZE },
    { NULL, NULL, 0, 0 }
};

/* ---- Public API ---- */

int client_init(doubao_client_t *client, const char *session_id,
                const char *output_format, const char *input_mod,
                const char *asr_audio_format, int recv_timeout) {
    memset(client, 0, sizeof(*client));

    strncpy(client->host, WS_HOST, sizeof(client->host) - 1);
    client->port = WS_PORT;
    strncpy(client->path, WS_PATH, sizeof(client->path) - 1);

    /* Read env vars */
    const char *app_id = getenv("DOUBAO_APP_ID");
    const char *access_key = getenv("DOUBAO_ACCESS_KEY");
    if (!app_id || !access_key || strlen(app_id) == 0 || strlen(access_key) == 0) {
        fprintf(stderr, "Error: DOUBAO_APP_ID and DOUBAO_ACCESS_KEY env vars must be set\n");
        return -1;
    }
    strncpy(client->app_id, app_id, sizeof(client->app_id) - 1);
    strncpy(client->access_key, access_key, sizeof(client->access_key) - 1);
    strncpy(client->resource_id, WS_RESOURCE_ID, sizeof(client->resource_id) - 1);
    strncpy(client->app_key, WS_APP_KEY, sizeof(client->app_key) - 1);
    generate_uuid(client->connect_id, sizeof(client->connect_id));

    strncpy(client->session_id, session_id, sizeof(client->session_id) - 1);
    strncpy(client->output_format, output_format, sizeof(client->output_format) - 1);
    strncpy(client->input_mod, input_mod, sizeof(client->input_mod) - 1);
    if (asr_audio_format && strlen(asr_audio_format) > 0) {
        strncpy(client->asr_audio_format, asr_audio_format, sizeof(client->asr_audio_format) - 1);
        client->use_opus_input = true;
    } else {
        client->use_opus_input = false;
    }
    client->recv_timeout = recv_timeout;

    client->send_head = 0;
    client->send_tail = 0;
    pthread_mutex_init(&client->send_mutex, NULL);

    client->recv_buf = malloc(CLIENT_RECV_BUF_SIZE);
    client->recv_buf_len = 0;
    client->recv_buf_cap = CLIENT_RECV_BUF_SIZE;

    client->connected = false;
    client->running = true;

    return 0;
}

void client_set_recv_callback(doubao_client_t *client, recv_callback_t cb, void *userdata) {
    client->on_recv = cb;
    client->recv_userdata = userdata;
}

int client_connect(doubao_client_t *client) {
    struct lws_context_creation_info ctx_info;
    memset(&ctx_info, 0, sizeof(ctx_info));
    ctx_info.port = CONTEXT_PORT_NO_LISTEN;
    ctx_info.protocols = protocols;
    ctx_info.options = LWS_SERVER_OPTION_DO_SSL_GLOBAL_INIT;
    ctx_info.user = client;

    client->context = lws_create_context(&ctx_info);
    if (!client->context) {
        fprintf(stderr, "Failed to create lws context\n");
        return -1;
    }

    struct lws_client_connect_info conn_info;
    memset(&conn_info, 0, sizeof(conn_info));
    conn_info.context = client->context;
    conn_info.address = client->host;
    conn_info.port = client->port;
    conn_info.path = client->path;
    conn_info.host = client->host;
    conn_info.origin = client->host;
    conn_info.ssl_connection = LCCSCF_USE_SSL;
    conn_info.protocol = protocols[0].name;

    client->wsi = lws_client_connect_via_info(&conn_info);
    if (!client->wsi) {
        fprintf(stderr, "Failed to initiate WebSocket connection\n");
        lws_context_destroy(client->context);
        return -1;
    }

    /* Service until connected */
    int timeout_ms = 10000;
    while (!client->connected && client->running && timeout_ms > 0) {
        lws_service(client->context, 50);
        timeout_ms -= 50;
    }

    if (!client->connected) {
        fprintf(stderr, "WebSocket connection timeout\n");
        return -1;
    }

    printf("WebSocket connected to %s:%d%s\n", client->host, client->port, client->path);
    return 0;
}

void client_service_loop(doubao_client_t *client) {
    while (client->running) {
        lws_service(client->context, 50);
    }
}

int client_send_raw(doubao_client_t *client, const uint8_t *data, size_t len) {
    /* data must have LWS_PRE bytes prepended */
    pthread_mutex_lock(&client->send_mutex);
    int next_tail = (client->send_tail + 1) % CLIENT_SEND_QUEUE_SIZE;
    if (next_tail == client->send_head) {
        pthread_mutex_unlock(&client->send_mutex);
        fprintf(stderr, "Send queue full!\n");
        return -1;
    }
    /* Copy the data (including LWS_PRE) */
    uint8_t *copy = malloc(LWS_PRE + len);
    if (!copy) {
        pthread_mutex_unlock(&client->send_mutex);
        return -1;
    }
    memcpy(copy, data, LWS_PRE + len);
    client->send_queue[client->send_tail].data = copy;
    client->send_queue[client->send_tail].len = len;
    client->send_tail = next_tail;
    pthread_mutex_unlock(&client->send_mutex);

    if (client->wsi)
        lws_callback_on_writable(client->wsi);
    return 0;
}

/* Helper: queue a built command (buf already has LWS_PRE prepended) */
static int send_command(doubao_client_t *client, uint8_t *buf, size_t msg_len) {
    if (!buf) return -1;
    int ret = client_send_raw(client, buf, msg_len);
    free(buf);
    return ret;
}

int client_start_connection(doubao_client_t *client) {
    const char *payload = "{}";
    size_t msg_len;
    uint8_t *buf = build_command(
        MSG_CLIENT_FULL_REQUEST, FLAG_MSG_WITH_EVENT,
        SERIAL_JSON, COMPRESS_GZIP,
        CMD_START_CONNECTION, NULL,
        (const uint8_t *)payload, strlen(payload),
        false, &msg_len);
    return send_command(client, buf, msg_len);
}

int client_start_session(doubao_client_t *client) {
    const char *asr_fmt = (strlen(client->asr_audio_format) > 0) ? client->asr_audio_format : NULL;
    const char *json = build_start_session_json(
        asr_fmt, client->input_mod, client->recv_timeout);
    size_t msg_len;
    uint8_t *buf = build_command(
        MSG_CLIENT_FULL_REQUEST, FLAG_MSG_WITH_EVENT,
        SERIAL_JSON, COMPRESS_GZIP,
        CMD_START_SESSION, client->session_id,
        (const uint8_t *)json, strlen(json),
        true, &msg_len);
    return send_command(client, buf, msg_len);
}

int client_say_hello(doubao_client_t *client) {
    const char *payload = "{\"content\":\"你好，我是豆包，有什么可以帮助你的？\"}";
    size_t msg_len;
    uint8_t *buf = build_command(
        MSG_CLIENT_FULL_REQUEST, FLAG_MSG_WITH_EVENT,
        SERIAL_JSON, COMPRESS_GZIP,
        CMD_SAY_HELLO, client->session_id,
        (const uint8_t *)payload, strlen(payload),
        true, &msg_len);
    return send_command(client, buf, msg_len);
}

int client_task_request(doubao_client_t *client, const uint8_t *audio, size_t audio_len) {
    /* Opus is already compressed, no gzip. PCM uses gzip. */
    uint8_t compress = client->use_opus_input ? COMPRESS_NONE : COMPRESS_GZIP;
    size_t msg_len;
    uint8_t *buf = build_command(
        MSG_CLIENT_AUDIO_ONLY, FLAG_MSG_WITH_EVENT,
        SERIAL_NONE, compress,
        CMD_TASK_REQUEST, client->session_id,
        audio, audio_len,
        true, &msg_len);
    return send_command(client, buf, msg_len);
}

int client_chat_text_query(doubao_client_t *client, const char *content) {
    char payload[4096];
    snprintf(payload, sizeof(payload), "{\"content\":\"%s\"}", content);
    size_t msg_len;
    uint8_t *buf = build_command(
        MSG_CLIENT_FULL_REQUEST, FLAG_MSG_WITH_EVENT,
        SERIAL_JSON, COMPRESS_GZIP,
        CMD_CHAT_TEXT_QUERY, client->session_id,
        (const uint8_t *)payload, strlen(payload),
        true, &msg_len);
    return send_command(client, buf, msg_len);
}

int client_finish_session(doubao_client_t *client) {
    const char *payload = "{}";
    size_t msg_len;
    uint8_t *buf = build_command(
        MSG_CLIENT_FULL_REQUEST, FLAG_MSG_WITH_EVENT,
        SERIAL_JSON, COMPRESS_GZIP,
        CMD_FINISH_SESSION, client->session_id,
        (const uint8_t *)payload, strlen(payload),
        true, &msg_len);
    return send_command(client, buf, msg_len);
}

int client_finish_connection(doubao_client_t *client) {
    const char *payload = "{}";
    size_t msg_len;
    uint8_t *buf = build_command(
        MSG_CLIENT_FULL_REQUEST, FLAG_MSG_WITH_EVENT,
        SERIAL_JSON, COMPRESS_GZIP,
        CMD_FINISH_CONNECTION, NULL,
        (const uint8_t *)payload, strlen(payload),
        false, &msg_len);
    return send_command(client, buf, msg_len);
}

void client_destroy(doubao_client_t *client) {
    client->running = false;
    if (client->context) {
        lws_context_destroy(client->context);
        client->context = NULL;
    }
    free(client->recv_buf);
    client->recv_buf = NULL;

    /* Free any remaining messages in send queue */
    pthread_mutex_lock(&client->send_mutex);
    while (client->send_head != client->send_tail) {
        free(client->send_queue[client->send_head].data);
        client->send_head = (client->send_head + 1) % CLIENT_SEND_QUEUE_SIZE;
    }
    pthread_mutex_unlock(&client->send_mutex);
    pthread_mutex_destroy(&client->send_mutex);
}
