#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>
#include <inttypes.h>
#include <pthread.h>
#include <glib.h>
#include <jansson.h>
#include <libwebsockets.h>
#include "audiobridge-deps/speex/speex_resampler.h"

#include "abmod_consumer.h"

#define QCAP 1024
#define ITEM_EVENT 1
#define ITEM_MIX 2
#define ITEM_PUB 3

typedef struct qitem_s {
    int type;
    /* Mix summary */
    uint64_t frame_seq;
    uint64_t talk_version;
    uint32_t rtp_ts;
    int channels;
    double energy_l;
    double energy_r;
    /* Event */
    char event_name[16];
    char user_id[128];
    /* Publish */
    char *pub_event_name;
    char *pub_payload;
} qitem;

/* Outbound message queue for WS thread */
typedef struct ws_msg_s {
    char *data;
    struct ws_msg_s *next;
} ws_msg;

typedef struct ws_client_s {
    struct lws_context *ctx;
    struct lws *wsi;
    pthread_mutex_t qmtx;
    ws_msg *qhead, *qtail;
    int connected;
    /* HTTP headers */
    char auth_value[256]; /* "Bearer sk-..." */
    char beta_value[64];  /* "realtime=v1" */
    /* URL parts */
    char host[128];
    int port;
    int use_ssl;
    char path[256];
    int transcription_mode; /* true if ?intent=transcription */
    /* Reconnect backoff */
    long long next_connect_us;
    int backoff_ms;
    /* RX buffer */
    char *rx;
    size_t rx_len;
    size_t rx_cap;
    /* Partial accumulator */
    char *partial;
    size_t partial_len;
    size_t partial_cap;
    /* Backref */
    struct abmod_consumer *owner;
} ws_client;

#define AUDIO_RING_CAP_SAMPLES (24000*10) /* 10s at 24k mono */
#define WS_CHUNK_SAMPLES_24K   (2400)     /* 100ms at 24kHz */

struct abmod_consumer {
    uint32_t rate;
    int channels;
    pthread_t th;
    int running;
    pthread_mutex_t mtx;
    pthread_cond_t cv;
    qitem queue[QCAP];
    size_t head, tail, size;
    /* Active users */
    char **active;
    size_t active_len, active_cap;
    /* Emitter */
    abmod_consumer_emit_cb emit_cb;
    void *emit_user;
    /* STT state */
    int stt_active;
    char stt_user[128];
    /* Audio ring (mono at input rate) */
    pthread_mutex_t a_mtx;
    pthread_cond_t a_cv;
    int16_t *a_ring;
    size_t a_cap;
    size_t a_len;
    size_t a_head;
    /* WS thread */
    pthread_t ws_th;
    int ws_running;
    ws_client *ws;
    /* Config */
    char *cfg_api_key;
    char *cfg_model;
    char *cfg_ws_url;
    char *cfg_prompt;
    /* Resampler (input-rate -> 16k) */
    SpeexResamplerState *resampler;
    /* Flow control */
    uint64_t samples_since_commit;
    int force_final;
};

/* Forward declarations */
static void *ws_thread(void *arg);
static int ws_lws_callback(struct lws *wsi, enum lws_callback_reasons reason, void *user, void *in, size_t len);
static void ws_enqueue(ws_client *ws, const char *json_text);
static void ws_send_session_update(ws_client *ws, const char *instructions);
static void ws_send_audio_append(ws_client *ws, const void *pcm16_24k, size_t samples);
static void ws_send_commit_and_request(ws_client *ws);
static void consumer_publish_partial(struct abmod_consumer *c, const char *user, const char *text);
static void consumer_publish_final(struct abmod_consumer *c, const char *user, const char *text);

/* Minimal helpers */
static void str_assign(char **dst, const char *src) {
    if(*dst) { free(*dst); *dst = NULL; }
    if(src) *dst = strdup(src);
}

