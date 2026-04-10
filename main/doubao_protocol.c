#include "doubao_protocol.h"
#include <stdlib.h>
#include <string.h>
#include "esp_log.h"
#include "esp_heap_caps.h"
#include "miniz.h"

static const char *TAG = "doubao_proto";

int protocol_generate_header(uint8_t *buf, size_t buf_len,
                             uint8_t message_type,
                             uint8_t flags,
                             uint8_t serial_method,
                             uint8_t compression) {
    if (buf_len < 4) return -1;
    buf[0] = (PROTOCOL_VERSION << 4) | DEFAULT_HEADER_SIZE;
    buf[1] = (message_type << 4) | flags;
    buf[2] = (serial_method << 4) | compression;
    buf[3] = 0x00;
    return 4;
}

/* CRC32 for gzip trailer */
static uint32_t crc32_calc(const uint8_t *data, size_t len) {
    uint32_t crc = 0xFFFFFFFF;
    for (size_t i = 0; i < len; i++) {
        crc ^= data[i];
        for (int j = 0; j < 8; j++) {
            crc = (crc >> 1) ^ (0xEDB88320 & (-(crc & 1)));
        }
    }
    return ~crc;
}

uint8_t *gzip_compress(const uint8_t *data, size_t data_len, size_t *out_len) {
    /* Gzip = 10-byte header + deflate stream + 8-byte trailer (CRC32 + size) */
    size_t deflate_bound = data_len + (data_len / 1000) + 256;
    size_t total_bound = 10 + deflate_bound + 8;

    uint8_t *out = heap_caps_malloc(total_bound, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    if (!out) {
        out = malloc(total_bound);
        if (!out) return NULL;
    }

    /* Gzip header (10 bytes, minimal) */
    out[0] = 0x1F;  /* magic */
    out[1] = 0x8B;  /* magic */
    out[2] = 0x08;  /* method: deflate */
    out[3] = 0x00;  /* flags: none */
    out[4] = out[5] = out[6] = out[7] = 0x00;  /* mtime */
    out[8] = 0x00;  /* xfl */
    out[9] = 0xFF;  /* OS: unknown */

    /* Raw deflate compress using tdefl */
    size_t compressed_len = tdefl_compress_mem_to_mem(
        out + 10, deflate_bound,
        data, data_len,
        TDEFL_DEFAULT_MAX_PROBES);

    if (compressed_len > 0 && compressed_len != (size_t)-1) {
        /* Gzip trailer: CRC32 + original size (little-endian) */
        size_t pos = 10 + compressed_len;
        uint32_t crc = crc32_calc(data, data_len);
        out[pos + 0] = (crc >>  0) & 0xFF;
        out[pos + 1] = (crc >>  8) & 0xFF;
        out[pos + 2] = (crc >> 16) & 0xFF;
        out[pos + 3] = (crc >> 24) & 0xFF;
        uint32_t isize = (uint32_t)(data_len & 0xFFFFFFFF);
        out[pos + 4] = (isize >>  0) & 0xFF;
        out[pos + 5] = (isize >>  8) & 0xFF;
        out[pos + 6] = (isize >> 16) & 0xFF;
        out[pos + 7] = (isize >> 24) & 0xFF;

        *out_len = pos + 8;
        return out;
    }

    ESP_LOGE(TAG, "tdefl_compress_mem_to_mem failed");
    free(out);
    return NULL;
}

uint8_t *gzip_decompress(const uint8_t *data, size_t data_len, size_t *out_len) {
    /* Parse gzip header to find start of deflate stream */
    if (data_len < 10 || data[0] != 0x1F || data[1] != 0x8B) {
        ESP_LOGE(TAG, "Not a gzip stream");
        return NULL;
    }

    size_t offset = 10;
    uint8_t flags = data[3];

    /* Skip optional gzip header fields */
    if (flags & 0x04) { /* FEXTRA */
        if (offset + 2 > data_len) return NULL;
        uint16_t xlen = data[offset] | ((uint16_t)data[offset + 1] << 8);
        offset += 2 + xlen;
    }
    if (flags & 0x08) { /* FNAME */
        while (offset < data_len && data[offset] != 0) offset++;
        offset++;
    }
    if (flags & 0x10) { /* FCOMMENT */
        while (offset < data_len && data[offset] != 0) offset++;
        offset++;
    }
    if (flags & 0x02) { /* FHCRC */
        offset += 2;
    }

    if (offset >= data_len) return NULL;

    /* Deflate data starts at offset, trailer is last 8 bytes */
    const uint8_t *deflate_data = data + offset;
    size_t deflate_len = data_len - offset - 8;
    if (deflate_len == 0 || (int)deflate_len < 0) {
        deflate_len = data_len - offset;
    }

    /* Allocate output buffer - start with 8x input */
    size_t buf_size = data_len * 8;
    if (buf_size < 4096) buf_size = 4096;

    uint8_t *out = heap_caps_malloc(buf_size, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    if (!out) {
        out = malloc(buf_size);
        if (!out) return NULL;
    }

    /* Decompress using tinfl (raw deflate, no zlib header) */
    size_t decompressed_len = tinfl_decompress_mem_to_mem(
        out, buf_size,
        deflate_data, deflate_len, 0);

    if (decompressed_len == TINFL_DECOMPRESS_MEM_TO_MEM_FAILED) {
        ESP_LOGE(TAG, "tinfl decompress failed");
        free(out);
        return NULL;
    }

    *out_len = decompressed_len;
    return out;
}

static uint32_t read_u32_be(const uint8_t *p) {
    return ((uint32_t)p[0] << 24) | ((uint32_t)p[1] << 16) |
           ((uint32_t)p[2] << 8)  | ((uint32_t)p[3]);
}

int protocol_parse_response(const uint8_t *data, size_t len, parsed_response_t *resp) {
    if (len < 4) return -1;
    memset(resp, 0, sizeof(*resp));
    resp->event = -1;

    uint8_t header_size = data[0] & 0x0F;
    uint8_t message_type = data[1] >> 4;
    uint8_t msg_flags = data[1] & 0x0F;
    uint8_t serial_method = data[2] >> 4;
    uint8_t compression = data[2] & 0x0F;

    size_t header_bytes = (size_t)header_size * 4;
    if (len < header_bytes) return -1;

    const uint8_t *payload = data + header_bytes;
    size_t payload_len = len - header_bytes;
    size_t offset = 0;

    resp->message_type = message_type;

    if (message_type == MSG_SERVER_FULL_RESPONSE || message_type == MSG_SERVER_ACK) {
        if (msg_flags & FLAG_NEG_SEQUENCE) {
            if (offset + 4 > payload_len) return -1;
            resp->seq = read_u32_be(payload + offset);
            resp->has_seq = 1;
            offset += 4;
        }

        if (msg_flags & FLAG_MSG_WITH_EVENT) {
            if (offset + 4 > payload_len) return -1;
            resp->event = (int)read_u32_be(payload + offset);
            resp->has_event = 1;
            offset += 4;
        }

        if (offset + 4 > payload_len) return -1;
        int32_t sid_len = (int32_t)read_u32_be(payload + offset);
        offset += 4;
        if (sid_len > 0) {
            if (offset + (size_t)sid_len > payload_len) return -1;
            size_t copy_len = (size_t)sid_len < sizeof(resp->session_id) - 1
                            ? (size_t)sid_len : sizeof(resp->session_id) - 1;
            memcpy(resp->session_id, payload + offset, copy_len);
            resp->session_id[copy_len] = '\0';
            offset += (size_t)sid_len;
        }

        if (offset + 4 > payload_len) return -1;
        resp->payload_size = read_u32_be(payload + offset);
        offset += 4;

        const uint8_t *msg_data = payload + offset;
        size_t msg_len = payload_len - offset;

        if (msg_len == 0 || resp->payload_size == 0) {
            resp->payload_data = NULL;
            resp->payload_data_len = 0;
            return 0;
        }

        uint8_t *decompressed = NULL;
        size_t decompressed_len = 0;
        if (compression == COMPRESS_GZIP) {
            decompressed = gzip_decompress(msg_data, msg_len, &decompressed_len);
            if (!decompressed) return -1;
        } else {
            decompressed_len = msg_len;
            decompressed = heap_caps_malloc(decompressed_len, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
            if (!decompressed) {
                decompressed = malloc(decompressed_len);
                if (!decompressed) return -1;
            }
            memcpy(decompressed, msg_data, decompressed_len);
        }

        resp->is_binary = (serial_method == SERIAL_NONE) ? 1 : 0;
        resp->payload_data = decompressed;
        resp->payload_data_len = decompressed_len;

    } else if (message_type == MSG_SERVER_ERROR) {
        if (payload_len < 8) return -1;
        resp->error_code = read_u32_be(payload);
        resp->payload_size = read_u32_be(payload + 4);
        const uint8_t *msg_data = payload + 8;
        size_t msg_len = payload_len - 8;

        if (msg_len > 0) {
            uint8_t *decompressed = NULL;
            size_t decompressed_len = 0;
            if (compression == COMPRESS_GZIP) {
                decompressed = gzip_decompress(msg_data, msg_len, &decompressed_len);
            } else {
                decompressed_len = msg_len;
                decompressed = malloc(decompressed_len);
                if (decompressed) memcpy(decompressed, msg_data, decompressed_len);
            }
            resp->payload_data = decompressed;
            resp->payload_data_len = decompressed_len;
        }
    }

    return 0;
}
