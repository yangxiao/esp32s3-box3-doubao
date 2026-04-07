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

/* Returns node or NULL. Waits up to timeout_ms. */
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
                Pa_WriteStream(am->output_stream, node->data,
                               node->len / (am->output_channels * Pa_GetSampleSize(am->output_format)));
            }
            free(node->data);
            free(node);
        }
    }
    return NULL;
}

/* ---- Output buffer accumulation ---- */

static void output_buffer_append(audio_manager_t *am, const uint8_t *data, size_t len) {
    if (am->output_buffer_len + len > am->output_buffer_cap) {
        size_t new_cap = (am->output_buffer_cap + len) * 2;
        if (new_cap < 65536) new_cap = 65536;
        uint8_t *tmp = realloc(am->output_buffer, new_cap);
        if (!tmp) return;
        am->output_buffer = tmp;
        am->output_buffer_cap = new_cap;
    }
    memcpy(am->output_buffer + am->output_buffer_len, data, len);
    am->output_buffer_len += len;
}

/* ---- Public API ---- */

int audio_init(audio_manager_t *am, const char *output_format) {
    memset(am, 0, sizeof(*am));

    am->input_sample_rate = INPUT_SAMPLE_RATE;
    am->input_channels = INPUT_CHANNELS;
    am->input_chunk = INPUT_CHUNK;
    am->input_format = paInt16;

    am->output_sample_rate = OUTPUT_SAMPLE_RATE;
    am->output_channels = OUTPUT_CHANNELS;
    am->output_chunk = OUTPUT_CHUNK;

    if (output_format && strcmp(output_format, "pcm_s16le") == 0) {
        am->output_format = paInt16;
    } else {
        am->output_format = paFloat32;
    }

    queue_init(&am->play_queue);
    am->playing = false;

    am->output_buffer = NULL;
    am->output_buffer_len = 0;
    am->output_buffer_cap = 0;

    PaError err = Pa_Initialize();
    if (err != paNoError) {
        fprintf(stderr, "PortAudio init error: %s\n", Pa_GetErrorText(err));
        return -1;
    }
    return 0;
}

int audio_open_input(audio_manager_t *am) {
    PaError err = Pa_OpenDefaultStream(
        &am->input_stream,
        am->input_channels,    /* input channels */
        0,                     /* output channels */
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
    printf("Microphone opened (rate=%d, channels=%d, chunk=%d)\n",
           am->input_sample_rate, am->input_channels, am->input_chunk);
    return 0;
}

int audio_open_output(audio_manager_t *am) {
    PaError err = Pa_OpenDefaultStream(
        &am->output_stream,
        0,                      /* input channels */
        am->output_channels,    /* output channels */
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

    /* Start player thread */
    am->playing = true;
    if (pthread_create(&am->player_thread, NULL, player_thread_func, am) != 0) {
        fprintf(stderr, "Failed to create player thread\n");
        return -1;
    }

    printf("Speaker opened (rate=%d, channels=%d, chunk=%d)\n",
           am->output_sample_rate, am->output_channels, am->output_chunk);
    return 0;
}

uint8_t *audio_read_input(audio_manager_t *am, size_t *out_len) {
    if (!am->input_stream) return NULL;
    size_t sample_size = Pa_GetSampleSize(am->input_format);
    size_t buf_size = am->input_chunk * am->input_channels * sample_size;
    uint8_t *buf = malloc(buf_size);
    if (!buf) return NULL;

    PaError err = Pa_ReadStream(am->input_stream, buf, am->input_chunk);
    if (err != paNoError && err != paInputOverflowed) {
        fprintf(stderr, "Read input error: %s\n", Pa_GetErrorText(err));
        free(buf);
        return NULL;
    }
    *out_len = buf_size;
    return buf;
}

void audio_enqueue(audio_manager_t *am, const uint8_t *data, size_t len) {
    queue_push(&am->play_queue, data, len);
    output_buffer_append(am, data, len);
}

void audio_queue_clear(audio_manager_t *am) {
    queue_clear(&am->play_queue);
}

void audio_save_output(audio_manager_t *am, const char *filename) {
    if (!am->output_buffer || am->output_buffer_len == 0) {
        printf("No audio data to save.\n");
        return;
    }
    FILE *f = fopen(filename, "wb");
    if (!f) {
        fprintf(stderr, "Failed to open %s for writing\n", filename);
        return;
    }
    fwrite(am->output_buffer, 1, am->output_buffer_len, f);
    fclose(f);
    printf("Saved %zu bytes to %s\n", am->output_buffer_len, filename);
}

void audio_cleanup(audio_manager_t *am) {
    am->playing = false;
    /* Wake up player thread if waiting */
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

    queue_destroy(&am->play_queue);
    free(am->output_buffer);
    am->output_buffer = NULL;
}