static void *consumer_thread(void *arg) {
    abmod_consumer *c = (abmod_consumer*)arg;
    while(1) {
        pthread_mutex_lock(&c->mtx);
        while(c->running && c->size == 0)
            pthread_cond_wait(&c->cv, &c->mtx);
        if(!c->running && c->size == 0) {
            pthread_mutex_unlock(&c->mtx);
            break;
        }
        qitem it = c->queue[c->head];
        c->head = (c->head + 1) % QCAP;
        c->size--;
        pthread_mutex_unlock(&c->mtx);
        if(it.type == ITEM_PUB) {
            if(c->emit_cb && it.pub_event_name) {
                c->emit_cb(c->emit_user, it.pub_event_name, it.pub_payload);
            }
            if(it.pub_event_name) free(it.pub_event_name);
            if(it.pub_payload) free(it.pub_payload);
        } else if(it.type == ITEM_EVENT) {
            if(strcmp(it.event_name, "talking") == 0) {
                int found = 0;
                for(size_t i=0;i<c->active_len;i++) if(c->active[i] && strcmp(c->active[i], it.user_id) == 0) { found = 1; break; }
                if(!found) {
                    if(c->active_len == c->active_cap) {
                        size_t ncap = c->active_cap ? c->active_cap*2 : 8;
                        char **n = (char**)calloc(ncap, sizeof(char*));
                        memcpy(n, c->active, c->active_len*sizeof(char*));
                        free(c->active);
                        c->active = n;
                        c->active_cap = ncap;
                    }
                    c->active[c->active_len] = strdup(it.user_id);
                    c->active_len++;
                }
                /* User started talking - track for transcription attribution */
                c->stt_active = 1;
                snprintf(c->stt_user, sizeof(c->stt_user), "%s", it.user_id);
                /* Clear partial accumulator on WS side for new speaker */
                if(c->ws) {
                    pthread_mutex_lock(&c->ws->qmtx);
                    if(c->ws->partial) c->ws->partial_len = 0;
                    pthread_mutex_unlock(&c->ws->qmtx);
                }
            } else {
                for(size_t i=0;i<c->active_len;i++) if(c->active[i] && strcmp(c->active[i], it.user_id) == 0) {
                    free(c->active[i]);
                    c->active[i] = c->active[c->active_len-1];
                    c->active[c->active_len-1] = NULL;
                    c->active_len--;
                    break;
                }
                /* User stopped talking - keep streaming, server VAD handles turn detection */
            }
        } else if(it.type == ITEM_MIX) {
            /* Audio is continuously streamed to API via audio ring buffer in enqueue_mix_pcm */
            /* ITEM_MIX events are queued for potential future use (e.g., energy levels, metadata) */
        }
    }
    return NULL;
}

abmod_consumer *abmod_consumer_create(uint32_t sampling_rate, int channels) {
    abmod_consumer *c = (abmod_consumer*)calloc(1, sizeof(*c));
    if(!c) return NULL;
    c->rate = sampling_rate;
    c->channels = channels;
    pthread_mutex_init(&c->mtx, NULL);
    pthread_cond_init(&c->cv, NULL);
    pthread_mutex_init(&c->a_mtx, NULL);
    pthread_cond_init(&c->a_cv, NULL);
    c->running = 1;
    c->ws_running = 1;
    /* Env config */
    const char *api_key = getenv("OPENAI_API_KEY");
    const char *model = getenv("ABMOD_OPENAI_MODEL");
    const char *ws_url = getenv("ABMOD_OPENAI_WS_URL");
    const char *prompt = getenv("ABMOD_OPENAI_PROMPT");
    str_assign(&c->cfg_api_key, api_key);
    str_assign(&c->cfg_model, model ? model : "gpt-4o-mini-transcribe");
    /* Use transcription-specific endpoint by default */
    str_assign(&c->cfg_ws_url, ws_url ? ws_url : "wss://api.openai.com/v1/realtime?intent=transcription");
    str_assign(&c->cfg_prompt, prompt);
    fprintf(stderr, "[abmod] init: rate=%u channels=%d model=%s url=%s\n",
            c->rate, c->channels,
            c->cfg_model ? c->cfg_model : "",
            c->cfg_ws_url ? c->cfg_ws_url : "");
    /* Audio ring */
    c->a_cap = AUDIO_RING_CAP_SAMPLES;
    c->a_ring = (int16_t*)calloc(c->a_cap, sizeof(int16_t));
    /* Resampler for input-rate -> 24k mono (required by OpenAI) */
    int err = 0;
    c->resampler = speex_resampler_init(1, sampling_rate, 24000, SPEEX_RESAMPLER_QUALITY_VOIP, &err);
    if(!c->resampler || err != RESAMPLER_ERR_SUCCESS) {
        fprintf(stderr, "[abmod] Failed to init resampler (err=%d)\n", err);
    }
    pthread_create(&c->th, NULL, consumer_thread, c);
    /* WS client */
    c->ws = (ws_client*)calloc(1, sizeof(ws_client));
    c->ws->owner = c;
    pthread_mutex_init(&c->ws->qmtx, NULL);
    c->ws->rx_cap = 65536; c->ws->rx_len = 0; c->ws->rx = (char*)malloc(c->ws->rx_cap);
    c->ws->partial_cap = 4096; c->ws->partial_len = 0; c->ws->partial = (char*)malloc(c->ws->partial_cap);
    c->ws->backoff_ms = 10000; c->ws->next_connect_us = 0;
    /* Parse URL into host/port/path */
    const char *url = c->cfg_ws_url;
    c->ws->use_ssl = (strncmp(url, "wss://", 6) == 0); int offset = c->ws->use_ssl ? 6 : (strncmp(url, "ws://", 5)==0 ? 5 : 0);
    const char *host_start = url + offset;
    const char *slash = strchr(host_start, '/');
    size_t host_len = slash ? (size_t)(slash - host_start) : strlen(host_start);
    if(host_len >= sizeof(c->ws->host)) host_len = sizeof(c->ws->host)-1;
    memcpy(c->ws->host, host_start, host_len); c->ws->host[host_len] = '\0';
    /* Preserve full path including query (?intent=...) if present */
    if(slash && *slash) {
        snprintf(c->ws->path, sizeof(c->ws->path), "%s", slash);
    } else {
        /* Fallback to transcription-specific endpoint */
        snprintf(c->ws->path, sizeof(c->ws->path), "/v1/realtime?intent=transcription");
    }
    c->ws->port = c->ws->use_ssl ? 443 : 80;
    
    /* Detect if using transcription intent mode */
    c->ws->transcription_mode = (strstr(c->ws->path, "intent=transcription") != NULL);
    
    /* Store header values (not full "Header: value" format) */
    snprintf(c->ws->auth_value, sizeof(c->ws->auth_value), "Bearer %s", c->cfg_api_key ? c->cfg_api_key : "");
    snprintf(c->ws->beta_value, sizeof(c->ws->beta_value), "realtime=v1");
    fprintf(stderr, "[abmod] ws target host=%s port=%d path=%s\n", c->ws->host, c->ws->port, c->ws->path);
    fprintf(stderr, "[abmod] Transcription mode: %s\n", c->ws->transcription_mode ? "YES (intent=transcription)" : "NO (full realtime)");
    fprintf(stderr, "[abmod] API key loaded: %s\n", c->cfg_api_key ? "YES" : "NO (check OPENAI_API_KEY env var!)");
    if(!c->cfg_api_key || strlen(c->cfg_api_key) < 10) {
        fprintf(stderr, "[abmod] WARNING: API key appears to be missing or invalid!\n");
    }
    pthread_create(&c->ws_th, NULL, ws_thread, c);
    return c;
}

