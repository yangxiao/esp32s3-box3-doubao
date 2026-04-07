#include "audio.h"
#include "config.h"
#include <stdlib.h>
#include <string.h>
#include <stdio.h>
#include <unistd.h>

/* ---- Audio queue operations ---- */

static void queue_init(audio_queue_t *q) {
    q->head = NULL;
    q->tail = NULL;
    q->count = 0;
    pthread_mutex_init(&q->mutex, NULL);
    pthread_cond_init(&q->cond, NULL);
}

static void queue_destroy(audio_queue_t *q) {
    pthread_mutex_lock(&q->mutex);
    audio_node_t *node = q->head;
    while (node) {
        audio_node_t *next = node->next;
        free(node->data);
        free(node);
        node = next;
    }
    q->head = q->tail = NULL;
    q->count = 0;
    pthread_mutex_unlock(&q->mutex);
    pthread_mutex_destroy(&q->mutex);
    pthread_cond_destroy(&q->cond);
}

static void queue_push(audio_queue_t *q, const uint8_t *data, size_t len) {
    audio_node_t *node = malloc(sizeof(audio_node_t));
    if (!node) return;
    node->data = malloc(len);
    if (!node->data) { free(node); return; }
    memcpy(node->data, data, len);
    node->len = len;
    node->next = NULL;

    pthread_mutex_lock(&q->mutex);
    if (q->tail) {
        q->tail->next = node;
    } else {
        q->head = node;
    }
    q->tail = node;
    q->count++;
    pthread_cond_signal(&q->cond);
    pthread_mutex_unlock(&q->mutex);
}

static audio_node_t *queue_pop(audio_queue_t *q, int timeout_ms) {
    pthread_mutex_lock(&q->mutex);
    if (!q->head && timeout_ms > 0) {
        struct timespec ts;
        clock_gettime(CLOCK_REALTIME, &ts);
        ts.tv_sec += timeout_ms / 1000;
        ts.tv_nsec += (timeout_ms % 1000) * 1000000;
        if (ts.tv_nsec >= 1000000000) {
            ts.tv_sec++;
            ts.tv_nsec -= 1000000000;
        }
        pthread_cond_timedwait(&q->cond, &q->mutex, &ts);
    }

    audio_node_t *node = q->head;
    if (node) {
        q->head = node->next;
        if (!q->head) q->tail = NULL;
        q->count--;
    }
    pthread_mutex_unlock(&q->mutex);
    return node;
}

static void queue_clear(audio_queue_t *q) {
    pthread_mutex_lock(&q->mutex);
    audio_node_t *node = q->head;
    while (node) {
        audio_node_t *next = node->next;
        free(node->data);
        free(node);
        node = next;
    }
    q->head = q->tail = NULL;
    q->count = 0;
    pthread_mutex_unlock(&q->mutex);
}

/* ---- Player thread ---- */

static void *player_thread_func(void *arg) {
    audio_manager_t *am = (audio_manager_t *)arg;
    while (am->playing) {
        audio_node_t *node = queue_pop(&am->play_queue, 500);
        if (node) {
            if (am->output_stream) {
                long frames = (long)(node->len / (am->output_channels * Pa_GetSampleSize(am->output_format)));
                Pa_WriteStream(am->output_stream, node->data, frames);
            }
            free(node->data);
            free(node);
        }
    }
    return NULL;
}

/* ---- OGG buffer for saving raw data ---- */

static void ogg_buffer_append(audio_manager_t *am, const uint8_t *data, size_t len) {
    if (am->ogg_buffer_len + len > am->ogg_buffer_cap) {
        size_t new_cap = (am->ogg_buffer_cap + len) * 2;
        if (new_cap < 65536) new_cap = 65536;
        uint8_t *tmp = realloc(am->ogg_buffer, new_cap);
        if (!tmp) return;
        am->ogg_buffer = tmp;
        am->ogg_buffer_cap = new_cap;
    }
    memcpy(am->ogg_buffer + am->ogg_buffer_len, data, len);
    am->ogg_buffer_len += len;
}

/* ---- Public API ---- */

