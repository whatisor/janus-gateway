#include <stdint.h>
#include <stddef.h>

#include "janus_ab_module.h"

void *abmod_create(uint32_t sampling_rate, int channels, const char *config_json,
		const janus_abmod_callbacks *cbs, void *user);
void abmod_destroy(void *vctx);
void abmod_on_mix(void *vctx, const int16_t *pcm, size_t samples,
		uint32_t sampling_rate, int channels, uint32_t rtp_timestamp,
		uint64_t frame_seq, uint64_t active_talk_version);
void abmod_on_event(void *vctx, const char *event_name,
		const char *room_id, const char *user_id, int64_t event_time_us,
		uint64_t talk_version);
void abmod_on_participant_pcm(void *vctx, const char *room_id, const char *user_id,
		const int16_t *pcm, size_t samples, uint32_t sampling_rate, int channels,
		uint32_t rtp_timestamp, uint64_t frame_seq, uint64_t active_talk_version);

void *abmod_create(uint32_t sampling_rate, int channels, const char *config_json,
		const janus_abmod_callbacks *cbs, void *user) {
	(void)sampling_rate;
	(void)channels;
	(void)config_json;
	(void)cbs;
	(void)user;
	return NULL;
}

void abmod_destroy(void *vctx) {
	(void)vctx;
}

void abmod_on_mix(void *vctx, const int16_t *pcm, size_t samples,
		uint32_t sampling_rate, int channels, uint32_t rtp_timestamp,
		uint64_t frame_seq, uint64_t active_talk_version) {
	(void)vctx;
	(void)pcm;
	(void)samples;
	(void)sampling_rate;
	(void)channels;
	(void)rtp_timestamp;
	(void)frame_seq;
	(void)active_talk_version;
}

void abmod_on_event(void *vctx, const char *event_name,
		const char *room_id, const char *user_id, int64_t event_time_us,
		uint64_t talk_version) {
	(void)vctx;
	(void)event_name;
	(void)room_id;
	(void)user_id;
	(void)event_time_us;
	(void)talk_version;
}

void abmod_on_participant_pcm(void *vctx, const char *room_id, const char *user_id,
		const int16_t *pcm, size_t samples, uint32_t sampling_rate, int channels,
		uint32_t rtp_timestamp, uint64_t frame_seq, uint64_t active_talk_version) {
	(void)vctx;
	(void)room_id;
	(void)user_id;
	(void)pcm;
	(void)samples;
	(void)sampling_rate;
	(void)channels;
	(void)rtp_timestamp;
	(void)frame_seq;
	(void)active_talk_version;
}
