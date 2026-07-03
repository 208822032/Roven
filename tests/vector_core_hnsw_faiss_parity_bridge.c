#include "vector_core_hnsw_faiss_parity_bridge.h"

#include "kvs_config.h"
#include "kvs_memory.h"
#include "kvs_memory_internal.h"
#include "vector/kvs_vector_flat.h"
#include "vector/kvs_vector_hnsw_v2.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/time.h>

extern kvs_config_t g_cfg;

static uint64_t p1b_bridge_now_us(void) {
	struct timeval tv;

	gettimeofday(&tv, NULL);
	return (uint64_t)tv.tv_sec * 1000000ULL + (uint64_t)tv.tv_usec;
}

static int p1b_bridge_hits_descending(const kvs_vector_hit_t *hits, size_t count) {
	size_t i;

	for (i = 1; i < count; i++) {
		if (hits[i - 1].score < hits[i].score) {
			return 0;
		}
	}
	return 1;
}

static int p1b_bridge_exact_same_ids(
	const uint64_t *exact_ids,
	size_t exact_count,
	const kvs_vector_hit_t *hits,
	size_t hit_count
) {
	size_t i;

	if (exact_count != hit_count) {
		return 0;
	}
	for (i = 0; i < exact_count; i++) {
		if (exact_ids[i] != hits[i].record_id) {
			return 0;
		}
	}
	return 1;
}

static double p1b_bridge_recall_hits(
	const uint64_t *exact_ids,
	size_t exact_count,
	const kvs_vector_hit_t *hits,
	size_t hit_count
) {
	size_t i;
	size_t j;
	size_t matched = 0;

	if (exact_count == 0) {
		return 1.0;
	}

	for (i = 0; i < exact_count; i++) {
		for (j = 0; j < hit_count; j++) {
			if (hits[j].record_id == exact_ids[i]) {
				matched++;
				break;
			}
		}
	}
	return (double)matched / (double)exact_count;
}

static int p1b_bridge_cmp_u64(const void *lhs, const void *rhs) {
	uint64_t a = *(const uint64_t *)lhs;
	uint64_t b = *(const uint64_t *)rhs;

	return (a > b) - (a < b);
}

static uint64_t p1b_bridge_percentile_u64(const uint64_t *values, size_t count, double pct) {
	uint64_t *copy;
	size_t idx;
	uint64_t value;

	if (!values || count == 0) {
		return 0;
	}
	copy = (uint64_t *)malloc(count * sizeof(*copy));
	if (!copy) {
		return 0;
	}
	memcpy(copy, values, count * sizeof(*copy));
	qsort(copy, count, sizeof(*copy), p1b_bridge_cmp_u64);
	idx = (size_t)((pct / 100.0) * (double)count);
	if (idx >= count) {
		idx = count - 1;
	}
	value = copy[idx];
	free(copy);
	return value;
}

static double p1b_bridge_avg_u64(const uint64_t *values, size_t count) {
	uint64_t total = 0;
	size_t i;

	if (!values || count == 0) {
		return 0.0;
	}
	for (i = 0; i < count; i++) {
		total += values[i];
	}
	return (double)total / (double)count;
}