int audio_init(audio_manager_t *am, const char *output_format) {
    memset(am, 0, sizeof(*am));

    am->input_sample_rate = INPUT_SAMPLE_RATE;
    am->input_channels = INPUT_CHANNELS;
    am->input_chunk = OPUS_FRAME_SIZE; /* read exactly one Opus frame worth */
    am->input_format = paInt16;

    /* Opus decodes to 48kHz by default */
    am->output_sample_rate = OUTPUT_SAMPLE_RATE; /* 48000 */
    am->output_channels = OUTPUT_CHANNELS;
    am->output_chunk = OUTPUT_CHUNK;
    am->output_format = paInt16;

    queue_init(&am->play_queue);
    am->playing = false;

    am->ogg_buffer = NULL;
    am->ogg_buffer_len = 0;
    am->ogg_buffer_cap = 0;

    /* Init Opus encoder: 16kHz mono, VOIP application */
    int err;
    am->opus_encoder = opus_encoder_create(INPUT_SAMPLE_RATE, INPUT_CHANNELS,
                                            OPUS_APPLICATION_VOIP, &err);
    if (err != OPUS_OK || !am->opus_encoder) {
        fprintf(stderr, "Opus encoder create failed: %s\n", opus_strerror(err));
        return -1;
    }
    /* Set bitrate to 32kbps for good voice quality */
    opus_encoder_ctl(am->opus_encoder, OPUS_SET_BITRATE(32000));

    /* Init Opus decoder: 48kHz mono (Opus standard output rate) */
    am->opus_decoder = opus_decoder_create(48000, 1, &err);
    if (err != OPUS_OK || !am->opus_decoder) {
        fprintf(stderr, "Opus decoder create failed: %s\n", opus_strerror(err));
        opus_encoder_destroy(am->opus_encoder);
        return -1;
    }
    am->opus_decoder_inited = true;

    /* Init OGG sync state */
    ogg_sync_init(&am->ogg_sync);
    am->ogg_stream_inited = false;
    am->ogg_headers_parsed = false;
    am->ogg_header_count = 0;

    PaError pa_err = Pa_Initialize();
    if (pa_err != paNoError) {
        fprintf(stderr, "PortAudio init error: %s\n", Pa_GetErrorText(pa_err));
        return -1;
    }
    return 0;
}

int audio_open_input(audio_manager_t *am) {
    PaError err = Pa_OpenDefaultStream(
        &am->input_stream,
        am->input_channels, 0,
        am->input_format,
        am->input_sample_rate,
        am->input_chunk,
        NULL, NULL);
    if (err != paNoError) {
        fprintf(stderr, "Failed to open input stream: %s\n", Pa_GetErrorText(err));
        return -1;
    }
    err = Pa_StartStream(am->input_stream);
    if (err != paNoError) {
        fprintf(stderr, "Failed to start input stream: %s\n", Pa_GetErrorText(err));
        return -1;
    }
    printf("Microphone opened (rate=%d, channels=%d, opus_frame=%d)\n",
           am->input_sample_rate, am->input_channels, am->input_chunk);
    return 0;
}

int audio_open_output(audio_manager_t *am) {
    PaError err = Pa_OpenDefaultStream(
        &am->output_stream,
        0, am->output_channels,
        am->output_format,
        am->output_sample_rate,
        am->output_chunk,
        NULL, NULL);
    if (err != paNoError) {
        fprintf(stderr, "Failed to open output stream: %s\n", Pa_GetErrorText(err));
        return -1;
    }
    err = Pa_StartStream(am->output_stream);
    if (err != paNoError) {
        fprintf(stderr, "Failed to start output stream: %s\n", Pa_GetErrorText(err));
        return -1;
    }

    am->playing = true;
    if (pthread_create(&am->player_thread, NULL, player_thread_func, am) != 0) {
        fprintf(stderr, "Failed to create player thread\n");
        return -1;
    }

    printf("Speaker opened (rate=%d, channels=%d, format=int16, chunk=%d)\n",
           am->output_sample_rate, am->output_channels, am->output_chunk);
    return 0;
}

uint8_t *audio_read_input_opus(audio_manager_t *am, size_t *out_len) {
    if (!am->input_stream || !am->opus_encoder) return NULL;

    /* Read OPUS_FRAME_SIZE frames of int16 mono PCM */
    int16_t pcm_buf[OPUS_FRAME_SIZE];
    PaError err = Pa_ReadStream(am->input_stream, pcm_buf, OPUS_FRAME_SIZE);
    if (err != paNoError && err != paInputOverflowed) {
        fprintf(stderr, "Read input error: %s\n", Pa_GetErrorText(err));
        return NULL;
    }

    /* Encode to Opus */
    uint8_t *opus_buf = malloc(4000); /* max Opus frame size */
    if (!opus_buf) return NULL;

    int encoded = opus_encode(am->opus_encoder, pcm_buf, OPUS_FRAME_SIZE,
                              opus_buf, 4000);
    if (encoded < 0) {
        fprintf(stderr, "Opus encode error: %s\n", opus_strerror(encoded));
        free(opus_buf);
        return NULL;
    }

    *out_len = (size_t)encoded;
    return opus_buf;
}

