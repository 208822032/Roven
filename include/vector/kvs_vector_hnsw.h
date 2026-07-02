#pragma once

#include <stddef.h>
#include <stdint.h>

#include "kvs_vector.h"
#include "vector/kvs_vector_core.h"
#include "vector/kvs_vector_heap.h"
#include "vector/kvs_vector_id_slot_map.h"

/*
 * KVLite core/hnsw index skeleton.
 *
 * This is a vector_core implementation boundary.
 *
 * It only owns:
 *   record_id + vector + graph/search buffers
 *
 * It must not depend on:
 *   question / answer / scope / TTL / metadata / manager store
 *
 * Manager remains responsible for record lookup, business filtering, and
 * response assembly. This stage intentionally does not implement add/search.
 */

#define KVS_HNSW_DEFAULT_M 16
#define KVS_HNSW_DEFAULT_M0 32
#define KVS_HNSW_DEFAULT_EF_CONSTRUCTION 128
#define KVS_HNSW_DEFAULT_EF_SEARCH 128
#define KVS_HNSW_DEFAULT_CONTEXT_COUNT 2
#define KVS_HNSW_MAX_LEVEL 32

typedef float (*kvs_hnsw_distance_fn)(
	const float *lhs,
	const float *rhs,
	size_t dim
);

typedef struct kvs_hnsw_random_s {
	uint64_t state;
} kvs_hnsw_random_t;

typedef struct kvs_hnsw_search_context_s {
	kvs_vector_heap_t candidate_heap;
	kvs_vector_heap_t top_heap;
	uint32_t *neighbor_buffer;
	float *distance_buffer;
	kvs_vector_hit_t *hits;
	size_t capacity;
	int in_use;
} kvs_hnsw_search_context_t;

typedef struct kvs_hnsw_search_context_pool_s {
	kvs_hnsw_search_context_t *contexts;
	size_t count;
	size_t context_capacity;
} kvs_hnsw_search_context_pool_t;

