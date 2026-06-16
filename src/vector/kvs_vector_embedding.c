#define KVS_VECTOR_INTERNAL
#include "kvs_vector.h"

#include <math.h>
#include <stdint.h>
#include <time.h>

static pthread_mutex_t g_embedding_provider_encode_mu = PTHREAD_MUTEX_INITIALIZER;

typedef struct kvs_vector_embedding_thread_context_s {
	EmbeddingHandle *runtime_handle;
	kvs_vector_embedding_provider_type_t provider_type;
	size_t dimension;
	int initialized;
	int fallback_to_global;
} kvs_vector_embedding_thread_context_t;

static __thread kvs_vector_embedding_thread_context_t *g_embedding_thread_ctx;
static kvs_vector_embedding_runtime_stats_t g_embedding_runtime_stats;

static uint64_t embedding_stats_add(uint64_t *field, uint64_t value) {
	return __atomic_add_fetch(field, value, __ATOMIC_RELAXED);
}

static uint64_t embedding_stats_sub(uint64_t *field, uint64_t value) {
	return __atomic_sub_fetch(field, value, __ATOMIC_RELAXED);
}

static uint64_t embedding_stats_load(const uint64_t *field) {
	return __atomic_load_n(field, __ATOMIC_RELAXED);
}

static void embedding_stats_store(uint64_t *field, uint64_t value) {
	__atomic_store_n(field, value, __ATOMIC_RELAXED);
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

static void embedding_record_encode_time(uint64_t elapsed_us) {
	embedding_stats_add(&g_embedding_runtime_stats.encode_total_us, elapsed_us);
	embedding_stats_update_max(&g_embedding_runtime_stats.encode_max_us, elapsed_us);
}

static uint64_t embedding_encode_enter(void) {
	uint64_t inflight = embedding_stats_add(
		&g_embedding_runtime_stats.encode_current_inflight,
		1
	);
	embedding_stats_update_max(&g_embedding_runtime_stats.encode_max_inflight, inflight);
	return embedding_now_us();
}

static void embedding_encode_leave(uint64_t start_us) {
	uint64_t end_us = embedding_now_us();

	if (end_us >= start_us) {
		embedding_record_encode_time(end_us - start_us);
	}
	embedding_stats_sub(&g_embedding_runtime_stats.encode_current_inflight, 1);
}

static int kvs_vector_embedding_l2_normalize(float *embedding, int dim) {
	double norm_sq = 0.0;
	double inv_norm;
	int i;

	if (!embedding || dim <= 0) {
		return KVS_VECTOR_ERR_INVALID_ARG;
	}

	for (i = 0; i < dim; ++i) {
		double v = (double)embedding[i];
		norm_sq += v * v;
	}

	if (norm_sq <= 0.0) {
		return KVS_VECTOR_ERR_INVALID_ARG;
	}

	inv_norm = 1.0 / sqrt(norm_sq);
	for (i = 0; i < dim; ++i) {
		embedding[i] = (float)((double)embedding[i] * inv_norm);
	}

	return KVS_VECTOR_OK;
}

static int kvs_vector_embedding_l2_normalize_batch(
	float *embeddings,
	size_t text_count,
	int dim
) {
	size_t i;
	int ret;

	if (!embeddings || text_count == 0 || dim <= 0) {
		return KVS_VECTOR_ERR_INVALID_ARG;
	}

	for (i = 0; i < text_count; ++i) {
		ret = kvs_vector_embedding_l2_normalize(
			embeddings + (i * (size_t)dim),
			dim
		);
		if (ret != KVS_VECTOR_OK) {
			return ret;
		}
	}

	return KVS_VECTOR_OK;
}

static int kvs_vector_embedding_error_to_status(EmbeddingErrorCode code) {
	switch (code) {
		case EMBEDDING_OK:
			return KVS_VECTOR_OK;
		case EMBEDDING_ERR_INVALID_ARGUMENT:
		case EMBEDDING_ERR_DIM_MISMATCH:
		case EMBEDDING_ERR_TEXT_TOO_LONG:
			return KVS_VECTOR_ERR_INVALID_ARG;
		case EMBEDDING_ERR_NOT_INITIALIZED:
			return KVS_VECTOR_ERR_NOT_INITIALIZED;
		case EMBEDDING_ERR_OUT_OF_MEMORY:
		case EMBEDDING_ERR_MODEL_LOAD_FAILED:
		case EMBEDDING_ERR_INFERENCE_FAILED:
		default:
			return KVS_VECTOR_ERR;
	}
}

static void kvs_vector_embedding_build_config(EmbeddingConfig *config) {
	if (!config) {
		return;
	}
	memset(config, 0, sizeof(*config));
	config->model_path = g_cfg.embedding_model_path[0] != '\0' ?
		g_cfg.embedding_model_path :
		NULL;
	config->embedding_dim = g_cfg.embedding_dim;
	config->num_threads = g_cfg.embedding_num_threads;
	config->max_text_len = g_cfg.embedding_max_text_len;
	config->normalize = 0;
}

static int kvs_vector_embedding_create_runtime_handle(
	EmbeddingHandle **out_handle,
	size_t *out_dimension
) {
	EmbeddingConfig config;
	EmbeddingHandle *handle = NULL;
	EmbeddingErrorCode err;
	int dimension;

	if (!out_handle || !out_dimension) {
		return KVS_VECTOR_ERR_INVALID_ARG;
	}
	*out_handle = NULL;
	*out_dimension = 0;

	kvs_vector_embedding_build_config(&config);
	err = embedding_create(&config, &handle);
	if (err != EMBEDDING_OK) {
		return kvs_vector_embedding_error_to_status(err);
	}

	dimension = embedding_dimension(handle);
	if (dimension <= 0) {
		embedding_destroy(handle);
		return KVS_VECTOR_ERR_INVALID_ARG;
	}

	*out_handle = handle;
	*out_dimension = (size_t)dimension;
	return KVS_VECTOR_OK;
}

void kvs_vector_embedding_runtime_get_stats(
	kvs_vector_embedding_runtime_stats_t *out
) {
	if (!out) {
		return;
	}
	out->per_thread_context_enabled = embedding_stats_load(&g_embedding_runtime_stats.per_thread_context_enabled);
	out->per_thread_context_created = embedding_stats_load(&g_embedding_runtime_stats.per_thread_context_created);
	out->per_thread_context_destroyed = embedding_stats_load(&g_embedding_runtime_stats.per_thread_context_destroyed);
	out->per_thread_context_create_failed = embedding_stats_load(&g_embedding_runtime_stats.per_thread_context_create_failed);
	out->per_thread_context_fallback_count = embedding_stats_load(&g_embedding_runtime_stats.per_thread_context_fallback_count);
	out->encode_global_mutex_count = embedding_stats_load(&g_embedding_runtime_stats.encode_global_mutex_count);
	out->encode_thread_context_count = embedding_stats_load(&g_embedding_runtime_stats.encode_thread_context_count);
	out->encode_current_inflight = embedding_stats_load(&g_embedding_runtime_stats.encode_current_inflight);
	out->encode_max_inflight = embedding_stats_load(&g_embedding_runtime_stats.encode_max_inflight);
	out->encode_total_us = embedding_stats_load(&g_embedding_runtime_stats.encode_total_us);
	out->encode_max_us = embedding_stats_load(&g_embedding_runtime_stats.encode_max_us);
}

int kvs_vector_embedding_thread_context_init(void) {
	kvs_vector_embedding_provider_t *provider = &g_vector_state.embedding_provider;
	kvs_vector_embedding_thread_context_t *ctx;
	EmbeddingHandle *handle = NULL;
	size_t dimension = 0;
	int ret;

	if (g_embedding_thread_ctx) {
		return 0;
	}
	if (!g_cfg.embedding_per_thread_runtime_enabled) {
		return 0;
	}
	embedding_stats_store(&g_embedding_runtime_stats.per_thread_context_enabled, 1);

	if (!provider->initialized ||
	    provider->type != KVS_VECTOR_EMBEDDING_PROVIDER_LOCAL_C) {
		return 0;
	}

	ctx = (kvs_vector_embedding_thread_context_t *)malloc(sizeof(*ctx));
	if (!ctx) {
		embedding_stats_add(&g_embedding_runtime_stats.per_thread_context_create_failed, 1);
		if (g_cfg.embedding_per_thread_runtime_fallback_mutex) {
			embedding_stats_add(&g_embedding_runtime_stats.per_thread_context_fallback_count, 1);
			return 0;
		}
		return -1;
	}
	memset(ctx, 0, sizeof(*ctx));

	ret = kvs_vector_embedding_create_runtime_handle(&handle, &dimension);
	if (ret != KVS_VECTOR_OK) {
		free(ctx);
		embedding_stats_add(&g_embedding_runtime_stats.per_thread_context_create_failed, 1);
		if (g_cfg.embedding_per_thread_runtime_fallback_mutex) {
			embedding_stats_add(&g_embedding_runtime_stats.per_thread_context_fallback_count, 1);
			KVS_LOG_ERROR(
				KVS_VECTOR_LOG_MODULE,
				"per-thread embedding runtime init failed; falling back to global mutex ret=%d",
				ret
			);
			return 0;
		}
		KVS_LOG_ERROR(
			KVS_VECTOR_LOG_MODULE,
			"per-thread embedding runtime init failed ret=%d",
			ret
		);
		return -1;
	}

	ctx->runtime_handle = handle;
	ctx->provider_type = provider->type;
	ctx->dimension = dimension;
	ctx->initialized = 1;
	g_embedding_thread_ctx = ctx;
	embedding_stats_add(&g_embedding_runtime_stats.per_thread_context_created, 1);
	KVS_LOG_INFO(
		KVS_VECTOR_LOG_MODULE,
		"per-thread embedding runtime ready dimension=%zu",
		dimension
	);
	return 0;
}

void kvs_vector_embedding_thread_context_destroy(void) {
	kvs_vector_embedding_thread_context_t *ctx = g_embedding_thread_ctx;

	if (!ctx) {
		return;
	}
	g_embedding_thread_ctx = NULL;
	if (ctx->runtime_handle) {
		embedding_destroy(ctx->runtime_handle);
		ctx->runtime_handle = NULL;
	}
	embedding_stats_add(&g_embedding_runtime_stats.per_thread_context_destroyed, 1);
	free(ctx);
}

int kvs_vector_embedding_thread_context_available(void) {
	return g_embedding_thread_ctx &&
	       g_embedding_thread_ctx->initialized &&
	       g_embedding_thread_ctx->runtime_handle != NULL;
}

/*
 * 初始化 embedding provider。
 *
 * 当前支持 hash provider 和本地 C ABI runtime provider。
 */
int kvs_vector_embedding_provider_init(
	kvs_vector_embedding_provider_t *provider
) {
	int use_local_c;

	if (!provider) {
		KVS_LOG_ERROR(KVS_VECTOR_LOG_MODULE, "embedding provider init failed: provider is null");
		return KVS_VECTOR_ERR_INVALID_ARG;
	}

	memset(provider, 0, sizeof(*provider));

	use_local_c =
		strcasecmp(g_cfg.embedding_provider, "local_c") == 0 ||
		strcasecmp(g_cfg.embedding_provider, "c_api") == 0;

	if (use_local_c) {
		size_t dimension;
		int ret;

		ret = kvs_vector_embedding_create_runtime_handle(
			&provider->runtime_handle,
			&dimension
		);
		if (ret != KVS_VECTOR_OK) {
			KVS_LOG_ERROR(
				KVS_VECTOR_LOG_MODULE,
				"local_c embedding provider init failed ret=%d",
				ret
			);
			return ret;
		}

		provider->type = KVS_VECTOR_EMBEDDING_PROVIDER_LOCAL_C;
		provider->name = "local_c";
		provider->dimension = dimension;
		provider->initialized = 1;

		KVS_LOG_INFO(
			KVS_VECTOR_LOG_MODULE,
			"local_c embedding provider ready dimension=%zu max_text_len=%d threads=%d",
			provider->dimension,
			g_cfg.embedding_max_text_len,
			g_cfg.embedding_num_threads
		);

		return KVS_VECTOR_OK;
	}

	provider->type = KVS_VECTOR_EMBEDDING_PROVIDER_HASH;
	provider->name = "hash";
	provider->dimension = KVS_VECTOR_DEFAULT_DIMENSION;
	provider->initialized = 1;

	KVS_LOG_INFO(
		KVS_VECTOR_LOG_MODULE,
		"hash embedding provider ready dimension=%d",
		KVS_VECTOR_DEFAULT_DIMENSION
	);

	return KVS_VECTOR_OK;
}

/*
 * 销毁 embedding provider。
 */
void kvs_vector_embedding_provider_destroy(
	kvs_vector_embedding_provider_t *provider
) {
	if (!provider) {
		return;
	}

	if (provider->initialized &&
		provider->type == KVS_VECTOR_EMBEDDING_PROVIDER_HASH) {
		KVS_LOG_DEBUG(
			KVS_VECTOR_LOG_MODULE,
			"destroy hash embedding provider"
		);
	}

	if (provider->runtime_handle) {
		embedding_destroy(provider->runtime_handle);
	}

	memset(provider, 0, sizeof(*provider));
}

/*
 * 返回当前 embedding provider 名称。
 */
const char *kvs_vector_embedding_provider_name(void) {
	if (!g_vector_state.embedding_provider.initialized ||
		!g_vector_state.embedding_provider.name) {
		return "unknown";
	}

	return g_vector_state.embedding_provider.name;
}

/*
 * 获取当前 embedding provider 的有效维度。
 */
size_t kvs_vector_embedding_provider_dimension(void) {
	if (!g_vector_state.embedding_provider.initialized ||
	    g_vector_state.embedding_provider.dimension == 0) {
		return KVS_VECTOR_DEFAULT_DIMENSION;
	}

	return g_vector_state.embedding_provider.dimension;
}

/*
 * 带 seed 的字节 hash。
 */
uint32_t kvs_vector_hash_bytes_seeded(
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
		hash ^= (uint32_t)i + (seed << 1);
	}

	return hash;
}

