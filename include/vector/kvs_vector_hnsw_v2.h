#pragma once

#include <stddef.h>
#include <stdint.h>

#include "kvs_vector.h"
#include "vector/kvs_vector_core.h"
#include "vector/kvs_vector_id_slot_map.h"

#define KVS_HNSW_V2_DEFAULT_M 32
#define KVS_HNSW_V2_DEFAULT_M0 64
#define KVS_HNSW_V2_DEFAULT_EF_CONSTRUCTION 256
#define KVS_HNSW_V2_DEFAULT_EF_SEARCH 256
#define KVS_HNSW_V2_DEFAULT_CONTEXT_COUNT 2
#define KVS_HNSW_V2_VECTOR_ALIGNMENT 64
#define KVS_HNSW_V2_MAX_LEVEL 32
#define KVS_HNSW_V2_REVERSE_PRUNE_HIST_BUCKETS 257
#define KVS_HNSW_V2_PAIR_DIAG_EMPTY_KEY UINT64_MAX

typedef struct kvs_hnsw_v2_random_s {
	uint64_t state;
} kvs_hnsw_v2_random_t;

typedef struct kvs_hnsw_v2_candidate_s kvs_hnsw_v2_candidate_t;
typedef struct kvs_hnsw_v2_search_context_s kvs_hnsw_v2_search_context_t;
typedef struct kvs_hnsw_v2_score_cache_s kvs_hnsw_v2_score_cache_t;

typedef enum kvs_hnsw_v2_reverse_prune_score_kind_e {
	KVS_HNSW_V2_RP_SCORE_DIRECT_EXISTING = 1,
	KVS_HNSW_V2_RP_SCORE_DIRECT_NEW = 2,
	KVS_HNSW_V2_RP_SCORE_NEIGHBOR_SELECT_PAIRWISE = 3
} kvs_hnsw_v2_reverse_prune_score_kind_t;

typedef struct kvs_hnsw_v2_pair_diag_entry_s {
	uint64_t key;
	uint64_t count;
} kvs_hnsw_v2_pair_diag_entry_t;

typedef struct kvs_hnsw_v2_score_hint_s {
	uint32_t from_slot;
	uint32_t to_slot;
	float score;
	int has_score;
} kvs_hnsw_v2_score_hint_t;

typedef struct kvs_hnsw_v2_reverse_prune_ctx_s {
	kvs_hnsw_v2_score_hint_t direct_hint;
	kvs_hnsw_v2_score_cache_t *local_pair_cache;
} kvs_hnsw_v2_reverse_prune_ctx_t;

typedef struct kvs_hnsw_v2_context_pool_s {
	kvs_hnsw_v2_search_context_t *contexts;
	size_t count;
	size_t bytes;
} kvs_hnsw_v2_context_pool_t;

struct kvs_hnsw_v2_search_context_s {
	kvs_hnsw_v2_candidate_t *candidates;
	kvs_hnsw_v2_candidate_t *top;
	kvs_hnsw_v2_candidate_t *scratch;
	size_t candidates_capacity;
	size_t top_capacity;
	size_t scratch_capacity;
};

typedef struct kvs_hnsw_v2_memory_s {
	size_t vectors_bytes;
	size_t slot_to_record_id_bytes;
	size_t record_id_to_slot_bytes;
	size_t levels_bytes;
	size_t deleted_flags_bytes;
	size_t visited_tags_bytes;
	size_t level0_graph_bytes;
	size_t upper_graph_bytes;
	size_t context_bytes;
	size_t total_bytes;
} kvs_hnsw_v2_memory_t;

typedef struct kvs_hnsw_v2_degree_stats_s {
	uint16_t min;
	uint16_t p50;
	uint16_t p95;
	uint16_t max;
	double avg;
} kvs_hnsw_v2_degree_stats_t;

