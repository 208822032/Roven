#pragma once

#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef struct p1b_bridge_result_s {
	uint64_t build_us;
	double search_avg_us;
	uint64_t search_p50_us;
	uint64_t search_p95_us;
	double recall;
	size_t memory_bytes;
	double visited_avg;
	double distance_avg;
	uint64_t duplicate_edge;
	uint64_t degree_overflow;
} p1b_bridge_result_t;

typedef struct p1b_bridge_graph_s {
	size_t memory_bytes;
	uint8_t max_level;
	uint8_t entry_point_level;
	double level0_degree_avg;
	double upper_degree_avg;
	uint64_t isolated_node_count;
	uint64_t duplicate_edge_count;
	uint64_t degree_overflow_count;
	uint64_t selected_neighbor_count;
	uint64_t reverse_prune_count;
} p1b_bridge_graph_t;

int p1b_bridge_init_runtime(void);

int p1b_bridge_flat_crosscheck(
	size_t count,
	size_t dim,
	size_t topk,
	uint64_t base_record_id,
	const float *base,
	size_t queries,
	const float *query,
	const uint64_t *exact_ids,
	const size_t *exact_counts,
	size_t *mismatch_count
);

int p1b_bridge_run_v2_profile(
	size_t count,
	size_t dim,
	size_t topk,
	uint64_t base_record_id,
	const float *base,
	size_t queries,
	const float *query,
	const uint64_t *exact_ids,
	const size_t *exact_counts,
	size_t M,
	size_t M0,
	size_t ef_construction,
	const size_t *efs,
	size_t ef_count,
	uint64_t seed,
	p1b_bridge_graph_t *graph,
	p1b_bridge_result_t *results
);

#ifdef __cplusplus
}
#endif