/*
 * 本地 HASH embedding provider。
 *
 * 策略：
 * 1. 固定 64 维；
 * 2. 按字符/字节 hash 分桶；
 * 3. 返回原始桶计数向量，由 embedding 统一入口执行 L2 normalize。
 */
int kvs_vector_hash_embedding_generate(
	const char *text,
	size_t text_len,
	float **out_embedding,
	int *out_dim
) {
	float *embedding;
	size_t i;
	size_t dim = kvs_vector_embedding_provider_dimension();

	if (!out_embedding || !out_dim) {
		return KVS_VECTOR_ERR_INVALID_ARG;
	}

	*out_embedding = NULL;
	*out_dim = 0;

	if (!text || text_len == 0 || dim == 0) {
		return KVS_VECTOR_ERR_INVALID_ARG;
	}

	embedding = (float *)kvs_malloc(dim * sizeof(float));
	if (!embedding) {
		return KVS_VECTOR_ERR;
	}

	memset(embedding, 0, dim * sizeof(float));

	for (i = 0; i < text_len; ++i) {
		unsigned char ch = (unsigned char)text[i];
		uint32_t h1 = kvs_vector_hash_bytes_seeded((const char *)&text[i], 1, (uint32_t)i + 1);
		uint32_t h2 = kvs_vector_hash_bytes_seeded(text, text_len, (uint32_t)ch + 131u);
		size_t bucket1 = (size_t)(h1 % dim);
		size_t bucket2 = (size_t)(h2 % dim);

		embedding[bucket1] += 1.0f;
		embedding[bucket2] += 0.5f;
	}

	*out_embedding = embedding;
	*out_dim = (int)dim;

	return KVS_VECTOR_OK;
}