void abmod_consumer_destroy(abmod_consumer *c) {
    if(!c) return;
    pthread_mutex_lock(&c->mtx);
    c->running = 0;
    pthread_cond_broadcast(&c->cv);
    pthread_mutex_unlock(&c->mtx);
    pthread_join(c->th, NULL);
    /* Stop WS */
    c->ws_running = 0;
    pthread_cond_broadcast(&c->a_cv);
    if(c->ws_th) pthread_join(c->ws_th, NULL);
    if(c->ws) {
        pthread_mutex_destroy(&c->ws->qmtx);
        free(c->ws->rx);
        free(c->ws->partial);
        free(c->ws);
    }
    if(c->active) {
        for(size_t i=0;i<c->active_len;i++) free(c->active[i]);
        free(c->active);
    }
    if(c->a_ring) free(c->a_ring);
    if(c->resampler) speex_resampler_destroy(c->resampler);
    free(c->cfg_api_key);
    free(c->cfg_model);
    free(c->cfg_ws_url);
    free(c->cfg_prompt);
    pthread_mutex_destroy(&c->mtx);
    pthread_cond_destroy(&c->cv);
    pthread_mutex_destroy(&c->a_mtx);
    pthread_cond_destroy(&c->a_cv);
    fprintf(stderr, "[abmod] consumer destroyed\n");
    free(c);
}

void abmod_consumer_set_emitter(abmod_consumer *c,
        abmod_consumer_emit_cb cb,
        void *user) {
    if(!c) return;
    pthread_mutex_lock(&c->mtx);
    c->emit_cb = cb;
    c->emit_user = user;
    pthread_mutex_unlock(&c->mtx);
}

void abmod_consumer_enqueue_event(abmod_consumer *c, const char *event_name, const char *user_id) {
    if(!c) return;
    pthread_mutex_lock(&c->mtx);
    if(c->size < QCAP) {
        size_t pos = (c->head + c->size) % QCAP;
        qitem *it = &c->queue[pos];
        it->type = ITEM_EVENT;
        snprintf(it->event_name, sizeof(it->event_name), "%s", event_name ? event_name : "");
        snprintf(it->user_id, sizeof(it->user_id), "%s", user_id ? user_id : "");
        c->size++;
        pthread_cond_signal(&c->cv);
    }
    pthread_mutex_unlock(&c->mtx);
}

