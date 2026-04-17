/* Stub when Janus is built without --enable-abmod-aws-sdk (no AWS C++ SDK linked). */

#include "abmod_provider_aws_sdk.h"

void abmod_aws_native_global_init(void) {}

void abmod_aws_native_global_shutdown(void) {}

void *abmod_aws_native_stream_open(const AbmodAwsNativeConfig *cfg,
		const char *room_id,
		const char *user_id,
		abmod_sdk_transcript_fn on_text,
		abmod_sdk_error_fn on_err,
		void *user) {
	(void)cfg;
	(void)room_id;
	(void)user_id;
	(void)on_text;
	(void)on_err;
	(void)user;
	return NULL;
}

int abmod_aws_native_stream_send_pcm(void *stream,
		const int16_t *pcm,
		size_t samples,
		int channels) {
	(void)stream;
	(void)pcm;
	(void)samples;
	(void)channels;
	return -1;
}

void abmod_aws_native_stream_close(void *stream) {
	(void)stream;
}
