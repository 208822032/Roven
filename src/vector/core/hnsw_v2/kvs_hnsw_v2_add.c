#include "vector/kvs_vector_hnsw_v2.h"
#include "vector/kvs_vector_hnsw_v2_heap.h"
#include "vector/kvs_vector_hnsw_v2_score_cache.h"

#include "kvs_memory.h"

#include <stdlib.h>
#include <string.h>
#include <sys/time.h>

static uint64_t kvs_hnsw_v2_now_us(void) {
	struct timeval tv;

	gettimeofday(&tv, NULL);
	return (uint64_t)tv.tv_sec * 1000000ULL + (uint64_t)tv.tv_usec;
}

static void kvs_hnsw_v2_next_construction_visit_tag(
	kvs_hnsw_v2_index_t *index
) {
	if (!index || !index->visited_tags) {
		return;
	}

	index->current_visit_tag++;
	if (index->current_visit_tag == 0) {
		memset(index->visited_tags, 0, index->capacity * sizeof(uint32_t));
		index->current_visit_tag = 1;
	}
}

static int kvs_hnsw_v2_construction_mark_visited(
	kvs_hnsw_v2_index_t *index,
	uint32_t slot
) {
	if (!index || !index->visited_tags || slot >= index->size) {
		return 0;
	}

	if (index->visited_tags[slot] == index->current_visit_tag) {
		return 0;
	}

	index->visited_tags[slot] = index->current_visit_tag;
	index->build_stats.construction_visited_count++;
	return 1;
}

static uint32_t kvs_hnsw_v2_best_candidate_slot(
	const kvs_hnsw_v2_candidate_t *items,
	size_t count,
	uint32_t fallback
) {
	if (!items || count == 0) {
		return fallback;
	}

	return items[0].slot;
}

/* ── Stage 9E1a diagnostic helpers ───────────────────────────────────
 * These are gated by index->diag.enabled and add zero measurable
 * overhead when diag is disabled (one branch per sampled site).
 * They compute exact full-candidate sets for comparison with bounded
 * layer-search results and are NOT optimization/production code. */

static int kvs_hnsw_v2_diag_should_sample(kvs_hnsw_v2_index_t *index)
{
	if (!index || !index->diag.enabled) {
		return 0;
	}
	if (index->diag.sample_interval == 0) {
		return 1;
	}
	return (index->diag.add_seq % index->diag.sample_interval) == 0;
}

static void kvs_hnsw_v2_reverse_prune_hist_add(
	uint64_t *hist,
	size_t value
) {
	size_t bucket;

	if (!hist) {
		return;
	}
	bucket = value;
	if (bucket >= KVS_HNSW_V2_REVERSE_PRUNE_HIST_BUCKETS) {
		bucket = KVS_HNSW_V2_REVERSE_PRUNE_HIST_BUCKETS - 1;
	}
	hist[bucket]++;
}

static uint64_t kvs_hnsw_v2_pair_diag_key(
	uint32_t lhs_slot,
	uint32_t rhs_slot
) {
	uint32_t lo = lhs_slot < rhs_slot ? lhs_slot : rhs_slot;
	uint32_t hi = lhs_slot < rhs_slot ? rhs_slot : lhs_slot;

	return ((uint64_t)lo << 32) | (uint64_t)hi;
}

static size_t kvs_hnsw_v2_pair_diag_hash(uint64_t key)
{
	key ^= key >> 33;
	key *= 0xff51afd7ed558ccdULL;
	key ^= key >> 33;
	key *= 0xc4ceb9fe1a85ec53ULL;
	key ^= key >> 33;
	return (size_t)key;
}

static int kvs_hnsw_v2_reverse_prune_diag_begin(
	kvs_hnsw_v2_index_t *index
) {
	kvs_hnsw_v2_reverse_prune_diag_t *diag;

	if (!index) {
		return 0;
	}
	diag = &index->reverse_prune_diag;
	diag->reverse_prune_seq++;
	diag->sample_active = 0;
	diag->in_neighbor_select = 0;
	if (!diag->enabled || !diag->pair_table || diag->pair_table_capacity == 0) {
		return 0;
	}
	if (diag->sample_interval != 0 &&
	    ((diag->reverse_prune_seq - 1) % diag->sample_interval) != 0) {
		return 0;
	}

	diag->sample_active = 1;
	diag->sampled_reverse_prune_call_count++;
	return 1;
}

static void kvs_hnsw_v2_reverse_prune_diag_end(
	kvs_hnsw_v2_index_t *index
) {
	if (!index) {
		return;
	}
	index->reverse_prune_diag.sample_active = 0;
	index->reverse_prune_diag.in_neighbor_select = 0;
}

void kvs_hnsw_v2_reverse_prune_diag_record_score_pair(
	kvs_hnsw_v2_index_t *index,
	uint32_t lhs_slot,
	uint32_t rhs_slot,
	kvs_hnsw_v2_reverse_prune_score_kind_t kind
) {
	kvs_hnsw_v2_reverse_prune_diag_t *diag;
	uint64_t key;
	size_t mask;
	size_t pos;
	kvs_hnsw_v2_pair_diag_entry_t *entry;

	if (!index || lhs_slot == rhs_slot) {
		return;
	}
	diag = &index->reverse_prune_diag;
	if (!diag->enabled || !diag->sample_active || !diag->pair_table ||
	    diag->pair_table_capacity == 0) {
		return;
	}
	if (kind == KVS_HNSW_V2_RP_SCORE_NEIGHBOR_SELECT_PAIRWISE &&
	    !diag->in_neighbor_select) {
		return;
	}

	diag->sampled_score_pair_observation_count++;
	if (kind == KVS_HNSW_V2_RP_SCORE_DIRECT_EXISTING) {
		diag->sampled_direct_existing_score_count++;
	} else if (kind == KVS_HNSW_V2_RP_SCORE_DIRECT_NEW) {
		diag->sampled_direct_new_score_count++;
	} else if (kind == KVS_HNSW_V2_RP_SCORE_NEIGHBOR_SELECT_PAIRWISE) {
		diag->sampled_neighbor_select_pairwise_score_count++;
	}

	key = kvs_hnsw_v2_pair_diag_key(lhs_slot, rhs_slot);
	if (key == KVS_HNSW_V2_PAIR_DIAG_EMPTY_KEY) {
		return;
	}

	mask = diag->pair_table_capacity - 1;
	if ((diag->pair_table_capacity & mask) != 0) {
		mask = 0;
	}
	pos = mask ? (kvs_hnsw_v2_pair_diag_hash(key) & mask) :
		(kvs_hnsw_v2_pair_diag_hash(key) % diag->pair_table_capacity);
	entry = &diag->pair_table[pos];
	if (entry->key == key) {
		entry->count++;
		diag->sampled_repeated_pair_score_count++;
		if (kind == KVS_HNSW_V2_RP_SCORE_DIRECT_EXISTING) {
			diag->sampled_direct_existing_repeated_count++;
		} else if (kind == KVS_HNSW_V2_RP_SCORE_NEIGHBOR_SELECT_PAIRWISE) {
			diag->sampled_neighbor_select_pairwise_repeated_count++;
		}
		return;
	}
	if (entry->key == KVS_HNSW_V2_PAIR_DIAG_EMPTY_KEY) {
		entry->key = key;
		entry->count = 1;
		diag->pair_table_used++;
		diag->sampled_unique_pair_score_count++;
		return;
	}

	diag->pair_table_saturated_count++;
}

