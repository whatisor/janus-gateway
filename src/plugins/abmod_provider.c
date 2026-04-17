#include <stdlib.h>
#include <string.h>

#include "abmod_provider.h"
#include "abmod_provider_impl.h"

struct abmod_provider {
	char *provider_name;
	void *impl;
	const abmod_provider_vtbl *vtbl;
};

/* ------------------------------------------------------------------ *
 * Provider registry — add a forward declaration and a table entry to
 * register a new provider; no other files need to change.
 * ------------------------------------------------------------------ */
typedef int (*abmod_provider_init_fn)(const char *config_json,
		const abmod_provider_callbacks *cbs,
		void *cb_user,
		void **out_impl,
		const abmod_provider_vtbl **out_vtbl);

int abmod_provider_aws_init(const char *, const abmod_provider_callbacks *, void *, void **, const abmod_provider_vtbl **);
int abmod_provider_openai_init(const char *, const abmod_provider_callbacks *, void *, void **, const abmod_provider_vtbl **);

static const struct { const char *name; abmod_provider_init_fn init; } PROVIDERS[] = {
	{ "aws",    abmod_provider_aws_init    },
	{ "openai", abmod_provider_openai_init },
};
static const size_t PROVIDERS_COUNT = sizeof(PROVIDERS) / sizeof(PROVIDERS[0]);

abmod_provider *abmod_provider_create(const char *provider_name,
		const char *config_json,
		const abmod_provider_callbacks *cbs,
		void *user) {
	const char *pname = (provider_name && *provider_name) ? provider_name : "aws";
	void *impl = NULL;
	const abmod_provider_vtbl *vtbl = NULL;
	int ok = 0;

	for(size_t i = 0; i < PROVIDERS_COUNT; i++) {
		if(strcmp(pname, PROVIDERS[i].name) == 0) {
			ok = (PROVIDERS[i].init(config_json, cbs, user, &impl, &vtbl) == 0);
			break;
		}
	}
	if(!ok || !impl || !vtbl)
		return NULL;

	abmod_provider *p = (abmod_provider *)calloc(1, sizeof(*p));
	if(!p) {
		vtbl->destroy(impl);
		return NULL;
	}
	p->provider_name = strdup(pname);
	p->impl = impl;
	p->vtbl = vtbl;
	return p;
}

void abmod_provider_destroy(abmod_provider *provider) {
	if(!provider)
		return;
	if(provider->vtbl && provider->vtbl->destroy)
		provider->vtbl->destroy(provider->impl);
	free(provider->provider_name);
	free(provider);
}

int abmod_provider_open_user_stream(abmod_provider *provider,
		const char *room_id,
		const char *user_id,
		uint32_t sample_rate,
		int channels) {
	if(!provider || !provider->vtbl || !provider->vtbl->open_user_stream)
		return -1;
	return provider->vtbl->open_user_stream(provider->impl, room_id, user_id, sample_rate, channels);
}

int abmod_provider_send_pcm(abmod_provider *provider,
		const char *room_id,
		const char *user_id,
		const int16_t *pcm,
		size_t samples,
		uint32_t sample_rate,
		int channels) {
	if(!provider || !provider->vtbl || !provider->vtbl->send_pcm)
		return -1;
	return provider->vtbl->send_pcm(provider->impl, room_id, user_id, pcm, samples, sample_rate, channels);
}

int abmod_provider_close_user_stream(abmod_provider *provider,
		const char *room_id,
		const char *user_id) {
	if(!provider || !provider->vtbl || !provider->vtbl->close_user_stream)
		return -1;
	return provider->vtbl->close_user_stream(provider->impl, room_id, user_id);
}

