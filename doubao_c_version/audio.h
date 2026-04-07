#ifndef AUDIO_H
#define AUDIO_H

#include <portaudio.h>
#include <opus.h>
#include <ogg/ogg.h>
#include <pthread.h>
#include <stdbool.h>
#include <stdint.h>
#include <stddef.h>

/* Audio queue node */
typedef struct audio_node {
    uint8_t *data;
    size_t len;
    struct audio_node *next;
} audio_node_t;

/* Thread-safe audio queue */
typedef struct {
    audio_node_t *head;
    audio_node_t *tail;
    int count;
    pthread_mutex_t mutex;
    pthread_cond_t cond;
} audio_queue_t;

/* Audio manager */
typedef struct {
    /* PortAudio streams */
    PaStream *input_stream;
    PaStream *output_stream;

    /* Config */
    int input_sample_rate;
    int input_channels;
    int input_chunk;        /* frames per read */
    PaSampleFormat input_format;

    int output_sample_rate;
    int output_channels;
    int output_chunk;
    PaSampleFormat output_format;

    /* Opus encoder (mic PCM → Opus frames) */
    OpusEncoder *opus_encoder;

    /* Opus decoder (OGG/Opus → PCM for speaker) */
    OpusDecoder *opus_decoder;
    bool opus_decoder_inited;

    /* OGG demuxer state */
    ogg_sync_state ogg_sync;
    ogg_stream_state ogg_stream;
    bool ogg_stream_inited;
    bool ogg_headers_parsed;
    int ogg_header_count;

    /* Playback queue and thread */
    audio_queue_t play_queue;
    pthread_t player_thread;
    volatile bool playing;

    /* Raw OGG buffer for saving to file */
    uint8_t *ogg_buffer;
    size_t ogg_buffer_len;
    size_t ogg_buffer_cap;
} audio_manager_t;

/* Initialize audio manager. Returns 0 on success. */
int audio_init(audio_manager_t *am, const char *output_format);

/* Open input stream (microphone) */
int audio_open_input(audio_manager_t *am);

/* Open output stream (speaker) and start player thread */
int audio_open_output(audio_manager_t *am);

/* Read a chunk from microphone, encode to Opus.
 * Returns malloc'd Opus frame, sets *out_len. Caller must free. */
uint8_t *audio_read_input_opus(audio_manager_t *am, size_t *out_len);

/* Decode OGG/Opus data from server and enqueue decoded PCM for playback.
 * Also saves raw OGG data to internal buffer. */
void audio_decode_ogg_opus(audio_manager_t *am, const uint8_t *data, size_t len);

/* Enqueue raw PCM data for playback (used internally) */
void audio_enqueue(audio_manager_t *am, const uint8_t *data, size_t len);

/* Clear playback queue */
void audio_queue_clear(audio_manager_t *am);

/* Save accumulated OGG buffer to file */
void audio_save_output(audio_manager_t *am, const char *filename);

/* Reset OGG demuxer state (called on event 450 when audio queue is cleared) */
void audio_reset_ogg_state(audio_manager_t *am);

/* Cleanup and release resources */
void audio_cleanup(audio_manager_t *am);

#endif /* AUDIO_H */
