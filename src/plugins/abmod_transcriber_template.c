/* Minimal template module for AudioBridge custom module ABI */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>
#include "janus_ab_module.h"

typedef struct template_ctx_s {
    uint32_t rate;
    int channels;
    char *config;
    janus_abmod_callbacks cbs;
    void *user;
    uint64_t frames;
} template_ctx;

static void emit(template_ctx *ctx, const char *name, const char *payload) {
    if(ctx->cbs.emit_event)
        ctx->cbs.emit_event(ctx->cbs.emit_event_user, name, payload);
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
    emit(ctx, "abmod.started", "{\"module\":\"template\"}");
    return ctx;
}

void abmod_destroy(void *vctx) {
    template_ctx *ctx = (template_ctx*)vctx;
    if(!ctx) return;
    emit(ctx, "abmod.stopped", "{}");
    free(ctx->config);
    free(ctx);
}

void abmod_on_mix(void *vctx, const int16_t *pcm, size_t samples,
        uint32_t sampling_rate, int channels) {
    template_ctx *ctx = (template_ctx*)vctx;
    if(!ctx || !pcm) return;
    (void)sampling_rate; (void)channels;
    ctx->frames++;
    if((ctx->frames % 250) == 0) {
        emit(ctx, "abmod.heartbeat", "{}");
    }
}

void abmod_on_event(void *vctx, const char *event_name,
        const char *room_id, const char *user_id) {
    template_ctx *ctx = (template_ctx*)vctx;
    if(!ctx) return;
    (void)room_id; (void)user_id;
    if(ctx->cbs.emit_event) {
        ctx->cbs.emit_event(ctx->cbs.emit_event_user, event_name, "{}");
    }
}