/*
 * HASH 批量 embedding provider。
 *
 * 返回连续 float 数组：embeddings[i * out_dim + j]
 * 调用方使用 kvs_free 释放 out_embeddings。
 */
int kvs_vector_hash_embedding_generate_batch(
	const char **texts,
	const size_t *text_lens,
	size_t text_count,
	float **out_embeddings,
	int *out_dim
) {
	float *batch_embeddings = NULL;
	size_t dim = kvs_vector_embedding_provider_dimension();
	size_t i;
	size_t bytes;

	if (!texts || !text_lens || text_count == 0 || !out_embeddings || !out_dim) {
		return KVS_VECTOR_ERR_INVALID_ARG;
	}

	*out_embeddings = NULL;
	*out_dim = 0;

	bytes = text_count * dim * sizeof(float);
	if (dim == 0 || bytes / sizeof(float) / dim != text_count) {
		return KVS_VECTOR_ERR_INVALID_ARG;
	}

	batch_embeddings = (float *)kvs_malloc(bytes);
	if (!batch_embeddings) {
		return KVS_VECTOR_ERR;
	}

	memset(batch_embeddings, 0, bytes);

	for (i = 0; i < text_count; ++i) {
		float *one_embedding = NULL;
		int one_dim = 0;
		int ret;

		ret = kvs_vector_hash_embedding_generate(
			texts[i],
			text_lens[i],
			&one_embedding,
			&one_dim
		);
		if (ret != KVS_VECTOR_OK) {
			kvs_free(batch_embeddings);
			return ret;
		}

		if (!one_embedding || one_dim != (int)dim) {
			if (one_embedding) {
				kvs_free(one_embedding);
			}
			kvs_free(batch_embeddings);
			return KVS_VECTOR_ERR;
		}

		memcpy(
			batch_embeddings + (i * dim),
			one_embedding,
			dim * sizeof(float)
		);

		kvs_free(one_embedding);
	}

	*out_embeddings = batch_embeddings;
	*out_dim = (int)dim;

	return KVS_VECTOR_OK;
}

