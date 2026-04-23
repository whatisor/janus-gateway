/*
 * AWS provider implementation for abmod_provider.
 *
 * This file provides the AWS implementation behind a vtable so the public
 * abmod_provider API can dispatch across multiple providers.
 *
 * Streaming uses official AWS SDK for C++ (abmod_provider_aws_sdk.cpp) when built
 * with --enable-abmod-aws-sdk; otherwise abmod_provider_aws_stub.c.
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>
#include <pthread.h>
#include <glib.h>
#include <jansson.h>

#include "abmod_provider_impl.h"
#include "abmod_provider_aws_sdk.h"

#define ABMOD_AWS_PROVIDER_NAME "aws"

#define ABMOD_LOG(fmt, ...) \
	fprintf(stderr, "[ABMod][aws] " fmt "\n", ##__VA_ARGS__)

typedef struct abmod_provider_aws abmod_provider_aws;

typedef struct abmod_aws_stream {
	char *key;
	char *room_id;
	char *user_id;
	uint32_t sample_rate;
	void *native;
	abmod_provider_aws *provider;
} abmod_aws_stream;

/* Entry used by the vocab background thread to reconnect a stream. */
typedef struct {
	char *room_id;
	char *user_id;
	uint32_t sample_rate;
	void *old_native;
} VocabReconnectEntry;

struct abmod_provider_aws {
	char *language_code;
	char *region;
	char *specialty;
	char *stream_type;
	char *session_id;
	char *aws_access_key_id;
	char *aws_secret_access_key;
	char *aws_session_token;
	char *vocabulary_name;
	char *vocabulary_prompt;
	char *created_vocabulary_name; /* non-NULL when we own a temp vocab to delete on destroy */
	pthread_t vocab_thread;
	int vocab_thread_running;
	volatile int vocab_cancel;
	int medical_enabled;
	int medical_redaction;
	int fast_mode;
	GHashTable *streams;
	pthread_mutex_t streams_mtx;
	abmod_provider_callbacks cbs;
	void *cb_user;
};

static char *abmod_strdup_or_default(const char *value, const char *default_value) {
	return g_strdup(value && *value ? value : default_value);
}

static char *abmod_build_stream_key(const char *room_id, const char *user_id) {
	return g_strdup_printf("%s|%s", room_id ? room_id : "", user_id ? user_id : "");
}


static void abmod_provider_emit_error(abmod_provider_aws *provider, const char *room_id, const char *user_id, const char *error_message) {
	if(provider && provider->cbs.on_error)
		provider->cbs.on_error(provider->cb_user, ABMOD_AWS_PROVIDER_NAME, room_id, user_id, error_message);
}

static void sdk_on_text(void *user,
		const char *room_id,
		const char *user_id,
		const char *text,
		const char *item_id,
		int is_final) {
	ABMOD_LOG("sdk_on_text room=%s user=%s %s item_id=%s: %s",
		room_id ? room_id : "?", user_id ? user_id : "?",
		is_final ? "FINAL" : "partial", item_id ? item_id : "?", text ? text : "(empty)");
	abmod_provider_aws *p = (abmod_provider_aws *)user;
	if(p && p->cbs.on_transcript)
		p->cbs.on_transcript(p->cb_user, ABMOD_AWS_PROVIDER_NAME, room_id, user_id, text, is_final, item_id);
}

static void sdk_on_err(void *user,
		const char *room_id,
		const char *user_id,
		const char *msg) {
	ABMOD_LOG("sdk_on_err room=%s user=%s: %s",
		room_id ? room_id : "?", user_id ? user_id : "?", msg ? msg : "(null)");
	abmod_provider_aws *p = (abmod_provider_aws *)user;
	if(p && p->cbs.on_error)
		p->cbs.on_error(p->cb_user, ABMOD_AWS_PROVIDER_NAME, room_id, user_id, msg);
}

/* Opens a native stream for a given room/user with the current provider config.
 * Caller must hold streams_mtx. */
