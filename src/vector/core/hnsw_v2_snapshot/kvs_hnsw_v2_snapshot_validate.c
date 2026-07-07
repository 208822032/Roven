#include "vector/kvs_vector_hnsw_v2_snapshot.h"

#include "kvs_hnsw_v2_snapshot_format.h"

#include <stdint.h>
#include <string.h>

static int kvs_hnsw_v2_snapshot_validate_fail(
	char *reason,
	size_t reason_size,
	const char *message
) {
	kvs_hnsw_v2_snapshot_set_reason(reason, reason_size, message);
	return KVS_VECTOR_ERR;
}

static int kvs_hnsw_v2_snapshot_check_neighbor_list(
	const kvs_hnsw_v2_index_t *index,
	uint32_t slot,
	uint8_t level,
	const uint32_t *neighbors,
	uint16_t count,
	size_t capacity,
	char *reason,
	size_t reason_size
) {
	uint16_t i;
	uint16_t j;

	if (count > capacity) {
		return kvs_hnsw_v2_snapshot_validate_fail(
			reason,
			reason_size,
			"degree overflow"
		);
	}

	for (i = 0; i < count; i++) {
		uint32_t neighbor = neighbors[i];

		if (neighbor >= index->size) {
			return kvs_hnsw_v2_snapshot_validate_fail(
				reason,
				reason_size,
				"neighbor out of range"
			);
		}
		if (neighbor == slot) {
			return kvs_hnsw_v2_snapshot_validate_fail(
				reason,
				reason_size,
				"self edge"
			);
		}
		if (level > 0 && index->levels[neighbor] < level) {
			return kvs_hnsw_v2_snapshot_validate_fail(
				reason,
				reason_size,
				"upper neighbor level mismatch"
			);
		}
		for (j = (uint16_t)(i + 1); j < count; j++) {
			if (neighbors[j] == neighbor) {
				return kvs_hnsw_v2_snapshot_validate_fail(
					reason,
					reason_size,
					"duplicate edge"
				);
			}
		}
	}

	return KVS_VECTOR_OK;
}

static int kvs_hnsw_v2_snapshot_check_active_ids(
	const kvs_hnsw_v2_index_t *index,
	char *reason,
	size_t reason_size
) {
	kvs_vector_id_slot_map_t active;
	size_t i;
	int ret;

	memset(&active, 0, sizeof(active));
	ret = kvs_vector_id_slot_map_init(&active, index->size * 2 + 1);
	if (ret != KVS_VECTOR_OK) {
		return kvs_hnsw_v2_snapshot_validate_fail(
			reason,
			reason_size,
			"id map allocation failed"
		);
	}

	for (i = 0; i < index->size; i++) {
		uint32_t old_slot = 0;
		uint64_t record_id = index->slot_to_record_id[i];

		if (index->deleted_flags[i]) {
			continue;
		}
		if (kvs_vector_id_slot_map_get(&active, record_id, &old_slot) ==
		    KVS_VECTOR_OK) {
			kvs_vector_id_slot_map_destroy(&active);
			return kvs_hnsw_v2_snapshot_validate_fail(
				reason,
				reason_size,
				"duplicate active id"
			);
		}
		ret = kvs_vector_id_slot_map_put(&active, record_id, (uint32_t)i);
		if (ret != KVS_VECTOR_OK) {
			kvs_vector_id_slot_map_destroy(&active);
			return kvs_hnsw_v2_snapshot_validate_fail(
				reason,
				reason_size,
				"id map insert failed"
			);
		}
	}

	kvs_vector_id_slot_map_destroy(&active);
	return KVS_VECTOR_OK;
}

