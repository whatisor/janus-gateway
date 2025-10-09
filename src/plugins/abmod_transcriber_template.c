/* Minimal template module for AudioBridge custom module ABI */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>
#include <stdbool.h>
#include <inttypes.h>
#include "janus_ab_module.h"
#include "abmod_consumer.h"

typedef struct template_ctx_s {
    uint32_t rate;
    int channels;
    char *config;
    janus_abmod_callbacks cbs;
    void *user;
    uint64_t frames;
    /* Background consumer (logging) */
    abmod_consumer *consumer;
} template_ctx;

/* Adapter to forward consumer emits to Janus */
static void consumer_emit_adapter(void *user, const char *event_name, const char *json_payload) {
    template_ctx *ctx = (template_ctx*)user;
    if(ctx && ctx->cbs.emit_event)
        ctx->cbs.emit_event(ctx->cbs.emit_event_user, event_name, json_payload);
}

void* abmod_create(uint32_t sampling_rate, int channels,
        const char *config_json, const janus_abmod_callbacks *cbs, void *user) {
    template_ctx *ctx = (template_ctx*)calloc(1, sizeof(template_ctx));
    if(!ctx) return NULL;
    ctx->rate = sampling_rate;
    ctx->channels = channels;
    ctx->config = config_json ? strdup(config_json) : NULL;
    if(cbs) ctx->cbs = *cbs; else memset(&ctx->cbs, 0, sizeof(ctx->cbs));
    ctx->user = user;
    ctx->frames = 0;
    /* Create background consumer acting as sample STT worker */
    ctx->consumer = abmod_consumer_create(sampling_rate, channels);
    /* Forward consumer-generated STT to Janus */
    abmod_consumer_set_emitter(ctx->consumer, consumer_emit_adapter, ctx);
    return ctx;
}

void abmod_destroy(void *vctx) {
    template_ctx *ctx = (template_ctx*)vctx;
    if(!ctx) return;
    /* Stop consumer */
    abmod_consumer_destroy(ctx->consumer);
    free(ctx->config);
    free(ctx);
}

void abmod_on_mix(void *vctx, const int16_t *pcm, size_t samples,
        uint32_t sampling_rate, int channels,
        uint32_t rtp_timestamp, uint64_t frame_seq, uint64_t active_talk_version) {
    template_ctx *ctx = (template_ctx*)vctx;
    if(!ctx || !pcm) return;
    (void)sampling_rate;
    ctx->frames++;
    /* Hand off to background consumer (computes energies and logs) */
    abmod_consumer_enqueue_mix_pcm(ctx->consumer, pcm, samples, rtp_timestamp, frame_seq, active_talk_version, channels);
    /* No external emits here; background worker should publish when ready */
}

void abmod_on_event(void *vctx, const char *event_name,
        const char *room_id, const char *user_id,
        int64_t event_time_us, uint64_t talk_version) {
    template_ctx *ctx = (template_ctx*)vctx;
    if(!ctx) return;
    (void)room_id; (void)event_time_us;
    /* Internal queue for module logic only; do not emit externally here */
    abmod_consumer_enqueue_event(ctx->consumer, event_name, user_id);
}