int p1b_bridge_init_runtime(void) {
	memset(&g_cfg, 0, sizeof(g_cfg));
	kvs_config_init_default(&g_cfg);
	g_cfg.mem_backend = KVS_MEM_BACKEND_SYSTEM;
	snprintf(g_cfg.vector_distance_backend, sizeof(g_cfg.vector_distance_backend), "%s", "auto");
	mem_set_backend(KVS_MEM_BACKEND_SYSTEM);
	return 0;
}

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
) {
	kvs_flat_index_t flat;
	kvs_vector_hit_t *hits = NULL;
	size_t mismatch = 0;
	size_t i;
	int failed = 0;

	if (!base || !query || !exact_ids || !exact_counts || !mismatch_count) {
		return 1;
	}
	memset(&flat, 0, sizeof(flat));
	hits = (kvs_vector_hit_t *)malloc(topk * sizeof(*hits));
	if (!hits) {
		return 1;
	}

	if (kvs_flat_index_init(&flat, dim, KVS_VECTOR_METRIC_IP) != KVS_VECTOR_OK ||
	    kvs_flat_index_reserve(&flat, count) != KVS_VECTOR_OK) {
		failed = 1;
		goto done;
	}

	for (i = 0; i < count; i++) {
		if (kvs_flat_index_add(
			    &flat,
			    base_record_id + (uint64_t)i,
			    base + i * dim,
			    dim
		    ) != KVS_VECTOR_OK) {
			failed = 1;
			goto done;
		}
	}

	for (i = 0; i < queries; i++) {
		size_t hit_count = 0;
		if (kvs_flat_index_search(
			    &flat,
			    query + i * dim,
			    dim,
			    topk,
			    hits,
			    &hit_count
		    ) != KVS_VECTOR_OK ||
		    !p1b_bridge_hits_descending(hits, hit_count) ||
		    !p1b_bridge_exact_same_ids(
			    exact_ids + i * topk,
			    exact_counts[i],
			    hits,
			    hit_count
		    )) {
			mismatch++;
		}
	}

done:
	kvs_flat_index_destroy(&flat);
	free(hits);
	*mismatch_count = mismatch;
	return failed;
}

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
) {
	kvs_hnsw_v2_index_t index;
	kvs_vector_hit_t *hits = NULL;
	uint64_t *elapsed = NULL;
	uint64_t *visited = NULL;
	uint64_t *distance = NULL;
	uint64_t build_start_us;
	uint64_t build_us = 0;
	size_t i;
	size_t ef_idx;
	int failed = 0;

	if (!base || !query || !exact_ids || !exact_counts || !efs || !graph || !results) {
		return 1;
	}
	memset(&index, 0, sizeof(index));
	memset(graph, 0, sizeof(*graph));
	hits = (kvs_vector_hit_t *)malloc(topk * sizeof(*hits));
	elapsed = (uint64_t *)malloc(queries * sizeof(*elapsed));
	visited = (uint64_t *)malloc(queries * sizeof(*visited));
	distance = (uint64_t *)malloc(queries * sizeof(*distance));
	if (!hits || !elapsed || !visited || !distance) {
		failed = 1;
		goto done;
	}

	if (kvs_hnsw_v2_index_init(&index, dim, KVS_VECTOR_METRIC_IP) != KVS_VECTOR_OK) {
		failed = 1;
		goto done;
	}
	index.M = M;
	index.M0 = M0;
	index.ef_construction = ef_construction;
	index.ef_search = KVS_HNSW_V2_DEFAULT_EF_SEARCH;
	kvs_hnsw_v2_index_set_seed(&index, seed);
	if (kvs_hnsw_v2_index_reserve(&index, count) != KVS_VECTOR_OK) {
		failed = 1;
		goto done;
	}

	build_start_us = p1b_bridge_now_us();
	for (i = 0; i < count; i++) {
		if (kvs_hnsw_v2_index_add(
			    &index,
			    base_record_id + (uint64_t)i,
			    base + i * dim,
			    dim
		    ) != KVS_VECTOR_OK) {
			failed = 1;
			goto done;
		}
	}
	build_us = p1b_bridge_now_us() - build_start_us;
	kvs_hnsw_v2_graph_stats_refresh(&index);
	kvs_hnsw_v2_memory_refresh(&index);

	graph->memory_bytes = index.memory.total_bytes;
	graph->max_level = index.graph_stats.max_level;
	graph->entry_point_level = index.has_entry ? index.levels[index.entry_point] : 0;
	graph->level0_degree_avg = index.graph_stats.level0_degree.avg;
	graph->upper_degree_avg = index.graph_stats.upper_degree.avg;
	graph->isolated_node_count = index.graph_stats.isolated_node_count;
	graph->duplicate_edge_count = index.graph_stats.duplicate_edge_count;
	graph->degree_overflow_count = index.graph_stats.degree_overflow_count;
	graph->selected_neighbor_count = index.graph_stats.selected_neighbor_count;
	graph->reverse_prune_count = index.graph_stats.reverse_prune_count;

	if (index.graph_stats.duplicate_edge_count != 0 ||
	    index.graph_stats.degree_overflow_count != 0) {
		failed = 1;
		goto done;
	}

	for (ef_idx = 0; ef_idx < ef_count; ef_idx++) {
		double recall_sum = 0.0;
		size_t ef = efs[ef_idx];
		size_t q;

		memset(&index.search_stats, 0, sizeof(index.search_stats));
		for (q = 0; q < queries; q++) {
			size_t hit_count = 0;
			uint64_t start_us = p1b_bridge_now_us();
			int ret = kvs_hnsw_v2_index_search(
				&index,
				query + q * dim,
				dim,
				topk,
				ef,
				hits,
				&hit_count
			);
			elapsed[q] = p1b_bridge_now_us() - start_us;
			if (ret != KVS_VECTOR_OK || !p1b_bridge_hits_descending(hits, hit_count)) {
				failed = 1;
				goto done;
			}
			visited[q] = index.search_stats.last_visited_count;
			distance[q] = index.search_stats.last_distance_compute_count;
			recall_sum += p1b_bridge_recall_hits(
				exact_ids + q * topk,
				exact_counts[q],
				hits,
				hit_count
			);
		}

		memset(&results[ef_idx], 0, sizeof(results[ef_idx]));
		results[ef_idx].build_us = build_us;
		results[ef_idx].search_avg_us = p1b_bridge_avg_u64(elapsed, queries);
		results[ef_idx].search_p50_us = p1b_bridge_percentile_u64(elapsed, queries, 50.0);
		results[ef_idx].search_p95_us = p1b_bridge_percentile_u64(elapsed, queries, 95.0);
		results[ef_idx].recall = recall_sum / (double)queries;
		results[ef_idx].memory_bytes = index.memory.total_bytes;
		results[ef_idx].visited_avg = p1b_bridge_avg_u64(visited, queries);
		results[ef_idx].distance_avg = p1b_bridge_avg_u64(distance, queries);
		results[ef_idx].duplicate_edge = index.graph_stats.duplicate_edge_count;
		results[ef_idx].degree_overflow = index.graph_stats.degree_overflow_count;
	}

done:
	kvs_hnsw_v2_index_destroy(&index);
	free(hits);
	free(elapsed);
	free(visited);
	free(distance);
	return failed;
}
