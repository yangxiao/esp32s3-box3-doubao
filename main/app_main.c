#include <string.h>
#include <stdlib.h>
#include <time.h>
#include <sys/time.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/event_groups.h"
#include "freertos/ringbuf.h"
#include "esp_system.h"
#include "esp_wifi.h"
#include "esp_event.h"
#include "esp_log.h"
#include "esp_netif.h"
#include "esp_sntp.h"
#include "nvs_flash.h"
#include "esp_heap_caps.h"
#include "driver/gpio.h"

#include "bsp/esp-bsp.h"

#include "doubao_protocol.h"
#include "doubao_ws_client.h"
#include "audio_hal.h"
#include "opus_processor.h"

static const char *TAG = "app_main";

/* ---- Application state machine ---- */

typedef enum {
    APP_STATE_IDLE = 0,
    APP_STATE_CONNECTING,       /* WiFi + WebSocket connecting */
    APP_STATE_HANDSHAKING,      /* StartConnection + StartSession + SayHello */
    APP_STATE_LISTENING,        /* Microphone active, streaming to server */
    APP_STATE_SPEAKING,         /* Playing TTS audio from server */
} app_state_t;

/* ---- Global shared state ---- */

static volatile app_state_t g_app_state = APP_STATE_IDLE;
static doubao_ws_client_t g_ws_client;
static opus_processor_t g_opus_proc;

static volatile bool g_running = true;
static volatile bool g_session_active = false;
static volatile bool g_say_hello_done = false;
static volatile bool g_user_querying = false;

/* Button GPIO (BOX-3 BOOT/CONFIG button is GPIO0, active low) */
#define BUTTON_GPIO  BSP_BUTTON_CONFIG_IO

static bool button_pressed(void) {
    return gpio_get_level(BUTTON_GPIO) == 0;
}

/* WiFi event group */
static EventGroupHandle_t s_wifi_event_group;
#define WIFI_CONNECTED_BIT  BIT0
#define WIFI_FAIL_BIT       BIT1
static int s_retry_num = 0;
#define WIFI_MAX_RETRY  5

/* ---- Time Sync ---- */

static void time_sync_notification_cb(struct timeval *tv)
{
    ESP_LOGI(TAG, "Time synchronized");
}

static int initialize_sntp(void)
{
    ESP_LOGI(TAG, "Initializing SNTP");
    esp_sntp_setoperatingmode(SNTP_OPMODE_POLL);
    esp_sntp_setservername(0, "pool.ntp.org");
    esp_sntp_setservername(1, "ntp.aliyun.com");
    esp_sntp_set_time_sync_notification_cb(time_sync_notification_cb);
    esp_sntp_init();
    return 0;
}

static int sync_system_time(void)
{
    initialize_sntp();

    /* Wait for time to be set */
    time_t now = 0;
    struct tm timeinfo = { 0 };
    int retry = 0;
    const int retry_count = 15;
    while (esp_sntp_get_sync_status() == SNTP_SYNC_STATUS_RESET && ++retry < retry_count) {
        ESP_LOGI(TAG, "Waiting for system time to be set... (%d/%d)", retry, retry_count);
        vTaskDelay(pdMS_TO_TICKS(2000));
    }

    time(&now);
    localtime_r(&now, &timeinfo);

    if (timeinfo.tm_year < (2016 - 1900)) {
        ESP_LOGE(TAG, "Time not synchronized yet");
        return -1;
    }

    /* Set timezone to Beijing (UTC+8) */
    setenv("TZ", "CST-8", 1);
    tzset();

    char strftime_buf[64];
    localtime_r(&now, &timeinfo);
    strftime(strftime_buf, sizeof(strftime_buf), "%c", &timeinfo);
    ESP_LOGI(TAG, "Current time in Beijing: %s", strftime_buf);

    return 0;
}

/* ---- WiFi ---- */

static void wifi_event_handler(void *arg, esp_event_base_t event_base,
                                int32_t event_id, void *event_data) {
    if (event_base == WIFI_EVENT && event_id == WIFI_EVENT_STA_START) {
        esp_wifi_connect();
    } else if (event_base == WIFI_EVENT && event_id == WIFI_EVENT_STA_DISCONNECTED) {
        if (s_retry_num < WIFI_MAX_RETRY) {
            esp_wifi_connect();
            s_retry_num++;
            ESP_LOGI(TAG, "Retrying WiFi connection (%d/%d)", s_retry_num, WIFI_MAX_RETRY);
        } else {
            xEventGroupSetBits(s_wifi_event_group, WIFI_FAIL_BIT);
        }
    } else if (event_base == IP_EVENT && event_id == IP_EVENT_STA_GOT_IP) {
        ip_event_got_ip_t *event = (ip_event_got_ip_t *)event_data;
        ESP_LOGI(TAG, "Got IP: " IPSTR, IP2STR(&event->ip_info.ip));
        s_retry_num = 0;
        xEventGroupSetBits(s_wifi_event_group, WIFI_CONNECTED_BIT);
    }
}