int kvs_hnsw_v2_index_validate_basic(
	const kvs_hnsw_v2_index_t *index,
	char *reason,
	size_t reason_size
) {
	size_t i;
	size_t level;
	uint64_t deleted_count = 0;
	uint8_t observed_max_level = 0;

	kvs_hnsw_v2_snapshot_set_reason(reason, reason_size, "ok");

	if (!index) {
		return kvs_hnsw_v2_snapshot_validate_fail(
			reason,
			reason_size,
			"null index"
		);
	}
	if (!index->ready) {
		return kvs_hnsw_v2_snapshot_validate_fail(
			reason,
			reason_size,
			"index not ready"
		);
	}
	if (index->dim == 0 || index->M == 0 || index->M0 == 0 ||
	    index->M0 < index->M || index->M > UINT16_MAX ||
	    index->M0 > UINT16_MAX) {
		return kvs_hnsw_v2_snapshot_validate_fail(
			reason,
			reason_size,
			"invalid params"
		);
	}
	if (index->capacity > UINT32_MAX || index->size > index->capacity ||
	    index->deleted_count > index->size) {
		return kvs_hnsw_v2_snapshot_validate_fail(
			reason,
			reason_size,
			"invalid size"
		);
	}
	if (index->vector_stride != kvs_hnsw_v2_snapshot_vector_stride(index->dim)) {
		return kvs_hnsw_v2_snapshot_validate_fail(
			reason,
			reason_size,
			"invalid vector stride"
		);
	}
	if (index->max_level >= KVS_HNSW_V2_MAX_LEVEL) {
		return kvs_hnsw_v2_snapshot_validate_fail(
			reason,
			reason_size,
			"invalid max level"
		);
	}

	if (index->size == 0) {
		if (index->deleted_count != 0 || index->has_entry ||
		    index->max_level != 0) {
			return kvs_hnsw_v2_snapshot_validate_fail(
				reason,
				reason_size,
				"invalid empty state"
			);
		}
		return KVS_VECTOR_OK;
	}

	if (!index->vectors || !index->slot_to_record_id ||
	    !index->record_id_to_slot.entries || !index->levels ||
	    !index->deleted_flags || !index->level0_neighbors ||
	    !index->level0_counts || !index->upper_offsets ||
	    !index->upper_neighbors || !index->upper_counts) {
		return kvs_hnsw_v2_snapshot_validate_fail(
			reason,
			reason_size,
			"missing storage"
		);
	}
	if (!index->has_entry || index->entry_point >= index->size) {
		return kvs_hnsw_v2_snapshot_validate_fail(
			reason,
			reason_size,
			"invalid entry"
		);
	}

	for (i = 0; i < index->size; i++) {
		uint8_t node_level = index->levels[i];
		uint32_t *neighbors0;
		uint16_t count0;

		if (node_level >= KVS_HNSW_V2_MAX_LEVEL) {
			return kvs_hnsw_v2_snapshot_validate_fail(
				reason,
				reason_size,
				"node level out of range"
			);
		}
		if (node_level > observed_max_level) {
			observed_max_level = node_level;
		}
		if (index->deleted_flags[i] > 1) {
			return kvs_hnsw_v2_snapshot_validate_fail(
				reason,
				reason_size,
				"invalid deleted flag"
			);
		}
		if (index->deleted_flags[i]) {
			deleted_count++;
		}

		neighbors0 = index->level0_neighbors + i * index->M0;
		count0 = index->level0_counts[i];
		if (kvs_hnsw_v2_snapshot_check_neighbor_list(
			    index,
			    (uint32_t)i,
			    0,
			    neighbors0,
			    count0,
			    index->M0,
			    reason,
			    reason_size
		    ) != KVS_VECTOR_OK) {
			return KVS_VECTOR_ERR;
		}

		for (level = 1; level < KVS_HNSW_V2_MAX_LEVEL; level++) {
			size_t count_index =
				i * (KVS_HNSW_V2_MAX_LEVEL - 1) + (level - 1);
			uint16_t count = index->upper_counts[count_index];
			uint32_t *neighbors = index->upper_neighbors +
				index->upper_offsets[i] + (level - 1) * index->M;

			if (level > node_level) {
				if (count != 0) {
					return kvs_hnsw_v2_snapshot_validate_fail(
						reason,
						reason_size,
						"upper count above node level"
					);
				}
				continue;
			}
			if (kvs_hnsw_v2_snapshot_check_neighbor_list(
				    index,
				    (uint32_t)i,
				    (uint8_t)level,
				    neighbors,
				    count,
				    index->M,
				    reason,
				    reason_size
			    ) != KVS_VECTOR_OK) {
				return KVS_VECTOR_ERR;
			}
		}
	}

	if (deleted_count != index->deleted_count) {
		return kvs_hnsw_v2_snapshot_validate_fail(
			reason,
			reason_size,
			"deleted count mismatch"
		);
	}
	if (observed_max_level != index->max_level ||
	    index->levels[index->entry_point] != index->max_level) {
		return kvs_hnsw_v2_snapshot_validate_fail(
			reason,
			reason_size,
			"entry level mismatch"
		);
	}

	return kvs_hnsw_v2_snapshot_check_active_ids(index, reason, reason_size);
}
