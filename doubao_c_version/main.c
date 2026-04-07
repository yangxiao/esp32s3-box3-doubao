#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <signal.h>
#include <unistd.h>
#include <getopt.h>
#include <pthread.h>

#include "config.h"
#include "protocol.h"
#include "client.h"
#include "audio.h"

/* ---- Session state ---- */
typedef struct {
    doubao_client_t client;
    audio_manager_t audio;
    char output_format[32];
    char input_mod[32];
    char audio_file[512];
    int recv_timeout;

    volatile bool running;
    volatile bool session_finished;
    volatile bool user_querying;
    volatile bool say_hello_done;
    volatile bool has_audio;      /* whether audio I/O is active */

    pthread_t service_thread;
    pthread_t input_thread;
    pthread_mutex_t state_mutex;
    pthread_cond_t hello_cond;
} session_t;

static session_t g_session;

/* ---- Signal handler ---- */
static void signal_handler(int sig) {
    (void)sig;
    printf("\nReceived Ctrl+C, shutting down...\n");
    g_session.running = false;
    g_session.client.running = false;
}

/* ---- WebSocket receive callback ---- */
static void on_ws_receive(const uint8_t *data, size_t len, void *userdata) {
    session_t *s = (session_t *)userdata;
    parsed_response_t resp;
    if (protocol_parse_response(data, len, &resp) != 0) {
        fprintf(stderr, "Failed to parse server response\n");
        return;
    }

    if (resp.message_type == MSG_SERVER_ACK && resp.is_binary && resp.payload_data) {
        /* OGG/Opus audio data — decode and enqueue PCM */
        if (s->has_audio) {
            audio_decode_ogg_opus(&s->audio, resp.payload_data, resp.payload_data_len);
        }
    } else if (resp.message_type == MSG_SERVER_FULL_RESPONSE) {
        /* Print text response */
        if (resp.payload_data && !resp.is_binary) {
            printf("Server: %.*s\n", (int)resp.payload_data_len, (char *)resp.payload_data);
        }

        if (resp.has_event) {
            switch (resp.event) {
            case EVENT_CLEAR_CACHE:
                printf("Event 450: clear audio cache\n");
                if (s->has_audio) {
                    audio_queue_clear(&s->audio);
                    audio_reset_ogg_state(&s->audio);
                }
                s->user_querying = true;
                break;
            case EVENT_USER_QUERY_END:
                printf("Event 459: user query end\n");
                s->user_querying = false;
                break;
            case EVENT_TTS_ENDED:
                printf("Event 359: TTS ended\n");
                if (!s->say_hello_done) {
                    pthread_mutex_lock(&s->state_mutex);
                    s->say_hello_done = true;
                    pthread_cond_signal(&s->hello_cond);
                    pthread_mutex_unlock(&s->state_mutex);

                    if (strcmp(s->input_mod, "text") == 0) {
                        printf("Please input text:\n");
                    }
                }
                break;
            case EVENT_SESSION_FINISH_1:
            case EVENT_SESSION_FINISH_2:
                printf("Event %d: session finished\n", resp.event);
                s->session_finished = true;
                break;
            default:
                break;
            }
        }
    } else if (resp.message_type == MSG_SERVER_ERROR) {
        fprintf(stderr, "Server error (code=%u): %.*s\n",
                resp.error_code,
                (int)(resp.payload_data_len),
                resp.payload_data ? (char *)resp.payload_data : "");
        s->running = false;
    }

    free(resp.payload_data);
}

/* ---- LWS service thread ---- */
static void *service_thread_func(void *arg) {
    session_t *s = (session_t *)arg;
    client_service_loop(&s->client);
    return NULL;
}

/* ---- Microphone input thread ---- */
static void *mic_input_thread_func(void *arg) {
    session_t *s = (session_t *)arg;

    /* Wait for say_hello to complete */
    pthread_mutex_lock(&s->state_mutex);
    while (!s->say_hello_done && s->running) {
        pthread_cond_wait(&s->hello_cond, &s->state_mutex);
    }
    pthread_mutex_unlock(&s->state_mutex);

    if (!s->running) return NULL;

    printf("Microphone active, speak now...\n");

    while (s->running) {
        size_t audio_len;
        uint8_t *audio_data = audio_read_input_opus(&s->audio, &audio_len);
        if (audio_data) {
            client_task_request(&s->client, audio_data, audio_len);
            free(audio_data);
        }
        usleep(10000); /* 10ms, avoid CPU spinning */
    }
    return NULL;
}

