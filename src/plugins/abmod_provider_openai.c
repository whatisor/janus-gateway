/*
 * OpenAI provider implementation for abmod_provider.
 *
 * Advanced features:
 *  fast_mode  – a "mini" stream per user with aggressive VAD emits partials;
 *               the main stream(s) emit finals.
 *  concurrent – N parallel main-stream connections per user.  Finals are only
 *               emitted when all N respond within GUARD_WINDOW_US (hallucination
 *               guard).  With concurrent=1 finals are emitted immediately.
 *  VAD config – threshold, prefix_padding_ms, silence_duration_ms, type.
 *  presentation_mode – halves silence_duration_ms.
 *  prompt     – client-supplied verbatim; server never generates one.
 */
// TODO: Not tested yet.
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>
#include <pthread.h>
#include <time.h>

#include <glib.h>
#include <jansson.h>
#include <libwebsockets.h>
#include "audiobridge-deps/speex/speex_resampler.h"

#include "abmod_provider_impl.h"
#include "abmod_embedding.h"
#include <sys/stat.h>

#ifndef ABMOD_EMBEDDING_DEFAULT_DIR
#define ABMOD_EMBEDDING_DEFAULT_DIR "/usr/local/share/janus/models/all-MiniLM-L6-v2"
#endif

/* Simple stderr logger — visible in `docker logs janus-gateway` */
#define ABMOD_LOG(fmt, ...) \
	fprintf(stderr, "[ABMod][provider_openai] " fmt "\n", ##__VA_ARGS__)

#define STR_FIELD(key, dst) \
if((s = json_string_value(json_object_get(cfg, key))) && *s) \
	str_assign(&p->dst, s)

static int abmod_file_exists(const char *path) {
	struct stat st;
	return path && *path && stat(path, &st) == 0 && S_ISREG(st.st_mode);
}

#define OPENAI_PROVIDER_NAME  "openai"
#define OPENAI_INPUT_RATE     24000
#define GUARD_WINDOW_US       3000000LL  /* 3 s */

/* Mini-stream VAD — matches hook's miniTurnDetection defaults */
#define MINI_VAD_THRESHOLD    0.75f
#define MINI_VAD_PREFIX_MS    250
#define MINI_VAD_SILENCE_MS   250

/* ── Forward declarations ────────────────────────────────────────── */

typedef struct openai_provider_s   openai_provider;
typedef struct openai_stream_s     openai_stream;
typedef struct openai_user_session_s openai_user_session;
int abmod_provider_openai_init(const char *config_json,
		const abmod_provider_callbacks *cbs,
		void *cb_user,
		void **out_impl,
		const abmod_provider_vtbl **out_vtbl);

/* ── Message queue ───────────────────────────────────────────────── */

typedef struct openai_msg_s {
	char *data;
	struct openai_msg_s *next;
} openai_msg;

/* ── Guard bucket (collects concurrent main finals) ─────────────── */

typedef struct openai_bucket_s {
	char   *room_id;
	char   *user_id;
	char   *item_id;
	char  **texts;
	int     count;
	int     target;
	int64_t deadline_us;
	size_t  prompt_len;  /* for master-result shortcut: accept any text > prompt_len */
	struct openai_bucket_s *next;
} openai_bucket;

/* ── Per-stream config snapshot (copied at creation) ────────────── */

typedef struct {
	const char *prompt;
	const char *lang;
	const char *noise_reduction;
	const char *vad_type;
	float       vad_threshold;
	int         vad_prefix_padding_ms;
	int         vad_silence_duration_ms;
	int         presentation_mode;
	int         is_fast;   /* 1 = mini/partial stream */
} openai_stream_config;

/* ── User session (one fast stream + N main streams per user) ────── */

struct openai_user_session_s {
	char          *room_id;
	char          *user_id;
	openai_stream *fast_stream;
	openai_stream **main_streams;
	int            concurrent;
	/* Mirrors hook behavior: one shared item_id per active utterance */
	char          *current_item_id;
	uint64_t       next_item_seq;
	pthread_mutex_t item_mtx;
};

/* ── Provider ─────────────────────────────────────────────────────── */

struct openai_provider_s {
	GHashTable             *sessions;
	pthread_mutex_t         mtx;
	abmod_provider_callbacks cbs;
	void                   *cb_user;
	/* Config */
	char *api_key;
	char *model;
	char *ws_url;
	char *prompt;
	char *lang;
	char *noise_reduction;
	/* VAD config */
	char *vad_type;
	float vad_threshold;
	int   vad_prefix_padding_ms;
	int   vad_silence_duration_ms;
	int   presentation_mode;
	/* Concurrency */
	int fast_mode;
	int concurrent;
	/* Embedding selectBest */
	char *embedding_model_path;
	char *embedding_vocab_path;
	float embedding_similarity_threshold;
	/* Guard */
	openai_bucket  *guard_head;
	pthread_mutex_t guard_mtx;
	pthread_cond_t  guard_cv;
	pthread_t       guard_thread;
	int             guard_running;
};

/* ── Stream ──────────────────────────────────────────────────────── */

struct openai_stream_s {
	char           *key;
	char           *room_id;
	char           *user_id;
	openai_provider *p;
	openai_user_session *session;
	/* Per-stream config snapshot */
	char  *prompt;
	char  *lang;
	char  *noise_reduction;
	char  *vad_type;
	float  vad_threshold;
	int    vad_prefix_padding_ms;
	int    vad_silence_duration_ms;
	int    presentation_mode;
	int    is_fast;
	/* Resampler (mono in → 24k) */
	SpeexResamplerState *resampler;
	uint32_t in_rate;
	/* WS */
	pthread_t        th;
	int              running;
	struct lws_context *lws_ctx;
	struct lws        *wsi;
	int               connected;
	pthread_mutex_t   qmtx;
	openai_msg       *qhead;
	openai_msg       *qtail;
	int               qlen;
	/* URL parts */
	char host[128];
	int  port;
	int  use_ssl;
	char path[256];
	/* Headers */
	char auth_value[256];
	char beta_value[64];
	/* RX buffer */
	char  *rx;
	size_t rx_len;
	size_t rx_cap;
	/* Partial accumulator */
	char  *partial;
	size_t partial_len;
	size_t partial_cap;
};

/* ═══════════════════════════════════════════════════════════════════
 * Helpers
 * ═══════════════════════════════════════════════════════════════════ */

static char *openai_build_key(const char *room_id, const char *user_id) {
	return g_strdup_printf("%s|%s", room_id ? room_id : "", user_id ? user_id : "");
}

static void str_assign(char **dst, const char *src) {
	if(*dst) { free(*dst); *dst = NULL; }
	if(src) *dst = strdup(src);
}

static void openai_emit_error(openai_provider *p,
		const char *room_id, const char *user_id, const char *msg) {
	if(p && p->cbs.on_error)
		p->cbs.on_error(p->cb_user, OPENAI_PROVIDER_NAME, room_id, user_id,
			msg ? msg : "OpenAI error");
}

static char *openai_session_make_item_id(openai_user_session *sess) {
	if(!sess) return g_strdup("");
	int64_t now_ms = g_get_real_time() / 1000;
	guint32 rnd = g_random_int();
	return g_strdup_printf("%s_%s_%lld_%llu_%u",
		sess->room_id ? sess->room_id : "",
		sess->user_id ? sess->user_id : "",
		(long long)now_ms,
		(unsigned long long)(++sess->next_item_seq),
		(unsigned int)rnd);
}

/* Keep one item_id pinned while a turn is in flight (same model as the TS hook). */
static char *openai_session_ensure_item_id(openai_user_session *sess,
		const char *preferred_item_id) {
	if(!sess) return g_strdup(preferred_item_id ? preferred_item_id : "");
	pthread_mutex_lock(&sess->item_mtx);
	if(!sess->current_item_id || !*sess->current_item_id) {
		if(preferred_item_id && *preferred_item_id)
			sess->current_item_id = strdup(preferred_item_id);
		else
			sess->current_item_id = openai_session_make_item_id(sess);
	}
	char *item_id = strdup(sess->current_item_id ? sess->current_item_id : "");
	pthread_mutex_unlock(&sess->item_mtx);
	return item_id;
}

static void openai_session_clear_item_if_matches(openai_user_session *sess,
		const char *item_id) {
	if(!sess || !item_id || !*item_id) return;
	pthread_mutex_lock(&sess->item_mtx);
	if(sess->current_item_id && strcmp(sess->current_item_id, item_id) == 0) {
		free(sess->current_item_id);
		sess->current_item_id = NULL;
	}
	pthread_mutex_unlock(&sess->item_mtx);
}

static void openai_provider_clear_session_item_if_matches(openai_provider *p,
		const char *room_id, const char *user_id, const char *item_id) {
	if(!p || !item_id || !*item_id) return;
	char *key = openai_build_key(room_id, user_id);
	if(!key) return;
	pthread_mutex_lock(&p->mtx);
	openai_user_session *sess =
		(openai_user_session *)g_hash_table_lookup(p->sessions, key);
	pthread_mutex_unlock(&p->mtx);
	g_free(key);
	if(sess)
		openai_session_clear_item_if_matches(sess, item_id);
}

/* ═══════════════════════════════════════════════════════════════════
 * Guard: collect concurrent main finals, resolve when all respond
 * ═══════════════════════════════════════════════════════════════════ */

static void openai_guard_resolve(openai_provider *p, openai_bucket *b) {
	const char *best = "";

	/* Master-result shortcut (mirrors hook's resolveBucket):
	 * if any single result is longer than the prompt, accept it immediately
	 * without requiring all N instances to agree. */
	if(b->prompt_len > 0) {
		for(int i = 0; i < b->count; i++) {
			if(b->texts[i] && strlen(b->texts[i]) > b->prompt_len) {
				best = b->texts[i];
				goto emit;
			}
		}
	}

	if(b->count < b->target) {
		/* Not all instances responded — discard (hallucination suppression) */
		best = "";
		ABMOD_LOG("Not all instances responded — discard (hallucination suppression)\n");
	} else if(abmod_embedding_ready()) {
		/* Embedding-based selectBest: mirrors embeddingGuard.ts exactly */
		int idx = abmod_embedding_select_best((const char **)b->texts, b->count);
		best = (idx >= 0 && b->texts[idx]) ? b->texts[idx] : "";
	} else {
		/* Fallback when embedding model not loaded: pick longest text */
		size_t best_len = 0;
		for(int i = 0; i < b->count; i++) {
			size_t l = b->texts[i] ? strlen(b->texts[i]) : 0;
			if(l > best_len) { best_len = l; best = b->texts[i]; }
		}
		if(best_len == 0) best = "";
	}

emit:
	if(p->cbs.on_transcript)
		p->cbs.on_transcript(p->cb_user, OPENAI_PROVIDER_NAME,
			b->room_id, b->user_id, best, -1.0f, 1, b->item_id);
	openai_provider_clear_session_item_if_matches(p,
		b->room_id, b->user_id, b->item_id);
	for(int i = 0; i < b->count; i++) free(b->texts[i]);
	free(b->texts);
	free(b->room_id);
	free(b->user_id);
	free(b->item_id);
	free(b);
}

/* Called from stream LWS threads — must be lock-safe */
static void openai_guard_add(openai_provider *p,
		const char *room_id, const char *user_id,
		const char *item_id, const char *text, int concurrent) {
	/* Mirror hook: double the window when concurrent >= 3 */
	int64_t window_us = (concurrent >= 3) ? GUARD_WINDOW_US * 2 : GUARD_WINDOW_US;

	pthread_mutex_lock(&p->guard_mtx);
	openai_bucket *b = p->guard_head;
	while(b) {
		if(strcmp(b->room_id, room_id) == 0 &&
				strcmp(b->user_id, user_id) == 0 &&
				strcmp(b->item_id, item_id ? item_id : "") == 0)
			break;
		b = b->next;
	}
	if(!b) {
		b = (openai_bucket *)calloc(1, sizeof(*b));
		if(!b) { pthread_mutex_unlock(&p->guard_mtx); return; }
		b->room_id     = strdup(room_id ? room_id : "");
		b->user_id     = strdup(user_id ? user_id : "");
		b->item_id     = strdup(item_id ? item_id : "");
		b->target      = concurrent;
		b->deadline_us = g_get_real_time() + window_us;
		b->prompt_len  = (p->prompt && *p->prompt) ? strlen(p->prompt) : 0;
		b->next        = p->guard_head;
		p->guard_head  = b;
	}
	b->texts = (char **)realloc(b->texts, (b->count + 1) * sizeof(char *));
	b->texts[b->count++] = strdup(text ? text : "");
	int ready = (b->count >= b->target);
	pthread_mutex_unlock(&p->guard_mtx);
	if(ready)
		pthread_cond_signal(&p->guard_cv);
}

/* Remove and discard any pending bucket for (room_id, user_id).
 * Called from close_user_stream to avoid a stale emit after teardown. */
static void openai_guard_cancel_user(openai_provider *p,
		const char *room_id, const char *user_id) {
	pthread_mutex_lock(&p->guard_mtx);
	openai_bucket *prev = NULL, *b = p->guard_head;
	while(b) {
		if(strcmp(b->room_id, room_id) == 0 &&
				strcmp(b->user_id, user_id) == 0) {
			if(prev) prev->next = b->next; else p->guard_head = b->next;
			pthread_mutex_unlock(&p->guard_mtx);
			for(int i = 0; i < b->count; i++) free(b->texts[i]);
			free(b->texts); free(b->room_id); free(b->user_id); free(b->item_id); free(b);
			return;
		}
		prev = b; b = b->next;
	}
	pthread_mutex_unlock(&p->guard_mtx);
}

static void *openai_guard_thread_fn(void *arg) {
	openai_provider *p = (openai_provider *)arg;
	while(p->guard_running) {
		struct timespec ts;
		clock_gettime(CLOCK_REALTIME, &ts);
		/* Wake at most every 100 ms to check for expired buckets */
		ts.tv_nsec += 100000000L;
		if(ts.tv_nsec >= 1000000000L) { ts.tv_sec++; ts.tv_nsec -= 1000000000L; }

		pthread_mutex_lock(&p->guard_mtx);
		pthread_cond_timedwait(&p->guard_cv, &p->guard_mtx, &ts);
		if(!p->guard_running) { pthread_mutex_unlock(&p->guard_mtx); break; }

		int64_t now = g_get_real_time();
		openai_bucket *prev = NULL, *b = p->guard_head;
		while(b) {
			if(b->count >= b->target || now >= b->deadline_us) {
				openai_bucket *next = b->next;
				if(prev) prev->next = next; else p->guard_head = next;
				pthread_mutex_unlock(&p->guard_mtx);
				openai_guard_resolve(p, b);
				pthread_mutex_lock(&p->guard_mtx);
				/* restart scan — list may have changed */
				prev = NULL; b = p->guard_head;
				continue;
			}
			prev = b; b = b->next;
		}
		pthread_mutex_unlock(&p->guard_mtx);
	}
	return NULL;
}

/* ═══════════════════════════════════════════════════════════════════
 * Transcript routing
 * ═══════════════════════════════════════════════════════════════════ */

/*
 * Called for every transcript event from the LWS thread.
 * Routing rules:
 *   is_fast=1            → always emit as partial (is_final forced to 0)
 *   is_fast=0, concurrent=1  → emit directly (partial or final)
 *   is_fast=0, concurrent>1  → partials skipped if fast_mode active;
 *                              finals go through the guard
 */
static void openai_stream_on_transcript_internal(openai_stream *s,
		const char *text, int is_final, const char *event_item_id) {
	openai_provider *p = s->p;
	if(!p) return;
	char *item_id = openai_session_ensure_item_id(s->session, event_item_id);
	if(!item_id) return;

	if(s->is_fast) {
		/* Mini stream: everything is a partial */
		if(p->cbs.on_transcript)
			p->cbs.on_transcript(p->cb_user, OPENAI_PROVIDER_NAME,
				s->room_id, s->user_id, text, -1.0f, 0, item_id);
		free(item_id);
		return;
	}

	if(p->concurrent <= 1) {
		/* Single main stream: pass through unchanged */
		if(p->cbs.on_transcript)
			p->cbs.on_transcript(p->cb_user, OPENAI_PROVIDER_NAME,
				s->room_id, s->user_id, text, -1.0f, is_final, item_id);
		if(is_final)
			openai_session_clear_item_if_matches(s->session, item_id);
		free(item_id);
		return;
	}

	/* Multiple main streams */
	if(!is_final) {
		/* Partials from main streams: forward only when there is no mini stream */
		if(!p->fast_mode && p->cbs.on_transcript)
			p->cbs.on_transcript(p->cb_user, OPENAI_PROVIDER_NAME,
				s->room_id, s->user_id, text, -1.0f, 0, item_id);
		free(item_id);
		return;
	}

	/* Final from one of the concurrent main streams → guard */
	openai_guard_add(p, s->room_id, s->user_id, item_id, text, p->concurrent);
	free(item_id);
}

static const char *openai_extract_item_id(json_t *root) {
	if(!root || !json_is_object(root)) return NULL;
	const char *item_id = json_string_value(json_object_get(root, "item_id"));
	if(item_id && *item_id) return item_id;
	item_id = json_string_value(json_object_get(root, "conversation_item_id"));
	if(item_id && *item_id) return item_id;
	json_t *item = json_object_get(root, "item");
	if(json_is_object(item)) {
		item_id = json_string_value(json_object_get(item, "id"));
		if(item_id && *item_id) return item_id;
	}
	return NULL;
}

/* ═══════════════════════════════════════════════════════════════════
 * WS message queue
 * ═══════════════════════════════════════════════════════════════════ */

static void openai_stream_enqueue(openai_stream *s, const char *json_text) {
	if(!s || !json_text) return;
	openai_msg *m = (openai_msg *)calloc(1, sizeof(*m));
	if(!m) return;
	m->data = strdup(json_text);
	if(!m->data) { free(m); return; }
	pthread_mutex_lock(&s->qmtx);
	if(s->qtail) s->qtail->next = m; else s->qhead = m;
	s->qtail = m;
	s->qlen++;
	pthread_mutex_unlock(&s->qmtx);
	if(s->lws_ctx) lws_cancel_service(s->lws_ctx);
}

/* Prepend to queue front — session update must precede buffered audio */
static void openai_stream_prepend(openai_stream *s, const char *json_text) {
	if(!s || !json_text) return;
	openai_msg *m = (openai_msg *)calloc(1, sizeof(*m));
	if(!m) return;
	m->data = strdup(json_text);
	if(!m->data) { free(m); return; }
	pthread_mutex_lock(&s->qmtx);
	m->next = s->qhead;
	s->qhead = m;
	if(!s->qtail) s->qtail = m;
	s->qlen++;
	pthread_mutex_unlock(&s->qmtx);
}

/* ═══════════════════════════════════════════════════════════════════
 * Session update (sent once per stream on WS connect)
 * ═══════════════════════════════════════════════════════════════════ */

static void openai_stream_send_session_update(openai_stream *s) {
	if(!s || !s->p) return;

	const char *model  = s->p->model ? s->p->model : "gpt-4o-transcribe";
	const char *prompt = (s->prompt && *s->prompt) ? s->prompt : NULL;
	const char *lang   = (s->lang && *s->lang) ? s->lang : "en";
	const char *noise  = (s->noise_reduction && *s->noise_reduction) ? s->noise_reduction : NULL;

	/* VAD: use per-stream values (mini stream has its own hardcoded fast params) */
	const char *vad_type  = (s->vad_type && *s->vad_type) ? s->vad_type : "server_vad";
	float       threshold = s->vad_threshold > 0.0f ? s->vad_threshold : 0.95f;
	int         prefix_ms = s->vad_prefix_padding_ms > 0 ? s->vad_prefix_padding_ms : 300;
	int         silence_ms = s->vad_silence_duration_ms > 0 ? s->vad_silence_duration_ms : 900;
	if(s->presentation_mode && silence_ms > 0)
		silence_ms /= 2;

	char transcription_json[512];
	char noise_json[256]        = "";
	char turn_detection_json[256] = "";

	if(prompt) {
		char *pj = json_dumps(json_string(prompt), JSON_ENCODE_ANY);
		if(pj && lang && *lang)
			snprintf(transcription_json, sizeof(transcription_json),
				"{\"model\":\"%s\",\"prompt\":%s,\"language\":\"%s\"}", model, pj, lang);
		else if(pj)
			snprintf(transcription_json, sizeof(transcription_json),
				"{\"model\":\"%s\",\"prompt\":%s}", model, pj);
		else
			snprintf(transcription_json, sizeof(transcription_json),
				"{\"model\":\"%s\"}", model);
		if(pj) free(pj);
	} else if(lang && *lang) {
		snprintf(transcription_json, sizeof(transcription_json),
			"{\"model\":\"%s\",\"language\":\"%s\"}", model, lang);
	} else {
		snprintf(transcription_json, sizeof(transcription_json),
			"{\"model\":\"%s\"}", model);
	}

	if(noise)
		snprintf(noise_json, sizeof(noise_json),
			",\"input_audio_noise_reduction\":{\"type\":\"%s\"}", noise);

	if(strcmp(vad_type, "none") != 0)
		snprintf(turn_detection_json, sizeof(turn_detection_json),
			",\"turn_detection\":{\"type\":\"%s\",\"threshold\":%.2f,"
			"\"prefix_padding_ms\":%d,\"silence_duration_ms\":%d}",
			vad_type, threshold, prefix_ms, silence_ms);

	char buf[2048];
	snprintf(buf, sizeof(buf),
		"{\"type\":\"transcription_session.update\",\"session\":{"
		"\"input_audio_format\":\"pcm16\","
		"\"input_audio_transcription\":%s"
		"%s%s"
		"}}",
		transcription_json, turn_detection_json, noise_json);
	openai_stream_prepend(s, buf);
}

#define openai_stream_prepend_session_update openai_stream_send_session_update

/* ═══════════════════════════════════════════════════════════════════
 * Audio send
 * ═══════════════════════════════════════════════════════════════════ */

static void openai_stream_send_audio_append(openai_stream *s,
		const void *pcm16, size_t samples) {
	if(!s || !pcm16 || samples == 0) return;
	size_t bytes = samples * sizeof(int16_t);
	char *b64 = (char *)g_base64_encode((const guchar *)pcm16, bytes);
	if(!b64) return;
	size_t cap = strlen(b64) + 64;
	char *json = (char *)malloc(cap);
	if(json) {
		snprintf(json, cap,
			"{\"type\":\"input_audio_buffer.append\",\"audio\":\"%s\"}", b64);
		openai_stream_enqueue(s, json);
		free(json);
	}
	g_free(b64);
}

/* Downmix + resample then enqueue audio for one stream */
static void openai_stream_send_pcm(openai_stream *s,
		const int16_t *pcm, size_t samples,
		uint32_t sample_rate, int channels) {
	if(!s || !s->resampler || !pcm || samples == 0) return;

	/* Cap pre-connect buffering (150 frames ≈ 3 s at 20 ms/frame) */
	if(!s->connected) {
		pthread_mutex_lock(&s->qmtx);
		int full = (s->qlen >= 150);
		pthread_mutex_unlock(&s->qmtx);
		if(full) return;
	}

	/* Downmix to mono */
	size_t in_mono = (channels == 2) ? (samples / 2) : samples;
	int16_t *mono = (int16_t *)malloc(in_mono * sizeof(int16_t));
	if(!mono) return;
	if(channels == 2) {
		for(size_t k = 0; k < in_mono; k++) {
			int32_t l = pcm[2 * k], r = pcm[2 * k + 1];
			mono[k] = (int16_t)((l + r) / 2);
		}
	} else {
		memcpy(mono, pcm, in_mono * sizeof(int16_t));
	}

	/* Resample to 24k */
	spx_uint32_t in_len = (spx_uint32_t)in_mono;
	size_t out_cap = (size_t)((double)in_mono *
		(double)OPENAI_INPUT_RATE / (double)(s->in_rate ? s->in_rate : 48000) + 64);
	int16_t *out = (int16_t *)malloc(out_cap * sizeof(int16_t));
	if(!out) { free(mono); return; }
	spx_uint32_t out_len = (spx_uint32_t)out_cap;
	speex_resampler_process_int(s->resampler, 0, mono, &in_len, out, &out_len);
	free(mono);
	if(out_len > 0)
		openai_stream_send_audio_append(s, out, out_len);
	free(out);
}

/* ═══════════════════════════════════════════════════════════════════
 * Incoming WS message processing
 * ═══════════════════════════════════════════════════════════════════ */

static void openai_stream_process_incoming(openai_stream *s,
		const char *msg, size_t len) {
	(void)len;
	if(!s || !msg || !s->p) return;
	json_error_t jerr;
	json_t *root = json_loads(msg, 0, &jerr);
	if(!root) {
		openai_emit_error(s->p, s->room_id, s->user_id, jerr.text);
		return;
	}
	const char *type = json_string_value(json_object_get(root, "type"));
	const char *event_item_id = openai_extract_item_id(root);

	if(type && (strcmp(type, "response.delta") == 0 ||
			strcmp(type, "transcription.delta") == 0)) {
		const char *delta = json_string_value(json_object_get(root, "delta"));
		if(delta && *delta) {
			pthread_mutex_lock(&s->qmtx);
			size_t need = s->partial_len + strlen(delta) + 1;
			if(need > s->partial_cap) {
				s->partial_cap = need * 2;
				s->partial = (char *)realloc(s->partial, s->partial_cap);
			}
			if(s->partial) {
				memcpy(s->partial + s->partial_len, delta, strlen(delta) + 1);
				s->partial_len += strlen(delta);
			}
			pthread_mutex_unlock(&s->qmtx);
			openai_stream_on_transcript_internal(s, delta, 0, event_item_id);
		}
	} else if(type && (strcmp(type, "response.completed") == 0 ||
			strcmp(type, "transcription.completed") == 0 ||
			strcmp(type, "conversation.item.input_audio_transcription.completed") == 0)) {
		const char *txt = json_string_value(json_object_get(root, "transcript"));
		if(!txt || !*txt) txt = json_string_value(json_object_get(root, "text"));
		if(!txt || !*txt) {
			pthread_mutex_lock(&s->qmtx);
			txt = (s->partial_len > 0 && s->partial) ? s->partial : NULL;
			pthread_mutex_unlock(&s->qmtx);
		}
		if(txt && *txt)
			openai_stream_on_transcript_internal(s, txt, 1, event_item_id);
		pthread_mutex_lock(&s->qmtx);
		s->partial_len = 0;
		pthread_mutex_unlock(&s->qmtx);
	} else if(type && strcmp(type, "error") == 0) {
		const char *err_msg = json_string_value(
			json_object_get(json_object_get(root, "error"), "message"));
		openai_emit_error(s->p, s->room_id, s->user_id,
			err_msg ? err_msg : "OpenAI error");
	}
	json_decref(root);
}

/* ═══════════════════════════════════════════════════════════════════
 * LWS callbacks
 * ═══════════════════════════════════════════════════════════════════ */

static int openai_lws_callback(struct lws *wsi, enum lws_callback_reasons reason,
		void *user, void *in, size_t len) {
	(void)user;
	openai_stream *s = (openai_stream *)lws_context_user(lws_get_context(wsi));
	switch(reason) {
		case LWS_CALLBACK_CLIENT_ESTABLISHED:
			s->wsi = wsi;
			s->connected = 1;
			openai_stream_prepend_session_update(s);
			lws_callback_on_writable(wsi);
			break;
		case LWS_CALLBACK_CLIENT_CONNECTION_ERROR:
			s->connected = 0;
			s->wsi = NULL;
			openai_emit_error(s->p, s->room_id, s->user_id,
				in ? (const char *)in : "connection error");
			break;
		case LWS_CALLBACK_CLIENT_CLOSED:
		case LWS_CALLBACK_CLOSED:
		case LWS_CALLBACK_WSI_DESTROY:
			s->connected = 0;
			s->wsi = NULL;
			break;
		case LWS_CALLBACK_CLIENT_APPEND_HANDSHAKE_HEADER: {
			unsigned char **p = (unsigned char **)in, *end = (*p) + len;
			if(lws_add_http_header_by_token(wsi, WSI_TOKEN_HTTP_AUTHORIZATION,
					(const unsigned char *)s->auth_value,
					strlen(s->auth_value), p, end))
				return -1;
			if(lws_add_http_header_by_name(wsi,
					(const unsigned char *)"openai-beta:",
					(const unsigned char *)s->beta_value,
					strlen(s->beta_value), p, end))
				return -1;
			break;
		}
		case LWS_CALLBACK_CLIENT_RECEIVE: {
			if(len == 0) break;
			if(s->rx_len + len + 1 > s->rx_cap) {
				s->rx_cap = (s->rx_len + len + 1) * 2;
				s->rx = (char *)realloc(s->rx, s->rx_cap);
			}
			if(!s->rx) break;
			memcpy(s->rx + s->rx_len, in, len);
			s->rx_len += len;
			s->rx[s->rx_len] = '\0';
			if(lws_is_final_fragment(wsi)) {
				openai_stream_process_incoming(s, s->rx, s->rx_len);
				s->rx_len = 0;
			}
			break;
		}
		case LWS_CALLBACK_CLIENT_WRITEABLE: {
			pthread_mutex_lock(&s->qmtx);
			openai_msg *m = s->qhead;
			if(m) {
				s->qhead = m->next;
				if(!s->qhead) s->qtail = NULL;
				s->qlen--;
			}
			pthread_mutex_unlock(&s->qmtx);
			if(m) {
				size_t n = strlen(m->data);
				unsigned char *buf = (unsigned char *)malloc(LWS_PRE + n);
				if(buf) {
					memcpy(buf + LWS_PRE, m->data, n);
					lws_write(wsi, buf + LWS_PRE, n, LWS_WRITE_TEXT);
					free(buf);
				}
				free(m->data);
				free(m);
				lws_callback_on_writable(wsi);
			}
			break;
		}
		case LWS_CALLBACK_EVENT_WAIT_CANCELLED:
			if(s->wsi && s->qhead)
				lws_callback_on_writable(s->wsi);
			break;
		default:
			break;
	}
	return 0;
}

static const struct lws_protocols openai_protocols[] = {
	{ .name = "openai-realtime", .callback = openai_lws_callback,
	  .per_session_data_size = 0, .rx_buffer_size = 4096 },
	{ .name = NULL, .callback = NULL }
};

/* ═══════════════════════════════════════════════════════════════════
 * URL parsing
 * ═══════════════════════════════════════════════════════════════════ */

static void openai_parse_url(openai_stream *s, const char *url) {
	snprintf(s->host, sizeof(s->host), "%s", "api.openai.com");
	s->port    = 443;
	s->use_ssl = 1;
	snprintf(s->path, sizeof(s->path), "%s", "/v1/realtime?intent=transcription");
	if(!url || !*url) return;
	s->use_ssl = (strncmp(url, "wss://", 6) == 0);
	int offset = s->use_ssl ? 6 : (strncmp(url, "ws://", 5) == 0 ? 5 : 0);
	const char *host_start = url + offset;
	const char *slash      = strchr(host_start, '/');
	size_t host_len        = slash ? (size_t)(slash - host_start) : strlen(host_start);
	const char *colon      = NULL;
	for(size_t i = 0; i < host_len; i++) {
		if(host_start[i] == ':') { colon = host_start + i; break; }
	}
	if(colon) {
		size_t name_len = (size_t)(colon - host_start);
		if(name_len >= sizeof(s->host)) name_len = sizeof(s->host) - 1;
		memcpy(s->host, host_start, name_len);
		s->host[name_len] = '\0';
		int p = atoi(colon + 1);
		s->port = (p > 0 && p < 65536) ? p : (s->use_ssl ? 443 : 80);
	} else {
		if(host_len >= sizeof(s->host)) host_len = sizeof(s->host) - 1;
		memcpy(s->host, host_start, host_len);
		s->host[host_len] = '\0';
		s->port = s->use_ssl ? 443 : 80;
	}
	if(slash && *slash)
		snprintf(s->path, sizeof(s->path), "%s", slash);
}

/* ═══════════════════════════════════════════════════════════════════
 * Stream lifecycle
 * ═══════════════════════════════════════════════════════════════════ */

static void *openai_stream_thread(void *arg) {
	openai_stream *s = (openai_stream *)arg;
	struct lws_context_creation_info info;
	memset(&info, 0, sizeof(info));
	info.port      = CONTEXT_PORT_NO_LISTEN;
	info.protocols = openai_protocols;
	info.options   = LWS_SERVER_OPTION_DO_SSL_GLOBAL_INIT;
	info.user      = s;
	s->lws_ctx = lws_create_context(&info);
	if(!s->lws_ctx) {
		openai_emit_error(s->p, s->room_id, s->user_id,
			"Failed to create lws context");
		return NULL;
	}
	while(s->running) {
		if(!s->connected && s->wsi == NULL) {
			struct lws_client_connect_info ci;
			memset(&ci, 0, sizeof(ci));
			ci.context            = s->lws_ctx;
			ci.address            = s->host;
			ci.port               = s->port;
			ci.path               = s->path;
			ci.host               = ci.address;
			ci.origin             = ci.address;
			ci.protocol           = NULL;
			ci.local_protocol_name = "openai-realtime";
			ci.ssl_connection     = s->use_ssl ? LCCSCF_USE_SSL : 0;
			ci.alpn               = "http/1.1";
			ci.pwsi               = &s->wsi;
			ci.userdata           = s;
			if(!lws_client_connect_via_info(&ci))
				lws_service(s->lws_ctx, 100);
		}
		lws_service(s->lws_ctx, 10);
	}
	if(s->wsi)
		lws_set_timeout(s->wsi, PENDING_TIMEOUT_KILLED_BY_PROXY_CLIENT_CLOSE,
			LWS_TO_KILL_ASYNC);
	if(s->lws_ctx) {
		lws_service(s->lws_ctx, 0);
		lws_context_destroy(s->lws_ctx);
		s->lws_ctx = NULL;
	}
	return NULL;
}

static openai_stream *openai_stream_create(openai_provider *p,
		openai_user_session *sess,
		const char *room_id, const char *user_id,
		uint32_t sample_rate, int channels,
		const openai_stream_config *cfg) {
	(void)channels;
	openai_stream *s = (openai_stream *)calloc(1, sizeof(*s));
	if(!s) return NULL;
	s->p       = p;
	s->session = sess;
	s->room_id = strdup(room_id ? room_id : "");
	s->user_id = strdup(user_id ? user_id : "");
	s->key     = openai_build_key(room_id, user_id);
	/* Copy per-stream config */
	if(cfg->prompt) s->prompt = strdup(cfg->prompt);
	if(cfg->lang) s->lang = strdup(cfg->lang);
	if(cfg->noise_reduction) s->noise_reduction = strdup(cfg->noise_reduction);
	if(cfg->vad_type) s->vad_type = strdup(cfg->vad_type);
	s->vad_threshold         = cfg->vad_threshold;
	s->vad_prefix_padding_ms = cfg->vad_prefix_padding_ms;
	s->vad_silence_duration_ms = cfg->vad_silence_duration_ms;
	s->presentation_mode     = cfg->presentation_mode;
	s->is_fast               = cfg->is_fast;
	pthread_mutex_init(&s->qmtx, NULL);
	s->rx_cap      = 65536;
	s->rx          = (char *)malloc(s->rx_cap);
	s->partial_cap = 4096;
	s->partial     = (char *)malloc(s->partial_cap);
	s->in_rate     = sample_rate ? sample_rate : 48000;
	int err = 0;
	s->resampler = speex_resampler_init(1, s->in_rate, OPENAI_INPUT_RATE,
		SPEEX_RESAMPLER_QUALITY_VOIP, &err);
	if(!s->resampler || err != RESAMPLER_ERR_SUCCESS)
		openai_emit_error(p, room_id, user_id, "Failed to init resampler");
	const char *url = p->ws_url ? p->ws_url
		: "wss://api.openai.com/v1/realtime?intent=transcription";
	openai_parse_url(s, url);
	snprintf(s->auth_value, sizeof(s->auth_value),
		"Bearer %s", p->api_key ? p->api_key : "");
	snprintf(s->beta_value, sizeof(s->beta_value), "realtime=v1");
	s->running = 1;
	if(pthread_create(&s->th, NULL, openai_stream_thread, s) != 0)
		s->running = 0;
	return s;
}

static void openai_stream_destroy(openai_stream *s) {
	if(!s) return;
	s->running = 0;
	if(s->lws_ctx) lws_cancel_service(s->lws_ctx);
	if(s->th) pthread_join(s->th, NULL);
	pthread_mutex_lock(&s->qmtx);
	openai_msg *m = s->qhead;
	while(m) { openai_msg *n = m->next; free(m->data); free(m); m = n; }
	s->qhead = s->qtail = NULL;
	pthread_mutex_unlock(&s->qmtx);
	pthread_mutex_destroy(&s->qmtx);
	if(s->resampler) speex_resampler_destroy(s->resampler);
	free(s->rx);
	free(s->partial);
	free(s->prompt);
	free(s->lang);
	free(s->noise_reduction);
	free(s->vad_type);
	g_free(s->key);
	free(s->room_id);
	free(s->user_id);
	free(s);
}

/* ═══════════════════════════════════════════════════════════════════
 * User session lifecycle
 * ═══════════════════════════════════════════════════════════════════ */

static openai_user_session *openai_session_create(openai_provider *p,
		const char *room_id, const char *user_id,
		uint32_t sample_rate, int channels) {
	openai_user_session *sess =
		(openai_user_session *)calloc(1, sizeof(*sess));
	if(!sess) return NULL;
	sess->room_id   = strdup(room_id ? room_id : "");
	sess->user_id   = strdup(user_id ? user_id : "");
	sess->concurrent = p->concurrent > 0 ? p->concurrent : 1;
	pthread_mutex_init(&sess->item_mtx, NULL);

	if(p->fast_mode) {
		openai_stream_config mini = {
			.prompt                = NULL,  /* mini never uses a prompt */
			.lang                  = p->lang,
			.noise_reduction       = p->noise_reduction,
			.vad_type              = "server_vad",
			.vad_threshold         = MINI_VAD_THRESHOLD,
			.vad_prefix_padding_ms = MINI_VAD_PREFIX_MS,
			.vad_silence_duration_ms = MINI_VAD_SILENCE_MS,
			.presentation_mode     = 0,
			.is_fast               = 1,
		};
		sess->fast_stream = openai_stream_create(p, sess, room_id, user_id,
			sample_rate, channels, &mini);
	}

	sess->main_streams =
		(openai_stream **)calloc(sess->concurrent, sizeof(openai_stream *));
	if(!sess->main_streams) {
		openai_stream_destroy(sess->fast_stream);
		free(sess->room_id); free(sess->user_id); free(sess);
		return NULL;
	}
	for(int i = 0; i < sess->concurrent; i++) {
		openai_stream_config main_cfg = {
			.prompt                = p->prompt,
			.lang                  = p->lang,
			.noise_reduction       = p->noise_reduction,
			.vad_type              = p->vad_type,
			.vad_threshold         = p->vad_threshold,
			.vad_prefix_padding_ms = p->vad_prefix_padding_ms,
			.vad_silence_duration_ms = p->vad_silence_duration_ms,
			.presentation_mode     = p->presentation_mode,
			.is_fast               = 0,
		};
		sess->main_streams[i] = openai_stream_create(p, sess, room_id, user_id,
			sample_rate, channels, &main_cfg);
	}
	return sess;
}

static void openai_session_destroy(openai_provider *p,
		openai_user_session *sess) {
	if(!sess) return;
	/* Cancel any pending guard bucket before tearing down streams */
	openai_guard_cancel_user(p, sess->room_id, sess->user_id);
	pthread_mutex_lock(&sess->item_mtx);
	free(sess->current_item_id);
	sess->current_item_id = NULL;
	pthread_mutex_unlock(&sess->item_mtx);
	openai_stream_destroy(sess->fast_stream);
	if(sess->main_streams) {
		for(int i = 0; i < sess->concurrent; i++)
			openai_stream_destroy(sess->main_streams[i]);
		free(sess->main_streams);
	}
	free(sess->room_id);
	free(sess->user_id);
	pthread_mutex_destroy(&sess->item_mtx);
	free(sess);
}

/* ═══════════════════════════════════════════════════════════════════
 * Provider vtbl
 * ═══════════════════════════════════════════════════════════════════ */

static void openai_provider_destroy(void *vimpl) {
	openai_provider *p = (openai_provider *)vimpl;
	if(!p) return;
	/* Stop guard thread first */
	pthread_mutex_lock(&p->guard_mtx);
	p->guard_running = 0;
	pthread_cond_signal(&p->guard_cv);
	pthread_mutex_unlock(&p->guard_mtx);
	pthread_join(p->guard_thread, NULL);
	/* Destroy all sessions */
	pthread_mutex_lock(&p->mtx);
	GHashTableIter iter;
	gpointer key = NULL, val = NULL;
	g_hash_table_iter_init(&iter, p->sessions);
	while(g_hash_table_iter_next(&iter, &key, &val)) {
		openai_session_destroy(p, (openai_user_session *)val);
		g_hash_table_iter_remove(&iter);
	}
	pthread_mutex_unlock(&p->mtx);
	g_hash_table_destroy(p->sessions);
	pthread_mutex_destroy(&p->mtx);
	/* Free any leftover guard buckets */
	openai_bucket *b = p->guard_head;
	while(b) {
		openai_bucket *n = b->next;
		for(int i = 0; i < b->count; i++) free(b->texts[i]);
		free(b->texts); free(b->room_id); free(b->user_id); free(b->item_id); free(b);
		b = n;
	}
	pthread_mutex_destroy(&p->guard_mtx);
	pthread_cond_destroy(&p->guard_cv);
	free(p->api_key); free(p->model); free(p->ws_url);
	free(p->prompt); free(p->lang); free(p->noise_reduction);
	free(p->vad_type);
	free(p->embedding_model_path);
	free(p->embedding_vocab_path);
	abmod_embedding_destroy();
	free(p);
}

static int openai_provider_open_user_stream(void *vimpl,
		const char *room_id, const char *user_id,
		uint32_t sample_rate, int channels) {
	openai_provider *p = (openai_provider *)vimpl;
	if(!p || !room_id || !user_id) return -1;
	char *key = openai_build_key(room_id, user_id);
	pthread_mutex_lock(&p->mtx);
	if(g_hash_table_lookup(p->sessions, key)) {
		pthread_mutex_unlock(&p->mtx);
		g_free(key);
		return 0;
	}
	openai_user_session *sess =
		openai_session_create(p, room_id, user_id, sample_rate, channels);
	if(!sess) {
		pthread_mutex_unlock(&p->mtx);
		g_free(key);
		return -1;
	}
	g_hash_table_insert(p->sessions, g_strdup(key), sess);
	pthread_mutex_unlock(&p->mtx);
	g_free(key);
	return 0;
}

static int openai_provider_send_pcm(void *vimpl,
		const char *room_id, const char *user_id,
		const int16_t *pcm, size_t samples,
		uint32_t sample_rate, int channels) {
	openai_provider *p = (openai_provider *)vimpl;
	if(!p || !room_id || !user_id || !pcm || samples == 0) return -1;
	char *key = openai_build_key(room_id, user_id);
	pthread_mutex_lock(&p->mtx);
	openai_user_session *sess =
		(openai_user_session *)g_hash_table_lookup(p->sessions, key);
	pthread_mutex_unlock(&p->mtx);
	g_free(key);
	if(!sess) return -1;
	/* Same PCM to fast stream and all main streams */
	openai_stream_send_pcm(sess->fast_stream, pcm, samples, sample_rate, channels);
	for(int i = 0; i < sess->concurrent; i++)
		openai_stream_send_pcm(sess->main_streams[i], pcm, samples, sample_rate, channels);
	return 0;
}

static int openai_provider_close_user_stream(void *vimpl,
		const char *room_id, const char *user_id) {
	openai_provider *p = (openai_provider *)vimpl;
	if(!p || !room_id || !user_id) return -1;
	char *key = openai_build_key(room_id, user_id);
	pthread_mutex_lock(&p->mtx);
	openai_user_session *sess =
		(openai_user_session *)g_hash_table_lookup(p->sessions, key);
	if(sess) g_hash_table_remove(p->sessions, key);
	pthread_mutex_unlock(&p->mtx);
	g_free(key);
	if(sess) openai_session_destroy(p, sess);
	return 0;
}

static const abmod_provider_vtbl OPENAI_VTBL = {
	.destroy             = openai_provider_destroy,
	.open_user_stream    = openai_provider_open_user_stream,
	.send_pcm            = openai_provider_send_pcm,
	.close_user_stream   = openai_provider_close_user_stream,
};

/* ═══════════════════════════════════════════════════════════════════
 * Init
 * ═══════════════════════════════════════════════════════════════════ */

int abmod_provider_openai_init(const char *config_json,
		const abmod_provider_callbacks *cbs,
		void *cb_user,
		void **out_impl,
		const abmod_provider_vtbl **out_vtbl) {
	if(!out_impl || !out_vtbl) return -1;
	*out_impl = NULL; *out_vtbl = NULL;

	openai_provider *p = (openai_provider *)calloc(1, sizeof(*p));
	if(!p) return -1;
	p->sessions = g_hash_table_new_full(g_str_hash, g_str_equal, g_free, NULL);
	pthread_mutex_init(&p->mtx, NULL);
	pthread_mutex_init(&p->guard_mtx, NULL);
	pthread_cond_init(&p->guard_cv, NULL);
	if(cbs) p->cbs = *cbs;
	p->cb_user = cb_user;

	/* Env defaults */
	str_assign(&p->api_key, getenv("OPENAI_API_KEY"));
	str_assign(&p->model,   getenv("ABMOD_OPENAI_MODEL"));
	str_assign(&p->ws_url,  getenv("ABMOD_OPENAI_WS_URL"));
	str_assign(&p->prompt,  getenv("ABMOD_OPENAI_PROMPT"));
	str_assign(&p->lang,    getenv("ABMOD_OPENAI_LANG"));
	str_assign(&p->noise_reduction, getenv("ABMOD_OPENAI_NOISE_REDUCTION"));
	str_assign(&p->vad_type, getenv("ABMOD_OPENAI_VAD_TYPE"));
	const char *env_v;
	if((env_v = getenv("ABMOD_OPENAI_VAD_THRESHOLD")))
		p->vad_threshold = (float)atof(env_v);
	if((env_v = getenv("ABMOD_OPENAI_VAD_PREFIX_PADDING_MS")))
		p->vad_prefix_padding_ms = atoi(env_v);
	if((env_v = getenv("ABMOD_OPENAI_VAD_SILENCE_DURATION_MS")))
		p->vad_silence_duration_ms = atoi(env_v);
	if((env_v = getenv("ABMOD_OPENAI_PRESENTATION_MODE")))
		p->presentation_mode = atoi(env_v) ? 1 : 0;
	if((env_v = getenv("ABMOD_OPENAI_FAST_MODE")))
		p->fast_mode = atoi(env_v) ? 1 : 0;
	if((env_v = getenv("ABMOD_OPENAI_CONCURRENT")))
		p->concurrent = atoi(env_v);
	if(!p->model)      str_assign(&p->model, "gpt-4o-transcribe");
	if(!p->ws_url)     str_assign(&p->ws_url,
		"wss://api.openai.com/v1/realtime?intent=transcription");
	if(p->concurrent <= 0) p->concurrent = 1;

	/* config_json overrides */
	if(config_json) {
		json_error_t err;
		json_t *cfg = json_loads(config_json, 0, &err);
		if(cfg && json_is_object(cfg)) {
			const char *s;
			json_t *j;
			STR_FIELD("openai_api_key",       api_key);
			STR_FIELD("openai_model",          model);
			STR_FIELD("openai_ws_url",         ws_url);
			STR_FIELD("openai_prompt",         prompt);
			STR_FIELD("openai_language",       lang);
			STR_FIELD("openai_noise_reduction",noise_reduction);
			STR_FIELD("openai_vad_type",       vad_type);
			if((j = json_object_get(cfg, "openai_vad_threshold")) && json_is_number(j))
				p->vad_threshold = (float)json_number_value(j);
			if((j = json_object_get(cfg, "openai_vad_prefix_padding_ms")) && json_is_integer(j))
				p->vad_prefix_padding_ms = (int)json_integer_value(j);
			if((j = json_object_get(cfg, "openai_vad_silence_duration_ms")) && json_is_integer(j))
				p->vad_silence_duration_ms = (int)json_integer_value(j);
			if((j = json_object_get(cfg, "openai_presentation_mode")))
				p->presentation_mode = json_is_true(j) ? 1 : 0;
			if((j = json_object_get(cfg, "openai_fast_mode")))
				p->fast_mode = json_is_true(j) ? 1 : 0;
			if((j = json_object_get(cfg, "openai_concurrent")) && json_is_integer(j)) {
				int c = (int)json_integer_value(j);
				if(c > 0) p->concurrent = c;
			}
			STR_FIELD("openai_embedding_model_path", embedding_model_path);
			STR_FIELD("openai_embedding_vocab_path", embedding_vocab_path);
			if((j = json_object_get(cfg, "openai_embedding_threshold")) && json_is_number(j))
				p->embedding_similarity_threshold = (float)json_number_value(j);
		}
		if(cfg) json_decref(cfg);
	}

	if(!p->api_key || strlen(p->api_key) < 10)
		openai_emit_error(p, "", "", "OPENAI_API_KEY missing/invalid");

	/* Embedding — env/config overrides first, then compile-time default paths.
	 * Loaded automatically on provider startup; no config needed if model
	 * files are present (installed via 'make download-models'). */
	str_assign(&p->embedding_model_path, getenv("ABMOD_OPENAI_EMBEDDING_MODEL"));
	str_assign(&p->embedding_vocab_path, getenv("ABMOD_OPENAI_EMBEDDING_VOCAB"));
	p->embedding_similarity_threshold = 0.5f;
	if((env_v = getenv("ABMOD_OPENAI_EMBEDDING_THRESHOLD")))
		p->embedding_similarity_threshold = (float)atof(env_v);
	/* Fall back to paths baked in at build time */
	if(!p->embedding_model_path)
		str_assign(&p->embedding_model_path,
			ABMOD_EMBEDDING_DEFAULT_DIR "/model.onnx");
	if(!p->embedding_vocab_path)
		str_assign(&p->embedding_vocab_path,
			ABMOD_EMBEDDING_DEFAULT_DIR "/vocab.txt");
	if(abmod_file_exists(p->embedding_model_path) &&
			abmod_file_exists(p->embedding_vocab_path)) {
		if(abmod_embedding_init(p->embedding_model_path, p->embedding_vocab_path,
				p->embedding_similarity_threshold) != 0)
			fprintf(stderr, "[openai] Embedding model failed to load — "
				"falling back to longest-text heuristic\n");
	} else {
		fprintf(stderr, "[openai] Embedding model not found at %s — "
			"run 'make download-models' to fetch it\n",
			p->embedding_model_path);
	}

	/* Start guard thread (needed even with concurrent=1 for future dynamic changes) */
	p->guard_running = 1;
	if(pthread_create(&p->guard_thread, NULL, openai_guard_thread_fn, p) != 0) {
		p->guard_running = 0;
		/* Non-fatal: guard won't fire, concurrent>1 finals will be silently dropped */
	}

	*out_impl  = p;
	*out_vtbl  = &OPENAI_VTBL;
	return 0;
}
