#include "vector/kvs_vector_hnsw_v2.h"

#include "kvs_memory.h"

#include <stdlib.h>
#include <string.h>

static size_t kvs_hnsw_v2_align_up_size(
	size_t value,
	size_t alignment
) {
	size_t rem;

	if (alignment == 0) {
		return value;
	}

	rem = value % alignment;
	if (rem == 0) {
		return value;
	}

	return value + (alignment - rem);
}

static size_t kvs_hnsw_v2_vector_stride(
	size_t dim
) {
	size_t floats_per_align;

	floats_per_align = KVS_HNSW_V2_VECTOR_ALIGNMENT / sizeof(float);
	if (floats_per_align == 0) {
		floats_per_align = 1;
	}

	return kvs_hnsw_v2_align_up_size(dim, floats_per_align);
}

static size_t kvs_hnsw_v2_upper_slot_count(
	const kvs_hnsw_v2_index_t *index,
	size_t capacity
) {
	if (!index || index->M == 0 || capacity == 0) {
		return 0;
	}

	if (capacity > ((size_t)-1) / (KVS_HNSW_V2_MAX_LEVEL - 1)) {
		return 0;
	}

	return capacity * (KVS_HNSW_V2_MAX_LEVEL - 1);
}

static int kvs_hnsw_v2_aligned_calloc(
	size_t alignment,
	size_t bytes,
	void **out
) {
	void *ptr = NULL;
	int ret;

	if (!out || bytes == 0) {
		return KVS_VECTOR_ERR_INVALID_ARG;
	}

	ret = posix_memalign(&ptr, alignment, bytes);
	if (ret != 0 || !ptr) {
		return KVS_VECTOR_ERR;
	}

	memset(ptr, 0, bytes);
	*out = ptr;
	return KVS_VECTOR_OK;
}

static void kvs_hnsw_v2_storage_free(
	kvs_hnsw_v2_index_t *index
) {
	if (!index) {
		return;
	}

	free(index->vectors);
	kvs_free(index->slot_to_record_id);
	kvs_vector_id_slot_map_destroy(&index->record_id_to_slot);
	kvs_free(index->levels);
	kvs_free(index->deleted_flags);
	kvs_free(index->visited_tags);
	kvs_free(index->level0_neighbors);
	kvs_free(index->level0_counts);
	kvs_free(index->upper_offsets);
	kvs_free(index->upper_neighbors);
	kvs_free(index->upper_counts);
	kvs_hnsw_v2_context_pool_destroy(&index->context_pool);

	index->vectors = NULL;
	index->slot_to_record_id = NULL;
	index->levels = NULL;
	index->deleted_flags = NULL;
	index->visited_tags = NULL;
	index->level0_neighbors = NULL;
	index->level0_counts = NULL;
	index->upper_offsets = NULL;
	index->upper_neighbors = NULL;
	index->upper_counts = NULL;
}