/* Compute exact top-`topk` candidates among all existing slots whose
 * level >= `level`.  This is a brute-force O(size * dim) scan, used
 * only for diagnostic comparison. */
static size_t kvs_hnsw_v2_diag_exact_top(
	kvs_hnsw_v2_index_t *index,
	const float *query,
	uint8_t level,
	size_t topk,
	kvs_hnsw_v2_candidate_t *out)
{
	size_t i;
	size_t out_count = 0;

	if (!index || !query || !out || topk == 0) {
		return 0;
	}

	for (i = 0; i < index->size; i++) {
		const float *vec;
		float score;
		kvs_hnsw_v2_candidate_t item;
		size_t pos;

		if (index->deleted_flags && index->deleted_flags[i]) {
			continue;
		}
		if (index->levels && index->levels[i] < level) {
			continue;
		}
		vec = kvs_hnsw_v2_slot_vector(index, (uint32_t)i);
		if (!vec) {
			continue;
		}
		score = kvs_hnsw_v2_metric_score(index, query, vec);
		/* Increment build_distance counter for visibility in profiles */
		index->build_stats.distance_compute_count++;

		if (out_count < topk || score > out[out_count - 1].score) {
			item.slot = (uint32_t)i;
			item.score = score;
			if (out_count < topk) {
				out_count++;
			}
			pos = out_count - 1;
			while (pos > 0 && out[pos - 1].score < score) {
				out[pos] = out[pos - 1];
				pos--;
			}
			out[pos] = item;
		}
	}

	return out_count;
}

/* Count how many slots in set_a also appear in set_b (slot equality). */
static size_t kvs_hnsw_v2_diag_slot_overlap(
	const kvs_hnsw_v2_candidate_t *set_a,
	size_t count_a,
	const kvs_hnsw_v2_candidate_t *set_b,
	size_t count_b)
{
	size_t i;
	size_t j;
	size_t overlap = 0;

	for (i = 0; i < count_a; i++) {
		for (j = 0; j < count_b; j++) {
			if (set_a[i].slot == set_b[j].slot) {
				overlap++;
				break;
			}
		}
	}
	return overlap;
}

/* Check whether a specific slot appears in a candidate set. */
static int kvs_hnsw_v2_diag_slot_in_set(
	uint32_t slot,
	const kvs_hnsw_v2_candidate_t *set,
	size_t count)
{
	size_t i;

	for (i = 0; i < count; i++) {
		if (set[i].slot == slot) {
			return 1;
		}
	}
	return 0;
}

/* Compare bounded layer-search results against exact full-candidate
 * set for one layer.  Updates diag counters in-place.
 * Checks visited_tags internally (no visited_set needed). */
static void kvs_hnsw_v2_diag_compare_layer_candidates(
	kvs_hnsw_v2_index_t *index,
	const float *query,
	uint8_t level,
	const kvs_hnsw_v2_candidate_t *bounded_top,
	size_t bounded_count)
{
	kvs_hnsw_v2_candidate_t exact_stack[256];
	kvs_hnsw_v2_candidate_t *exact;
	size_t ef;
	size_t M_layer;
	size_t exact_count;
	size_t overlap_at_M;
	size_t overlap_at_ef;
	int exact_best_visited;
	int exact_best_in_bounded;

	if (!index || !query || !bounded_top) {
		return;
	}

	ef = index->ef_construction;
	if (ef < index->M0) {
		ef = index->M0;
	}
	if (ef > 256) {
		ef = 256; /* cap diagnostic memory */
	}
	M_layer = (level == 0) ? index->M0 : index->M;

	exact = exact_stack;
	if (ef > 256) {
		exact = (kvs_hnsw_v2_candidate_t *)malloc(
			ef * sizeof(kvs_hnsw_v2_candidate_t));
		if (!exact) {
			return;
		}
	}

	exact_count = kvs_hnsw_v2_diag_exact_top(index, query, level, ef, exact);
	if (exact_count == 0) {
		if (exact != exact_stack) {
			free(exact);
		}
		return;
	}

	/* Overlap at M/M0 */
	{
		size_t k = M_layer < exact_count ? M_layer : exact_count;
		overlap_at_M = kvs_hnsw_v2_diag_slot_overlap(
			exact, k, bounded_top, bounded_count);
	}

	/* Overlap at ef_construction */
	overlap_at_ef = kvs_hnsw_v2_diag_slot_overlap(
		exact, exact_count, bounded_top, bounded_count);

	/* Exact best visited? Check visited_tags directly. */
	exact_best_visited = (index->visited_tags &&
		exact[0].slot < index->capacity &&
		index->visited_tags[exact[0].slot] == index->current_visit_tag);

	/* Exact best in bounded top? */
	exact_best_in_bounded = kvs_hnsw_v2_diag_slot_in_set(
		exact[0].slot, bounded_top, bounded_count);

	if (level == 0) {
		index->diag.level0_layer_search_count++;
		if (exact_best_visited) {
			index->diag.level0_exact_best_visited++;
		}
		if (exact_best_in_bounded) {
			index->diag.level0_exact_best_in_bounded++;
		}
		if (M_layer > 0) {
			index->diag.level0_recall_at_M0_sum +=
				(double)overlap_at_M / (double)M_layer;
		}
		if (ef > 0) {
			index->diag.level0_recall_at_ef_sum +=
				(double)overlap_at_ef / (double)ef;
		}
	} else {
		index->diag.upper_layer_search_count++;
		if (exact_best_visited) {
			index->diag.upper_exact_best_visited++;
		}
		if (exact_best_in_bounded) {
			index->diag.upper_exact_best_in_bounded++;
		}
		if (M_layer > 0) {
			index->diag.upper_recall_at_M_sum +=
				(double)overlap_at_M / (double)M_layer;
		}
		if (ef > 0) {
			index->diag.upper_recall_at_ef_sum +=
				(double)overlap_at_ef / (double)ef;
		}
	}

	if (exact != exact_stack) {
		free(exact);
	}
}

