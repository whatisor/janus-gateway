#ifndef ABMOD_PROVIDER_IMPL_H
#define ABMOD_PROVIDER_IMPL_H

#include <stddef.h>
#include <stdint.h>

#include "abmod_provider.h"

typedef struct abmod_provider_vtbl {
	void (*destroy)(void *impl);
	int (*open_user_stream)(void *impl,
			const char *room_id,
			const char *user_id,
			uint32_t sample_rate,
			int channels);
	int (*send_pcm)(void *impl,
			const char *room_id,
			const char *user_id,
			const int16_t *pcm,
			size_t samples,
			uint32_t sample_rate,
			int channels);
	int (*close_user_stream)(void *impl,
			const char *room_id,
			const char *user_id);
} abmod_provider_vtbl;

/* Provider implementations expose init() returning impl + vtable */
int abmod_provider_aws_init(const char *config_json,
		const abmod_provider_callbacks *cbs,
		void *cb_user,
		void **out_impl,
		const abmod_provider_vtbl **out_vtbl);

int abmod_provider_openai_init(const char *config_json,
		const abmod_provider_callbacks *cbs,
		void *cb_user,
		void **out_impl,
		const abmod_provider_vtbl **out_vtbl);

#endif /* ABMOD_PROVIDER_IMPL_H */

