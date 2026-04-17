/*
 * OpenAI provider implementation for abmod_provider.
 *
 * - One WebSocket session per (room_id,user_id) stream.
 * - send_pcm() does downmix+resample to 24kHz mono and enqueues base64 chunks.
 * - libwebsockets thread performs network I/O and invokes transcript callbacks.
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>
#include <pthread.h>

#include <glib.h>
#include <jansson.h>
#include <libwebsockets.h>
#include "audiobridge-deps/speex/speex_resampler.h"

#include "abmod_provider_impl.h"

#define OPENAI_PROVIDER_NAME "openai"
#define OPENAI_INPUT_RATE 24000

typedef struct openai_msg_s {
	char *data;
	struct openai_msg_s *next;
} openai_msg;

typedef struct openai_stream_s openai_stream;

typedef struct openai_provider_s {
	GHashTable *streams;
	pthread_mutex_t mtx;
	abmod_provider_callbacks cbs;
	void *cb_user;
	/* Config */
	char *api_key;
	char *model;
	char *ws_url;
	char *prompt;
	char *lang;
	char *noise_reduction;
} openai_provider;

struct openai_stream_s {
	char *key;
	char *room_id;
	char *user_id;
	/* Backref */
	openai_provider *p;
	/* Resampler (mono in -> 24k) */
	SpeexResamplerState *resampler;
	uint32_t in_rate;
	/* WS */
	pthread_t th;
	int running;
	struct lws_context *lws_ctx;
	struct lws *wsi;
	int connected;
	pthread_mutex_t qmtx;
	openai_msg *qhead;
	openai_msg *qtail;
	/* URL parts */
	char host[128];
	int port;
	int use_ssl;
	char path[256];
	/* Headers */
	char auth_value[256];
	char beta_value[64];
	/* RX buffer */
	char *rx;
	size_t rx_len;
	size_t rx_cap;
	/* Partial accumulator */
	char *partial;
	size_t partial_len;
	size_t partial_cap;
};

static char *openai_build_key(const char *room_id, const char *user_id) {
	return g_strdup_printf("%s|%s", room_id ? room_id : "", user_id ? user_id : "");
}

static void str_assign(char **dst, const char *src) {
	if(*dst) { free(*dst); *dst = NULL; }
	if(src) *dst = strdup(src);
}

static void openai_emit_error(openai_provider *p, const char *room_id, const char *user_id, const char *msg) {
	if(p && p->cbs.on_error)
		p->cbs.on_error(p->cb_user, OPENAI_PROVIDER_NAME, room_id, user_id, msg ? msg : "OpenAI error");
}

static void openai_stream_enqueue(openai_stream *s, const char *json_text) {
	if(!s || !json_text)
		return;
	openai_msg *m = (openai_msg *)calloc(1, sizeof(*m));
	if(!m)
		return;
	m->data = strdup(json_text);
	if(!m->data) {
		free(m);
		return;
	}
	pthread_mutex_lock(&s->qmtx);
	if(s->qtail) s->qtail->next = m; else s->qhead = m;
	s->qtail = m;
	pthread_mutex_unlock(&s->qmtx);
	if(s->lws_ctx && s->wsi)
		lws_callback_on_writable(s->wsi);
}

static void openai_stream_send_session_update(openai_stream *s) {
	if(!s || !s->p)
		return;
	const char *model = s->p->model ? s->p->model : "gpt-4o-transcribe";
	const char *prompt = (s->p->prompt && *s->p->prompt) ? s->p->prompt : NULL;
	const char *lang = (s->p->lang && *s->p->lang) ? s->p->lang : "en";
	const char *noise = (s->p->noise_reduction && *s->p->noise_reduction) ? s->p->noise_reduction : NULL;

	char transcription_json[512];
	char noise_json[256] = "";
	if(prompt && *prompt) {
		char *pj = json_dumps(json_string(prompt), JSON_ENCODE_ANY);
		if(pj && lang && *lang) {
			snprintf(transcription_json, sizeof(transcription_json),
				"{\"model\":\"%s\",\"prompt\":%s,\"language\":\"%s\"}", model, pj, lang);
		} else if(pj) {
			snprintf(transcription_json, sizeof(transcription_json),
				"{\"model\":\"%s\",\"prompt\":%s}", model, pj);
		} else {
			snprintf(transcription_json, sizeof(transcription_json),
				"{\"model\":\"%s\"}", model);
		}
		if(pj) free(pj);
	} else if(lang && *lang) {
		snprintf(transcription_json, sizeof(transcription_json),
			"{\"model\":\"%s\",\"language\":\"%s\"}", model, lang);
	} else {
		snprintf(transcription_json, sizeof(transcription_json),
			"{\"model\":\"%s\"}", model);
	}
	if(noise) {
		snprintf(noise_json, sizeof(noise_json),
			",\"input_audio_noise_reduction\":{\"type\":\"%s\"}", noise);
	}

	char buf[2048];
	snprintf(buf, sizeof(buf),
		"{\"type\":\"transcription_session.update\",\"session\":{"
		"\"input_audio_format\":\"pcm16\","
		"\"input_audio_transcription\":%s,"
		"\"turn_detection\":{\"type\":\"server_vad\",\"threshold\":0.5,\"prefix_padding_ms\":300,\"silence_duration_ms\":300}%s"
		"}}",
		transcription_json,
		noise_json
	);
	openai_stream_enqueue(s, buf);
}