/* Diagnostic: compare neighbor_select results from bounded vs exact
 * top-M/M0 candidates. */
static void kvs_hnsw_v2_diag_compare_neighbor_select(
	kvs_hnsw_v2_index_t *index,
	const float *query,
	uint8_t level,
	const kvs_hnsw_v2_candidate_t *bounded_candidates,
	size_t bounded_count,
	const kvs_hnsw_v2_candidate_t *bounded_selected,
	size_t bounded_selected_count)
{
	size_t M_layer;
	size_t ef;
	kvs_hnsw_v2_candidate_t exact_stack[256];
	kvs_hnsw_v2_candidate_t *exact;
	size_t exact_count;
	kvs_hnsw_v2_candidate_t exact_topM_stack[128];
	kvs_hnsw_v2_candidate_t *exact_topM;
	kvs_hnsw_v2_candidate_t exact_selected_stack[128];
	kvs_hnsw_v2_candidate_t *exact_selected;
	size_t exact_selected_count;
	size_t overlap;
	size_t i;

	if (!index || !query) {
		return;
	}

	M_layer = (level == 0) ? index->M0 : index->M;
	if (M_layer == 0) {
		return;
	}

	ef = index->ef_construction;
	if (ef < M_layer) {
		ef = M_layer;
	}
	if (ef > 256) {
		ef = 256;
	}

	exact = exact_stack;
	if (ef > 256) {
		exact = (kvs_hnsw_v2_candidate_t *)malloc(
			ef * sizeof(kvs_hnsw_v2_candidate_t));
		if (!exact) {
			return;
		}
	}

	exact_count = kvs_hnsw_v2_diag_exact_top(index, query, level, ef, exact);
	if (exact_count == 0) {
		if (exact != exact_stack) {
			free(exact);
		}
		return;
	}

	/* Take top M/M0 from exact */
	exact_topM = exact_topM_stack;
	if (M_layer > 128) {
		exact_topM = (kvs_hnsw_v2_candidate_t *)malloc(
			M_layer * sizeof(kvs_hnsw_v2_candidate_t));
		if (!exact_topM) {
			if (exact != exact_stack) {
				free(exact);
			}
			return;
		}
	}
	{
		size_t k = M_layer < exact_count ? M_layer : exact_count;
		for (i = 0; i < k; i++) {
			exact_topM[i] = exact[i];
		}
	}

	/* Run neighbor_select on exact top-M/M0 */
	exact_selected = exact_selected_stack;
	if (M_layer > 128) {
		exact_selected = (kvs_hnsw_v2_candidate_t *)malloc(
			M_layer * sizeof(kvs_hnsw_v2_candidate_t));
		if (!exact_selected) {
			if (exact_topM != exact_topM_stack) {
				free(exact_topM);
			}
			if (exact != exact_stack) {
				free(exact);
			}
			return;
		}
	}
	exact_selected_count = kvs_hnsw_v2_neighbor_select(
		index,
		query,
		exact_topM,
		M_layer < exact_count ? M_layer : exact_count,
		level,
		exact_selected,
		M_layer
	);

	/* Compare */
	overlap = kvs_hnsw_v2_diag_slot_overlap(
		bounded_selected,
		bounded_selected_count,
		exact_selected,
		exact_selected_count
	);

	index->diag.ns_diag_calls++;
	index->diag.ns_bounded_selected_total += bounded_selected_count;
	index->diag.ns_full_selected_total += exact_selected_count;
	index->diag.ns_overlap_total += overlap;

	/* Check whether any pruned candidate (from bounded) was in exact top-M */
	{
		size_t pruned_was_exact = 0;
		/* candidates that were in bounded but NOT selected by bounded
		 * neighbor_select, yet were in exact_topM */
		for (i = 0; i < bounded_count && i < ef; i++) {
			int in_bounded_selected = kvs_hnsw_v2_diag_slot_in_set(
				bounded_candidates[i].slot,
				bounded_selected,
				bounded_selected_count
			);
			if (in_bounded_selected) {
				continue;
			}
			if (kvs_hnsw_v2_diag_slot_in_set(
				    bounded_candidates[i].slot,
				    exact_topM,
				    M_layer < exact_count ? M_layer : exact_count
			    )) {
				pruned_was_exact++;
			}
		}
		index->diag.ns_pruned_was_in_exact_M += pruned_was_exact;
	}

	/* Check fallback: how many selected came from beyond the first M_layer unique candidates */
	{
		size_t fallback = 0;
		size_t first_M = M_layer < bounded_count ? M_layer : bounded_count;
		for (i = 0; i < bounded_selected_count; i++) {
			if (!kvs_hnsw_v2_diag_slot_in_set(
				    bounded_selected[i].slot,
				    bounded_candidates,
				    first_M
			    )) {
				fallback++;
			}
		}
		index->diag.ns_fallback_count += fallback;
	}

	if (exact_selected != exact_selected_stack) {
		free(exact_selected);
	}
	if (exact_topM != exact_topM_stack) {
		free(exact_topM);
	}
	if (exact != exact_stack) {
		free(exact);
	}
}

/* Diagnostic: entry-point drift in upper greedy search.
 * Computes exact best score at this level and compares with
 * greedy final score. */
static void kvs_hnsw_v2_diag_entry_drift(
	kvs_hnsw_v2_index_t *index,
	const float *query,
	uint8_t level,
	uint32_t greedy_final_slot,
	float greedy_final_score)
{
	kvs_hnsw_v2_candidate_t exact_top[1];
	size_t exact_count;
	uint32_t exact_best_slot;

	exact_count = kvs_hnsw_v2_diag_exact_top(
		index, query, level, 1, exact_top);
	if (exact_count == 0) {
		return;
	}

	exact_best_slot = exact_top[0].slot;
	index->diag.upper_greedy_diag_calls++;
	index->diag.upper_drift_score_sum +=
		(double)(exact_top[0].score - greedy_final_score);
	if (exact_best_slot == greedy_final_slot) {
		index->diag.upper_drift_slot_match++;
	}
}

/* Diagnostic: level0 entry quality.
 * Compares the entry point handed to level0 search with the exact
 * best node at level 0. */
static void kvs_hnsw_v2_diag_level0_entry_quality(
	kvs_hnsw_v2_index_t *index,
	const float *query,
	uint32_t entry_slot)
{
	kvs_hnsw_v2_candidate_t exact_top[1];
	size_t exact_count;
	const float *entry_vec;
	float entry_score;

	exact_count = kvs_hnsw_v2_diag_exact_top(
		index, query, 0, 1, exact_top);
	if (exact_count == 0) {
		return;
	}

	entry_vec = kvs_hnsw_v2_slot_vector(index, entry_slot);
	if (!entry_vec) {
		return;
	}
	entry_score = kvs_hnsw_v2_metric_score(index, query, entry_vec);

	index->diag.level0_entry_diag_calls++;
	if (exact_top[0].score > 0.0f) {
		/* For IP metric, higher is better.  Ratio measures how
		 * close entry is to the true best.  1.0 = perfect. */
		index->diag.level0_entry_score_ratio_sum +=
			(double)entry_score / (double)exact_top[0].score;
	} else if (entry_score <= exact_top[0].score) {
		index->diag.level0_entry_score_ratio_sum += 1.0;
	}
}

