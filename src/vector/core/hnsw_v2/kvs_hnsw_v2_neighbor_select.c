#include "vector/kvs_vector_hnsw_v2.h"

#include "kvs_hnsw_v2_score_cache.h"

#include <stdlib.h>
#include <string.h>

static void kvs_hnsw_v2_sort_candidates_desc(
	kvs_hnsw_v2_candidate_t *items,
	size_t count
) {
	size_t i;
	size_t j;

	for (i = 1; i < count; i++) {
		kvs_hnsw_v2_candidate_t item = items[i];
		j = i;
		while (j > 0 && items[j - 1].score < item.score) {
			items[j] = items[j - 1];
			j--;
		}
		items[j] = item;
	}
}

static int kvs_hnsw_v2_candidate_slot_seen(
	const kvs_hnsw_v2_candidate_t *items,
	size_t count,
	uint32_t slot
) {
	size_t i;

	for (i = 0; i < count; i++) {
		if (items[i].slot == slot) {
			return 1;
		}
	}

	return 0;
}

size_t kvs_hnsw_v2_neighbor_select_with_cache(
	kvs_hnsw_v2_index_t *index,
	const float *query,
	kvs_hnsw_v2_candidate_t *candidates,
	size_t candidate_count,
	uint8_t level,
	kvs_hnsw_v2_candidate_t *selected,
	size_t selected_capacity,
	kvs_hnsw_v2_reverse_prune_ctx_t *reverse_prune_ctx
) {
	size_t i;
	size_t selected_count = 0;
	size_t unique_count = 0;
	size_t pruned = 0;
	size_t duplicate = 0;
	size_t rejected = 0;
	kvs_hnsw_v2_candidate_t unique_stack[256];
	kvs_hnsw_v2_candidate_t *unique = unique_stack;

	if (!index || !query || !candidates || !selected || selected_capacity == 0) {
		return 0;
	}

	index->graph_stats.neighbor_select_call_count++;
	index->graph_stats.neighbor_select_candidate_count += candidate_count;

	if (candidate_count > 256) {
		unique = (kvs_hnsw_v2_candidate_t *)malloc(
			candidate_count * sizeof(kvs_hnsw_v2_candidate_t)
		);
		if (!unique) {
			return 0;
		}
	}

	for (i = 0; i < candidate_count; i++) {
		if (candidates[i].slot > index->size ||
		    candidates[i].slot >= index->capacity ||
		    (index->levels && index->levels[candidates[i].slot] < level)) {
			rejected++;
			continue;
		}
		if (kvs_hnsw_v2_candidate_slot_seen(unique, unique_count, candidates[i].slot)) {
			duplicate++;
			continue;
		}
		unique[unique_count++] = candidates[i];
	}

	kvs_hnsw_v2_sort_candidates_desc(unique, unique_count);

	for (i = 0; i < unique_count && selected_count < selected_capacity; i++) {
		int keep = 1;
		size_t j;

		for (j = 0; j < selected_count; j++) {
			const float *candidate_vec = kvs_hnsw_v2_slot_vector(
				index,
				unique[i].slot
			);
			const float *selected_vec = kvs_hnsw_v2_slot_vector(
				index,
				selected[j].slot
			);
			float inter_score;
			int cache_hit = 0;

			if (!candidate_vec || !selected_vec) {
				continue;
			}

			if (reverse_prune_ctx && reverse_prune_ctx->local_pair_cache) {
				cache_hit = kvs_hnsw_v2_score_cache_lookup(
					reverse_prune_ctx->local_pair_cache,
					unique[i].slot,
					selected[j].slot,
					&inter_score,
					&index->build_stats
				);
				if (cache_hit) {
					index->build_stats.reverse_prune_pair_score_saved_count++;
				}
			}
			if (!cache_hit) {
				inter_score = kvs_hnsw_v2_metric_score(
					index,
					candidate_vec,
					selected_vec);
				index->build_stats.distance_compute_count++;
				index->build_stats.neighbor_select_distance_compute_count++;
				if (reverse_prune_ctx &&
				    reverse_prune_ctx->local_pair_cache) {
					(void)kvs_hnsw_v2_score_cache_insert(
						reverse_prune_ctx->local_pair_cache,
						unique[i].slot,
						selected[j].slot,
						inter_score,
						&index->build_stats
					);
				}
			}
			if (index->reverse_prune_diag.sample_active &&
			    index->reverse_prune_diag.in_neighbor_select) {
				kvs_hnsw_v2_reverse_prune_diag_record_score_pair(
					index,
					unique[i].slot,
					selected[j].slot,
					KVS_HNSW_V2_RP_SCORE_NEIGHBOR_SELECT_PAIRWISE);
			}
			if (inter_score > unique[i].score && selected_count + 1 < selected_capacity) {
				keep = 0;
				break;
			}
		}

		if (keep) {
			selected[selected_count++] = unique[i];
		} else {
			pruned++;
		}
	}

	if (selected_count < selected_capacity) {
		for (i = 0; i < unique_count && selected_count < selected_capacity; i++) {
			if (kvs_hnsw_v2_candidate_slot_seen(
				    selected,
				    selected_count,
				    unique[i].slot
			    )) {
				continue;
			}
			selected[selected_count++] = unique[i];
		}
	}

	if (unique_count > selected_count + pruned) {
		rejected += unique_count - selected_count - pruned;
	}

	index->graph_stats.neighbor_select_selected_count += selected_count;
	index->graph_stats.neighbor_select_pruned_count += pruned;
	index->graph_stats.neighbor_select_duplicate_count += duplicate;
	index->graph_stats.neighbor_select_rejected_count += rejected;

	if (unique != unique_stack) {
		free(unique);
	}

	return selected_count;
}

size_t kvs_hnsw_v2_neighbor_select(
	kvs_hnsw_v2_index_t *index,
	const float *query,
	kvs_hnsw_v2_candidate_t *candidates,
	size_t candidate_count,
	uint8_t level,
	kvs_hnsw_v2_candidate_t *selected,
	size_t selected_capacity
) {
	return kvs_hnsw_v2_neighbor_select_with_cache(
		index,
		query,
		candidates,
		candidate_count,
		level,
		selected,
		selected_capacity,
		NULL);
}
