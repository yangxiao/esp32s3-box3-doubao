#include "audio_hal.h"
#include "esp_log.h"
#include "esp_heap_caps.h"
#include "bsp/esp-bsp.h"
#include "esp_codec_dev.h"
#include "driver/i2s_std.h"

static const char *TAG = "audio_hal";

static RingbufHandle_t capture_rb = NULL;
static RingbufHandle_t playback_rb = NULL;
static esp_codec_dev_handle_t play_dev = NULL;
static esp_codec_dev_handle_t rec_dev = NULL;

int audio_hal_init(void) {
    ESP_LOGI(TAG, "Initializing audio HAL...");
    esp_err_t err;

    /*
     * BSP initialization chain:
     *   1. bsp_i2c_init()   — I2C bus for codec control
     *   2. bsp_audio_init() — I2S peripheral (TX + RX channels)
     *   3. bsp_audio_codec_speaker_init()    — ES8311 codec (playback)
     *   4. bsp_audio_codec_microphone_init() — ES7210 ADC (capture)
     *
     * bsp_audio_codec_*_init() will call bsp_i2c_init + bsp_audio_init(NULL)
     * internally if not already done. We call bsp_audio_init() explicitly
     * with a custom I2S config to set 16kHz initial rate. The actual sample
     * rate is re-configured by esp_codec_dev_open() per device.
     */

    /* Step 1: Initialize I2C */
    err = bsp_i2c_init();
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "BSP I2C init failed: %s", esp_err_to_name(err));
        return -1;
    }

    /* Step 2: Initialize I2S with 16kHz config (codec_dev_open will adjust) */
    err = bsp_audio_init(NULL);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "BSP audio init failed: %s", esp_err_to_name(err));
        return -1;
    }

    /* Step 3: Initialize speaker codec (ES8311) */
    play_dev = bsp_audio_codec_speaker_init();
    if (!play_dev) {
        ESP_LOGE(TAG, "Failed to init speaker codec");
        return -1;
    }

    esp_codec_dev_sample_info_t play_sample = {
        .sample_rate = AUDIO_OUTPUT_SAMPLE_RATE,
        .channel = AUDIO_OUTPUT_CHANNELS,
        .bits_per_sample = AUDIO_OUTPUT_BITS,
    };
    err = esp_codec_dev_open(play_dev, &play_sample);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "Failed to open speaker: %s", esp_err_to_name(err));
        return -1;
    }
    esp_codec_dev_set_out_vol(play_dev, 80);
    ESP_LOGI(TAG, "Speaker: %dHz, %dch, %dbit",
             AUDIO_OUTPUT_SAMPLE_RATE, AUDIO_OUTPUT_CHANNELS, AUDIO_OUTPUT_BITS);

    /* Step 4: Initialize microphone codec (ES7210) */
    rec_dev = bsp_audio_codec_microphone_init();
    if (!rec_dev) {
        ESP_LOGE(TAG, "Failed to init microphone codec");
        return -1;
    }

    esp_codec_dev_sample_info_t rec_sample = {
        .sample_rate = AUDIO_INPUT_SAMPLE_RATE,
        .channel = AUDIO_INPUT_CHANNELS,
        .bits_per_sample = AUDIO_INPUT_BITS,
    };
    err = esp_codec_dev_open(rec_dev, &rec_sample);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "Failed to open microphone: %s", esp_err_to_name(err));
        return -1;
    }
    esp_codec_dev_set_in_gain(rec_dev, 48.0);
    ESP_LOGI(TAG, "Microphone: %dHz, %dch, %dbit",
             AUDIO_INPUT_SAMPLE_RATE, AUDIO_INPUT_CHANNELS, AUDIO_INPUT_BITS);

    /* Step 5: Create ring buffers in PSRAM */
    StaticRingbuffer_t *cap_rb_struct = heap_caps_malloc(sizeof(StaticRingbuffer_t),
                                                          MALLOC_CAP_SPIRAM);
    uint8_t *cap_rb_storage = heap_caps_malloc(CAPTURE_RB_SIZE,
                                                MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    if (cap_rb_struct && cap_rb_storage) {
        capture_rb = xRingbufferCreateStatic(CAPTURE_RB_SIZE, RINGBUF_TYPE_BYTEBUF,
                                              cap_rb_storage, cap_rb_struct);
    }
    if (!capture_rb) {
        ESP_LOGE(TAG, "Failed to create capture ring buffer");
        return -1;
    }

    StaticRingbuffer_t *play_rb_struct = heap_caps_malloc(sizeof(StaticRingbuffer_t),
                                                           MALLOC_CAP_SPIRAM);
    uint8_t *play_rb_storage = heap_caps_malloc(PLAYBACK_RB_SIZE,
                                                 MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    if (play_rb_struct && play_rb_storage) {
        playback_rb = xRingbufferCreateStatic(PLAYBACK_RB_SIZE, RINGBUF_TYPE_BYTEBUF,
                                               play_rb_storage, play_rb_struct);
    }
    if (!playback_rb) {
        ESP_LOGE(TAG, "Failed to create playback ring buffer");
        return -1;
    }

    ESP_LOGI(TAG, "Audio HAL initialized successfully");
    return 0;
}

RingbufHandle_t audio_hal_get_capture_rb(void) {
    return capture_rb;
}

RingbufHandle_t audio_hal_get_playback_rb(void) {
    return playback_rb;
}

int audio_hal_read(int16_t *buf, size_t bytes, TickType_t timeout) {
    if (!rec_dev) return -1;
    esp_err_t err = esp_codec_dev_read(rec_dev, buf, (int)bytes);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "Codec read error: %s", esp_err_to_name(err));
        return -1;
    }
    return (int)bytes;
}

int audio_hal_write(const int16_t *buf, size_t bytes, TickType_t timeout) {
    if (!play_dev) return -1;
    esp_err_t err = esp_codec_dev_write(play_dev, (void *)buf, (int)bytes);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "Codec write error: %s", esp_err_to_name(err));
        return -1;
    }
    return (int)bytes;
}

void audio_hal_clear_playback(void) {
    if (!playback_rb) return;
    size_t item_size;
    void *item;
    while ((item = xRingbufferReceive(playback_rb, &item_size, 0)) != NULL) {
        vRingbufferReturnItem(playback_rb, item);
    }
    ESP_LOGI(TAG, "Playback buffer cleared");
}

void audio_hal_clear_capture(void) {
    if (!capture_rb) return;
    size_t item_size;
    void *item;
    while ((item = xRingbufferReceive(capture_rb, &item_size, 0)) != NULL) {
        vRingbufferReturnItem(capture_rb, item);
    }
    ESP_LOGI(TAG, "Capture buffer cleared");
}

void audio_hal_deinit(void) {
    if (play_dev) {
        esp_codec_dev_close(play_dev);
        play_dev = NULL;
    }
    if (rec_dev) {
        esp_codec_dev_close(rec_dev);
        rec_dev = NULL;
    }
    ESP_LOGI(TAG, "Audio HAL deinitialized");
}
