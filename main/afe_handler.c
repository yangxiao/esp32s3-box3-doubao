#include "afe_handler.h"
#include "audio_hal.h"
#include <string.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/ringbuf.h"
#include "esp_log.h"
#include "esp_heap_caps.h"
#include "esp_afe_sr_iface.h"
#include "esp_afe_sr_models.h"
#include "esp_afe_config.h"

static const char *TAG = "afe_handler";

/* AFE state */
static const esp_afe_sr_iface_t *s_afe = NULL;
static esp_afe_sr_data_t *s_afe_data = NULL;
static srmodel_list_t *s_models = NULL;
static int s_feed_chunksize = 0;   /* total samples across all channels for feed() */
static int s_fetch_chunksize = 0;  /* single-channel output samples from fetch() */

/* Tasks */
static TaskHandle_t s_feed_task_handle = NULL;
static TaskHandle_t s_fetch_task_handle = NULL;
static volatile bool s_running = false;
static volatile bool s_streaming = false;

/* Callback */
static afe_wakeup_cb_t s_on_wakeup = NULL;
static void *s_wakeup_userdata = NULL;

/* Forward declaration */
static void afe_fetch_task(void *pvParameters);

/* ---- AFE Feed Task (Core 1, highest audio priority) ---- */

static void afe_feed_task(void *pvParameters) {
    const int total_ch = 3;  /* MMR: mic1, mic2, ref */
    int samples_per_ch = s_feed_chunksize / total_ch;

    /* Buffers in PSRAM */
    size_t mic_buf_bytes = samples_per_ch * 2 * sizeof(int16_t); /* 2-ch stereo from ES7210 */
    size_t ref_buf_bytes = samples_per_ch * sizeof(int16_t);
    size_t feed_buf_bytes = s_feed_chunksize * sizeof(int16_t);

    int16_t *mic_buf = heap_caps_malloc(mic_buf_bytes, MALLOC_CAP_SPIRAM);
    int16_t *ref_buf = heap_caps_malloc(ref_buf_bytes, MALLOC_CAP_SPIRAM);
    int16_t *feed_buf = heap_caps_malloc(feed_buf_bytes, MALLOC_CAP_SPIRAM);

    if (!mic_buf || !ref_buf || !feed_buf) {
        ESP_LOGE(TAG, "Failed to alloc feed buffers");
        free(mic_buf); free(ref_buf); free(feed_buf);
        vTaskDelete(NULL);
        return;
    }

    ESP_LOGI(TAG, "Feed task started: chunksize=%d, samples_per_ch=%d",
             s_feed_chunksize, samples_per_ch);

    /* Start fetch task before entering loop */
    ESP_LOGI(TAG, "Starting fetch task...");
    xTaskCreatePinnedToCore(afe_fetch_task, "afe_fetch", 3072, NULL, 22, &s_fetch_task_handle, 1);
    ESP_LOGI(TAG, "Fetch task started");

    RingbufHandle_t ref_rb = audio_hal_get_ref_rb();

    uint32_t feed_count = 0;
    while (s_running) {
        if (feed_count++ % 5000 == 0) {
            ESP_LOGI(TAG, "Feed loop alive, count=%lu", (unsigned long)feed_count);
        }

        /* Read 2-channel interleaved mic data */
        int ret = audio_hal_read_stereo(mic_buf, mic_buf_bytes, pdMS_TO_TICKS(200));
        if (ret <= 0) {
            ESP_LOGW(TAG, "audio_hal_read_stereo failed: %d", ret);
            vTaskDelay(pdMS_TO_TICKS(10));
            continue;
        }

        /* Get reference signal from ref_rb (non-blocking, fill zeros if not available) */
        memset(ref_buf, 0, ref_buf_bytes);
        if (ref_rb) {
            size_t ref_collected = 0;
            while (ref_collected < ref_buf_bytes) {
                size_t item_size;
                void *item = xRingbufferReceive(ref_rb, &item_size, 0);
                if (!item) break;
                size_t to_copy = ref_buf_bytes - ref_collected;
                if (to_copy > item_size) to_copy = item_size;
                memcpy((uint8_t *)ref_buf + ref_collected, item, to_copy);
                ref_collected += to_copy;
                vRingbufferReturnItem(ref_rb, item);
            }
        }

        /* Interleave: [mic1, mic2, ref, mic1, mic2, ref, ...] */
        for (int i = 0; i < samples_per_ch; i++) {
            feed_buf[i * 3 + 0] = mic_buf[i * 2 + 0];  /* MIC1 */
            feed_buf[i * 3 + 1] = mic_buf[i * 2 + 1];  /* MIC2 */
            feed_buf[i * 3 + 2] = ref_buf[i];           /* REF  */
        }

        /* Feed AFE */
        ESP_LOGD(TAG, "Calling afe->feed()...");
        s_afe->feed(s_afe_data, feed_buf);
        ESP_LOGD(TAG, "afe->feed() returned");
        vTaskDelay(1);
    }

    free(mic_buf);
    free(ref_buf);
    free(feed_buf);
    ESP_LOGI(TAG, "Feed task exiting");
    vTaskDelete(NULL);
}