/* ---- Text input thread ---- */
static void *text_input_thread_func(void *arg) {
    session_t *s = (session_t *)arg;

    /* Wait for say_hello to complete */
    pthread_mutex_lock(&s->state_mutex);
    while (!s->say_hello_done && s->running) {
        pthread_cond_wait(&s->hello_cond, &s->state_mutex);
    }
    pthread_mutex_unlock(&s->state_mutex);

    if (!s->running) return NULL;

    char line[1024];
    while (s->running) {
        if (fgets(line, sizeof(line), stdin) == NULL) {
            break; /* EOF */
        }
        /* Trim newline */
        size_t l = strlen(line);
        while (l > 0 && (line[l - 1] == '\n' || line[l - 1] == '\r'))
            line[--l] = '\0';

        if (l > 0) {
            client_chat_text_query(&s->client, line);
        }
    }
    return NULL;
}

/* ---- Audio file input ---- */
static void process_audio_file(session_t *s) {
    FILE *f = fopen(s->audio_file, "rb");
    if (!f) {
        fprintf(stderr, "Cannot open audio file: %s\n", s->audio_file);
        return;
    }

    printf("Processing audio file: %s\n", s->audio_file);

    /* PCM 16-bit mono: chunk_size frames * 2 bytes/frame * 1 channel */
    size_t chunk_bytes = INPUT_CHUNK * 2 * INPUT_CHANNELS;
    uint8_t *buf = malloc(chunk_bytes);
    if (!buf) { fclose(f); return; }

    double sleep_sec = (double)INPUT_CHUNK / (double)INPUT_SAMPLE_RATE;

    while (s->running) {
        size_t nread = fread(buf, 1, chunk_bytes, f);
        if (nread == 0) break;
        client_task_request(&s->client, buf, nread);
        usleep((useconds_t)(sleep_sec * 1000000));
    }

    free(buf);
    fclose(f);
    printf("Audio file processing done, waiting for server...\n");
}

/* ---- Main ---- */
static void print_usage(const char *prog) {
    printf("Usage: %s [options]\n"
           "  --format <pcm|pcm_s16le>  Output audio format (default: pcm)\n"
           "  --audio <file>            Audio file to send (WAV/PCM)\n"
           "  --mod <audio|text>        Input mode (default: audio)\n"
           "  --recv_timeout <N>        Receive timeout 10-120 (default: 10)\n"
           "  --help                    Show this help\n",
           prog);
}

