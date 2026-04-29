#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>
#include <math.h>
#include <pthread.h>

#include <deque>
#include <string>
#include <unordered_map>
#include <vector>
#include <array>

#include <glib.h>
#include <jansson.h>

#include "janus_ab_module.h"

#if __has_include(<onnxruntime_cxx_api.h>)
#include <onnxruntime_cxx_api.h>
#else
#include <onnxruntime/core/session/onnxruntime_cxx_api.h>
#endif

#ifndef ABMOD_GENDER_DEFAULT_MODEL
#define ABMOD_GENDER_DEFAULT_MODEL "/usr/share/janus/models/gender/model.onnx"
#endif

#define ABMOD_GENDER_LOG(fmt, ...) \
	fprintf(stderr, "[ABMod][gender] " fmt "\n", ##__VA_ARGS__)

extern "C" {
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
}

struct abmod_pcm_item {
	std::string room_id;
	std::string user_id;
	std::vector<int16_t> pcm;
	uint32_t sampling_rate;
	int channels;
};

struct abmod_evt_item {
	std::string room_id;
	std::string user_id;
	std::string event_name;
};

struct abmod_queue_item {
	int type; /* 1=pcm 2=event */
	abmod_pcm_item pcm;
	abmod_evt_item evt;
};

struct abmod_user_state {
	std::vector<float> mono16k;
	uint64_t last_infer_us;
};

struct abmod_ctx {
	uint32_t room_rate = 0;
	int room_channels = 0;
	char *config = NULL;
	janus_abmod_callbacks cbs;
	void *user = NULL;

	std::string model_path;
	int target_rate;
	size_t window_samples;
	float min_confidence;
	int min_emit_interval_ms;
	size_t max_user_buffer_samples;
	size_t queue_capacity;

	pthread_mutex_t lock;
	pthread_cond_t cv;
	int running = 0;
	pthread_t worker;
	std::deque<abmod_queue_item> queue;
	std::unordered_map<std::string, abmod_user_state> users;

	Ort::Env *env = NULL;
	Ort::SessionOptions *opts = NULL;
	Ort::Session *session = NULL;
	Ort::AllocatorWithDefaultOptions *alloc = NULL;
	bool has_input_values = false;
	std::string input_name;
	std::string output_name;
};

static std::string abmod_key(const std::string &room_id, const std::string &user_id) {
	return room_id + "|" + user_id;
}

static void abmod_emit(abmod_ctx *ctx, const char *event_name, json_t *payload) {
	if(!ctx || !event_name || !payload || !ctx->cbs.emit_event)
		return;
	char *text = json_dumps(payload, JSON_COMPACT);
	if(text) {
		ctx->cbs.emit_event(ctx->cbs.emit_event_user, event_name, text);
		free(text);
	}
}

static std::vector<float> abmod_downmix_resample_to_mono16k(const int16_t *pcm, size_t samples,
		uint32_t input_rate, int channels, int target_rate) {
	std::vector<float> mono;
	if(!pcm || samples == 0 || channels <= 0 || input_rate == 0 || target_rate <= 0)
		return mono;
	size_t frames = samples / (size_t)channels;
	if(frames == 0)
		return mono;
	std::vector<float> in(frames, 0.0f);
	for(size_t i = 0; i < frames; ++i) {
		float acc = 0.0f;
		for(int ch = 0; ch < channels; ++ch)
			acc += (float)pcm[i * (size_t)channels + (size_t)ch];
		in[i] = (acc / (float)channels) / 32768.0f;
	}
	double ratio = (double)target_rate / (double)input_rate;
	size_t out_frames = (size_t)((double)frames * ratio);
	if(out_frames == 0)
		return mono;
	mono.resize(out_frames);
	if(frames == 1) {
		for(size_t i = 0; i < out_frames; ++i)
			mono[i] = in[0];
		return mono;
	}
	for(size_t i = 0; i < out_frames; ++i) {
		double src = (double)i / ratio;
		size_t i0 = (size_t)floor(src);
		size_t i1 = i0 + 1;
		if(i1 >= frames)
			i1 = frames - 1;
		double frac = src - (double)i0;
		mono[i] = (float)((1.0 - frac) * in[i0] + frac * in[i1]);
	}
	return mono;
}

static bool abmod_init_onnx(abmod_ctx *ctx) {
	if(!ctx)
		return false;
	try {
		ctx->env = new Ort::Env(ORT_LOGGING_LEVEL_WARNING, "abmod_gender");
		ctx->opts = new Ort::SessionOptions();
		ctx->opts->SetIntraOpNumThreads(1);
		ctx->opts->SetGraphOptimizationLevel(GraphOptimizationLevel::ORT_ENABLE_ALL);
#ifdef _WIN32
		std::wstring wpath(ctx->model_path.begin(), ctx->model_path.end());
		ctx->session = new Ort::Session(*ctx->env, wpath.c_str(), *ctx->opts);
#else
		ctx->session = new Ort::Session(*ctx->env, ctx->model_path.c_str(), *ctx->opts);
#endif
		ctx->alloc = new Ort::AllocatorWithDefaultOptions();
		size_t n_inputs = ctx->session->GetInputCount();
		ctx->has_input_values = false;
		for(size_t i = 0; i < n_inputs; ++i) {
			auto name = ctx->session->GetInputNameAllocated(i, *ctx->alloc);
			std::string n(name.get());
			if(n == "input_values" || n == "audio" || n == "input") {
				ctx->has_input_values = true;
				ctx->input_name = n;
				break;
			}
		}
		if(!ctx->has_input_values && n_inputs > 0) {
			auto name = ctx->session->GetInputNameAllocated(0, *ctx->alloc);
			ctx->input_name = std::string(name.get());
			ctx->has_input_values = true;
		}
		size_t n_outputs = ctx->session->GetOutputCount();
		if(n_outputs > 0) {
			auto oname = ctx->session->GetOutputNameAllocated(0, *ctx->alloc);
			ctx->output_name = std::string(oname.get());
		}
		if(!ctx->has_input_values || ctx->output_name.empty()) {
			ABMOD_GENDER_LOG("model IO discovery failed (input='%s' output='%s')",
				ctx->input_name.c_str(), ctx->output_name.c_str());
			return false;
		}
		ABMOD_GENDER_LOG("loaded model=%s input=%s output=%s",
			ctx->model_path.c_str(), ctx->input_name.c_str(), ctx->output_name.c_str());
		return true;
	} catch(const Ort::Exception &e) {
		ABMOD_GENDER_LOG("onnx init failed: %s", e.what());
		return false;
	}
}

static void abmod_deinit_onnx(abmod_ctx *ctx) {
	if(!ctx)
		return;
	delete ctx->session;
	ctx->session = NULL;
	delete ctx->opts;
	ctx->opts = NULL;
	delete ctx->env;
	ctx->env = NULL;
	delete ctx->alloc;
	ctx->alloc = NULL;
}

static bool abmod_run_infer(abmod_ctx *ctx, const std::vector<float> &mono16k,
		float *male_prob_out, float *female_prob_out) {
	if(!ctx || !ctx->session || !male_prob_out || !female_prob_out || mono16k.empty())
		return false;
	try {
		std::array<int64_t, 2> shape = {1, (int64_t)mono16k.size()};
		Ort::MemoryInfo mem = Ort::MemoryInfo::CreateCpu(OrtArenaAllocator, OrtMemTypeDefault);
		Ort::Value input = Ort::Value::CreateTensor<float>(mem,
			const_cast<float *>(mono16k.data()), mono16k.size(), shape.data(), 2);
		const char *in_name = ctx->input_name.c_str();
		const char *out_name = ctx->output_name.c_str();
		auto outputs = ctx->session->Run(Ort::RunOptions{nullptr},
			&in_name, &input, 1, &out_name, 1);
		if(outputs.empty())
			return false;
		float *logits = outputs[0].GetTensorMutableData<float>();
		auto info = outputs[0].GetTensorTypeAndShapeInfo();
		std::vector<int64_t> out_shape = info.GetShape();
		size_t out_elems = info.GetElementCount();
		if(out_elems < 2)
			return false;
		float l0 = logits[0];
		float l1 = logits[1];
		float m = l0 > l1 ? l0 : l1;
		float e0 = expf(l0 - m);
		float e1 = expf(l1 - m);
		float s = e0 + e1;
		float p0 = e0 / s;
		float p1 = e1 / s;
		/* Convention: class0=female, class1=male */
		*female_prob_out = p0;
		*male_prob_out = p1;
		return true;
	} catch(const Ort::Exception &e) {
		ABMOD_GENDER_LOG("inference failed: %s", e.what());
		return false;
	}
}

static void abmod_handle_user_infer(abmod_ctx *ctx, const std::string &room_id, const std::string &user_id) {
	if(!ctx)
		return;
	const std::string key = abmod_key(room_id, user_id);
	auto it = ctx->users.find(key);
	if(it == ctx->users.end())
		return;
	abmod_user_state &st = it->second;
	if(st.mono16k.size() < ctx->window_samples)
		return;
	uint64_t now_us = (uint64_t)g_get_real_time();
	if(st.last_infer_us != 0 && now_us - st.last_infer_us < (uint64_t)ctx->min_emit_interval_ms * 1000ULL)
		return;
	std::vector<float> window(st.mono16k.end() - (ptrdiff_t)ctx->window_samples, st.mono16k.end());
	float male_p = 0.0f, female_p = 0.0f;
	if(!abmod_run_infer(ctx, window, &male_p, &female_p))
		return;
	const char *label = male_p >= female_p ? "male" : "female";
	float confidence = male_p >= female_p ? male_p : female_p;
	if(confidence < ctx->min_confidence)
		return;
	json_t *payload = json_object();
	json_object_set_new(payload, "room_id", json_string(room_id.c_str()));
	json_object_set_new(payload, "user_id", json_string(user_id.c_str()));
	json_object_set_new(payload, "type", json_string("gender"));
	json_object_set_new(payload, "label", json_string(label));
	json_object_set_new(payload, "confidence", json_real(confidence));
	json_object_set_new(payload, "male_prob", json_real(male_p));
	json_object_set_new(payload, "female_prob", json_real(female_p));
	json_object_set_new(payload, "window_ms", json_integer((json_int_t)((ctx->window_samples * 1000) / (size_t)ctx->target_rate)));
	json_object_set_new(payload, "sample_rate", json_integer(ctx->target_rate));
	json_object_set_new(payload, "model_path", json_string(ctx->model_path.c_str()));
	json_object_set_new(payload, "ts_us", json_integer((json_int_t)now_us));
	abmod_emit(ctx, "gender", payload);
	json_decref(payload);
	st.last_infer_us = now_us;
}

static bool abmod_queue_push(abmod_ctx *ctx, const abmod_queue_item &item) {
	if(!ctx)
		return false;
	pthread_mutex_lock(&ctx->lock);
	if(ctx->queue.size() >= ctx->queue_capacity) {
		if(item.type == 2) {
			for(auto it = ctx->queue.begin(); it != ctx->queue.end(); ++it) {
				if(it->type == 1) {
					ctx->queue.erase(it);
					break;
				}
			}
		}
	}
	if(ctx->queue.size() >= ctx->queue_capacity) {
		pthread_mutex_unlock(&ctx->lock);
		return false;
	}
	ctx->queue.push_back(item);
	pthread_cond_signal(&ctx->cv);
	pthread_mutex_unlock(&ctx->lock);
	return true;
}

static void *abmod_worker(void *arg) {
	abmod_ctx *ctx = (abmod_ctx *)arg;
	while(1) {
		abmod_queue_item item;
		bool has_item = false;
		pthread_mutex_lock(&ctx->lock);
		while(ctx->running && ctx->queue.empty())
			pthread_cond_wait(&ctx->cv, &ctx->lock);
		if(!ctx->running && ctx->queue.empty()) {
			pthread_mutex_unlock(&ctx->lock);
			break;
		}
		if(!ctx->queue.empty()) {
			item = std::move(ctx->queue.front());
			ctx->queue.pop_front();
			has_item = true;
		}
		pthread_mutex_unlock(&ctx->lock);
		if(!has_item)
			continue;

		if(item.type == 1) {
			std::vector<float> mono = abmod_downmix_resample_to_mono16k(item.pcm.pcm.data(),
				item.pcm.pcm.size(), item.pcm.sampling_rate, item.pcm.channels, ctx->target_rate);
			if(mono.empty())
				continue;
			std::string key = abmod_key(item.pcm.room_id, item.pcm.user_id);
			abmod_user_state &st = ctx->users[key];
			st.mono16k.insert(st.mono16k.end(), mono.begin(), mono.end());
			if(st.mono16k.size() > ctx->max_user_buffer_samples) {
				size_t trim = st.mono16k.size() - ctx->max_user_buffer_samples;
				st.mono16k.erase(st.mono16k.begin(), st.mono16k.begin() + (ptrdiff_t)trim);
			}
			abmod_handle_user_infer(ctx, item.pcm.room_id, item.pcm.user_id);
		} else if(item.type == 2) {
			if(item.evt.event_name == "left") {
				std::string key = abmod_key(item.evt.room_id, item.evt.user_id);
				ctx->users.erase(key);
			}
		}
	}
	return NULL;
}

static void abmod_parse_config(abmod_ctx *ctx, const char *config_json) {
	ctx->model_path = ABMOD_GENDER_DEFAULT_MODEL;
	ctx->target_rate = 16000;
	ctx->window_samples = 3 * 16000;
	ctx->min_confidence = 0.60f;
	ctx->min_emit_interval_ms = 1500;
	ctx->max_user_buffer_samples = 8 * 16000;
	ctx->queue_capacity = 512;
	if(!config_json)
		return;
	json_error_t err;
	json_t *root = json_loads(config_json, 0, &err);
	if(!root || !json_is_object(root)) {
		if(root)
			json_decref(root);
		return;
	}
	const char *model_path = json_string_value(json_object_get(root, "model_path"));
	if(model_path && *model_path)
		ctx->model_path = model_path;
	json_t *window_ms = json_object_get(root, "window_ms");
	if(window_ms && json_is_integer(window_ms)) {
		json_int_t w = json_integer_value(window_ms);
		if(w >= 1000 && w <= 10000)
			ctx->window_samples = (size_t)((w * ctx->target_rate) / 1000);
	}
	json_t *min_conf = json_object_get(root, "min_confidence");
	if(min_conf && (json_is_real(min_conf) || json_is_integer(min_conf))) {
		double mc = json_number_value(min_conf);
		if(mc >= 0.0 && mc <= 1.0)
			ctx->min_confidence = (float)mc;
	}
	json_t *emit_ms = json_object_get(root, "emit_interval_ms");
	if(emit_ms && json_is_integer(emit_ms)) {
		json_int_t ems = json_integer_value(emit_ms);
		if(ems >= 250 && ems <= 10000)
			ctx->min_emit_interval_ms = (int)ems;
	}
	json_t *max_buf_ms = json_object_get(root, "max_buffer_ms");
	if(max_buf_ms && json_is_integer(max_buf_ms)) {
		json_int_t b = json_integer_value(max_buf_ms);
		if(b >= 2000 && b <= 30000)
			ctx->max_user_buffer_samples = (size_t)((b * ctx->target_rate) / 1000);
	}
	json_decref(root);
}

extern "C" void *abmod_create(uint32_t sampling_rate, int channels, const char *config_json,
		const janus_abmod_callbacks *cbs, void *user) {
	abmod_ctx *ctx = new abmod_ctx();
	ctx->room_rate = sampling_rate;
	ctx->room_channels = channels;
	ctx->cbs.emit_event = NULL;
	ctx->cbs.emit_event_user = NULL;
	ctx->config = config_json ? g_strdup(config_json) : NULL;
	if(cbs)
		ctx->cbs = *cbs;
	ctx->user = user;
	pthread_mutex_init(&ctx->lock, NULL);
	pthread_cond_init(&ctx->cv, NULL);
	abmod_parse_config(ctx, config_json);
	if(!abmod_init_onnx(ctx)) {
		ABMOD_GENDER_LOG("create failed: unable to load ONNX model '%s'", ctx->model_path.c_str());
		pthread_cond_destroy(&ctx->cv);
		pthread_mutex_destroy(&ctx->lock);
		g_free(ctx->config);
		delete ctx;
		return NULL;
	}
	ctx->running = 1;
	if(pthread_create(&ctx->worker, NULL, abmod_worker, ctx) != 0) {
		ABMOD_GENDER_LOG("create failed: pthread_create");
		ctx->running = 0;
		abmod_deinit_onnx(ctx);
		pthread_cond_destroy(&ctx->cv);
		pthread_mutex_destroy(&ctx->lock);
		g_free(ctx->config);
		delete ctx;
		return NULL;
	}
	ABMOD_GENDER_LOG("ready model=%s window_samples=%zu min_confidence=%.2f",
		ctx->model_path.c_str(), ctx->window_samples, ctx->min_confidence);
	return ctx;
}

extern "C" void abmod_destroy(void *vctx) {
	abmod_ctx *ctx = (abmod_ctx *)vctx;
	if(!ctx)
		return;
	pthread_mutex_lock(&ctx->lock);
	ctx->running = 0;
	pthread_cond_broadcast(&ctx->cv);
	pthread_mutex_unlock(&ctx->lock);
	pthread_join(ctx->worker, NULL);
	abmod_deinit_onnx(ctx);
	pthread_cond_destroy(&ctx->cv);
	pthread_mutex_destroy(&ctx->lock);
	g_free(ctx->config);
	delete ctx;
}

extern "C" void abmod_on_mix(void *vctx, const int16_t *pcm, size_t samples,
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
	/* intentionally unused: this module focuses on per-participant attribution */
}

extern "C" void abmod_on_event(void *vctx, const char *event_name,
		const char *room_id, const char *user_id, int64_t event_time_us,
		uint64_t talk_version) {
	(void)event_time_us;
	(void)talk_version;
	abmod_ctx *ctx = (abmod_ctx *)vctx;
	if(!ctx || !event_name || !room_id || !user_id)
		return;
	abmod_queue_item item;
	item.type = 2;
	item.evt.event_name = event_name;
	item.evt.room_id = room_id;
	item.evt.user_id = user_id;
	abmod_queue_push(ctx, item);
}

extern "C" void abmod_on_participant_pcm(void *vctx, const char *room_id, const char *user_id,
		const int16_t *pcm, size_t samples, uint32_t sampling_rate, int channels,
		uint32_t rtp_timestamp, uint64_t frame_seq, uint64_t active_talk_version) {
	(void)rtp_timestamp;
	(void)frame_seq;
	(void)active_talk_version;
	abmod_ctx *ctx = (abmod_ctx *)vctx;
	if(!ctx || !room_id || !user_id || !pcm || samples == 0)
		return;
	abmod_queue_item item;
	item.type = 1;
	item.pcm.room_id = room_id;
	item.pcm.user_id = user_id;
	item.pcm.pcm.assign(pcm, pcm + samples);
	item.pcm.sampling_rate = sampling_rate;
	item.pcm.channels = channels;
	abmod_queue_push(ctx, item);
}
