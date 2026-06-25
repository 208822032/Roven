#include "vector/kvs_vector_flat.h"

#include "kvs_vector.h"

#include <stdint.h>
#include <string.h>

#define KVS_FLAT_MIN_CAPACITY 16

static size_t kvs_flat_next_capacity(size_t current) {
	if (current == 0) {
		return KVS_FLAT_MIN_CAPACITY;
	}

	return current * 2;
}

int kvs_flat_index_add(
	kvs_flat_index_t *index,
	uint64_t record_id,
	const float *vector,
	size_t dim
) {
	uint32_t slot = 0;
	int ret;

	if (!index || !vector || dim == 0) {
		return KVS_VECTOR_ERR_INVALID_ARG;
	}

	if (!index->ready || index->dim == 0 || dim != index->dim) {
		return KVS_VECTOR_ERR_INVALID_ARG;
	}

	/*
	 * Update existing record_id in place.
	 */
	if (index->capacity > 0 && index->id_to_slot.entries) {
		ret = kvs_vector_id_slot_map_get(
			&index->id_to_slot,
			record_id,
			&slot
		);
		if (ret == KVS_VECTOR_OK) {
			if ((size_t)slot >= index->capacity) {
				return KVS_VECTOR_ERR;
			}

			memcpy(
				index->vectors + ((size_t)slot * index->dim),
				vector,
				index->dim * sizeof(float)
			);

			index->slot_to_record_id[slot] = record_id;

			if (index->deleted_flags[slot]) {
				index->deleted_flags[slot] = 0;
				if (index->deleted_count > 0) {
					index->deleted_count--;
				}
			}

			return KVS_VECTOR_OK;
		}
	}

	/*
	 * Append new record_id.
	 */
	if (index->size >= index->capacity) {
		size_t new_capacity = kvs_flat_next_capacity(index->capacity);

		while (new_capacity <= index->size) {
			new_capacity *= 2;
		}

		ret = kvs_flat_index_reserve(index, new_capacity);
		if (ret != KVS_VECTOR_OK) {
			return ret;
		}
	}

	if (index->size > UINT32_MAX) {
		return KVS_VECTOR_ERR_INVALID_ARG;
	}

	slot = (uint32_t)index->size;

	memcpy(
		index->vectors + ((size_t)slot * index->dim),
		vector,
		index->dim * sizeof(float)
	);

	index->slot_to_record_id[slot] = record_id;
	index->deleted_flags[slot] = 0;

	ret = kvs_vector_id_slot_map_put(
		&index->id_to_slot,
		record_id,
		slot
	);
	if (ret != KVS_VECTOR_OK) {
		memset(
			index->vectors + ((size_t)slot * index->dim),
			0,
			index->dim * sizeof(float)
		);
		index->slot_to_record_id[slot] = 0;
		index->deleted_flags[slot] = 0;
		return ret;
	}

	index->size++;
	index->ready = 1;
	index->needs_rebuild = 0;

	return KVS_VECTOR_OK;
}

int kvs_flat_index_mark_delete(
	kvs_flat_index_t *index,
	uint64_t record_id
) {
	uint32_t slot = 0;
	int ret;

	if (!index) {
		return KVS_VECTOR_ERR_INVALID_ARG;
	}

	if (!index->ready || !index->deleted_flags ||
	    !index->slot_to_record_id || !index->id_to_slot.entries) {
		return KVS_VECTOR_ERR_INVALID_ARG;
	}

	ret = kvs_vector_id_slot_map_get(
		&index->id_to_slot,
		record_id,
		&slot
	);
	if (ret != KVS_VECTOR_OK) {
		return ret;
	}

	if ((size_t)slot >= index->size || (size_t)slot >= index->capacity) {
		return KVS_VECTOR_ERR;
	}

	if (index->slot_to_record_id[slot] != record_id) {
		return KVS_VECTOR_ERR;
	}

	if (index->deleted_flags[slot]) {
		return KVS_VECTOR_OK;
	}

	index->deleted_flags[slot] = 1;
	index->deleted_count++;

	return KVS_VECTOR_OK;
}