static int wifi_init_sta(void) {
    s_wifi_event_group = xEventGroupCreate();

    ESP_ERROR_CHECK(esp_netif_init());
    ESP_ERROR_CHECK(esp_event_loop_create_default());
    esp_netif_create_default_wifi_sta();

    wifi_init_config_t cfg = WIFI_INIT_CONFIG_DEFAULT();
    ESP_ERROR_CHECK(esp_wifi_init(&cfg));

    esp_event_handler_instance_t instance_any_id;
    esp_event_handler_instance_t instance_got_ip;
    ESP_ERROR_CHECK(esp_event_handler_instance_register(WIFI_EVENT,
                    ESP_EVENT_ANY_ID, &wifi_event_handler, NULL, &instance_any_id));
    ESP_ERROR_CHECK(esp_event_handler_instance_register(IP_EVENT,
                    IP_EVENT_STA_GOT_IP, &wifi_event_handler, NULL, &instance_got_ip));

    wifi_config_t wifi_config = {
        .sta = {
            .threshold.authmode = WIFI_AUTH_WPA2_PSK,
        },
    };
    strncpy((char *)wifi_config.sta.ssid, CONFIG_WIFI_SSID, sizeof(wifi_config.sta.ssid));
    strncpy((char *)wifi_config.sta.password, CONFIG_WIFI_PASSWORD, sizeof(wifi_config.sta.password));

    ESP_ERROR_CHECK(esp_wifi_set_mode(WIFI_MODE_STA));
    ESP_ERROR_CHECK(esp_wifi_set_config(WIFI_IF_STA, &wifi_config));
    ESP_ERROR_CHECK(esp_wifi_start());

    ESP_LOGI(TAG, "Connecting to WiFi SSID: %s", CONFIG_WIFI_SSID);

    EventBits_t bits = xEventGroupWaitBits(s_wifi_event_group,
                                            WIFI_CONNECTED_BIT | WIFI_FAIL_BIT,
                                            pdFALSE, pdFALSE,
                                            pdMS_TO_TICKS(30000));

    if (bits & WIFI_CONNECTED_BIT) {
        ESP_LOGI(TAG, "WiFi connected");
        return 0;
    } else {
        ESP_LOGE(TAG, "WiFi connection failed");
        return -1;
    }
}

/* ---- WebSocket receive callback ---- */

static void on_ws_receive(const parsed_response_t *resp, void *userdata) {
    if (resp->message_type == MSG_SERVER_ACK && resp->is_binary && resp->payload_data) {
        /* OGG/Opus audio data from TTS */
        opus_proc_decode_ogg(&g_opus_proc, resp->payload_data, resp->payload_data_len);

    } else if (resp->message_type == MSG_SERVER_FULL_RESPONSE) {
        if (resp->payload_data && !resp->is_binary) {
            ESP_LOGI(TAG, "Server: %.*s", (int)resp->payload_data_len, (char *)resp->payload_data);
        }

        if (resp->has_event) {
            switch (resp->event) {
            case EVENT_CLEAR_CACHE:
                ESP_LOGI(TAG, "Event 450: clear audio cache");
                audio_hal_clear_playback();
                opus_proc_reset_ogg(&g_opus_proc);
                g_user_querying = true;
                g_app_state = APP_STATE_LISTENING;
                break;

            case EVENT_USER_QUERY_END:
                ESP_LOGI(TAG, "Event 459: user query end");
                g_user_querying = false;
                g_app_state = APP_STATE_SPEAKING;
                break;

            case EVENT_TTS_ENDED:
                ESP_LOGI(TAG, "Event 359: TTS ended");
                if (!g_say_hello_done) {
                    g_say_hello_done = true;
                }
                g_app_state = APP_STATE_LISTENING;
                break;

            case EVENT_SESSION_FINISH_1:
            case EVENT_SESSION_FINISH_2:
                ESP_LOGI(TAG, "Event %d: session finished", resp->event);
                g_session_active = false;
                g_app_state = APP_STATE_IDLE;
                break;

            default:
                ESP_LOGD(TAG, "Event %d (unhandled)", resp->event);
                break;
            }
        }

    } else if (resp->message_type == MSG_SERVER_ERROR) {
        ESP_LOGE(TAG, "Server error (code=%u): %.*s",
                 (unsigned)resp->error_code,
                 (int)resp->payload_data_len,
                 resp->payload_data ? (char *)resp->payload_data : "");
        g_running = false;
    }
}

