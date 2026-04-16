#ifndef AUDIO_HAL_H
#define AUDIO_HAL_H

#include <stdint.h>
#include <stddef.h>
#include "freertos/FreeRTOS.h"
#include "freertos/ringbuf.h"

/* Audio configuration */
#define AUDIO_INPUT_SAMPLE_RATE     16000
#define AUDIO_INPUT_CHANNELS        2       /* Stereo: 2 mics from ES7210 */
#define AUDIO_INPUT_BITS            16

#define AUDIO_OUTPUT_SAMPLE_RATE    16000
#define AUDIO_OUTPUT_CHANNELS       1
#define AUDIO_OUTPUT_BITS           16

/* Ring buffer sizes (allocated in PSRAM) */
#define CAPTURE_RB_SIZE     (32 * 1024)   /* 32KB — AFE clean audio output */
#define PLAYBACK_RB_SIZE    (128 * 1024)  /* 128KB ~ 4s @ 16kHz/16bit/mono */
#define REF_RB_SIZE         (32 * 1024)   /* 32KB — AEC playback reference */

/**
 * Initialize BSP board, I2S (stereo for dual-mic), and audio codec.
 * Creates capture, playback, and reference ring buffers in PSRAM.
 */
int audio_hal_init(void);

/**
 * Get capture ring buffer handle (AFE processed audio written here).
 */
RingbufHandle_t audio_hal_get_capture_rb(void);

/**
 * Get playback ring buffer handle (decoded PCM written here, I2S reads from it).
 */
RingbufHandle_t audio_hal_get_playback_rb(void);

/**
 * Get AEC reference ring buffer handle (playback PCM copied here for AFE).
 */
RingbufHandle_t audio_hal_get_ref_rb(void);

/**
 * Read stereo PCM samples from microphone (2-channel interleaved).
 * Returns number of bytes read.
 */
int audio_hal_read_stereo(int16_t *buf, size_t bytes, TickType_t timeout);

/**
 * Write PCM samples to speaker (blocking).
 * Returns number of bytes written.
 */
int audio_hal_write(const int16_t *buf, size_t bytes, TickType_t timeout);

/**
 * Clear playback buffer (for barge-in / interrupt).
 */
void audio_hal_clear_playback(void);

/**
 * Clear capture buffer.
 */
void audio_hal_clear_capture(void);

/**
 * Clear AEC reference buffer.
 */
void audio_hal_clear_ref(void);

/**
 * Deinitialize audio hardware.
 */
void audio_hal_deinit(void);

#endif /* AUDIO_HAL_H */
