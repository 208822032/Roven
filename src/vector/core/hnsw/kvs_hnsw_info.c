#include "vector/kvs_vector_hnsw.h"

#include "kvs_vector.h"

#include <string.h>

static size_t kvs_hnsw_context_pool_memory_bytes(
	const kvs_hnsw_search_context_pool_t *pool
) {
	size_t per_context;

	if (!pool || !pool->contexts || pool->count == 0) {
		return 0;
	}

	per_context = sizeof(kvs_hnsw_search_context_t);
	per_context += pool->context_capacity * sizeof(kvs_vector_heap_item_t) * 2;
	per_context += pool->context_capacity * sizeof(uint32_t);
	per_context += pool->context_capacity * sizeof(float);
	per_context += pool->context_capacity * sizeof(kvs_vector_hit_t);

	return pool->count * per_context;
}

int kvs_hnsw_index_info(
	kvs_hnsw_index_t *index,
	kvs_vector_core_info_t *out
) {
	size_t memory_bytes = 0;

	if (!index || !out) {
		return KVS_VECTOR_ERR_INVALID_ARG;
	}

	memset(out, 0, sizeof(*out));

	memory_bytes += index->capacity * index->dim * sizeof(float);
	memory_bytes += index->capacity * sizeof(uint8_t);
	memory_bytes += index->capacity * sizeof(uint8_t);
	memory_bytes += index->capacity * sizeof(uint64_t);
	memory_bytes += index->record_id_to_slot.capacity *
		sizeof(kvs_vector_id_slot_entry_t);
	memory_bytes += kvs_hnsw_graph_level0_bytes(index->capacity, index->M0);
	memory_bytes += index->capacity * index->M0 * sizeof(float);
	memory_bytes += index->capacity * sizeof(uint16_t);
	memory_bytes += index->capacity * sizeof(uint16_t);
	memory_bytes += index->capacity * sizeof(float);
	memory_bytes += index->capacity * sizeof(uint8_t);
	memory_bytes += index->capacity * sizeof(size_t);
	memory_bytes += index->capacity * KVS_HNSW_MAX_LEVEL *
		index->M * sizeof(uint32_t);
	memory_bytes += index->capacity * KVS_HNSW_MAX_LEVEL *
		sizeof(uint16_t);
	memory_bytes += index->capacity * sizeof(uint32_t);
	memory_bytes += kvs_hnsw_context_pool_memory_bytes(&index->search_context_pool);

	out->capacity = index->capacity;
	out->size = index->size;
	out->dim = index->dim;
	out->metric_type = index->metric;
	out->ready = index->ready;
	out->needs_rebuild = index->needs_rebuild;
	out->memory_bytes = memory_bytes;
	out->deleted_count = index->deleted_count;

	return KVS_VECTOR_OK;
}