typedef struct kvs_hnsw_rebuild_profile_s {
	uint64_t rebuild_total_us;
	uint64_t rebuild_manager_scan_us;
	uint64_t rebuild_core_reset_us;
	uint64_t rebuild_core_reserve_us;
	uint64_t rebuild_core_add_total_us;
	uint64_t rebuild_core_add_max_us;
	uint64_t rebuild_core_add_count;
	uint64_t hnsw_add_random_level_us;
	uint64_t hnsw_add_greedy_search_us;
	uint64_t hnsw_add_layer_search_us;
	uint64_t hnsw_add_neighbor_select_us;
	uint64_t hnsw_add_link_us;
	uint64_t hnsw_add_vector_copy_us;

	/* Stage 7A: low-invasiveness event counters for memory access pattern audit.
	 * These count per-add hot-loop events across all layers; zero runtime
	 * overhead beyond a few integer increments.  They are audit-only and do
	 * not change algorithm semantics, graph structure, or memory layout. */
	uint64_t add_distance_compute_count;
	uint64_t add_neighbor_visit_count;
	uint64_t add_visited_skip_count;
	uint64_t add_active_check_count;
	uint64_t add_top_heap_push_count;
	uint64_t add_top_heap_replace_count;
	uint64_t add_candidate_push_count;
	uint64_t add_level0_neighbor_scan_count;
	uint64_t add_upper_neighbor_scan_count;

	/* Stage 7F: layer_search attribution counters. */
	uint64_t layer_search_call_count;
	uint64_t layer_search_level0_call_count;
	uint64_t layer_search_upper_call_count;
	uint64_t layer_search_level0_us;
	uint64_t layer_search_upper_us;
	uint64_t layer_search_distance_compute_count;
	uint64_t layer_search_visited_skip_count;
	uint64_t layer_search_neighbor_visit_count;
	uint64_t layer_search_level0_neighbor_scan_count;
	uint64_t layer_search_upper_neighbor_scan_count;
	uint64_t layer_search_candidate_heap_push_count;
	uint64_t layer_search_candidate_heap_pop_count;
	uint64_t layer_search_candidate_heap_replace_count;
	uint64_t layer_search_top_heap_push_count;
	uint64_t layer_search_top_heap_replace_count;
	uint64_t layer_search_lower_bound_update_count;
	uint64_t layer_search_deleted_active_check_count;

	/* Stage 7H: level0 neighbor access / visited_tags layout audit. */
	uint64_t layer_search_unique_visit_count;
	uint64_t layer_search_repeated_visit_count;
	uint64_t layer_search_visited_tag_access_count;
	uint64_t layer_search_slot_state_touch_count;
	uint64_t layer_search_level0_unique_visit_count;
	uint64_t layer_search_level0_repeated_visit_count;
	uint64_t layer_search_level0_visited_tag_access_count;
	uint64_t layer_search_level0_slot_state_touch_count;
	uint64_t layer_search_level0_neighbor_row_access_count;
	uint64_t layer_search_level0_neighbor_row_sequential_count;
	uint64_t layer_search_level0_neighbor_row_random_count;
	uint64_t layer_search_level0_neighbor_slot_sequential_count;
	uint64_t layer_search_level0_neighbor_slot_random_count;

	/* Stage 7L: post-layout remaining layer_search hotspot audit. */
	uint64_t layer_search_level0_candidate_row_expansion_count;
	uint64_t layer_search_level0_distance_compute_count;
	uint64_t layer_search_upper_distance_compute_count;
	uint64_t layer_search_level0_deleted_active_check_count;
	uint64_t layer_search_upper_deleted_active_check_count;
	uint64_t layer_search_candidate_heap_skip_count;
	uint64_t layer_search_candidate_heap_full_skip_count;
	uint64_t layer_search_top_heap_skip_count;
	uint64_t layer_search_level0_candidate_heap_push_count;
	uint64_t layer_search_level0_candidate_heap_pop_count;
	uint64_t layer_search_level0_candidate_heap_skip_count;
	uint64_t layer_search_level0_top_heap_push_count;
	uint64_t layer_search_level0_top_heap_replace_count;
	uint64_t layer_search_level0_top_heap_skip_count;
	uint64_t layer_search_level0_lower_bound_update_count;

	/* Stage 7D: neighbor_select attribution counters. */
	uint64_t neighbor_select_call_count;
	uint64_t neighbor_select_level0_call_count;
	uint64_t neighbor_select_upper_call_count;
	uint64_t neighbor_select_candidate_total;
	uint64_t neighbor_select_candidate_max;
	uint64_t neighbor_select_selected_total;
	uint64_t neighbor_select_selected_max;
	uint64_t neighbor_select_reject_count;
	uint64_t neighbor_select_heuristic_prune_count;
	uint64_t neighbor_select_distance_recompute_count;
	uint64_t neighbor_select_reused_candidate_score_count;
	uint64_t neighbor_select_duplicate_scan_count;
	uint64_t neighbor_select_worst_scan_count;
	uint64_t neighbor_select_worst_cache_hit_count;
	uint64_t neighbor_select_level0_candidate_total;
	uint64_t neighbor_select_upper_candidate_total;
	uint64_t neighbor_select_level0_selected_total;
	uint64_t neighbor_select_upper_selected_total;
	uint64_t neighbor_select_level0_reject_count;
	uint64_t neighbor_select_upper_reject_count;
	uint64_t neighbor_select_level0_heuristic_prune_count;
	uint64_t neighbor_select_upper_heuristic_prune_count;
	uint64_t neighbor_select_level0_distance_recompute_count;
	uint64_t neighbor_select_upper_distance_recompute_count;
	uint64_t neighbor_select_level0_reused_candidate_score_count;
	uint64_t neighbor_select_upper_reused_candidate_score_count;
	uint64_t neighbor_select_level0_duplicate_scan_count;
	uint64_t neighbor_select_upper_duplicate_scan_count;
	uint64_t neighbor_select_level0_worst_scan_count;
	uint64_t neighbor_select_upper_worst_scan_count;
	uint64_t neighbor_select_level0_worst_cache_hit_count;
	uint64_t neighbor_select_upper_worst_cache_hit_count;

	/* Stage 9A1: bidirectional link acceptance audit counters. */
	uint64_t bidirectional_selected_neighbor_count;
	uint64_t bidirectional_forward_edge_attempt_count;
	uint64_t bidirectional_forward_edge_added_count;
	uint64_t bidirectional_reverse_edge_attempt_count;
	uint64_t bidirectional_reverse_edge_accepted_count;
	uint64_t bidirectional_reverse_edge_rejected_count;
	uint64_t bidirectional_forward_edge_skipped_due_to_reverse_reject_count;
} kvs_hnsw_rebuild_profile_t;

