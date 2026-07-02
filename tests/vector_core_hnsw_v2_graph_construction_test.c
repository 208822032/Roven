#include "kvs_config.h"
#include "kvs_memory.h"
#include "kvs_memory_internal.h"
#include "vector/kvs_vector_hnsw_v2.h"

#include <math.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

extern kvs_config_t g_cfg;

static int expect_int(int cond, const char *label) {
	if (!cond) {
		fprintf(stderr, "FAIL: %s\n", label);
		return 1;
	}
	return 0;
}

static void fill_vector(
	float *vector,
	size_t dim,
	uint32_t id
) {
	size_t i;
	float norm = 0.0f;

	for (i = 0; i < dim; i++) {
		uint32_t value = id * 2654435761u + (uint32_t)i * 2246822519u;
		float x = (float)((int)(value % 2001u) - 1000) / 1000.0f;
		vector[i] = x;
		norm += x * x;
	}

	if (norm > 0.0f) {
		float inv = 1.0f / sqrtf(norm);
		for (i = 0; i < dim; i++) {
			vector[i] *= inv;
		}
	}
}

static int check_no_duplicate_edges(
	kvs_hnsw_v2_index_t *index
) {
	size_t slot;
	size_t level;

	for (slot = 0; slot < index->size; slot++) {
		uint8_t node_level = index->levels[slot];
		for (level = 0; level <= node_level; level++) {
			uint32_t *neighbors = kvs_hnsw_v2_layer_neighbors(
				index,
				(uint32_t)slot,
				(uint8_t)level
			);
			uint16_t *count_ptr = kvs_hnsw_v2_layer_count_ptr(
				index,
				(uint32_t)slot,
				(uint8_t)level
			);
			uint16_t i;
			uint16_t j;

			if (!neighbors || !count_ptr) {
				fprintf(stderr, "missing graph row slot=%zu level=%zu\n", slot, level);
				return 1;
			}

			for (i = 0; i < *count_ptr; i++) {
				if (neighbors[i] >= index->size || neighbors[i] == slot) {
					fprintf(
						stderr,
						"bad edge slot=%zu level=%zu neighbor=%u size=%zu\n",
						slot,
						level,
						neighbors[i],
						index->size
					);
					return 1;
				}
				for (j = (uint16_t)(i + 1); j < *count_ptr; j++) {
					if (neighbors[i] == neighbors[j]) {
						fprintf(
							stderr,
							"duplicate edge slot=%zu level=%zu neighbor=%u\n",
							slot,
							level,
							neighbors[i]
						);
						return 1;
					}
				}
			}
		}
	}

	return 0;
}

static int check_degree_limits(
	kvs_hnsw_v2_index_t *index
) {
	size_t slot;
	size_t level;

	for (slot = 0; slot < index->size; slot++) {
		uint8_t node_level = index->levels[slot];
		if (index->level0_counts[slot] > index->M0) {
			fprintf(stderr, "level0 overflow slot=%zu degree=%u\n", slot, index->level0_counts[slot]);
			return 1;
		}
		for (level = 1; level <= node_level; level++) {
			uint16_t *count_ptr = kvs_hnsw_v2_layer_count_ptr(
				index,
				(uint32_t)slot,
				(uint8_t)level
			);
			if (!count_ptr || *count_ptr > index->M) {
				fprintf(stderr, "upper overflow slot=%zu level=%zu\n", slot, level);
				return 1;
			}
		}
	}

	return 0;
}

static int add_range(
	kvs_hnsw_v2_index_t *index,
	uint64_t base_record_id,
	size_t count,
	size_t dim
) {
	float *vector;
	size_t i;

	vector = (float *)malloc(dim * sizeof(float));
	if (!vector) {
		return 1;
	}

	for (i = 0; i < count; i++) {
		fill_vector(vector, dim, (uint32_t)(base_record_id + i));
		if (kvs_hnsw_v2_index_add(
			    index,
			    base_record_id + i,
			    vector,
			    dim
		    ) != KVS_VECTOR_OK) {
			fprintf(stderr, "add failed i=%zu\n", i);
			free(vector);
			return 1;
		}
	}

	free(vector);
	return 0;
}

