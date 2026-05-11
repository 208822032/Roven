#define KVS_VECTOR_INTERNAL
#include "kvs_vector.h"

/*
 * 初始化 embedding provider。
 *
 * provider 类型来自全局配置：
 *
 * embedding_provider = hash:
 *   使用本地 hash embedding。
 *
 * embedding_provider = grpc:
 *   启动时连接外部 gRPC embedding server，并通过 Health 获取真实维度。
 *
 * 注意：
 * embedding_fallback 只用于“启动初始化阶段”的 fallback。
 * 如果 gRPC 初始化成功，运行时生成 embedding 失败时不再动态退回 hash，
 * 避免 384 维真实向量和 64 维 hash 向量混入同一个 flat index。
 */
int kvs_vector_embedding_provider_init(
	kvs_vector_embedding_provider_t *provider
) {
	int ret;
	int dimension = 0;
	char provider_name[128];
	char model_name[256];

	if (!provider) {
		KVS_LOG_ERROR(KVS_VECTOR_LOG_MODULE, "embedding provider init failed: provider is null");
		return KVS_VECTOR_ERR_INVALID_ARG;
	}

	memset(provider, 0, sizeof(*provider));

	KVS_LOG_INFO(
		KVS_VECTOR_LOG_MODULE,
		"embedding provider config provider=%s addr=%s timeout_ms=%d fallback=%d",
		g_cfg.embedding_provider,
		g_cfg.embedding_grpc_addr,
		g_cfg.embedding_timeout_ms,
		g_cfg.embedding_fallback
	);

	if (strcasecmp(g_cfg.embedding_provider, "grpc") == 0) {
		provider->type = KVS_VECTOR_EMBEDDING_PROVIDER_GRPC;
		provider->name = "grpc";
		provider->grpc_timeout_ms = g_cfg.embedding_timeout_ms > 0 ? g_cfg.embedding_timeout_ms : 3000;
		provider->fallback_to_hash = g_cfg.embedding_fallback ? 1 : 0;
		provider->initialized = 1;

		if (g_cfg.embedding_grpc_addr[0] != '\0') {
			snprintf(provider->grpc_addr, sizeof(provider->grpc_addr), "%s", g_cfg.embedding_grpc_addr);
		} else {
			snprintf(provider->grpc_addr, sizeof(provider->grpc_addr), "%s", "127.0.0.1:50051");
		}

		memset(provider_name, 0, sizeof(provider_name));
		memset(model_name, 0, sizeof(model_name));

		KVS_LOG_INFO(
			KVS_VECTOR_LOG_MODULE,
			"initializing grpc embedding provider addr=%s timeout_ms=%d fallback=%d",
			provider->grpc_addr,
			provider->grpc_timeout_ms,
			provider->fallback_to_hash
		);

		ret = kvs_embedding_grpc_init(
			provider->grpc_addr,
			provider->grpc_timeout_ms
		);

		if (ret != KVS_EMBEDDING_GRPC_OK) {
			KVS_LOG_ERROR(
				KVS_VECTOR_LOG_MODULE,
				"grpc embedding client init failed addr=%s timeout_ms=%d ret=%d",
				provider->grpc_addr,
				provider->grpc_timeout_ms,
				ret
			);
		}

		if (ret == KVS_EMBEDDING_GRPC_OK) {
			ret = kvs_embedding_grpc_health(
				&dimension,
				provider_name,
				sizeof(provider_name),
				model_name,
				sizeof(model_name)
			);

			if (ret != KVS_EMBEDDING_GRPC_OK) {
				KVS_LOG_ERROR(
					KVS_VECTOR_LOG_MODULE,
					"grpc embedding health failed addr=%s ret=%d",
					provider->grpc_addr,
					ret
				);
			}
		}

		if (ret == KVS_EMBEDDING_GRPC_OK && dimension > 0) {
			provider->grpc_healthy = 1;
			provider->grpc_dimension = dimension;

			KVS_LOG_INFO(
				KVS_VECTOR_LOG_MODULE,
				"grpc embedding provider ready addr=%s provider=%s model=%s dimension=%d",
				provider->grpc_addr,
				provider_name,
				model_name,
				dimension
			);

			return KVS_VECTOR_OK;
		}

		if (ret == KVS_EMBEDDING_GRPC_OK && dimension <= 0) {
			KVS_LOG_ERROR(
				KVS_VECTOR_LOG_MODULE,
				"grpc embedding health returned invalid dimension=%d addr=%s provider=%s model=%s",
				dimension,
				provider->grpc_addr,
				provider_name,
				model_name
			);
		}

		kvs_embedding_grpc_destroy();

		KVS_LOG_WARN(
			KVS_VECTOR_LOG_MODULE,
			"grpc embedding provider init failed addr=%s ret=%d fallback_to_hash=%d",
			provider->grpc_addr,
			ret,
			provider->fallback_to_hash
		);

		if (provider->fallback_to_hash) {
			memset(provider, 0, sizeof(*provider));
			provider->type = KVS_VECTOR_EMBEDDING_PROVIDER_HASH;
			provider->name = "hash";
			provider->initialized = 1;

			KVS_LOG_WARN(
				KVS_VECTOR_LOG_MODULE,
				"fallback to hash embedding provider dimension=%d",
				KVS_VECTOR_DEFAULT_DIMENSION
			);

			return KVS_VECTOR_OK;
		}

		KVS_LOG_ERROR(
			KVS_VECTOR_LOG_MODULE,
			"grpc embedding provider init failed and fallback disabled"
		);

		memset(provider, 0, sizeof(*provider));
		return KVS_VECTOR_ERR;
	}

	if (strcasecmp(g_cfg.embedding_provider, "hash") != 0) {
		KVS_LOG_WARN(
			KVS_VECTOR_LOG_MODULE,
			"unknown embedding provider=%s, fallback to hash",
			g_cfg.embedding_provider
		);
	}

	provider->type = KVS_VECTOR_EMBEDDING_PROVIDER_HASH;
	provider->name = "hash";
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
		provider->type == KVS_VECTOR_EMBEDDING_PROVIDER_GRPC) {
		KVS_LOG_INFO(
			KVS_VECTOR_LOG_MODULE,
			"destroy grpc embedding provider addr=%s dimension=%d",
			provider->grpc_addr,
			provider->grpc_dimension
		);
		kvs_embedding_grpc_destroy();
	} else if (provider->initialized &&
			   provider->type == KVS_VECTOR_EMBEDDING_PROVIDER_HASH) {
		KVS_LOG_DEBUG(
			KVS_VECTOR_LOG_MODULE,
			"destroy hash embedding provider"
		);
	}

	memset(provider, 0, sizeof(*provider));
}

