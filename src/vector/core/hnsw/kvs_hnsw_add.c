#include "vector/kvs_vector_hnsw.h"

#include "kvs_vector.h"
#include "kvs_vector_distance.h"

#include <stdlib.h>
#include <string.h>

static float kvs_hnsw_dot_score(
	const float *lhs,
	const float *rhs,
	size_t dim
) {
	return kvs_vector_distance_dot(lhs, rhs, dim);
}

static float kvs_hnsw_l2_score(
	const float *lhs,
	const float *rhs,
	size_t dim
) {
	return -kvs_vector_distance_squared_l2(lhs, rhs, dim);
}

static float kvs_hnsw_cosine_score(
	const float *lhs,
	const float *rhs,
	size_t dim
) {
	return kvs_vector_distance_cosine_score(lhs, rhs, dim);
}

static float kvs_hnsw_l2_score_avx2(
	const float *lhs,
	const float *rhs,
	size_t dim
) {
	return -kvs_vector_distance_squared_l2_avx2(lhs, rhs, dim);
}

static float kvs_hnsw_metric_score(
	const kvs_hnsw_index_t *index,
	const float *lhs,
	const float *rhs
) {
	if (index->distance) {
		return index->distance(lhs, rhs, index->dim);
	}

	switch (index->metric) {
	case KVS_VECTOR_METRIC_L2:
		return kvs_hnsw_l2_score(lhs, rhs, index->dim);
	case KVS_VECTOR_METRIC_COSINE:
		return kvs_hnsw_cosine_score(lhs, rhs, index->dim);
	case KVS_VECTOR_METRIC_IP:
	default:
		return kvs_hnsw_dot_score(lhs, rhs, index->dim);
	}
}

static kvs_hnsw_distance_fn kvs_hnsw_layer_search_score_fn(
	const kvs_hnsw_index_t *index
) {
	if (index->distance) {
		return index->distance;
	}

	if (kvs_vector_distance_backend_current() == KVS_VECTOR_DISTANCE_BACKEND_AVX2) {
		switch (index->metric) {
		case KVS_VECTOR_METRIC_L2:
			return kvs_hnsw_l2_score_avx2;
		case KVS_VECTOR_METRIC_COSINE:
			return kvs_vector_distance_cosine_avx2;
		case KVS_VECTOR_METRIC_IP:
		default:
			return kvs_vector_distance_dot_avx2;
		}
	}

	switch (index->metric) {
	case KVS_VECTOR_METRIC_L2:
		return kvs_hnsw_l2_score;
	case KVS_VECTOR_METRIC_COSINE:
		return kvs_hnsw_cosine_score;
	case KVS_VECTOR_METRIC_IP:
	default:
		return kvs_hnsw_dot_score;
	}
}

static const float *kvs_hnsw_slot_vector(
	const kvs_hnsw_index_t *index,
	uint32_t slot
) {
	return index->vectors + ((size_t)slot * index->dim);
}

static uint32_t *kvs_hnsw_layer_neighbors(
	kvs_hnsw_index_t *index,
	uint32_t slot,
	uint8_t level
) {
	if (level == 0) {
		return index->level0_neighbors + ((size_t)slot * index->M0);
	}

	return index->upper_neighbors +
		(((size_t)slot * KVS_HNSW_MAX_LEVEL + level) * index->M);
}

static float *kvs_hnsw_layer_level0_scores(
	kvs_hnsw_index_t *index,
	uint32_t slot
) {
	if (!index || !index->level0_neighbor_scores) {
		return NULL;
	}

	return index->level0_neighbor_scores + ((size_t)slot * index->M0);
}

static uint16_t *kvs_hnsw_layer_count_ptr(
	kvs_hnsw_index_t *index,
	uint32_t slot,
	uint8_t level
) {
	if (level == 0) {
		return &index->level0_counts[slot];
	}

	return &index->upper_counts[(size_t)slot * KVS_HNSW_MAX_LEVEL + level];
}

static size_t kvs_hnsw_layer_capacity(
	const kvs_hnsw_index_t *index,
	uint8_t level
) {
	return level == 0 ? index->M0 : index->M;
}

static int kvs_hnsw_forward_independent_link_enabled(void)
{
	static int cached = -1;

	if (cached < 0) {
		const char *value = getenv("KVS_HNSW_FORWARD_INDEPENDENT_LINK");

		cached = value && value[0] == '1' && value[1] == '\0';
	}

	return cached;
}

static void kvs_hnsw_profile_link_selected(
	kvs_hnsw_index_t *index,
	uint64_t count
) {
	if (!index) {
		return;
	}

	index->rebuild_profile.bidirectional_selected_neighbor_count += count;
}

static void kvs_hnsw_profile_link_forward_attempt(kvs_hnsw_index_t *index)
{
	if (!index) {
		return;
	}

	index->rebuild_profile.bidirectional_forward_edge_attempt_count++;
}

static void kvs_hnsw_profile_link_forward_added(kvs_hnsw_index_t *index)
{
	if (!index) {
		return;
	}

	index->rebuild_profile.bidirectional_forward_edge_added_count++;
}

static void kvs_hnsw_profile_link_reverse_result(
	kvs_hnsw_index_t *index,
	int accepted
) {
	if (!index) {
		return;
	}

	index->rebuild_profile.bidirectional_reverse_edge_attempt_count++;
	if (accepted) {
		index->rebuild_profile.bidirectional_reverse_edge_accepted_count++;
	} else {
		index->rebuild_profile.bidirectional_reverse_edge_rejected_count++;
	}
}

static void kvs_hnsw_profile_link_forward_skipped_reverse_reject(
	kvs_hnsw_index_t *index
) {
	if (!index) {
		return;
	}

	index->rebuild_profile.
		bidirectional_forward_edge_skipped_due_to_reverse_reject_count++;
}

static void kvs_hnsw_profile_neighbor_select_call(
	kvs_hnsw_index_t *index,
	uint8_t level,
	size_t candidate_count
) {
	kvs_hnsw_rebuild_profile_t *profile;

	if (!index) {
		return;
	}

	profile = &index->rebuild_profile;
	profile->neighbor_select_call_count++;
	if (level == 0) {
		profile->neighbor_select_level0_call_count++;
		profile->neighbor_select_level0_candidate_total += candidate_count;
	} else {
		profile->neighbor_select_upper_call_count++;
		profile->neighbor_select_upper_candidate_total += candidate_count;
	}
	profile->neighbor_select_candidate_total += candidate_count;
	if (candidate_count > profile->neighbor_select_candidate_max) {
		profile->neighbor_select_candidate_max = candidate_count;
	}
}

static void kvs_hnsw_profile_neighbor_select_selected(
	kvs_hnsw_index_t *index,
	uint8_t level,
	size_t selected_count
) {
	kvs_hnsw_rebuild_profile_t *profile;

	if (!index) {
		return;
	}

	profile = &index->rebuild_profile;
	profile->neighbor_select_selected_total += selected_count;
	if (level == 0) {
		profile->neighbor_select_level0_selected_total += selected_count;
	} else {
		profile->neighbor_select_upper_selected_total += selected_count;
	}
	if (selected_count > profile->neighbor_select_selected_max) {
		profile->neighbor_select_selected_max = selected_count;
	}
}

