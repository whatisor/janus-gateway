#ifndef ABMOD_GENDER_H
#define ABMOD_GENDER_H

#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef struct abmod_gender_engine abmod_gender_engine;

abmod_gender_engine *abmod_gender_create(const char *config_json);
void abmod_gender_destroy(abmod_gender_engine *engine);

/* Feed participant PCM; engine performs at most one inference per user. */
void abmod_gender_on_pcm(abmod_gender_engine *engine,
		const char *room_id,
		const char *user_id,
		const int16_t *pcm,
		size_t samples,
		uint32_t sampling_rate,
		int channels);

/* Transcript-aware trigger; gating policy is owned by gender engine config. */
int abmod_gender_trigger_on_transcript(abmod_gender_engine *engine,
		const char *room_id,
		const char *user_id,
		const char *text,
		float transcript_confidence,
		int is_final);

/* Clear cached state for a participant (e.g., on leave). */
void abmod_gender_clear_user(abmod_gender_engine *engine,
		const char *room_id,
		const char *user_id);

/* Retrieve cached result:
 *  - returns 1 when a label is available and copied to out_label/out_confidence
 *  - returns 0 otherwise; out_status gets "pending"/"unavailable"/"disabled" */
int abmod_gender_get_result(abmod_gender_engine *engine,
		const char *room_id,
		const char *user_id,
		char *out_label,
		size_t out_label_len,
		float *out_confidence,
		const char **out_status);

#ifdef __cplusplus
}
#endif

#endif /* ABMOD_GENDER_H */
