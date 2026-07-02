#include "vector/kvs_vector_hnsw.h"

#include "kvs_memory.h"
#include "kvs_vector.h"

#include <string.h>
#include <time.h>

uint64_t kvs_hnsw_profile_now_us(void) {
	struct timespec ts;

	if (clock_gettime(CLOCK_MONOTONIC, &ts) != 0) {
		return 0;
	}

	return (uint64_t)ts.tv_sec * 1000000ull + (uint64_t)ts.tv_nsec / 1000ull;
}

void kvs_hnsw_rebuild_profile_reset(
	kvs_hnsw_index_t *index
) {
	if (!index) {
		return;
	}

	memset(&index->rebuild_profile, 0, sizeof(index->rebuild_profile));
}

void kvs_hnsw_rebuild_profile_record_add(
	kvs_hnsw_index_t *index,
	uint64_t elapsed_us
) {
	kvs_hnsw_rebuild_profile_t *profile;

	if (!index) {
		return;
	}

	profile = &index->rebuild_profile;
	profile->rebuild_core_add_count++;
	profile->rebuild_core_add_total_us += elapsed_us;
	if (elapsed_us > profile->rebuild_core_add_max_us) {
		profile->rebuild_core_add_max_us = elapsed_us;
	}
}

static void kvs_hnsw_index_clear_storage(kvs_hnsw_index_t *index) {
	if (!index) {
		return;
	}

	kvs_free(index->vectors);
	kvs_free(index->levels);
	kvs_free(index->deleted_flags);
	kvs_free(index->slot_to_record_id);
	kvs_free(index->level0_neighbors);
	kvs_free(index->level0_neighbor_scores);
	kvs_free(index->level0_counts);
	kvs_free(index->level0_worst_pos);
	kvs_free(index->level0_worst_score);
	kvs_free(index->level0_worst_valid);
	kvs_free(index->upper_offsets);
	kvs_free(index->upper_neighbors);
	kvs_free(index->upper_counts);
	kvs_free(index->visited_tags);

	index->vectors = NULL;
	index->levels = NULL;
	index->deleted_flags = NULL;
	index->slot_to_record_id = NULL;
	index->level0_neighbors = NULL;
	index->level0_neighbor_scores = NULL;
	index->level0_counts = NULL;
	index->level0_worst_pos = NULL;
	index->level0_worst_score = NULL;
	index->level0_worst_valid = NULL;
	index->upper_offsets = NULL;
	index->upper_neighbors = NULL;
	index->upper_counts = NULL;
	index->visited_tags = NULL;

	kvs_vector_id_slot_map_destroy(&index->record_id_to_slot);
	kvs_hnsw_context_pool_destroy(&index->search_context_pool);

	index->capacity = 0;
	index->size = 0;
	index->deleted_count = 0;
	index->entry_point = 0;
	index->has_entry = 0;
	index->max_level = 0;
	index->current_visit_tag = 1;
}

int kvs_hnsw_index_init(
	kvs_hnsw_index_t *index,
	size_t dim,
	int metric
) {
	if (!index || dim == 0) {
		return KVS_VECTOR_ERR_INVALID_ARG;
	}

	memset(index, 0, sizeof(*index));

	index->dim = dim;
	index->metric = metric;
	index->M = KVS_HNSW_DEFAULT_M;
	index->M0 = KVS_HNSW_DEFAULT_M0;
	index->ef_construction = KVS_HNSW_DEFAULT_EF_CONSTRUCTION;
	index->ef_search = KVS_HNSW_DEFAULT_EF_SEARCH;
	index->entry_point = 0;
	index->has_entry = 0;
	index->max_level = 0;
	index->current_visit_tag = 1;
	kvs_hnsw_random_init(
		&index->rng,
		0x9e3779b97f4a7c15ULL ^ ((uint64_t)dim << 32) ^ (uint64_t)(uint32_t)metric
	);
	index->ready = 1;
	index->needs_rebuild = 0;

	return KVS_VECTOR_OK;
}

