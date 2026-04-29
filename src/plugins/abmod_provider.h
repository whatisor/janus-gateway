#ifndef ABMOD_PROVIDER_H
#define ABMOD_PROVIDER_H

#include <stddef.h>
#include <stdint.h>

typedef struct abmod_provider abmod_provider;

typedef void (*abmod_provider_transcript_cb)(void *user,
		const char *provider_name,
		const char *room_id,
		const char *user_id,
		const char *text,
		float transcript_confidence,
		int is_final,
		const char *item_id);

typedef void (*abmod_provider_error_cb)(void *user,
		const char *provider_name,
		const char *room_id,
		const char *user_id,
		const char *error_message);

typedef struct abmod_provider_callbacks {
	abmod_provider_transcript_cb on_transcript;
	abmod_provider_error_cb on_error;
} abmod_provider_callbacks;

abmod_provider *abmod_provider_create(const char *provider_name,
		const char *config_json,
		const abmod_provider_callbacks *cbs,
		void *user);

void abmod_provider_destroy(abmod_provider *provider);

int abmod_provider_open_user_stream(abmod_provider *provider,
		const char *room_id,
		const char *user_id,
		uint32_t sample_rate,
		int channels);

int abmod_provider_send_pcm(abmod_provider *provider,
		const char *room_id,
		const char *user_id,
		const int16_t *pcm,
		size_t samples,
		uint32_t sample_rate,
		int channels);

int abmod_provider_close_user_stream(abmod_provider *provider,
		const char *room_id,
		const char *user_id);

#endif
