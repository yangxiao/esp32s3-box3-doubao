#include "audio_hal.h"
#include "esp_log.h"
#include "esp_heap_caps.h"
#include "bsp/esp-bsp.h"
#include "esp_codec_dev.h"
#include "driver/i2s_std.h"

static const char *TAG = "audio_hal";

static RingbufHandle_t capture_rb = NULL;
static RingbufHandle_t playback_rb = NULL;
static RingbufHandle_t ref_rb = NULL;
static esp_codec_dev_handle_t play_dev = NULL;
static esp_codec_dev_handle_t rec_dev = NULL;

static RingbufHandle_t create_psram_ringbuf(size_t size, const char *name) {
    StaticRingbuffer_t *rb_struct = heap_caps_malloc(sizeof(StaticRingbuffer_t), MALLOC_CAP_SPIRAM);
    uint8_t *rb_storage = heap_caps_malloc(size, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    if (rb_struct && rb_storage) {
        RingbufHandle_t rb = xRingbufferCreateStatic(size, RINGBUF_TYPE_BYTEBUF, rb_storage, rb_struct);
        if (rb) return rb;
    }
    ESP_LOGE(TAG, "Failed to create %s ring buffer", name);
    free(rb_struct);
    free(rb_storage);
    return NULL;
}

static void drain_ringbuf(RingbufHandle_t rb) {
    if (!rb) return;
    size_t item_size;
    void *item;
    while ((item = xRingbufferReceive(rb, &item_size, 0)) != NULL) {
        vRingbufferReturnItem(rb, item);
    }
}

int audio_hal_init(void) {
    ESP_LOGI(TAG, "Initializing audio HAL (stereo mic for AFE)...");
    esp_err_t err;

    /* Step 1: Initialize I2C */
    err = bsp_i2c_init();
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "BSP I2C init failed: %s", esp_err_to_name(err));
        return -1;
    }

    /* Step 2: Initialize I2S with stereo config for dual-mic ES7210.
     * The TX side (ES8311 speaker) only uses slot 0 within the stereo frame. */
    i2s_std_config_t stereo_cfg = {
        .clk_cfg = I2S_STD_CLK_DEFAULT_CONFIG(AUDIO_INPUT_SAMPLE_RATE),
        .slot_cfg = I2S_STD_PHILIP_SLOT_DEFAULT_CONFIG(I2S_DATA_BIT_WIDTH_16BIT, I2S_SLOT_MODE_STEREO),
        .gpio_cfg = {
            .mclk = BSP_I2S_MCLK,
            .bclk = BSP_I2S_SCLK,
            .ws   = BSP_I2S_LCLK,
            .dout = BSP_I2S_DOUT,
            .din  = BSP_I2S_DSIN,
            .invert_flags = {
                .mclk_inv = false,
                .bclk_inv = false,
                .ws_inv = false,
            },
        },
    };
    err = bsp_audio_init(&stereo_cfg);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "BSP audio init failed: %s", esp_err_to_name(err));
        return -1;
    }

    /* Step 3: Initialize speaker codec (ES8311) — mono output */
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

    /* Step 4: Initialize microphone codec (ES7210) — stereo (2 mics) */
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
    capture_rb = create_psram_ringbuf(CAPTURE_RB_SIZE, "capture");
    if (!capture_rb) return -1;

    playback_rb = create_psram_ringbuf(PLAYBACK_RB_SIZE, "playback");
    if (!playback_rb) return -1;

    ref_rb = create_psram_ringbuf(REF_RB_SIZE, "ref");
    if (!ref_rb) return -1;

    ESP_LOGI(TAG, "Audio HAL initialized successfully (stereo mic + ref_rb)");
    return 0;
}

RingbufHandle_t audio_hal_get_capture_rb(void) {
    return capture_rb;
}

RingbufHandle_t audio_hal_get_playback_rb(void) {
    return playback_rb;
}

RingbufHandle_t audio_hal_get_ref_rb(void) {
    return ref_rb;
}

int audio_hal_read_stereo(int16_t *buf, size_t bytes, TickType_t timeout) {
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
    drain_ringbuf(playback_rb);
    ESP_LOGI(TAG, "Playback buffer cleared");
}

void audio_hal_clear_capture(void) {
    drain_ringbuf(capture_rb);
    ESP_LOGI(TAG, "Capture buffer cleared");
}

void audio_hal_clear_ref(void) {
    drain_ringbuf(ref_rb);
    ESP_LOGI(TAG, "Ref buffer cleared");
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