static void kvs_hnsw_profile_neighbor_select_reject(
	kvs_hnsw_index_t *index,
	uint8_t level
) {
	kvs_hnsw_rebuild_profile_t *profile;

	if (!index) {
		return;
	}

	profile = &index->rebuild_profile;
	profile->neighbor_select_reject_count++;
	profile->neighbor_select_heuristic_prune_count++;
	if (level == 0) {
		profile->neighbor_select_level0_reject_count++;
		profile->neighbor_select_level0_heuristic_prune_count++;
	} else {
		profile->neighbor_select_upper_reject_count++;
		profile->neighbor_select_upper_heuristic_prune_count++;
	}
}

static void kvs_hnsw_profile_neighbor_select_distance_recompute(
	kvs_hnsw_index_t *index,
	uint8_t level
) {
	kvs_hnsw_rebuild_profile_t *profile;

	if (!index) {
		return;
	}

	profile = &index->rebuild_profile;
	profile->neighbor_select_distance_recompute_count++;
	if (level == 0) {
		profile->neighbor_select_level0_distance_recompute_count++;
	} else {
		profile->neighbor_select_upper_distance_recompute_count++;
	}
}

static void kvs_hnsw_profile_neighbor_select_reused_score(
	kvs_hnsw_index_t *index,
	uint8_t level
) {
	kvs_hnsw_rebuild_profile_t *profile;

	if (!index) {
		return;
	}

	profile = &index->rebuild_profile;
	profile->neighbor_select_reused_candidate_score_count++;
	if (level == 0) {
		profile->neighbor_select_level0_reused_candidate_score_count++;
	} else {
		profile->neighbor_select_upper_reused_candidate_score_count++;
	}
}

static void kvs_hnsw_profile_neighbor_select_duplicate_scan(
	kvs_hnsw_index_t *index,
	uint8_t level
) {
	kvs_hnsw_rebuild_profile_t *profile;

	if (!index) {
		return;
	}

	profile = &index->rebuild_profile;
	profile->neighbor_select_duplicate_scan_count++;
	if (level == 0) {
		profile->neighbor_select_level0_duplicate_scan_count++;
	} else {
		profile->neighbor_select_upper_duplicate_scan_count++;
	}
}

static void kvs_hnsw_profile_neighbor_select_worst_scan(
	kvs_hnsw_index_t *index,
	uint8_t level
) {
	kvs_hnsw_rebuild_profile_t *profile;

	if (!index) {
		return;
	}

	profile = &index->rebuild_profile;
	profile->neighbor_select_worst_scan_count++;
	if (level == 0) {
		profile->neighbor_select_level0_worst_scan_count++;
	} else {
		profile->neighbor_select_upper_worst_scan_count++;
	}
}

static void kvs_hnsw_profile_neighbor_select_worst_cache_hit(
	kvs_hnsw_index_t *index,
	uint8_t level
) {
	kvs_hnsw_rebuild_profile_t *profile;

	if (!index) {
		return;
	}

	profile = &index->rebuild_profile;
	profile->neighbor_select_worst_cache_hit_count++;
	if (level == 0) {
		profile->neighbor_select_level0_worst_cache_hit_count++;
	} else {
		profile->neighbor_select_upper_worst_cache_hit_count++;
	}
}

static int kvs_hnsw_slot_active_at_level(
	const kvs_hnsw_index_t *index,
	uint32_t slot,
	uint8_t level
) {
	return (size_t)slot < index->size &&
		!index->deleted_flags[slot] &&
		index->levels[slot] >= level;
}

static void kvs_hnsw_layer_worst_cache_invalidate(
	kvs_hnsw_index_t *index,
	uint32_t slot,
	uint8_t level
) {
	if (!index || level != 0 || (size_t)slot >= index->capacity ||
	    !index->level0_worst_valid) {
		return;
	}

	index->level0_worst_valid[slot] = 0;
}

static float kvs_hnsw_layer_score_for_neighbor(
	kvs_hnsw_index_t *index,
	uint32_t slot,
	uint32_t neighbor,
	uint8_t level,
	float candidate_score,
	int has_candidate_score
) {
	if (level == 0 && has_candidate_score && !index->distance) {
		kvs_hnsw_profile_neighbor_select_reused_score(index, level);
		return candidate_score;
	}

	kvs_hnsw_profile_neighbor_select_distance_recompute(index, level);
	return kvs_hnsw_metric_score(
		index,
		kvs_hnsw_slot_vector(index, slot),
		kvs_hnsw_slot_vector(index, neighbor)
	);
}

static float kvs_hnsw_level0_score_for_cache(
	kvs_hnsw_index_t *index,
	uint32_t slot,
	uint32_t neighbor,
	float candidate_score
) {
	if (!index->distance) {
		return candidate_score;
	}

	return kvs_hnsw_metric_score(
		index,
		kvs_hnsw_slot_vector(index, slot),
		kvs_hnsw_slot_vector(index, neighbor)
	);
}

static void kvs_hnsw_refresh_level0_neighbor_scores(
	kvs_hnsw_index_t *index
) {
	size_t slot;

	if (!index || !index->level0_neighbor_scores ||
	    !index->level0_neighbors || !index->level0_counts) {
		return;
	}

	for (slot = 0; slot < index->size; slot++) {
		uint32_t *neighbors = kvs_hnsw_layer_neighbors(index, (uint32_t)slot, 0);
		float *scores = kvs_hnsw_layer_level0_scores(index, (uint32_t)slot);
		size_t count = index->level0_counts[slot];
		size_t i;

		if (!scores || index->deleted_flags[slot]) {
			continue;
		}

		for (i = 0; i < count; i++) {
			uint32_t neighbor = neighbors[i];

			if ((size_t)neighbor >= index->size || index->deleted_flags[neighbor]) {
				scores[i] = 0.0f;
				continue;
			}
			scores[i] = kvs_hnsw_metric_score(
				index,
				kvs_hnsw_slot_vector(index, (uint32_t)slot),
				kvs_hnsw_slot_vector(index, neighbor)
			);
		}
	}
}

static int kvs_hnsw_layer_has_neighbor(
	kvs_hnsw_index_t *index,
	uint32_t slot,
	uint32_t neighbor,
	uint8_t level
) {
	uint32_t *neighbors;
	size_t count;
	size_t i;

	if (!index || (size_t)slot >= index->size) {
		return 0;
	}

	neighbors = kvs_hnsw_layer_neighbors(index, slot, level);
	count = *kvs_hnsw_layer_count_ptr(index, slot, level);

	for (i = 0; i < count; i++) {
		if (neighbors[i] == neighbor) {
			return 1;
		}
	}

	return 0;
}