/* ── end Stage 9E1a diagnostic helpers ──────────────────────────── */

static uint32_t kvs_hnsw_v2_greedy_layer_for_add(
	kvs_hnsw_v2_index_t *index,
	const float *query,
	uint8_t level,
	uint32_t entry
) {
	uint32_t current = entry;
	const float *current_vec;
	float current_score;
	int changed = 1;

	if (!index || !query || current >= index->size) {
		return entry;
	}

	current_vec = kvs_hnsw_v2_slot_vector(index, current);
	if (!current_vec) {
		return entry;
	}

	current_score = kvs_hnsw_v2_metric_score(index, query, current_vec);
	index->build_stats.distance_compute_count++;
	index->build_stats.construction_distance_compute_count++;
	if (level == 0) {
		index->build_stats.construction_level0_distance_compute_count++;
	} else {
		index->build_stats.construction_upper_distance_compute_count++;
	}

	while (changed) {
		uint32_t *neighbors;
		uint16_t *count_ptr;
		uint16_t i;

		changed = 0;
		neighbors = kvs_hnsw_v2_layer_neighbors(index, current, level);
		count_ptr = kvs_hnsw_v2_layer_count_ptr(index, current, level);
		if (!neighbors || !count_ptr) {
			break;
		}

		for (i = 0; i < *count_ptr; i++) {
			uint32_t neighbor = neighbors[i];
			const float *neighbor_vec;
			float score;

			index->build_stats.construction_neighbor_scan_count++;
			if (neighbor >= index->size ||
			    (index->deleted_flags && index->deleted_flags[neighbor]) ||
			    (index->levels && index->levels[neighbor] < level)) {
				index->build_stats.construction_candidate_skip_count++;
				continue;
			}

			neighbor_vec = kvs_hnsw_v2_slot_vector(index, neighbor);
			if (!neighbor_vec) {
				index->build_stats.construction_candidate_skip_count++;
				continue;
			}

			score = kvs_hnsw_v2_metric_score(index, query, neighbor_vec);
			index->build_stats.distance_compute_count++;
			index->build_stats.construction_distance_compute_count++;
			if (level == 0) {
				index->build_stats.construction_level0_distance_compute_count++;
			} else {
				index->build_stats.construction_upper_distance_compute_count++;
			}
			if (score > current_score) {
				current = neighbor;
				current_score = score;
				changed = 1;
			}
		}
	}

	return current;
}

static int kvs_hnsw_v2_search_layer_for_add(
	kvs_hnsw_v2_index_t *index,
	const float *query,
	uint32_t entry,
	uint8_t level,
	size_t ef_construction,
	kvs_hnsw_v2_candidate_t *candidate_queue,
	size_t candidate_queue_capacity,
	kvs_hnsw_v2_candidate_t *top,
	size_t top_capacity,
	size_t *out_count
) {
	kvs_hnsw_v2_candidate_t item;
	size_t candidate_count = 0;
	size_t top_count = 0;
	const float *entry_vec;
	float entry_score;

	if (!index || !query || !candidate_queue || !top || !out_count ||
	    ef_construction == 0 || top_capacity == 0 || entry >= index->size) {
		return KVS_VECTOR_ERR_INVALID_ARG;
	}

	*out_count = 0;
	entry_vec = kvs_hnsw_v2_slot_vector(index, entry);
	if (!entry_vec) {
		return KVS_VECTOR_ERR_INVALID_ARG;
	}

	kvs_hnsw_v2_next_construction_visit_tag(index);
	entry_score = kvs_hnsw_v2_metric_score(index, query, entry_vec);
	index->build_stats.distance_compute_count++;
	index->build_stats.construction_distance_compute_count++;
	if (level == 0) {
		index->build_stats.construction_level0_distance_compute_count++;
	} else {
		index->build_stats.construction_upper_distance_compute_count++;
	}
	item.slot = entry;
	item.score = entry_score;
	kvs_hnsw_v2_construction_mark_visited(index, entry);
	if (kvs_hnsw_v2_candidate_heap_push(
		    candidate_queue,
		    &candidate_count,
		    candidate_queue_capacity,
		    item,
		    &index->build_stats.construction_heap_sift_count
	    ) != KVS_VECTOR_OK) {
		return KVS_VECTOR_ERR;
	}
	index->build_stats.construction_candidate_push_count++;
	if (kvs_hnsw_v2_top_heap_add_bounded(
		    top,
		    &top_count,
		    top_capacity,
		    item,
		    NULL,
		    &index->build_stats.construction_heap_sift_count
	    ) == KVS_VECTOR_OK) {
		index->build_stats.construction_top_push_count++;
	} else {
		index->build_stats.construction_top_skip_count++;
	}

	while (candidate_count > 0) {
		kvs_hnsw_v2_candidate_t current;
		uint32_t *neighbors;
		uint16_t *count_ptr;
		uint16_t i;
		float lower_bound;

		if (kvs_hnsw_v2_candidate_heap_pop_best(
			    candidate_queue,
			    &candidate_count,
			    &current,
			    &index->build_stats.construction_heap_sift_count
		    ) != KVS_VECTOR_OK) {
			break;
		}
		index->build_stats.construction_candidate_pop_count++;

		lower_bound = kvs_hnsw_v2_top_heap_lower_bound(top, top_count);
		if (top_count >= ef_construction && current.score < lower_bound) {
			break;
		}

		neighbors = kvs_hnsw_v2_layer_neighbors(index, current.slot, level);
		count_ptr = kvs_hnsw_v2_layer_count_ptr(index, current.slot, level);
		if (!neighbors || !count_ptr) {
			continue;
		}

		for (i = 0; i < *count_ptr; i++) {
			uint32_t neighbor = neighbors[i];
			const float *neighbor_vec;
			float score;

			index->build_stats.construction_neighbor_scan_count++;
			if (neighbor >= index->size ||
			    (index->levels && index->levels[neighbor] < level)) {
				index->build_stats.construction_candidate_skip_count++;
				continue;
			}
			if (!kvs_hnsw_v2_construction_mark_visited(index, neighbor)) {
				index->build_stats.construction_candidate_skip_count++;
				continue;
			}
			if (index->deleted_flags && index->deleted_flags[neighbor]) {
				index->build_stats.construction_candidate_skip_count++;
				continue;
			}

			neighbor_vec = kvs_hnsw_v2_slot_vector(index, neighbor);
			if (!neighbor_vec) {
				index->build_stats.construction_candidate_skip_count++;
				continue;
			}
			score = kvs_hnsw_v2_metric_score(index, query, neighbor_vec);
			index->build_stats.distance_compute_count++;
			index->build_stats.construction_distance_compute_count++;
			if (level == 0) {
				index->build_stats.construction_level0_distance_compute_count++;
			} else {
				index->build_stats.construction_upper_distance_compute_count++;
			}

			lower_bound = kvs_hnsw_v2_top_heap_lower_bound(top, top_count);
			if (top_count < ef_construction || score > lower_bound) {
				kvs_hnsw_v2_candidate_t candidate;
				int top_replaced = 0;

				candidate.slot = neighbor;
				candidate.score = score;
				if (kvs_hnsw_v2_candidate_heap_push(
					    candidate_queue,
					    &candidate_count,
					    candidate_queue_capacity,
					    candidate,
					    &index->build_stats.construction_heap_sift_count
				    ) == KVS_VECTOR_OK) {
					index->build_stats.construction_candidate_push_count++;
					if (kvs_hnsw_v2_top_heap_add_bounded(
						top,
						&top_count,
						top_capacity,
						candidate,
						&top_replaced,
						&index->build_stats.construction_heap_sift_count
					    ) == KVS_VECTOR_OK) {
						if (top_replaced) {
							index->build_stats.construction_top_replace_count++;
						} else {
							index->build_stats.construction_top_push_count++;
						}
					} else {
						index->build_stats.construction_top_skip_count++;
					}
				} else {
					index->build_stats.construction_candidate_skip_count++;
				}
			} else {
				index->build_stats.construction_candidate_skip_count++;
				index->build_stats.construction_top_skip_count++;
			}
		}
	}

	index->build_stats.candidate_collect_kept_count += top_count;
	kvs_hnsw_v2_heap_sort_desc(top, top_count);

	/* Stage 9E1a: diagnostic candidate-quality comparison */
	if (kvs_hnsw_v2_diag_should_sample(index)) {
		kvs_hnsw_v2_diag_compare_layer_candidates(
			index, query, level, top, top_count);
	}

	*out_count = top_count;
	return KVS_VECTOR_OK;
}