void abmod_consumer_enqueue_mix_pcm(abmod_consumer *c,
        const int16_t *pcm, size_t samples,
        uint32_t rtp_timestamp, uint64_t frame_seq,
        uint64_t active_talk_version, int channels) {
    if(!c || !pcm) return;
    /* Compute quick energies */
    double el = 0.0, er = 0.0;
    if(channels == 2) {
        for(size_t i=0;i<samples; i++) {
            int16_t s = pcm[i];
            if((i & 1) == 0) el += (double)s * (double)s; else er += (double)s * (double)s;
        }
    } else {
        for(size_t i=0;i<samples; i++) { int16_t s = pcm[i]; el += (double)s * (double)s; }
        er = el;
    }
    /* Always feed mono audio to ring for continuous transcription */
    pthread_mutex_lock(&c->a_mtx);
    if(c->a_ring) {
        /* Downmix to mono at input rate */
        size_t in_mono = channels == 2 ? (samples/2) : samples;
        /* Write directly into ring, converting on the fly */
        for(size_t k=0;k<in_mono;k++) {
            int16_t m;
            if(channels == 2) {
                int32_t l = pcm[2*k];
                int32_t r = pcm[2*k+1];
                m = (int16_t)((l + r) / 2);
            } else {
                m = pcm[k];
            }
            if(c->a_len < c->a_cap) {
                size_t idx = (c->a_head + c->a_len) % c->a_cap;
                c->a_ring[idx] = m;
                c->a_len++;
            } else {
                /* Drop oldest */
                c->a_ring[c->a_head] = m;
                c->a_head = (c->a_head + 1) % c->a_cap;
            }
        }
        pthread_cond_signal(&c->a_cv);
    }
    pthread_mutex_unlock(&c->a_mtx);
    pthread_mutex_lock(&c->mtx);
    if(c->size < QCAP) {
        size_t pos = (c->head + c->size) % QCAP;
        qitem *it = &c->queue[pos];
        it->type = ITEM_MIX;
        it->frame_seq = frame_seq;
        it->talk_version = active_talk_version;
        it->rtp_ts = rtp_timestamp;
        it->channels = channels;
        it->energy_l = el;
        it->energy_r = er;
        c->size++;
        pthread_cond_signal(&c->cv);
    }
    pthread_mutex_unlock(&c->mtx);
}

static void abmod_consumer_publish_(abmod_consumer *c,
        const char *event_name,
        const char *json_payload) {
    if(!c || !event_name) return;
    pthread_mutex_lock(&c->mtx);
    if(c->size < QCAP) {
        size_t pos = (c->head + c->size) % QCAP;
        qitem *it = &c->queue[pos];
        it->type = ITEM_PUB;
        it->pub_event_name = strdup(event_name);
        it->pub_payload = json_payload ? strdup(json_payload) : NULL;
        c->size++;
        pthread_cond_signal(&c->cv);
    }
    pthread_mutex_unlock(&c->mtx);
}

void abmod_consumer_publish_partial(abmod_consumer *c, const char *json_payload) {
    abmod_consumer_publish_(c, "transcription.partial", json_payload);
}

void abmod_consumer_publish_final(abmod_consumer *c, const char *json_payload) {
    abmod_consumer_publish_(c, "transcription.final", json_payload);
}

void abmod_consumer_publish_error(abmod_consumer *c, const char *json_payload) {
    abmod_consumer_publish_(c, "transcription.error", json_payload);
}

/* ---------------------------- WS integration ---------------------------- */
/*
 * WebSocket client implementation for OpenAI Realtime API
 * 
 * Two modes supported:
 * 
 * 1. TRANSCRIPTION INTENT MODE (default):
 *    URL: wss://api.openai.com/v1/realtime?intent=transcription
 *    - Simplified transcription-only endpoint
 *    - Audio format: pcm16 (24 kHz mono PCM, resampled from input rate)
 *    - Model: gpt-4o-mini-transcribe (default)
 *    - Event type: transcription_session.update
 *    - Transcription events: transcription.delta, transcription.completed
 *    - No audio output from OpenAI
 *    - Server-side VAD automatically detects speech
 * 
 * 2. FULL REALTIME MODE:
 *    URL: wss://api.openai.com/v1/realtime?model=gpt-4o-realtime-preview-2024-10-01
 *    - Full conversational AI capabilities
 *    - Audio format: pcm16 (24 kHz mono PCM)
 *    - Event type: session.update
 *    - Events: conversation.item.input_audio_transcription.*
 *    - Can generate audio responses (disabled in our config)
 * 
 * Connection approach:
 * - Uses libwebsockets for WebSocket connectivity
 * - Required headers: Authorization (Bearer token), OpenAI-Beta: realtime=v1
 * - SSL/TLS connection to api.openai.com:443
 * 
 * Message flow:
 * 1. Connect and send transcription_session.update / session.update
 * 2. Stream audio via input_audio_buffer.append (base64-encoded PCM16 @ 24kHz)
 * 3. Receive transcription deltas and completed events
 */

static void ws_enqueue(ws_client *ws, const char *json_text) {
    if(!ws || !json_text) return;
    ws_msg *m = (ws_msg*)calloc(1, sizeof(ws_msg));
    m->data = strdup(json_text);
    pthread_mutex_lock(&ws->qmtx);
    if(ws->qtail) ws->qtail->next = m; else ws->qhead = m;
    ws->qtail = m;
    pthread_mutex_unlock(&ws->qmtx);
    if(ws->ctx && ws->wsi)
        lws_callback_on_writable(ws->wsi);
}