static void kvs_hnsw_layer_remove_neighbor(
	kvs_hnsw_index_t *index,
	uint32_t slot,
	uint32_t neighbor,
	uint8_t level
) {
	uint32_t *neighbors;
	uint16_t *count_ptr;
	size_t count;
	size_t i;

	if (!index || (size_t)slot >= index->size) {
		return;
	}

	neighbors = kvs_hnsw_layer_neighbors(index, slot, level);
	count_ptr = kvs_hnsw_layer_count_ptr(index, slot, level);
	count = *count_ptr;

	for (i = 0; i < count; i++) {
		if (neighbors[i] != neighbor) {
			continue;
		}

		if (i + 1 < count) {
			float *scores = level == 0 ?
				kvs_hnsw_layer_level0_scores(index, slot) : NULL;

			memmove(
				&neighbors[i],
				&neighbors[i + 1],
				(count - i - 1) * sizeof(uint32_t)
			);
			if (scores) {
				memmove(
					&scores[i],
					&scores[i + 1],
					(count - i - 1) * sizeof(float)
				);
			}
		}
		neighbors[count - 1] = 0;
		if (level == 0) {
			float *scores = kvs_hnsw_layer_level0_scores(index, slot);

			if (scores) {
				scores[count - 1] = 0.0f;
			}
		}
		*count_ptr = (uint16_t)(count - 1);
		kvs_hnsw_layer_worst_cache_invalidate(index, slot, level);
		return;
	}
}

static void kvs_hnsw_layer_find_worst_neighbor(
	kvs_hnsw_index_t *index,
	uint32_t slot,
	uint8_t level,
	uint32_t *neighbors,
	size_t count,
	size_t *out_worst_pos,
	float *out_worst_score
) {
	size_t worst_pos = 0;
	float worst_score = 0.0f;
	const float *slot_vec;
	float *level0_scores;
	size_t i;

	if (!index || !neighbors || count == 0 ||
	    !out_worst_pos || !out_worst_score) {
		return;
	}

	if (level == 0 &&
	    index->deleted_count == 0 &&
	    index->level0_worst_valid &&
	    index->level0_worst_pos &&
	    index->level0_worst_score &&
	    (size_t)slot < index->capacity &&
	    index->level0_worst_valid[slot] &&
	    index->level0_worst_pos[slot] < count) {
		kvs_hnsw_profile_neighbor_select_worst_cache_hit(index, level);
		*out_worst_pos = index->level0_worst_pos[slot];
		*out_worst_score = index->level0_worst_score[slot];
		return;
	}

	slot_vec = kvs_hnsw_slot_vector(index, slot);
	level0_scores = level == 0 ?
		kvs_hnsw_layer_level0_scores(index, slot) : NULL;
	for (i = 0; i < count; i++) {
		float score;

		if (level0_scores && index->deleted_count == 0) {
			score = level0_scores[i];
			kvs_hnsw_profile_neighbor_select_worst_scan(index, level);
		} else {
			const float *current_vec = kvs_hnsw_slot_vector(index, neighbors[i]);

			score = kvs_hnsw_metric_score(index, slot_vec, current_vec);
			kvs_hnsw_profile_neighbor_select_worst_scan(index, level);
			kvs_hnsw_profile_neighbor_select_distance_recompute(index, level);
		}
		if (i == 0 || score < worst_score) {
			worst_score = score;
			worst_pos = i;
		}
	}

	if (level == 0 &&
	    index->deleted_count == 0 &&
	    index->level0_worst_valid &&
	    index->level0_worst_pos &&
	    index->level0_worst_score &&
	    (size_t)slot < index->capacity &&
	    worst_pos <= UINT16_MAX) {
		index->level0_worst_pos[slot] = (uint16_t)worst_pos;
		index->level0_worst_score[slot] = worst_score;
		index->level0_worst_valid[slot] = 1;
	}

	*out_worst_pos = worst_pos;
	*out_worst_score = worst_score;
}

static int kvs_hnsw_layer_plan_append_or_replace(
	kvs_hnsw_index_t *index,
	uint32_t slot,
	uint32_t neighbor,
	uint8_t level,
	float candidate_score,
	int has_candidate_score,
	int *out_accepted,
	uint32_t *out_removed,
	size_t *out_replace_pos,
	float *out_neighbor_score
) {
	uint32_t *neighbors;
	size_t count;
	size_t capacity;
	size_t worst_pos = 0;
	float worst_score = 0.0f;
	float neighbor_score;
	size_t i;

	if (out_accepted) {
		*out_accepted = 0;
	}
	if (out_removed) {
		*out_removed = UINT32_MAX;
	}
	if (out_replace_pos) {
		*out_replace_pos = (size_t)-1;
	}
	if (out_neighbor_score) {
		*out_neighbor_score = 0.0f;
	}

	if (!index || slot == neighbor ||
	    !kvs_hnsw_slot_active_at_level(index, slot, level) ||
	    !kvs_hnsw_slot_active_at_level(index, neighbor, level)) {
		return KVS_VECTOR_ERR_INVALID_ARG;
	}

	neighbors = kvs_hnsw_layer_neighbors(index, slot, level);
	count = *kvs_hnsw_layer_count_ptr(index, slot, level);
	capacity = kvs_hnsw_layer_capacity(index, level);

	for (i = 0; i < count; i++) {
		kvs_hnsw_profile_neighbor_select_duplicate_scan(index, level);
		if (neighbors[i] == neighbor) {
			if (out_accepted) {
				*out_accepted = 1;
			}
			return KVS_VECTOR_OK;
		}
	}

	if (count < capacity) {
		if (level == 0 && out_neighbor_score) {
			*out_neighbor_score = kvs_hnsw_layer_score_for_neighbor(
				index,
				slot,
				neighbor,
				level,
				candidate_score,
				has_candidate_score
			);
		}
		if (out_accepted) {
			*out_accepted = 1;
		}
		if (out_replace_pos) {
			*out_replace_pos = count;
		}
		return KVS_VECTOR_OK;
	}

	kvs_hnsw_layer_find_worst_neighbor(
		index,
		slot,
		level,
		neighbors,
		count,
		&worst_pos,
		&worst_score
	);

	neighbor_score = kvs_hnsw_layer_score_for_neighbor(
		index,
		slot,
		neighbor,
		level,
		candidate_score,
		has_candidate_score
	);
	if (out_neighbor_score) {
		*out_neighbor_score = neighbor_score;
	}

	if (neighbor_score > worst_score) {
		if (out_accepted) {
			*out_accepted = 1;
		}
		if (out_removed) {
			*out_removed = neighbors[worst_pos];
		}
		if (out_replace_pos) {
			*out_replace_pos = worst_pos;
		}
	} else {
		kvs_hnsw_profile_neighbor_select_reject(index, level);
	}

	return KVS_VECTOR_OK;
}