static int kvs_hnsw_v2_storage_allocate(
	kvs_hnsw_v2_index_t *index,
	size_t capacity,
	kvs_hnsw_v2_index_t *storage
) {
	size_t vector_bytes;
	size_t level0_slots;
	size_t upper_slots;
	size_t i;
	int ret;

	if (!index || !storage || capacity == 0) {
		return KVS_VECTOR_ERR_INVALID_ARG;
	}

	if (capacity > UINT32_MAX) {
		return KVS_VECTOR_ERR_INVALID_ARG;
	}
	if (index->M == 0 || index->M0 == 0 ||
	    index->M > UINT16_MAX || index->M0 > UINT16_MAX) {
		return KVS_VECTOR_ERR_INVALID_ARG;
	}
	if (index->M0 < index->M) {
		return KVS_VECTOR_ERR_INVALID_ARG;
	}
	if (index->M0 > 0 && capacity > ((size_t)-1) / index->M0) {
		return KVS_VECTOR_ERR_INVALID_ARG;
	}
	level0_slots = capacity * index->M0;
	if (level0_slots > ((size_t)-1) / sizeof(uint32_t)) {
		return KVS_VECTOR_ERR_INVALID_ARG;
	}
	if (capacity > ((size_t)-1) / sizeof(size_t)) {
		return KVS_VECTOR_ERR_INVALID_ARG;
	}

	memset(storage, 0, sizeof(*storage));
	storage->dim = index->dim;
	storage->metric = index->metric;
	storage->M = index->M;
	storage->M0 = index->M0;
	storage->ef_construction = index->ef_construction;
	storage->ef_search = index->ef_search;
	storage->capacity = capacity;
	storage->vector_stride = kvs_hnsw_v2_vector_stride(index->dim);

	if (storage->vector_stride > ((size_t)-1) / capacity ||
	    storage->vector_stride * capacity > ((size_t)-1) / sizeof(float)) {
		return KVS_VECTOR_ERR_INVALID_ARG;
	}
	vector_bytes = capacity * storage->vector_stride * sizeof(float);

	ret = kvs_hnsw_v2_aligned_calloc(
		KVS_HNSW_V2_VECTOR_ALIGNMENT,
		vector_bytes,
		(void **)&storage->vectors
	);
	if (ret != KVS_VECTOR_OK) {
		goto fail;
	}

	storage->slot_to_record_id = (uint64_t *)kvs_malloc(
		capacity * sizeof(uint64_t)
	);
	if (!storage->slot_to_record_id) {
		ret = KVS_VECTOR_ERR;
		goto fail;
	}
	memset(storage->slot_to_record_id, 0, capacity * sizeof(uint64_t));

	ret = kvs_vector_id_slot_map_init(
		&storage->record_id_to_slot,
		capacity * 2
	);
	if (ret != KVS_VECTOR_OK) {
		goto fail;
	}

	storage->levels = (uint8_t *)kvs_malloc(capacity * sizeof(uint8_t));
	if (!storage->levels) {
		ret = KVS_VECTOR_ERR;
		goto fail;
	}
	memset(storage->levels, 0, capacity * sizeof(uint8_t));

	storage->deleted_flags = (uint8_t *)kvs_malloc(capacity * sizeof(uint8_t));
	if (!storage->deleted_flags) {
		ret = KVS_VECTOR_ERR;
		goto fail;
	}
	memset(storage->deleted_flags, 0, capacity * sizeof(uint8_t));

	storage->visited_tags = (uint32_t *)kvs_malloc(capacity * sizeof(uint32_t));
	if (!storage->visited_tags) {
		ret = KVS_VECTOR_ERR;
		goto fail;
	}
	memset(storage->visited_tags, 0, capacity * sizeof(uint32_t));

	storage->level0_neighbors = (uint32_t *)kvs_malloc(
		level0_slots * sizeof(uint32_t)
	);
	if (!storage->level0_neighbors) {
		ret = KVS_VECTOR_ERR;
		goto fail;
	}
	memset(
		storage->level0_neighbors,
		0,
		level0_slots * sizeof(uint32_t)
	);

	storage->level0_counts = (uint16_t *)kvs_malloc(
		capacity * sizeof(uint16_t)
	);
	if (!storage->level0_counts) {
		ret = KVS_VECTOR_ERR;
		goto fail;
	}
	memset(storage->level0_counts, 0, capacity * sizeof(uint16_t));

	storage->upper_offsets = (size_t *)kvs_malloc(capacity * sizeof(size_t));
	if (!storage->upper_offsets) {
		ret = KVS_VECTOR_ERR;
		goto fail;
	}
	memset(storage->upper_offsets, 0, capacity * sizeof(size_t));

	upper_slots = kvs_hnsw_v2_upper_slot_count(index, capacity);
	if (upper_slots == 0 ||
	    upper_slots > ((size_t)-1) / index->M ||
	    upper_slots * index->M > ((size_t)-1) / sizeof(uint32_t) ||
	    upper_slots > ((size_t)-1) / sizeof(uint16_t)) {
		ret = KVS_VECTOR_ERR_INVALID_ARG;
		goto fail;
	}

	storage->upper_counts = (uint16_t *)kvs_malloc(
		upper_slots * sizeof(uint16_t)
	);
	if (!storage->upper_counts) {
		ret = KVS_VECTOR_ERR;
		goto fail;
	}
	memset(storage->upper_counts, 0, upper_slots * sizeof(uint16_t));

	storage->upper_neighbors = (uint32_t *)kvs_malloc(
		upper_slots * index->M * sizeof(uint32_t)
	);
	if (!storage->upper_neighbors) {
		ret = KVS_VECTOR_ERR;
		goto fail;
	}
	memset(
		storage->upper_neighbors,
		0,
		upper_slots * index->M * sizeof(uint32_t)
	);

	for (i = 0; i < capacity; i++) {
		storage->upper_offsets[i] = i * (KVS_HNSW_V2_MAX_LEVEL - 1) * index->M;
	}

	ret = kvs_hnsw_v2_context_pool_init(
		&storage->context_pool,
		KVS_HNSW_V2_DEFAULT_CONTEXT_COUNT
	);
	if (ret != KVS_VECTOR_OK) {
		goto fail;
	}

	storage->current_visit_tag = 1;
	storage->ready = 1;
	storage->needs_rebuild = 0;
	kvs_hnsw_v2_random_init(
		&storage->rng,
		0x6a09e667f3bcc909ULL ^ ((uint64_t)index->dim << 32) ^
			(uint64_t)(uint32_t)index->metric
	);
	kvs_hnsw_v2_memory_refresh(storage);

	return KVS_VECTOR_OK;

fail:
	kvs_hnsw_v2_storage_free(storage);
	return ret;
}