/* ── Stage 9E1a reverse prune quality diagnostic ────────────────── */

static void kvs_hnsw_v2_diag_reverse_quality(
	kvs_hnsw_v2_index_t *index,
	uint32_t old_slot,
	uint32_t new_slot,
	uint8_t level,
	int rejected_before_prune,
	int pruned_after_select,
	const kvs_hnsw_v2_candidate_t *selected,
	size_t selected_count)
{
	kvs_hnsw_v2_candidate_t exact_best[1];
	const float *old_vec;
	const float *new_vec;
	float new_score;
	size_t exact_count;
	float best_selected_score;

	if (!index || !selected) {
		return;
	}

	old_vec = kvs_hnsw_v2_slot_vector(index, old_slot);
	new_vec = kvs_hnsw_v2_slot_vector(index, new_slot);
	if (!old_vec || !new_vec) {
		return;
	}

	/* Compute exact best neighbor for old_slot at this level */
	exact_count = kvs_hnsw_v2_diag_exact_top(
		index, old_vec, level, 1, exact_best);
	if (exact_count == 0) {
		return;
	}

	index->diag.rp_attempts_sampled++;

	/* Is new_slot the exact best neighbor for old_slot? */
	if (exact_best[0].slot == new_slot) {
		if (rejected_before_prune) {
			index->diag.rp_rejected_was_exact_best++;
		}
		if (pruned_after_select) {
			index->diag.rp_pruned_was_exact_best++;
		}
	}

	/* Score ratio: new_slot score vs best selected score */
	new_score = kvs_hnsw_v2_metric_score(index, old_vec, new_vec);
	if (selected_count > 0) {
		best_selected_score = selected[0].score;
		if (best_selected_score > 0.0f) {
			index->diag.rp_new_slot_score_ratio_sum +=
				(double)new_score / (double)best_selected_score;
		} else if (new_score <= best_selected_score) {
			index->diag.rp_new_slot_score_ratio_sum += 1.0;
		}
	}
}