static int kvs_hnsw_layer_apply_append_or_replace(
	kvs_hnsw_index_t *index,
	uint32_t slot,
	uint32_t neighbor,
	uint8_t level,
	int accepted,
	uint32_t removed,
	size_t replace_pos,
	float neighbor_score
) {
	uint32_t *neighbors;
	uint16_t *count_ptr;
	size_t count;
	size_t capacity;

	if (!accepted) {
		return KVS_VECTOR_OK;
	}

	if (!index || slot == neighbor ||
	    !kvs_hnsw_slot_active_at_level(index, slot, level) ||
	    !kvs_hnsw_slot_active_at_level(index, neighbor, level)) {
		return KVS_VECTOR_ERR_INVALID_ARG;
	}

	neighbors = kvs_hnsw_layer_neighbors(index, slot, level);
	count_ptr = kvs_hnsw_layer_count_ptr(index, slot, level);
	count = *count_ptr;
	capacity = kvs_hnsw_layer_capacity(index, level);

	if (replace_pos == (size_t)-1) {
		return KVS_VECTOR_OK;
	}

		if (count < capacity && replace_pos == count) {
			neighbors[count] = neighbor;
			if (level == 0) {
				float *scores = kvs_hnsw_layer_level0_scores(index, slot);

				if (scores) {
					scores[count] = neighbor_score;
				}
			}
			*count_ptr = (uint16_t)(count + 1);
			kvs_hnsw_layer_worst_cache_invalidate(index, slot, level);
			return KVS_VECTOR_OK;
		}

	if (replace_pos < count && removed != UINT32_MAX) {
		neighbors[replace_pos] = neighbor;
			if (level == 0) {
				float *scores = kvs_hnsw_layer_level0_scores(index, slot);

				if (scores) {
					scores[replace_pos] = neighbor_score;
				}
			}
			kvs_hnsw_layer_worst_cache_invalidate(index, slot, level);
			return KVS_VECTOR_OK;
		}

	return KVS_VECTOR_ERR;
}

static int kvs_hnsw_layer_plan_new_slot_append_or_replace(
	kvs_hnsw_index_t *index,
	uint32_t slot,
	uint32_t new_slot,
	uint8_t level,
	float candidate_score,
	int *out_accepted,
	uint32_t *out_removed,
	size_t *out_replace_pos,
	float *out_neighbor_score
) {
	uint32_t *neighbors;
	size_t count;
	size_t capacity;
	size_t worst_pos = 0;
	float worst_score = 0.0f;
	float neighbor_score;

	if (out_accepted) {
		*out_accepted = 0;
	}
	if (out_removed) {
		*out_removed = UINT32_MAX;
	}
	if (out_replace_pos) {
		*out_replace_pos = (size_t)-1;
	}
	if (out_neighbor_score) {
		*out_neighbor_score = 0.0f;
	}

	if (!index || slot == new_slot ||
	    !kvs_hnsw_slot_active_at_level(index, slot, level) ||
	    !kvs_hnsw_slot_active_at_level(index, new_slot, level)) {
		return KVS_VECTOR_ERR_INVALID_ARG;
	}

	neighbors = kvs_hnsw_layer_neighbors(index, slot, level);
	count = *kvs_hnsw_layer_count_ptr(index, slot, level);
	capacity = kvs_hnsw_layer_capacity(index, level);

	if (count < capacity) {
		if (level == 0 && out_neighbor_score) {
			*out_neighbor_score = kvs_hnsw_layer_score_for_neighbor(
				index,
				slot,
				new_slot,
				level,
				candidate_score,
				1
			);
		}
		if (out_accepted) {
			*out_accepted = 1;
		}
		if (out_replace_pos) {
			*out_replace_pos = count;
		}
		return KVS_VECTOR_OK;
	}

	kvs_hnsw_layer_find_worst_neighbor(
		index,
		slot,
		level,
		neighbors,
		count,
		&worst_pos,
		&worst_score
	);
	neighbor_score = kvs_hnsw_layer_score_for_neighbor(
		index,
		slot,
		new_slot,
		level,
		candidate_score,
		1
	);
	if (out_neighbor_score) {
		*out_neighbor_score = neighbor_score;
	}

	if (neighbor_score > worst_score) {
		if (out_accepted) {
			*out_accepted = 1;
		}
		if (out_removed) {
			*out_removed = neighbors[worst_pos];
		}
		if (out_replace_pos) {
			*out_replace_pos = worst_pos;
		}
	} else {
		kvs_hnsw_profile_neighbor_select_reject(index, level);
	}

	return KVS_VECTOR_OK;
}

static int kvs_hnsw_add_bidirectional_edge(
	kvs_hnsw_index_t *index,
	uint32_t a,
	uint32_t b,
	uint8_t level
) {
	int accepted_ab = 0;
	int accepted_ba = 0;
	uint32_t removed_ab = UINT32_MAX;
	uint32_t removed_ba = UINT32_MAX;
	size_t replace_pos_ab = (size_t)-1;
	size_t replace_pos_ba = (size_t)-1;
	float score_ab = 0.0f;
	float score_ba = 0.0f;
	uint64_t stage_start_us;
	int ret;

	if (a == b) {
		return KVS_VECTOR_OK;
	}

	stage_start_us = kvs_hnsw_profile_now_us();
	ret = kvs_hnsw_layer_plan_append_or_replace(
		index,
		a,
		b,
		level,
		0.0f,
		0,
		&accepted_ab,
		&removed_ab,
		&replace_pos_ab,
		&score_ab
	);
	if (ret != KVS_VECTOR_OK) {
		return ret;
	}
	kvs_hnsw_profile_link_forward_attempt(index);

	ret = kvs_hnsw_layer_plan_append_or_replace(
		index,
		b,
		a,
		level,
		0.0f,
		0,
		&accepted_ba,
		&removed_ba,
		&replace_pos_ba,
		&score_ba
	);
	if (ret != KVS_VECTOR_OK) {
		return ret;
	}
	kvs_hnsw_profile_link_reverse_result(index, accepted_ba);
	index->rebuild_profile.hnsw_add_neighbor_select_us +=
		kvs_hnsw_profile_now_us() - stage_start_us;

	if (!accepted_ab) {
		return KVS_VECTOR_OK;
	}

	if (!accepted_ba && !kvs_hnsw_forward_independent_link_enabled()) {
		kvs_hnsw_profile_link_forward_skipped_reverse_reject(index);
		return KVS_VECTOR_OK;
	}

	stage_start_us = kvs_hnsw_profile_now_us();
	ret = kvs_hnsw_layer_apply_append_or_replace(
		index,
		a,
		b,
		level,
		accepted_ab,
		removed_ab,
		replace_pos_ab,
		score_ab
	);
	if (ret != KVS_VECTOR_OK) {
		return ret;
	}

	ret = kvs_hnsw_layer_apply_append_or_replace(
		index,
		b,
		a,
		level,
		accepted_ba,
		removed_ba,
		replace_pos_ba,
		score_ba
	);
	if (ret != KVS_VECTOR_OK) {
		return ret;
	}
	if (replace_pos_ab != (size_t)-1) {
		kvs_hnsw_profile_link_forward_added(index);
	}

	if (removed_ab != UINT32_MAX) {
		kvs_hnsw_layer_remove_neighbor(index, removed_ab, a, level);
	}
	if (removed_ba != UINT32_MAX) {
		kvs_hnsw_layer_remove_neighbor(index, removed_ba, b, level);
	}
	index->rebuild_profile.hnsw_add_link_us +=
		kvs_hnsw_profile_now_us() - stage_start_us;

	return KVS_VECTOR_OK;
}

