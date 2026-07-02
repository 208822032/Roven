#include "vector/kvs_vector_hnsw_v2.h"

#include "kvs_memory.h"

#include <string.h>

int kvs_hnsw_v2_context_pool_init(
	kvs_hnsw_v2_context_pool_t *pool,
	size_t count
) {
	size_t bytes;
	kvs_hnsw_v2_search_context_t *contexts;

	if (!pool || count == 0) {
		return KVS_VECTOR_ERR_INVALID_ARG;
	}

	memset(pool, 0, sizeof(*pool));

	bytes = count * sizeof(kvs_hnsw_v2_search_context_t);
	contexts = (kvs_hnsw_v2_search_context_t *)kvs_malloc(bytes);
	if (!contexts) {
		return KVS_VECTOR_ERR;
	}

	memset(contexts, 0, bytes);
	pool->contexts = contexts;
	pool->count = count;
	pool->bytes = bytes;

	return KVS_VECTOR_OK;
}

void kvs_hnsw_v2_context_pool_reset(
	kvs_hnsw_v2_context_pool_t *pool
) {
	(void)pool;
}

void kvs_hnsw_v2_context_pool_destroy(
	kvs_hnsw_v2_context_pool_t *pool
) {
	size_t i;

	if (!pool) {
		return;
	}

	if (pool->contexts) {
		for (i = 0; i < pool->count; i++) {
			kvs_free(pool->contexts[i].candidates);
			kvs_free(pool->contexts[i].top);
			kvs_free(pool->contexts[i].scratch);
		}
	}
	kvs_free(pool->contexts);
	memset(pool, 0, sizeof(*pool));
}
