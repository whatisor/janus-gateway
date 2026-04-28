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
#include "abmod_provider_aws_sdk.h"

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
#define ABMOD_TRANSLATION_STOP ((abmod_translation_task *)(intptr_t)1)
#define ABMOD_EVENT_SET_OUTPUT_LANGUAGE "set-output-language:"
#define ABMOD_EVENT_SET_LANGUAGE "set-language:"
#define ABMOD_EVENT_CLEAR_OUTPUT_LANGUAGE "clear-output-language"

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

typedef struct abmod_lang_route_s {
	char *room_id;
	char *target_user_id;
	char *language;
} abmod_lang_route;

typedef struct abmod_translation_target_s {
	char *target_user_id;
	char *language;
} abmod_translation_target;

typedef struct abmod_translation_task_s {
	char *provider_name;
	char *room_id;
	char *user_id;
	char *item_id;
	char *source_language;
	char *text;
	int is_final;
	GPtrArray *targets;
} abmod_translation_task;

typedef struct abmod_participant_ref_s {
	char *room_id;
	char *user_id;
} abmod_participant_ref;

typedef struct abmod_ctx_s {
	uint32_t rate;
	int channels;
	char *config;
	char *provider_name;
	char *input_language;
	int translate_partials;
	char *aws_region;
	char *aws_access_key_id;
	char *aws_secret_access_key;
	char *aws_session_token;
	GPtrArray *participant_routes;
	GHashTable *known_participants;
	GAsyncQueue *translation_queue;
	pthread_t translation_thread;
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
} abmod_ctx;

static void abmod_emit(abmod_ctx *ctx, const char *event_name, json_t *payload);
static int abmod_is_auth_error(const char *message);

static char *abmod_participant_key(const char *room_id, const char *user_id) {
	return g_strdup_printf("%s|%s", room_id ? room_id : "", user_id ? user_id : "");
}

static void abmod_lang_route_free(gpointer data) {
	abmod_lang_route *route = (abmod_lang_route *)data;
	if(!route)
		return;
	free(route->room_id);
	free(route->target_user_id);
	free(route->language);
	free(route);
}

static void abmod_translation_target_free(gpointer data) {
	abmod_translation_target *target = (abmod_translation_target *)data;
	if(!target)
		return;
	free(target->target_user_id);
	free(target->language);
	free(target);
}

static void abmod_translation_task_free(gpointer data) {
	abmod_translation_task *task = (abmod_translation_task *)data;
	if(!task)
		return;
	free(task->provider_name);
	free(task->room_id);
	free(task->user_id);
	free(task->item_id);
	free(task->source_language);
	free(task->text);
	if(task->targets)
		g_ptr_array_free(task->targets, TRUE);
	free(task);
}

static void abmod_participant_ref_free(gpointer data) {
	abmod_participant_ref *ref = (abmod_participant_ref *)data;
	if(!ref)
		return;
	free(ref->room_id);
	free(ref->user_id);
	free(ref);
}

static void abmod_set_participant_known_locked(abmod_ctx *ctx,
		const char *room_id,
		const char *user_id) {
	if(!ctx || !ctx->known_participants || !room_id || !*room_id || !user_id || !*user_id)
		return;
	char *key = abmod_participant_key(room_id, user_id);
	if(!key)
		return;
	if(g_hash_table_lookup(ctx->known_participants, key)) {
		g_free(key);
		return;
	}
	abmod_participant_ref *ref = (abmod_participant_ref *)calloc(1, sizeof(*ref));
	if(!ref) {
		g_free(key);
		return;
	}
	ref->room_id = strdup(room_id);
	ref->user_id = strdup(user_id);
	if(!ref->room_id || !ref->user_id) {
		abmod_participant_ref_free(ref);
		g_free(key);
		return;
	}
	g_hash_table_insert(ctx->known_participants, key, ref);
}

static void abmod_clear_participant_known_locked(abmod_ctx *ctx,
		const char *room_id,
		const char *user_id) {
	if(!ctx || !ctx->known_participants || !room_id || !*room_id || !user_id || !*user_id)
		return;
	char *key = abmod_participant_key(room_id, user_id);
	if(!key)
		return;
	g_hash_table_remove(ctx->known_participants, key);
	g_free(key);
}

