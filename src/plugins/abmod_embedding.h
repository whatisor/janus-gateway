#ifndef ABMOD_EMBEDDING_H
#define ABMOD_EMBEDDING_H

#ifdef __cplusplus
extern "C" {
#endif

/*
 * Embedding-based selectBest using all-MiniLM-L6-v2 via ONNX Runtime.
 *
 * Algorithm mirrors embeddingGuard.ts:
 *   1. Embed each candidate (mean-pool + L2-normalize → 384-dim unit vector)
 *   2. Build pairwise cosine similarity matrix (dot product, vectors are unit)
 *   3. For each candidate compute average similarity to all others
 *   4. Filter candidates with avg_sim >= similarity_threshold
 *   5. Among those, return the index of the longest (tie-break: most central)
 *   6. Return -1 if no candidate passes the threshold (caller discards result)
 *
 * Usage:
 *   abmod_embedding_init("/path/to/model.onnx", "/path/to/vocab.txt", 0.5f);
 *   int best = abmod_embedding_select_best(texts, n);
 *   abmod_embedding_destroy();
 */

/* Initialize the singleton embedding model. Safe to call multiple times;
 * only the first call loads the model. Returns 0 on success, -1 on failure. */
int abmod_embedding_init(const char *model_path,
                         const char *vocab_path,
                         float similarity_threshold);

/* Release the singleton. */
void abmod_embedding_destroy(void);

/* Returns the index of the best candidate, or -1 if candidates diverge.
 * texts: array of n C strings. Thread-safe (serialised internally). */
int abmod_embedding_select_best(const char **texts, int n);

/* 1 if the model was successfully loaded, 0 otherwise. */
int abmod_embedding_ready(void);

#ifdef __cplusplus
}
#endif

#endif /* ABMOD_EMBEDDING_H */
