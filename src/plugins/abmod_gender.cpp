#include "abmod_gender.h"

#if __has_include(<onnxruntime_cxx_api.h>)
#include <onnxruntime_cxx_api.h>
#else
#include <onnxruntime/core/session/onnxruntime_cxx_api.h>
#endif

#include <algorithm>
#include <array>
#include <cctype>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <mutex>
#include <string>
#include <unordered_map>
#include <vector>

#include <jansson.h>

#ifndef ABMOD_GENDER_DEFAULT_MODEL
#define ABMOD_GENDER_DEFAULT_MODEL "/usr/share/janus/models/gender/model.onnx"
#endif

struct GenderState {
	bool attempted = false;
	bool known = false;
	bool trigger_wait_logged = false;
	std::string label;
	float confidence = 0.0f;
	std::vector<float> mono16k;
};

struct abmod_gender_engine {
	std::mutex mtx;
	bool enabled = true;
	bool ready = false;
	std::string model_path = ABMOD_GENDER_DEFAULT_MODEL;
	int target_rate = 16000;
	size_t window_samples = 3 * 16000;
	float min_confidence = 0.60f;
	size_t max_buffer_samples = 6 * 16000;
	size_t trigger_min_chars = 4;
	bool trigger_final_only = true;
	float trigger_min_transcript_confidence = 0.50f;
	bool trigger_allow_unknown_confidence = false;

	Ort::Env *env = nullptr;
	Ort::SessionOptions *opts = nullptr;
	Ort::Session *session = nullptr;
	Ort::AllocatorWithDefaultOptions *alloc = nullptr;
	std::string input_name;
	std::string output_name;
	std::unordered_map<std::string, GenderState> users;
};

static std::string key_for(const char *room_id, const char *user_id) {
	return std::string(room_id ? room_id : "") + "|" + std::string(user_id ? user_id : "");
}

static std::vector<float> to_mono_16k(const int16_t *pcm, size_t samples,
		uint32_t input_rate, int channels, int target_rate) {
	std::vector<float> out;
	if(!pcm || samples == 0 || input_rate == 0 || channels <= 0 || target_rate <= 0)
		return out;
	size_t frames = samples / (size_t)channels;
	if(frames == 0)
		return out;
	std::vector<float> mono(frames, 0.0f);
	for(size_t i = 0; i < frames; ++i) {
		float s = 0.0f;
		for(int c = 0; c < channels; ++c)
			s += (float)pcm[i * (size_t)channels + (size_t)c];
		mono[i] = (s / (float)channels) / 32768.0f;
	}
	double ratio = (double)target_rate / (double)input_rate;
	size_t out_frames = (size_t)((double)frames * ratio);
	if(out_frames == 0)
		return out;
	out.resize(out_frames);
	if(frames == 1) {
		std::fill(out.begin(), out.end(), mono[0]);
		return out;
	}
	for(size_t i = 0; i < out_frames; ++i) {
		double src = (double)i / ratio;
		size_t i0 = (size_t)floor(src);
		size_t i1 = i0 + 1;
		if(i1 >= frames)
			i1 = frames - 1;
		double frac = src - (double)i0;
		out[i] = (float)((1.0 - frac) * mono[i0] + frac * mono[i1]);
	}
	return out;
}

