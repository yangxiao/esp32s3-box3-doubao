#ifndef AUDIO_HAL_H
#define AUDIO_HAL_H

#include <stdint.h>
#include <stddef.h>
#include "freertos/FreeRTOS.h"
#include "freertos/ringbuf.h"

/* Audio configuration */
#define AUDIO_INPUT_SAMPLE_RATE     16000
#define AUDIO_INPUT_CHANNELS        1
#define AUDIO_INPUT_BITS            16

#define AUDIO_OUTPUT_SAMPLE_RATE    16000
#define AUDIO_OUTPUT_CHANNELS       1
#define AUDIO_OUTPUT_BITS           16

/* Ring buffer sizes (allocated in PSRAM) */
#define CAPTURE_RB_SIZE     (32 * 1024)   /* 32KB ~ 1s @ 16kHz/16bit/mono */
#define PLAYBACK_RB_SIZE    (32 * 1024)   /* 32KB ~ 1s @ 16kHz/16bit/mono */

/**
 * Initialize BSP board, I2S, and audio codec.
 * Creates capture and playback ring buffers in PSRAM.
 */
int audio_hal_init(void);

/**
 * Get capture ring buffer handle (I2S mic data written here).
 */
RingbufHandle_t audio_hal_get_capture_rb(void);

/**
 * Get playback ring buffer handle (decoded PCM written here, I2S reads from it).
 */
RingbufHandle_t audio_hal_get_playback_rb(void);

/**
 * Read PCM samples from microphone (blocking).
 * Returns number of bytes read.
 */
int audio_hal_read(int16_t *buf, size_t bytes, TickType_t timeout);

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
 * Deinitialize audio hardware.
 */
void audio_hal_deinit(void);

#endif /* AUDIO_HAL_H */
