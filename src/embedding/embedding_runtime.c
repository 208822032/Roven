#include "embedding_c_api.h"

#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#define EMBEDDING_RUNTIME_DEFAULT_MAX_TEXT_LEN 4096
#define EMBEDDING_RUNTIME_MAX_ERROR 256

struct EmbeddingHandle {
	int embedding_dim;
	int num_threads;
	int max_text_len;
	int normalize;
	char *model_path;
	char last_error[EMBEDDING_RUNTIME_MAX_ERROR];
};

typedef struct EmbeddingHashRuntimeStats {
	uint64_t encode_count;
	uint64_t embedding_encode_total_us;
	uint64_t embedding_encode_max_us;
} EmbeddingHashRuntimeStats;

static EmbeddingHashRuntimeStats g_hash_runtime_stats;

static uint64_t embedding_stats_add(uint64_t *field, uint64_t value) {
	return __atomic_add_fetch(field, value, __ATOMIC_RELAXED);
}

static uint64_t embedding_stats_load(const uint64_t *field) {
	return __atomic_load_n(field, __ATOMIC_RELAXED);
}

static void embedding_stats_update_max(uint64_t *field, uint64_t value) {
	uint64_t old_value = __atomic_load_n(field, __ATOMIC_RELAXED);

	while (value > old_value) {
		if (__atomic_compare_exchange_n(field,
		                                &old_value,
		                                value,
		                                0,
		                                __ATOMIC_RELAXED,
		                                __ATOMIC_RELAXED)) {
			break;
		}
	}
}

static uint64_t embedding_now_us(void) {
	struct timespec ts;

	if (clock_gettime(CLOCK_MONOTONIC, &ts) != 0) {
		return 0;
	}
	return (uint64_t)ts.tv_sec * 1000000ull + (uint64_t)ts.tv_nsec / 1000ull;
}

static void embedding_set_error(EmbeddingHandle *handle, const char *message) {
	if (!handle || !message) {
		return;
	}

	snprintf(handle->last_error, sizeof(handle->last_error), "%s", message);
}

static char *embedding_strdup_or_null(const char *src) {
	size_t len;
	char *copy;

	if (!src) {
		return NULL;
	}

	len = strlen(src);
	copy = (char *)malloc(len + 1);
	if (!copy) {
		return NULL;
	}

	memcpy(copy, src, len + 1);
	return copy;
}

static uint32_t embedding_hash_bytes_seeded(
	const char *data,
	size_t len,
	uint32_t seed
) {
	uint32_t hash = 2166136261u ^ seed;
	size_t i;

	if (!data || len == 0) {
		return hash;
	}

	for (i = 0; i < len; ++i) {
		hash ^= (unsigned char)data[i];
		hash *= 16777619u;
		hash ^= (uint32_t)(i + 0x9e3779b9u + (seed << 1));
	}

	return hash;
}

EmbeddingErrorCode embedding_create(
	const EmbeddingConfig *config,
	EmbeddingHandle **handle
) {
	EmbeddingHandle *created;

	if (!config || !handle) {
		return EMBEDDING_ERR_INVALID_ARGUMENT;
	}

	*handle = NULL;

	if (config->embedding_dim <= 0) {
		return EMBEDDING_ERR_INVALID_ARGUMENT;
	}

	created = (EmbeddingHandle *)calloc(1, sizeof(*created));
	if (!created) {
		return EMBEDDING_ERR_OUT_OF_MEMORY;
	}

	created->embedding_dim = config->embedding_dim;
	created->num_threads = config->num_threads > 0 ? config->num_threads : 1;
	created->max_text_len = config->max_text_len > 0 ?
		config->max_text_len :
		EMBEDDING_RUNTIME_DEFAULT_MAX_TEXT_LEN;
	created->normalize = config->normalize ? 1 : 0;

	if (config->model_path) {
		created->model_path = embedding_strdup_or_null(config->model_path);
		if (!created->model_path) {
			free(created);
			return EMBEDDING_ERR_OUT_OF_MEMORY;
		}
	}

	snprintf(created->last_error, sizeof(created->last_error), "%s", "OK");
	*handle = created;

	return EMBEDDING_OK;
}