static int kvs_vector_local_c_embedding_generate(
	kvs_vector_embedding_provider_t *provider,
	const char *text,
	size_t text_len,
	float **out_embedding,
	int *out_dim
) {
	float *embedding;
	size_t dim;
	EmbeddingErrorCode err;
	EmbeddingHandle *handle;
	int use_thread_context = 0;
	uint64_t start_us;

	if (!provider || !provider->runtime_handle || !out_embedding || !out_dim) {
		return KVS_VECTOR_ERR_INVALID_ARG;
	}

	*out_embedding = NULL;
	*out_dim = 0;

	if (!text || text_len == 0 || provider->dimension == 0) {
		return KVS_VECTOR_ERR_INVALID_ARG;
	}

	dim = provider->dimension;
	embedding = (float *)kvs_malloc(dim * sizeof(float));
	if (!embedding) {
		return KVS_VECTOR_ERR;
	}

	handle = provider->runtime_handle;
	if (g_embedding_thread_ctx &&
	    g_embedding_thread_ctx->initialized &&
	    g_embedding_thread_ctx->runtime_handle &&
	    g_embedding_thread_ctx->provider_type == provider->type &&
	    g_embedding_thread_ctx->dimension == dim) {
		handle = g_embedding_thread_ctx->runtime_handle;
		use_thread_context = 1;
	}

	if (use_thread_context) {
		embedding_stats_add(&g_embedding_runtime_stats.encode_thread_context_count, 1);
		start_us = embedding_encode_enter();
	} else {
		embedding_stats_add(&g_embedding_runtime_stats.encode_global_mutex_count, 1);
		pthread_mutex_lock(&g_embedding_provider_encode_mu);
		start_us = embedding_encode_enter();
	}
	err = embedding_encode(
		handle,
		text,
		text_len,
		embedding,
		(int)dim
	);
	if (err != EMBEDDING_OK) {
		KVS_LOG_ERROR(
			KVS_VECTOR_LOG_MODULE,
			"local_c embedding encode failed err=%d message=%s text_len=%zu thread_context=%d",
			(int)err,
			embedding_last_error(handle),
			text_len,
			use_thread_context
		);
		embedding_encode_leave(start_us);
		if (!use_thread_context) {
			pthread_mutex_unlock(&g_embedding_provider_encode_mu);
		}
		kvs_free(embedding);
		return kvs_vector_embedding_error_to_status(err);
	}
	embedding_encode_leave(start_us);
	if (!use_thread_context) {
		pthread_mutex_unlock(&g_embedding_provider_encode_mu);
	}

	*out_embedding = embedding;
	*out_dim = (int)dim;

	return KVS_VECTOR_OK;
}

