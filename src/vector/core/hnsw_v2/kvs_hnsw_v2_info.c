#include "vector/kvs_vector_hnsw_v2.h"

#include <string.h>

void kvs_hnsw_v2_memory_refresh(
	kvs_hnsw_v2_index_t *index
) {
	kvs_hnsw_v2_memory_t *memory;
	size_t total = 0;

	if (!index) {
		return;
	}

	memory = &index->memory;
	memset(memory, 0, sizeof(*memory));

	memory->vectors_bytes = index->capacity *
		index->vector_stride * sizeof(float);
	memory->slot_to_record_id_bytes = index->capacity * sizeof(uint64_t);
	memory->record_id_to_slot_bytes = index->record_id_to_slot.capacity *
		sizeof(kvs_vector_id_slot_entry_t);
	memory->levels_bytes = index->capacity * sizeof(uint8_t);
	memory->deleted_flags_bytes = index->capacity * sizeof(uint8_t);
	memory->visited_tags_bytes = index->capacity * sizeof(uint32_t);
	memory->level0_graph_bytes = index->capacity * index->M0 *
		sizeof(uint32_t);
	memory->level0_graph_bytes += index->capacity * sizeof(uint16_t);
	memory->upper_graph_bytes = index->capacity * sizeof(size_t);
	memory->upper_graph_bytes += index->capacity *
		(KVS_HNSW_V2_MAX_LEVEL - 1) * sizeof(uint16_t);
	memory->upper_graph_bytes += index->capacity *
		(KVS_HNSW_V2_MAX_LEVEL - 1) * index->M * sizeof(uint32_t);
	memory->context_bytes = index->context_pool.bytes;
	if (index->context_pool.contexts) {
		size_t i;

		for (i = 0; i < index->context_pool.count; i++) {
			memory->context_bytes += index->context_pool.contexts[i].candidates_capacity *
				sizeof(kvs_hnsw_v2_candidate_t);
			memory->context_bytes += index->context_pool.contexts[i].top_capacity *
				sizeof(kvs_hnsw_v2_candidate_t);
			memory->context_bytes += index->context_pool.contexts[i].scratch_capacity *
				sizeof(kvs_hnsw_v2_candidate_t);
		}
	}

	total += memory->vectors_bytes;
	total += memory->slot_to_record_id_bytes;
	total += memory->record_id_to_slot_bytes;
	total += memory->levels_bytes;
	total += memory->deleted_flags_bytes;
	total += memory->visited_tags_bytes;
	total += memory->level0_graph_bytes;
	total += memory->upper_graph_bytes;
	total += memory->context_bytes;
	memory->total_bytes = total;
}

int kvs_hnsw_v2_index_info(
	const kvs_hnsw_v2_index_t *index,
	kvs_vector_core_info_t *out
) {
	if (!index || !out) {
		return KVS_VECTOR_ERR_INVALID_ARG;
	}

	memset(out, 0, sizeof(*out));
	out->capacity = index->capacity;
	out->size = index->size;
	out->dim = index->dim;
	out->metric_type = index->metric;
	out->ready = index->ready;
	out->needs_rebuild = index->needs_rebuild;
	out->memory_bytes = index->memory.total_bytes;
	out->deleted_count = index->deleted_count;

	return KVS_VECTOR_OK;
}