int kvs_hnsw_index_reserve(
	kvs_hnsw_index_t *index,
	size_t capacity
) {
	float *vectors;
	uint8_t *levels;
	uint8_t *deleted_flags;
	uint64_t *slot_to_record_id;
	uint32_t *level0_neighbors;
	float *level0_neighbor_scores;
	uint16_t *level0_counts;
	uint16_t *level0_worst_pos;
	float *level0_worst_score;
	uint8_t *level0_worst_valid;
	size_t *upper_offsets;
	uint32_t *upper_neighbors;
	uint16_t *upper_counts;
	uint32_t *visited_tags;
	kvs_vector_id_slot_map_t new_map;
	kvs_hnsw_search_context_pool_t new_pool;
	size_t copy_size;
	size_t upper_slots;
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

	if (capacity > ((size_t)-1) / KVS_HNSW_MAX_LEVEL) {
		return KVS_VECTOR_ERR_INVALID_ARG;
	}
	upper_slots = capacity * KVS_HNSW_MAX_LEVEL;
	if (index->M > 0 &&
	    upper_slots > ((size_t)-1) / index->M) {
		return KVS_VECTOR_ERR_INVALID_ARG;
	}

	memset(&new_map, 0, sizeof(new_map));
	memset(&new_pool, 0, sizeof(new_pool));

	vectors = (float *)kvs_malloc(capacity * index->dim * sizeof(float));
	if (!vectors) {
		return KVS_VECTOR_ERR;
	}
	memset(vectors, 0, capacity * index->dim * sizeof(float));

	levels = (uint8_t *)kvs_malloc(capacity * sizeof(uint8_t));
	if (!levels) {
		kvs_free(vectors);
		return KVS_VECTOR_ERR;
	}
	memset(levels, 0, capacity * sizeof(uint8_t));

	deleted_flags = (uint8_t *)kvs_malloc(capacity * sizeof(uint8_t));
	if (!deleted_flags) {
		kvs_free(levels);
		kvs_free(vectors);
		return KVS_VECTOR_ERR;
	}
	memset(deleted_flags, 0, capacity * sizeof(uint8_t));

	slot_to_record_id = (uint64_t *)kvs_malloc(capacity * sizeof(uint64_t));
	if (!slot_to_record_id) {
		kvs_free(deleted_flags);
		kvs_free(levels);
		kvs_free(vectors);
		return KVS_VECTOR_ERR;
	}
	memset(slot_to_record_id, 0, capacity * sizeof(uint64_t));

	level0_neighbors = (uint32_t *)kvs_malloc(
		capacity * index->M0 * sizeof(uint32_t)
	);
	if (!level0_neighbors) {
		kvs_free(slot_to_record_id);
		kvs_free(deleted_flags);
		kvs_free(levels);
		kvs_free(vectors);
		return KVS_VECTOR_ERR;
	}
	memset(level0_neighbors, 0, capacity * index->M0 * sizeof(uint32_t));

	level0_neighbor_scores = (float *)kvs_malloc(
		capacity * index->M0 * sizeof(float)
	);
	if (!level0_neighbor_scores) {
		kvs_free(level0_neighbors);
		kvs_free(slot_to_record_id);
		kvs_free(deleted_flags);
		kvs_free(levels);
		kvs_free(vectors);
		return KVS_VECTOR_ERR;
	}
	memset(level0_neighbor_scores, 0, capacity * index->M0 * sizeof(float));

	level0_counts = (uint16_t *)kvs_malloc(capacity * sizeof(uint16_t));
	if (!level0_counts) {
		kvs_free(level0_neighbor_scores);
		kvs_free(level0_neighbors);
		kvs_free(slot_to_record_id);
		kvs_free(deleted_flags);
		kvs_free(levels);
		kvs_free(vectors);
		return KVS_VECTOR_ERR;
	}
	memset(level0_counts, 0, capacity * sizeof(uint16_t));

	level0_worst_pos = (uint16_t *)kvs_malloc(capacity * sizeof(uint16_t));
	if (!level0_worst_pos) {
		kvs_free(level0_counts);
		kvs_free(level0_neighbor_scores);
		kvs_free(level0_neighbors);
		kvs_free(slot_to_record_id);
		kvs_free(deleted_flags);
		kvs_free(levels);
		kvs_free(vectors);
		return KVS_VECTOR_ERR;
	}
	memset(level0_worst_pos, 0, capacity * sizeof(uint16_t));

	level0_worst_score = (float *)kvs_malloc(capacity * sizeof(float));
	if (!level0_worst_score) {
		kvs_free(level0_worst_pos);
		kvs_free(level0_counts);
		kvs_free(level0_neighbor_scores);
		kvs_free(level0_neighbors);
		kvs_free(slot_to_record_id);
		kvs_free(deleted_flags);
		kvs_free(levels);
		kvs_free(vectors);
		return KVS_VECTOR_ERR;
	}
	memset(level0_worst_score, 0, capacity * sizeof(float));

	level0_worst_valid = (uint8_t *)kvs_malloc(capacity * sizeof(uint8_t));
	if (!level0_worst_valid) {
		kvs_free(level0_worst_score);
		kvs_free(level0_worst_pos);
		kvs_free(level0_counts);
		kvs_free(level0_neighbor_scores);
		kvs_free(level0_neighbors);
		kvs_free(slot_to_record_id);
		kvs_free(deleted_flags);
		kvs_free(levels);
		kvs_free(vectors);
		return KVS_VECTOR_ERR;
	}
	memset(level0_worst_valid, 0, capacity * sizeof(uint8_t));

	upper_offsets = (size_t *)kvs_malloc(capacity * sizeof(size_t));
	if (!upper_offsets) {
		kvs_free(level0_worst_valid);
		kvs_free(level0_worst_score);
		kvs_free(level0_worst_pos);
		kvs_free(level0_counts);
		kvs_free(level0_neighbor_scores);
		kvs_free(level0_neighbors);
		kvs_free(slot_to_record_id);
		kvs_free(deleted_flags);
		kvs_free(levels);
		kvs_free(vectors);
		return KVS_VECTOR_ERR;
	}
	memset(upper_offsets, 0, capacity * sizeof(size_t));
	for (i = 0; i < capacity; i++) {
		upper_offsets[i] = i * KVS_HNSW_MAX_LEVEL * index->M;
	}

	upper_neighbors = (uint32_t *)kvs_malloc(
		upper_slots * index->M * sizeof(uint32_t)
	);
	if (!upper_neighbors) {
		kvs_free(upper_offsets);
		kvs_free(level0_worst_valid);
		kvs_free(level0_worst_score);
		kvs_free(level0_worst_pos);
		kvs_free(level0_counts);
		kvs_free(level0_neighbor_scores);
		kvs_free(level0_neighbors);
		kvs_free(slot_to_record_id);
		kvs_free(deleted_flags);
		kvs_free(levels);
		kvs_free(vectors);
		return KVS_VECTOR_ERR;
	}
	memset(upper_neighbors, 0, upper_slots * index->M * sizeof(uint32_t));

	upper_counts = (uint16_t *)kvs_malloc(upper_slots * sizeof(uint16_t));
	if (!upper_counts) {
		kvs_free(upper_neighbors);
		kvs_free(upper_offsets);
		kvs_free(level0_worst_valid);
		kvs_free(level0_worst_score);
		kvs_free(level0_worst_pos);
		kvs_free(level0_counts);
		kvs_free(level0_neighbor_scores);
		kvs_free(level0_neighbors);
		kvs_free(slot_to_record_id);
		kvs_free(deleted_flags);
		kvs_free(levels);
		kvs_free(vectors);
		return KVS_VECTOR_ERR;
	}
	memset(upper_counts, 0, upper_slots * sizeof(uint16_t));

	visited_tags = (uint32_t *)kvs_malloc(capacity * sizeof(uint32_t));
	if (!visited_tags) {
		kvs_free(upper_counts);
		kvs_free(upper_neighbors);
		kvs_free(upper_offsets);
		kvs_free(level0_worst_valid);
		kvs_free(level0_worst_score);
		kvs_free(level0_worst_pos);
		kvs_free(level0_counts);
		kvs_free(level0_neighbor_scores);
		kvs_free(level0_neighbors);
		kvs_free(slot_to_record_id);
		kvs_free(deleted_flags);
		kvs_free(levels);
		kvs_free(vectors);
		return KVS_VECTOR_ERR;
	}
	memset(visited_tags, 0, capacity * sizeof(uint32_t));

	ret = kvs_vector_id_slot_map_init(&new_map, capacity * 2);
	if (ret != KVS_VECTOR_OK) {
		kvs_free(visited_tags);
		kvs_free(upper_counts);
		kvs_free(upper_neighbors);
		kvs_free(upper_offsets);
		kvs_free(level0_worst_valid);
		kvs_free(level0_worst_score);
		kvs_free(level0_worst_pos);
		kvs_free(level0_counts);
		kvs_free(level0_neighbor_scores);
		kvs_free(level0_neighbors);
		kvs_free(slot_to_record_id);
		kvs_free(deleted_flags);
		kvs_free(levels);
		kvs_free(vectors);
		return ret;
	}

	ret = kvs_hnsw_context_pool_init(
		&new_pool,
		KVS_HNSW_DEFAULT_CONTEXT_COUNT,
		index->ef_search > index->ef_construction ?
			index->ef_search : index->ef_construction
	);
	if (ret != KVS_VECTOR_OK) {
		kvs_vector_id_slot_map_destroy(&new_map);
		kvs_free(visited_tags);
		kvs_free(upper_counts);
		kvs_free(upper_neighbors);
		kvs_free(upper_offsets);
		kvs_free(level0_worst_valid);
		kvs_free(level0_worst_score);
		kvs_free(level0_worst_pos);
		kvs_free(level0_counts);
		kvs_free(level0_neighbor_scores);
		kvs_free(level0_neighbors);
		kvs_free(slot_to_record_id);
		kvs_free(deleted_flags);
		kvs_free(levels);
		kvs_free(vectors);
		return ret;
	}

	copy_size = index->size;
	if (copy_size > capacity) {
		copy_size = capacity;
	}

	if (index->vectors && copy_size > 0) {
		memcpy(vectors, index->vectors, copy_size * index->dim * sizeof(float));
	}
	if (index->levels && copy_size > 0) {
		memcpy(levels, index->levels, copy_size * sizeof(uint8_t));
	}
	if (index->deleted_flags && copy_size > 0) {
		memcpy(deleted_flags, index->deleted_flags, copy_size * sizeof(uint8_t));
	}
	if (index->slot_to_record_id && copy_size > 0) {
		memcpy(slot_to_record_id, index->slot_to_record_id, copy_size * sizeof(uint64_t));
	}
	if (index->level0_neighbors && copy_size > 0) {
		memcpy(
			level0_neighbors,
			index->level0_neighbors,
			copy_size * index->M0 * sizeof(uint32_t)
		);
	}
	if (index->level0_neighbor_scores && copy_size > 0) {
		memcpy(
			level0_neighbor_scores,
			index->level0_neighbor_scores,
			copy_size * index->M0 * sizeof(float)
		);
	}
	if (index->level0_counts && copy_size > 0) {
		memcpy(level0_counts, index->level0_counts, copy_size * sizeof(uint16_t));
	}
	if (index->upper_offsets && copy_size > 0) {
		for (i = 0; i < copy_size; i++) {
			upper_offsets[i] = i * KVS_HNSW_MAX_LEVEL * index->M;
		}
	}
	if (index->upper_counts && copy_size > 0) {
		memcpy(
			upper_counts,
			index->upper_counts,
			copy_size * KVS_HNSW_MAX_LEVEL * sizeof(uint16_t)
		);
	}
	if (index->upper_neighbors && copy_size > 0) {
		memcpy(
			upper_neighbors,
			index->upper_neighbors,
			copy_size * KVS_HNSW_MAX_LEVEL * index->M * sizeof(uint32_t)
		);
	}
	if (index->visited_tags && copy_size > 0) {
		memcpy(visited_tags, index->visited_tags, copy_size * sizeof(uint32_t));
	}

	for (i = 0; i < copy_size; i++) {
		ret = kvs_vector_id_slot_map_put(
			&new_map,
			slot_to_record_id[i],
			(uint32_t)i
		);
		if (ret != KVS_VECTOR_OK) {
			kvs_hnsw_context_pool_destroy(&new_pool);
			kvs_vector_id_slot_map_destroy(&new_map);
			kvs_free(visited_tags);
			kvs_free(upper_counts);
			kvs_free(upper_neighbors);
			kvs_free(upper_offsets);
			kvs_free(level0_worst_valid);
			kvs_free(level0_worst_score);
			kvs_free(level0_worst_pos);
			kvs_free(level0_counts);
			kvs_free(level0_neighbor_scores);
			kvs_free(level0_neighbors);
			kvs_free(slot_to_record_id);
			kvs_free(deleted_flags);
			kvs_free(levels);
			kvs_free(vectors);
			return ret;
		}
	}

	kvs_free(index->vectors);
	kvs_free(index->levels);
	kvs_free(index->deleted_flags);
	kvs_free(index->slot_to_record_id);
	kvs_free(index->level0_neighbors);
	kvs_free(index->level0_neighbor_scores);
	kvs_free(index->level0_counts);
	kvs_free(index->level0_worst_pos);
	kvs_free(index->level0_worst_score);
	kvs_free(index->level0_worst_valid);
	kvs_free(index->upper_offsets);
	kvs_free(index->upper_neighbors);
	kvs_free(index->upper_counts);
	kvs_free(index->visited_tags);
	kvs_vector_id_slot_map_destroy(&index->record_id_to_slot);
	kvs_hnsw_context_pool_destroy(&index->search_context_pool);

	index->vectors = vectors;
	index->levels = levels;
	index->deleted_flags = deleted_flags;
	index->slot_to_record_id = slot_to_record_id;
	index->record_id_to_slot = new_map;
	index->level0_neighbors = level0_neighbors;
	index->level0_neighbor_scores = level0_neighbor_scores;
	index->level0_counts = level0_counts;
	index->level0_worst_pos = level0_worst_pos;
	index->level0_worst_score = level0_worst_score;
	index->level0_worst_valid = level0_worst_valid;
	index->upper_offsets = upper_offsets;
	index->upper_neighbors = upper_neighbors;
	index->upper_counts = upper_counts;
	index->visited_tags = visited_tags;
	index->search_context_pool = new_pool;
	index->capacity = capacity;
	index->ready = 1;
	index->needs_rebuild = 0;

	return KVS_VECTOR_OK;
}