static void *abmod_open_native_stream(abmod_provider_aws *provider,
		const char *room_id,
		const char *user_id,
		uint32_t sample_rate) {
	AbmodAwsNativeConfig cfg = {0};
	cfg.region = provider->region;
	cfg.language_code = provider->language_code;
	cfg.specialty = provider->specialty;
	cfg.stream_type = provider->stream_type;
	cfg.session_id = (provider->session_id && *provider->session_id) ? provider->session_id : NULL;
	cfg.sample_rate = sample_rate;
	cfg.medical_redaction = provider->medical_redaction;
	cfg.fast_mode = provider->fast_mode;
	cfg.vocabulary_name = provider->vocabulary_name;
	cfg.access_key_id = provider->aws_access_key_id;
	cfg.secret_access_key = provider->aws_secret_access_key;
	cfg.session_token = provider->aws_session_token;
	void *native = abmod_aws_native_stream_open(&cfg, room_id, user_id, sdk_on_text, sdk_on_err, provider);
	return native;
}

/* Background thread: waits for vocabulary to become READY then reconnects
 * all active streams so they benefit from the vocabulary. */
static void *vocab_creation_thread(void *arg) {
	abmod_provider_aws *provider = (abmod_provider_aws *)arg;

	char vname[256] = {0};
	AbmodAwsNativeConfig tmp = {0};
	tmp.region = provider->region;
	tmp.access_key_id = provider->aws_access_key_id;
	tmp.secret_access_key = provider->aws_secret_access_key;
	tmp.session_token = provider->aws_session_token;

	if(abmod_aws_native_create_vocabulary_from_prompt(&tmp, provider->vocabulary_prompt,
			vname, sizeof(vname), 180, &provider->vocab_cancel) != 0) {
		ABMOD_LOG("vocab thread: creation failed or cancelled");
		return NULL;
	}
	ABMOD_LOG("vocab thread: '%s' ready, reconnecting active streams", vname);

	/* Build reconnect list and swap native handles while holding the lock.
	 * abmod_aws_native_stream_open only spawns a thread — returns fast. */
	GArray *old_natives = g_array_new(FALSE, TRUE, sizeof(void *));

	pthread_mutex_lock(&provider->streams_mtx);
	provider->created_vocabulary_name = g_strdup(vname);
	g_free(provider->vocabulary_name);
	provider->vocabulary_name = g_strdup(vname);

	GHashTableIter iter;
	gpointer key = NULL, value = NULL;
	g_hash_table_iter_init(&iter, provider->streams);
	while(g_hash_table_iter_next(&iter, &key, &value)) {
		abmod_aws_stream *st = (abmod_aws_stream *)value;
		void *old = st->native;
		st->native = abmod_open_native_stream(provider, st->room_id, st->user_id, st->sample_rate);
		g_array_append_val(old_natives, old);
	}
	pthread_mutex_unlock(&provider->streams_mtx);

	/* Close old handles outside the lock */
	for(guint i = 0; i < old_natives->len; i++) {
		void *old = g_array_index(old_natives, void *, i);
		if(old)
			abmod_aws_native_stream_close(old);
	}
	g_array_free(old_natives, TRUE);
	ABMOD_LOG("vocab thread: reconnect done");
	return NULL;
}

static void abmod_stream_destroy(abmod_aws_stream *stream) {
	if(!stream)
		return;
	if(stream->native)
		abmod_aws_native_stream_close(stream->native);
	g_free(stream->key);
	g_free(stream->room_id);
	g_free(stream->user_id);
	g_free(stream);
}