EmbeddingErrorCode embedding_encode(
	EmbeddingHandle *handle,
	const char *text,
	size_t text_len,
	float *output,
	int output_dim
) {
	size_t i;
	int dim;
	uint64_t start_us;
	uint64_t end_us;
	uint64_t elapsed_us;

	if (!handle) {
		return EMBEDDING_ERR_NOT_INITIALIZED;
	}

	if (!text || text_len == 0 || !output) {
		embedding_set_error(handle, "invalid text or output buffer");
		return EMBEDDING_ERR_INVALID_ARGUMENT;
	}

	if (text_len > (size_t)handle->max_text_len) {
		embedding_set_error(handle, "text exceeds max_text_len");
		return EMBEDDING_ERR_TEXT_TOO_LONG;
	}

	if (output_dim != handle->embedding_dim) {
		embedding_set_error(handle, "output dimension mismatch");
		return EMBEDDING_ERR_DIM_MISMATCH;
	}

	dim = handle->embedding_dim;
	start_us = embedding_now_us();
	memset(output, 0, (size_t)dim * sizeof(float));

	for (i = 0; i < text_len; ++i) {
		unsigned char ch = (unsigned char)text[i];
		uint32_t h1 = embedding_hash_bytes_seeded(&text[i], 1, (uint32_t)i + 1u);
		uint32_t h2 = embedding_hash_bytes_seeded(text, text_len, (uint32_t)ch + 131u);
		uint32_t h3_seed = (uint32_t)(i * 2654435761u) ^ (uint32_t)text_len;
		uint32_t h3 = embedding_hash_bytes_seeded(text + i, text_len - i, h3_seed);
		size_t b1 = (size_t)(h1 % (uint32_t)dim);
		size_t b2 = (size_t)(h2 % (uint32_t)dim);
		size_t b3 = (size_t)(h3 % (uint32_t)dim);
		float sign = (h2 & 1u) ? 1.0f : -1.0f;

		output[b1] += 1.0f;
		output[b2] += 0.5f * sign;
		output[b3] += 0.25f;
	}
	end_us = embedding_now_us();
	elapsed_us = end_us >= start_us ? end_us - start_us : 0;
	embedding_stats_add(&g_hash_runtime_stats.encode_count, 1);
	embedding_stats_add(&g_hash_runtime_stats.embedding_encode_total_us, elapsed_us);
	embedding_stats_update_max(&g_hash_runtime_stats.embedding_encode_max_us, elapsed_us);

	snprintf(handle->last_error, sizeof(handle->last_error), "%s", "OK");
	return EMBEDDING_OK;
}

EmbeddingErrorCode embedding_encode_batch(
	EmbeddingHandle *handle,
	const char **texts,
	const size_t *text_lens,
	size_t text_count,
	float *outputs,
	int output_dim
) {
	size_t i;

	if (!handle) {
		return EMBEDDING_ERR_NOT_INITIALIZED;
	}
	if (!texts || !text_lens || text_count == 0 || !outputs) {
		return EMBEDDING_ERR_INVALID_ARGUMENT;
	}

	for (i = 0; i < text_count; ++i) {
		EmbeddingErrorCode err = embedding_encode(
			handle,
			texts[i],
			text_lens[i],
			outputs + (i * (size_t)output_dim),
			output_dim
		);
		if (err != EMBEDDING_OK) {
			return err;
		}
	}

	return EMBEDDING_OK;
}

int embedding_dimension(EmbeddingHandle *handle) {
	if (!handle) {
		return 0;
	}

	return handle->embedding_dim;
}

const char *embedding_last_error(EmbeddingHandle *handle) {
	if (!handle) {
		return "embedding handle is not initialized";
	}

	return handle->last_error;
}

void embedding_runtime_get_resource_stats(
	EmbeddingRuntimeResourceStats *out
) {
	if (!out) {
		return;
	}

	memset(out, 0, sizeof(*out));
	out->encode_count = embedding_stats_load(&g_hash_runtime_stats.encode_count);
	out->embedding_encode_total_us =
		embedding_stats_load(&g_hash_runtime_stats.embedding_encode_total_us);
	out->embedding_encode_max_us =
		embedding_stats_load(&g_hash_runtime_stats.embedding_encode_max_us);
}

void embedding_destroy(EmbeddingHandle *handle) {
	if (!handle) {
		return;
	}

	free(handle->model_path);
	free(handle);
}