typedef struct kvs_hnsw_v2_graph_stats_s {
	size_t size;
	uint8_t max_level;
	uint32_t entry_point;
	int has_entry;
	uint64_t level_distribution[KVS_HNSW_V2_MAX_LEVEL];
	kvs_hnsw_v2_degree_stats_t level0_degree;
	kvs_hnsw_v2_degree_stats_t upper_degree;
	uint64_t duplicate_edge_count;
	uint64_t degree_overflow_count;
	uint64_t selected_neighbor_count;
	uint64_t forward_edge_added_count;
	uint64_t reverse_edge_attempt_count;
	uint64_t reverse_edge_accepted_count;
	uint64_t reverse_edge_rejected_count;
	uint64_t reverse_prune_count;
	uint64_t neighbor_select_call_count;
	uint64_t neighbor_select_candidate_count;
	uint64_t neighbor_select_selected_count;
	uint64_t neighbor_select_pruned_count;
	uint64_t neighbor_select_duplicate_count;
	uint64_t neighbor_select_rejected_count;
	uint64_t isolated_node_count;
} kvs_hnsw_v2_graph_stats_t;

typedef struct kvs_hnsw_v2_build_stats_s {
	uint64_t add_count;
	uint64_t add_total_us;
	uint64_t random_level_us;
	uint64_t vector_write_us;
	uint64_t id_slot_map_us;
	uint64_t candidate_collect_us;
	uint64_t construction_layer_search_us;
	uint64_t neighbor_select_us;
	uint64_t forward_link_us;
	uint64_t reverse_link_us;
	uint64_t reverse_prune_us;
	uint64_t graph_stats_refresh_us;
	uint64_t memory_refresh_us;
	uint64_t distance_compute_count;
	uint64_t candidate_collect_scan_count;
	uint64_t candidate_collect_kept_count;
	uint64_t construction_candidate_push_count;
	uint64_t construction_candidate_pop_count;
	uint64_t construction_candidate_skip_count;
	uint64_t construction_top_push_count;
	uint64_t construction_top_replace_count;
	uint64_t construction_top_skip_count;
	uint64_t construction_heap_sift_count;
	uint64_t construction_visited_count;
	uint64_t construction_neighbor_scan_count;
	uint64_t construction_distance_compute_count;
	uint64_t construction_upper_distance_compute_count;
	uint64_t construction_level0_distance_compute_count;
	uint64_t neighbor_select_distance_compute_count;
	uint64_t reverse_prune_distance_compute_count;
	uint64_t reverse_prune_neighbor_select_distance_compute_count;
	uint64_t reverse_prune_call_count;
	uint64_t reverse_prune_direct_score_count;
	uint64_t reverse_prune_existing_score_count;
	uint64_t reverse_prune_new_score_count;
	uint64_t reverse_prune_neighbor_select_call_count;
	uint64_t reverse_prune_candidate_count_total;
	uint64_t reverse_prune_existing_degree_total;
	uint64_t reverse_prune_rewrite_count;
	uint64_t reverse_prune_reject_count;
	uint64_t reverse_prune_candidate_score_known_recompute_count;
	uint64_t reverse_prune_known_score_reuse_count;
	uint64_t reverse_prune_known_score_reuse_miss_count;
	uint64_t reverse_prune_direct_score_saved_count;
	uint64_t reverse_prune_pair_cache_lookup_count;
	uint64_t reverse_prune_pair_cache_hit_count;
	uint64_t reverse_prune_pair_cache_miss_count;
	uint64_t reverse_prune_pair_cache_insert_count;
	uint64_t reverse_prune_pair_cache_collision_count;
	uint64_t reverse_prune_pair_score_saved_count;
	uint64_t reverse_prune_candidate_count_hist[KVS_HNSW_V2_REVERSE_PRUNE_HIST_BUCKETS];
	uint64_t reverse_prune_existing_degree_hist[KVS_HNSW_V2_REVERSE_PRUNE_HIST_BUCKETS];
} kvs_hnsw_v2_build_stats_t;

/* Stage 9E3b: reverse-prune attribution is diagnostic-only.  It is
 * disabled by default and only populated by dedicated profile tests. */
