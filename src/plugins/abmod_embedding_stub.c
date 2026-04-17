/* Stub: ONNX Runtime not available. All embedding calls are no-ops. */
#include "abmod_embedding.h"

int  abmod_embedding_init(const char *model_path, const char *vocab_path,
                          float similarity_threshold) {
	(void)model_path; (void)vocab_path; (void)similarity_threshold;
	return -1;
}
void abmod_embedding_destroy(void) {}
int  abmod_embedding_select_best(const char **texts, int n) {
	(void)texts; (void)n; return -1;
}
int  abmod_embedding_ready(void) { return 0; }