int kvs_hnsw_index_reset(
	kvs_hnsw_index_t *index
) {
	if (!index) {
		return KVS_VECTOR_ERR_INVALID_ARG;
	}

	if (index->vectors && index->capacity > 0 && index->dim > 0) {
		memset(index->vectors, 0, index->capacity * index->dim * sizeof(float));
	}
	if (index->levels && index->capacity > 0) {
		memset(index->levels, 0, index->capacity * sizeof(uint8_t));
	}
	if (index->deleted_flags && index->capacity > 0) {
		memset(index->deleted_flags, 0, index->capacity * sizeof(uint8_t));
	}
	if (index->slot_to_record_id && index->capacity > 0) {
		memset(index->slot_to_record_id, 0, index->capacity * sizeof(uint64_t));
	}
	if (index->level0_neighbors && index->capacity > 0) {
		memset(index->level0_neighbors, 0, index->capacity * index->M0 * sizeof(uint32_t));
	}
	if (index->level0_neighbor_scores && index->capacity > 0) {
		memset(index->level0_neighbor_scores, 0, index->capacity * index->M0 * sizeof(float));
	}
	if (index->level0_counts && index->capacity > 0) {
		memset(index->level0_counts, 0, index->capacity * sizeof(uint16_t));
	}
	if (index->level0_worst_pos && index->capacity > 0) {
		memset(index->level0_worst_pos, 0, index->capacity * sizeof(uint16_t));
	}
	if (index->level0_worst_score && index->capacity > 0) {
		memset(index->level0_worst_score, 0, index->capacity * sizeof(float));
	}
	if (index->level0_worst_valid && index->capacity > 0) {
		memset(index->level0_worst_valid, 0, index->capacity * sizeof(uint8_t));
	}
	if (index->upper_offsets && index->capacity > 0) {
		size_t i;
		for (i = 0; i < index->capacity; i++) {
			index->upper_offsets[i] = i * KVS_HNSW_MAX_LEVEL * index->M;
		}
	}
	if (index->upper_neighbors && index->capacity > 0) {
		memset(
			index->upper_neighbors,
			0,
			index->capacity * KVS_HNSW_MAX_LEVEL * index->M * sizeof(uint32_t)
		);
	}
	if (index->upper_counts && index->capacity > 0) {
		memset(
			index->upper_counts,
			0,
			index->capacity * KVS_HNSW_MAX_LEVEL * sizeof(uint16_t)
		);
	}
	if (index->visited_tags && index->capacity > 0) {
		memset(index->visited_tags, 0, index->capacity * sizeof(uint32_t));
	}

	kvs_vector_id_slot_map_reset(&index->record_id_to_slot);

	index->size = 0;
	index->deleted_count = 0;
	index->entry_point = 0;
	index->has_entry = 0;
	index->max_level = 0;
	index->current_visit_tag = 1;
	index->needs_rebuild = 0;
	index->ready = 1;

	return KVS_VECTOR_OK;
}

void kvs_hnsw_index_destroy(
	kvs_hnsw_index_t *index
) {
	if (!index) {
		return;
	}

	kvs_hnsw_index_clear_storage(index);
	memset(index, 0, sizeof(*index));
}

uint32_t kvs_hnsw_next_visit_tag(
	kvs_hnsw_index_t *index
) {
	if (!index) {
		return 0;
	}

	index->current_visit_tag++;
	if (index->current_visit_tag == 0) {
		if (index->visited_tags && index->capacity > 0) {
			memset(index->visited_tags, 0, index->capacity * sizeof(uint32_t));
		}
		index->current_visit_tag = 1;
	}

	return index->current_visit_tag;
}

uint8_t kvs_hnsw_index_next_level(
	kvs_hnsw_index_t *index
) {
	if (!index) {
		return 0;
	}

	return kvs_hnsw_random_level(&index->rng, index->M);
}