int kvs_hnsw_v2_index_init(
	kvs_hnsw_v2_index_t *index,
	size_t dim,
	int metric
) {
	if (!index || dim == 0) {
		return KVS_VECTOR_ERR_INVALID_ARG;
	}

	memset(index, 0, sizeof(*index));
	index->build_profiling_enabled = 1;
	index->search_profiling_enabled = 1;
	index->dim = dim;
	index->metric = metric;
	index->M = KVS_HNSW_V2_DEFAULT_M;
	index->M0 = KVS_HNSW_V2_DEFAULT_M0;
	index->ef_construction = KVS_HNSW_V2_DEFAULT_EF_CONSTRUCTION;
	index->ef_search = KVS_HNSW_V2_DEFAULT_EF_SEARCH;
	index->vector_stride = kvs_hnsw_v2_vector_stride(dim);
	index->entry_point = 0;
	index->has_entry = 0;
	index->max_level = 0;
	index->current_visit_tag = 1;
	kvs_hnsw_v2_random_init(
		&index->rng,
		0x6a09e667f3bcc909ULL ^ ((uint64_t)dim << 32) ^
			(uint64_t)(uint32_t)metric
	);
	index->ready = 1;
	index->needs_rebuild = 0;
	kvs_hnsw_v2_memory_refresh(index);

	return KVS_VECTOR_OK;
}

int kvs_hnsw_v2_index_reserve(
	kvs_hnsw_v2_index_t *index,
	size_t capacity
) {
	kvs_hnsw_v2_index_t storage;
	int ret;

	if (!index || index->dim == 0 || capacity == 0) {
		return KVS_VECTOR_ERR_INVALID_ARG;
	}

	if (capacity <= index->capacity) {
		return KVS_VECTOR_OK;
	}

	ret = kvs_hnsw_v2_storage_allocate(index, capacity, &storage);
	if (ret != KVS_VECTOR_OK) {
		return ret;
	}

	kvs_hnsw_v2_storage_free(index);

	index->capacity = storage.capacity;
	index->vector_stride = storage.vector_stride;
	index->vectors = storage.vectors;
	index->slot_to_record_id = storage.slot_to_record_id;
	index->record_id_to_slot = storage.record_id_to_slot;
	index->levels = storage.levels;
	index->deleted_flags = storage.deleted_flags;
	index->visited_tags = storage.visited_tags;
	index->level0_neighbors = storage.level0_neighbors;
	index->level0_counts = storage.level0_counts;
	index->upper_offsets = storage.upper_offsets;
	index->upper_neighbors = storage.upper_neighbors;
	index->upper_counts = storage.upper_counts;
	index->rng = storage.rng;
	index->context_pool = storage.context_pool;
	index->size = 0;
	index->deleted_count = 0;
	index->entry_point = 0;
	index->has_entry = 0;
	index->max_level = 0;
	index->current_visit_tag = 1;
	index->ready = 1;
	index->needs_rebuild = 0;
	kvs_hnsw_v2_memory_refresh(index);

	memset(&storage, 0, sizeof(storage));
	return KVS_VECTOR_OK;
}