static void abmod_provider_aws_destroy(void *vimpl) {
	abmod_provider_aws *provider = (abmod_provider_aws *)vimpl;
	if(!provider)
		return;
	/* Cancel the vocabulary background thread and wait for it to exit before
	 * we free any fields it might still be reading (region, credentials, etc.) */
	if(provider->vocab_thread_running) {
		provider->vocab_cancel = 1;
		pthread_join(provider->vocab_thread, NULL);
		provider->vocab_thread_running = 0;
	}
	pthread_mutex_lock(&provider->streams_mtx);
	GHashTableIter iter;
	gpointer key = NULL, value = NULL;
	g_hash_table_iter_init(&iter, provider->streams);
	while(g_hash_table_iter_next(&iter, &key, &value)) {
		abmod_aws_stream *stream = (abmod_aws_stream *)value;
		g_hash_table_iter_remove(&iter);
		abmod_stream_destroy(stream);
	}
	pthread_mutex_unlock(&provider->streams_mtx);
	g_hash_table_destroy(provider->streams);
	pthread_mutex_destroy(&provider->streams_mtx);
	if(provider->created_vocabulary_name) {
		AbmodAwsNativeConfig tmp = {0};
		tmp.region = provider->region;
		tmp.access_key_id = provider->aws_access_key_id;
		tmp.secret_access_key = provider->aws_secret_access_key;
		tmp.session_token = provider->aws_session_token;
		abmod_aws_native_delete_vocabulary(&tmp, provider->created_vocabulary_name);
		g_free(provider->created_vocabulary_name);
	}
	g_free(provider->language_code);
	g_free(provider->region);
	g_free(provider->specialty);
	g_free(provider->stream_type);
	g_free(provider->session_id);
	g_free(provider->aws_access_key_id);
	g_free(provider->aws_secret_access_key);
	g_free(provider->aws_session_token);
	g_free(provider->vocabulary_name);
	g_free(provider->vocabulary_prompt);
	g_free(provider);
	abmod_aws_native_global_shutdown();
}

static int abmod_provider_aws_open_user_stream(void *vimpl,
		const char *room_id,
		const char *user_id,
		uint32_t sample_rate,
		int channels) {
	(void)channels;
	abmod_provider_aws *provider = (abmod_provider_aws *)vimpl;
	if(!provider || !room_id || !user_id)
		return -1;
	char *key = abmod_build_stream_key(room_id, user_id);
	pthread_mutex_lock(&provider->streams_mtx);
	if(g_hash_table_lookup(provider->streams, key)) {
		pthread_mutex_unlock(&provider->streams_mtx);
		g_free(key);
		return 0;
	}
	ABMOD_LOG("opening native stream room=%s user=%s region=%s lang=%s vocab=%s",
		room_id, user_id,
		provider->region ? provider->region : "?",
		provider->language_code ? provider->language_code : "?",
		provider->vocabulary_name ? provider->vocabulary_name : "(none — vocab thread may still be running)");
	void *native = abmod_open_native_stream(provider, room_id, user_id, sample_rate);
	if(!native) {
		ABMOD_LOG("native stream open FAILED room=%s user=%s", room_id, user_id);
		pthread_mutex_unlock(&provider->streams_mtx);
		g_free(key);
		abmod_provider_emit_error(provider, room_id, user_id,
			"AWS native stream failed (build with --enable-abmod-aws-sdk and link AWS SDK, or set AWS credentials)");
		return -1;
	}
	ABMOD_LOG("native stream opened OK room=%s user=%s", room_id, user_id);
	abmod_aws_stream *stream = (abmod_aws_stream *)g_malloc0(sizeof(*stream));
	stream->key = g_strdup(key);
	stream->room_id = g_strdup(room_id);
	stream->user_id = g_strdup(user_id);
	stream->sample_rate = sample_rate;
	stream->native = native;
	stream->provider = provider;
	g_hash_table_insert(provider->streams, g_strdup(key), stream);
	pthread_mutex_unlock(&provider->streams_mtx);
	g_free(key);
	return 0;
}

static int abmod_provider_aws_send_pcm(void *vimpl,
		const char *room_id,
		const char *user_id,
		const int16_t *pcm,
		size_t samples,
		uint32_t sample_rate,
		int channels) {
	(void)sample_rate;
	abmod_provider_aws *provider = (abmod_provider_aws *)vimpl;
	if(!provider || !room_id || !user_id || !pcm || samples == 0)
		return -1;
	char *key = abmod_build_stream_key(room_id, user_id);
	pthread_mutex_lock(&provider->streams_mtx);
	abmod_aws_stream *stream = (abmod_aws_stream *)g_hash_table_lookup(provider->streams, key);
	int rc = -1;
	if(stream && stream->native)
		rc = abmod_aws_native_stream_send_pcm(stream->native, pcm, samples, channels);
	pthread_mutex_unlock(&provider->streams_mtx);
	g_free(key);
	return rc;
}