/*
 * 返回当前 embedding provider 名称。
 *
 * 当前没有暴露到 VINFO，因为 kvs_vector_info_t 还没有 provider 字段。
 * 后续如果要扩展 VINFO，可以复用这个 helper。
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
 *
 * gRPC provider 可用时，优先使用服务端 Health 返回的真实维度。
 * 否则回退到本地 hash embedding 默认维度。
 */
size_t kvs_vector_embedding_provider_dimension(void) {
	kvs_vector_embedding_provider_t *provider = &g_vector_state.embedding_provider;

	if (provider->initialized &&
		provider->type == KVS_VECTOR_EMBEDDING_PROVIDER_GRPC &&
		provider->grpc_healthy &&
		provider->grpc_dimension > 0) {
		return (size_t)provider->grpc_dimension;
	}

	return KVS_VECTOR_DEFAULT_DIMENSION;
}

/*
 * 带 seed 的字节 hash。
 *
 * 用于把文本字节稳定映射到 embedding bucket。
 * 这里不追求密码学安全，只要求确定性和分布相对均匀。
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
 * 获取当前 embedding provider 的有效维度。
 *
 * gRPC provider 可用时，优先使用服务端 Health 返回的真实维度。
 * 如果 gRPC 不可用或没有维度，则回退到本地 hash embedding 默认维度。
 */
// static size_t kvs_vector_embedding_provider_dimension(void) {
// 	kvs_vector_embedding_provider_t *provider = &g_vector_state.embedding_provider;

// 	if (provider->initialized &&
// 		provider->type == KVS_VECTOR_EMBEDDING_PROVIDER_GRPC &&
// 		provider->grpc_healthy &&
// 		provider->grpc_dimension > 0) {
// 		return (size_t)provider->grpc_dimension;
// 	}

// 	return KVS_VECTOR_DEFAULT_DIMENSION;
// }

/*
 * 本地 HASH embedding provider。
 *
 * MVP 策略：
 * 1. 固定 64 维；
 * 2. 按字符/字节 hash 分桶；
 * 3. 使用 L1 归一化，避免引入 sqrtf 和 -lm；
 * 4. 后续接真实 embedding 时，不再改 VSET/VSEARCH 主流程，只替换 provider。
 */
