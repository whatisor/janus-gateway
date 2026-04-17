/*
 * Embedding-based selectBest — C++ implementation using ONNX Runtime.
 *
 * Model: all-MiniLM-L6-v2  (sentence-transformers/all-MiniLM-L6-v2 on HF)
 * ONNX export:  onnx/model.onnx  (from the same HF repo)
 * Vocab:        vocab.txt        (standard BERT WordPiece vocab)
 *
 * Mirrors embeddingGuard.ts selection logic exactly.
 */

#include "abmod_embedding.h"

/* Pre-built ORT release tarball puts headers flat in include/;
 * from-source / distro packages nest them under onnxruntime/core/session/. */
#if __has_include(<onnxruntime_cxx_api.h>)
#  include <onnxruntime_cxx_api.h>
#else
#  include <onnxruntime/core/session/onnxruntime_cxx_api.h>
#endif

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <fstream>
#include <mutex>
#include <numeric>
#include <string>
#include <unordered_map>
#include <vector>

/* ── Constants ───────────────────────────────────────────────────────── */

static constexpr int   CLS_ID      = 101;
static constexpr int   SEP_ID      = 102;
static constexpr int   PAD_ID      = 0;
static constexpr int   UNK_ID      = 100;
static constexpr int   MAX_SEQ_LEN = 256;
static constexpr int   EMBED_DIM   = 384;  /* all-MiniLM-L6-v2 */

/* ── WordPiece tokenizer ─────────────────────────────────────────────── */

class WordPieceTokenizer {
    std::unordered_map<std::string, int> vocab_;

    static std::string toLower(const std::string &s) {
        std::string r = s;
        for (char &c : r)
            if (c >= 'A' && c <= 'Z') c += 32;
        return r;
    }

    /* Split on whitespace and ASCII punctuation */
    static std::vector<std::string> basicTokenize(const std::string &text) {
        std::vector<std::string> tokens;
        std::string cur;
        for (unsigned char c : text) {
            if (c <= 32) {
                if (!cur.empty()) { tokens.push_back(cur); cur.clear(); }
            } else if (ispunct(c)) {
                if (!cur.empty()) { tokens.push_back(cur); cur.clear(); }
                tokens.push_back(std::string(1, (char)c));
            } else {
                cur += (char)c;
            }
        }
        if (!cur.empty()) tokens.push_back(cur);
        return tokens;
    }

    std::vector<int> wordPiece(const std::string &word) const {
        std::vector<int> ids;
        size_t start = 0;
        while (start < word.size()) {
            size_t end = word.size();
            int found_id = -1;
            while (end > start) {
                std::string sub = (start == 0 ? "" : "##") +
                                  word.substr(start, end - start);
                auto it = vocab_.find(sub);
                if (it != vocab_.end()) { found_id = it->second; break; }
                --end;
            }
            if (found_id == -1) return {UNK_ID};
            ids.push_back(found_id);
            start = end;
        }
        return ids;
    }

public:
    bool load(const char *vocab_path) {
        std::ifstream f(vocab_path);
        if (!f.is_open()) return false;
        std::string line;
        int idx = 0;
        while (std::getline(f, line)) {
            if (!line.empty() && line.back() == '\r') line.pop_back();
            vocab_[line] = idx++;
        }
        return !vocab_.empty();
    }

    /* Returns token ids including [CLS] and [SEP], truncated to max_len */
    std::vector<int64_t> encode(const std::string &text, int max_len) const {
        std::vector<int64_t> ids;
        ids.push_back(CLS_ID);
        for (auto &w : basicTokenize(toLower(text))) {
            for (int id : wordPiece(w)) {
                if ((int)ids.size() >= max_len - 1) goto done;
                ids.push_back(id);
            }
        }
    done:
        ids.push_back(SEP_ID);
        return ids;
    }
};

/* ── Embedding model ─────────────────────────────────────────────────── */

class EmbeddingModel {
    Ort::Env                       env_;
    Ort::SessionOptions            opts_;
    Ort::Session                   session_;
    Ort::AllocatorWithDefaultOptions alloc_;
    WordPieceTokenizer             tok_;
    std::mutex                     mtx_;
    float                          threshold_;
    bool                           has_direct_output_ = false; /* sentence_embedding */
    bool                           has_token_type_ids_ = true;