static bool parse_config(abmod_gender_engine *e, const char *config_json) {
	if(!e)
		return false;
	if(!config_json)
		return true;
	json_error_t jerr;
	json_t *cfg = json_loads(config_json, 0, &jerr);
	if(!cfg || !json_is_object(cfg)) {
		if(cfg)
			json_decref(cfg);
		return true;
	}
	json_t *enabled = json_object_get(cfg, "gender_enabled");
	if(enabled)
		e->enabled = json_is_true(enabled);
	const char *model_path = json_string_value(json_object_get(cfg, "gender_model_path"));
	if(model_path && *model_path)
		e->model_path = model_path;
	json_t *window_ms = json_object_get(cfg, "gender_window_ms");
	if(window_ms && json_is_integer(window_ms)) {
		json_int_t v = json_integer_value(window_ms);
		if(v >= 1000 && v <= 10000)
			e->window_samples = (size_t)((v * e->target_rate) / 1000);
	}
	json_t *min_conf = json_object_get(cfg, "gender_min_confidence");
	if(min_conf && (json_is_real(min_conf) || json_is_integer(min_conf))) {
		double v = json_number_value(min_conf);
		if(v >= 0.0 && v <= 1.0)
			e->min_confidence = (float)v;
	}
	json_t *max_buf_ms = json_object_get(cfg, "gender_max_buffer_ms");
	if(max_buf_ms && json_is_integer(max_buf_ms)) {
		json_int_t v = json_integer_value(max_buf_ms);
		if(v >= 2000 && v <= 15000)
			e->max_buffer_samples = (size_t)((v * e->target_rate) / 1000);
	}
	json_t *trigger_min_chars = json_object_get(cfg, "gender_trigger_min_chars");
	if(trigger_min_chars && json_is_integer(trigger_min_chars)) {
		json_int_t v = json_integer_value(trigger_min_chars);
		if(v >= 1 && v <= 256)
			e->trigger_min_chars = (size_t)v;
	}
	json_t *trigger_final_only = json_object_get(cfg, "gender_trigger_final_only");
	if(trigger_final_only)
		e->trigger_final_only = json_is_true(trigger_final_only);
	json_t *trigger_min_conf = json_object_get(cfg, "gender_trigger_min_transcript_confidence");
	if(trigger_min_conf && (json_is_real(trigger_min_conf) || json_is_integer(trigger_min_conf))) {
		double v = json_number_value(trigger_min_conf);
		if(v >= 0.0 && v <= 1.0)
			e->trigger_min_transcript_confidence = (float)v;
	}
	json_t *trigger_allow_unknown = json_object_get(cfg, "gender_trigger_allow_unknown_confidence");
	if(trigger_allow_unknown)
		e->trigger_allow_unknown_confidence = json_is_true(trigger_allow_unknown);
	json_decref(cfg);
	return true;
}

static bool init_model(abmod_gender_engine *e) {
	if(!e || !e->enabled)
		return true;
	try {
		e->env = new Ort::Env(ORT_LOGGING_LEVEL_WARNING, "abmod_gender");
		e->opts = new Ort::SessionOptions();
		e->opts->SetIntraOpNumThreads(1);
		e->opts->SetGraphOptimizationLevel(GraphOptimizationLevel::ORT_ENABLE_ALL);
#ifdef _WIN32
		std::wstring wpath(e->model_path.begin(), e->model_path.end());
		e->session = new Ort::Session(*e->env, wpath.c_str(), *e->opts);
#else
		e->session = new Ort::Session(*e->env, e->model_path.c_str(), *e->opts);
#endif
		e->alloc = new Ort::AllocatorWithDefaultOptions();
		size_t in_count = e->session->GetInputCount();
		for(size_t i = 0; i < in_count; ++i) {
			auto n = e->session->GetInputNameAllocated(i, *e->alloc);
			std::string name(n.get());
			if(name == "input_values" || name == "audio" || name == "input") {
				e->input_name = name;
				break;
			}
		}
		if(e->input_name.empty() && in_count > 0) {
			auto n = e->session->GetInputNameAllocated(0, *e->alloc);
			e->input_name = n.get();
		}
		size_t out_count = e->session->GetOutputCount();
		if(out_count > 0) {
			auto on = e->session->GetOutputNameAllocated(0, *e->alloc);
			e->output_name = on.get();
		}
		e->ready = !e->input_name.empty() && !e->output_name.empty();
		if(!e->ready) {
			fprintf(stderr, "[ABMod][gender] invalid model IO\n");
		}
		return e->ready;
	} catch(const Ort::Exception &ex) {
		fprintf(stderr, "[ABMod][gender] init failed: %s\n", ex.what());
		e->ready = false;
		return false;
	}
}