static int kvs_hnsw_add_new_slot_bidirectional_edge(
	kvs_hnsw_index_t *index,
	uint32_t slot,
	uint32_t neighbor,
	uint8_t level,
	float candidate_score
) {
	uint32_t removed_ba = UINT32_MAX;
	size_t replace_pos_ba = (size_t)-1;
	int accepted_ba = 0;
	uint32_t *slot_neighbors;
	uint16_t *slot_count_ptr;
	size_t slot_count;
	size_t slot_capacity;
	float score_ba = 0.0f;
	uint64_t stage_start_us;
	int ret;

	if (slot == neighbor) {
		return KVS_VECTOR_OK;
	}

	if (!index ||
	    !kvs_hnsw_slot_active_at_level(index, slot, level) ||
	    !kvs_hnsw_slot_active_at_level(index, neighbor, level)) {
		return KVS_VECTOR_ERR_INVALID_ARG;
	}

	slot_neighbors = kvs_hnsw_layer_neighbors(index, slot, level);
	slot_count_ptr = kvs_hnsw_layer_count_ptr(index, slot, level);
	slot_count = *slot_count_ptr;
	slot_capacity = kvs_hnsw_layer_capacity(index, level);

	if (slot_count >= slot_capacity) {
		return kvs_hnsw_add_bidirectional_edge(index, slot, neighbor, level);
	}

	stage_start_us = kvs_hnsw_profile_now_us();
	ret = kvs_hnsw_layer_plan_new_slot_append_or_replace(
		index,
		neighbor,
		slot,
		level,
		candidate_score,
		&accepted_ba,
		&removed_ba,
		&replace_pos_ba,
		&score_ba
	);
	if (ret != KVS_VECTOR_OK) {
		return ret;
	}
	kvs_hnsw_profile_link_forward_attempt(index);
	kvs_hnsw_profile_link_reverse_result(index, accepted_ba);
	index->rebuild_profile.hnsw_add_neighbor_select_us +=
		kvs_hnsw_profile_now_us() - stage_start_us;

	if (!accepted_ba && !kvs_hnsw_forward_independent_link_enabled()) {
		kvs_hnsw_profile_link_forward_skipped_reverse_reject(index);
		return KVS_VECTOR_OK;
	}

	stage_start_us = kvs_hnsw_profile_now_us();
	slot_neighbors[slot_count] = neighbor;
	if (level == 0) {
		float *slot_scores = kvs_hnsw_layer_level0_scores(index, slot);

		if (slot_scores) {
			slot_scores[slot_count] = kvs_hnsw_level0_score_for_cache(
				index,
				slot,
				neighbor,
				candidate_score
			);
		}
	}
	*slot_count_ptr = (uint16_t)(slot_count + 1);
	kvs_hnsw_layer_worst_cache_invalidate(index, slot, level);

	ret = kvs_hnsw_layer_apply_append_or_replace(
		index,
		neighbor,
		slot,
		level,
		accepted_ba,
		removed_ba,
		replace_pos_ba,
		score_ba
	);
	if (ret != KVS_VECTOR_OK) {
		*slot_count_ptr = (uint16_t)slot_count;
		slot_neighbors[slot_count] = 0;
		if (level == 0) {
			float *slot_scores = kvs_hnsw_layer_level0_scores(index, slot);

			if (slot_scores) {
				slot_scores[slot_count] = 0.0f;
			}
		}
		kvs_hnsw_layer_worst_cache_invalidate(index, slot, level);
		return ret;
	}
	kvs_hnsw_profile_link_forward_added(index);

	if (removed_ba != UINT32_MAX) {
		kvs_hnsw_layer_remove_neighbor(index, removed_ba, neighbor, level);
	}
	index->rebuild_profile.hnsw_add_link_us +=
		kvs_hnsw_profile_now_us() - stage_start_us;

	return KVS_VECTOR_OK;
}

static uint32_t kvs_hnsw_greedy_search_layer(
	kvs_hnsw_index_t *index,
	uint32_t entry,
	const float *query,
	uint8_t level
) {
	uint32_t current = entry;
	float current_score;
	int improved = 1;
	const float *vectors;
	size_t dim;

	if (!kvs_hnsw_slot_active_at_level(index, entry, level)) {
		return entry;
	}

	vectors = index->vectors;
	dim = index->dim;
	current_score = kvs_hnsw_metric_score(
		index,
		query,
		vectors + ((size_t)current * dim)
	);

	while (improved) {
		uint32_t *neighbors = kvs_hnsw_layer_neighbors(index, current, level);
		size_t count = *kvs_hnsw_layer_count_ptr(index, current, level);
		size_t i;

		improved = 0;
		for (i = 0; i < count; i++) {
			uint32_t neighbor = neighbors[i];
			float score;

			if (!kvs_hnsw_slot_active_at_level(index, neighbor, level)) {
				continue;
			}

			score = kvs_hnsw_metric_score(
				index,
				query,
				vectors + ((size_t)neighbor * dim)
			);
			if (score > current_score) {
				current = neighbor;
				current_score = score;
				improved = 1;
			}
		}
	}

	return current;
}

static void kvs_hnsw_top_heap_offer(
	kvs_vector_heap_t *heap,
	kvs_vector_heap_item_t item
) {
	const kvs_vector_heap_item_t *top;

	if (heap->size < heap->capacity) {
		(void)kvs_vector_heap_push(heap, item);
		return;
	}

	top = kvs_vector_heap_top(heap);
	if (top && item.score > top->score) {
		(void)kvs_vector_heap_replace_top(heap, item);
	}
}

static void kvs_hnsw_add_search_context_reset(
	kvs_hnsw_search_context_t *ctx
) {
	if (!ctx) {
		return;
	}

	kvs_vector_heap_reset(&ctx->candidate_heap);
	kvs_vector_heap_reset(&ctx->top_heap);
}

static void kvs_hnsw_add_export_best_candidates(
	kvs_hnsw_search_context_t *ctx,
	size_t limit,
	size_t *out_count
) {
	size_t count = 0;
	size_t i;

	if (!ctx || !out_count || limit == 0) {
		if (out_count) {
			*out_count = 0;
		}
		return;
	}

	if (limit > ctx->capacity) {
		limit = ctx->capacity;
	}

	for (i = 0; i < ctx->top_heap.size; i++) {
		const kvs_vector_heap_item_t *item = &ctx->top_heap.items[i];
		size_t pos;

		if (count >= limit && item->score <= ctx->distance_buffer[count - 1]) {
			continue;
		}

		pos = count;
		if (pos >= limit) {
			pos = limit - 1;
		} else {
			count++;
		}

		while (pos > 0 && ctx->distance_buffer[pos - 1] < item->score) {
			ctx->neighbor_buffer[pos] = ctx->neighbor_buffer[pos - 1];
			ctx->distance_buffer[pos] = ctx->distance_buffer[pos - 1];
			pos--;
		}

		ctx->neighbor_buffer[pos] = item->slot;
		ctx->distance_buffer[pos] = item->score;
	}

	*out_count = count;
}