typedef struct kvs_hnsw_index_s {
	size_t dim;
	int metric;

	size_t M;
	size_t M0;
	size_t ef_construction;
	size_t ef_search;

	size_t capacity;
	size_t size;

	float *vectors;
	uint8_t *levels;
	uint8_t *deleted_flags;
	uint64_t *slot_to_record_id;
	kvs_vector_id_slot_map_t record_id_to_slot;

	uint32_t *level0_neighbors;
	float *level0_neighbor_scores;
	uint16_t *level0_counts;
	uint16_t *level0_worst_pos;
	float *level0_worst_score;
	uint8_t *level0_worst_valid;

	size_t *upper_offsets;
	uint32_t *upper_neighbors;
	uint16_t *upper_counts;

	uint32_t entry_point;
	int has_entry;
	uint8_t max_level;

	uint32_t *visited_tags;
	uint32_t current_visit_tag;

	kvs_hnsw_random_t rng;
	kvs_hnsw_search_context_pool_t search_context_pool;
	kvs_hnsw_distance_fn distance;

	int ready;
	int needs_rebuild;
	size_t deleted_count;
	kvs_hnsw_rebuild_profile_t rebuild_profile;
} kvs_hnsw_index_t;

int kvs_hnsw_index_init(
	kvs_hnsw_index_t *index,
	size_t dim,
	int metric
);

int kvs_hnsw_index_reserve(
	kvs_hnsw_index_t *index,
	size_t capacity
);

int kvs_hnsw_index_reset(
	kvs_hnsw_index_t *index
);

int kvs_hnsw_index_info(
	kvs_hnsw_index_t *index,
	kvs_vector_core_info_t *out
);

int kvs_hnsw_index_add(
	kvs_hnsw_index_t *index,
	uint64_t record_id,
	const float *vector,
	size_t dim
);

int kvs_hnsw_index_search(
	kvs_hnsw_index_t *index,
	const float *query,
	size_t dim,
	size_t topk,
	kvs_vector_hit_t *out,
	size_t *out_count
);

int kvs_hnsw_index_mark_delete(
	kvs_hnsw_index_t *index,
	uint64_t record_id
);

void kvs_hnsw_index_destroy(
	kvs_hnsw_index_t *index
);

int kvs_hnsw_context_pool_init(
	kvs_hnsw_search_context_pool_t *pool,
	size_t count,
	size_t context_capacity
);

void kvs_hnsw_context_pool_destroy(
	kvs_hnsw_search_context_pool_t *pool
);

kvs_hnsw_search_context_t *kvs_hnsw_context_acquire(
	kvs_hnsw_search_context_pool_t *pool
);

void kvs_hnsw_context_release(
	kvs_hnsw_search_context_pool_t *pool,
	kvs_hnsw_search_context_t *ctx
);

void kvs_hnsw_context_reset(
	kvs_hnsw_search_context_t *ctx
);

uint32_t kvs_hnsw_next_visit_tag(
	kvs_hnsw_index_t *index
);

size_t kvs_hnsw_graph_level0_bytes(
	size_t capacity,
	size_t m0
);

void kvs_hnsw_random_init(
	kvs_hnsw_random_t *rng,
	uint64_t seed
);

uint32_t kvs_hnsw_random_next_u32(
	kvs_hnsw_random_t *rng
);

float kvs_hnsw_random_next_unit(
	kvs_hnsw_random_t *rng
);

uint8_t kvs_hnsw_random_level(
	kvs_hnsw_random_t *rng,
	size_t M
);

uint8_t kvs_hnsw_index_next_level(
	kvs_hnsw_index_t *index
);

uint64_t kvs_hnsw_profile_now_us(void);

void kvs_hnsw_rebuild_profile_reset(
	kvs_hnsw_index_t *index
);

void kvs_hnsw_rebuild_profile_record_add(
	kvs_hnsw_index_t *index,
	uint64_t elapsed_us
);