static int abmod_provider_aws_close_user_stream(void *vimpl,
		const char *room_id,
		const char *user_id) {
	abmod_provider_aws *provider = (abmod_provider_aws *)vimpl;
	if(!provider || !room_id || !user_id)
		return -1;
	char *key = abmod_build_stream_key(room_id, user_id);
	pthread_mutex_lock(&provider->streams_mtx);
	abmod_aws_stream *stream = (abmod_aws_stream *)g_hash_table_lookup(provider->streams, key);
	if(stream)
		g_hash_table_remove(provider->streams, key);
	pthread_mutex_unlock(&provider->streams_mtx);
	g_free(key);
	if(stream) {
		ABMOD_LOG("closed stream room=%s user=%s", room_id, user_id);
		abmod_stream_destroy(stream);
	}
	return 0;
}

static const abmod_provider_vtbl ABMOD_AWS_VTBL = {
	.destroy = abmod_provider_aws_destroy,
	.open_user_stream = abmod_provider_aws_open_user_stream,
	.send_pcm = abmod_provider_aws_send_pcm,
	.close_user_stream = abmod_provider_aws_close_user_stream
};

int abmod_provider_aws_init(const char *config_json,
		const abmod_provider_callbacks *cbs,
		void *cb_user,
		void **out_impl,
		const abmod_provider_vtbl **out_vtbl) {
	if(!out_impl || !out_vtbl)
		return -1;
	*out_impl = NULL;
	*out_vtbl = NULL;

	abmod_provider_aws *provider = (abmod_provider_aws *)g_malloc0(sizeof(*provider));
	if(!provider)
		return -1;
	provider->streams = g_hash_table_new_full(g_str_hash, g_str_equal, g_free, NULL);
	pthread_mutex_init(&provider->streams_mtx, NULL);
	if(cbs)
		provider->cbs = *cbs;
	provider->cb_user = cb_user;

	provider->language_code = g_strdup("en-US");
	provider->region = g_strdup("us-east-1");
	provider->specialty = g_strdup("PRIMARYCARE");
	provider->stream_type = g_strdup("CONVERSATION");
	provider->session_id = g_strdup("");
	provider->aws_access_key_id = g_strdup(getenv("AWS_ACCESS_KEY_ID"));
	provider->aws_secret_access_key = g_strdup(getenv("AWS_SECRET_ACCESS_KEY"));
	provider->aws_session_token = g_strdup(getenv("AWS_SESSION_TOKEN"));
	provider->medical_enabled = 1;
	provider->medical_redaction = 0;
	provider->fast_mode = 1;

	if(config_json) {
		json_error_t err;
		json_t *cfg = json_loads(config_json, 0, &err);
		if(cfg && json_is_object(cfg)) {
			const char *lang = json_string_value(json_object_get(cfg, "aws_language_code"));
			const char *region = json_string_value(json_object_get(cfg, "aws_region"));
			const char *specialty = json_string_value(json_object_get(cfg, "aws_specialty"));
			const char *stream_type = json_string_value(json_object_get(cfg, "aws_stream_type"));
			const char *session_id = json_string_value(json_object_get(cfg, "aws_session_id"));
			const char *vocabulary_name = json_string_value(json_object_get(cfg, "aws_vocabulary_name"));
			const char *access_key_id = json_string_value(json_object_get(cfg, "aws_access_key_id"));
			const char *secret_access_key = json_string_value(json_object_get(cfg, "aws_secret_access_key"));
			const char *session_token = json_string_value(json_object_get(cfg, "aws_session_token"));
			json_t *medical_enabled_obj = json_object_get(cfg, "aws_medical_enabled");
			if(!medical_enabled_obj)
				medical_enabled_obj = json_object_get(cfg, "medical");
			json_t *fast_mode_obj = json_object_get(cfg, "aws_fast_mode");
			if(!fast_mode_obj)
				fast_mode_obj = json_object_get(cfg, "fast_mode");
			int redaction = json_boolean_value(json_object_get(cfg, "aws_medical_redaction"));
			g_free(provider->language_code);
			provider->language_code = abmod_strdup_or_default(lang, "en-US");
			g_free(provider->region);
			provider->region = abmod_strdup_or_default(region, "us-east-1");
			g_free(provider->specialty);
			provider->specialty = abmod_strdup_or_default(specialty, "PRIMARYCARE");
			g_free(provider->stream_type);
			provider->stream_type = abmod_strdup_or_default(stream_type, "CONVERSATION");
			g_free(provider->session_id);
			provider->session_id = abmod_strdup_or_default(session_id, "");
			if(access_key_id && *access_key_id) {
				g_free(provider->aws_access_key_id);
				provider->aws_access_key_id = g_strdup(access_key_id);
			}
			if(secret_access_key && *secret_access_key) {
				g_free(provider->aws_secret_access_key);
				provider->aws_secret_access_key = g_strdup(secret_access_key);
			}
			if(session_token && *session_token) {
				g_free(provider->aws_session_token);
				provider->aws_session_token = g_strdup(session_token);
			}
			if(vocabulary_name && *vocabulary_name) {
				g_free(provider->vocabulary_name);
				provider->vocabulary_name = g_strdup(vocabulary_name);
			}
			const char *vocabulary_prompt = json_string_value(json_object_get(cfg, "aws_vocabulary_prompt"));
			if(vocabulary_prompt && *vocabulary_prompt) {
				g_free(provider->vocabulary_prompt);
				provider->vocabulary_prompt = g_strdup(vocabulary_prompt);
			}
			if(json_is_boolean(medical_enabled_obj))
				provider->medical_enabled = json_boolean_value(medical_enabled_obj) ? 1 : 0;
			provider->medical_redaction = redaction ? 1 : 0;
			if(json_is_boolean(fast_mode_obj))
				provider->fast_mode = json_boolean_value(fast_mode_obj) ? 1 : 0;
			if(!provider->medical_enabled) {
				provider->medical_redaction = 0;
				g_free(provider->specialty);
				provider->specialty = g_strdup("PRIMARYCARE");
			}
		}
		if(cfg)
			json_decref(cfg);
	}

	ABMOD_LOG("init OK region=%s lang=%s specialty=%s stream_type=%s medical=%s redaction=%s vocabulary=%s fast_mode=%s",
		provider->region, provider->language_code, provider->specialty, provider->stream_type,
		provider->medical_enabled ? "ON" : "OFF",
		provider->medical_redaction ? "redaction ON" : "redaction OFF",
		provider->vocabulary_name ? provider->vocabulary_name : (provider->vocabulary_prompt ? provider->vocabulary_prompt : "no vocab"),
		provider->fast_mode ? "ON" : "OFF");
	abmod_aws_native_global_init();

	/* If a vocabulary prompt was given and no explicit vocabulary name was set,
	 * create the vocabulary in the background so transcription can start immediately.
	 * Once READY (up to 3 min), the thread reconnects all active streams. */
	if(provider->vocabulary_prompt && *provider->vocabulary_prompt &&
	   (!provider->vocabulary_name || !*provider->vocabulary_name)) {
		provider->vocab_cancel = 0;
		if(pthread_create(&provider->vocab_thread, NULL, vocab_creation_thread, provider) == 0) {
			provider->vocab_thread_running = 1;
			ABMOD_LOG("vocabulary creation started in background (max 3 min wait)");
		} else {
			ABMOD_LOG("failed to start vocab thread — streams will proceed without vocabulary");
		}
	}

	*out_impl = provider;
	*out_vtbl = &ABMOD_AWS_VTBL;
	return 0;
}
