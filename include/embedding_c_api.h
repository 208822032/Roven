#pragma once

#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef struct EmbeddingHandle EmbeddingHandle;

typedef struct {
	const char *model_path;
	int embedding_dim;
	int num_threads;
	int max_text_len;
	int normalize;
} EmbeddingConfig;

typedef enum {
	EMBEDDING_OK = 0,
	EMBEDDING_ERR_INVALID_ARGUMENT = 1,
	EMBEDDING_ERR_MODEL_LOAD_FAILED = 2,
	EMBEDDING_ERR_INFERENCE_FAILED = 3,
	EMBEDDING_ERR_DIM_MISMATCH = 4,
	EMBEDDING_ERR_TEXT_TOO_LONG = 5,
	EMBEDDING_ERR_OUT_OF_MEMORY = 6,
	EMBEDDING_ERR_NOT_INITIALIZED = 7
} EmbeddingErrorCode;

typedef struct {
	uint64_t shared_ort_env_enabled;
	uint64_t shared_ort_env_created;
	uint64_t shared_ort_env_destroyed;
	uint64_t shared_ort_env_refcount_current;
	uint64_t shared_ort_env_refcount_max;
	uint64_t ort_session_created;
	uint64_t ort_session_destroyed;
	uint64_t encode_count;
	uint64_t embedding_encode_total_us;
	uint64_t embedding_encode_max_us;
	uint64_t tokenizer_total_us;
	uint64_t tokenizer_max_us;
	uint64_t ort_run_total_us;
	uint64_t ort_run_max_us;
} EmbeddingRuntimeResourceStats;

EmbeddingErrorCode embedding_create(
	const EmbeddingConfig *config,
	EmbeddingHandle **handle
);

EmbeddingErrorCode embedding_encode(
	EmbeddingHandle *handle,
	const char *text,
	size_t text_len,
	float *output,
	int output_dim
);

int embedding_dimension(EmbeddingHandle *handle);

const char *embedding_last_error(EmbeddingHandle *handle);

void embedding_runtime_get_resource_stats(
	EmbeddingRuntimeResourceStats *out
);

void embedding_destroy(EmbeddingHandle *handle);

#ifdef __cplusplus
}
#endif
