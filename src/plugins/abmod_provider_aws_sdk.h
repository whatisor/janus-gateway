/* C API for AWS Transcribe Medical streaming via official AWS SDK for C++.
 * Implemented in abmod_provider_aws_sdk.cpp when built with --enable-abmod-aws-sdk.
 */

#ifndef ABMOD_PROVIDER_AWS_SDK_H
#define ABMOD_PROVIDER_AWS_SDK_H

#include <stddef.h>
#include <stdint.h>

typedef struct AbmodAwsNativeConfig {
	const char *region;
	const char *language_code;
	const char *specialty;
	const char *stream_type; /* CONVERSATION or DICTATION */
	const char *session_id;
	uint32_t sample_rate;
	int medical_redaction;
	int fast_mode; /* 1=emit partial+final, 0=emit final only */
	const char *access_key_id;
	const char *secret_access_key;
	const char *session_token;
} AbmodAwsNativeConfig;

typedef void (*abmod_sdk_transcript_fn)(void *user,
		const char *room_id,
		const char *user_id,
		const char *text,
		const char *item_id,
		int is_final);

typedef void (*abmod_sdk_error_fn)(void *user,
		const char *room_id,
		const char *user_id,
		const char *msg);

#ifdef __cplusplus
extern "C" {
#endif

void abmod_aws_native_global_init(void);
void abmod_aws_native_global_shutdown(void);

void *abmod_aws_native_stream_open(const AbmodAwsNativeConfig *cfg,
		const char *room_id,
		const char *user_id,
		abmod_sdk_transcript_fn on_text,
		abmod_sdk_error_fn on_err,
		void *user);

int abmod_aws_native_stream_send_pcm(void *stream,
		const int16_t *pcm,
		size_t samples,
		int channels);

void abmod_aws_native_stream_close(void *stream);

#ifdef __cplusplus
} /* extern "C" */
#endif

#endif
