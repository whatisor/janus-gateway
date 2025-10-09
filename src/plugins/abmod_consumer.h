/* Simple consumer module for the transcription template: logs talk events
 * and per-frame left/right energy without blocking the mixer thread. */

#ifndef ABMOD_CONSUMER_H
#define ABMOD_CONSUMER_H

#include <stddef.h>
#include <stdint.h>

typedef struct abmod_consumer abmod_consumer;

/* Emitter used by the consumer thread to publish transcription outputs */
typedef void (*abmod_consumer_emit_cb)(void *user,
        const char *event_name,
        const char *json_payload);

abmod_consumer *abmod_consumer_create(uint32_t sampling_rate, int channels);
void abmod_consumer_destroy(abmod_consumer *c);

/* Configure emitter; safe to call any time */
void abmod_consumer_set_emitter(abmod_consumer *c,
        abmod_consumer_emit_cb cb,
        void *user);

/* Non-blocking enqueue; drops if queue is full */
void abmod_consumer_enqueue_event(abmod_consumer *c, const char *event_name, const char *user_id);
void abmod_consumer_enqueue_mix_pcm(abmod_consumer *c,
        const int16_t *pcm, size_t samples,
        uint32_t rtp_timestamp, uint64_t frame_seq,
        uint64_t active_talk_version, int channels);

/* Typed publish helpers for transcription outputs */
void abmod_consumer_publish_partial(abmod_consumer *c, const char *json_payload);
void abmod_consumer_publish_final(abmod_consumer *c, const char *json_payload);
void abmod_consumer_publish_error(abmod_consumer *c, const char *json_payload);

#endif /* ABMOD_CONSUMER_H */