static int test_small_graph(void) {
	kvs_hnsw_v2_index_t index;
	float vector[8];
	float wrong[7];
	int failed = 0;

	memset(&index, 0, sizeof(index));
	failed |= expect_int(
		kvs_hnsw_v2_index_init(&index, 8, KVS_VECTOR_METRIC_IP) == KVS_VECTOR_OK,
		"small init"
	);
	index.M = 4;
	index.M0 = 8;
	index.ef_construction = 16;
	kvs_hnsw_v2_index_set_seed(&index, 12345);
	failed |= expect_int(kvs_hnsw_v2_index_reserve(&index, 10) == KVS_VECTOR_OK, "small reserve");

	fill_vector(vector, 8, 1);
	failed |= expect_int(kvs_hnsw_v2_index_add(&index, 1001, vector, 8) == KVS_VECTOR_OK, "single add");
	failed |= expect_int(index.size == 1, "single size");
	failed |= expect_int(index.has_entry == 1, "single has entry");
	failed |= expect_int(index.entry_point == 0, "single entry point");
	failed |= expect_int(index.max_level == index.levels[0], "single max level");

	failed |= expect_int(
		kvs_hnsw_v2_index_add(&index, 1001, vector, 8) == KVS_VECTOR_ERR,
		"duplicate reject"
	);
	failed |= expect_int(index.size == 1, "duplicate does not grow");
	failed |= expect_int(
		kvs_hnsw_v2_index_add(&index, 1002, wrong, 7) == KVS_VECTOR_ERR_INVALID_ARG,
		"dim mismatch reject"
	);

	failed |= add_range(&index, 1002, 9, 8);
	failed |= expect_int(index.size == 10, "ten node size");
	fill_vector(vector, 8, 999);
	failed |= expect_int(
		kvs_hnsw_v2_index_add(&index, 2000, vector, 8) == KVS_VECTOR_ERR,
		"capacity full reject"
	);
	failed |= expect_int(check_degree_limits(&index) == 0, "small degree limits");
	failed |= expect_int(check_no_duplicate_edges(&index) == 0, "small no duplicate edges");
	failed |= expect_int(index.graph_stats.forward_edge_added_count > 0, "small forward edges");
	failed |= expect_int(index.graph_stats.reverse_edge_attempt_count > 0, "small reverse attempts");
	failed |= expect_int(index.graph_stats.selected_neighbor_count > 0, "small selected neighbors");

	failed |= expect_int(kvs_hnsw_v2_index_reset(&index) == KVS_VECTOR_OK, "small reset");
	failed |= expect_int(index.capacity == 10, "reset keeps capacity");
	failed |= expect_int(index.size == 0, "reset clears size");
	failed |= expect_int(index.has_entry == 0, "reset clears entry");
	failed |= expect_int(index.level0_counts[0] == 0, "reset clears graph");

	kvs_hnsw_v2_index_destroy(&index);
	return failed;
}