void audio_decode_ogg_opus(audio_manager_t *am, const uint8_t *data, size_t len) {
    if (!data || len == 0) return;

    /* Save raw OGG data for debug */
    ogg_buffer_append(am, data, len);

    /* Feed data to OGG sync layer */
    char *ogg_buf = ogg_sync_buffer(&am->ogg_sync, (long)len);
    if (!ogg_buf) return;
    memcpy(ogg_buf, data, len);
    ogg_sync_wrote(&am->ogg_sync, (long)len);

    /* Process OGG pages */
    ogg_page page;
    while (ogg_sync_pageout(&am->ogg_sync, &page) == 1) {
        /* Initialize stream on first page */
        if (!am->ogg_stream_inited) {
            int serial = ogg_page_serialno(&page);
            ogg_stream_init(&am->ogg_stream, serial);
            am->ogg_stream_inited = true;
            am->ogg_headers_parsed = false;
            am->ogg_header_count = 0;
        }

        /* Check for new logical stream (new serial number) */
        if (ogg_page_bos(&page) && am->ogg_stream_inited) {
            int new_serial = ogg_page_serialno(&page);
            if (new_serial != am->ogg_stream.serialno) {
                ogg_stream_clear(&am->ogg_stream);
                ogg_stream_init(&am->ogg_stream, new_serial);
                am->ogg_headers_parsed = false;
                am->ogg_header_count = 0;
            }
        }

        ogg_stream_pagein(&am->ogg_stream, &page);

        /* Process OGG packets */
        ogg_packet packet;
        while (ogg_stream_packetout(&am->ogg_stream, &packet) == 1) {
            /* Skip OGG/Opus header packets (first 2: OpusHead + OpusTags) */
            if (!am->ogg_headers_parsed) {
                am->ogg_header_count++;
                if (am->ogg_header_count >= 2) {
                    am->ogg_headers_parsed = true;
                }
                continue;
            }

            /* Decode Opus packet to PCM */
            /* Max frame: 120ms @ 48kHz = 5760 samples */
            int16_t pcm_out[5760];
            int decoded = opus_decode(am->opus_decoder,
                                       packet.packet, (opus_int32)packet.bytes,
                                       pcm_out, 5760, 0);
            if (decoded > 0) {
                size_t pcm_bytes = (size_t)decoded * sizeof(int16_t);
                queue_push(&am->play_queue, (const uint8_t *)pcm_out, pcm_bytes);
            } else if (decoded < 0) {
                fprintf(stderr, "Opus decode error: %s\n", opus_strerror(decoded));
            }
        }
    }
}

void audio_enqueue(audio_manager_t *am, const uint8_t *data, size_t len) {
    queue_push(&am->play_queue, data, len);
}

void audio_queue_clear(audio_manager_t *am) {
    queue_clear(&am->play_queue);
}

void audio_reset_ogg_state(audio_manager_t *am) {
    /* Reset OGG demuxer for new audio stream */
    if (am->ogg_stream_inited) {
        ogg_stream_clear(&am->ogg_stream);
        am->ogg_stream_inited = false;
    }
    ogg_sync_clear(&am->ogg_sync);
    ogg_sync_init(&am->ogg_sync);
    am->ogg_headers_parsed = false;
    am->ogg_header_count = 0;

    /* Reset decoder state */
    if (am->opus_decoder) {
        opus_decoder_ctl(am->opus_decoder, OPUS_RESET_STATE);
    }
}

void audio_save_output(audio_manager_t *am, const char *filename) {
    if (!am->ogg_buffer || am->ogg_buffer_len == 0) {
        printf("No audio data to save.\n");
        return;
    }
    FILE *f = fopen(filename, "wb");
    if (!f) {
        fprintf(stderr, "Failed to open %s for writing\n", filename);
        return;
    }
    fwrite(am->ogg_buffer, 1, am->ogg_buffer_len, f);
    fclose(f);
    printf("Saved %zu bytes to %s\n", am->ogg_buffer_len, filename);
}

void audio_cleanup(audio_manager_t *am) {
    am->playing = false;
    pthread_cond_signal(&am->play_queue.cond);
    pthread_join(am->player_thread, NULL);

    if (am->input_stream) {
        Pa_StopStream(am->input_stream);
        Pa_CloseStream(am->input_stream);
    }
    if (am->output_stream) {
        Pa_StopStream(am->output_stream);
        Pa_CloseStream(am->output_stream);
    }
    Pa_Terminate();

    if (am->opus_encoder) {
        opus_encoder_destroy(am->opus_encoder);
        am->opus_encoder = NULL;
    }
    if (am->opus_decoder) {
        opus_decoder_destroy(am->opus_decoder);
        am->opus_decoder = NULL;
    }

    if (am->ogg_stream_inited) {
        ogg_stream_clear(&am->ogg_stream);
    }
    ogg_sync_clear(&am->ogg_sync);

    queue_destroy(&am->play_queue);
    free(am->ogg_buffer);
    am->ogg_buffer = NULL;
}