static abmod_lang_route *abmod_find_participant_route_locked(abmod_ctx *ctx,
		const char *room_id,
		const char *target_user_id) {
	if(!ctx || !ctx->participant_routes || !target_user_id || !*target_user_id)
		return NULL;
	for(guint i = 0; i < ctx->participant_routes->len; ++i) {
		abmod_lang_route *route = (abmod_lang_route *)g_ptr_array_index(ctx->participant_routes, i);
		if(!route || !route->target_user_id)
			continue;
		if(strcmp(route->target_user_id, target_user_id) != 0)
			continue;
		if(route->room_id && *route->room_id) {
			if(!room_id || strcmp(route->room_id, room_id) != 0)
				continue;
		}
		return route;
	}
	return NULL;
}

static void abmod_set_participant_language_locked(abmod_ctx *ctx,
		const char *room_id,
		const char *target_user_id,
		const char *language) {
	if(!ctx || !ctx->participant_routes || !target_user_id || !*target_user_id || !language || !*language)
		return;
	abmod_lang_route *route = abmod_find_participant_route_locked(ctx, room_id, target_user_id);
	if(route) {
		free(route->language);
		route->language = strdup(language);
		return;
	}
	route = (abmod_lang_route *)calloc(1, sizeof(*route));
	if(!route)
		return;
	route->room_id = (room_id && *room_id) ? strdup(room_id) : NULL;
	route->target_user_id = strdup(target_user_id);
	route->language = strdup(language);
	if(!route->target_user_id || !route->language) {
		abmod_lang_route_free(route);
		return;
	}
	g_ptr_array_add(ctx->participant_routes, route);
}

static void abmod_clear_participant_language_locked(abmod_ctx *ctx,
		const char *room_id,
		const char *target_user_id) {
	if(!ctx || !ctx->participant_routes || !target_user_id || !*target_user_id)
		return;
	for(guint i = 0; i < ctx->participant_routes->len; ++i) {
		abmod_lang_route *route = (abmod_lang_route *)g_ptr_array_index(ctx->participant_routes, i);
		if(!route || !route->target_user_id)
			continue;
		if(strcmp(route->target_user_id, target_user_id) != 0)
			continue;
		if(route->room_id && *route->room_id) {
			if(!room_id || strcmp(route->room_id, room_id) != 0)
				continue;
		}
		g_ptr_array_remove_index(ctx->participant_routes, i);
		return;
	}
}

static const char *abmod_event_lang_value(const char *event_name) {
	if(!event_name)
		return NULL;
	if(g_str_has_prefix(event_name, ABMOD_EVENT_SET_OUTPUT_LANGUAGE))
		return event_name + strlen(ABMOD_EVENT_SET_OUTPUT_LANGUAGE);
	if(g_str_has_prefix(event_name, ABMOD_EVENT_SET_LANGUAGE))
		return event_name + strlen(ABMOD_EVENT_SET_LANGUAGE);
	return NULL;
}

static int abmod_try_apply_config_event(abmod_ctx *ctx,
		const char *event_name,
		const char *room_id,
		const char *user_id) {
	if(!ctx || !event_name || event_name[0] != '{')
		return 0;
	json_error_t jerr;
	json_t *cfg = json_loads(event_name, 0, &jerr);
	if(!cfg || !json_is_object(cfg)) {
		if(cfg)
			json_decref(cfg);
		return 0;
	}

	const char *cfg_room_id = json_string_value(json_object_get(cfg, "room_id"));
	const char *cfg_user_id = json_string_value(json_object_get(cfg, "target_user_id"));
	if(!cfg_user_id || !*cfg_user_id)
		cfg_user_id = json_string_value(json_object_get(cfg, "user_id"));
	if(!cfg_room_id || !*cfg_room_id)
		cfg_room_id = room_id;
	if(!cfg_user_id || !*cfg_user_id)
		cfg_user_id = user_id;

	json_t *set_out_lang = json_object_get(cfg, "set-output-language");
	json_t *set_lang = json_object_get(cfg, "set-language");
	json_t *output_lang = json_object_get(cfg, "output_language");
	json_t *clear_out_lang = json_object_get(cfg, "clear-output-language");

	const char *language = NULL;
	if(json_is_string(set_out_lang))
		language = json_string_value(set_out_lang);
	else if(json_is_string(set_lang))
		language = json_string_value(set_lang);
	else if(json_is_string(output_lang))
		language = json_string_value(output_lang);

	int handled = 0;
	if(cfg_room_id && *cfg_room_id && cfg_user_id && *cfg_user_id && language && *language) {
		pthread_mutex_lock(&ctx->lock);
		abmod_set_participant_known_locked(ctx, cfg_room_id, cfg_user_id);
		abmod_set_participant_language_locked(ctx, cfg_room_id, cfg_user_id, language);
		pthread_mutex_unlock(&ctx->lock);
		ABMOD_LOG("config set participant output language room=%s user=%s lang=%s",
			cfg_room_id, cfg_user_id, language);
		handled = 1;
	} else if(cfg_room_id && *cfg_room_id && cfg_user_id && *cfg_user_id &&
			((json_is_boolean(clear_out_lang) && json_is_true(clear_out_lang)) ||
			 (language != NULL && !*language))) {
		pthread_mutex_lock(&ctx->lock);
		abmod_clear_participant_language_locked(ctx, cfg_room_id, cfg_user_id);
		pthread_mutex_unlock(&ctx->lock);
		ABMOD_LOG("config cleared participant output language room=%s user=%s",
			cfg_room_id, cfg_user_id);
		handled = 1;
	}

	json_decref(cfg);
	return handled;
}

