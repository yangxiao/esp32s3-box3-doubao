#ifndef CONFIG_H
#define CONFIG_H

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

/* ---- Audio config ---- */
#define INPUT_SAMPLE_RATE   16000
#define INPUT_CHANNELS      1
#define INPUT_CHUNK         3200    /* frames per chunk */

#define OUTPUT_SAMPLE_RATE  48000
#define OUTPUT_CHANNELS     1
#define OUTPUT_CHUNK        2880    /* 60ms @ 48kHz */

/* Opus encoder frame size: 960 frames = 60ms @ 16kHz */
#define OPUS_FRAME_SIZE     960

/* ---- WebSocket config ---- */
#define WS_HOST         "openspeech.bytedance.com"
#define WS_PORT         443
#define WS_PATH         "/api/v3/realtime/dialogue"
#define WS_RESOURCE_ID  "volc.speech.dialog"
#define WS_APP_KEY      "PlgvMymc7f3tQnJ6"

/* ---- TTS speaker ---- */
#define TTS_SPEAKER     "zh_female_vv_jupiter_bigtts"

/* ---- UUID generation (simple v4-style) ---- */
static inline void generate_uuid(char *buf, size_t len) {
    static const char hex[] = "0123456789abcdef";
    /* 8-4-4-4-12 */
    static const int groups[] = {8, 4, 4, 4, 12};
    static int seeded = 0;
    if (!seeded) { srand((unsigned)time(NULL)); seeded = 1; }

    int pos = 0;
    for (int g = 0; g < 5; g++) {
        if (g > 0 && pos < (int)len - 1) buf[pos++] = '-';
        for (int i = 0; i < groups[g] && pos < (int)len - 1; i++) {
            buf[pos++] = hex[rand() % 16];
        }
    }
    buf[pos] = '\0';
}

/* ---- Build start_session JSON ---- */
/* asr_audio_format: "speech_opus" for opus mic input, or NULL to omit audio_info (PCM default) */
static inline const char *build_start_session_json(const char *asr_audio_format,
                                                    const char *input_mod,
                                                    int recv_timeout) {
    static char json_buf[2048];
    char asr_block[256];
    if (asr_audio_format) {
        snprintf(asr_block, sizeof(asr_block),
            "\"asr\":{"
                "\"audio_info\":{"
                    "\"format\":\"%s\","
                    "\"sample_rate\":16000,"
                    "\"channel\":1"
                "},"
                "\"extra\":{\"end_smooth_window_ms\":1500}"
            "},",
            asr_audio_format);
    } else {
        snprintf(asr_block, sizeof(asr_block),
            "\"asr\":{\"extra\":{\"end_smooth_window_ms\":1500}},");
    }
    snprintf(json_buf, sizeof(json_buf),
        "{"
            "%s"
            "\"tts\":{"
                "\"speaker\":\"%s\","
                "\"audio_config\":{"
                    "\"channel\":1,"
                    "\"format\":\"ogg_opus\","
                    "\"sample_rate\":24000"
                "}"
            "},"
            "\"dialog\":{"
                "\"bot_name\":\"豆包\","
                "\"system_role\":\"你使用活泼灵动的女声，性格开朗，热爱生活。\","
                "\"speaking_style\":\"你的说话风格简洁明了，语速适中，语调自然。\","
                "\"location\":{\"city\":\"北京\"},"
                "\"extra\":{"
                    "\"strict_audit\":false,"
                    "\"audit_response\":\"支持客户自定义安全审核回复话术。\","
                    "\"recv_timeout\":%d,"
                    "\"input_mod\":\"%s\""
                "}"
            "}"
        "}",
        asr_block, TTS_SPEAKER, recv_timeout, input_mod);
    return json_buf;
}

#endif /* CONFIG_H */
