#include "abmod_gender.h"

#include <stddef.h>

struct abmod_gender_engine {};

abmod_gender_engine *abmod_gender_create(const char *config_json) {
	(void)config_json;
	return NULL;
}

void abmod_gender_destroy(abmod_gender_engine *engine) {
	(void)engine;
}

void abmod_gender_on_pcm(abmod_gender_engine *engine,
		const char *room_id,
		const char *user_id,
		const int16_t *pcm,
		size_t samples,
		uint32_t sampling_rate,
		int channels) {
	(void)engine;
	(void)room_id;
	(void)user_id;
	(void)pcm;
	(void)samples;
	(void)sampling_rate;
	(void)channels;
}

int abmod_gender_trigger_on_transcript(abmod_gender_engine *engine,
		const char *room_id,
		const char *user_id,
		const char *text,
		float transcript_confidence,
		int is_final) {
	(void)engine;
	(void)room_id;
	(void)user_id;
	(void)text;
	(void)transcript_confidence;
	(void)is_final;
	return 0;
}

void abmod_gender_clear_user(abmod_gender_engine *engine,
		const char *room_id,
		const char *user_id) {
	(void)engine;
	(void)room_id;
	(void)user_id;
}

int abmod_gender_get_result(abmod_gender_engine *engine,
		const char *room_id,
		const char *user_id,
		char *out_label,
		size_t out_label_len,
		float *out_confidence,
		const char **out_status) {
	(void)engine;
	(void)room_id;
	(void)user_id;
	(void)out_label;
	(void)out_label_len;
	(void)out_confidence;
	if(out_status)
		*out_status = "disabled";
	return 0;
}