static void ws_send_session_update(ws_client *ws, const char *instructions) {
    if(!ws) return;
    const abmod_consumer *c = ws->owner;
    const char *model = c && c->cfg_model ? c->cfg_model : "gpt-4o-mini-transcribe";
    const char *prompt = c ? c->cfg_prompt : NULL;
    const char *lang = getenv("ABMOD_OPENAI_LANG");
    const char *noise_reduction = getenv("ABMOD_OPENAI_NOISE_REDUCTION"); /* "near_field" or "far_field" */
    char buf[4096];
    
    if(ws->transcription_mode) {
        /* Transcription intent mode: use actual working format */
        char transcription_json[256];
        char noise_json[256] = "";
        
        /* Build transcription config with optional prompt and language */
        if(prompt && *prompt) {
            char *pj = json_dumps(json_string(prompt), JSON_ENCODE_ANY);
            if(pj && lang && *lang) {
                snprintf(transcription_json, sizeof(transcription_json), 
                         "{\"model\":\"%s\",\"prompt\":%s,\"language\":\"%s\"}", model, pj, lang);
            } else if(pj) {
                snprintf(transcription_json, sizeof(transcription_json), 
                         "{\"model\":\"%s\",\"prompt\":%s}", model, pj);
            } else {
                snprintf(transcription_json, sizeof(transcription_json), 
                         "{\"model\":\"%s\"}", model);
            }
            if(pj) free(pj);
        } else if(lang && *lang) {
            snprintf(transcription_json, sizeof(transcription_json), 
                     "{\"model\":\"%s\",\"language\":\"%s\"}", model, lang);
        } else {
            snprintf(transcription_json, sizeof(transcription_json), 
                     "{\"model\":\"%s\"}", model);
        }
        
        /* Build optional noise reduction */
        if(noise_reduction && *noise_reduction) {
            snprintf(noise_json, sizeof(noise_json), 
                     ",\"input_audio_noise_reduction\":{\"type\":\"%s\"}", noise_reduction);
        }
        
        /* Use actual working format for transcription intent */
        snprintf(buf, sizeof(buf),
          "{\"type\":\"transcription_session.update\",\"session\":{"
          "\"input_audio_format\":\"pcm16\","
          "\"input_audio_transcription\":%s,"
          "\"turn_detection\":{\"type\":\"server_vad\",\"threshold\":0.5,\"prefix_padding_ms\":300,\"silence_duration_ms\":500}%s"
          "}}",
          transcription_json,
          noise_json
        );
        /* Session configuration sent */
    } else {
        /* Full realtime mode: complete configuration */
        char prompt_json[1024];
        if(prompt && *prompt) {
            char *pj = json_dumps(json_string(prompt), JSON_ENCODE_ANY);
            snprintf(prompt_json, sizeof(prompt_json), "%s", pj ? pj : "null");
            if(pj) free(pj);
        } else {
            snprintf(prompt_json, sizeof(prompt_json), "null");
        }
        
        snprintf(buf, sizeof(buf),
          "{\"type\":\"session.update\",\"session\":{"
          "\"modalities\":[\"text\"],"
          "\"input_audio_format\":\"pcm16\","
          "\"output_audio_format\":\"pcm16\","
          "\"input_audio_transcription\":{\"model\":\"%s\"%s%s},"
          "\"turn_detection\":{\"type\":\"server_vad\",\"threshold\":0.5,\"prefix_padding_ms\":300,\"silence_duration_ms\":500},"
          "\"instructions\":\"You are a transcription assistant. Transcribe the audio accurately.\""
          "}}",
          model,
          (prompt && *prompt) ? ",\"prompt\":" : "",
          (prompt && *prompt) ? prompt_json : ""
        );
        /* Session configuration sent */
    }
    
    ws_enqueue(ws, buf);
}

static void ws_send_audio_append(ws_client *ws, const void *pcm16_24k, size_t samples) {
    if(!ws || !pcm16_24k || samples == 0) return;
    size_t bytes = samples * sizeof(int16_t);
    char *b64 = (char*)g_base64_encode((const guchar*)pcm16_24k, bytes);
    if(!b64) return;
    size_t json_cap = strlen(b64) + 64;
    char *json = (char*)malloc(json_cap);
    snprintf(json, json_cap, "{\"type\":\"input_audio_buffer.append\",\"audio\":\"%s\"}", b64);
    ws_enqueue(ws, json);
    /* Verbose logging removed - audio is continuously streaming */
    g_free(b64);
    free(json);
}

static void ws_send_commit_and_request(ws_client *ws) {
    if(!ws || ws->transcription_mode) return; /* in intent mode, VAD commits */
    ws_enqueue(ws, "{\"type\":\"input_audio_buffer.commit\"}");
    ws_enqueue(ws, "{\"type\":\"response.create\"}");
}