int main(int argc, char *argv[]) {
    /* Disable stdout buffering for immediate output */
    setbuf(stdout, NULL);

    /* Default options */
    char format[32] = "pcm";
    char audio_file[512] = "";
    char mod[32] = "audio";
    int recv_timeout = 10;

    static struct option long_options[] = {
        {"format",       required_argument, 0, 'f'},
        {"audio",        required_argument, 0, 'a'},
        {"mod",          required_argument, 0, 'm'},
        {"recv_timeout", required_argument, 0, 't'},
        {"help",         no_argument,       0, 'h'},
        {0, 0, 0, 0}
    };

    int opt;
    while ((opt = getopt_long(argc, argv, "f:a:m:t:h", long_options, NULL)) != -1) {
        switch (opt) {
        case 'f': strncpy(format, optarg, sizeof(format) - 1); break;
        case 'a': strncpy(audio_file, optarg, sizeof(audio_file) - 1); break;
        case 'm': strncpy(mod, optarg, sizeof(mod) - 1); break;
        case 't': recv_timeout = atoi(optarg); break;
        case 'h': print_usage(argv[0]); return 0;
        default:  print_usage(argv[0]); return 1;
        }
    }

    if (recv_timeout < 10) recv_timeout = 10;
    if (recv_timeout > 120) recv_timeout = 120;

    /* If audio file specified, force audio_file mode */
    bool is_audio_file = (strlen(audio_file) > 0);
    if (is_audio_file) {
        strcpy(mod, "audio_file");
    }

    /* Setup signal handler */
    signal(SIGINT, signal_handler);

    /* Initialize session */
    session_t *s = &g_session;
    memset(s, 0, sizeof(*s));
    strncpy(s->output_format, format, sizeof(s->output_format) - 1);
    strncpy(s->input_mod, mod, sizeof(s->input_mod) - 1);
    strncpy(s->audio_file, audio_file, sizeof(s->audio_file) - 1);
    s->recv_timeout = recv_timeout;
    s->running = true;
    s->session_finished = false;
    s->user_querying = false;
    s->say_hello_done = false;
    s->has_audio = !is_audio_file; /* audio I/O only for mic/text modes */
    pthread_mutex_init(&s->state_mutex, NULL);
    pthread_cond_init(&s->hello_cond, NULL);

    /* Generate session ID */
    char session_id[64];
    generate_uuid(session_id, sizeof(session_id));

    /* Init client */
    /* Use opus ASR format for mic/text modes, PCM (NULL) for audio file mode */
    const char *asr_fmt = is_audio_file ? NULL : "speech_opus";
    if (client_init(&s->client, session_id, format, mod, asr_fmt, recv_timeout) != 0) {
        return 1;
    }
    client_set_recv_callback(&s->client, on_ws_receive, s);

    /* Init audio (if not audio_file mode) */
    if (s->has_audio) {
        if (audio_init(&s->audio, format) != 0) {
            client_destroy(&s->client);
            return 1;
        }
        if (audio_open_output(&s->audio) != 0) {
            audio_cleanup(&s->audio);
            client_destroy(&s->client);
            return 1;
        }
    }

    /* Connect WebSocket */
    printf("Connecting to Doubao realtime dialog server...\n");
    if (client_connect(&s->client) != 0) {
        if (s->has_audio) audio_cleanup(&s->audio);
        client_destroy(&s->client);
        return 1;
    }

    /* Start LWS service thread */
    pthread_create(&s->service_thread, NULL, service_thread_func, s);

    /* StartConnection */
    printf("Sending StartConnection...\n");
    client_start_connection(&s->client);
    usleep(500000); /* wait for response */

    /* StartSession */
    printf("Sending StartSession...\n");
    client_start_session(&s->client);
    usleep(500000);

    /* SayHello */
    if (!is_audio_file) {
        printf("Sending SayHello...\n");
        client_say_hello(&s->client);
    }

    /* Start input thread based on mode */
    if (is_audio_file) {
        process_audio_file(s);
    } else if (strcmp(mod, "text") == 0) {
        pthread_create(&s->input_thread, NULL, text_input_thread_func, s);
    } else {
        /* Audio mode: open mic and start input thread */
        if (audio_open_input(&s->audio) != 0) {
            fprintf(stderr, "Failed to open microphone\n");
            s->running = false;
        } else {
            pthread_create(&s->input_thread, NULL, mic_input_thread_func, s);
        }
    }

    /* Wait until session ends or user quits */
    while (s->running && !s->session_finished) {
        usleep(100000); /* 100ms */
    }

    /* Graceful shutdown */
    printf("Finishing session...\n");
    client_finish_session(&s->client);
    /* Wait briefly for session finish events */
    for (int i = 0; i < 30 && !s->session_finished; i++) {
        usleep(100000);
    }

    printf("Finishing connection...\n");
    client_finish_connection(&s->client);
    usleep(200000);

    s->running = false;
    s->client.running = false;

    /* Wait for threads */
    pthread_join(s->service_thread, NULL);
    if (!is_audio_file && s->input_thread) {
        pthread_cancel(s->input_thread);
        pthread_join(s->input_thread, NULL);
    }

    /* Save output audio */
    if (s->has_audio) {
        audio_save_output(&s->audio, "output.ogg");
        audio_cleanup(&s->audio);
    }

    client_destroy(&s->client);
    pthread_mutex_destroy(&s->state_mutex);
    pthread_cond_destroy(&s->hello_cond);

    printf("Dialog session ended. logid: %s, mod: %s\n", s->client.logid, s->input_mod);
    return 0;
}