static int abmod_lang_equals(const char *a, const char *b) {
	if(!a || !b)
		return 0;
	char *na = g_ascii_strdown(a, -1);
	char *nb = g_ascii_strdown(b, -1);
	if(!na || !nb) {
		g_free(na);
		g_free(nb);
		return 0;
	}
	for(char *p = na; *p; ++p) {
		if(*p == '_')
			*p = '-';
	}
	for(char *p = nb; *p; ++p) {
		if(*p == '_')
			*p = '-';
	}
	char *dash_a = strchr(na, '-');
	char *dash_b = strchr(nb, '-');
	if(dash_a)
		*dash_a = '\0';
	if(dash_b)
		*dash_b = '\0';
	int eq = strcmp(na, nb) == 0;
	g_free(na);
	g_free(nb);
	return eq;
}

static int abmod_should_translate(const char *source_language, const char *target_language) {
	if(!target_language || !*target_language)
		return 0;
	if(!source_language || !*source_language)
		return 1;
	return !abmod_lang_equals(source_language, target_language);
}

static void abmod_emit_transcription(abmod_ctx *ctx,
		const char *provider_name,
		const char *room_id,
		const char *user_id,
		const char *item_id,
		const char *language,
		const char *text,
		int is_final,
		const char *source_language,
		const char *source_text,
		const char *target_user_id,
		int translated) {
	json_t *payload = json_object();
	json_object_set_new(payload, "provider", json_string(provider_name ? provider_name : "aws"));
	json_object_set_new(payload, "room_id", json_string(room_id ? room_id : ""));
	json_object_set_new(payload, "user_id", json_string(user_id ? user_id : ""));
	json_object_set_new(payload, "item_id", json_string(item_id ? item_id : ""));
	json_object_set_new(payload, "language", json_string(language ? language : "en-US"));
	json_object_set_new(payload, "text", json_string(text ? text : ""));
	json_object_set_new(payload, "type", json_string(is_final ? "final" : "partial"));
	json_object_set_new(payload, "translated", json_boolean(translated ? 1 : 0));
	if(source_language && *source_language)
		json_object_set_new(payload, "source_language", json_string(source_language));
	if(source_text && *source_text)
		json_object_set_new(payload, "source_text", json_string(source_text));
	if(target_user_id && *target_user_id)
		json_object_set_new(payload, "target_user_id", json_string(target_user_id));
	json_object_set_new(payload, "ts_us", json_integer((json_int_t)g_get_real_time()));
	abmod_emit(ctx, "transcription", payload);
	json_decref(payload);
}

