#define KVS_VECTOR_INTERNAL
#include "kvs_vector.h"

#include <math.h>

static pthread_mutex_t g_embedding_provider_encode_mu = PTHREAD_MUTEX_INITIALIZER;

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
		EmbeddingConfig config;
		EmbeddingErrorCode err;
		int dimension;

		memset(&config, 0, sizeof(config));
		config.model_path = g_cfg.embedding_model_path[0] != '\0' ?
			g_cfg.embedding_model_path :
			NULL;
		config.embedding_dim = g_cfg.embedding_dim;
		config.num_threads = g_cfg.embedding_num_threads;
		config.max_text_len = g_cfg.embedding_max_text_len;
		config.normalize = 0;

		err = embedding_create(&config, &provider->runtime_handle);
		if (err != EMBEDDING_OK) {
			KVS_LOG_ERROR(
				KVS_VECTOR_LOG_MODULE,
				"local_c embedding provider init failed err=%d",
				(int)err
			);
			return kvs_vector_embedding_error_to_status(err);
		}

		dimension = embedding_dimension(provider->runtime_handle);
		if (dimension <= 0) {
			KVS_LOG_ERROR(
				KVS_VECTOR_LOG_MODULE,
				"local_c embedding provider returned invalid dimension=%d",
				dimension
			);
			embedding_destroy(provider->runtime_handle);
			memset(provider, 0, sizeof(*provider));
			return KVS_VECTOR_ERR_INVALID_ARG;
		}

		provider->type = KVS_VECTOR_EMBEDDING_PROVIDER_LOCAL_C;
		provider->name = "local_c";
		provider->dimension = (size_t)dimension;
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

	pthread_mutex_lock(&g_embedding_provider_encode_mu);
	err = embedding_encode(
		provider->runtime_handle,
		text,
		text_len,
		embedding,
		(int)dim
	);
	if (err != EMBEDDING_OK) {
		KVS_LOG_ERROR(
			KVS_VECTOR_LOG_MODULE,
			"local_c embedding encode failed err=%d message=%s text_len=%zu",
			(int)err,
			embedding_last_error(provider->runtime_handle),
			text_len
		);
		pthread_mutex_unlock(&g_embedding_provider_encode_mu);
		kvs_free(embedding);
		return kvs_vector_embedding_error_to_status(err);
	}
	pthread_mutex_unlock(&g_embedding_provider_encode_mu);

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

		if (provider_dim == 0 ||
		    text_count > ((size_t)-1 / provider_dim / sizeof(float))) {
			return KVS_VECTOR_ERR_INVALID_ARG;
		}

		bytes = text_count * provider_dim * sizeof(float);
		embeddings = (float *)kvs_malloc(bytes);
		if (!embeddings) {
			return KVS_VECTOR_ERR;
		}

		pthread_mutex_lock(&g_embedding_provider_encode_mu);
		for (i = 0; i < text_count; ++i) {
			EmbeddingErrorCode err = embedding_encode(
				provider->runtime_handle,
				texts[i],
				text_lens[i],
				embeddings + (i * provider_dim),
				(int)provider_dim
			);
			if (err != EMBEDDING_OK) {
				KVS_LOG_ERROR(
					KVS_VECTOR_LOG_MODULE,
					"local_c embedding batch encode failed err=%d message=%s index=%zu text_len=%zu",
					(int)err,
					embedding_last_error(provider->runtime_handle),
					i,
					text_lens[i]
				);
				pthread_mutex_unlock(&g_embedding_provider_encode_mu);
				kvs_free(embeddings);
				return kvs_vector_embedding_error_to_status(err);
			}
		}
		pthread_mutex_unlock(&g_embedding_provider_encode_mu);

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