static int kvs_hnsw_v2_add_reverse_edge(
	kvs_hnsw_v2_index_t *index,
	uint32_t old_slot,
	uint32_t new_slot,
	uint8_t level,
	kvs_hnsw_v2_reverse_prune_ctx_t *reverse_prune_ctx
) {
	int ret;
	uint16_t before;
	uint16_t *count_ptr;
	uint32_t *neighbors;
	size_t capacity;
	size_t candidate_count;
	size_t selected_count;
	size_t i;
	const float *old_vec;
	kvs_hnsw_v2_candidate_t stack_candidates[128];
	kvs_hnsw_v2_candidate_t stack_selected[128];
	kvs_hnsw_v2_candidate_t *candidates = stack_candidates;
	kvs_hnsw_v2_candidate_t *selected = stack_selected;
	uint64_t start_us;
	uint64_t prune_start_us = 0;
	int diag_active = 0;

	start_us = kvs_hnsw_v2_now_us();
	index->graph_stats.reverse_edge_attempt_count++;
	count_ptr = kvs_hnsw_v2_layer_count_ptr(index, old_slot, level);
	neighbors = kvs_hnsw_v2_layer_neighbors(index, old_slot, level);
	capacity = kvs_hnsw_v2_layer_capacity(index, level);
	old_vec = kvs_hnsw_v2_slot_vector(index, old_slot);
	if (!count_ptr || !neighbors || !old_vec || capacity == 0) {
		index->graph_stats.reverse_edge_rejected_count++;
		return KVS_VECTOR_ERR_INVALID_ARG;
	}

	if (kvs_hnsw_v2_layer_has_edge(index, old_slot, new_slot, level)) {
		index->graph_stats.duplicate_edge_count++;
		index->graph_stats.reverse_edge_rejected_count++;
		/* Stage 9E1a: reverse prune quality diagnostic */
		if (kvs_hnsw_v2_diag_should_sample(index)) {
			kvs_hnsw_v2_diag_reverse_quality(
				index, old_slot, new_slot, level,
				1, 0, NULL, 0);
		}
		return KVS_VECTOR_ERR;
	}

	before = *count_ptr;
	if ((size_t)before < kvs_hnsw_v2_layer_capacity(index, level)) {
		ret = kvs_hnsw_v2_layer_append_edge(index, old_slot, new_slot, level);
		if (ret == KVS_VECTOR_OK) {
			index->graph_stats.reverse_edge_accepted_count++;
		} else {
			index->graph_stats.reverse_edge_rejected_count++;
		}
		index->build_stats.reverse_link_us += kvs_hnsw_v2_now_us() - start_us;
		return ret;
	}

	prune_start_us = kvs_hnsw_v2_now_us();
	candidate_count = (size_t)before + 1;
	index->build_stats.reverse_prune_call_count++;
	index->build_stats.reverse_prune_candidate_count_total += candidate_count;
	index->build_stats.reverse_prune_existing_degree_total += before;
	kvs_hnsw_v2_reverse_prune_hist_add(
		index->build_stats.reverse_prune_candidate_count_hist,
		candidate_count);
	kvs_hnsw_v2_reverse_prune_hist_add(
		index->build_stats.reverse_prune_existing_degree_hist,
		before);
	if (candidate_count > 128) {
		candidates = (kvs_hnsw_v2_candidate_t *)kvs_malloc(
			candidate_count * sizeof(kvs_hnsw_v2_candidate_t)
		);
		selected = (kvs_hnsw_v2_candidate_t *)kvs_malloc(
			capacity * sizeof(kvs_hnsw_v2_candidate_t)
		);
		if (!candidates || !selected) {
			kvs_free(candidates);
			kvs_free(selected);
			index->graph_stats.reverse_edge_rejected_count++;
			index->build_stats.reverse_link_us +=
				kvs_hnsw_v2_now_us() - start_us;
			index->build_stats.reverse_prune_us +=
				kvs_hnsw_v2_now_us() - prune_start_us;
			return KVS_VECTOR_ERR;
		}
	}

	diag_active = kvs_hnsw_v2_reverse_prune_diag_begin(index);
	for (i = 0; i < before; i++) {
		const float *neighbor_vec = kvs_hnsw_v2_slot_vector(index, neighbors[i]);

		candidates[i].slot = neighbors[i];
		candidates[i].score = neighbor_vec ?
			kvs_hnsw_v2_metric_score(index, old_vec, neighbor_vec) : 0.0f;
		if (neighbor_vec) {
			index->build_stats.distance_compute_count++;
			index->build_stats.reverse_prune_distance_compute_count++;
			index->build_stats.reverse_prune_direct_score_count++;
			index->build_stats.reverse_prune_existing_score_count++;
			if (index->reverse_prune_diag.sample_active) {
				kvs_hnsw_v2_reverse_prune_diag_record_score_pair(
					index,
					old_slot,
					neighbors[i],
					KVS_HNSW_V2_RP_SCORE_DIRECT_EXISTING);
			}
		}
	}
	candidates[before].slot = new_slot;
	if (reverse_prune_ctx && reverse_prune_ctx->direct_hint.has_score &&
	    reverse_prune_ctx->direct_hint.from_slot == new_slot &&
	    reverse_prune_ctx->direct_hint.to_slot == old_slot) {
		candidates[before].score = reverse_prune_ctx->direct_hint.score;
		index->build_stats.reverse_prune_known_score_reuse_count++;
		index->build_stats.reverse_prune_direct_score_saved_count++;
		if (reverse_prune_ctx->local_pair_cache) {
			(void)kvs_hnsw_v2_score_cache_insert(
				reverse_prune_ctx->local_pair_cache,
				old_slot,
				new_slot,
				candidates[before].score,
				&index->build_stats
			);
		}
	} else {
		const float *new_vec = kvs_hnsw_v2_slot_vector(index, new_slot);

		candidates[before].score = kvs_hnsw_v2_metric_score(
			index,
			old_vec,
			new_vec
		);
		index->build_stats.distance_compute_count++;
		index->build_stats.reverse_prune_distance_compute_count++;
		index->build_stats.reverse_prune_direct_score_count++;
		index->build_stats.reverse_prune_new_score_count++;
		index->build_stats.reverse_prune_candidate_score_known_recompute_count++;
		index->build_stats.reverse_prune_known_score_reuse_miss_count++;
		if (index->reverse_prune_diag.sample_active) {
			kvs_hnsw_v2_reverse_prune_diag_record_score_pair(
				index,
				old_slot,
				new_slot,
				KVS_HNSW_V2_RP_SCORE_DIRECT_NEW);
		}
	}

	{
		uint64_t select_start_us = kvs_hnsw_v2_now_us();
		uint64_t ns_distance_before =
			index->build_stats.neighbor_select_distance_compute_count;
		index->build_stats.reverse_prune_neighbor_select_call_count++;
		if (diag_active) {
			index->reverse_prune_diag.in_neighbor_select = 1;
		}
		selected_count = kvs_hnsw_v2_neighbor_select_with_cache(
			index,
			old_vec,
			candidates,
			candidate_count,
			level,
			selected,
			capacity,
			reverse_prune_ctx
		);
		if (diag_active) {
			index->reverse_prune_diag.in_neighbor_select = 0;
		}
		index->build_stats.neighbor_select_us +=
			kvs_hnsw_v2_now_us() - select_start_us;
		index->build_stats.reverse_prune_neighbor_select_distance_compute_count +=
			index->build_stats.neighbor_select_distance_compute_count -
			ns_distance_before;
	}

	{
		int rewritten = selected_count != before;
		size_t compare_count = selected_count < (size_t)before ?
			selected_count : (size_t)before;

		for (i = 0; i < compare_count; i++) {
			if (neighbors[i] != selected[i].slot) {
				rewritten = 1;
				break;
			}
		}
		if (rewritten) {
			index->build_stats.reverse_prune_rewrite_count++;
		}
	}

	for (i = 0; i < selected_count; i++) {
		neighbors[i] = selected[i].slot;
	}
	for (; i < before; i++) {
		neighbors[i] = 0;
	}
	*count_ptr = (uint16_t)selected_count;
	if (selected_count < candidate_count) {
		index->graph_stats.reverse_prune_count += candidate_count - selected_count;
	}

	/* Stage 9E1a: reverse prune quality diagnostic */
	if (kvs_hnsw_v2_diag_should_sample(index)) {
		int new_was_pruned = 1;
		size_t di;
		for (di = 0; di < selected_count; di++) {
			if (selected[di].slot == new_slot) {
				new_was_pruned = 0;
				break;
			}
		}
		kvs_hnsw_v2_diag_reverse_quality(
			index, old_slot, new_slot, level,
			0, new_was_pruned,
			selected, selected_count);
	}

	ret = kvs_hnsw_v2_layer_has_edge(index, old_slot, new_slot, level) ?
		KVS_VECTOR_OK : KVS_VECTOR_ERR;
	if (ret == KVS_VECTOR_OK) {
		index->graph_stats.reverse_edge_accepted_count++;
	} else {
		index->graph_stats.reverse_edge_rejected_count++;
		index->build_stats.reverse_prune_reject_count++;
	}
	kvs_hnsw_v2_reverse_prune_diag_end(index);

	if (candidates != stack_candidates) {
		kvs_free(candidates);
		kvs_free(selected);
	}

	index->build_stats.reverse_prune_us += kvs_hnsw_v2_now_us() - prune_start_us;
	index->build_stats.reverse_link_us += kvs_hnsw_v2_now_us() - start_us;
	return ret;
}