static int kvs_hnsw_search_layer_for_add(
	kvs_hnsw_index_t *index,
	kvs_hnsw_search_context_t *ctx,
	uint32_t entry,
	const float *query,
	uint8_t level,
	size_t *out_count
) {
	kvs_vector_heap_item_t item;
	uint32_t visit_tag;
	size_t export_limit;
	size_t ef;
	float lower_bound_score;
	int no_deleted;
	/* hoisted locals for hot loop */
		const float *vectors;
		size_t dim;
		uint32_t *visited_tags;
		size_t candidate_capacity;
		uint64_t search_start_us;
		kvs_hnsw_distance_fn score_fn;
	uint32_t prev_level0_row_slot = 0;
	int have_prev_level0_row = 0;

	if (!index || !ctx || !out_count ||
	    !kvs_hnsw_slot_active_at_level(index, entry, level)) {
		return KVS_VECTOR_ERR_INVALID_ARG;
	}

	ef = index->ef_construction;
	if (ef > ctx->top_heap.capacity) {
		ef = ctx->top_heap.capacity;
	}
	if (ef == 0) {
		return KVS_VECTOR_ERR_INVALID_ARG;
	}

	kvs_hnsw_add_search_context_reset(ctx);
	visit_tag = kvs_hnsw_next_visit_tag(index);
	no_deleted = index->deleted_count == 0;
		vectors = index->vectors;
		dim = index->dim;
		visited_tags = index->visited_tags;
		candidate_capacity = ctx->candidate_heap.capacity;
		score_fn = kvs_hnsw_layer_search_score_fn(index);
		visited_tags[entry] = visit_tag;
		search_start_us = kvs_hnsw_profile_now_us();

		index->rebuild_profile.add_distance_compute_count++;
		index->rebuild_profile.layer_search_call_count++;
		if (level == 0) {
			index->rebuild_profile.layer_search_level0_call_count++;
		} else {
		index->rebuild_profile.layer_search_upper_call_count++;
	}
	index->rebuild_profile.layer_search_unique_visit_count++;
	index->rebuild_profile.layer_search_visited_tag_access_count++;
	index->rebuild_profile.layer_search_slot_state_touch_count++;
	if (level == 0) {
		index->rebuild_profile.layer_search_level0_unique_visit_count++;
		index->rebuild_profile.layer_search_level0_visited_tag_access_count++;
		index->rebuild_profile.layer_search_level0_slot_state_touch_count++;
	}
	index->rebuild_profile.layer_search_distance_compute_count++;
	if (level == 0) {
		index->rebuild_profile.layer_search_level0_distance_compute_count++;
	} else {
		index->rebuild_profile.layer_search_upper_distance_compute_count++;
	}
	item.record_id = 0;
	item.slot = entry;
	item.score = score_fn(
		query,
		vectors + ((size_t)entry * dim),
		dim
	);
	if (kvs_vector_heap_push(&ctx->candidate_heap, item) != KVS_VECTOR_OK) {
		return KVS_VECTOR_ERR;
	}
	index->rebuild_profile.layer_search_candidate_heap_push_count++;
	if (level == 0) {
		index->rebuild_profile.layer_search_level0_candidate_heap_push_count++;
	}
	if (kvs_vector_heap_push(&ctx->top_heap, item) != KVS_VECTOR_OK) {
		return KVS_VECTOR_ERR;
	}
	index->rebuild_profile.add_top_heap_push_count++;
	index->rebuild_profile.layer_search_top_heap_push_count++;
	if (level == 0) {
		index->rebuild_profile.layer_search_level0_top_heap_push_count++;
	}
	lower_bound_score = item.score;
	index->rebuild_profile.layer_search_lower_bound_update_count++;
	if (level == 0) {
		index->rebuild_profile.layer_search_level0_lower_bound_update_count++;
	}

		while (!kvs_vector_heap_empty(&ctx->candidate_heap)) {
			kvs_vector_heap_item_t current;
			uint32_t *neighbors;
		size_t neighbor_count;
		size_t i;

		if (kvs_vector_heap_pop(&ctx->candidate_heap, &current) != KVS_VECTOR_OK) {
			break;
		}
		index->rebuild_profile.layer_search_candidate_heap_pop_count++;
		if (level == 0) {
			index->rebuild_profile.layer_search_level0_candidate_heap_pop_count++;
		}

		if (ctx->top_heap.size >= ef && current.score < lower_bound_score) {
			break;
			}

			if (level == 0) {
				neighbors = kvs_hnsw_layer_neighbors(index, current.slot, level);
				neighbor_count =
					*kvs_hnsw_layer_count_ptr(index, current.slot, level);
				index->rebuild_profile.layer_search_slot_state_touch_count++;
				index->rebuild_profile.layer_search_level0_slot_state_touch_count++;
				index->rebuild_profile.layer_search_level0_neighbor_row_access_count++;
				index->rebuild_profile.layer_search_level0_candidate_row_expansion_count++;
				if (have_prev_level0_row &&
				    current.slot == prev_level0_row_slot + 1) {
				index->rebuild_profile.layer_search_level0_neighbor_row_sequential_count++;
			} else {
				index->rebuild_profile.layer_search_level0_neighbor_row_random_count++;
			}
			prev_level0_row_slot = current.slot;
			have_prev_level0_row = 1;
		} else {
			neighbors = kvs_hnsw_layer_neighbors(index, current.slot, level);
			neighbor_count = *kvs_hnsw_layer_count_ptr(index, current.slot, level);
			index->rebuild_profile.layer_search_slot_state_touch_count++;
		}

		for (i = 0; i < neighbor_count; i++) {
			uint32_t neighbor = neighbors[i];
			kvs_vector_heap_item_t next;
			const kvs_vector_heap_item_t *top;

			if (level == 0) {
				index->rebuild_profile.add_level0_neighbor_scan_count++;
				index->rebuild_profile.layer_search_level0_neighbor_scan_count++;
				if (i == 0) {
					index->rebuild_profile.layer_search_level0_neighbor_slot_random_count++;
				} else {
					index->rebuild_profile.layer_search_level0_neighbor_slot_sequential_count++;
				}
			} else {
				index->rebuild_profile.add_upper_neighbor_scan_count++;
				index->rebuild_profile.layer_search_upper_neighbor_scan_count++;
			}
			index->rebuild_profile.add_neighbor_visit_count++;
			index->rebuild_profile.layer_search_neighbor_visit_count++;

			index->rebuild_profile.layer_search_visited_tag_access_count++;
			index->rebuild_profile.layer_search_slot_state_touch_count++;
			if (level == 0) {
					index->rebuild_profile.layer_search_level0_visited_tag_access_count++;
					index->rebuild_profile.layer_search_level0_slot_state_touch_count++;
				}
				if (visited_tags[neighbor] == visit_tag) {
					index->rebuild_profile.add_visited_skip_count++;
					index->rebuild_profile.layer_search_visited_skip_count++;
					index->rebuild_profile.layer_search_repeated_visit_count++;
					if (level == 0) {
						index->rebuild_profile.layer_search_level0_repeated_visit_count++;
					}
					continue;
				}
				visited_tags[neighbor] = visit_tag;
				index->rebuild_profile.layer_search_unique_visit_count++;
				index->rebuild_profile.layer_search_visited_tag_access_count++;
				index->rebuild_profile.layer_search_slot_state_touch_count++;
			if (level == 0) {
				index->rebuild_profile.layer_search_level0_unique_visit_count++;
				index->rebuild_profile.layer_search_level0_visited_tag_access_count++;
					index->rebuild_profile.layer_search_level0_slot_state_touch_count++;
				}
				if (!no_deleted) {
					if (!kvs_hnsw_slot_active_at_level(index, neighbor, level)) {
						index->rebuild_profile.add_active_check_count++;
						index->rebuild_profile.layer_search_deleted_active_check_count++;
						if (level == 0) {
							index->rebuild_profile.layer_search_level0_deleted_active_check_count++;
						} else {
							index->rebuild_profile.layer_search_upper_deleted_active_check_count++;
						}
					index->rebuild_profile.layer_search_slot_state_touch_count++;
					if (level == 0) {
						index->rebuild_profile.layer_search_level0_slot_state_touch_count++;
					}
					continue;
				}
				index->rebuild_profile.add_active_check_count++;
				index->rebuild_profile.layer_search_deleted_active_check_count++;
				if (level == 0) {
					index->rebuild_profile.layer_search_level0_deleted_active_check_count++;
				} else {
					index->rebuild_profile.layer_search_upper_deleted_active_check_count++;
				}
				index->rebuild_profile.layer_search_slot_state_touch_count++;
				if (level == 0) {
					index->rebuild_profile.layer_search_level0_slot_state_touch_count++;
				}
			}

			index->rebuild_profile.add_distance_compute_count++;
			index->rebuild_profile.layer_search_distance_compute_count++;
			if (level == 0) {
				index->rebuild_profile.layer_search_level0_distance_compute_count++;
			} else {
				index->rebuild_profile.layer_search_upper_distance_compute_count++;
			}
			next.record_id = 0;
			next.slot = neighbor;
			next.score = score_fn(
				query,
				vectors + ((size_t)neighbor * dim),
				dim
			);

			if (ctx->top_heap.size < ef || next.score > lower_bound_score) {
				if (ctx->top_heap.size < ef) {
					if (kvs_vector_heap_push(&ctx->top_heap, next) !=
					    KVS_VECTOR_OK) {
						return KVS_VECTOR_ERR;
					}
					index->rebuild_profile.add_top_heap_push_count++;
					index->rebuild_profile.layer_search_top_heap_push_count++;
					if (level == 0) {
						index->rebuild_profile.layer_search_level0_top_heap_push_count++;
					}
					if (next.score < lower_bound_score) {
						lower_bound_score = next.score;
						index->rebuild_profile.layer_search_lower_bound_update_count++;
						if (level == 0) {
							index->rebuild_profile.layer_search_level0_lower_bound_update_count++;
						}
					}
				} else {
					if (kvs_vector_heap_replace_top(&ctx->top_heap, next) !=
					    KVS_VECTOR_OK) {
						return KVS_VECTOR_ERR;
					}
					index->rebuild_profile.add_top_heap_replace_count++;
					index->rebuild_profile.layer_search_top_heap_replace_count++;
					if (level == 0) {
						index->rebuild_profile.layer_search_level0_top_heap_replace_count++;
					}
					top = kvs_vector_heap_top(&ctx->top_heap);
					if (top) {
						lower_bound_score = top->score;
						index->rebuild_profile.layer_search_lower_bound_update_count++;
						if (level == 0) {
							index->rebuild_profile.layer_search_level0_lower_bound_update_count++;
						}
					}
				}
				if (ctx->candidate_heap.size < candidate_capacity) {
					(void)kvs_vector_heap_push(&ctx->candidate_heap, next);
					index->rebuild_profile.add_candidate_push_count++;
					index->rebuild_profile.layer_search_candidate_heap_push_count++;
					if (level == 0) {
						index->rebuild_profile.layer_search_level0_candidate_heap_push_count++;
					}
				} else {
					index->rebuild_profile.layer_search_candidate_heap_skip_count++;
					index->rebuild_profile.layer_search_candidate_heap_full_skip_count++;
					if (level == 0) {
						index->rebuild_profile.layer_search_level0_candidate_heap_skip_count++;
					}
				}
			} else {
				index->rebuild_profile.layer_search_top_heap_skip_count++;
				index->rebuild_profile.layer_search_candidate_heap_skip_count++;
				if (level == 0) {
					index->rebuild_profile.layer_search_level0_top_heap_skip_count++;
					index->rebuild_profile.layer_search_level0_candidate_heap_skip_count++;
				}
			}
		}
	}

	export_limit = kvs_hnsw_layer_capacity(index, level);
	kvs_hnsw_add_export_best_candidates(ctx, export_limit, out_count);
	if (level == 0) {
		index->rebuild_profile.layer_search_level0_us +=
			kvs_hnsw_profile_now_us() - search_start_us;
	} else {
		index->rebuild_profile.layer_search_upper_us +=
			kvs_hnsw_profile_now_us() - search_start_us;
	}

	return KVS_VECTOR_OK;
}