    void detectInputsOutputs() {
        /* Check if model exposes a ready-normalised sentence_embedding output */
        size_t n_out = session_.GetOutputCount();
        for (size_t i = 0; i < n_out; i++) {
            auto name = session_.GetOutputNameAllocated(i, alloc_);
            if (std::string(name.get()) == "sentence_embedding") {
                has_direct_output_ = true;
                break;
            }
        }
        /* Check if token_type_ids is expected as input */
        size_t n_in = session_.GetInputCount();
        has_token_type_ids_ = false;
        for (size_t i = 0; i < n_in; i++) {
            auto name = session_.GetInputNameAllocated(i, alloc_);
            if (std::string(name.get()) == "token_type_ids") {
                has_token_type_ids_ = true;
                break;
            }
        }
    }

    static std::vector<float> meanPoolAndNorm(
            const float *hidden, const int64_t *mask,
            int seq_len, int dim) {
        std::vector<float> out(dim, 0.0f);
        int count = 0;
        for (int t = 0; t < seq_len; t++) {
            if (!mask[t]) continue;
            for (int d = 0; d < dim; d++) out[d] += hidden[t * dim + d];
            count++;
        }
        if (count > 0) for (float &v : out) v /= count;
        float norm = 0.0f;
        for (float v : out) norm += v * v;
        norm = std::sqrt(norm);
        if (norm > 1e-8f) for (float &v : out) v /= norm;
        return out;
    }

public:
    EmbeddingModel()
        : env_(ORT_LOGGING_LEVEL_WARNING, "abmod_embedding"),
          session_(nullptr),
          threshold_(0.5f) {}

    bool init(const char *model_path, const char *vocab_path, float threshold) {
        if (!tok_.load(vocab_path)) {
            fprintf(stderr, "[embedding] Failed to load vocab: %s\n", vocab_path);
            return false;
        }
        threshold_ = threshold;
        opts_.SetIntraOpNumThreads(1);
        opts_.SetGraphOptimizationLevel(GraphOptimizationLevel::ORT_ENABLE_ALL);
        try {
#ifdef _WIN32
            std::wstring wpath(model_path, model_path + strlen(model_path));
            session_ = Ort::Session(env_, wpath.c_str(), opts_);
#else
            session_ = Ort::Session(env_, model_path, opts_);
#endif
        } catch (const Ort::Exception &e) {
            fprintf(stderr, "[embedding] ORT session failed: %s\n", e.what());
            return false;
        }
        detectInputsOutputs();
        fprintf(stderr, "[embedding] Loaded model=%s direct_output=%d\n",
            model_path, (int)has_direct_output_);
        return true;
    }

    bool ready() const { return session_ != nullptr; }

    /* Compute a unit-normalised 384-dim embedding. Returns empty on error. */
    std::vector<float> embed(const std::string &text) {
        auto ids = tok_.encode(text, MAX_SEQ_LEN);
        int seq_len = (int)ids.size();
        ids.resize(MAX_SEQ_LEN, PAD_ID);

        std::vector<int64_t> mask(MAX_SEQ_LEN, 0);
        for (int i = 0; i < seq_len; i++) mask[i] = 1;
        std::vector<int64_t> type_ids(MAX_SEQ_LEN, 0);

        std::array<int64_t, 2> shape = {1, MAX_SEQ_LEN};
        auto mem = Ort::MemoryInfo::CreateCpu(OrtArenaAllocator, OrtMemTypeDefault);

        std::vector<Ort::Value> inputs;
        inputs.push_back(Ort::Value::CreateTensor<int64_t>(
            mem, ids.data(), ids.size(), shape.data(), 2));
        inputs.push_back(Ort::Value::CreateTensor<int64_t>(
            mem, mask.data(), mask.size(), shape.data(), 2));
        if (has_token_type_ids_)
            inputs.push_back(Ort::Value::CreateTensor<int64_t>(
                mem, type_ids.data(), type_ids.size(), shape.data(), 2));

        /* Build input/output name lists dynamically */
        std::vector<const char *> in_names = {"input_ids", "attention_mask"};
        if (has_token_type_ids_) in_names.push_back("token_type_ids");
        const char *out_name = has_direct_output_
            ? "sentence_embedding" : "last_hidden_state";

        std::vector<float> result;
        try {
            std::lock_guard<std::mutex> lk(mtx_);
            auto outputs = session_.Run(
                Ort::RunOptions{nullptr},
                in_names.data(), inputs.data(), inputs.size(),
                &out_name, 1);

            float *data = outputs[0].GetTensorMutableData<float>();
            if (has_direct_output_) {
                /* Already mean-pooled and normalised */
                result.assign(data, data + EMBED_DIM);
                /* Renormalise defensively */
                float norm = 0.0f;
                for (float v : result) norm += v * v;
                norm = std::sqrt(norm);
                if (norm > 1e-8f) for (float &v : result) v /= norm;
            } else {
                result = meanPoolAndNorm(data, mask.data(), MAX_SEQ_LEN, EMBED_DIM);
            }
        } catch (const Ort::Exception &e) {
            fprintf(stderr, "[embedding] Inference error: %s\n", e.what());
        }
        return result;
    }

