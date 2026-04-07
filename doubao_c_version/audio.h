#ifndef AUDIO_H
#define AUDIO_H

#include <portaudio.h>
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

    /* Playback queue and thread */
    audio_queue_t play_queue;
    pthread_t player_thread;
    volatile bool playing;

    /* Output PCM buffer for saving */
    uint8_t *output_buffer;
    size_t output_buffer_len;
    size_t output_buffer_cap;
} audio_manager_t;

/* Initialize audio manager with default config. Returns 0 on success. */
int audio_init(audio_manager_t *am, const char *output_format);

/* Open input stream (microphone) */
int audio_open_input(audio_manager_t *am);

/* Open output stream (speaker) and start player thread */
int audio_open_output(audio_manager_t *am);

/* Read a chunk from microphone. Returns malloc'd buffer, sets *out_len.
 * Caller must free. */
uint8_t *audio_read_input(audio_manager_t *am, size_t *out_len);

/* Enqueue audio data for playback */
void audio_enqueue(audio_manager_t *am, const uint8_t *data, size_t len);

/* Clear playback queue */
void audio_queue_clear(audio_manager_t *am);

/* Save accumulated output buffer to file */
void audio_save_output(audio_manager_t *am, const char *filename);

/* Cleanup and release resources */
void audio_cleanup(audio_manager_t *am);

#endif /* AUDIO_H */