static void ws_process_incoming(ws_client *ws, const char *msg, size_t len) {
    (void)len;
    if(!ws || !msg) return;
    /* Parse without verbose logging */
    json_error_t jerr; json_t *root = json_loads(msg, 0, &jerr);
    if(!root) { fprintf(stderr, "[abmod] JSON parse error at %d: %s\n", (int)jerr.position, jerr.text); return; }
    const char *type = json_string_value(json_object_get(root, "type"));
    
    if(type && strcmp(type, "response.delta")==0) {
        const char *delta = json_string_value(json_object_get(root, "delta"));
        if(delta && delta[0]) {
            /* accumulate */
            pthread_mutex_lock(&ws->qmtx);
            size_t need = ws->partial_len + strlen(delta) + 1;
            if(need > ws->partial_cap) { ws->partial_cap = need*2; ws->partial = (char*)realloc(ws->partial, ws->partial_cap); }
            memcpy(ws->partial + ws->partial_len, delta, strlen(delta)+1);
            ws->partial_len += strlen(delta);
            pthread_mutex_unlock(&ws->qmtx);
            /* publish partial */
            consumer_publish_partial(ws->owner, ws->owner->stt_user, ws->partial);
        }
    } else if((type && strcmp(type, "response.completed")==0) || (type && strcmp(type, "transcription.completed")==0)) {
        /* Final transcription from either mode */
        pthread_mutex_lock(&ws->qmtx);
        const char *final_text = ws->partial_len > 0 ? ws->partial : NULL;
        if(final_text && final_text[0]) {
            fprintf(stderr, "[abmod] FINAL: '%s'\n", final_text);
            consumer_publish_final(ws->owner, ws->owner->stt_user, final_text);
        }
        ws->partial_len = 0;
        pthread_mutex_unlock(&ws->qmtx);
    } else if(type && strcmp(type, "transcription.delta")==0) {
        /* Transcription intent mode event */
        const char *delta = json_string_value(json_object_get(root, "delta"));
        if(delta && delta[0]) {
            fprintf(stderr, "[abmod] DELTA: '%s'\n", delta);
            pthread_mutex_lock(&ws->qmtx);
            size_t need = ws->partial_len + strlen(delta) + 1;
            if(need > ws->partial_cap) { ws->partial_cap = need*2; ws->partial = (char*)realloc(ws->partial, ws->partial_cap); }
            memcpy(ws->partial + ws->partial_len, delta, strlen(delta)+1);
            ws->partial_len += strlen(delta);
            pthread_mutex_unlock(&ws->qmtx);
            consumer_publish_partial(ws->owner, ws->owner->stt_user, ws->partial);
        }
    } else if(type && strcmp(type, "response.audio_transcript.delta")==0) {
        const char *delta = json_string_value(json_object_get(root, "delta"));
        if(delta && delta[0]) {
            fprintf(stderr, "[abmod] DELTA: '%s'\n", delta);
            consumer_publish_partial(ws->owner, ws->owner->stt_user, delta);
        }
    } else if(type && strcmp(type, "response.audio_transcript.done")==0) {
        consumer_publish_final(ws->owner, ws->owner->stt_user, "");
    } else if(type && strcmp(type, "conversation.item.input_audio_transcription.delta")==0) {
        const char *delta = json_string_value(json_object_get(root, "delta"));
        if(delta && delta[0]) {
            fprintf(stderr, "[abmod] DELTA: '%s'\n", delta);
            consumer_publish_partial(ws->owner, ws->owner->stt_user, delta);
        }
    } else if(type && strcmp(type, "conversation.item.input_audio_transcription.completed")==0) {
        const char *txt = json_string_value(json_object_get(root, "transcript"));
        if(txt && txt[0]) {
            fprintf(stderr, "[abmod] FINAL: '%s'\n", txt);
            consumer_publish_final(ws->owner, ws->owner->stt_user, txt);
        }
    } else if(type && strcmp(type, "input_audio_buffer.committed")==0) {
        /* Silent - audio committed */
    } else if(type && strcmp(type, "session.created")==0) {
        fprintf(stderr, "[abmod] Session created\n");
    } else if(type && strcmp(type, "session.updated")==0) {
        fprintf(stderr, "[abmod] Session updated successfully\n");
    } else if(type && strcmp(type, "error")==0) {
        const char *err_msg = json_string_value(json_object_get(json_object_get(root, "error"), "message"));
        fprintf(stderr, "[abmod] ERROR: %s\n", err_msg ? err_msg : "(unknown)");
    }
    /* Other event types are silently ignored */
    json_decref(root);
}

