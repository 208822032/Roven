#include "vector/kvs_vector_hnsw.h"

#include "kvs_memory.h"
#include "kvs_vector.h"

#include <string.h>

static void kvs_hnsw_context_destroy(
	kvs_hnsw_search_context_t *ctx
) {
	if (!ctx) {
		return;
	}

	kvs_vector_heap_destroy(&ctx->candidate_heap);
	kvs_vector_heap_destroy(&ctx->top_heap);
	kvs_free(ctx->neighbor_buffer);
	kvs_free(ctx->distance_buffer);
	kvs_free(ctx->hits);
	memset(ctx, 0, sizeof(*ctx));
}

static int kvs_hnsw_context_init(
	kvs_hnsw_search_context_t *ctx,
	size_t capacity
) {
	int ret;

	if (!ctx || capacity == 0) {
		return KVS_VECTOR_ERR_INVALID_ARG;
	}

	memset(ctx, 0, sizeof(*ctx));

	ret = kvs_vector_heap_init(
		&ctx->candidate_heap,
		capacity,
		kvs_vector_heap_max_score_less
	);
	if (ret != KVS_VECTOR_OK) {
		kvs_hnsw_context_destroy(ctx);
		return ret;
	}

	ret = kvs_vector_heap_init(
		&ctx->top_heap,
		capacity,
		kvs_vector_heap_min_score_less
	);
	if (ret != KVS_VECTOR_OK) {
		kvs_hnsw_context_destroy(ctx);
		return ret;
	}

	ctx->neighbor_buffer = (uint32_t *)kvs_malloc(capacity * sizeof(uint32_t));
	if (!ctx->neighbor_buffer) {
		kvs_hnsw_context_destroy(ctx);
		return KVS_VECTOR_ERR;
	}
	memset(ctx->neighbor_buffer, 0, capacity * sizeof(uint32_t));

	ctx->distance_buffer = (float *)kvs_malloc(capacity * sizeof(float));
	if (!ctx->distance_buffer) {
		kvs_hnsw_context_destroy(ctx);
		return KVS_VECTOR_ERR;
	}
	memset(ctx->distance_buffer, 0, capacity * sizeof(float));

	ctx->hits = (kvs_vector_hit_t *)kvs_malloc(capacity * sizeof(kvs_vector_hit_t));
	if (!ctx->hits) {
		kvs_hnsw_context_destroy(ctx);
		return KVS_VECTOR_ERR;
	}
	memset(ctx->hits, 0, capacity * sizeof(kvs_vector_hit_t));

	ctx->capacity = capacity;
	ctx->in_use = 0;
	return KVS_VECTOR_OK;
}

int kvs_hnsw_context_pool_init(
	kvs_hnsw_search_context_pool_t *pool,
	size_t count,
	size_t context_capacity
) {
	size_t i;
	int ret;

	if (!pool || count == 0 || context_capacity == 0) {
		return KVS_VECTOR_ERR_INVALID_ARG;
	}

	memset(pool, 0, sizeof(*pool));

	pool->contexts = (kvs_hnsw_search_context_t *)kvs_malloc(
		count * sizeof(kvs_hnsw_search_context_t)
	);
	if (!pool->contexts) {
		return KVS_VECTOR_ERR;
	}
	memset(pool->contexts, 0, count * sizeof(kvs_hnsw_search_context_t));

	pool->count = count;
	pool->context_capacity = context_capacity;

	for (i = 0; i < count; i++) {
		ret = kvs_hnsw_context_init(&pool->contexts[i], context_capacity);
		if (ret != KVS_VECTOR_OK) {
			kvs_hnsw_context_pool_destroy(pool);
			return ret;
		}
	}

	return KVS_VECTOR_OK;
}

void kvs_hnsw_context_pool_destroy(
	kvs_hnsw_search_context_pool_t *pool
) {
	size_t i;

	if (!pool) {
		return;
	}

	if (pool->contexts) {
		for (i = 0; i < pool->count; i++) {
			kvs_hnsw_context_destroy(&pool->contexts[i]);
		}
	}

	kvs_free(pool->contexts);
	memset(pool, 0, sizeof(*pool));
}

kvs_hnsw_search_context_t *kvs_hnsw_context_acquire(
	kvs_hnsw_search_context_pool_t *pool
) {
	size_t i;

	if (!pool || !pool->contexts) {
		return NULL;
	}

	for (i = 0; i < pool->count; i++) {
		if (!pool->contexts[i].in_use) {
			pool->contexts[i].in_use = 1;
			kvs_hnsw_context_reset(&pool->contexts[i]);
			return &pool->contexts[i];
		}
	}

	return NULL;
}

void kvs_hnsw_context_release(
	kvs_hnsw_search_context_pool_t *pool,
	kvs_hnsw_search_context_t *ctx
) {
	(void)pool;

	if (!ctx) {
		return;
	}

	kvs_hnsw_context_reset(ctx);
	ctx->in_use = 0;
}

void kvs_hnsw_context_reset(
	kvs_hnsw_search_context_t *ctx
) {
	if (!ctx) {
		return;
	}

	kvs_vector_heap_reset(&ctx->candidate_heap);
	kvs_vector_heap_reset(&ctx->top_heap);
	if (ctx->neighbor_buffer && ctx->capacity > 0) {
		memset(ctx->neighbor_buffer, 0, ctx->capacity * sizeof(uint32_t));
	}
	if (ctx->distance_buffer && ctx->capacity > 0) {
		memset(ctx->distance_buffer, 0, ctx->capacity * sizeof(float));
	}
	if (ctx->hits && ctx->capacity > 0) {
		memset(ctx->hits, 0, ctx->capacity * sizeof(kvs_vector_hit_t));
	}
}
