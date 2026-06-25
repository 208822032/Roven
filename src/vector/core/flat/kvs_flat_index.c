#include "vector/kvs_vector_flat.h"

#include "kvs_vector.h"
#include "kvs_memory.h"

#include <string.h>

static void kvs_flat_index_clear_storage(kvs_flat_index_t *index) {
	if (!index) {
		return;
	}

	kvs_free(index->vectors);
	kvs_free(index->slot_to_record_id);
	kvs_free(index->deleted_flags);

	index->vectors = NULL;
	index->slot_to_record_id = NULL;
	index->deleted_flags = NULL;

	kvs_vector_id_slot_map_destroy(&index->id_to_slot);

	index->capacity = 0;
	index->size = 0;
	index->deleted_count = 0;
	index->ready = 0;
	index->needs_rebuild = 0;
}

int kvs_flat_index_init(
	kvs_flat_index_t *index,
	size_t dim,
	int metric
) {
	if (!index || dim == 0) {
		return KVS_VECTOR_ERR_INVALID_ARG;
	}

	memset(index, 0, sizeof(*index));

	index->dim = dim;
	index->metric = metric;
	index->ready = 1;
	index->needs_rebuild = 0;

	return KVS_VECTOR_OK;
}

void kvs_flat_index_destroy(
	kvs_flat_index_t *index
) {
	if (!index) {
		return;
	}

	kvs_flat_index_clear_storage(index);
	memset(index, 0, sizeof(*index));
}

int kvs_flat_index_reserve(
	kvs_flat_index_t *index,
	size_t capacity
) {
	float *vectors;
	uint64_t *slot_to_record_id;
	uint8_t *deleted_flags;
	kvs_vector_id_slot_map_t new_map;
	size_t i;
	int ret;

	if (!index || index->dim == 0 || capacity == 0) {
		return KVS_VECTOR_ERR_INVALID_ARG;
	}

	if (capacity <= index->capacity) {
		return KVS_VECTOR_OK;
	}

	if (capacity > UINT32_MAX) {
		return KVS_VECTOR_ERR_INVALID_ARG;
	}

	memset(&new_map, 0, sizeof(new_map));

	vectors = (float *)kvs_malloc(capacity * index->dim * sizeof(float));
	if (!vectors) {
		return KVS_VECTOR_ERR;
	}
	memset(vectors, 0, capacity * index->dim * sizeof(float));

	slot_to_record_id = (uint64_t *)kvs_malloc(capacity * sizeof(uint64_t));
	if (!slot_to_record_id) {
		kvs_free(vectors);
		return KVS_VECTOR_ERR;
	}
	memset(slot_to_record_id, 0, capacity * sizeof(uint64_t));

	deleted_flags = (uint8_t *)kvs_malloc(capacity * sizeof(uint8_t));
	if (!deleted_flags) {
		kvs_free(slot_to_record_id);
		kvs_free(vectors);
		return KVS_VECTOR_ERR;
	}
	memset(deleted_flags, 0, capacity * sizeof(uint8_t));

	if (index->vectors && index->size > 0) {
		memcpy(
			vectors,
			index->vectors,
			index->size * index->dim * sizeof(float)
		);
	}

	if (index->slot_to_record_id && index->size > 0) {
		memcpy(
			slot_to_record_id,
			index->slot_to_record_id,
			index->size * sizeof(uint64_t)
		);
	}

	if (index->deleted_flags && index->size > 0) {
		memcpy(
			deleted_flags,
			index->deleted_flags,
			index->size * sizeof(uint8_t)
		);
	}

	ret = kvs_vector_id_slot_map_init(&new_map, capacity * 2);
	if (ret != KVS_VECTOR_OK) {
		kvs_free(deleted_flags);
		kvs_free(slot_to_record_id);
		kvs_free(vectors);
		return ret;
	}

	for (i = 0; i < index->size; i++) {
		ret = kvs_vector_id_slot_map_put(
			&new_map,
			slot_to_record_id[i],
			(uint32_t)i
		);
		if (ret != KVS_VECTOR_OK) {
			kvs_vector_id_slot_map_destroy(&new_map);
			kvs_free(deleted_flags);
			kvs_free(slot_to_record_id);
			kvs_free(vectors);
			return ret;
		}
	}

	kvs_free(index->vectors);
	kvs_free(index->slot_to_record_id);
	kvs_free(index->deleted_flags);
	kvs_vector_id_slot_map_destroy(&index->id_to_slot);

	index->vectors = vectors;
	index->slot_to_record_id = slot_to_record_id;
	index->deleted_flags = deleted_flags;
	index->id_to_slot = new_map;
	index->capacity = capacity;

	index->needs_rebuild = 0;
	index->ready = 1;

	return KVS_VECTOR_OK;
}

int kvs_flat_index_reset(
	kvs_flat_index_t *index
) {
	if (!index) {
		return KVS_VECTOR_ERR_INVALID_ARG;
	}

	if (index->vectors && index->capacity > 0 && index->dim > 0) {
		memset(index->vectors, 0, index->capacity * index->dim * sizeof(float));
	}

	if (index->slot_to_record_id && index->capacity > 0) {
		memset(index->slot_to_record_id, 0, index->capacity * sizeof(uint64_t));
	}

	if (index->deleted_flags && index->capacity > 0) {
		memset(index->deleted_flags, 0, index->capacity * sizeof(uint8_t));
	}

	kvs_vector_id_slot_map_reset(&index->id_to_slot);

	index->size = 0;
	index->deleted_count = 0;
	index->needs_rebuild = 0;
	index->ready = 1;

	return KVS_VECTOR_OK;
}

int kvs_flat_index_info(
	kvs_flat_index_t *index,
	kvs_vector_core_info_t *out
) {
	size_t memory_bytes = 0;

	if (!index || !out) {
		return KVS_VECTOR_ERR_INVALID_ARG;
	}

	memset(out, 0, sizeof(*out));

	memory_bytes += index->capacity * index->dim * sizeof(float);
	memory_bytes += index->capacity * sizeof(uint64_t);
	memory_bytes += index->capacity * sizeof(uint8_t);
	memory_bytes += index->id_to_slot.capacity *
		sizeof(kvs_vector_id_slot_entry_t);

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