static bool run_infer(abmod_gender_engine *e, const std::vector<float> &audio,
		std::string *label, float *confidence) {
	if(!e || !e->ready || audio.empty() || !label || !confidence)
		return false;
	try {
		std::array<int64_t, 2> shape = {1, (int64_t)audio.size()};
		Ort::MemoryInfo mem = Ort::MemoryInfo::CreateCpu(OrtArenaAllocator, OrtMemTypeDefault);
		Ort::Value input = Ort::Value::CreateTensor<float>(mem,
			const_cast<float *>(audio.data()), audio.size(), shape.data(), 2);
		const char *in_name = e->input_name.c_str();
		const char *out_name = e->output_name.c_str();
		auto outputs = e->session->Run(Ort::RunOptions{nullptr},
			&in_name, &input, 1, &out_name, 1);
		if(outputs.empty())
			return false;
		float *logits = outputs[0].GetTensorMutableData<float>();
		auto ti = outputs[0].GetTensorTypeAndShapeInfo();
		size_t elems = ti.GetElementCount();
		if(elems < 2)
			return false;
		float l0 = logits[0], l1 = logits[1];
		float m = std::max(l0, l1);
		float e0 = expf(l0 - m), e1 = expf(l1 - m);
		float denom = e0 + e1;
		float p0 = e0 / denom; /* female */
		float p1 = e1 / denom; /* male */
		if(p1 >= p0) {
			*label = "male";
			*confidence = p1;
		} else {
			*label = "female";
			*confidence = p0;
		}
		return true;
	} catch(const Ort::Exception &ex) {
		fprintf(stderr, "[ABMod][gender] inference failed: %s\n", ex.what());
		return false;
	}
}

extern "C" abmod_gender_engine *abmod_gender_create(const char *config_json) {
	abmod_gender_engine *e = new abmod_gender_engine();
	parse_config(e, config_json);
	fprintf(stderr, "[ABMod][gender] create enabled=%d model=%s window_samples=%zu min_confidence=%.2f max_buffer_samples=%zu trigger_min_chars=%zu trigger_final_only=%d trigger_min_transcript_confidence=%.3f trigger_allow_unknown_confidence=%d\n",
		e->enabled ? 1 : 0, e->model_path.c_str(), e->window_samples, e->min_confidence, e->max_buffer_samples,
		e->trigger_min_chars, e->trigger_final_only ? 1 : 0, e->trigger_min_transcript_confidence, e->trigger_allow_unknown_confidence ? 1 : 0);
	if(!e->enabled) {
		fprintf(stderr, "[ABMod][gender] disabled via config (gender_enabled=false)\n");
		e->ready = false;
		return e;
	}
	if(!init_model(e)) {
		e->enabled = false;
		fprintf(stderr, "[ABMod][gender] disabled: model init failed\n");
	} else {
		fprintf(stderr, "[ABMod][gender] ready: one-shot inference enabled\n");
	}
	return e;
}

extern "C" void abmod_gender_destroy(abmod_gender_engine *engine) {
	if(!engine)
		return;
	delete engine->session;
	delete engine->opts;
	delete engine->env;
	delete engine->alloc;
	delete engine;
}

extern "C" void abmod_gender_on_pcm(abmod_gender_engine *engine,
		const char *room_id, const char *user_id,
		const int16_t *pcm, size_t samples, uint32_t sampling_rate, int channels) {
	if(!engine || !engine->enabled || !engine->ready || !room_id || !user_id || !pcm || samples == 0)
		return;
	std::lock_guard<std::mutex> lk(engine->mtx);
	GenderState &st = engine->users[key_for(room_id, user_id)];
	if(st.attempted)
		return;
	std::vector<float> mono = to_mono_16k(pcm, samples, sampling_rate, channels, engine->target_rate);
	if(mono.empty())
		return;
	st.mono16k.insert(st.mono16k.end(), mono.begin(), mono.end());
	if(st.mono16k.size() > engine->max_buffer_samples) {
		size_t trim = st.mono16k.size() - engine->max_buffer_samples;
		st.mono16k.erase(st.mono16k.begin(), st.mono16k.begin() + (ptrdiff_t)trim);
	}
	if(st.mono16k.size() < engine->window_samples) {
		if(st.mono16k.size() == mono.size() || (st.mono16k.size() / 16000) != ((st.mono16k.size() - mono.size()) / 16000)) {
			fprintf(stderr, "[ABMod][gender] buffering room=%s user=%s samples=%zu/%zu\n",
				room_id, user_id, st.mono16k.size(), engine->window_samples);
		}
		return;
	}
	if(!st.trigger_wait_logged) {
		fprintf(stderr, "[ABMod][gender] buffered room=%s user=%s samples=%zu (waiting transcript trigger)\n",
			room_id, user_id, st.mono16k.size());
		st.trigger_wait_logged = true;
	}
}

