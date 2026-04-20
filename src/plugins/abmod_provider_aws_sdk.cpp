/*
 * AWS Transcribe Medical streaming using the official AWS SDK for C++.
 * https://docs.aws.amazon.com/sdk-for-cpp/v1/developer-guide/cpp_transcribe-streaming_code_examples.html
 */

#include "abmod_provider_aws_sdk.h"

#include <atomic>
#include <cstdio>
#include <condition_variable>
#include <cstring>
#include <memory>
#include <mutex>
#include <queue>
#include <string>
#include <thread>
#include <vector>

#if defined(_WIN32)
#include <string.h>
#define ABMOD_STRICMP _stricmp
#else
#include <strings.h>
#define ABMOD_STRICMP strcasecmp
#endif

#define ABMOD_LOG(fmt, ...) \
	fprintf(stderr, "[ABMod][aws] " fmt "\n", ##__VA_ARGS__)
#include <aws/core/Aws.h>
#include <aws/core/auth/AWSCredentials.h>
#include <aws/core/auth/AWSCredentialsProvider.h>
#include <aws/core/client/ClientConfiguration.h>
#include <aws/core/utils/threading/Semaphore.h>
#include <aws/transcribestreaming/TranscribeStreamingServiceClient.h>
#include <aws/transcribestreaming/model/AudioEvent.h>
#include <aws/transcribestreaming/model/AudioStream.h>
#include <aws/transcribestreaming/model/MediaEncoding.h>
#include <aws/transcribestreaming/model/MedicalAlternative.h>
#include <aws/transcribestreaming/model/MedicalResult.h>
#include <aws/transcribestreaming/model/MedicalTranscript.h>
#include <aws/transcribestreaming/model/MedicalTranscriptEvent.h>
#include <aws/transcribestreaming/model/StartMedicalStreamTranscriptionHandler.h>
#include <aws/transcribestreaming/model/StartMedicalStreamTranscriptionRequest.h>
#include <aws/transcribestreaming/model/Type.h>

using namespace Aws;
using namespace Aws::Client;
using namespace Aws::Auth;
using namespace Aws::TranscribeStreamingService;
using namespace Aws::TranscribeStreamingService::Model;

namespace {

std::mutex g_sdk_mutex;
std::atomic<int> g_sdk_refcount{0};
SDKOptions g_sdk_options;

static LanguageCode parse_language(const char *lc) {
	(void)lc;
	/* Medical streaming US English; extend mapping if needed */
	return LanguageCode::en_US;
}

static Specialty parse_specialty(const char *sp) {
	(void)sp;
	return Specialty::PRIMARYCARE;
}

static Type parse_type(const char *t) {
	if(t && ABMOD_STRICMP(t, "DICTATION") == 0)
		return Type::DICTATION;
	return Type::CONVERSATION;
}

/* Max PCM chunks buffered before we start dropping (prevents unbounded growth
 * when AWS is slow or the connection stalls). At 960 samples / 16 kHz =
 * 60 ms/frame, 200 frames = ~12 s of audio. */
static constexpr size_t NATIVE_QUEUE_MAX = 200;

struct NativeStream {
	std::mutex mu;
	std::condition_variable cv;
	std::queue<std::vector<uint8_t>> q;
	bool stop{false};
	bool stream_error{false}; /* set when WriteAudioEvent fails; stop accepting PCM */

	void *cb_user{nullptr};
	abmod_sdk_transcript_fn on_text{nullptr};
	abmod_sdk_error_fn on_err{nullptr};
	char room_id[256]{};
	char user_id[256]{};

	/* Deep copies of config strings — owned by this object so they are valid
	 * on the worker thread even after the caller frees its temporaries. */
	std::string cfg_region;
	std::string cfg_language_code;
	std::string cfg_specialty;
	std::string cfg_stream_type;
	std::string cfg_session_id;
	std::string cfg_access_key_id;
	std::string cfg_secret_access_key;
	std::string cfg_session_token;
	uint32_t cfg_sample_rate{16000};
	int cfg_medical_redaction{0};

	std::shared_ptr<TranscribeStreamingServiceClient> client;
	std::thread worker;

	~NativeStream() {
		close_internal();
	}

	void emit_err(const char *msg) {
		if(on_err && msg)
			on_err(cb_user, room_id, user_id, msg);
	}

	void emit_txt(const Aws::String &text, bool is_final) {
		if(!on_text || text.empty())
			return;
		on_text(cb_user, room_id, user_id, text.c_str(), is_final ? 1 : 0);
	}

	void close_internal() {
		{
			std::lock_guard<std::mutex> lk(mu);
			stop = true;
		}
		cv.notify_all();
		if(worker.joinable())
			worker.join();
	}

	void push_pcm(const int16_t *pcm, size_t samples, int channels) {
		if(!pcm || samples == 0)
			return;
		std::vector<uint8_t> buf;
		if(channels > 1) {
			buf.resize(samples * sizeof(int16_t));
			for(size_t i = 0; i < samples; i++) {
				int16_t m = pcm[i * channels];
				memcpy(buf.data() + i * sizeof(int16_t), &m, sizeof(int16_t));
			}
		} else {
			buf.resize(samples * sizeof(int16_t));
			memcpy(buf.data(), pcm, buf.size());
		}
		std::lock_guard<std::mutex> lk(mu);
		if(stop || stream_error)
			return;
		if(q.size() >= NATIVE_QUEUE_MAX) {
			/* Drop oldest frame to keep latency bounded */
			q.pop();
		}
		q.push(std::move(buf));
		cv.notify_one();
	}

	void run_async() {
		ClientConfiguration client_cfg;
		client_cfg.region = cfg_region;

		/* SDK 1.11: pass credentials directly to the client constructor;
		 * ClientConfiguration no longer has a credentialsProvider field. */
		if(!cfg_access_key_id.empty() && !cfg_secret_access_key.empty()) {
			AWSCredentials creds(
				cfg_access_key_id,
				cfg_secret_access_key);
			client = std::make_shared<TranscribeStreamingServiceClient>(creds, client_cfg);
		} else {
			client = std::make_shared<TranscribeStreamingServiceClient>(client_cfg);
		}

		StartMedicalStreamTranscriptionHandler handler;
		handler.SetOnErrorCallback([this](const Aws::Client::AWSError<TranscribeStreamingServiceErrors> &err) {
			emit_err(err.GetMessage().c_str());
		});
		/* SDK 1.11: Medical streaming uses MedicalTranscriptEvent / MedicalResult /
		 * MedicalAlternative — same logical structure, different type names. */
		handler.SetMedicalTranscriptEventCallback([this](const MedicalTranscriptEvent &ev) {
			const auto &tr = ev.GetTranscript();
			const auto &results = tr.GetResults();
			for(const auto &r : results) {
				const auto &alts = r.GetAlternatives();
				if(alts.empty())
					continue;
				Aws::String txt = alts[0].GetTranscript();
				if(txt.empty())
					continue;
				bool is_partial = r.GetIsPartial();
				emit_txt(txt, !is_partial);
			}
		});

		StartMedicalStreamTranscriptionRequest request;
		request.SetLanguageCode(parse_language(cfg_language_code.c_str()));
		request.SetMediaSampleRateHertz(static_cast<int>(cfg_sample_rate));
		request.SetMediaEncoding(MediaEncoding::pcm);
		request.SetSpecialty(parse_specialty(cfg_specialty.c_str()));
		request.SetType(parse_type(cfg_stream_type.c_str()));
		// if(!cfg_session_id.empty())
		// 	request.SetSessionId(Aws::String(cfg_session_id));
		if(cfg_medical_redaction)
			request.SetContentIdentificationType(MedicalContentIdentificationType::PHI);

		request.SetEventStreamHandler(handler);

		Utils::Threading::Semaphore done(0, 1);

		auto on_stream_ready = [this](AudioStream &stream) {
			for(;;) {
				std::vector<uint8_t> chunk;
				{
					std::unique_lock<std::mutex> lk(mu);
					cv.wait(lk, [this] {
						return stop || !q.empty();
					});
					if(stop && q.empty()) {
						/* Empty AudioEvent ends the stream per AWS bidirectional event spec */
						if(!stream.WriteAudioEvent(AudioEvent())) {
							emit_err("WriteAudioEvent(empty) failed");
							return;
						}
						stream.flush();
						stream.Close();
						return;
					}
					if(!q.empty()) {
						chunk = std::move(q.front());
						q.pop();
					}
				}
			if(!chunk.empty()) {
				Aws::Vector<unsigned char> bits(chunk.begin(), chunk.end());
				AudioEvent aev(std::move(bits));
				if(!stream.WriteAudioEvent(aev)) {
					/* Mark stream as broken so push_pcm stops accepting data */
					{
						std::lock_guard<std::mutex> lk(mu);
						stream_error = true;
						/* Drain queue to free memory */
						while(!q.empty()) q.pop();
					}
					/* Include chunk size for diagnosis (credentials/network issues
					 * often manifest as immediate write failures on the first chunk) */
					char errmsg[128];
					snprintf(errmsg, sizeof(errmsg),
						"WriteAudioEvent failed (chunk=%zu bytes) — check AWS credentials and region",
						chunk.size());
					emit_err(errmsg);
					return;
				}
			}
			}
		};

		auto on_response = [this, &done](
				const TranscribeStreamingServiceClient *,
				const StartMedicalStreamTranscriptionRequest &,
				const StartMedicalStreamTranscriptionOutcome &outcome,
				const std::shared_ptr<const Aws::Client::AsyncCallerContext> &) {
			if(!outcome.IsSuccess())
				emit_err(outcome.GetError().GetMessage().c_str());
			done.Release();
		};

		client->StartMedicalStreamTranscriptionAsync(request, on_stream_ready, on_response, nullptr);
		done.WaitOne();
	}
};

} /* namespace */

void abmod_aws_native_global_init(void) {
	std::lock_guard<std::mutex> lk(g_sdk_mutex);
	if(g_sdk_refcount.fetch_add(1) == 0)
		InitAPI(g_sdk_options);
}

void abmod_aws_native_global_shutdown(void) {
	std::lock_guard<std::mutex> lk(g_sdk_mutex);
	int v = g_sdk_refcount.fetch_sub(1) - 1;
	if(v == 0)
		ShutdownAPI(g_sdk_options);
}

void *abmod_aws_native_stream_open(const AbmodAwsNativeConfig *cfg,
		const char *room_id,
		const char *user_id,
		abmod_sdk_transcript_fn on_text,
		abmod_sdk_error_fn on_err,
		void *user) {
	if(!cfg || !room_id || !user_id || !on_text || !on_err)
		return nullptr;

	auto *s = new (std::nothrow) NativeStream();
	if(!s)
		return nullptr;
	s->cb_user = user;
	s->on_text = on_text;
	s->on_err = on_err;
	snprintf(s->room_id, sizeof(s->room_id), "%s", room_id);
	snprintf(s->user_id, sizeof(s->user_id), "%s", user_id);

	/* Deep-copy all config strings into NativeStream so the worker thread
	 * never touches a pointer the caller might free after we return. */
	s->cfg_region           = cfg->region          ? cfg->region          : "us-east-1";
	s->cfg_language_code    = cfg->language_code    ? cfg->language_code   : "en-US";
	s->cfg_specialty        = cfg->specialty        ? cfg->specialty       : "PRIMARYCARE";
	s->cfg_stream_type      = cfg->stream_type      ? cfg->stream_type     : "CONVERSATION";
	s->cfg_session_id       = cfg->session_id       ? cfg->session_id      : "";
	s->cfg_access_key_id    = cfg->access_key_id    ? cfg->access_key_id   : "";
	s->cfg_secret_access_key= cfg->secret_access_key? cfg->secret_access_key: "";
	s->cfg_session_token    = cfg->session_token    ? cfg->session_token   : "";
	s->cfg_sample_rate      = cfg->sample_rate;
	s->cfg_medical_redaction= cfg->medical_redaction;
	ABMOD_LOG("abmod_aws_native_stream_open region=%s language_code=%s specialty=%s stream_type=%s sample_rate=%d medical_redaction=%d",
		cfg->region ? cfg->region : "(null)",
		cfg->language_code ? cfg->language_code : "(null)",
		cfg->specialty ? cfg->specialty : "(null)",
		cfg->stream_type ? cfg->stream_type : "(null)",
		cfg->sample_rate, cfg->medical_redaction);
	s->worker = std::thread([s]() {
		try {
			s->run_async();
		} catch(const std::exception &ex) {
			s->emit_err(ex.what());
		} catch(...) {
			s->emit_err("AWS SDK unknown exception");
		}
	});

	return s;
}

int abmod_aws_native_stream_send_pcm(void *stream,
		const int16_t *pcm,
		size_t samples,
		int channels) {
	auto *s = static_cast<NativeStream *>(stream);
	if(!s || !pcm || samples == 0)
		return -1;
	s->push_pcm(pcm, samples, channels);
	return 0;
}

void abmod_aws_native_stream_close(void *stream) {
	auto *s = static_cast<NativeStream *>(stream);
	if(!s)
		return;
	s->close_internal();
	delete s;
}