int kvs_hnsw_v2_index_reset(
	kvs_hnsw_v2_index_t *index
) {
	if (!index) {
		return KVS_VECTOR_ERR_INVALID_ARG;
	}

	if (index->vectors && index->capacity > 0) {
		memset(
			index->vectors,
			0,
			index->capacity * index->vector_stride * sizeof(float)
		);
	}
	if (index->slot_to_record_id && index->capacity > 0) {
		memset(index->slot_to_record_id, 0, index->capacity * sizeof(uint64_t));
	}
	if (index->levels && index->capacity > 0) {
		memset(index->levels, 0, index->capacity * sizeof(uint8_t));
	}
	if (index->deleted_flags && index->capacity > 0) {
		memset(index->deleted_flags, 0, index->capacity * sizeof(uint8_t));
	}
	if (index->visited_tags && index->capacity > 0) {
		memset(index->visited_tags, 0, index->capacity * sizeof(uint32_t));
	}
	if (index->level0_neighbors && index->capacity > 0) {
		memset(
			index->level0_neighbors,
			0,
			index->capacity * index->M0 * sizeof(uint32_t)
		);
	}
	if (index->level0_counts && index->capacity > 0) {
		memset(index->level0_counts, 0, index->capacity * sizeof(uint16_t));
	}
	if (index->upper_counts && index->capacity > 0) {
		memset(
			index->upper_counts,
			0,
			index->capacity * (KVS_HNSW_V2_MAX_LEVEL - 1) *
				sizeof(uint16_t)
		);
	}
	if (index->upper_neighbors && index->capacity > 0) {
		memset(
			index->upper_neighbors,
			0,
			index->capacity * (KVS_HNSW_V2_MAX_LEVEL - 1) *
				index->M * sizeof(uint32_t)
		);
	}
	kvs_vector_id_slot_map_reset(&index->record_id_to_slot);
	kvs_hnsw_v2_context_pool_reset(&index->context_pool);

	index->size = 0;
	index->deleted_count = 0;
	index->entry_point = 0;
	index->has_entry = 0;
	index->max_level = 0;
	index->current_visit_tag = 1;
	memset(&index->graph_stats, 0, sizeof(index->graph_stats));
	memset(&index->build_stats, 0, sizeof(index->build_stats));
	memset(&index->search_stats, 0, sizeof(index->search_stats));
	kvs_hnsw_v2_random_init(
		&index->rng,
		0x6a09e667f3bcc909ULL ^ ((uint64_t)index->dim << 32) ^
			(uint64_t)(uint32_t)index->metric
	);
	index->ready = 1;
	index->needs_rebuild = 0;
	kvs_hnsw_v2_memory_refresh(index);

	return KVS_VECTOR_OK;
}

int kvs_hnsw_v2_index_mark_delete(
	kvs_hnsw_v2_index_t *index,
	uint64_t record_id
) {
	uint32_t slot;
	int ret;

	if (!index) {
		return KVS_VECTOR_ERR_INVALID_ARG;
	}
	if (!index->ready || !index->deleted_flags || !index->slot_to_record_id) {
		return KVS_VECTOR_ERR_NOT_INITIALIZED;
	}

	ret = kvs_vector_id_slot_map_get(&index->record_id_to_slot, record_id, &slot);
	if (ret != KVS_VECTOR_OK) {
		return ret;
	}
	if (slot >= index->size || index->slot_to_record_id[slot] != record_id) {
		return KVS_VECTOR_ERR_NOT_FOUND;
	}

	if (!index->deleted_flags[slot]) {
		index->deleted_flags[slot] = 1;
		index->deleted_count++;
	}

	return KVS_VECTOR_OK;
}

void kvs_hnsw_v2_index_destroy(
	kvs_hnsw_v2_index_t *index
) {
	if (!index) {
		return;
	}

	kvs_hnsw_v2_storage_free(index);
	memset(index, 0, sizeof(*index));
}