typedef struct kvs_hnsw_v2_reverse_prune_diag_s {
	int enabled;
	size_t sample_interval; /* 0 = every reverse prune, N = every Nth */
	uint64_t reverse_prune_seq;
	int sample_active;
	int in_neighbor_select;

	kvs_hnsw_v2_pair_diag_entry_t *pair_table;
	size_t pair_table_capacity;
	size_t pair_table_used;
	uint64_t pair_table_saturated_count;

	uint64_t sampled_reverse_prune_call_count;
	uint64_t sampled_score_pair_observation_count;
	uint64_t sampled_unique_pair_score_count;
	uint64_t sampled_repeated_pair_score_count;
	uint64_t sampled_direct_existing_score_count;
	uint64_t sampled_direct_existing_repeated_count;
	uint64_t sampled_neighbor_select_pairwise_score_count;
	uint64_t sampled_neighbor_select_pairwise_repeated_count;
	uint64_t sampled_direct_new_score_count;
} kvs_hnsw_v2_reverse_prune_diag_t;

/* Stage 9E1a: diagnostic counters for candidate-quality attribution.
 * These are v2-only, diagnostic-mode only, and must not affect normal
 * build/search behavior.  diag.enabled gates all diagnostic measurement. */
typedef struct kvs_hnsw_v2_diag_s {
	int enabled;
	size_t sample_interval; /* 0 = every add; N = every Nth add */
	size_t add_seq;         /* internal counter, 0-based */

	/* 1. bounded layer-search candidates vs exact top candidates */
	uint64_t level0_layer_search_count;
	uint64_t level0_exact_best_visited;    /* exact best was marked visited */
	uint64_t level0_exact_best_in_bounded; /* exact best slot in bounded top */
	double   level0_recall_at_M0_sum;      /* |bounded ∩ exact_M0| / M0 */
	double   level0_recall_at_ef_sum;      /* |bounded ∩ exact_ef| / ef_construction */

	uint64_t upper_layer_search_count;
	uint64_t upper_exact_best_visited;
	uint64_t upper_exact_best_in_bounded;
	double   upper_recall_at_M_sum;
	double   upper_recall_at_ef_sum;

	/* 2. neighbor_select quality */
	uint64_t ns_diag_calls;
	uint64_t ns_bounded_selected_total;
	uint64_t ns_full_selected_total;
	uint64_t ns_overlap_total;          /* |bounded_selected ∩ full_selected| */
	uint64_t ns_pruned_was_in_exact_M;  /* pruned candidate was in exact top-M/M0 */
	uint64_t ns_fallback_count;         /* how many selected came from fallback */

	/* 3. reverse prune quality */
	uint64_t rp_attempts_sampled;
	uint64_t rp_rejected_was_exact_best; /* rejected edge was exact nearest */
	uint64_t rp_pruned_was_exact_best;   /* pruned edge was exact nearest */
	double   rp_new_slot_score_ratio_sum; /* new_slot_score / best_kept_score */

	/* 4. entry-point / upper greedy drift */
	uint64_t upper_greedy_diag_calls;
	double   upper_drift_score_sum;      /* exact_best_score - greedy_final_score */
	uint64_t upper_drift_slot_match;     /* greedy final == exact best slot */
	double   level0_entry_score_ratio_sum; /* entry_score / exact_best_level0_score */
	uint64_t level0_entry_diag_calls;
} kvs_hnsw_v2_diag_t;

struct kvs_hnsw_v2_candidate_s {
	uint32_t slot;
	float score;
};

typedef struct kvs_hnsw_v2_search_query_stats_s {
	uint64_t visited_count;
	uint64_t distance_compute_count;
	uint64_t metric_score_call_count;
	uint64_t upper_distance_compute_count;
	uint64_t level0_distance_compute_count;
	uint64_t candidate_push_count;
	uint64_t candidate_pop_count;
	uint64_t candidate_skip_count;
	uint64_t top_push_count;
	uint64_t top_replace_count;
	uint64_t early_stop_count;
	uint64_t deleted_skip_count;
	uint64_t upper_greedy_step_count;
	uint64_t level0_neighbor_scan_count;
	uint64_t upper_greedy_us;
	uint64_t level0_search_us;
	uint64_t candidate_select_us;
	uint64_t top_maintenance_us;
	uint64_t result_sort_us;
	uint64_t context_acquire_release_us;
	uint64_t elapsed_us;
} kvs_hnsw_v2_search_query_stats_t;