static void openai_stream_send_audio_append(openai_stream *s, const void *pcm16, size_t samples) {
	if(!s || !pcm16 || samples == 0)
		return;
	size_t bytes = samples * sizeof(int16_t);
	char *b64 = (char *)g_base64_encode((const guchar *)pcm16, bytes);
	if(!b64)
		return;
	size_t json_cap = strlen(b64) + 64;
	char *json = (char *)malloc(json_cap);
	if(!json) {
		g_free(b64);
		return;
	}
	snprintf(json, json_cap, "{\"type\":\"input_audio_buffer.append\",\"audio\":\"%s\"}", b64);
	openai_stream_enqueue(s, json);
	g_free(b64);
	free(json);
}

static void openai_stream_process_incoming(openai_stream *s, const char *msg, size_t len) {
	(void)len;
	if(!s || !msg || !s->p)
		return;
	json_error_t jerr;
	json_t *root = json_loads(msg, 0, &jerr);
	if(!root) {
		openai_emit_error(s->p, s->room_id, s->user_id, jerr.text);
		return;
	}
	const char *type = json_string_value(json_object_get(root, "type"));

	if(type && (strcmp(type, "response.delta") == 0 || strcmp(type, "transcription.delta") == 0)) {
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
			if(s->p->cbs.on_transcript)
				s->p->cbs.on_transcript(s->p->cb_user, OPENAI_PROVIDER_NAME, s->room_id, s->user_id, delta, 0);
		}
	} else if(type && (strcmp(type, "response.completed") == 0 || strcmp(type, "transcription.completed") == 0
			|| strcmp(type, "conversation.item.input_audio_transcription.completed") == 0)) {
		const char *txt = json_string_value(json_object_get(root, "transcript"));
		if(!txt || !*txt) txt = json_string_value(json_object_get(root, "text"));
		if(!txt || !*txt) {
			pthread_mutex_lock(&s->qmtx);
			txt = (s->partial_len > 0 && s->partial) ? s->partial : NULL;
			pthread_mutex_unlock(&s->qmtx);
		}
		if(txt && *txt && s->p->cbs.on_transcript)
			s->p->cbs.on_transcript(s->p->cb_user, OPENAI_PROVIDER_NAME, s->room_id, s->user_id, txt, 1);
		pthread_mutex_lock(&s->qmtx);
		s->partial_len = 0;
		pthread_mutex_unlock(&s->qmtx);
	} else if(type && strcmp(type, "error") == 0) {
		const char *err_msg = json_string_value(json_object_get(json_object_get(root, "error"), "message"));
		openai_emit_error(s->p, s->room_id, s->user_id, err_msg ? err_msg : "OpenAI error");
	}
	json_decref(root);
}