/* ---- AFE Fetch Task (Core 0) ---- */

static void afe_fetch_task(void *pvParameters) {
    RingbufHandle_t capture_rb = audio_hal_get_capture_rb();

    ESP_LOGI(TAG, "Fetch task started: chunksize=%d samples", s_fetch_chunksize);

    uint32_t fetch_count = 0;
    while (s_running) {
        if (fetch_count++ % 5000 == 0) {
            ESP_LOGI(TAG, "Fetch loop alive, count=%lu", (unsigned long)fetch_count);
        }

        ESP_LOGD(TAG, "Calling afe->fetch()...");
        afe_fetch_result_t *res = s_afe->fetch(s_afe_data);
        ESP_LOGD(TAG, "afe->fetch() returned: res=%p", res);

        if (!res) {
            ESP_LOGW(TAG, "fetch returned NULL");
            vTaskDelay(pdMS_TO_TICKS(10));
            continue;
        }

        if (res->data_size <= 0) {
            ESP_LOGD(TAG, "fetch returned zero data_size");
            vTaskDelay(pdMS_TO_TICKS(10));
            continue;
        }

        /* Check wake word detection */
        if (res->wakeup_state == WAKENET_DETECTED) {
            ESP_LOGI(TAG, "Wake word detected! index=%d", res->wake_word_index);
            if (s_on_wakeup) {
                s_on_wakeup(res->wake_word_index, s_wakeup_userdata);
            }
        }

        /* Stream processed audio to capture ring buffer if enabled */
        if (s_streaming && res->data && res->data_size > 0 && capture_rb) {
            if (xRingbufferSend(capture_rb, res->data, res->data_size, pdMS_TO_TICKS(10)) != pdTRUE) {
                ESP_LOGD(TAG, "Capture buffer full");
            }
        }
    }

    ESP_LOGI(TAG, "Fetch task exiting");
    vTaskDelete(NULL);
}

/* ---- Public API ---- */