typedef struct kvs_hnsw_v2_search_stats_s {
	uint64_t search_query_count;
	size_t search_ef_search;
	uint64_t search_visited_count_total;
	uint64_t search_distance_compute_count_total;
	uint64_t search_metric_score_call_count_total;
	uint64_t search_upper_distance_compute_count_total;
	uint64_t search_level0_distance_compute_count_total;
	uint64_t search_candidate_push_count;
	uint64_t search_candidate_pop_count;
	uint64_t search_candidate_skip_count;
	uint64_t search_top_push_count;
	uint64_t search_top_replace_count;
	uint64_t search_early_stop_count;
	uint64_t search_deleted_skip_count;
	uint64_t search_upper_greedy_step_count;
	uint64_t search_level0_neighbor_scan_count;
	uint64_t search_upper_greedy_us;
	uint64_t search_level0_search_us;
	uint64_t search_candidate_select_us;
	uint64_t search_top_maintenance_us;
	uint64_t search_result_sort_us;
	uint64_t search_context_acquire_release_us;
	uint64_t search_total_us;
	uint64_t search_avg_us;
	uint64_t last_visited_count;
	uint64_t last_distance_compute_count;
	uint64_t last_metric_score_call_count;
	uint64_t last_upper_distance_compute_count;
	uint64_t last_level0_distance_compute_count;
	uint64_t last_candidate_push_count;
	uint64_t last_candidate_pop_count;
	uint64_t last_candidate_skip_count;
	uint64_t last_top_push_count;
	uint64_t last_top_replace_count;
	uint64_t last_early_stop_count;
	uint64_t last_deleted_skip_count;
	uint64_t last_upper_greedy_step_count;
	uint64_t last_level0_neighbor_scan_count;
	uint64_t last_upper_greedy_us;
	uint64_t last_level0_search_us;
	uint64_t last_candidate_select_us;
	uint64_t last_top_maintenance_us;
	uint64_t last_result_sort_us;
	uint64_t last_context_acquire_release_us;
} kvs_hnsw_v2_search_stats_t;

typedef struct kvs_hnsw_v2_index_s {
	size_t dim;
	int metric;
	size_t M;
	size_t M0;
	size_t ef_construction;
	size_t ef_search;

	size_t capacity;
	size_t size;
	size_t vector_stride;

	float *vectors;
	uint64_t *slot_to_record_id;
	kvs_vector_id_slot_map_t record_id_to_slot;

	uint8_t *levels;
	uint8_t *deleted_flags;
	uint32_t *visited_tags;
	uint32_t current_visit_tag;

	uint32_t entry_point;
	int has_entry;
	uint8_t max_level;

	uint32_t *level0_neighbors;
	uint16_t *level0_counts;
	size_t *upper_offsets;
	uint32_t *upper_neighbors;
	uint16_t *upper_counts;

	kvs_hnsw_v2_random_t rng;
	kvs_hnsw_v2_context_pool_t context_pool;
	kvs_hnsw_v2_memory_t memory;
	kvs_hnsw_v2_graph_stats_t graph_stats;
	kvs_hnsw_v2_build_stats_t build_stats;
	kvs_hnsw_v2_search_stats_t search_stats;
	kvs_hnsw_v2_diag_t diag;
	kvs_hnsw_v2_reverse_prune_diag_t reverse_prune_diag;

	int ready;
	int needs_rebuild;
	size_t deleted_count;
} kvs_hnsw_v2_index_t;

int kvs_hnsw_v2_index_init(
	kvs_hnsw_v2_index_t *index,
	size_t dim,
	int metric
);

int kvs_hnsw_v2_index_reserve(
	kvs_hnsw_v2_index_t *index,
	size_t capacity
);

int kvs_hnsw_v2_index_reset(
	kvs_hnsw_v2_index_t *index
);

int kvs_hnsw_v2_index_info(
	const kvs_hnsw_v2_index_t *index,
	kvs_vector_core_info_t *out
);