int kvs_vector_hash_embedding_generate(
	const char *text,
	size_t text_len,
	float **out_embedding,
	int *out_dim
) {
	float *embedding;
	size_t i;
	float norm = 0.0f;
	size_t dim = KVS_VECTOR_DEFAULT_DIMENSION;

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

		/*
		 * 字符自身桶 + 上下文桶。
		 * 这样 exact / 相似文本会有更多重叠，但仍保持确定性。
		 */
		embedding[bucket1] += 1.0f;
		embedding[bucket2] += 0.5f;
	}

	for (i = 0; i < dim; ++i) {
		if (embedding[i] < 0.0f) {
			norm -= embedding[i];
		} else {
			norm += embedding[i];
		}
	}

	if (norm <= 0.0f) {
		kvs_free(embedding);
		return KVS_VECTOR_ERR_INVALID_ARG;
	}

	for (i = 0; i < dim; ++i) {
		embedding[i] = embedding[i] / norm;
	}

	*out_embedding = embedding;
	*out_dim = (int)dim;

	return KVS_VECTOR_OK;
}

/*
 * gRPC embedding provider。
 *
 * 通过 C ABI 调用 C++ gRPC client。
 * bridge 返回的 embedding 使用 malloc 分配；
 * vector 模块复制一份到 kvs_malloc 内存，再释放 bridge 内存。
 */
int kvs_vector_grpc_embedding_generate(
	const char *text,
	size_t text_len,
	float **out_embedding,
	int *out_dim
) {
	float *grpc_embedding = NULL;
	float *embedding_copy = NULL;
	int grpc_dim = 0;
	size_t bytes;
	int ret;

	if (!text || text_len == 0 || !out_embedding || !out_dim) {
		KVS_LOG_WARN(
			KVS_VECTOR_LOG_MODULE,
			"grpc embedding generate invalid arg text_len=%zu",
			text_len
		);
		return KVS_VECTOR_ERR_INVALID_ARG;
	}

	*out_embedding = NULL;
	*out_dim = 0;

	ret = kvs_embedding_grpc_generate(
		text,
		text_len,
		&grpc_embedding,
		&grpc_dim
	);
	if (ret != KVS_EMBEDDING_GRPC_OK) {
		KVS_LOG_ERROR(
			KVS_VECTOR_LOG_MODULE,
			"grpc embedding generate failed text_len=%zu ret=%d",
			text_len,
			ret
		);
		return KVS_VECTOR_ERR;
	}

	if (!grpc_embedding || grpc_dim <= 0) {
		KVS_LOG_ERROR(
			KVS_VECTOR_LOG_MODULE,
			"grpc embedding generate bad response text_len=%zu dimension=%d has_embedding=%d",
			text_len,
			grpc_dim,
			grpc_embedding ? 1 : 0
		);
		kvs_embedding_grpc_free_embedding(grpc_embedding);
		return KVS_VECTOR_ERR;
	}

	bytes = (size_t)grpc_dim * sizeof(float);

	embedding_copy = (float *)kvs_malloc(bytes);
	if (!embedding_copy) {
		KVS_LOG_ERROR(
			KVS_VECTOR_LOG_MODULE,
			"grpc embedding copy allocation failed dimension=%d bytes=%zu",
			grpc_dim,
			bytes
		);
		kvs_embedding_grpc_free_embedding(grpc_embedding);
		return KVS_VECTOR_ERR;
	}

	memcpy(embedding_copy, grpc_embedding, bytes);
	kvs_embedding_grpc_free_embedding(grpc_embedding);

	*out_embedding = embedding_copy;
	*out_dim = grpc_dim;

	KVS_LOG_DEBUG(
		KVS_VECTOR_LOG_MODULE,
		"grpc embedding generate success text_len=%zu dimension=%d",
		text_len,
		grpc_dim
	);

	return KVS_VECTOR_OK;
}

/*
 * embedding 统一入口。
 *
 * VSET / VSEARCH / VINDEX 只调用这个函数，不直接关心具体 provider。
 */
int kvs_vector_embedding_generate(
	const char *text,
	size_t text_len,
	float **out_embedding,
	int *out_dim
) {
	kvs_vector_embedding_provider_t *provider = &g_vector_state.embedding_provider;

	if (!provider->initialized) {
		return KVS_VECTOR_ERR_NOT_INITIALIZED;
	}

	switch (provider->type) {
		case KVS_VECTOR_EMBEDDING_PROVIDER_HASH:
			return kvs_vector_hash_embedding_generate(
				text,
				text_len,
				out_embedding,
				out_dim
			);

		case KVS_VECTOR_EMBEDDING_PROVIDER_GRPC:
			/*
			 * gRPC provider 初始化成功后，运行时不再动态 fallback。
			 * 否则可能把 64 维 hash 向量混入 384 维 gRPC 索引。
			 */
			return kvs_vector_grpc_embedding_generate(
				text,
				text_len,
				out_embedding,
				out_dim
			);

		default:
			return KVS_VECTOR_ERR;
	}
}