static int ws_lws_callback(struct lws *wsi, enum lws_callback_reasons reason, void *user, void *in, size_t len) {
    (void)user;
    ws_client *ws = (ws_client*)lws_context_user(lws_get_context(wsi));
    switch(reason) {
        case LWS_CALLBACK_CLIENT_ESTABLISHED:
            ws->wsi = wsi;
            ws->connected = 1;
            ws->backoff_ms = 10000; 
            ws->next_connect_us = 0;
            fprintf(stderr, "[abmod] Connected to OpenAI Realtime endpoint (host=%s path=%s)\n", ws->host, ws->path);
            /* Send appropriate session update based on mode */
            if(ws->transcription_mode) {
                ws_send_session_update(ws, NULL);
            } else if(ws->owner && ws->owner->cfg_prompt) {
                ws_send_session_update(ws, ws->owner->cfg_prompt);
            }
            lws_callback_on_writable(wsi);
            break;
        case LWS_CALLBACK_CLIENT_CONNECTION_ERROR:
            fprintf(stderr, "[abmod] Connection error: %s\n", in ? (char *)in : "(null)");
            {
                int http_status = lws_http_client_http_response(wsi);
                if(http_status > 0)
                    fprintf(stderr, "[abmod] HTTP status=%d\n", http_status);
            }
            ws->connected = 0;
            ws->wsi = NULL;
            ws->next_connect_us = g_get_monotonic_time() + 10000000LL; /* 10s backoff */
            break;
        case LWS_CALLBACK_CLOSED:
        case LWS_CALLBACK_CLIENT_CLOSED:
            fprintf(stderr, "[abmod] Connection closed\n");
            ws->connected = 0;
            ws->wsi = NULL;
            ws->next_connect_us = g_get_monotonic_time() + 10000000LL; /* 10s backoff */
            break;
        case LWS_CALLBACK_WSI_DESTROY:
            ws->connected = 0;
            ws->wsi = NULL;
            ws->next_connect_us = g_get_monotonic_time() + 10000000LL; /* 10s backoff */
            break;
        case LWS_CALLBACK_CLIENT_APPEND_HANDSHAKE_HEADER: {
            unsigned char **p = (unsigned char **)in, *end = (*p) + len;
            
            /* Add authorization header using token method */
            if(lws_add_http_header_by_token(wsi, WSI_TOKEN_HTTP_AUTHORIZATION,
                                            (const unsigned char*)ws->auth_value,
                                            strlen(ws->auth_value), p, end)) {
                fprintf(stderr, "[abmod] ERROR: Failed to add authorization header\n");
                return -1;
            }
            
            /* Add OpenAI-Beta header (custom header, use by_name) */
            if(lws_add_http_header_by_name(wsi, (const unsigned char*)"openai-beta:",
                                          (const unsigned char*)ws->beta_value,
                                          strlen(ws->beta_value), p, end)) {
                fprintf(stderr, "[abmod] ERROR: Failed to add openai-beta header\n");
                return -1;
            }
            break;
        }
        case LWS_CALLBACK_CLIENT_RECEIVE: {
            if(len == 0) break;
            /* Accumulate received data in case of fragmented messages */
            if(ws->rx_len + len + 1 > ws->rx_cap) { 
                ws->rx_cap = (ws->rx_len + len + 1)*2; 
                ws->rx = (char*)realloc(ws->rx, ws->rx_cap); 
            }
            memcpy(ws->rx + ws->rx_len, in, len);
            ws->rx_len += len; 
            ws->rx[ws->rx_len] = '\0';
            
            /* Check if this is a complete message (JSON frame) */
            int is_final = lws_is_final_fragment(wsi);
            if(is_final) {
                /* Process complete message */
                ws_process_incoming(ws, ws->rx, ws->rx_len);
                ws->rx_len = 0; /* reset buffer for next message */
            }
            break;
        }
        case LWS_CALLBACK_RECEIVE_CLIENT_HTTP:
        case LWS_CALLBACK_RECEIVE_CLIENT_HTTP_READ: {
            /* Drain HTTP body to avoid spinning if upgrade failed */
            const char *bp = (const char*)in;
            size_t blen = len;
            do {
                char *pp = (char*)bp;
                size_t plen = blen;
                int n = lws_http_client_read(wsi, &pp, &plen);
                if(n < 0) break;
                if(n == 0) break;
            } while(1);
            return 0;
        }
        case LWS_CALLBACK_COMPLETED_CLIENT_HTTP:
        case LWS_CALLBACK_CLOSED_CLIENT_HTTP:
            ws->connected = 0;
            ws->wsi = NULL;
            return 0;
        case LWS_CALLBACK_CLIENT_WRITEABLE: {
            /* Pop one queued message */
            pthread_mutex_lock(&ws->qmtx);
            ws_msg *m = ws->qhead;
            if(m) {
                ws->qhead = m->next; if(!ws->qhead) ws->qtail = NULL;
            }
            pthread_mutex_unlock(&ws->qmtx);
            if(m) {
                size_t n = strlen(m->data);
                unsigned char *buf = (unsigned char*)malloc(LWS_PRE + n);
                memcpy(buf + LWS_PRE, m->data, n);
                lws_write(wsi, buf + LWS_PRE, n, LWS_WRITE_TEXT);
                free(buf);
                free(m->data);
                free(m);
                lws_callback_on_writable(wsi);
            }
            break;
        }
        case LWS_CALLBACK_EVENT_WAIT_CANCELLED:
            if(ws->wsi && ws->qhead)
                lws_callback_on_writable(ws->wsi);
            return 0;
        default:
            break;
    }
    return 0;
}

static const struct lws_protocols abmod_ws_protocols[] = {
    { "openai-realtime", ws_lws_callback, 0, 4096 }, /* 4KB rx buffer per connection */
    { NULL, NULL, 0, 0 } /* terminator */
};

static void abmod_lws_log_emit(int level, const char *line) {
    (void)level;
    fprintf(stderr, "[abmod][lws] %s", line);
  }