int kvs_hnsw_v2_index_add(
	kvs_hnsw_v2_index_t *index,
	uint64_t record_id,
	const float *vector,
	size_t dim
);

int kvs_hnsw_v2_index_mark_delete(
	kvs_hnsw_v2_index_t *index,
	uint64_t record_id
);

int kvs_hnsw_v2_index_search(
	kvs_hnsw_v2_index_t *index,
	const float *query,
	size_t dim,
	size_t topk,
	size_t ef_search,
	kvs_vector_hit_t *hits,
	size_t *out_count
);

void kvs_hnsw_v2_index_destroy(
	kvs_hnsw_v2_index_t *index
);

void kvs_hnsw_v2_random_init(
	kvs_hnsw_v2_random_t *rng,
	uint64_t seed
);

uint32_t kvs_hnsw_v2_random_next_u32(
	kvs_hnsw_v2_random_t *rng
);

float kvs_hnsw_v2_random_next_unit(
	kvs_hnsw_v2_random_t *rng
);

uint8_t kvs_hnsw_v2_random_level(
	kvs_hnsw_v2_random_t *rng,
	size_t M
);

void kvs_hnsw_v2_index_set_seed(
	kvs_hnsw_v2_index_t *index,
	uint64_t seed
);

int kvs_hnsw_v2_context_pool_init(
	kvs_hnsw_v2_context_pool_t *pool,
	size_t count
);

void kvs_hnsw_v2_context_pool_reset(
	kvs_hnsw_v2_context_pool_t *pool
);

void kvs_hnsw_v2_context_pool_destroy(
	kvs_hnsw_v2_context_pool_t *pool
);

void kvs_hnsw_v2_memory_refresh(
	kvs_hnsw_v2_index_t *index
);

void kvs_hnsw_v2_graph_stats_refresh(
	kvs_hnsw_v2_index_t *index
);

const float *kvs_hnsw_v2_slot_vector(
	const kvs_hnsw_v2_index_t *index,
	uint32_t slot
);

uint32_t *kvs_hnsw_v2_layer_neighbors(
	kvs_hnsw_v2_index_t *index,
	uint32_t slot,
	uint8_t level
);

uint16_t *kvs_hnsw_v2_layer_count_ptr(
	kvs_hnsw_v2_index_t *index,
	uint32_t slot,
	uint8_t level
);

size_t kvs_hnsw_v2_layer_capacity(
	const kvs_hnsw_v2_index_t *index,
	uint8_t level
);

int kvs_hnsw_v2_layer_has_edge(
	kvs_hnsw_v2_index_t *index,
	uint32_t from,
	uint32_t to,
	uint8_t level
);

int kvs_hnsw_v2_layer_append_edge(
	kvs_hnsw_v2_index_t *index,
	uint32_t from,
	uint32_t to,
	uint8_t level
);

float kvs_hnsw_v2_metric_score(
	const kvs_hnsw_v2_index_t *index,
	const float *lhs,
	const float *rhs
);

size_t kvs_hnsw_v2_neighbor_select(
	kvs_hnsw_v2_index_t *index,
	const float *query,
	kvs_hnsw_v2_candidate_t *candidates,
	size_t candidate_count,
	uint8_t level,
	kvs_hnsw_v2_candidate_t *selected,
	size_t selected_capacity
);

size_t kvs_hnsw_v2_neighbor_select_with_cache(
	kvs_hnsw_v2_index_t *index,
	const float *query,
	kvs_hnsw_v2_candidate_t *candidates,
	size_t candidate_count,
	uint8_t level,
	kvs_hnsw_v2_candidate_t *selected,
	size_t selected_capacity,
	kvs_hnsw_v2_reverse_prune_ctx_t *reverse_prune_ctx
);

void kvs_hnsw_v2_reverse_prune_diag_record_score_pair(
	kvs_hnsw_v2_index_t *index,
	uint32_t lhs_slot,
	uint32_t rhs_slot,
	kvs_hnsw_v2_reverse_prune_score_kind_t kind
);