static size_t count_nonspace_chars(const char *text) {
	if(!text)
		return 0;
	size_t n = 0;
	for(const unsigned char *p = (const unsigned char *)text; *p; ++p) {
		if(!isspace(*p))
			++n;
	}
	return n;
}

static int abmod_gender_trigger_if_text(abmod_gender_engine *engine,
		const char *room_id, const char *user_id,
		const char *text, size_t min_chars) {
	if(!engine || !engine->enabled || !engine->ready || !room_id || !user_id || !text)
		return 0;
	size_t text_len = count_nonspace_chars(text);
	if(text_len < min_chars)
		return 0;
	std::lock_guard<std::mutex> lk(engine->mtx);
	GenderState &st = engine->users[key_for(room_id, user_id)];
	if(st.attempted)
		return 0;
	if(st.mono16k.size() < engine->window_samples) {
		fprintf(stderr, "[ABMod][gender] trigger-deferred room=%s user=%s buffered=%zu/%zu text_len=%zu\n",
			room_id, user_id, st.mono16k.size(), engine->window_samples, text_len);
		return 0;
	}
	size_t start = st.mono16k.size() - engine->window_samples;
	st.attempted = true;
	std::vector<float> window(st.mono16k.begin() + (ptrdiff_t)start, st.mono16k.end());
	std::string label;
	float conf = 0.0f;
	if(run_infer(engine, window, &label, &conf) && conf >= engine->min_confidence) {
		st.known = true;
		st.label = label;
		st.confidence = conf;
		fprintf(stderr, "[ABMod][gender] inferred room=%s user=%s label=%s confidence=%.3f (text_len=%zu)\n",
			room_id, user_id, st.label.c_str(), st.confidence, text_len);
	} else {
		fprintf(stderr, "[ABMod][gender] unavailable room=%s user=%s (trigger infer failed or confidence below %.2f)\n",
			room_id, user_id, engine->min_confidence);
	}
	std::vector<float>().swap(st.mono16k);
	return 1;
}

extern "C" int abmod_gender_trigger_on_transcript(abmod_gender_engine *engine,
		const char *room_id, const char *user_id,
		const char *text, float transcript_confidence, int is_final) {
	if(!engine || !engine->enabled || !engine->ready)
		return 0;
	if(engine->trigger_final_only && !is_final)
		return 0;
	if(transcript_confidence < 0.0f) {
		if(!engine->trigger_allow_unknown_confidence)
			return 0;
	} else if(transcript_confidence < engine->trigger_min_transcript_confidence) {
		return 0;
	}
	return abmod_gender_trigger_if_text(engine, room_id, user_id, text, engine->trigger_min_chars);
}

extern "C" void abmod_gender_clear_user(abmod_gender_engine *engine,
		const char *room_id, const char *user_id) {
	if(!engine || !room_id || !user_id)
		return;
	std::lock_guard<std::mutex> lk(engine->mtx);
	fprintf(stderr, "[ABMod][gender] clear room=%s user=%s\n", room_id, user_id);
	engine->users.erase(key_for(room_id, user_id));
}

extern "C" int abmod_gender_get_result(abmod_gender_engine *engine,
		const char *room_id, const char *user_id, char *out_label,
		size_t out_label_len, float *out_confidence, const char **out_status) {
	if(out_status)
		*out_status = "disabled";
	if(!engine)
		return 0;
	if(!engine->enabled || !engine->ready)
		return 0;
	if(!room_id || !user_id)
		return 0;
	std::lock_guard<std::mutex> lk(engine->mtx);
	auto it = engine->users.find(key_for(room_id, user_id));
	if(it == engine->users.end()) {
		if(out_status)
			*out_status = "pending";
		return 0;
	}
	const GenderState &st = it->second;
	if(st.known) {
		if(out_label && out_label_len > 0) {
			snprintf(out_label, out_label_len, "%s", st.label.c_str());
		}
		if(out_confidence)
			*out_confidence = st.confidence;
		if(out_status)
			*out_status = "ready";
		return 1;
	}
	if(out_status)
		*out_status = st.attempted ? "unavailable" : "pending";
	if(st.attempted) {
		fprintf(stderr, "[ABMod][gender] status room=%s user=%s -> unavailable\n", room_id, user_id);
	}
	return 0;
}