static int openai_lws_callback(struct lws *wsi, enum lws_callback_reasons reason,
		void *user, void *in, size_t len) {
	(void)user;
	openai_stream *s = (openai_stream *)lws_context_user(lws_get_context(wsi));
	switch(reason) {
		case LWS_CALLBACK_CLIENT_ESTABLISHED:
			s->wsi = wsi;
			s->connected = 1;
			openai_stream_send_session_update(s);
			lws_callback_on_writable(wsi);
			break;
		case LWS_CALLBACK_CLIENT_CONNECTION_ERROR:
			s->connected = 0;
			s->wsi = NULL;
			openai_emit_error(s->p, s->room_id, s->user_id, in ? (const char *)in : "connection error");
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
					(const unsigned char *)s->auth_value, strlen(s->auth_value), p, end))
				return -1;
			if(lws_add_http_header_by_name(wsi, (const unsigned char *)"openai-beta:",
					(const unsigned char *)s->beta_value, strlen(s->beta_value), p, end))
				return -1;
			break;
		}
		case LWS_CALLBACK_CLIENT_RECEIVE: {
			if(len == 0)
				break;
			if(s->rx_len + len + 1 > s->rx_cap) {
				s->rx_cap = (s->rx_len + len + 1) * 2;
				s->rx = (char *)realloc(s->rx, s->rx_cap);
			}
			if(!s->rx)
				break;
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

static void openai_parse_url(openai_stream *s, const char *url) {
	/* Defaults */
	snprintf(s->host, sizeof(s->host), "%s", "api.openai.com");
	s->port = 443;
	s->use_ssl = 1;
	snprintf(s->path, sizeof(s->path), "%s", "/v1/realtime?intent=transcription");

	if(!url || !*url)
		return;
	s->use_ssl = (strncmp(url, "wss://", 6) == 0);
	int offset = s->use_ssl ? 6 : (strncmp(url, "ws://", 5) == 0 ? 5 : 0);
	const char *host_start = url + offset;
	const char *slash = strchr(host_start, '/');
	size_t host_len = slash ? (size_t)(slash - host_start) : strlen(host_start);
	const char *colon = NULL;
	for(size_t i = 0; i < host_len; ++i) {
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

static void *openai_stream_thread(void *arg) {
	openai_stream *s = (openai_stream *)arg;
	struct lws_context_creation_info info;
	memset(&info, 0, sizeof(info));
	info.port = CONTEXT_PORT_NO_LISTEN;
	info.protocols = openai_protocols;
	info.options = LWS_SERVER_OPTION_DO_SSL_GLOBAL_INIT;
	info.user = s;
	s->lws_ctx = lws_create_context(&info);
	if(!s->lws_ctx) {
		openai_emit_error(s->p, s->room_id, s->user_id, "Failed to create lws context");
		return NULL;
	}
	while(s->running) {
		if(!s->connected && s->wsi == NULL) {
			struct lws_client_connect_info ci;
			memset(&ci, 0, sizeof(ci));
			ci.context = s->lws_ctx;
			ci.address = s->host;
			ci.port = s->port;
			ci.path = s->path;
			ci.host = ci.address;
			ci.origin = ci.address;
			ci.protocol = NULL;
			ci.local_protocol_name = "openai-realtime";
			ci.ssl_connection = s->use_ssl ? LCCSCF_USE_SSL : 0;
			ci.alpn = "http/1.1";
			ci.pwsi = &s->wsi;
			ci.userdata = s;
			if(!lws_client_connect_via_info(&ci)) {
				/* Backoff a bit */
				lws_service(s->lws_ctx, 100);
			}
		}
		lws_service(s->lws_ctx, 10);
	}
	if(s->lws_ctx) {
		lws_context_destroy(s->lws_ctx);
		s->lws_ctx = NULL;
	}
	return NULL;
}

static openai_stream *openai_stream_create(openai_provider *p,
		const char *room_id,
		const char *user_id,
		uint32_t sample_rate,
		int channels) {
	(void)channels;
	openai_stream *s = (openai_stream *)calloc(1, sizeof(*s));
	if(!s)
		return NULL;
	s->p = p;
	s->room_id = strdup(room_id ? room_id : "");
	s->user_id = strdup(user_id ? user_id : "");
	s->key = openai_build_key(room_id, user_id);
	pthread_mutex_init(&s->qmtx, NULL);
	s->rx_cap = 65536;
	s->rx = (char *)malloc(s->rx_cap);
	s->partial_cap = 4096;
	s->partial = (char *)malloc(s->partial_cap);
	s->in_rate = sample_rate ? sample_rate : 48000;
	int err = 0;
	s->resampler = speex_resampler_init(1, s->in_rate, OPENAI_INPUT_RATE, SPEEX_RESAMPLER_QUALITY_VOIP, &err);
	if(!s->resampler || err != RESAMPLER_ERR_SUCCESS) {
		openai_emit_error(p, room_id, user_id, "Failed to init resampler");
	}
	const char *url = p->ws_url ? p->ws_url : "wss://api.openai.com/v1/realtime?intent=transcription";
	openai_parse_url(s, url);
	snprintf(s->auth_value, sizeof(s->auth_value), "Bearer %s", p->api_key ? p->api_key : "");
	snprintf(s->beta_value, sizeof(s->beta_value), "realtime=v1");
	s->running = 1;
	if(pthread_create(&s->th, NULL, openai_stream_thread, s) != 0) {
		s->running = 0;
		return s;
	}
	return s;
}

static void openai_stream_destroy(openai_stream *s) {
	if(!s)
		return;
	s->running = 0;
	if(s->th)
		pthread_join(s->th, NULL);
	pthread_mutex_lock(&s->qmtx);
	openai_msg *m = s->qhead;
	while(m) {
		openai_msg *n = m->next;
		free(m->data);
		free(m);
		m = n;
	}
	s->qhead = s->qtail = NULL;
	pthread_mutex_unlock(&s->qmtx);
	pthread_mutex_destroy(&s->qmtx);
	if(s->resampler) speex_resampler_destroy(s->resampler);
	free(s->rx);
	free(s->partial);
	g_free(s->key);
	free(s->room_id);
	free(s->user_id);
	free(s);
}

static void openai_provider_destroy(void *vimpl) {
	openai_provider *p = (openai_provider *)vimpl;
	if(!p)
		return;
	pthread_mutex_lock(&p->mtx);
	GHashTableIter iter;
	gpointer key = NULL, val = NULL;
	g_hash_table_iter_init(&iter, p->streams);
	while(g_hash_table_iter_next(&iter, &key, &val)) {
		openai_stream_destroy((openai_stream *)val);
		g_hash_table_iter_remove(&iter);
	}
	pthread_mutex_unlock(&p->mtx);
	g_hash_table_destroy(p->streams);
	pthread_mutex_destroy(&p->mtx);
	free(p->api_key);
	free(p->model);
	free(p->ws_url);
	free(p->prompt);
	free(p->lang);
	free(p->noise_reduction);
	free(p);
}

static int openai_provider_open_user_stream(void *vimpl,
		const char *room_id,
		const char *user_id,
		uint32_t sample_rate,
		int channels) {
	openai_provider *p = (openai_provider *)vimpl;
	if(!p || !room_id || !user_id)
		return -1;
	char *key = openai_build_key(room_id, user_id);
	pthread_mutex_lock(&p->mtx);
	openai_stream *s = (openai_stream *)g_hash_table_lookup(p->streams, key);
	if(s) {
		pthread_mutex_unlock(&p->mtx);
		g_free(key);
		return 0;
	}
	s = openai_stream_create(p, room_id, user_id, sample_rate, channels);
	if(!s) {
		pthread_mutex_unlock(&p->mtx);
		g_free(key);
		return -1;
	}
	g_hash_table_insert(p->streams, g_strdup(key), s);
	pthread_mutex_unlock(&p->mtx);
	g_free(key);
	return 0;
}

static int openai_provider_send_pcm(void *vimpl,
		const char *room_id,
		const char *user_id,
		const int16_t *pcm,
		size_t samples,
		uint32_t sample_rate,
		int channels) {
	(void)sample_rate;
	openai_provider *p = (openai_provider *)vimpl;
	if(!p || !room_id || !user_id || !pcm || samples == 0)
		return -1;
	char *key = openai_build_key(room_id, user_id);
	pthread_mutex_lock(&p->mtx);
	openai_stream *s = (openai_stream *)g_hash_table_lookup(p->streams, key);
	pthread_mutex_unlock(&p->mtx);
	g_free(key);
	if(!s || !s->connected || !s->resampler)
		return -1;

	/* Downmix to mono */
	size_t in_mono = channels == 2 ? (samples / 2) : samples;
	int16_t *mono = (int16_t *)malloc(in_mono * sizeof(int16_t));
	if(!mono)
		return -1;
	if(channels == 2) {
		for(size_t k = 0; k < in_mono; ++k) {
			int32_t l = pcm[2 * k];
			int32_t r = pcm[2 * k + 1];
			mono[k] = (int16_t)((l + r) / 2);
		}
	} else {
		memcpy(mono, pcm, in_mono * sizeof(int16_t));
	}

	/* Resample to 24k */
	spx_uint32_t in_len = (spx_uint32_t)in_mono;
	/* Conservative output cap */
	size_t out_cap = (size_t)((double)in_mono * (double)OPENAI_INPUT_RATE / (double)(s->in_rate ? s->in_rate : 48000) + 64);
	int16_t *out = (int16_t *)malloc(out_cap * sizeof(int16_t));
	if(!out) {
		free(mono);
		return -1;
	}
	spx_uint32_t out_len = (spx_uint32_t)out_cap;
	speex_resampler_process_int(s->resampler, 0, mono, &in_len, out, &out_len);
	free(mono);

	if(out_len > 0)
		openai_stream_send_audio_append(s, out, out_len);
	free(out);
	return 0;
}

static int openai_provider_close_user_stream(void *vimpl,
		const char *room_id,
		const char *user_id) {
	openai_provider *p = (openai_provider *)vimpl;
	if(!p || !room_id || !user_id)
		return -1;
	char *key = openai_build_key(room_id, user_id);
	pthread_mutex_lock(&p->mtx);
	openai_stream *s = (openai_stream *)g_hash_table_lookup(p->streams, key);
	if(s)
		g_hash_table_remove(p->streams, key);
	pthread_mutex_unlock(&p->mtx);
	g_free(key);
	if(s)
		openai_stream_destroy(s);
	return 0;
}

static const abmod_provider_vtbl OPENAI_VTBL = {
	.destroy = openai_provider_destroy,
	.open_user_stream = openai_provider_open_user_stream,
	.send_pcm = openai_provider_send_pcm,
	.close_user_stream = openai_provider_close_user_stream
};

int abmod_provider_openai_init(const char *config_json,
		const abmod_provider_callbacks *cbs,
		void *cb_user,
		void **out_impl,
		const abmod_provider_vtbl **out_vtbl) {
	if(!out_impl || !out_vtbl)
		return -1;
	*out_impl = NULL;
	*out_vtbl = NULL;

	openai_provider *p = (openai_provider *)calloc(1, sizeof(*p));
	if(!p)
		return -1;
	p->streams = g_hash_table_new_full(g_str_hash, g_str_equal, g_free, NULL);
	pthread_mutex_init(&p->mtx, NULL);
	if(cbs)
		p->cbs = *cbs;
	p->cb_user = cb_user;

	/* Env defaults */
	str_assign(&p->api_key, getenv("OPENAI_API_KEY"));
	str_assign(&p->model, getenv("ABMOD_OPENAI_MODEL"));
	str_assign(&p->ws_url, getenv("ABMOD_OPENAI_WS_URL"));
	str_assign(&p->prompt, getenv("ABMOD_OPENAI_PROMPT"));
	str_assign(&p->lang, getenv("ABMOD_OPENAI_LANG"));
	str_assign(&p->noise_reduction, getenv("ABMOD_OPENAI_NOISE_REDUCTION"));
	if(!p->model) str_assign(&p->model, "gpt-4o-transcribe");
	if(!p->ws_url) str_assign(&p->ws_url, "wss://api.openai.com/v1/realtime?intent=transcription");

	/* Optional config_json overrides */
	if(config_json) {
		json_error_t err;
		json_t *cfg = json_loads(config_json, 0, &err);
		if(cfg && json_is_object(cfg)) {
			const char *api_key = json_string_value(json_object_get(cfg, "openai_api_key"));
			const char *model = json_string_value(json_object_get(cfg, "openai_model"));
			const char *ws_url = json_string_value(json_object_get(cfg, "openai_ws_url"));
			const char *prompt = json_string_value(json_object_get(cfg, "openai_prompt"));
			const char *lang = json_string_value(json_object_get(cfg, "openai_language"));
			const char *noise = json_string_value(json_object_get(cfg, "openai_noise_reduction"));
			if(api_key && *api_key) str_assign(&p->api_key, api_key);
			if(model && *model) str_assign(&p->model, model);
			if(ws_url && *ws_url) str_assign(&p->ws_url, ws_url);
			if(prompt && *prompt) str_assign(&p->prompt, prompt);
			if(lang && *lang) str_assign(&p->lang, lang);
			if(noise && *noise) str_assign(&p->noise_reduction, noise);
		}
		if(cfg) json_decref(cfg);
	}

	if(!p->api_key || strlen(p->api_key) < 10) {
		openai_emit_error(p, "", "", "OPENAI_API_KEY missing/invalid");
	}

	*out_impl = p;
	*out_vtbl = &OPENAI_VTBL;
	return 0;
}