static int kvs_hnsw_v2_connect_layer(
	kvs_hnsw_v2_index_t *index,
	uint32_t new_slot,
	uint8_t level,
	kvs_hnsw_v2_candidate_t *selected,
	size_t selected_count,
	kvs_hnsw_v2_score_cache_t *local_pair_cache
) {
	size_t i;

	for (i = 0; i < selected_count; i++) {
		uint32_t neighbor = selected[i].slot;
		uint64_t forward_start_us = kvs_hnsw_v2_now_us();
		kvs_hnsw_v2_reverse_prune_ctx_t reverse_prune_ctx;

		if (kvs_hnsw_v2_layer_append_edge(index, new_slot, neighbor, level) ==
		    KVS_VECTOR_OK) {
			index->graph_stats.forward_edge_added_count++;
		}
		index->build_stats.forward_link_us +=
			kvs_hnsw_v2_now_us() - forward_start_us;

		memset(&reverse_prune_ctx, 0, sizeof(reverse_prune_ctx));
		reverse_prune_ctx.direct_hint.from_slot = new_slot;
		reverse_prune_ctx.direct_hint.to_slot = neighbor;
		reverse_prune_ctx.direct_hint.score = selected[i].score;
		reverse_prune_ctx.direct_hint.has_score = 1;
		reverse_prune_ctx.local_pair_cache = local_pair_cache;
		(void)kvs_hnsw_v2_add_reverse_edge(
			index,
			neighbor,
			new_slot,
			level,
			&reverse_prune_ctx);
	}

	return KVS_VECTOR_OK;
}

static int kvs_hnsw_v2_connect_new_node(
	kvs_hnsw_v2_index_t *index,
	uint32_t new_slot,
	uint8_t node_level
) {
	kvs_hnsw_v2_candidate_t *candidate_queue;
	kvs_hnsw_v2_candidate_t *top_candidates;
	kvs_hnsw_v2_candidate_t *selected;
	kvs_hnsw_v2_score_cache_t score_cache;
	kvs_hnsw_v2_score_cache_entry_t *score_cache_entries = NULL;
	const float *query;
	size_t ef_construction;
	size_t search_capacity;
	size_t score_cache_capacity;
	uint32_t entry;
	uint8_t max_existing_level;
	uint8_t level;
	int ret = KVS_VECTOR_OK;

	if (index->size == 0) {
		return KVS_VECTOR_OK;
	}

	query = kvs_hnsw_v2_slot_vector(index, new_slot);
	if (!query || !index->has_entry || index->entry_point >= index->size) {
		return KVS_VECTOR_ERR_INVALID_ARG;
	}

	ef_construction = index->ef_construction;
	if (ef_construction < index->M0) {
		ef_construction = index->M0;
	}
	if (ef_construction > index->size) {
		ef_construction = index->size;
	}
	if (ef_construction == 0) {
		ef_construction = 1;
	}
	search_capacity = index->size;

	candidate_queue = (kvs_hnsw_v2_candidate_t *)kvs_malloc(
		search_capacity * sizeof(kvs_hnsw_v2_candidate_t)
	);
	top_candidates = (kvs_hnsw_v2_candidate_t *)kvs_malloc(
		ef_construction * sizeof(kvs_hnsw_v2_candidate_t)
	);
	selected = (kvs_hnsw_v2_candidate_t *)kvs_malloc(
		index->M0 * sizeof(kvs_hnsw_v2_candidate_t)
	);
	if (!candidate_queue || !top_candidates || !selected) {
		kvs_free(candidate_queue);
		kvs_free(top_candidates);
		kvs_free(selected);
		return KVS_VECTOR_ERR;
	}

	score_cache_capacity = index->M0 * index->M0;
	if (score_cache_capacity < 4096) {
		score_cache_capacity = 4096;
	}
	score_cache_entries = (kvs_hnsw_v2_score_cache_entry_t *)kvs_malloc(
		score_cache_capacity * sizeof(kvs_hnsw_v2_score_cache_entry_t)
	);
	if (!score_cache_entries) {
		kvs_free(candidate_queue);
		kvs_free(top_candidates);
		kvs_free(selected);
		return KVS_VECTOR_ERR;
	}
	kvs_hnsw_v2_score_cache_init(
		&score_cache,
		score_cache_entries,
		score_cache_capacity);
	kvs_hnsw_v2_score_cache_reset(&score_cache);

	entry = index->entry_point;
	max_existing_level = index->max_level;
	if (max_existing_level >= KVS_HNSW_V2_MAX_LEVEL) {
		max_existing_level = KVS_HNSW_V2_MAX_LEVEL - 1;
	}

	if (max_existing_level > node_level) {
		for (level = max_existing_level; level > node_level; level--) {
			uint64_t layer_start_us = kvs_hnsw_v2_now_us();

			entry = kvs_hnsw_v2_greedy_layer_for_add(
				index,
				query,
				level,
				entry
			);
			index->build_stats.construction_layer_search_us +=
				kvs_hnsw_v2_now_us() - layer_start_us;

			/* Stage 9E1a: entry-point drift diagnostic */
			if (kvs_hnsw_v2_diag_should_sample(index)) {
				const float *final_vec =
					kvs_hnsw_v2_slot_vector(index, entry);
				float final_score = final_vec ?
					kvs_hnsw_v2_metric_score(index, query, final_vec)
					: 0.0f;
				kvs_hnsw_v2_diag_entry_drift(
					index, query, level,
					entry, final_score);
			}
		}
	}

	level = max_existing_level < node_level ? max_existing_level : node_level;
	for (;;) {
		uint8_t current_level = level;
		size_t candidate_count = 0;
		size_t selected_capacity = kvs_hnsw_v2_layer_capacity(index, current_level);
		size_t selected_count;
		uint64_t layer_start_us;
		uint64_t select_start_us;
		int diag_sample;

		diag_sample = kvs_hnsw_v2_diag_should_sample(index);

		/* Stage 9E1a: level0 entry quality diagnostic */
		if (current_level == 0 && diag_sample) {
			kvs_hnsw_v2_diag_level0_entry_quality(
				index, query, entry);
		}

		layer_start_us = kvs_hnsw_v2_now_us();
		ret = kvs_hnsw_v2_search_layer_for_add(
			index,
			query,
			entry,
			current_level,
			ef_construction,
			candidate_queue,
			search_capacity,
			top_candidates,
			ef_construction,
			&candidate_count
		);
		index->build_stats.construction_layer_search_us +=
			kvs_hnsw_v2_now_us() - layer_start_us;
		if (ret != KVS_VECTOR_OK) {
			break;
		}
		entry = kvs_hnsw_v2_best_candidate_slot(
			top_candidates,
			candidate_count,
			entry
		);

		select_start_us = kvs_hnsw_v2_now_us();
		selected_count = kvs_hnsw_v2_neighbor_select(
			index,
			query,
			top_candidates,
			candidate_count,
			current_level,
			selected,
			selected_capacity
		);
		index->build_stats.neighbor_select_us +=
			kvs_hnsw_v2_now_us() - select_start_us;

		/* Stage 9E1a: neighbor select quality comparison */
		if (diag_sample) {
			kvs_hnsw_v2_diag_compare_neighbor_select(
				index, query, current_level,
				top_candidates, candidate_count,
				selected, selected_count);
		}

		index->graph_stats.selected_neighbor_count += selected_count;
		if (selected_count > 0) {
			entry = selected[0].slot;
		}
		ret = kvs_hnsw_v2_connect_layer(
			index,
			new_slot,
			current_level,
			selected,
			selected_count,
			&score_cache
		);
		if (ret != KVS_VECTOR_OK) {
			break;
		}
		if (level == 0) {
			break;
		}
		level--;
	}

	kvs_free(candidate_queue);
	kvs_free(top_candidates);
	kvs_free(selected);
	kvs_free(score_cache_entries);
	return ret;
}

