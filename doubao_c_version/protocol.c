#include "protocol.h"
#include <stdlib.h>
#include <string.h>
#include <stdio.h>
#include <zlib.h>

int protocol_generate_header(uint8_t *buf, size_t buf_len,
                             uint8_t message_type,
                             uint8_t flags,
                             uint8_t serial_method,
                             uint8_t compression) {
    if (buf_len < 4) return -1;
    buf[0] = (PROTOCOL_VERSION << 4) | DEFAULT_HEADER_SIZE;
    buf[1] = (message_type << 4) | flags;
    buf[2] = (serial_method << 4) | compression;
    buf[3] = 0x00; /* reserved */
    return 4;
}

uint8_t *gzip_compress(const uint8_t *data, size_t data_len, size_t *out_len) {
    /* Worst case: input size + overhead */
    size_t bound = compressBound(data_len) + 256;
    uint8_t *out = malloc(bound);
    if (!out) return NULL;

    z_stream strm;
    memset(&strm, 0, sizeof(strm));
    /* windowBits = 15 + 16 for gzip format */
    if (deflateInit2(&strm, Z_DEFAULT_COMPRESSION, Z_DEFLATED,
                     15 + 16, 8, Z_DEFAULT_STRATEGY) != Z_OK) {
        free(out);
        return NULL;
    }

    strm.next_in = (uint8_t *)data;
    strm.avail_in = (uInt)data_len;
    strm.next_out = out;
    strm.avail_out = (uInt)bound;

    if (deflate(&strm, Z_FINISH) != Z_STREAM_END) {
        deflateEnd(&strm);
        free(out);
        return NULL;
    }

    *out_len = strm.total_out;
    deflateEnd(&strm);
    return out;
}

uint8_t *gzip_decompress(const uint8_t *data, size_t data_len, size_t *out_len) {
    size_t buf_size = data_len * 8;
    if (buf_size < 4096) buf_size = 4096;
    uint8_t *out = malloc(buf_size);
    if (!out) return NULL;

    z_stream strm;
    memset(&strm, 0, sizeof(strm));
    /* windowBits = 15 + 32 to auto-detect gzip/zlib */
    if (inflateInit2(&strm, 15 + 32) != Z_OK) {
        free(out);
        return NULL;
    }

    strm.next_in = (uint8_t *)data;
    strm.avail_in = (uInt)data_len;
    strm.next_out = out;
    strm.avail_out = (uInt)buf_size;

    int ret;
    while ((ret = inflate(&strm, Z_NO_FLUSH)) != Z_STREAM_END) {
        if (ret == Z_OK && strm.avail_out == 0) {
            /* Need more output space */
            size_t new_size = buf_size * 2;
            uint8_t *tmp = realloc(out, new_size);
            if (!tmp) { inflateEnd(&strm); free(out); return NULL; }
            out = tmp;
            strm.next_out = out + buf_size;
            strm.avail_out = (uInt)(new_size - buf_size);
            buf_size = new_size;
        } else if (ret != Z_OK) {
            inflateEnd(&strm);
            free(out);
            return NULL;
        }
    }

    *out_len = strm.total_out;
    inflateEnd(&strm);
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

    /* Parse header */
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
        /* Check for sequence */
        if (msg_flags & FLAG_NEG_SEQUENCE) {
            if (offset + 4 > payload_len) return -1;
            resp->seq = read_u32_be(payload + offset);
            resp->has_seq = 1;
            offset += 4;
        }

        /* Check for event */
        if (msg_flags & FLAG_MSG_WITH_EVENT) {
            if (offset + 4 > payload_len) return -1;
            resp->event = (int)read_u32_be(payload + offset);
            resp->has_event = 1;
            offset += 4;
        }

        /* Session ID */
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

        /* Payload data */
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

        /* Decompress if gzip */
        uint8_t *decompressed = NULL;
        size_t decompressed_len = 0;
        if (compression == COMPRESS_GZIP) {
            decompressed = gzip_decompress(msg_data, msg_len, &decompressed_len);
            if (!decompressed) return -1;
        } else {
            decompressed_len = msg_len;
            decompressed = malloc(decompressed_len);
            if (!decompressed) return -1;
            memcpy(decompressed, msg_data, decompressed_len);
        }

        if (serial_method == SERIAL_JSON) {
            resp->is_binary = 0;
        } else if (serial_method == SERIAL_NONE) {
            resp->is_binary = 1;
        } else {
            resp->is_binary = 0;
        }

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