/*
 * embedding 统一入口。
 */
int kvs_vector_embedding_generate(
	const char *text,
	size_t text_len,
	float **out_embedding,
	int *out_dim
) {
	kvs_vector_embedding_provider_t *provider = &g_vector_state.embedding_provider;
	float *embedding = NULL;
	int dim = 0;
	int ret;

	if (!provider->initialized) {
		return KVS_VECTOR_ERR_NOT_INITIALIZED;
	}

	if (!text || text_len == 0 || !out_embedding || !out_dim) {
		return KVS_VECTOR_ERR_INVALID_ARG;
	}

	*out_embedding = NULL;
	*out_dim = 0;

	if (provider->type == KVS_VECTOR_EMBEDDING_PROVIDER_LOCAL_C) {
		ret = kvs_vector_local_c_embedding_generate(
			provider,
			text,
			text_len,
			&embedding,
			&dim
		);
	} else {
		ret = kvs_vector_hash_embedding_generate(
			text,
			text_len,
			&embedding,
			&dim
		);
	}
	if (ret != KVS_VECTOR_OK) {
		return ret;
	}

	ret = kvs_vector_embedding_l2_normalize(embedding, dim);
	if (ret != KVS_VECTOR_OK) {
		kvs_free(embedding);
		return ret;
	}

	*out_embedding = embedding;
	*out_dim = dim;

	return KVS_VECTOR_OK;
}