static int kvs_hnsw_connect_layer_from_context(
	kvs_hnsw_index_t *index,
	uint32_t slot,
	kvs_hnsw_search_context_t *ctx,
	size_t candidate_count,
	uint8_t level
) {
	size_t limit = candidate_count;
	size_t capacity = kvs_hnsw_layer_capacity(index, level);
	size_t selected_count;
	size_t i;

	if (limit > capacity) {
		limit = capacity;
	}
	selected_count = limit;
	kvs_hnsw_profile_neighbor_select_call(index, level, candidate_count);
	kvs_hnsw_profile_neighbor_select_selected(index, level, selected_count);
	kvs_hnsw_profile_link_selected(index, selected_count);

	for (i = 0; i < limit; i++) {
		uint32_t neighbor = ctx->neighbor_buffer[i];
		float candidate_score = ctx->distance_buffer[i];
		int ret;

		if (neighbor == slot) {
			continue;
		}

		ret = kvs_hnsw_add_new_slot_bidirectional_edge(
			index,
			slot,
			neighbor,
			level,
			candidate_score
		);
		if (ret != KVS_VECTOR_OK) {
			return ret;
		}
	}

	return KVS_VECTOR_OK;
}

static void kvs_hnsw_clear_slot_edges(
	kvs_hnsw_index_t *index,
	uint32_t slot
) {
	if ((size_t)slot >= index->capacity) {
		return;
	}

		memset(
			index->level0_neighbors + ((size_t)slot * index->M0),
			0,
			index->M0 * sizeof(uint32_t)
		);
		if (index->level0_neighbor_scores) {
			memset(
				index->level0_neighbor_scores + ((size_t)slot * index->M0),
			0,
			index->M0 * sizeof(float)
		);
	}
	index->level0_counts[slot] = 0;

	memset(
		index->upper_neighbors +
			((size_t)slot * KVS_HNSW_MAX_LEVEL * index->M),
		0,
		KVS_HNSW_MAX_LEVEL * index->M * sizeof(uint32_t)
	);
	memset(
		index->upper_counts + ((size_t)slot * KVS_HNSW_MAX_LEVEL),
		0,
		KVS_HNSW_MAX_LEVEL * sizeof(uint16_t)
	);
}

