#ifndef DOUBAO_PROTOCOL_H
#define DOUBAO_PROTOCOL_H

#include <stdint.h>
#include <stddef.h>

/* Protocol version & header */
#define PROTOCOL_VERSION        0x01
#define DEFAULT_HEADER_SIZE     0x01

/* Message Types */
#define MSG_CLIENT_FULL_REQUEST     0x01
#define MSG_CLIENT_AUDIO_ONLY       0x02
#define MSG_SERVER_FULL_RESPONSE    0x09
#define MSG_SERVER_ACK              0x0B
#define MSG_SERVER_ERROR            0x0F

/* Message Type Specific Flags */
#define FLAG_NO_SEQUENCE    0x00
#define FLAG_POS_SEQUENCE   0x01
#define FLAG_NEG_SEQUENCE   0x02
#define FLAG_NEG_SEQUENCE_1 0x03
#define FLAG_MSG_WITH_EVENT 0x04

/* Serialization Methods */
#define SERIAL_NONE     0x00
#define SERIAL_JSON     0x01
#define SERIAL_THRIFT   0x03
#define SERIAL_CUSTOM   0x0F

/* Compression Types */
#define COMPRESS_NONE   0x00
#define COMPRESS_GZIP   0x01
#define COMPRESS_CUSTOM 0x0F

/* Command IDs */
#define CMD_START_CONNECTION    1
#define CMD_FINISH_CONNECTION   2
#define CMD_START_SESSION       100
#define CMD_FINISH_SESSION      102
#define CMD_TASK_REQUEST        200
#define CMD_SAY_HELLO           300
#define CMD_CHAT_TTS_TEXT       500
#define CMD_CHAT_TEXT_QUERY     501
#define CMD_CHAT_RAG_TEXT       502

/* Event codes */
#define EVENT_TTS_TEXT_CLEAR    350
#define EVENT_TTS_ENDED        359
#define EVENT_CLEAR_CACHE      450
#define EVENT_USER_QUERY_END   459
#define EVENT_SESSION_FINISH_1 152
#define EVENT_SESSION_FINISH_2 153

/* Parsed response */
typedef struct {
    int message_type;
    int event;
    int has_event;
    uint32_t seq;
    int has_seq;
    char session_id[128];
    uint32_t payload_size;
    uint8_t *payload_data;      /* caller must free with free() */
    size_t payload_data_len;
    int is_binary;              /* 1 = raw audio, 0 = JSON text */
    uint32_t error_code;
} parsed_response_t;

/**
 * Generate 4-byte protocol header.
 * Returns number of bytes written (always 4), or -1 on error.
 */
int protocol_generate_header(uint8_t *buf, size_t buf_len,
                             uint8_t message_type,
                             uint8_t flags,
                             uint8_t serial_method,
                             uint8_t compression);

/**
 * Parse a server response.
 * resp->payload_data is allocated with heap_caps_malloc, caller must free.
 * Returns 0 on success, -1 on error.
 */
int protocol_parse_response(const uint8_t *data, size_t len, parsed_response_t *resp);

/**
 * Gzip compress data. Returns malloc'd buffer, sets out_len.
 * Caller must free returned buffer.
 */
uint8_t *gzip_compress(const uint8_t *data, size_t data_len, size_t *out_len);

/**
 * Gzip decompress data. Returns malloc'd buffer, sets out_len.
 * Caller must free returned buffer.
 */
uint8_t *gzip_decompress(const uint8_t *data, size_t data_len, size_t *out_len);

#endif /* DOUBAO_PROTOCOL_H */
