#ifndef OPUS_PROCESSOR_H
#define OPUS_PROCESSOR_H

#include <stdint.h>
#include <stddef.h>
#include <stdbool.h>
#include <opus.h>
#include <ogg/ogg.h>

/* Opus encoder config: 960 frames = 60ms @ 16kHz */
#define OPUS_ENC_FRAME_SIZE     960
#define OPUS_ENC_SAMPLE_RATE    16000
#define OPUS_ENC_CHANNELS       1
#define OPUS_ENC_BITRATE        32000
#define OPUS_ENC_MAX_OUT        4000

/* Opus decoder config */
#define OPUS_DEC_SAMPLE_RATE    16000
#define OPUS_DEC_CHANNELS       1
#define OPUS_DEC_MAX_FRAME      1920    /* max 120ms @ 16kHz */

/* Callback for decoded PCM output */
typedef void (*opus_pcm_output_cb_t)(const int16_t *pcm, size_t samples, void *userdata);

typedef struct {
    /* Encoder */
    OpusEncoder *encoder;

    /* Decoder */
    OpusDecoder *decoder;

    /* OGG demuxer state */
    ogg_sync_state ogg_sync;
    ogg_stream_state ogg_stream;
    bool ogg_stream_inited;
    bool ogg_headers_parsed;
    int ogg_header_count;

    /* Decode output buffer (heap-allocated to avoid stack overflow) */
    int16_t *decode_buf;

    /* PCM output callback */
    opus_pcm_output_cb_t pcm_cb;
    void *pcm_cb_userdata;
} opus_processor_t;

/**
 * Initialize Opus encoder and decoder.
 */
int opus_proc_init(opus_processor_t *proc);

/**
 * Set callback for decoded PCM output.
 */
void opus_proc_set_output_cb(opus_processor_t *proc, opus_pcm_output_cb_t cb, void *userdata);

/**
 * Encode one frame of PCM to Opus.
 * pcm: OPUS_ENC_FRAME_SIZE samples of 16-bit mono PCM.
 * opus_out: buffer of at least OPUS_ENC_MAX_OUT bytes.
 * Returns encoded size in bytes, or -1 on error.
 */
int opus_proc_encode(opus_processor_t *proc, const int16_t *pcm,
                     uint8_t *opus_out, size_t max_out);

/**
 * Decode OGG/Opus data from server.
 * Calls pcm_cb for each decoded PCM block.
 */
void opus_proc_decode_ogg(opus_processor_t *proc, const uint8_t *data, size_t len);

/**
 * Reset OGG demuxer state (called on barge-in / event 450).
 */
void opus_proc_reset_ogg(opus_processor_t *proc);

/**
 * Cleanup and release resources.
 */
void opus_proc_cleanup(opus_processor_t *proc);

#endif /* OPUS_PROCESSOR_H */
