#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>
#include <inttypes.h>
#include <pthread.h>

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
    /* Sample STT state */
    int stt_active;
    char stt_user[128];
    uint64_t stt_frames_since_partial;
    uint64_t stt_total_frames;
};

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
                fprintf(stderr, "[abmod] EVENT talk user=%s\n", it.user_id);
                /* Start sample STT for this user */
                c->stt_active = 1;
                snprintf(c->stt_user, sizeof(c->stt_user), "%s", it.user_id);
                c->stt_frames_since_partial = 0;
                c->stt_total_frames = 0;
            } else {
                for(size_t i=0;i<c->active_len;i++) if(c->active[i] && strcmp(c->active[i], it.user_id) == 0) {
                    free(c->active[i]);
                    c->active[i] = c->active[c->active_len-1];
                    c->active[c->active_len-1] = NULL;
                    c->active_len--;
                    break;
                }
                fprintf(stderr, "[abmod] EVENT stop user=%s\n", it.user_id);
                /* Finalize sample STT if active */
                if(c->stt_active && strcmp(c->stt_user, it.user_id) == 0) {
                    char payload[256];
                    snprintf(payload, sizeof(payload), "{\"user\":\"%s\",\"text\":\"final-%" PRIu64 "\"}", c->stt_user, c->stt_total_frames);
                    abmod_consumer_publish_final(c, payload);
                }
                c->stt_active = 0;
                c->stt_user[0] = '\0';
                c->stt_frames_since_partial = 0;
                c->stt_total_frames = 0;
            }
        } else if(it.type == ITEM_MIX) {
            const char *side = "equal";
            if(it.channels == 2) {
                if(it.energy_l > it.energy_r*1.2) side = "left";
                else if(it.energy_r > it.energy_l*1.2) side = "right";
            }
            //fprintf(stderr, "[abmod] MIX frame=%" PRIu64 " ts=%u v=%" PRIu64 " side=%s EL=%.0f ER=%.0f active=", it.frame_seq, it.rtp_ts, it.talk_version, side, it.energy_l, it.energy_r);
            // if(c->active_len == 0) {
            //     fprintf(stderr, "none\n");
            // } else {
            //     for(size_t i=0;i<c->active_len;i++) fprintf(stderr, "%s%s", c->active[i], (i+1<c->active_len)?",":"\n");
            // }
            /* Emit sample partial transcripts periodically while talking */
            if(c->stt_active && c->stt_user[0] != '\0') {
                c->stt_frames_since_partial++;
                c->stt_total_frames++;
                if(c->stt_frames_since_partial >= 50) {
                    c->stt_frames_since_partial = 0;
                    char payload[256];
                    snprintf(payload, sizeof(payload), "{\"user\":\"%s\",\"text\":\"partial-%" PRIu64 "\"}", c->stt_user, c->stt_total_frames);
                    abmod_consumer_publish_partial(c, payload);
                }
            }
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
    c->running = 1;
    pthread_create(&c->th, NULL, consumer_thread, c);
    return c;
}

void abmod_consumer_destroy(abmod_consumer *c) {
    if(!c) return;
    pthread_mutex_lock(&c->mtx);
    c->running = 0;
    pthread_cond_broadcast(&c->cv);
    pthread_mutex_unlock(&c->mtx);
    pthread_join(c->th, NULL);
    if(c->active) {
        for(size_t i=0;i<c->active_len;i++) free(c->active[i]);
        free(c->active);
    }
    pthread_mutex_destroy(&c->mtx);
    pthread_cond_destroy(&c->cv);
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