    /*
     * Mirror of embeddingGuard.ts selectBest:
     *   1. Embed all candidates
     *   2. Compute pairwise cosine similarity (dot product — vectors are unit)
     *   3. For each candidate: avg_sim = mean cosine to all others
     *   4. Filter by avg_sim >= threshold
     *   5. Among passing: sort by length DESC, then avg_sim DESC
     *   6. Return index of winner, or -1 if none pass
     */
    int selectBest(const char **texts, int n) {
        if (n <= 0) return -1;
        if (n == 1) return 0;

        /* Embed all candidates */
        std::vector<std::vector<float>> embs(n);
        for (int i = 0; i < n; i++) {
            embs[i] = embed(texts[i] ? texts[i] : "");
            if (embs[i].empty()) return -1;
        }

        /* Pairwise dot products (== cosine because vectors are unit-normalised) */
        std::vector<std::vector<float>> sim(n, std::vector<float>(n, 0.0f));
        for (int i = 0; i < n; i++)
            for (int j = i + 1; j < n; j++) {
                float dot = 0.0f;
                for (int d = 0; d < EMBED_DIM; d++) dot += embs[i][d] * embs[j][d];
                sim[i][j] = sim[j][i] = dot;
            }

        /* Average similarity to all other candidates */
        std::vector<float> avg_sim(n, 0.0f);
        for (int i = 0; i < n; i++) {
            for (int j = 0; j < n; j++)
                if (j != i) avg_sim[i] += sim[i][j];
            avg_sim[i] /= (n - 1);
        }

        /* Filter and sort: longest first, then most central */
        struct Candidate { int idx; float avg_sim; };
        std::vector<Candidate> passed;
        for (int i = 0; i < n; i++)
            if (avg_sim[i] >= threshold_)
                passed.push_back({i, avg_sim[i]});

        if (passed.empty()) return -1;

        std::sort(passed.begin(), passed.end(), [&](const Candidate &a, const Candidate &b) {
            size_t la = texts[a.idx] ? strlen(texts[a.idx]) : 0;
            size_t lb = texts[b.idx] ? strlen(texts[b.idx]) : 0;
            if (lb != la) return lb < la;   /* longest first */
            return b.avg_sim < a.avg_sim;   /* tie-break: most central */
        });

        return passed[0].idx;
    }

    float threshold() const { return threshold_; }
};

/* ── Singleton ───────────────────────────────────────────────────────── */

static EmbeddingModel *g_model   = nullptr;
static std::mutex      g_init_mtx;

extern "C" {

int abmod_embedding_init(const char *model_path, const char *vocab_path,
                         float similarity_threshold) {
    std::lock_guard<std::mutex> lk(g_init_mtx);
    if (g_model) return 0;  /* already loaded */
    g_model = new EmbeddingModel();
    if (!g_model->init(model_path, vocab_path, similarity_threshold)) {
        delete g_model;
        g_model = nullptr;
        return -1;
    }
    return 0;
}

void abmod_embedding_destroy(void) {
    std::lock_guard<std::mutex> lk(g_init_mtx);
    delete g_model;
    g_model = nullptr;
}

int abmod_embedding_select_best(const char **texts, int n) {
    if (!g_model || !g_model->ready()) return -1;
    return g_model->selectBest(texts, n);
}

int abmod_embedding_ready(void) {
    return (g_model && g_model->ready()) ? 1 : 0;
}

} /* extern "C" */
