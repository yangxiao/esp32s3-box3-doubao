#ifndef AFE_HANDLER_H
#define AFE_HANDLER_H

#include <stdbool.h>
#include <stdint.h>

/* Callback invoked when wake word is detected (from afe_fetch_task context) */
typedef void (*afe_wakeup_cb_t)(int wake_word_index, void *userdata);

typedef struct {
    afe_wakeup_cb_t on_wakeup;
    void *wakeup_userdata;
} afe_handler_config_t;

/**
 * Initialize AFE: load models, create AFE instance with "MMR" (2 mic + 1 ref).
 * Must be called after audio_hal_init().
 */
int afe_handler_init(const afe_handler_config_t *config);

/**
 * Start AFE feed and fetch tasks.
 */
void afe_handler_start(void);

/**
 * Enable/disable streaming processed audio to capture ring buffer.
 */
void afe_handler_set_streaming(bool enable);

/**
 * Enable/disable wake word detection.
 */
void afe_handler_enable_wakenet(void);
void afe_handler_disable_wakenet(void);

/**
 * Stop AFE tasks.
 */
void afe_handler_stop(void);

/**
 * Deinitialize and free AFE resources.
 */
void afe_handler_deinit(void);

#endif /* AFE_HANDLER_H */