/*
 * embedding 批量统一入口。
 */
int kvs_vector_embedding_generate_batch(
	const char **texts,
	const size_t *text_lens,
	size_t text_count,
	float **out_embeddings,
	int *out_dim
) {
	kvs_vector_embedding_provider_t *provider = &g_vector_state.embedding_provider;
	float *embeddings = NULL;
	int dim = 0;
	int ret;

	if (!provider->initialized) {
		return KVS_VECTOR_ERR_NOT_INITIALIZED;
	}

	if (!texts || !text_lens || text_count == 0 || !out_embeddings || !out_dim) {
		return KVS_VECTOR_ERR_INVALID_ARG;
	}

	*out_embeddings = NULL;
	*out_dim = 0;

	if (provider->type == KVS_VECTOR_EMBEDDING_PROVIDER_LOCAL_C) {
		size_t provider_dim = provider->dimension;
		size_t i;
		size_t bytes;
		EmbeddingHandle *handle = provider->runtime_handle;
		int use_thread_context = 0;
		uint64_t start_us;

		if (provider_dim == 0 ||
		    text_count > ((size_t)-1 / provider_dim / sizeof(float))) {
			return KVS_VECTOR_ERR_INVALID_ARG;
		}

		bytes = text_count * provider_dim * sizeof(float);
		embeddings = (float *)kvs_malloc(bytes);
		if (!embeddings) {
			return KVS_VECTOR_ERR;
		}

		if (g_embedding_thread_ctx &&
		    g_embedding_thread_ctx->initialized &&
		    g_embedding_thread_ctx->runtime_handle &&
		    g_embedding_thread_ctx->provider_type == provider->type &&
		    g_embedding_thread_ctx->dimension == provider_dim) {
			handle = g_embedding_thread_ctx->runtime_handle;
			use_thread_context = 1;
		}

		if (use_thread_context) {
			embedding_stats_add(&g_embedding_runtime_stats.encode_thread_context_count, text_count);
			start_us = embedding_encode_enter();
		} else {
			embedding_stats_add(&g_embedding_runtime_stats.encode_global_mutex_count, text_count);
			pthread_mutex_lock(&g_embedding_provider_encode_mu);
			start_us = embedding_encode_enter();
		}
		for (i = 0; i < text_count; ++i) {
			EmbeddingErrorCode err = embedding_encode(
				handle,
				texts[i],
				text_lens[i],
				embeddings + (i * provider_dim),
				(int)provider_dim
			);
			if (err != EMBEDDING_OK) {
				KVS_LOG_ERROR(
					KVS_VECTOR_LOG_MODULE,
					"local_c embedding batch encode failed err=%d message=%s index=%zu text_len=%zu thread_context=%d",
					(int)err,
					embedding_last_error(handle),
					i,
					text_lens[i],
					use_thread_context
				);
				embedding_encode_leave(start_us);
				if (!use_thread_context) {
					pthread_mutex_unlock(&g_embedding_provider_encode_mu);
				}
				kvs_free(embeddings);
				return kvs_vector_embedding_error_to_status(err);
			}
		}
		embedding_encode_leave(start_us);
		if (!use_thread_context) {
			pthread_mutex_unlock(&g_embedding_provider_encode_mu);
		}

		dim = (int)provider_dim;
	} else {
		ret = kvs_vector_hash_embedding_generate_batch(
			texts,
			text_lens,
			text_count,
			&embeddings,
			&dim
		);
		if (ret != KVS_VECTOR_OK) {
			return ret;
		}
	}

	ret = kvs_vector_embedding_l2_normalize_batch(embeddings, text_count, dim);
	if (ret != KVS_VECTOR_OK) {
		kvs_free(embeddings);
		return ret;
	}

	*out_embeddings = embeddings;
	*out_dim = dim;

	return KVS_VECTOR_OK;
}

/*
 * embedding 并发批量统一入口。
 *
 * hash provider 没有远程 RPC，直接复用普通 batch。
 */
int kvs_vector_embedding_generate_batch_parallel(
	const char **texts,
	const size_t *text_lens,
	size_t text_count,
	size_t batch_size,
	size_t max_inflight,
	float **out_embeddings,
	int *out_dim
) {
	(void)batch_size;
	(void)max_inflight;

	return kvs_vector_embedding_generate_batch(
		texts,
		text_lens,
		text_count,
		out_embeddings,
		out_dim
	);
}