static void *ws_thread(void *arg) {
    abmod_consumer *c = (abmod_consumer*)arg;
    ws_client *ws = c->ws;
    /* Build lws context */
    struct lws_context_creation_info info; memset(&info, 0, sizeof(info));
    info.port = CONTEXT_PORT_NO_LISTEN;
    info.protocols = abmod_ws_protocols;
    info.options = LWS_SERVER_OPTION_DO_SSL_GLOBAL_INIT;
    info.user = ws;
    ws->ctx = lws_create_context(&info);
    if(!ws->ctx) {
        fprintf(stderr, "[abmod] Failed to create lws context\n");
        return NULL;
    }
    lws_set_log_level(LLL_ERR | LLL_WARN | LLL_NOTICE | LLL_CLIENT | LLL_HEADER, abmod_lws_log_emit);
    while(c->ws_running) {
        if(!ws->connected && ws->wsi == NULL) {
            /* Throttle connection attempts */
            long long now = g_get_monotonic_time();
            if(ws->next_connect_us != 0 && now < ws->next_connect_us) {
                lws_service(ws->ctx, 10);
                continue;
            }
            /* Connect - headers will be added in LWS_CALLBACK_CLIENT_APPEND_HANDSHAKE_HEADER */
            struct lws_client_connect_info ci; 
            memset(&ci, 0, sizeof(ci));
            ci.context = ws->ctx;
            ci.address = ws->host;
            ci.port = ws->port;
            ci.path = ws->path;
            ci.host = ci.address;
            ci.origin = ci.address;
            ci.protocol = NULL; /* OpenAI doesn't use WebSocket subprotocols */
            ci.local_protocol_name = "openai-realtime"; /* Bind to our local protocol/callback */
            ci.ssl_connection = ws->use_ssl ? LCCSCF_USE_SSL : 0;
            ci.alpn = "http/1.1"; /* Force HTTP/1.1 to avoid HTTP/2 issues */
            ci.pwsi = &ws->wsi;
            ci.userdata = ws;
            
            if(!lws_client_connect_via_info(&ci)) {
                fprintf(stderr, "[abmod] Connection failed (host=%s)\n", ws->host);
                /* Backoff before next attempt (fixed 10s) */
                ws->next_connect_us = now + 10000000LL;
                ws->wsi = NULL;
            }
        }
        /* Drain audio ring: resample and send chunks (continuous streaming) */
        {
            /* Pull up to a chunk worth of mono input samples */
            int16_t in_buf[4800]; /* up to 100ms at 48k */
            spx_uint32_t in_len = 0;
            pthread_mutex_lock(&c->a_mtx);
            /* Wait for audio if buffer is empty */
            while(c->ws_running && c->a_len == 0)
                pthread_cond_wait(&c->a_cv, &c->a_mtx);
            size_t avail = c->a_len;
            size_t take = avail > 4800 ? 4800 : avail;
            for(size_t i=0;i<take;i++) {
                in_buf[i] = c->a_ring[c->a_head];
                c->a_head = (c->a_head + 1) % c->a_cap;
            }
            c->a_len -= take;
            pthread_mutex_unlock(&c->a_mtx);
            in_len = (spx_uint32_t)take;
            if(in_len > 0 && c->resampler && ws->connected) {
                /* Resample to 24kHz (required by OpenAI) */
                int16_t out_buf[WS_CHUNK_SAMPLES_24K*4];
                spx_uint32_t out_len = WS_CHUNK_SAMPLES_24K*4;
                speex_resampler_process_int(c->resampler, 0, in_buf, &in_len, out_buf, &out_len);
                if(out_len > 0) {
                    ws_send_audio_append(ws, out_buf, out_len);
                    c->samples_since_commit += out_len;
                }
            }
        }
        lws_service(ws->ctx, 10);
    }
    if(ws->ctx) { lws_context_destroy(ws->ctx); ws->ctx = NULL; }
    fprintf(stderr, "[abmod] WS thread destroyed\n");
    return NULL;
}

static void consumer_publish_partial(struct abmod_consumer *c, const char *user, const char *text) {
    if(!c || !user || !text) return;
    char *payload = NULL;
    size_t cap = strlen(user) + strlen(text) + 64;
    payload = (char*)malloc(cap);
    snprintf(payload, cap, "{\"user\":\"%s\",\"text\":%s}", user, json_dumps(json_string(text), JSON_ENCODE_ANY));
    abmod_consumer_publish_partial(c, payload);
    free(payload);
}

static void consumer_publish_final(struct abmod_consumer *c, const char *user, const char *text) {
    if(!c || !user || !text) return;
    char *payload = NULL;
    size_t cap = strlen(user) + strlen(text) + 64;
    payload = (char*)malloc(cap);
    snprintf(payload, cap, "{\"user\":\"%s\",\"text\":%s}", user, json_dumps(json_string(text), JSON_ENCODE_ANY));
    abmod_consumer_publish_final(c, payload);
    free(payload);
}