/* ---- Decoded PCM callback (from Opus decoder → playback ring buffer) ---- */

static void on_decoded_pcm(const int16_t *pcm, size_t samples, void *userdata) {
    RingbufHandle_t playback_rb = audio_hal_get_playback_rb();
    if (playback_rb) {
        size_t bytes = samples * sizeof(int16_t);
        /* Non-blocking write; drop if buffer full */
        if (xRingbufferSend(playback_rb, pcm, bytes, pdMS_TO_TICKS(50)) != pdTRUE) {
            ESP_LOGW(TAG, "Playback buffer full, dropping %d samples", (int)samples);
        }
    }
}

/* ---- FreeRTOS Tasks ---- */

/**
 * Audio Capture Task (Core 1, Priority 22)
 * Reads PCM from I2S mic → capture ring buffer
 */
static void audio_capture_task(void *pvParameters) {
    const size_t frame_bytes = OPUS_ENC_FRAME_SIZE * sizeof(int16_t);
    int16_t *pcm_buf = heap_caps_malloc(frame_bytes, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    if (!pcm_buf) {
        ESP_LOGE(TAG, "Failed to alloc capture PCM buffer");
        vTaskDelete(NULL);
        return;
    }

    RingbufHandle_t capture_rb = audio_hal_get_capture_rb();

    while (g_running) {
        if (g_app_state != APP_STATE_LISTENING && g_app_state != APP_STATE_SPEAKING) {
            vTaskDelay(pdMS_TO_TICKS(50));
            continue;
        }

        int read = audio_hal_read(pcm_buf, frame_bytes, pdMS_TO_TICKS(100));
        if (read > 0 && capture_rb) {
            if (xRingbufferSend(capture_rb, pcm_buf, read, pdMS_TO_TICKS(10)) != pdTRUE) {
                ESP_LOGW(TAG, "Capture buffer full");
            }
        }
    }

    free(pcm_buf);
    vTaskDelete(NULL);
}

/**
 * Audio Play Task (Core 1, Priority 21)
 * Reads PCM from playback ring buffer → I2S speaker
 */
static void audio_play_task(void *pvParameters) {
    RingbufHandle_t playback_rb = audio_hal_get_playback_rb();

    while (g_running) {
        if (!playback_rb) {
            vTaskDelay(pdMS_TO_TICKS(50));
            continue;
        }

        size_t item_size;
        void *item = xRingbufferReceive(playback_rb, &item_size, pdMS_TO_TICKS(100));
        if (item) {
            audio_hal_write((const int16_t *)item, item_size, pdMS_TO_TICKS(200));
            vRingbufferReturnItem(playback_rb, item);
        }
    }

    vTaskDelete(NULL);
}

/**
 * WebSocket TX Task (Core 0, Priority 15)
 * Reads PCM from capture ring buffer → Opus encode → send via WebSocket
 */
static void ws_tx_task(void *pvParameters) {
    const size_t frame_bytes = OPUS_ENC_FRAME_SIZE * sizeof(int16_t);
    uint8_t *opus_out = heap_caps_malloc(OPUS_ENC_MAX_OUT, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    int16_t *pcm_frame = heap_caps_malloc(frame_bytes, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    if (!pcm_frame || !opus_out) {
        ESP_LOGE(TAG, "Failed to alloc TX buffers");
        free(pcm_frame);
        free(opus_out);
        vTaskDelete(NULL);
        return;
    }

    RingbufHandle_t capture_rb = audio_hal_get_capture_rb();
    size_t accumulated = 0;

    while (g_running) {
        if (g_app_state != APP_STATE_LISTENING && g_app_state != APP_STATE_SPEAKING) {
            vTaskDelay(pdMS_TO_TICKS(50));
            accumulated = 0;
            continue;
        }

        if (!capture_rb || !doubao_ws_is_connected(&g_ws_client)) {
            vTaskDelay(pdMS_TO_TICKS(50));
            continue;
        }

        /* Accumulate exactly one Opus frame worth of PCM */
        size_t item_size;
        void *item = xRingbufferReceive(capture_rb, &item_size, pdMS_TO_TICKS(60));
        if (item) {
            size_t to_copy = item_size;
            if (accumulated + to_copy > frame_bytes) {
                to_copy = frame_bytes - accumulated;
            }
            memcpy((uint8_t *)pcm_frame + accumulated, item, to_copy);
            accumulated += to_copy;
            vRingbufferReturnItem(capture_rb, item);

            if (accumulated >= frame_bytes) {
                /* Encode and send */
                int encoded = opus_proc_encode(&g_opus_proc, pcm_frame,
                                                opus_out, OPUS_ENC_MAX_OUT);
                if (encoded > 0) {
                    doubao_ws_send_audio(&g_ws_client, opus_out, (size_t)encoded);
                }
                accumulated = 0;
            }
        }
    }

    free(pcm_frame);
    free(opus_out);
    vTaskDelete(NULL);
}

/**
 * Main FSM Task (Core 0, Priority 10)
 * Handles button press, session lifecycle
 */
static void main_fsm_task(void *pvParameters) {
    ESP_LOGI(TAG, "FSM task started, waiting for button press...");

    while (g_running) {
        switch (g_app_state) {
        case APP_STATE_IDLE:
            /* Phase 1: check button press to start session */
            /* BOX-3 has boot button on GPIO0 */
            if (button_pressed()) {
                ESP_LOGI(TAG, "Button pressed, starting session...");
                g_app_state = APP_STATE_CONNECTING;
                vTaskDelay(pdMS_TO_TICKS(300)); /* debounce */
            }
            vTaskDelay(pdMS_TO_TICKS(50));
            break;

        case APP_STATE_CONNECTING: {
            /* Connect WebSocket */
            doubao_ws_config_t ws_config = {
                .app_id = CONFIG_DOUBAO_APP_ID,
                .access_key = CONFIG_DOUBAO_ACCESS_KEY,
                .app_key = CONFIG_DOUBAO_APP_KEY,
                .resource_id = "volc.speech.dialog",
                .tts_speaker = CONFIG_DOUBAO_TTS_SPEAKER,
                .input_mod = "audio",
                .asr_audio_format = "speech_opus",
                .recv_timeout = 10,
            };

            if (doubao_ws_init(&g_ws_client, &ws_config) != 0) {
                ESP_LOGE(TAG, "WS init failed");
                g_app_state = APP_STATE_IDLE;
                break;
            }

            doubao_ws_set_recv_callback(&g_ws_client, on_ws_receive, NULL);

            if (doubao_ws_connect(&g_ws_client) != 0) {
                ESP_LOGE(TAG, "WS connect failed");
                doubao_ws_destroy(&g_ws_client);
                g_app_state = APP_STATE_IDLE;
                break;
            }

            g_app_state = APP_STATE_HANDSHAKING;
            break;
        }

        case APP_STATE_HANDSHAKING: {
            /* Protocol handshake sequence — wait for each command to complete
             * before sending the next, to avoid WS mutex contention */
            ESP_LOGI(TAG, "Starting handshake...");

            if (doubao_ws_start_connection(&g_ws_client) != 0) {
                ESP_LOGE(TAG, "StartConnection failed");
                doubao_ws_destroy(&g_ws_client);
                g_app_state = APP_STATE_IDLE;
                break;
            }
            vTaskDelay(pdMS_TO_TICKS(1000));

            if (doubao_ws_start_session(&g_ws_client) != 0) {
                ESP_LOGE(TAG, "StartSession failed");
                doubao_ws_destroy(&g_ws_client);
                g_app_state = APP_STATE_IDLE;
                break;
            }
            vTaskDelay(pdMS_TO_TICKS(1000));

            g_say_hello_done = false;
            g_session_active = true;
            if (doubao_ws_say_hello(&g_ws_client) != 0) {
                ESP_LOGE(TAG, "SayHello failed");
            }

            /* Wait for SayHello TTS to complete */
            int wait_count = 0;
            while (!g_say_hello_done && wait_count < 100 && g_running) {
                vTaskDelay(pdMS_TO_TICKS(100));
                wait_count++;
            }

            if (g_say_hello_done) {
                ESP_LOGI(TAG, "Handshake complete, listening...");
                g_app_state = APP_STATE_LISTENING;
            } else {
                ESP_LOGW(TAG, "SayHello timeout, starting listening anyway");
                g_app_state = APP_STATE_LISTENING;
            }
            break;
        }

        case APP_STATE_LISTENING:
        case APP_STATE_SPEAKING:
            /* Check button for manual session end */
            if (button_pressed()) {
                ESP_LOGI(TAG, "Button pressed, ending session...");
                doubao_ws_finish_session(&g_ws_client);
                vTaskDelay(pdMS_TO_TICKS(1000));
                doubao_ws_finish_connection(&g_ws_client);
                vTaskDelay(pdMS_TO_TICKS(500));

                doubao_ws_destroy(&g_ws_client);
                g_session_active = false;
                g_app_state = APP_STATE_IDLE;

                /* Debounce */
                vTaskDelay(pdMS_TO_TICKS(500));
            }

            /* Check if session ended by server */
            if (!g_session_active && g_app_state != APP_STATE_IDLE) {
                doubao_ws_destroy(&g_ws_client);
                g_app_state = APP_STATE_IDLE;
            }

            vTaskDelay(pdMS_TO_TICKS(50));
            break;
        }
    }

    vTaskDelete(NULL);
}

/* ---- Entry Point ---- */

void app_main(void) {
    ESP_LOGI(TAG, "=== ESP32-S3-BOX-3 Doubao AI Speaker ===");
    ESP_LOGI(TAG, "Free heap: %lu, PSRAM: %lu",
             (unsigned long)esp_get_free_heap_size(),
             (unsigned long)heap_caps_get_free_size(MALLOC_CAP_SPIRAM));

    /* Initialize NVS */
    esp_err_t ret = nvs_flash_init();
    if (ret == ESP_ERR_NVS_NO_FREE_PAGES || ret == ESP_ERR_NVS_NEW_VERSION_FOUND) {
        ESP_ERROR_CHECK(nvs_flash_erase());
        ret = nvs_flash_init();
    }
    ESP_ERROR_CHECK(ret);

    /* Initialize WiFi */
    if (wifi_init_sta() != 0) {
        ESP_LOGE(TAG, "WiFi init failed, restarting...");
        vTaskDelay(pdMS_TO_TICKS(3000));
        esp_restart();
    }

    /* Sync system time to Beijing time */
    ESP_LOGI(TAG, "Syncing system time to Beijing time...");
    if (sync_system_time() != 0) {
        ESP_LOGW(TAG, "Time sync failed, continuing anyway...");
    }

    /* Initialize button GPIO (active low with internal pull-up) */
    gpio_config_t btn_cfg = {
        .pin_bit_mask = (1ULL << BUTTON_GPIO),
        .mode = GPIO_MODE_INPUT,
        .pull_up_en = GPIO_PULLUP_ENABLE,
        .pull_down_en = GPIO_PULLDOWN_DISABLE,
        .intr_type = GPIO_INTR_DISABLE,
    };
    gpio_config(&btn_cfg);

    /* Initialize audio hardware */
    if (audio_hal_init() != 0) {
        ESP_LOGE(TAG, "Audio HAL init failed");
        return;
    }

    /* Initialize Opus processor */
    if (opus_proc_init(&g_opus_proc) != 0) {
        ESP_LOGE(TAG, "Opus processor init failed");
        return;
    }
    opus_proc_set_output_cb(&g_opus_proc, on_decoded_pcm, NULL);

    ESP_LOGI(TAG, "All subsystems initialized. Press button to start conversation.");
    ESP_LOGI(TAG, "Free heap: %lu, PSRAM: %lu",
             (unsigned long)esp_get_free_heap_size(),
             (unsigned long)heap_caps_get_free_size(MALLOC_CAP_SPIRAM));

    /* Create FreeRTOS tasks */

    /* Audio tasks on Core 1 */
    xTaskCreatePinnedToCore(audio_capture_task, "audio_cap",
                            8192, NULL, 22, NULL, 1);
    xTaskCreatePinnedToCore(audio_play_task, "audio_play",
                            8192, NULL, 21, NULL, 1);

    /* WebSocket TX task on Core 0 */
    xTaskCreatePinnedToCore(ws_tx_task, "ws_tx",
                            8192, NULL, 15, NULL, 0);

    /* Main FSM task on Core 0 (ws_rx is handled by esp_websocket_client internally) */
    xTaskCreatePinnedToCore(main_fsm_task, "main_fsm",
                            6144, NULL, 10, NULL, 0);
}