int kvs_hnsw_v2_index_add(
	kvs_hnsw_v2_index_t *index,
	uint64_t record_id,
	const float *vector,
	size_t dim
) {
	uint32_t existing_slot;
	uint32_t slot;
	uint8_t level;
	float *dst;
	int ret;
	int replacing_deleted_record = 0;
	uint64_t add_start_us;
	uint64_t start_us;

	if (!index || !vector || dim != index->dim || index->dim == 0) {
		return KVS_VECTOR_ERR_INVALID_ARG;
	}
	if (!index->ready || !index->vectors || index->capacity == 0) {
		return KVS_VECTOR_ERR_NOT_INITIALIZED;
	}
	if (index->size >= index->capacity || index->size > UINT32_MAX) {
		return KVS_VECTOR_ERR;
	}
	if (kvs_vector_id_slot_map_get(
		    &index->record_id_to_slot,
		    record_id,
		    &existing_slot
	    ) == KVS_VECTOR_OK) {
		if (existing_slot >= index->size ||
		    index->slot_to_record_id[existing_slot] != record_id ||
		    !index->deleted_flags ||
		    !index->deleted_flags[existing_slot]) {
			return KVS_VECTOR_ERR;
		}
		replacing_deleted_record = 1;
	}

	/* Stage 9E1a: diagnostic add sequence counter */
	index->diag.add_seq++;

	add_start_us = kvs_hnsw_v2_now_us();
	start_us = kvs_hnsw_v2_now_us();
	slot = (uint32_t)index->size;
	level = kvs_hnsw_v2_random_level(&index->rng, index->M);
	if (level >= KVS_HNSW_V2_MAX_LEVEL) {
		level = KVS_HNSW_V2_MAX_LEVEL - 1;
	}
	index->build_stats.random_level_us += kvs_hnsw_v2_now_us() - start_us;

	start_us = kvs_hnsw_v2_now_us();
	dst = index->vectors + ((size_t)slot * index->vector_stride);
	memset(dst, 0, index->vector_stride * sizeof(float));
	memcpy(dst, vector, index->dim * sizeof(float));
	index->slot_to_record_id[slot] = record_id;
	index->levels[slot] = level;
	index->deleted_flags[slot] = 0;
	index->visited_tags[slot] = 0;
	index->build_stats.vector_write_us += kvs_hnsw_v2_now_us() - start_us;

	start_us = kvs_hnsw_v2_now_us();
	ret = kvs_vector_id_slot_map_put(&index->record_id_to_slot, record_id, slot);
	index->build_stats.id_slot_map_us += kvs_hnsw_v2_now_us() - start_us;
	if (ret != KVS_VECTOR_OK) {
		memset(dst, 0, index->vector_stride * sizeof(float));
		index->slot_to_record_id[slot] = 0;
		index->levels[slot] = 0;
		index->deleted_flags[slot] = 0;
		return ret;
	}

	if (index->size == 0) {
		index->entry_point = slot;
		index->has_entry = 1;
		index->max_level = level;
		index->size++;
		start_us = kvs_hnsw_v2_now_us();
		kvs_hnsw_v2_graph_stats_refresh(index);
		index->build_stats.graph_stats_refresh_us +=
			kvs_hnsw_v2_now_us() - start_us;
		start_us = kvs_hnsw_v2_now_us();
		kvs_hnsw_v2_memory_refresh(index);
		index->build_stats.memory_refresh_us += kvs_hnsw_v2_now_us() - start_us;
		index->build_stats.add_count++;
		index->build_stats.add_total_us += kvs_hnsw_v2_now_us() - add_start_us;
		return KVS_VECTOR_OK;
	}

	ret = kvs_hnsw_v2_connect_new_node(index, slot, level);
	if (ret != KVS_VECTOR_OK) {
		if (replacing_deleted_record) {
			(void)kvs_vector_id_slot_map_put(
				&index->record_id_to_slot,
				record_id,
				existing_slot
			);
		} else {
			kvs_vector_id_slot_map_del(&index->record_id_to_slot, record_id);
		}
		memset(dst, 0, index->vector_stride * sizeof(float));
		index->slot_to_record_id[slot] = 0;
		index->levels[slot] = 0;
		index->deleted_flags[slot] = 0;
		return ret;
	}

	if (!index->has_entry || level > index->max_level) {
		index->entry_point = slot;
		index->has_entry = 1;
		index->max_level = level;
	}

	index->size++;
	start_us = kvs_hnsw_v2_now_us();
	kvs_hnsw_v2_graph_stats_refresh(index);
	index->build_stats.graph_stats_refresh_us += kvs_hnsw_v2_now_us() - start_us;
	start_us = kvs_hnsw_v2_now_us();
	kvs_hnsw_v2_memory_refresh(index);
	index->build_stats.memory_refresh_us += kvs_hnsw_v2_now_us() - start_us;
	index->build_stats.add_count++;
	index->build_stats.add_total_us += kvs_hnsw_v2_now_us() - add_start_us;
	return KVS_VECTOR_OK;
}