static int test_large_graph(void) {
	kvs_hnsw_v2_index_t index;
	int failed = 0;
	uint64_t level_sum = 0;
	size_t i;

	memset(&index, 0, sizeof(index));
	failed |= expect_int(
		kvs_hnsw_v2_index_init(&index, 64, KVS_VECTOR_METRIC_IP) == KVS_VECTOR_OK,
		"large init"
	);
	index.M = 8;
	index.M0 = 16;
	index.ef_construction = 32;
	kvs_hnsw_v2_index_set_seed(&index, 0xabcdef);
	failed |= expect_int(kvs_hnsw_v2_index_reserve(&index, 1000) == KVS_VECTOR_OK, "large reserve");
	failed |= add_range(&index, 5000, 1000, 64);

	kvs_hnsw_v2_graph_stats_refresh(&index);
	failed |= expect_int(index.size == 1000, "large size");
	failed |= expect_int(index.has_entry == 1, "large has entry");
	failed |= expect_int(index.entry_point < index.size, "large entry point range");
	failed |= expect_int(index.max_level < KVS_HNSW_V2_MAX_LEVEL, "large max level range");
	failed |= expect_int(index.graph_stats.size == 1000, "stats size");
	failed |= expect_int(index.graph_stats.level0_degree.max <= index.M0, "stats level0 max");
	failed |= expect_int(index.graph_stats.upper_degree.max <= index.M, "stats upper max");
	failed |= expect_int(index.graph_stats.duplicate_edge_count == 0, "stats duplicate counter");
	failed |= expect_int(index.graph_stats.degree_overflow_count == 0, "stats overflow counter");
	failed |= expect_int(index.graph_stats.forward_edge_added_count > 0, "large forward edges");
	failed |= expect_int(index.graph_stats.reverse_edge_attempt_count > 0, "large reverse attempts");
	failed |= expect_int(index.graph_stats.reverse_edge_accepted_count > 0, "large reverse accepted");
	failed |= expect_int(index.graph_stats.neighbor_select_call_count > 0, "neighbor select calls");
	failed |= expect_int(check_degree_limits(&index) == 0, "large degree limits");
	failed |= expect_int(check_no_duplicate_edges(&index) == 0, "large no duplicate edges");

	for (i = 0; i < KVS_HNSW_V2_MAX_LEVEL; i++) {
		level_sum += index.graph_stats.level_distribution[i];
	}
	failed |= expect_int(level_sum == index.size, "level distribution sums to size");
	failed |= expect_int(index.graph_stats.isolated_node_count == 0, "large no isolated level0 nodes");

	printf(
		"hnsw_v2_graph_stats size=%zu max_level=%u entry_point=%u "
		"level0_degree=min:%u avg:%.2f p50:%u p95:%u max:%u "
		"upper_degree=min:%u avg:%.2f p50:%u p95:%u max:%u "
		"selected=%llu forward=%llu reverse_attempt=%llu reverse_accepted=%llu "
		"reverse_rejected=%llu reverse_prune=%llu duplicate=%llu overflow=%llu "
		"isolated=%llu\n",
		index.graph_stats.size,
		(unsigned)index.graph_stats.max_level,
		index.graph_stats.entry_point,
		index.graph_stats.level0_degree.min,
		index.graph_stats.level0_degree.avg,
		index.graph_stats.level0_degree.p50,
		index.graph_stats.level0_degree.p95,
		index.graph_stats.level0_degree.max,
		index.graph_stats.upper_degree.min,
		index.graph_stats.upper_degree.avg,
		index.graph_stats.upper_degree.p50,
		index.graph_stats.upper_degree.p95,
		index.graph_stats.upper_degree.max,
		(unsigned long long)index.graph_stats.selected_neighbor_count,
		(unsigned long long)index.graph_stats.forward_edge_added_count,
		(unsigned long long)index.graph_stats.reverse_edge_attempt_count,
		(unsigned long long)index.graph_stats.reverse_edge_accepted_count,
		(unsigned long long)index.graph_stats.reverse_edge_rejected_count,
		(unsigned long long)index.graph_stats.reverse_prune_count,
		(unsigned long long)index.graph_stats.duplicate_edge_count,
		(unsigned long long)index.graph_stats.degree_overflow_count,
		(unsigned long long)index.graph_stats.isolated_node_count
	);

	kvs_hnsw_v2_index_destroy(&index);
	return failed;
}

int main(void) {
	int failed = 0;

	memset(&g_cfg, 0, sizeof(g_cfg));
	kvs_config_init_default(&g_cfg);
	g_cfg.mem_backend = KVS_MEM_BACKEND_SYSTEM;
	snprintf(g_cfg.vector_distance_backend, sizeof(g_cfg.vector_distance_backend), "%s", "fallback");
	mem_set_backend(KVS_MEM_BACKEND_SYSTEM);

	failed |= test_small_graph();
	failed |= test_large_graph();

	if (failed) {
		return 1;
	}

	printf("vector_core_hnsw_v2_graph_construction_test PASS\n");
	return 0;
}
