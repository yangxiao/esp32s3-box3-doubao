#include "opus_processor.h"
#include <string.h>
#include <stdlib.h>
#include "esp_log.h"
#include "esp_heap_caps.h"

static const char *TAG = "opus_proc";

int opus_proc_init(opus_processor_t *proc) {
    memset(proc, 0, sizeof(*proc));

    /* Init Opus encoder: 16kHz mono, VOIP application */
    int err;
    proc->encoder = opus_encoder_create(OPUS_ENC_SAMPLE_RATE, OPUS_ENC_CHANNELS,
                                         OPUS_APPLICATION_VOIP, &err);
    if (err != OPUS_OK || !proc->encoder) {
        ESP_LOGE(TAG, "Opus encoder create failed: %s", opus_strerror(err));
        return -1;
    }
    opus_encoder_ctl(proc->encoder, OPUS_SET_BITRATE(OPUS_ENC_BITRATE));
    opus_encoder_ctl(proc->encoder, OPUS_SET_COMPLEXITY(5));  /* balanced for ESP32-S3 */
    ESP_LOGI(TAG, "Opus encoder: %dHz, %dch, %dbps, complexity=5",
             OPUS_ENC_SAMPLE_RATE, OPUS_ENC_CHANNELS, OPUS_ENC_BITRATE);

    /* Init Opus decoder: 48kHz mono */
    proc->decoder = opus_decoder_create(OPUS_DEC_SAMPLE_RATE, OPUS_DEC_CHANNELS, &err);
    if (err != OPUS_OK || !proc->decoder) {
        ESP_LOGE(TAG, "Opus decoder create failed: %s", opus_strerror(err));
        opus_encoder_destroy(proc->encoder);
        return -1;
    }
    ESP_LOGI(TAG, "Opus decoder: %dHz, %dch", OPUS_DEC_SAMPLE_RATE, OPUS_DEC_CHANNELS);

    /* Init OGG sync state */
    ogg_sync_init(&proc->ogg_sync);
    proc->ogg_stream_inited = false;
    proc->ogg_headers_parsed = false;
    proc->ogg_header_count = 0;

    /* Allocate decode buffer in PSRAM (too large for stack) */
    proc->decode_buf = heap_caps_malloc(OPUS_DEC_MAX_FRAME * sizeof(int16_t),
                                         MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    if (!proc->decode_buf) {
        proc->decode_buf = malloc(OPUS_DEC_MAX_FRAME * sizeof(int16_t));
        if (!proc->decode_buf) {
            ESP_LOGE(TAG, "Failed to alloc decode buffer");
            opus_decoder_destroy(proc->decoder);
            opus_encoder_destroy(proc->encoder);
            return -1;
        }
    }

    return 0;
}

void opus_proc_set_output_cb(opus_processor_t *proc, opus_pcm_output_cb_t cb, void *userdata) {
    proc->pcm_cb = cb;
    proc->pcm_cb_userdata = userdata;
}

int opus_proc_encode(opus_processor_t *proc, const int16_t *pcm,
                     uint8_t *opus_out, size_t max_out) {
    if (!proc->encoder) return -1;

    int encoded = opus_encode(proc->encoder, pcm, OPUS_ENC_FRAME_SIZE,
                              opus_out, (opus_int32)max_out);
    if (encoded < 0) {
        ESP_LOGE(TAG, "Opus encode error: %s", opus_strerror(encoded));
        return -1;
    }
    return encoded;
}

void opus_proc_decode_ogg(opus_processor_t *proc, const uint8_t *data, size_t len) {
    if (!data || len == 0 || !proc->decoder) return;

    /* Feed data to OGG sync layer */
    char *ogg_buf = ogg_sync_buffer(&proc->ogg_sync, (long)len);
    if (!ogg_buf) return;
    memcpy(ogg_buf, data, len);
    ogg_sync_wrote(&proc->ogg_sync, (long)len);

    /* Process OGG pages */
    ogg_page page;
    while (ogg_sync_pageout(&proc->ogg_sync, &page) == 1) {
        /* Initialize stream on first page */
        if (!proc->ogg_stream_inited) {
            int serial = ogg_page_serialno(&page);
            ogg_stream_init(&proc->ogg_stream, serial);
            proc->ogg_stream_inited = true;
            proc->ogg_headers_parsed = false;
            proc->ogg_header_count = 0;
        }

        /* Check for new logical stream (new serial number) */
        if (ogg_page_bos(&page) && proc->ogg_stream_inited) {
            int new_serial = ogg_page_serialno(&page);
            if (new_serial != proc->ogg_stream.serialno) {
                ogg_stream_clear(&proc->ogg_stream);
                ogg_stream_init(&proc->ogg_stream, new_serial);
                proc->ogg_headers_parsed = false;
                proc->ogg_header_count = 0;
            }
        }

        ogg_stream_pagein(&proc->ogg_stream, &page);

        /* Process OGG packets */
        ogg_packet packet;
        while (ogg_stream_packetout(&proc->ogg_stream, &packet) == 1) {
            /* Skip OGG/Opus header packets (first 2: OpusHead + OpusTags) */
            if (!proc->ogg_headers_parsed) {
                proc->ogg_header_count++;
                if (proc->ogg_header_count >= 2) {
                    proc->ogg_headers_parsed = true;
                }
                continue;
            }

            /* Decode Opus packet to PCM */
            int decoded = opus_decode(proc->decoder,
                                       packet.packet, (opus_int32)packet.bytes,
                                       proc->decode_buf, OPUS_DEC_MAX_FRAME, 0);
            if (decoded > 0) {
                if (proc->pcm_cb) {
                    proc->pcm_cb(proc->decode_buf, (size_t)decoded, proc->pcm_cb_userdata);
                }
            } else if (decoded < 0) {
                ESP_LOGE(TAG, "Opus decode error: %s", opus_strerror(decoded));
            }
        }
    }
}

void opus_proc_reset_ogg(opus_processor_t *proc) {
    if (proc->ogg_stream_inited) {
        ogg_stream_clear(&proc->ogg_stream);
        proc->ogg_stream_inited = false;
    }
    ogg_sync_clear(&proc->ogg_sync);
    ogg_sync_init(&proc->ogg_sync);
    proc->ogg_headers_parsed = false;
    proc->ogg_header_count = 0;

    if (proc->decoder) {
        opus_decoder_ctl(proc->decoder, OPUS_RESET_STATE);
    }
    ESP_LOGI(TAG, "OGG state reset");
}

void opus_proc_cleanup(opus_processor_t *proc) {
    if (proc->encoder) {
        opus_encoder_destroy(proc->encoder);
        proc->encoder = NULL;
    }
    if (proc->decoder) {
        opus_decoder_destroy(proc->decoder);
        proc->decoder = NULL;
    }
    free(proc->decode_buf);
    proc->decode_buf = NULL;
    if (proc->ogg_stream_inited) {
        ogg_stream_clear(&proc->ogg_stream);
        proc->ogg_stream_inited = false;
    }
    ogg_sync_clear(&proc->ogg_sync);
    ESP_LOGI(TAG, "Opus processor cleaned up");
}
