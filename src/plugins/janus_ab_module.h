/*
 * Simple AudioBridge custom module ABI
 * The module is a runtime-loaded shared library exposing the symbols below.
 *
 * Lifecycle:
 *  - abmod_create(...) -> returns an opaque context pointer per room
 *  - abmod_destroy(ctx)
 *  - abmod_on_mix(ctx, pcm, samples, rate, channels) is called for mixed PCM16 frames
 *  - abmod_on_event(ctx, name, room_id, user_id) is called for talk events
 */

#ifndef JANUS_AB_MODULE_H
#define JANUS_AB_MODULE_H

#include <stddef.h>
#include <stdint.h>

/* Callback to allow the module to emit events back to Janus */
typedef void (*janus_abmod_emit_event_cb)(void *user,
        const char *event_name,
        const char *json_payload);

typedef struct janus_abmod_callbacks {
    janus_abmod_emit_event_cb emit_event; /* optional, may be NULL */
    void *emit_event_user;                /* provided back on emit_event */
} janus_abmod_callbacks;

/* Create a module instance for a room; return context pointer or NULL on error */
typedef void* (*janus_abmod_create_f)(uint32_t sampling_rate,
        int channels,
        const char *config_json, /* may be NULL */
        const janus_abmod_callbacks *cbs, /* may be NULL */
        void *user /* provided back to callbacks */);

/* Destroy a previously created instance */
typedef void (*janus_abmod_destroy_f)(void *ctx);

/* Called on every mixed frame; pcm has 'samples' int16 samples (per channel) */
typedef void (*janus_abmod_on_mix_f)(void *ctx,
        const int16_t *pcm,
        size_t samples,
        uint32_t sampling_rate,
        int channels,
        uint32_t rtp_timestamp,
        uint64_t frame_seq,
        uint64_t active_talk_version);

/* Called on talk state change events */
typedef void (*janus_abmod_on_event_f)(void *ctx,
        const char *event_name, /* e.g., "talking" or "stopped-talking" */
        const char *room_id,
        const char *user_id,
        int64_t event_time_us,
        uint64_t talk_version);

/* Symbol names that modules must export */
#define JANUS_ABMOD_CREATE_SYMBOL "abmod_create"
#define JANUS_ABMOD_DESTROY_SYMBOL "abmod_destroy"
#define JANUS_ABMOD_ON_MIX_SYMBOL "abmod_on_mix"
#define JANUS_ABMOD_ON_EVENT_SYMBOL "abmod_on_event"

#endif /* JANUS_AB_MODULE_H */