static char *abmod_translate_text_aws(abmod_ctx *ctx,
		const char *room_id,
		const char *user_id,
		const char *source_language,
		const char *target_language,
		const char *text,
		int *ok) {
	if(ok)
		*ok = 0;
	if(!ctx || !target_language || !*target_language || !text)
		return NULL;
	if(!abmod_should_translate(source_language, target_language)) {
		char *copy = strdup(text);
		if(ok)
			*ok = copy ? 1 : 0;
		return copy;
	}
	if(!ctx->provider_name || g_ascii_strcasecmp(ctx->provider_name, "aws") != 0)
		return NULL;

	AbmodAwsNativeConfig cfg = {0};
	cfg.region = (ctx->aws_region && *ctx->aws_region) ? ctx->aws_region : "us-east-1";
	cfg.access_key_id = (ctx->aws_access_key_id && *ctx->aws_access_key_id) ? ctx->aws_access_key_id : NULL;
	cfg.secret_access_key = (ctx->aws_secret_access_key && *ctx->aws_secret_access_key) ? ctx->aws_secret_access_key : NULL;
	cfg.session_token = (ctx->aws_session_token && *ctx->aws_session_token) ? ctx->aws_session_token : NULL;
	char *translated = NULL;
	char *err = NULL;
	int rc = abmod_aws_native_translate_text_dup(&cfg, source_language, target_language, text, &translated, &err);
	if(rc == 0 && translated) {
		if(ok)
			*ok = 1;
		return translated;
	}
	ABMOD_LOG("translation failed room=%s user=%s %s->%s: %s",
		room_id ? room_id : "?",
		user_id ? user_id : "?",
		source_language ? source_language : "auto",
		target_language,
		err ? err : "unknown error");
	if(err) {
		json_t *payload = json_object();
		json_object_set_new(payload, "provider", json_string("aws"));
		json_object_set_new(payload, "room_id", json_string(room_id ? room_id : ""));
		json_object_set_new(payload, "user_id", json_string(user_id ? user_id : ""));
		json_object_set_new(payload, "language", json_string(target_language));
		json_object_set_new(payload, "text", json_string(err));
		json_object_set_new(payload, "type", json_string(abmod_is_auth_error(err) ? "auth_error" : "error"));
		json_object_set_new(payload, "ts_us", json_integer((json_int_t)g_get_real_time()));
		abmod_emit(ctx, "error", payload);
		json_decref(payload);
		free(err);
	}
	free(translated);
	return NULL;
}

static void abmod_translation_group_array_destroy(gpointer data) {
	GPtrArray *arr = (GPtrArray *)data;
	if(arr)
		g_ptr_array_free(arr, TRUE);
}

static void abmod_translation_group_add(GHashTable *groups,
		const char *language,
		const char *target_user_id) {
	if(!groups || !language || !*language || !target_user_id || !*target_user_id)
		return;
	GPtrArray *users = (GPtrArray *)g_hash_table_lookup(groups, language);
	if(!users) {
		users = g_ptr_array_new_with_free_func(free);
		g_hash_table_insert(groups, strdup(language), users);
	}
	for(guint i = 0; i < users->len; ++i) {
		const char *existing = (const char *)g_ptr_array_index(users, i);
		if(existing && strcmp(existing, target_user_id) == 0)
			return;
	}
	g_ptr_array_add(users, strdup(target_user_id));
}

static void *abmod_translation_worker(void *arg) {
	abmod_ctx *ctx = (abmod_ctx *)arg;
	if(!ctx || !ctx->translation_queue)
		return NULL;
	while(1) {
		abmod_translation_task *task = (abmod_translation_task *)g_async_queue_pop(ctx->translation_queue);
		if(task == ABMOD_TRANSLATION_STOP)
			break;
		if(!task)
			continue;

		GHashTable *groups = g_hash_table_new_full(g_str_hash, g_str_equal, free, abmod_translation_group_array_destroy);
		if(task->targets) {
			for(guint i = 0; i < task->targets->len; ++i) {
				abmod_translation_target *target = (abmod_translation_target *)g_ptr_array_index(task->targets, i);
				if(!target || !target->language || !target->target_user_id)
					continue;
				/* Same target/source language => direct source delivery, no translate call. */
				if(abmod_lang_equals(task->source_language, target->language)) {
					abmod_emit_transcription(ctx,
						task->provider_name,
						task->room_id,
						task->user_id,
						task->item_id,
						task->source_language,
						task->text,
						task->is_final,
						task->source_language,
						task->text,
						target->target_user_id,
						0);
					continue;
				}
				abmod_translation_group_add(groups, target->language, target->target_user_id);
			}
		}

		GHashTableIter iter;
		gpointer key = NULL, value = NULL;
		g_hash_table_iter_init(&iter, groups);
		while(g_hash_table_iter_next(&iter, &key, &value)) {
			const char *language = (const char *)key;
			GPtrArray *users = (GPtrArray *)value;
			if(!language || !*language || !users || users->len == 0)
				continue;
			int ok = 0;
			char *translated = abmod_translate_text_aws(ctx,
				task->room_id,
				task->user_id,
				task->source_language,
				language,
				task->text,
				&ok);
			if(!ok || !translated) {
				free(translated);
				continue;
			}
			for(guint i = 0; i < users->len; ++i) {
				const char *target_user_id = (const char *)g_ptr_array_index(users, i);
				if(!target_user_id || !*target_user_id)
					continue;
				abmod_emit_transcription(ctx,
					task->provider_name,
					task->room_id,
					task->user_id,
					task->item_id,
					language,
					translated,
					task->is_final,
					task->source_language,
					task->text,
					target_user_id,
					1);
			}
			free(translated);
		}

		g_hash_table_destroy(groups);
		abmod_translation_task_free(task);
	}
	return NULL;
}

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

