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
#include "cJSON.h"
#include "esp_task_wdt.h"

#include "bsp/esp-bsp.h"

#include "doubao_protocol.h"
#include "doubao_ws_client.h"
#include "audio_hal.h"
#include "opus_processor.h"
#include "afe_handler.h"
#include "ui_lcd.h"

static const char *TAG = "app_main";

/* ---- Application state machine ---- */

typedef enum {
    APP_STATE_IDLE = 0,
    APP_STATE_CONNECTING,       /* WiFi + WebSocket connecting */
    APP_STATE_CONNECTED_IDLE,   /* WebSocket connected, waiting for wake word/button */
    APP_STATE_HANDSHAKING,      /* StartSession + SayHello (StartConnection already done) */
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
static volatile bool g_bargein_requested = false;  /* Phase 2: barge-in flag */
static char g_tts_text[512] = {0};  /* Accumulate TTS text for display */
static char g_last_reply_id[128] = {0};  /* Last reply_id to detect new replies */

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

/* ---- Helper: set state with UI update ---- */

static void set_app_state(app_state_t new_state) {
    g_app_state = new_state;
    const char *label;
    switch (new_state) {
    case APP_STATE_IDLE:        label = "Idle";        break;
    case APP_STATE_CONNECTING:  label = "Connecting";  break;
    case APP_STATE_CONNECTED_IDLE: label = "Ready";    break;
    case APP_STATE_HANDSHAKING: label = "Handshaking"; break;
    case APP_STATE_LISTENING:   label = "Listening";   break;
    case APP_STATE_SPEAKING:    label = "Speaking";    break;
    default:                    label = "Unknown";     break;
    }
    ui_lcd_set_state(label);
    ESP_LOGI(TAG, "State -> %s", label);
}

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

/* ---- Wake word callback (called from afe_fetch_task context) ---- */

static void on_wake_word(int wake_word_index, void *userdata) {
    ESP_LOGI(TAG, "Wake word detected! index=%d, state=%d", wake_word_index, g_app_state);

    if (g_app_state == APP_STATE_CONNECTED_IDLE) {
        /* Start a new session */
        afe_handler_disable_wakenet();
        set_app_state(APP_STATE_HANDSHAKING);
        ui_lcd_set_hint("Wake word detected!");
    } else if (g_app_state == APP_STATE_SPEAKING) {
        /* Path A: local barge-in — interrupt TTS playback */
        ESP_LOGI(TAG, "Barge-in: wake word during TTS playback");
        audio_hal_clear_playback();
        audio_hal_clear_ref();
        opus_proc_reset_ogg(&g_opus_proc);
        g_bargein_requested = true;
        ui_lcd_set_hint("Barge-in detected!");
    }
}

/* ---- WebSocket receive callback ---- */

static void on_ws_receive(const parsed_response_t *resp, void *userdata) {
    ESP_LOGI(TAG, "WS recv: msg_type=%d, has_event=%d, event=%d, is_binary=%d, payload_len=%d",
             resp->message_type, resp->has_event, resp->event, resp->is_binary, (int)resp->payload_data_len);

    if (resp->message_type == MSG_SERVER_ACK && resp->is_binary && resp->payload_data) {
        /* OGG/Opus audio data from TTS */
        ESP_LOGD(TAG, "Received TTS audio: %d bytes", (int)resp->payload_data_len);
        opus_proc_decode_ogg(&g_opus_proc, resp->payload_data, resp->payload_data_len);

    } else if (resp->message_type == MSG_SERVER_FULL_RESPONSE) {
        if (resp->payload_data && !resp->is_binary) {
            //ESP_LOGI(TAG, "Server: %.*s", (int)resp->payload_data_len, (char *)resp->payload_data);
            ESP_LOGI(TAG, "Server: payload_len=%d", (int)resp->payload_data_len);
        }

        if (resp->has_event) {
            ESP_LOGI(TAG, "Received event: %d", resp->event);
            switch (resp->event) {
            case 50:
                ESP_LOGI(TAG, "Event 50: connection started");
                break;

            case 150:
                ESP_LOGI(TAG, "Event 150: session started");
                if (resp->payload_data && !resp->is_binary) {
                    ESP_LOGI(TAG, "Session info: %.*s", (int)resp->payload_data_len, (char *)resp->payload_data);
                }
                break;

            case 154:
                ESP_LOGI(TAG, "Event 154: usage stats");
                if (resp->payload_data && !resp->is_binary) {
                    ESP_LOGI(TAG, "Usage: %.*s", (int)resp->payload_data_len, (char *)resp->payload_data);
                }
                break;

            case 350:
                ESP_LOGI(TAG, "Event 350: TTS text available");
                if (resp->payload_data && !resp->is_binary) {
                    ESP_LOGI(TAG, "TTS text: %.*s", (int)resp->payload_data_len, (char *)resp->payload_data);

                    /* Parse JSON to get text field and append */
                    cJSON *root = cJSON_ParseWithLength((const char *)resp->payload_data, resp->payload_data_len);
                    if (root) {
                        cJSON *text = cJSON_GetObjectItem(root, "text");
                        if (text && cJSON_IsString(text)) {
                            /* Append to accumulated text */
                            size_t current_len = strlen(g_tts_text);
                            size_t text_len = strlen(text->valuestring);
                            if (current_len + text_len < sizeof(g_tts_text) - 1) {
                                strcat(g_tts_text, text->valuestring);
                                ui_lcd_set_tts_text(g_tts_text);
                            }
                        }
                        cJSON_Delete(root);
                    }
                }
                break;

            case 351:
                ESP_LOGI(TAG, "Event 351: TTS text complete");
                break;

            case 352:
                /* TTS audio data - handled by msg_type=11 check above */
                break;

            case 451:
                /* ASR interim result */
                ESP_LOGI(TAG, "Event 451: ASR interim result");
                if (resp->payload_data && !resp->is_binary) {
                    ESP_LOGI(TAG, "ASR: %.*s", (int)resp->payload_data_len, (char *)resp->payload_data);

                    /* Parse JSON to check is_interim from results[0] */
                    cJSON *root = cJSON_ParseWithLength((const char *)resp->payload_data, resp->payload_data_len);
                    if (root) {
                        cJSON *results = cJSON_GetObjectItem(root, "results");
                        if (results && cJSON_IsArray(results) && cJSON_GetArraySize(results) > 0) {
                            cJSON *result0 = cJSON_GetArrayItem(results, 0);
                            if (result0) {
                                cJSON *is_interim = cJSON_GetObjectItem(result0, "is_interim");
                                bool is_final = true;
                                if (is_interim && cJSON_IsBool(is_interim)) {
                                    is_final = !cJSON_IsTrue(is_interim);
                                    ESP_LOGI(TAG, "is_interim=%d, is_final=%d", cJSON_IsTrue(is_interim), is_final);
                                }

                                if (is_final) {
                                    cJSON *text = cJSON_GetObjectItem(result0, "text");
                                    if (text && cJSON_IsString(text)) {
                                        ui_lcd_set_asr_text(text->valuestring);
                                    }
                                }
                            }
                        }
                        cJSON_Delete(root);
                    }
                }
                break;

            case EVENT_CLEAR_CACHE:
                ESP_LOGI(TAG, "Event 450: clear audio cache (cloud barge-in)");
                audio_hal_clear_playback();
                audio_hal_clear_capture();
                audio_hal_clear_ref();
                opus_proc_reset_ogg(&g_opus_proc);
                g_user_querying = true;
                afe_handler_disable_wakenet();
                afe_handler_set_streaming(true);
                set_app_state(APP_STATE_LISTENING);
                ui_lcd_set_hint("Listening...");
                break;

            case EVENT_USER_QUERY_END:
                ESP_LOGI(TAG, "Event 459: user query end");
                g_user_querying = false;
                afe_handler_set_streaming(false);
                afe_handler_enable_wakenet();
                audio_hal_clear_capture();
                set_app_state(APP_STATE_SPEAKING);
                ui_lcd_set_hint("AI is responding...");
                break;

            case 550:
                ESP_LOGI(TAG, "Event 550: dialogue text update");
                if (resp->payload_data && !resp->is_binary) {
                    ESP_LOGI(TAG, "Reply: %.*s", (int)resp->payload_data_len, (char *)resp->payload_data);

                    /* Parse JSON to get reply_id and content */
                    cJSON *root = cJSON_ParseWithLength((const char *)resp->payload_data, resp->payload_data_len);
                    if (root) {
                        /* Check if reply_id changed - if yes, clear accumulated text */
                        cJSON *reply_id = cJSON_GetObjectItem(root, "reply_id");
                        if (reply_id && cJSON_IsString(reply_id)) {
                            /* First check if there was a previous reply_id */
                            if (g_last_reply_id[0] != '\0') {
                                if (strcmp(g_last_reply_id, reply_id->valuestring) != 0) {
                                    /* New reply detected, clear accumulated text */
                                    ESP_LOGI(TAG, "New reply_id detected, clearing TTS text");
                                    g_tts_text[0] = '\0';
                                }
                            }
                            /* Save current reply_id */
                            strncpy(g_last_reply_id, reply_id->valuestring, sizeof(g_last_reply_id) - 1);
                            g_last_reply_id[sizeof(g_last_reply_id) - 1] = '\0';
                        }

                        /* Get and append content */
                        cJSON *content = cJSON_GetObjectItem(root, "content");
                        if (content && cJSON_IsString(content)) {
                            /* Append to accumulated text */
                            size_t current_len = strlen(g_tts_text);
                            size_t content_len = strlen(content->valuestring);
                            if (current_len + content_len < sizeof(g_tts_text) - 1) {
                                strcat(g_tts_text, content->valuestring);
                                ui_lcd_set_tts_text(g_tts_text);
                            }
                        }
                        cJSON_Delete(root);
                    }
                }
                break;

            case 559:
                ESP_LOGI(TAG, "Event 559: dialogue turn complete");
                break;

            case EVENT_TTS_ENDED: {
                ESP_LOGI(TAG, "Event 359: TTS ended");
                if (!g_say_hello_done) {
                    g_say_hello_done = true;
                }

                /* Check for exit intent: status_code "20000002" */
                bool exit_intent = false;
                if (resp->payload_data && !resp->is_binary && resp->payload_data_len > 0) {
                    cJSON *root = cJSON_ParseWithLength((const char *)resp->payload_data, resp->payload_data_len);
                    if (root) {
                        cJSON *sc = cJSON_GetObjectItem(root, "status_code");
                        if (sc && cJSON_IsString(sc) && strcmp(sc->valuestring, "20000002") == 0) {
                            exit_intent = true;
                            ESP_LOGI(TAG, "Exit intent detected (status_code=20000002)");
                        }
                        cJSON_Delete(root);
                    }
                }

                audio_hal_clear_playback();
                audio_hal_clear_capture();
                audio_hal_clear_ref();
                opus_proc_reset_ogg(&g_opus_proc);

                if (exit_intent) {
                    /* Gracefully end session, keep connection alive */
                    ESP_LOGI(TAG, "Exit intent: finishing session, keeping connection alive");
                    afe_handler_set_streaming(false);
                    afe_handler_enable_wakenet();
                    doubao_ws_finish_session(&g_ws_client);
                    vTaskDelay(pdMS_TO_TICKS(500));
                    g_session_active = false;
                    set_app_state(APP_STATE_CONNECTED_IDLE);
                    ui_lcd_set_asr_text("");
                    ui_lcd_set_tts_text("");
                    ui_lcd_set_hint("Say \"hi, jason\" or press button");
                } else {
                    /* Continue conversation: go back to LISTENING */
                    g_user_querying = true;
                    afe_handler_disable_wakenet();
                    afe_handler_set_streaming(true);
                    set_app_state(APP_STATE_LISTENING);
                    ui_lcd_set_hint("Listening...");
                }
                break;
            }

            case EVENT_SESSION_FINISH_1:
            case EVENT_SESSION_FINISH_2:
                ESP_LOGI(TAG, "Event %d: session finished", resp->event);
                g_session_active = false;
                afe_handler_set_streaming(false);
                afe_handler_enable_wakenet();
                set_app_state(APP_STATE_IDLE);
                ui_lcd_set_asr_text("");
                ui_lcd_set_tts_text("");
                ui_lcd_set_hint("Say \"hi, jason\" or press button");
                break;

            default:
                ESP_LOGI(TAG, "Event %d (unhandled)", resp->event);
                if (resp->payload_data && !resp->is_binary) {
                    ESP_LOGI(TAG, "Payload: %.*s", (int)resp->payload_data_len, (char *)resp->payload_data);
                }
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

/* ---- Decoded PCM callback (from Opus decoder -> playback ring buffer) ---- */

static void on_decoded_pcm(const int16_t *pcm, size_t samples, void *userdata) {
    RingbufHandle_t playback_rb = audio_hal_get_playback_rb();
    if (playback_rb) {
        size_t bytes = samples * sizeof(int16_t);
        if (xRingbufferSend(playback_rb, pcm, bytes, pdMS_TO_TICKS(500)) != pdTRUE) {
            ESP_LOGW(TAG, "Playback buffer full, dropping %d samples", (int)samples);
        }
    }
}

/* ---- FreeRTOS Tasks ---- */

/**
 * Audio Play Task (Core 1, Priority 21)
 * Reads PCM from playback ring buffer -> I2S speaker + ref_rb for AEC
 */
static void audio_play_task(void *pvParameters) {
    RingbufHandle_t playback_rb = audio_hal_get_playback_rb();
    RingbufHandle_t ref_rb = audio_hal_get_ref_rb();
    uint32_t play_count = 0;
    ESP_LOGI(TAG, "Audio play task started.");
    while (g_running) {
        if (play_count++ % 1000 == 0) {
            ESP_LOGI(TAG, "Play loop alive, count=%lu", (unsigned long)play_count);
        }
        if (!playback_rb) {
            vTaskDelay(pdMS_TO_TICKS(50));
            continue;
        }

        size_t item_size;
        void *item = xRingbufferReceive(playback_rb, &item_size, pdMS_TO_TICKS(100));
        if (item) {
            /* Copy to ref_rb for AEC before writing to speaker (best-effort, non-blocking) */
            if (ref_rb) {
                xRingbufferSend(ref_rb, item, item_size, 0);
            }
            audio_hal_write((const int16_t *)item, item_size, pdMS_TO_TICKS(200));
            vRingbufferReturnItem(playback_rb, item);
        }
    }

    vTaskDelete(NULL);
}

/**
 * WebSocket TX Task (Core 0, Priority 15)
 * Reads PCM from capture ring buffer -> send raw PCM via WebSocket
 */
static void ws_tx_task(void *pvParameters) {
    ESP_LOGI(TAG, "WS TX task started.");
    /* Send in 320-sample chunks (20ms @ 16kHz) */
    const size_t chunk_bytes = 320 * sizeof(int16_t);
    int16_t *pcm_chunk = heap_caps_malloc(chunk_bytes, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    if (!pcm_chunk) {
        ESP_LOGE(TAG, "Failed to alloc TX buffer");
        vTaskDelete(NULL);
        return;
    }

    RingbufHandle_t capture_rb = audio_hal_get_capture_rb();
    size_t accumulated = 0;
    static uint32_t tx_log_counter = 0;
    static uint32_t state_log_counter = 0;

    while (g_running) {
        if (g_app_state != APP_STATE_LISTENING) {
            vTaskDelay(pdMS_TO_TICKS(50));
            accumulated = 0;
            continue;
        }

        if (state_log_counter++ % 200 == 0) {
            ESP_LOGI(TAG, "ws_tx_task: LISTENING state, connected=%d", doubao_ws_is_connected(&g_ws_client));
        }

        if (!capture_rb || !doubao_ws_is_connected(&g_ws_client)) {
            vTaskDelay(pdMS_TO_TICKS(50));
            continue;
        }

        /* Accumulate a chunk of PCM */
        size_t item_size;
        void *item = xRingbufferReceive(capture_rb, &item_size, pdMS_TO_TICKS(60));
        if (item) {
            size_t to_copy = item_size;
            if (accumulated + to_copy > chunk_bytes) {
                to_copy = chunk_bytes - accumulated;
            }
            memcpy((uint8_t *)pcm_chunk + accumulated, item, to_copy);
            accumulated += to_copy;
            vRingbufferReturnItem(capture_rb, item);

            if (accumulated >= chunk_bytes) {
                if (g_app_state == APP_STATE_LISTENING) {
                    doubao_ws_send_audio(&g_ws_client, (const uint8_t *)pcm_chunk, accumulated);
                    if (tx_log_counter++ % 50 == 0) {
                        ESP_LOGI(TAG, "Sent audio chunk: %d bytes", (int)accumulated);
                    }
                }
                accumulated = 0;
            }
        }
    }

    free(pcm_chunk);
    vTaskDelete(NULL);
}

/**
 * Main FSM Task (Core 0, Priority 10)
 * Handles button press, wake word trigger, barge-in, session lifecycle
 */
static void main_fsm_task(void *pvParameters) {
    ESP_LOGI(TAG, "FSM task started, say \"hi, jason\" or press button...");

    static uint32_t state_log_counter = 0;
    while (g_running) {
        if (state_log_counter++ % 200 == 0) {
            ESP_LOGI(TAG, "FSM state: %d, session_active=%d", g_app_state, g_session_active);
        }

        /* Handle barge-in request from wake word callback (Path A) */
        if (g_bargein_requested) {
            g_bargein_requested = false;
            ESP_LOGI(TAG, "Processing barge-in: finishing current session, restarting");

            /* Clear TTS text display for new session */
            g_tts_text[0] = '\0';
            g_last_reply_id[0] = '\0';

            /* Finish the current session and start a new one - keep connection alive */
            doubao_ws_finish_session(&g_ws_client);
            vTaskDelay(pdMS_TO_TICKS(300));
            g_session_active = false;

            /* Immediately start new session */
            set_app_state(APP_STATE_HANDSHAKING);
            ui_lcd_set_asr_text("");
            ui_lcd_set_tts_text("");
        }

        switch (g_app_state) {
        case APP_STATE_IDLE:
            /* Should not be in IDLE, we keep connection always connected */
            ESP_LOGW(TAG, "Unexpected IDLE state, reconnecting...");
            set_app_state(APP_STATE_CONNECTING);
            vTaskDelay(pdMS_TO_TICKS(50));
            break;

        case APP_STATE_CONNECTING: {
            /* Disable wake word during connection setup */
            afe_handler_disable_wakenet();

            /* Connect WebSocket */
            doubao_ws_config_t ws_config = {
                .app_id = CONFIG_DOUBAO_APP_ID,
                .access_key = CONFIG_DOUBAO_ACCESS_KEY,
                .app_key = CONFIG_DOUBAO_APP_KEY,
                .resource_id = "volc.speech.dialog",
                .tts_speaker = CONFIG_DOUBAO_TTS_SPEAKER,
                .input_mod = "audio",
                .asr_audio_format = NULL, /* Raw PCM */
                .recv_timeout = 10,
            };
            ESP_LOGI(TAG, "B4 WS connect, Free heap: %lu, PSRAM: %lu, IRAM: %lu",
             (unsigned long)esp_get_free_heap_size(),
             (unsigned long)heap_caps_get_free_size(MALLOC_CAP_SPIRAM),
             (unsigned long)heap_caps_get_free_size(MALLOC_CAP_INTERNAL));
            if (doubao_ws_init(&g_ws_client, &ws_config) != 0) {
                ESP_LOGE(TAG, "WS init failed");
                afe_handler_enable_wakenet();
                set_app_state(APP_STATE_CONNECTED_IDLE);
                ui_lcd_set_hint("Connection failed, retrying...");
                vTaskDelay(pdMS_TO_TICKS(3000));
                set_app_state(APP_STATE_CONNECTING);
                break;
            }

            doubao_ws_set_recv_callback(&g_ws_client, on_ws_receive, NULL);

            if (doubao_ws_connect(&g_ws_client) != 0) {
                ESP_LOGE(TAG, "WS connect failed");
                doubao_ws_destroy(&g_ws_client);
                afe_handler_enable_wakenet();
                set_app_state(APP_STATE_CONNECTED_IDLE);
                ui_lcd_set_hint("Connection failed, retrying...");
                vTaskDelay(pdMS_TO_TICKS(3000));
                set_app_state(APP_STATE_CONNECTING);
                break;
            }

            /* Start connection level */
            if (doubao_ws_start_connection(&g_ws_client) != 0) {
                ESP_LOGE(TAG, "StartConnection failed");
                doubao_ws_destroy(&g_ws_client);
                afe_handler_enable_wakenet();
                set_app_state(APP_STATE_CONNECTED_IDLE);
                ui_lcd_set_hint("Connection failed, retrying...");
                vTaskDelay(pdMS_TO_TICKS(3000));
                set_app_state(APP_STATE_CONNECTING);
                break;
            }

            /* Connection established, go to idle waiting for wake word/button */
            afe_handler_enable_wakenet();
            set_app_state(APP_STATE_CONNECTED_IDLE);
            ui_lcd_set_hint("Say \"hi, jason\" or press button");
            break;
        }

        case APP_STATE_CONNECTED_IDLE:
            /* Check button press or wake word (wake word sets state via callback) */
            if (button_pressed()) {
                ESP_LOGI(TAG, "Button pressed, starting session...");
                afe_handler_disable_wakenet();
                set_app_state(APP_STATE_HANDSHAKING);
                ui_lcd_set_hint("Button pressed!");
                vTaskDelay(pdMS_TO_TICKS(300)); /* debounce */
            }
            vTaskDelay(pdMS_TO_TICKS(50));
            break;

        case APP_STATE_HANDSHAKING: {
            ESP_LOGI(TAG, "Starting new session...");

            /* Clear TTS text display for new session */
            g_tts_text[0] = '\0';
            g_last_reply_id[0] = '\0';
            ui_lcd_set_tts_text("");

            /* Start session and say hello - connection already established */
            if (doubao_ws_start_session(&g_ws_client) != 0) {
                ESP_LOGE(TAG, "StartSession failed");
                /* Don't destroy connection, just go back to idle */
                afe_handler_enable_wakenet();
                set_app_state(APP_STATE_CONNECTED_IDLE);
                ui_lcd_set_hint("Session failed, try again");
                break;
            }
            vTaskDelay(pdMS_TO_TICKS(500));

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
            } else {
                ESP_LOGW(TAG, "SayHello timeout, starting listening anyway");
            }

            /* Enable AFE streaming for listening */
            audio_hal_clear_capture();
            afe_handler_disable_wakenet();
            afe_handler_set_streaming(true);
            g_user_querying = true;
            set_app_state(APP_STATE_LISTENING);
            ui_lcd_set_hint("Listening...");
            break;
        }

        case APP_STATE_LISTENING:
        case APP_STATE_SPEAKING:
            /* Check button for manual session end */
            if (button_pressed()) {
                ESP_LOGI(TAG, "Button pressed, ending session...");
                afe_handler_set_streaming(false);

                /* Only finish session, keep connection alive */
                doubao_ws_finish_session(&g_ws_client);
                vTaskDelay(pdMS_TO_TICKS(500));

                g_session_active = false;
                afe_handler_enable_wakenet();
                set_app_state(APP_STATE_CONNECTED_IDLE);
                ui_lcd_set_asr_text("");
                ui_lcd_set_tts_text("");
                ui_lcd_set_hint("Say \"hi, jason\" or press button");

                vTaskDelay(pdMS_TO_TICKS(500)); /* debounce */
            }

            /* Check if session ended by server */
            if (!g_session_active && g_app_state != APP_STATE_CONNECTED_IDLE) {
                /* Don't destroy connection, just go back to idle */
                afe_handler_set_streaming(false);
                afe_handler_enable_wakenet();
                set_app_state(APP_STATE_CONNECTED_IDLE);
                ui_lcd_set_asr_text("");
                ui_lcd_set_tts_text("");
                ui_lcd_set_hint("Say \"hi, jason\" or press button");
            }

            vTaskDelay(pdMS_TO_TICKS(50));
            break;
        }
    }

    vTaskDelete(NULL);
}

/* ---- Entry Point ---- */

void app_main(void) {
    ESP_LOGI(TAG, "=== ESP32-S3-BOX-3 Doubao AI Speaker (Phase 2) ===");
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

    /* Initialize LCD UI */
    if (ui_lcd_init() != 0) {
        ESP_LOGW(TAG, "LCD UI init failed, continuing without display");
    }
    ui_lcd_set_state("Booting...");

    /* Initialize audio hardware */
    if (audio_hal_init() != 0) {
        ESP_LOGE(TAG, "Audio HAL init failed");
        ui_lcd_set_state("Audio Error");
        return;
    }

    /* Initialize Opus processor */
    if (opus_proc_init(&g_opus_proc) != 0) {
        ESP_LOGE(TAG, "Opus processor init failed");
        ui_lcd_set_state("Opus Error");
        return;
    }
    opus_proc_set_output_cb(&g_opus_proc, on_decoded_pcm, NULL);

    /* Initialize AFE handler (AFE + WakeNet) */
    afe_handler_config_t afe_cfg = {
        .on_wakeup = on_wake_word,
        .wakeup_userdata = NULL,
    };
    if (afe_handler_init(&afe_cfg) != 0) {
        ESP_LOGE(TAG, "AFE handler init failed");
        ui_lcd_set_state("AFE Error");
        return;
    }

    /* Start AFE tasks (feed + fetch) */
    afe_handler_start();

    ESP_LOGI(TAG, "All subsystems initialized. Say \"hi, jason\" or press button.");
    ESP_LOGI(TAG, "Free heap: %lu, PSRAM: %lu",
             (unsigned long)esp_get_free_heap_size(),
             (unsigned long)heap_caps_get_free_size(MALLOC_CAP_SPIRAM));

    ui_lcd_set_state("Connecting");
    ui_lcd_set_hint("Connecting to server...");

    /* Create FreeRTOS tasks */
    /* Audio play task on Core 1 (also feeds ref_rb for AEC) */
    int task_ret;
    task_ret = xTaskCreatePinnedToCore(audio_play_task, "audio_play",
                            4096, NULL, 21, NULL, 1);
    ESP_LOGI(TAG, "create audio play task ret=%d", task_ret);
    /* WebSocket TX task on Core 0 */

    task_ret = xTaskCreatePinnedToCore(ws_tx_task, "ws_tx",
                            4096, NULL, 15, NULL, 0);
    ESP_LOGI(TAG, "ws_tx_task task ret=%d", task_ret);

    /* Main FSM task on Core 0 */
    task_ret = xTaskCreatePinnedToCore(main_fsm_task, "main_fsm",
                            6144, NULL, 10, NULL, 0);
    ESP_LOGI(TAG, "main_fsm_task task ret=%d", task_ret);

    /* Wait a bit for AFE tasks to start */
    vTaskDelay(pdMS_TO_TICKS(500));

    // 打印所有任务状态和优先级
    ESP_LOGI("SCHED", "=== All Tasks ===");
    static char task_list_buf[2048];
    vTaskList(task_list_buf);
    ESP_LOGI("SCHED", "Task list:\n%s", task_list_buf);

    /* Immediately start connecting to server */
    set_app_state(APP_STATE_CONNECTING);
}
