#include "vector/kvs_vector_hnsw.h"

#include "kvs_vector.h"

int kvs_hnsw_index_mark_delete(
	kvs_hnsw_index_t *index,
	uint64_t record_id
) {
	uint32_t slot = 0;
	int ret;

	if (!index || !index->deleted_flags || !index->record_id_to_slot.entries) {
		return KVS_VECTOR_ERR_INVALID_ARG;
	}

	ret = kvs_vector_id_slot_map_get(&index->record_id_to_slot, record_id, &slot);
	if (ret != KVS_VECTOR_OK) {
		return ret;
	}

	if ((size_t)slot >= index->size) {
		return KVS_VECTOR_ERR;
	}

	if (index->deleted_flags[slot]) {
		return KVS_VECTOR_OK;
	}

	index->deleted_flags[slot] = 1;
	index->deleted_count++;
	return KVS_VECTOR_OK;
}