int kvs_hnsw_index_add(
	kvs_hnsw_index_t *index,
	uint64_t record_id,
	const float *vector,
	size_t dim
) {
	kvs_hnsw_search_context_t *ctx = NULL;
	uint32_t existing_slot = 0;
	uint32_t slot;
	uint32_t curr;
	uint8_t new_level;
	uint8_t old_max_level;
	uint8_t connect_start_level;
	int level;
	size_t candidate_count = 0;
	uint64_t stage_start_us;
	int ret;

	if (!index || !vector || dim == 0) {
		return KVS_VECTOR_ERR_INVALID_ARG;
	}

	if (!index->ready || index->dim == 0 || dim != index->dim) {
		return KVS_VECTOR_ERR_INVALID_ARG;
	}

	if (!index->vectors || !index->levels || !index->deleted_flags ||
	    !index->slot_to_record_id || !index->level0_neighbors ||
	    !index->level0_neighbor_scores || !index->level0_counts ||
	    !index->upper_neighbors ||
	    !index->upper_counts || !index->record_id_to_slot.entries) {
		return KVS_VECTOR_ERR_INVALID_ARG;
	}

	ret = kvs_vector_id_slot_map_get(
		&index->record_id_to_slot,
		record_id,
		&existing_slot
	);
	if (ret == KVS_VECTOR_OK) {
		if ((size_t)existing_slot >= index->size) {
			return KVS_VECTOR_ERR;
		}

		memcpy(
			index->vectors + ((size_t)existing_slot * index->dim),
			vector,
			index->dim * sizeof(float)
		);
		kvs_hnsw_refresh_level0_neighbor_scores(index);
		if (index->level0_worst_valid && index->capacity > 0) {
			memset(index->level0_worst_valid, 0, index->capacity * sizeof(uint8_t));
		}

		if (index->deleted_flags[existing_slot]) {
			index->deleted_flags[existing_slot] = 0;
			if (index->deleted_count > 0) {
				index->deleted_count--;
			}
		}

		index->slot_to_record_id[existing_slot] = record_id;
		return KVS_VECTOR_OK;
	}
	if (ret != KVS_VECTOR_ERR_NOT_FOUND) {
		return ret;
	}

	if (index->size >= index->capacity || index->size > UINT32_MAX) {
		return KVS_VECTOR_ERR;
	}

	slot = (uint32_t)index->size;
	stage_start_us = kvs_hnsw_profile_now_us();
	new_level = kvs_hnsw_index_next_level(index);
	index->rebuild_profile.hnsw_add_random_level_us +=
		kvs_hnsw_profile_now_us() - stage_start_us;
	if (new_level >= KVS_HNSW_MAX_LEVEL) {
		new_level = KVS_HNSW_MAX_LEVEL - 1;
	}
	old_max_level = index->max_level;
	curr = index->entry_point;

	stage_start_us = kvs_hnsw_profile_now_us();
	memcpy(
		index->vectors + ((size_t)slot * index->dim),
		vector,
		index->dim * sizeof(float)
	);
	index->rebuild_profile.hnsw_add_vector_copy_us +=
		kvs_hnsw_profile_now_us() - stage_start_us;
		index->levels[slot] = new_level;
		index->deleted_flags[slot] = 0;
		index->slot_to_record_id[slot] = record_id;
		kvs_hnsw_clear_slot_edges(index, slot);

	ret = kvs_vector_id_slot_map_put(&index->record_id_to_slot, record_id, slot);
	if (ret != KVS_VECTOR_OK) {
			memset(index->vectors + ((size_t)slot * index->dim), 0, index->dim * sizeof(float));
			index->levels[slot] = 0;
			index->deleted_flags[slot] = 0;
			index->slot_to_record_id[slot] = 0;
		kvs_hnsw_clear_slot_edges(index, slot);
		return ret;
	}

	index->size++;

	if (!index->has_entry) {
		index->entry_point = slot;
		index->has_entry = 1;
		index->max_level = new_level;
		index->ready = 1;
		index->needs_rebuild = 0;
		return KVS_VECTOR_OK;
	}

	ctx = kvs_hnsw_context_acquire(&index->search_context_pool);
	if (!ctx) {
		index->needs_rebuild = 1;
		return KVS_VECTOR_ERR;
	}

	for (level = (int)old_max_level; level > (int)new_level; level--) {
		stage_start_us = kvs_hnsw_profile_now_us();
		curr = kvs_hnsw_greedy_search_layer(index, curr, vector, (uint8_t)level);
		index->rebuild_profile.hnsw_add_greedy_search_us +=
			kvs_hnsw_profile_now_us() - stage_start_us;
	}

	connect_start_level = new_level < old_max_level ? new_level : old_max_level;
	for (level = (int)connect_start_level; level >= 0; level--) {
		stage_start_us = kvs_hnsw_profile_now_us();
		ret = kvs_hnsw_search_layer_for_add(
			index,
			ctx,
			curr,
			vector,
			(uint8_t)level,
			&candidate_count
		);
		if (ret != KVS_VECTOR_OK) {
			kvs_hnsw_context_release(&index->search_context_pool, ctx);
			index->needs_rebuild = 1;
			return ret;
		}
		index->rebuild_profile.hnsw_add_layer_search_us +=
			kvs_hnsw_profile_now_us() - stage_start_us;

		ret = kvs_hnsw_connect_layer_from_context(
			index,
			slot,
			ctx,
			candidate_count,
			(uint8_t)level
		);
		if (ret != KVS_VECTOR_OK) {
			kvs_hnsw_context_release(&index->search_context_pool, ctx);
			index->needs_rebuild = 1;
			return ret;
		}

		if (candidate_count > 0) {
			curr = ctx->neighbor_buffer[0];
		}
	}

	kvs_hnsw_context_release(&index->search_context_pool, ctx);

	if (new_level > old_max_level) {
		index->entry_point = slot;
		index->max_level = new_level;
	}

	index->ready = 1;
	index->needs_rebuild = 0;
	return KVS_VECTOR_OK;
}