static void abmod_on_transcript(void *user,
		const char *provider_name,
		const char *room_id,
		const char *user_id,
		const char *text,
		int is_final,
		const char *item_id) {
	abmod_ctx *ctx = (abmod_ctx *)user;
	if(!ctx)
		return;
	const char *source_language = ctx->input_language ? ctx->input_language : "en-US";
	ABMOD_LOG("transcript [%s] room=%s user=%s item_id=%s %s: %s",
		provider_name ? provider_name : "?",
		room_id ? room_id : "?",
		user_id ? user_id : "?",
		item_id ? item_id : "?",
		is_final ? "FINAL" : "partial",
		text ? text : "(empty)");

	if(!text)
		text = "";
	if(!is_final && !ctx->translate_partials)
		return;

	if(!ctx->participant_routes || !ctx->translation_queue || !ctx->known_participants)
		return;

	abmod_translation_task *task = (abmod_translation_task *)calloc(1, sizeof(*task));
	if(!task)
		return;
	task->provider_name = strdup(provider_name ? provider_name : "aws");
	task->room_id = strdup(room_id ? room_id : "");
	task->user_id = strdup(user_id ? user_id : "");
	task->item_id = strdup(item_id ? item_id : "");
	task->source_language = strdup(source_language ? source_language : "en-US");
	task->text = strdup(text);
	task->is_final = is_final;
	task->targets = g_ptr_array_new_with_free_func(abmod_translation_target_free);
	if(!task->provider_name || !task->room_id || !task->user_id || !task->item_id || !task->source_language || !task->text || !task->targets) {
		abmod_translation_task_free(task);
		return;
	}

	pthread_mutex_lock(&ctx->lock);
	GHashTableIter piter;
	gpointer pkey = NULL, pvalue = NULL;
	g_hash_table_iter_init(&piter, ctx->known_participants);
	while(g_hash_table_iter_next(&piter, &pkey, &pvalue)) {
		abmod_participant_ref *pref = (abmod_participant_ref *)pvalue;
		if(!pref || !pref->room_id || !pref->user_id)
			continue;
		if(!room_id || strcmp(pref->room_id, room_id) != 0)
			continue;
		abmod_lang_route *route = abmod_find_participant_route_locked(ctx, room_id, pref->user_id);
		const char *effective_language = (route && route->language && *route->language) ? route->language : source_language;
		abmod_translation_target *target = (abmod_translation_target *)calloc(1, sizeof(*target));
		if(!target)
			continue;
		target->target_user_id = strdup(pref->user_id);
		target->language = strdup(effective_language);
		if(!target->target_user_id || !target->language) {
			abmod_translation_target_free(target);
			continue;
		}
		g_ptr_array_add(task->targets, target);
	}
	pthread_mutex_unlock(&ctx->lock);

	if(task->targets->len == 0) {
		/* Default behavior: when no participant output language is set,
		 * deliver source transcript directly without running translation. */
		abmod_emit_transcription(ctx,
			provider_name,
			room_id,
			user_id,
			item_id,
			source_language,
			text,
			is_final,
			source_language,
			text,
			NULL,
			0);
		abmod_translation_task_free(task);
		return;
	}

	g_async_queue_push(ctx->translation_queue, task);
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
	json_object_set_new(payload, "language", json_string(ctx->input_language ? ctx->input_language : "en-US"));
	json_object_set_new(payload, "text", json_string(err));
	json_object_set_new(payload, "type", json_string(etype));
	json_object_set_new(payload, "ts_us", json_integer((json_int_t)g_get_real_time()));
	abmod_emit(ctx, "error", payload);
	json_decref(payload);
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
			else if(strcmp(item.event_name, "muted") == 0 || strcmp(item.event_name, "left") == 0)
				abmod_close_stream_locked(ctx, item.room_id, item.user_id);
		} else if(item.type == ABMOD_ITEM_PCM_USER || item.type == ABMOD_ITEM_PCM_MIX) {
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
	ctx->input_language = strdup("en-US");
	ctx->translate_partials = 0;
	ctx->aws_region = strdup("us-east-1");
	ctx->aws_access_key_id = getenv("AWS_ACCESS_KEY_ID") ? strdup(getenv("AWS_ACCESS_KEY_ID")) : NULL;
	ctx->aws_secret_access_key = getenv("AWS_SECRET_ACCESS_KEY") ? strdup(getenv("AWS_SECRET_ACCESS_KEY")) : NULL;
	ctx->aws_session_token = getenv("AWS_SESSION_TOKEN") ? strdup(getenv("AWS_SESSION_TOKEN")) : NULL;
	ctx->participant_routes = g_ptr_array_new_with_free_func(abmod_lang_route_free);
	ctx->known_participants = g_hash_table_new_full(g_str_hash, g_str_equal, g_free, abmod_participant_ref_free);
	ctx->translation_queue = g_async_queue_new();
	ctx->enable_mix = 0;
	ctx->last_room_id = NULL;
	if(cbs)
		ctx->cbs = *cbs;
	ctx->user = user;
	pthread_mutex_init(&ctx->lock, NULL);
	pthread_cond_init(&ctx->cv, NULL);
	ctx->active_streams = g_hash_table_new_full(g_str_hash, g_str_equal, g_free, NULL);

	if(config_json) {
		json_error_t jerr;
		json_t *cfg = json_loads(config_json, 0, &jerr);
		if(cfg && json_is_object(cfg)) {
			const char *provider_name = json_string_value(json_object_get(cfg, "provider"));
			const char *language = json_string_value(json_object_get(cfg, "aws_language_code"));
			const char *region = json_string_value(json_object_get(cfg, "aws_region"));
			const char *access_key_id = json_string_value(json_object_get(cfg, "aws_access_key_id"));
			const char *secret_access_key = json_string_value(json_object_get(cfg, "aws_secret_access_key"));
			const char *session_token = json_string_value(json_object_get(cfg, "aws_session_token"));
			json_t *translate_partials = json_object_get(cfg, "translate_partials");
			int enable_mix = json_boolean_value(json_object_get(cfg, "enable_mix"));
			if(provider_name && *provider_name) {
				free(ctx->provider_name);
				ctx->provider_name = strdup(provider_name);
			}
			if(language && *language) {
				free(ctx->input_language);
				ctx->input_language = strdup(language);
			}
			if(region && *region) {
				free(ctx->aws_region);
				ctx->aws_region = strdup(region);
			}
			if(access_key_id && *access_key_id) {
				free(ctx->aws_access_key_id);
				ctx->aws_access_key_id = strdup(access_key_id);
			}
			if(secret_access_key && *secret_access_key) {
				free(ctx->aws_secret_access_key);
				ctx->aws_secret_access_key = strdup(secret_access_key);
			}
			if(session_token && *session_token) {
				free(ctx->aws_session_token);
				ctx->aws_session_token = strdup(session_token);
			}
			if(json_is_boolean(translate_partials))
				ctx->translate_partials = json_boolean_value(translate_partials) ? 1 : 0;
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
		g_ptr_array_free(ctx->participant_routes, TRUE);
		g_hash_table_destroy(ctx->known_participants);
		g_async_queue_unref(ctx->translation_queue);
		pthread_cond_destroy(&ctx->cv);
		pthread_mutex_destroy(&ctx->lock);
		free(ctx->provider_name);
		free(ctx->input_language);
		free(ctx->aws_region);
		free(ctx->aws_access_key_id);
		free(ctx->aws_secret_access_key);
		free(ctx->aws_session_token);
		free(ctx->config);
		free(ctx);
		return NULL;
	}

	if(pthread_create(&ctx->translation_thread, NULL, abmod_translation_worker, ctx) != 0) {
		abmod_provider_destroy(ctx->provider);
		g_hash_table_destroy(ctx->active_streams);
		g_ptr_array_free(ctx->participant_routes, TRUE);
		g_hash_table_destroy(ctx->known_participants);
		g_async_queue_unref(ctx->translation_queue);
		pthread_cond_destroy(&ctx->cv);
		pthread_mutex_destroy(&ctx->lock);
		free(ctx->provider_name);
		free(ctx->input_language);
		free(ctx->aws_region);
		free(ctx->aws_access_key_id);
		free(ctx->aws_secret_access_key);
		free(ctx->aws_session_token);
		free(ctx->config);
		free(ctx);
		return NULL;
	}

	ABMOD_LOG("provider '%s' ready, starting worker thread", ctx->provider_name);
	ctx->running = 1;
	if(pthread_create(&ctx->worker_thread, NULL, abmod_worker, ctx) != 0) {
		g_async_queue_push(ctx->translation_queue, ABMOD_TRANSLATION_STOP);
		pthread_join(ctx->translation_thread, NULL);
		ctx->running = 0;
		abmod_provider_destroy(ctx->provider);
		g_hash_table_destroy(ctx->active_streams);
		g_ptr_array_free(ctx->participant_routes, TRUE);
		g_hash_table_destroy(ctx->known_participants);
		g_async_queue_unref(ctx->translation_queue);
		pthread_cond_destroy(&ctx->cv);
		pthread_mutex_destroy(&ctx->lock);
		free(ctx->provider_name);
		free(ctx->input_language);
		free(ctx->aws_region);
		free(ctx->aws_access_key_id);
		free(ctx->aws_secret_access_key);
		free(ctx->aws_session_token);
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
	g_async_queue_push(ctx->translation_queue, ABMOD_TRANSLATION_STOP);
	pthread_join(ctx->translation_thread, NULL);
	pthread_join(ctx->worker_thread, NULL);
	for(size_t i = 0; i < ABMOD_QCAP; ++i)
		abmod_queue_item_reset(&ctx->queue[i]);
	abmod_provider_destroy(ctx->provider);
	g_hash_table_destroy(ctx->active_streams);
	g_ptr_array_free(ctx->participant_routes, TRUE);
	g_hash_table_destroy(ctx->known_participants);
	g_async_queue_unref(ctx->translation_queue);
	pthread_cond_destroy(&ctx->cv);
	pthread_mutex_destroy(&ctx->lock);
	free(ctx->provider_name);
	free(ctx->input_language);
	free(ctx->aws_region);
	free(ctx->aws_access_key_id);
	free(ctx->aws_secret_access_key);
	free(ctx->aws_session_token);
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
	if(abmod_try_apply_config_event(ctx, event_name, room_id, user_id))
		return;
	if(strcmp(event_name, ABMOD_EVENT_CLEAR_OUTPUT_LANGUAGE) == 0) {
		pthread_mutex_lock(&ctx->lock);
		abmod_clear_participant_language_locked(ctx, room_id, user_id);
		pthread_mutex_unlock(&ctx->lock);
		ABMOD_LOG("cleared participant output language room=%s user=%s",
			room_id ? room_id : "?", user_id ? user_id : "?");
		return;
	}
	if(strcmp(event_name, "talking") == 0 || strcmp(event_name, "unmuted") == 0 ||
			strcmp(event_name, "muted") == 0 || strcmp(event_name, "left") == 0) {
		if(strcmp(event_name, "talking") == 0 || strcmp(event_name, "unmuted") == 0 || strcmp(event_name, "muted") == 0) {
			pthread_mutex_lock(&ctx->lock);
			abmod_set_participant_known_locked(ctx, room_id, user_id);
			pthread_mutex_unlock(&ctx->lock);
		}
		if(strcmp(event_name, "left") == 0) {
			pthread_mutex_lock(&ctx->lock);
			abmod_clear_participant_language_locked(ctx, room_id, user_id);
			abmod_clear_participant_known_locked(ctx, room_id, user_id);
			pthread_mutex_unlock(&ctx->lock);
		}
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

	/* Throttled log: print once every 500 frames (~10 s at 20 ms/frame) */
	static volatile int pcm_log_counter = 0;
	if(__atomic_add_fetch(&pcm_log_counter, 1, __ATOMIC_RELAXED) % 500 == 1)
		ABMOD_LOG("pcm room=%s user=%s samples=%zu rate=%u", room_id, user_id, samples, sampling_rate);

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