int afe_handler_init(const afe_handler_config_t *config) {
    ESP_LOGI(TAG, "Initializing AFE handler...");
    ESP_LOGI(TAG, "Free PSRAM before AFE: %lu",
             (unsigned long)heap_caps_get_free_size(MALLOC_CAP_SPIRAM));

    /* Load models from SPIFFS "model" partition */
    s_models = esp_srmodel_init("model");
    if (!s_models) {
        ESP_LOGE(TAG, "Failed to init SR models from partition");
        return -1;
    }
    ESP_LOGI(TAG, "Loaded %d SR models", s_models->num);
    for (int i = 0; i < s_models->num; i++) {
        ESP_LOGI(TAG, "  Model[%d]: %s", i, s_models->model_name[i]);
    }

    /* Find wake word model */
    char *wn_name = esp_srmodel_filter(s_models, ESP_WN_PREFIX, "hijason");
    if (!wn_name) {
        ESP_LOGW(TAG, "WakeNet model 'hijason' not found, trying any WakeNet");
        wn_name = esp_srmodel_filter(s_models, ESP_WN_PREFIX, NULL);
    }
    if (wn_name) {
        ESP_LOGI(TAG, "Using WakeNet model: %s", wn_name);
    } else {
        ESP_LOGW(TAG, "No WakeNet model found, wake word detection disabled");
    }

    /* Create AFE config: 2 mics + 1 reference */
    // afe_config_t *afe_config = afe_config_init("MMR", s_models, AFE_TYPE_SR, AFE_MODE_HIGH_PERF);
    // 暂时关闭 AEC 和高性能模式，看系统是否还卡住
    afe_config_t *afe_config = afe_config_init("MMR", s_models, AFE_TYPE_SR, AFE_MODE_LOW_COST);
    afe_config->aec_init = false;
    if (!afe_config) {
        ESP_LOGE(TAG, "afe_config_init failed");
        return -1;
    }

    /* Customize config */
    afe_config->memory_alloc_mode = AFE_MEMORY_ALLOC_MORE_PSRAM;
    afe_config->wakenet_init = (wn_name != NULL);
    if (wn_name) {
        afe_config->wakenet_model_name = wn_name;
    }
    afe_config->afe_perferred_core = 1;
    afe_config->afe_perferred_priority = 21;

    afe_config_print(afe_config);

    /* Get AFE interface handle */
    s_afe = esp_afe_handle_from_config(afe_config);
    if (!s_afe) {
        ESP_LOGE(TAG, "esp_afe_handle_from_config failed");
        afe_config_free(afe_config);
        return -1;
    }

    /* Create AFE instance */
    s_afe_data = s_afe->create_from_config(afe_config);
    if (!s_afe_data) {
        ESP_LOGE(TAG, "AFE create_from_config failed");
        afe_config_free(afe_config);
        return -1;
    }

    s_feed_chunksize = s_afe->get_feed_chunksize(s_afe_data);
    s_fetch_chunksize = s_afe->get_fetch_chunksize(s_afe_data);

    ESP_LOGI(TAG, "AFE created: feed_chunksize=%d, fetch_chunksize=%d",
             s_feed_chunksize, s_fetch_chunksize);
    ESP_LOGI(TAG, "Free PSRAM after AFE: %lu",
             (unsigned long)heap_caps_get_free_size(MALLOC_CAP_SPIRAM));

    /* Store callback */
    s_on_wakeup = config->on_wakeup;
    s_wakeup_userdata = config->wakeup_userdata;

    afe_config_free(afe_config);
    return 0;
}

void afe_handler_start(void) {
    if (s_running) return;
    s_running = true;
    s_streaming = false;

    /* Only start feed task, fetch task will be started from feed task */
    xTaskCreatePinnedToCore(afe_feed_task, "afe_feed", 3072, NULL, 23, &s_feed_task_handle, 1);
    ESP_LOGI(TAG, "AFE feed task started");
}

void afe_handler_set_streaming(bool enable) {
    s_streaming = enable;
    ESP_LOGI(TAG, "Streaming %s", enable ? "enabled" : "disabled");
}

void afe_handler_enable_wakenet(void) {
    if (s_afe && s_afe_data) {
        s_afe->enable_wakenet(s_afe_data);
        ESP_LOGI(TAG, "WakeNet enabled");
    }
}

void afe_handler_disable_wakenet(void) {
    if (s_afe && s_afe_data) {
        s_afe->disable_wakenet(s_afe_data);
        ESP_LOGI(TAG, "WakeNet disabled");
    }
}

void afe_handler_stop(void) {
    s_running = false;
    /* Tasks will self-delete after loop exits */
    vTaskDelay(pdMS_TO_TICKS(200));
    s_feed_task_handle = NULL;
    s_fetch_task_handle = NULL;
    ESP_LOGI(TAG, "AFE tasks stopped");
}

void afe_handler_deinit(void) {
    afe_handler_stop();
    if (s_afe && s_afe_data) {
        s_afe->destroy(s_afe_data);
        s_afe_data = NULL;
    }
    if (s_models) {
        esp_srmodel_deinit(s_models);
        s_models = NULL;
    }
    s_afe = NULL;
    ESP_LOGI(TAG, "AFE handler deinitialized");
}
