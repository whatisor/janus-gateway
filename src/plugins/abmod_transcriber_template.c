/*
 * Transcriber module for AudioBridge custom module ABI.
 *
 * Goals:
 * - Per-user transcription is the default path via abmod_on_participant_pcm.
 * - abmod_on_mix is optional (disabled by default) since it lacks room_id/user_id.
 * - No blocking work on AudioBridge decode/mix threads: PCM and events are enqueued
 *   into a bounded queue and processed by a worker thread.
 *
 * Provider:
 * - Uses abmod_provider abstraction.
 * - Default provider is AWS ("aws").
 * - OpenAI is supported via provider implementation ("openai").
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>
#include <pthread.h>
#include <glib.h>
#include <jansson.h>

#include "janus_ab_module.h"
#include "abmod_provider.h"
#include "abmod_gender.h"

/* Simple stderr logger — visible in `docker logs janus-gateway` */
#define ABMOD_LOG(fmt, ...) \
	fprintf(stderr, "[ABMod][transcriber] " fmt "\n", ##__VA_ARGS__)

/* Forward declarations of exported ABI symbols (silences -Wmissing-prototypes) */
void *abmod_create(uint32_t sampling_rate, int channels, const char *config_json,
                   const janus_abmod_callbacks *cbs, void *user);
void  abmod_destroy(void *vctx);
void  abmod_on_mix(void *vctx, const int16_t *pcm, size_t samples,
                   uint32_t sampling_rate, int channels,
                   uint32_t rtp_timestamp, uint64_t frame_seq,
                   uint64_t active_talk_version);
void  abmod_on_event(void *vctx, const char *event_name,
                     const char *room_id, const char *user_id,
                     int64_t event_time_us, uint64_t talk_version);
void  abmod_on_participant_pcm(void *vctx, const char *room_id, const char *user_id,
                               const int16_t *pcm, size_t samples,
                               uint32_t sampling_rate, int channels,
                               uint32_t rtp_timestamp, uint64_t frame_seq,
                               uint64_t active_talk_version);

#define ABMOD_QCAP 300
#define ABMOD_ITEM_EVENT 1
#define ABMOD_ITEM_PCM_USER 2
#define ABMOD_ITEM_PCM_MIX 3

typedef struct abmod_qitem_s {
	int type;
	char room_id[128];
	char user_id[128];
	char event_name[32];
	int16_t *pcm;
	size_t samples;
	uint32_t sampling_rate;
	int channels;
} abmod_qitem;

typedef struct abmod_ctx_s {
	uint32_t rate;
	int channels;
	char *config;
	char *provider_name;
	char *language;
	int enable_mix;
	char *last_room_id;
	janus_abmod_callbacks cbs;
	void *user;
	pthread_t worker_thread;
	int running;
	pthread_mutex_t lock;
	pthread_cond_t cv;
	abmod_qitem queue[ABMOD_QCAP];
	size_t head;
	size_t size;
	GHashTable *active_streams;
	abmod_provider *provider;
	abmod_gender_engine *gender;
} abmod_ctx;

/* Internal queue helper used by callbacks declared later in the file. */
static int abmod_enqueue_locked(abmod_ctx *ctx, const abmod_qitem *src);

static void abmod_emit(abmod_ctx *ctx, const char *event_name, json_t *payload) {
	if(!ctx || !ctx->cbs.emit_event || !event_name || !payload)
		return;
	char *text = json_dumps(payload, JSON_COMPACT);
	if(text) {
		ctx->cbs.emit_event(ctx->cbs.emit_event_user, event_name, text);
		free(text);
	}
}

static int abmod_is_auth_error(const char *message) {
	if(!message || !*message)
		return 0;
	char *lower = g_ascii_strdown(message, -1);
	if(!lower)
		return 0;
	int is_auth =
		strstr(lower, "auth") != NULL ||
		strstr(lower, "unauthorized") != NULL ||
		strstr(lower, "forbidden") != NULL ||
		strstr(lower, "credential") != NULL ||
		strstr(lower, "signature") != NULL ||
		strstr(lower, "token") != NULL ||
		strstr(lower, "access denied") != NULL ||
		strstr(lower, "expired") != NULL ||
		strstr(lower, "403") != NULL ||
		strstr(lower, "401") != NULL;
	g_free(lower);
	return is_auth;
}

static int abmod_is_idle_timeout_error(const char *message) {
	if(!message || !*message)
		return 0;
	char *lower = g_ascii_strdown(message, -1);
	if(!lower)
		return 0;
	int is_timeout =
		strstr(lower, "timed out because no new audio was received") != NULL ||
		strstr(lower, "no new audio was received") != NULL;
	g_free(lower);
	return is_timeout;
}

static void abmod_on_transcript(void *user,
		const char *provider_name,
		const char *room_id,
		const char *user_id,
		const char *text,
		float transcript_confidence,
		int is_final,
		const char *item_id) {
	abmod_ctx *ctx = (abmod_ctx *)user;
	if(!ctx)
		return;
	ABMOD_LOG("transcript [%s] room=%s user=%s item_id=%s %s conf=%.3f: %s",
		provider_name ? provider_name : "?",
		room_id ? room_id : "?",
		user_id ? user_id : "?",
		item_id ? item_id : "?",
		is_final ? "FINAL" : "partial",
		transcript_confidence,
		text ? text : "(empty)");
	json_t *payload = json_object();
	json_object_set_new(payload, "provider", json_string(provider_name ? provider_name : "aws"));
	json_object_set_new(payload, "room_id", json_string(room_id ? room_id : ""));
	json_object_set_new(payload, "user_id", json_string(user_id ? user_id : ""));
	json_object_set_new(payload, "item_id", json_string(item_id ? item_id : ""));
	json_object_set_new(payload, "language", json_string(ctx->language ? ctx->language : "en-US"));
	json_object_set_new(payload, "text", json_string(text ? text : ""));
	json_object_set_new(payload, "transcript_confidence", json_real(transcript_confidence));
	json_object_set_new(payload, "type", json_string(is_final ? "final" : "partial"));
	char gender_label[32] = {0};
	float gender_confidence = 0.0f;
	const char *gender_status = "disabled";
	if(room_id && user_id && ctx->gender) {
		abmod_gender_trigger_on_transcript(ctx->gender, room_id, user_id, text, transcript_confidence, is_final);
		if(abmod_gender_get_result(ctx->gender, room_id, user_id,
				gender_label, sizeof(gender_label),
				&gender_confidence, &gender_status)) {
			json_object_set_new(payload, "gender", json_string(gender_label));
			json_object_set_new(payload, "gender_confidence", json_real(gender_confidence));
		}
	}
	json_object_set_new(payload, "gender_status", json_string(gender_status ? gender_status : "disabled"));
	json_object_set_new(payload, "ts_us", json_integer((json_int_t)g_get_real_time()));
	abmod_emit(ctx, "transcription", payload);
	json_decref(payload);
}

static void abmod_on_error(void *user,
		const char *provider_name,
		const char *room_id,
		const char *user_id,
		const char *error_message) {
	abmod_ctx *ctx = (abmod_ctx *)user;
	if(!ctx)
		return;
	ABMOD_LOG("ERROR [%s] room=%s user=%s: %s",
		provider_name ? provider_name : "?",
		room_id ? room_id : "?",
		user_id ? user_id : "?",
		error_message ? error_message : "(null)");
	const char *err = error_message ? error_message : "provider error";
	const char *etype = abmod_is_auth_error(err) ? "auth_error" : "error";
	json_t *payload = json_object();
	json_object_set_new(payload, "provider", json_string(provider_name ? provider_name : "aws"));
	json_object_set_new(payload, "room_id", json_string(room_id ? room_id : ""));
	json_object_set_new(payload, "user_id", json_string(user_id ? user_id : ""));
	json_object_set_new(payload, "language", json_string(ctx->language ? ctx->language : "en-US"));
	json_object_set_new(payload, "text", json_string(err));
	json_object_set_new(payload, "type", json_string(etype));
	json_object_set_new(payload, "ts_us", json_integer((json_int_t)g_get_real_time()));
	abmod_emit(ctx, "error", payload);
	json_decref(payload);

	/* On AWS idle-timeout,
	 * stream and active_streams bookkeeping stay in sync. */
	if(room_id && *room_id && user_id && *user_id && abmod_is_idle_timeout_error(err)) {
		abmod_qitem item;
		memset(&item, 0, sizeof(item));
		item.type = ABMOD_ITEM_EVENT;
		snprintf(item.event_name, sizeof(item.event_name), "%s", "idle_timeout");
		snprintf(item.room_id, sizeof(item.room_id), "%s", room_id);
		snprintf(item.user_id, sizeof(item.user_id), "%s", user_id);
		pthread_mutex_lock(&ctx->lock);
		abmod_enqueue_locked(ctx, &item);
		pthread_mutex_unlock(&ctx->lock);
	}
}

static char *abmod_stream_key(const char *room_id, const char *user_id) {
	return g_strdup_printf("%s|%s", room_id ? room_id : "", user_id ? user_id : "");
}

static void abmod_queue_item_reset(abmod_qitem *item) {
	if(!item)
		return;
	if(item->pcm) {
		free(item->pcm);
		item->pcm = NULL;
	}
	memset(item, 0, sizeof(*item));
}

static int abmod_enqueue_locked(abmod_ctx *ctx, const abmod_qitem *src) {
	if(!ctx || !src)
		return -1;
	if(ctx->size >= ABMOD_QCAP) {
		/* Prefer enqueuing control events (e.g., stopped-talking) by dropping one PCM item */
		if(src->type == ABMOD_ITEM_EVENT) {
			for(size_t i = 0; i < ctx->size; ++i) {
				size_t idx = (ctx->head + i) % ABMOD_QCAP;
				if(ctx->queue[idx].type == ABMOD_ITEM_PCM_USER || ctx->queue[idx].type == ABMOD_ITEM_PCM_MIX) {
					abmod_queue_item_reset(&ctx->queue[idx]);
					while(i + 1 < ctx->size) {
						size_t from = (ctx->head + i + 1) % ABMOD_QCAP;
						size_t to = (ctx->head + i) % ABMOD_QCAP;
						ctx->queue[to] = ctx->queue[from];
						memset(&ctx->queue[from], 0, sizeof(ctx->queue[from]));
						i++;
					}
					size_t tail = (ctx->head + ctx->size - 1) % ABMOD_QCAP;
					memset(&ctx->queue[tail], 0, sizeof(ctx->queue[tail]));
					ctx->size--;
					break;
				}
			}
		}
		if(ctx->size >= ABMOD_QCAP)
			return -1;
	}
	size_t pos = (ctx->head + ctx->size) % ABMOD_QCAP;
	ctx->queue[pos] = *src;
	ctx->size++;
	pthread_cond_signal(&ctx->cv);
	return 0;
}

static int abmod_ensure_stream_locked(abmod_ctx *ctx,
		const char *room_id,
		const char *user_id,
		uint32_t sampling_rate,
		int channels) {
	char *key = abmod_stream_key(room_id, user_id);
	gpointer present = g_hash_table_lookup(ctx->active_streams, key);
	if(present) {
		g_free(key);
		return 0;
	}
	ABMOD_LOG("opening stream room=%s user=%s rate=%u ch=%d",
		room_id ? room_id : "?", user_id ? user_id : "?", sampling_rate, channels);
	if(abmod_provider_open_user_stream(ctx->provider, room_id, user_id, sampling_rate, channels) != 0) {
		ABMOD_LOG("open_user_stream FAILED room=%s user=%s",
			room_id ? room_id : "?", user_id ? user_id : "?");
		g_free(key);
		return -1;
	}
	ABMOD_LOG("stream opened room=%s user=%s", room_id ? room_id : "?", user_id ? user_id : "?");
	g_hash_table_insert(ctx->active_streams, key, GINT_TO_POINTER(1));
	return 0;
}

static void abmod_close_stream_locked(abmod_ctx *ctx, const char *room_id, const char *user_id) {
	char *key = abmod_stream_key(room_id, user_id);
	gboolean present = g_hash_table_remove(ctx->active_streams, key);
	g_free(key);
	if(present) {
		ABMOD_LOG("closing stream room=%s user=%s", room_id ? room_id : "?", user_id ? user_id : "?");
		abmod_provider_close_user_stream(ctx->provider, room_id, user_id);
	}
}

static void *abmod_worker(void *arg) {
	abmod_ctx *ctx = (abmod_ctx *)arg;
	while(1) {
		abmod_qitem item;
		memset(&item, 0, sizeof(item));
		pthread_mutex_lock(&ctx->lock);
		while(ctx->running && ctx->size == 0)
			pthread_cond_wait(&ctx->cv, &ctx->lock);
		if(!ctx->running && ctx->size == 0) {
			pthread_mutex_unlock(&ctx->lock);
			break;
		}
		item = ctx->queue[ctx->head];
		memset(&ctx->queue[ctx->head], 0, sizeof(ctx->queue[ctx->head]));
		ctx->head = (ctx->head + 1) % ABMOD_QCAP;
		ctx->size--;
		pthread_mutex_unlock(&ctx->lock);

		if(item.type == ABMOD_ITEM_EVENT) {
			if(strcmp(item.event_name, "talking") == 0 || strcmp(item.event_name, "unmuted") == 0)
				abmod_ensure_stream_locked(ctx, item.room_id, item.user_id,
					ctx->rate, ctx->channels);
			else if(strcmp(item.event_name, "muted") == 0
				|| strcmp(item.event_name, "left") == 0 || strcmp(item.event_name, "idle_timeout") == 0) {
				abmod_close_stream_locked(ctx, item.room_id, item.user_id);
				if(strcmp(item.event_name, "left") == 0 || strcmp(item.event_name, "idle_timeout") == 0)
					abmod_gender_clear_user(ctx->gender, item.room_id, item.user_id);
			}
		} else if(item.type == ABMOD_ITEM_PCM_USER || item.type == ABMOD_ITEM_PCM_MIX) {
			if(item.type == ABMOD_ITEM_PCM_USER)
				abmod_gender_on_pcm(ctx->gender, item.room_id, item.user_id,
					item.pcm, item.samples, item.sampling_rate, item.channels);
			if(abmod_ensure_stream_locked(ctx, item.room_id, item.user_id, item.sampling_rate, item.channels) == 0)
				abmod_provider_send_pcm(ctx->provider, item.room_id, item.user_id,
					item.pcm, item.samples, item.sampling_rate, item.channels);
		}
		abmod_queue_item_reset(&item);
	}
	return NULL;
}

void* abmod_create(uint32_t sampling_rate, int channels,
		const char *config_json, const janus_abmod_callbacks *cbs, void *user) {
	ABMOD_LOG("create rate=%u ch=%d config=%s", sampling_rate, channels,
		config_json ? config_json : "(none)");
	abmod_ctx *ctx = (abmod_ctx *)calloc(1, sizeof(abmod_ctx));
	if(!ctx)
		return NULL;
	ctx->rate = sampling_rate;
	ctx->channels = channels;
	ctx->config = config_json ? strdup(config_json) : NULL;
	ctx->provider_name = strdup("aws");
	ctx->language = strdup("en-US");
	ctx->enable_mix = 0;
	ctx->last_room_id = NULL;
	if(cbs)
		ctx->cbs = *cbs;
	ctx->user = user;
	pthread_mutex_init(&ctx->lock, NULL);
	pthread_cond_init(&ctx->cv, NULL);
	ctx->active_streams = g_hash_table_new_full(g_str_hash, g_str_equal, g_free, NULL);
	ctx->gender = abmod_gender_create(config_json);

	if(config_json) {
		json_error_t jerr;
		json_t *cfg = json_loads(config_json, 0, &jerr);
		if(cfg && json_is_object(cfg)) {
			const char *provider_name = json_string_value(json_object_get(cfg, "provider"));
			const char *language = json_string_value(json_object_get(cfg, "aws_language_code"));
			int enable_mix = json_boolean_value(json_object_get(cfg, "enable_mix"));
			if(provider_name && *provider_name) {
				free(ctx->provider_name);
				ctx->provider_name = strdup(provider_name);
			}
			if(language && *language) {
				free(ctx->language);
				ctx->language = strdup(language);
			}
			ctx->enable_mix = enable_mix ? 1 : 0;
		}
		if(cfg)
			json_decref(cfg);
	}

	abmod_provider_callbacks provider_callbacks = {0};
	provider_callbacks.on_transcript = abmod_on_transcript;
	provider_callbacks.on_error = abmod_on_error;
	ABMOD_LOG("creating provider '%s'", ctx->provider_name);
	ctx->provider = abmod_provider_create(ctx->provider_name, config_json, &provider_callbacks, ctx);
	if(!ctx->provider) {
		ABMOD_LOG("provider_create FAILED for '%s'", ctx->provider_name);
		g_hash_table_destroy(ctx->active_streams);
		pthread_cond_destroy(&ctx->cv);
		pthread_mutex_destroy(&ctx->lock);
		free(ctx->provider_name);
		free(ctx->language);
		free(ctx->config);
		free(ctx);
		return NULL;
	}

	ABMOD_LOG("provider '%s' ready, starting worker thread", ctx->provider_name);
	ctx->running = 1;
	if(pthread_create(&ctx->worker_thread, NULL, abmod_worker, ctx) != 0) {
		ctx->running = 0;
		abmod_provider_destroy(ctx->provider);
		abmod_gender_destroy(ctx->gender);
		g_hash_table_destroy(ctx->active_streams);
		pthread_cond_destroy(&ctx->cv);
		pthread_mutex_destroy(&ctx->lock);
		free(ctx->provider_name);
		free(ctx->language);
		free(ctx->config);
		free(ctx);
		return NULL;
	}
	ABMOD_LOG("created OK (provider=%s rate=%u)", ctx->provider_name, ctx->rate);
	return ctx;
}

void abmod_destroy(void *vctx) {
	abmod_ctx *ctx = (abmod_ctx *)vctx;
	if(!ctx)
		return;
	ABMOD_LOG("destroy");
	pthread_mutex_lock(&ctx->lock);
	ctx->running = 0;
	pthread_cond_broadcast(&ctx->cv);
	pthread_mutex_unlock(&ctx->lock);
	pthread_join(ctx->worker_thread, NULL);
	for(size_t i = 0; i < ABMOD_QCAP; ++i)
		abmod_queue_item_reset(&ctx->queue[i]);
	abmod_provider_destroy(ctx->provider);
	abmod_gender_destroy(ctx->gender);
	g_hash_table_destroy(ctx->active_streams);
	pthread_cond_destroy(&ctx->cv);
	pthread_mutex_destroy(&ctx->lock);
	free(ctx->provider_name);
	free(ctx->language);
	free(ctx->last_room_id);
	free(ctx->config);
	free(ctx);
}

void abmod_on_mix(void *vctx, const int16_t *pcm, size_t samples,
		uint32_t sampling_rate, int channels,
		uint32_t rtp_timestamp, uint64_t frame_seq, uint64_t active_talk_version) {
	(void)rtp_timestamp;
	(void)frame_seq;
	(void)active_talk_version;
	abmod_ctx *ctx = (abmod_ctx *)vctx;
	if(!ctx || !pcm || samples == 0)
		return;
	if(!ctx->enable_mix)
		return;
	/* abmod_on_mix has no room_id/user_id; use last seen room_id if available */
	const char *room_id = ctx->last_room_id;
	if(!room_id || !*room_id)
		return;
	int16_t *pcm_copy = (int16_t *)malloc(samples * sizeof(int16_t));
	if(!pcm_copy)
		return;
	memcpy(pcm_copy, pcm, samples * sizeof(int16_t));
	abmod_qitem item;
	memset(&item, 0, sizeof(item));
	item.type = ABMOD_ITEM_PCM_MIX;
	snprintf(item.room_id, sizeof(item.room_id), "%s", room_id);
	snprintf(item.user_id, sizeof(item.user_id), "%s", "mixed");
	item.pcm = pcm_copy;
	item.samples = samples;
	item.sampling_rate = sampling_rate;
	item.channels = channels;
	pthread_mutex_lock(&ctx->lock);
	if(abmod_enqueue_locked(ctx, &item) == 0)
		pcm_copy = NULL;
	pthread_mutex_unlock(&ctx->lock);
	if(pcm_copy)
		free(pcm_copy);
}

void abmod_on_event(void *vctx, const char *event_name,
		const char *room_id, const char *user_id,
		int64_t event_time_us, uint64_t talk_version) {
	(void)event_time_us;
	(void)talk_version;
	abmod_ctx *ctx = (abmod_ctx *)vctx;
	if(!ctx || !event_name || !room_id || !user_id)
		return;
	/* Keep a room_id hint for optional abmod_on_mix */
	pthread_mutex_lock(&ctx->lock);
	if(!ctx->last_room_id || strcmp(ctx->last_room_id, room_id) != 0) {
		free(ctx->last_room_id);
		ctx->last_room_id = strdup(room_id);
	}
	pthread_mutex_unlock(&ctx->lock);

	ABMOD_LOG("event '%s' room=%s user=%s", event_name, room_id, user_id);
	if(strcmp(event_name, "talking") == 0 || strcmp(event_name, "unmuted") == 0 ||
			strcmp(event_name, "muted") == 0 || strcmp(event_name, "left") == 0 ||
			strcmp(event_name, "stopped-talking") == 0) {
		abmod_qitem item;
		memset(&item, 0, sizeof(item));
		item.type = ABMOD_ITEM_EVENT;
		snprintf(item.event_name, sizeof(item.event_name), "%s", event_name);
		snprintf(item.room_id, sizeof(item.room_id), "%s", room_id);
		snprintf(item.user_id, sizeof(item.user_id), "%s", user_id);
		pthread_mutex_lock(&ctx->lock);
		abmod_enqueue_locked(ctx, &item);
		pthread_mutex_unlock(&ctx->lock);
	}
}

void abmod_on_participant_pcm(void *vctx,
		const char *room_id,
		const char *user_id,
		const int16_t *pcm,
		size_t samples,
		uint32_t sampling_rate,
		int channels,
		uint32_t rtp_timestamp,
		uint64_t frame_seq,
		uint64_t active_talk_version) {
	(void)rtp_timestamp;
	(void)frame_seq;
	(void)active_talk_version;
	abmod_ctx *ctx = (abmod_ctx *)vctx;
	if(!ctx || !room_id || !user_id || !pcm || samples == 0)
		return;
	/* Remember room_id for optional abmod_on_mix */
	pthread_mutex_lock(&ctx->lock);
	if(!ctx->last_room_id || strcmp(ctx->last_room_id, room_id) != 0) {
		free(ctx->last_room_id);
		ctx->last_room_id = strdup(room_id);
	}
	pthread_mutex_unlock(&ctx->lock);

	/* enable_mix uses mixed PCM path only; skip per-user streaming */
	if(ctx->enable_mix)
		return;

	int16_t *pcm_copy = (int16_t *)malloc(samples * sizeof(int16_t));
	if(!pcm_copy)
		return;
	memcpy(pcm_copy, pcm, samples * sizeof(int16_t));
	abmod_qitem item;
	memset(&item, 0, sizeof(item));
	item.type = ABMOD_ITEM_PCM_USER;
	snprintf(item.room_id, sizeof(item.room_id), "%s", room_id);
	snprintf(item.user_id, sizeof(item.user_id), "%s", user_id);
	item.pcm = pcm_copy;
	item.samples = samples;
	item.sampling_rate = sampling_rate;
	item.channels = channels;
	pthread_mutex_lock(&ctx->lock);
	if(abmod_enqueue_locked(ctx, &item) == 0)
		pcm_copy = NULL;
	pthread_mutex_unlock(&ctx->lock);
	if(pcm_copy)
		free(pcm_copy);
}
