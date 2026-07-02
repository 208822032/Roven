#include "kvs_config.h"
#include "kvs_memory.h"
#include "kvs_memory_internal.h"
#include "vector/kvs_vector_hnsw_v2.h"

#include <math.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/time.h>

extern kvs_config_t g_cfg;

typedef struct exact_item_s {
	uint64_t record_id;
	float score;
} exact_item_t;

typedef struct search_profile_s {
	uint64_t search_us;
	double recall;
} search_profile_t;

static uint64_t now_us(void) {
	struct timeval tv;

	gettimeofday(&tv, NULL);
	return (uint64_t)tv.tv_sec * 1000000ULL + (uint64_t)tv.tv_usec;
}

static uint32_t rng_next(uint64_t *state) {
	uint64_t z;

	*state += 0x9e3779b97f4a7c15ULL;
	z = *state;
	z = (z ^ (z >> 30)) * 0xbf58476d1ce4e5b9ULL;
	z = (z ^ (z >> 27)) * 0x94d049bb133111ebULL;
	z ^= z >> 31;
	return (uint32_t)(z >> 32);
}

static void fill_random_unit_vector(
	float *vector,
	size_t dim,
	uint64_t seed
) {
	uint64_t state = seed;
	size_t i;
	float norm = 0.0f;

	for (i = 0; i < dim; i++) {
		uint32_t raw = rng_next(&state);
		float x = (float)((int)(raw % 20001u) - 10000) / 10000.0f;
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

static float exact_score_ip(
	const float *lhs,
	const float *rhs,
	size_t dim
) {
	size_t i;
	float sum = 0.0f;

	for (i = 0; i < dim; i++) {
		sum += lhs[i] * rhs[i];
	}
	return sum;
}

static void exact_topk_ip(
	const float *base,
	size_t count,
	size_t dim,
	const uint8_t *deleted,
	const float *query,
	size_t topk,
	exact_item_t *out,
	size_t *out_count
) {
	size_t i;
	size_t n = 0;

	*out_count = 0;
	for (i = 0; i < count; i++) {
		size_t pos;
		float score;
		exact_item_t item;

		if (deleted && deleted[i]) {
			continue;
		}
		score = exact_score_ip(query, base + i * dim, dim);
		if (n == topk && score <= out[n - 1].score) {
			continue;
		}

		item.record_id = 1000000ULL + i;
		item.score = score;
		if (n < topk) {
			n++;
		}
		pos = n - 1;
		while (pos > 0 && out[pos - 1].score < score) {
			out[pos] = out[pos - 1];
			pos--;
		}
		out[pos] = item;
	}

	*out_count = n;
}

static double recall_at_k(
	const exact_item_t *exact,
	size_t exact_count,
	const kvs_vector_hit_t *ann,
	size_t ann_count
) {
	size_t i;
	size_t j;
	size_t hit = 0;

	if (exact_count == 0) {
		return 1.0;
	}
	for (i = 0; i < exact_count; i++) {
		for (j = 0; j < ann_count; j++) {
			if (exact[i].record_id == ann[j].record_id) {
				hit++;
				break;
			}
		}
	}
	return (double)hit / (double)exact_count;
}

static int hits_descending(
	const kvs_vector_hit_t *hits,
	size_t count
) {
	size_t i;

	for (i = 1; i < count; i++) {
		if (hits[i - 1].score < hits[i].score) {
			return 0;
		}
	}
	return 1;
}

static int cmp_u64(const void *lhs, const void *rhs) {
	uint64_t a = *(const uint64_t *)lhs;
	uint64_t b = *(const uint64_t *)rhs;

	return (a > b) - (a < b);
}

static uint64_t percentile_u64(
	uint64_t *values,
	size_t count,
	double pct
) {
	size_t idx;

	if (!values || count == 0) {
		return 0;
	}
	qsort(values, count, sizeof(uint64_t), cmp_u64);
	idx = (size_t)((pct / 100.0) * (double)count);
	if (idx >= count) {
		idx = count - 1;
	}
	return values[idx];
}

static uint64_t percentile_hist_u64(
	const uint64_t *hist,
	size_t hist_count,
	uint64_t total,
	double pct
) {
	uint64_t target;
	uint64_t seen = 0;
	size_t i;

	if (!hist || hist_count == 0 || total == 0) {
		return 0;
	}
	target = (uint64_t)((pct / 100.0) * (double)total);
	if (target >= total) {
		target = total - 1;
	}
	for (i = 0; i < hist_count; i++) {
		seen += hist[i];
		if (seen > target) {
			return (uint64_t)i;
		}
	}
	return (uint64_t)(hist_count - 1);
}

static void init_pair_diag_table(
	kvs_hnsw_v2_pair_diag_entry_t *table,
	size_t capacity
) {
	size_t i;

	for (i = 0; i < capacity; i++) {
		table[i].key = KVS_HNSW_V2_PAIR_DIAG_EMPTY_KEY;
		table[i].count = 0;
	}
}

static int run_search_profile(
	kvs_hnsw_v2_index_t *index,
	const exact_item_t *exact,
	const size_t *exact_counts,
	const float *queries_data,
	size_t dim,
	size_t topk,
	size_t queries,
	size_t ef_search,
	search_profile_t *out
) {
	kvs_vector_hit_t *hits;
	uint64_t *times;
	double recall_sum = 0.0;
	uint64_t search_sum = 0;
	size_t q;
	int failed = 0;

	hits = (kvs_vector_hit_t *)malloc(topk * sizeof(*hits));
	times = (uint64_t *)malloc(queries * sizeof(*times));
	if (!hits || !times) {
		free(hits);
		free(times);
		return 1;
	}

	for (q = 0; q < queries; q++) {
		size_t out_count = 0;
		uint64_t start_us = now_us();
		int ret = kvs_hnsw_v2_index_search(
			index,
			queries_data + q * dim,
			dim,
			topk,
			ef_search,
			hits,
			&out_count);
		uint64_t elapsed = now_us() - start_us;

		if (ret != KVS_VECTOR_OK || !hits_descending(hits, out_count)) {
			failed = 1;
			break;
		}
		times[q] = elapsed;
		search_sum += elapsed;
		recall_sum += recall_at_k(
			exact + q * topk,
			exact_counts[q],
			hits,
			out_count);
	}

	if (!failed) {
		out->search_us = search_sum / queries;
		out->recall = recall_sum / (double)queries;
		printf(
			"hnsw_v2_reverse_prune_profile_result ef=%zu "
			"search_avg_us=%.2f search_p50_us=%llu search_p95_us=%llu "
			"recall_at_10=%.4f visited_avg=%.2f distance_avg=%.2f "
			"level0_scan_avg=%.2f\n",
			ef_search,
			(double)search_sum / (double)queries,
			(unsigned long long)percentile_u64(times, queries, 50.0),
			(unsigned long long)percentile_u64(times, queries, 95.0),
			out->recall,
			index->search_stats.search_query_count ?
				(double)index->search_stats.search_visited_count_total /
				(double)index->search_stats.search_query_count : 0.0,
			index->search_stats.search_query_count ?
				(double)index->search_stats.search_distance_compute_count_total /
				(double)index->search_stats.search_query_count : 0.0,
			index->search_stats.search_query_count ?
				(double)index->search_stats.search_level0_neighbor_scan_count /
				(double)index->search_stats.search_query_count : 0.0);
	}

	free(hits);
	free(times);
	return failed;
}

static size_t pair_table_capacity_for_count(size_t count) {
	if (count <= 1000) {
		return (size_t)1 << 20;
	}
	if (count <= 5000) {
		return (size_t)1 << 22;
	}
	return (size_t)1 << 23;
}

static size_t sample_interval_for_count(size_t count) {
	if (count <= 1000) {
		return 1;
	}
	if (count <= 5000) {
		return 32;
	}
	return 128;
}

static int run_count_profile(
	size_t count,
	size_t dim,
	size_t topk,
	size_t queries
) {
	kvs_hnsw_v2_index_t index;
	float *base = NULL;
	float *queries_data = NULL;
	exact_item_t *exact = NULL;
	size_t *exact_counts = NULL;
	kvs_hnsw_v2_pair_diag_entry_t *pair_table = NULL;
	size_t pair_table_capacity = pair_table_capacity_for_count(count);
	size_t sample_interval = sample_interval_for_count(count);
	uint64_t build_start_us;
	uint64_t build_us;
	size_t i;
	search_profile_t ef256;
	search_profile_t ef512;
	uint64_t reverse_score_total;
	uint64_t estimated_repeated;
	uint64_t estimated_cacheable_direct;
	uint64_t estimated_cacheable_neighbor_select;
	uint64_t estimated_reusable;
	uint64_t non_cacheable;
	double estimated_reusable_ratio;
	double reverse_candidate_avg;
	double reverse_degree_avg;
	int failed = 0;

	memset(&index, 0, sizeof(index));
	memset(&ef256, 0, sizeof(ef256));
	memset(&ef512, 0, sizeof(ef512));

	base = (float *)malloc(count * dim * sizeof(float));
	queries_data = (float *)malloc(queries * dim * sizeof(float));
	exact = (exact_item_t *)calloc(queries * topk, sizeof(*exact));
	exact_counts = (size_t *)calloc(queries, sizeof(*exact_counts));
	pair_table = (kvs_hnsw_v2_pair_diag_entry_t *)malloc(
		pair_table_capacity * sizeof(*pair_table));
	if (!base || !queries_data || !exact || !exact_counts || !pair_table) {
		fprintf(stderr, "reverse prune profile alloc failed count=%zu\n", count);
		failed = 1;
		goto done;
	}
	init_pair_diag_table(pair_table, pair_table_capacity);

	if (kvs_hnsw_v2_index_init(&index, dim, KVS_VECTOR_METRIC_IP) != KVS_VECTOR_OK) {
		fprintf(stderr, "reverse prune profile init failed count=%zu\n", count);
		failed = 1;
		goto done;
	}
	index.M = KVS_HNSW_V2_DEFAULT_M;
	index.M0 = KVS_HNSW_V2_DEFAULT_M0;
	index.ef_construction = KVS_HNSW_V2_DEFAULT_EF_CONSTRUCTION;
	index.ef_search = KVS_HNSW_V2_DEFAULT_EF_SEARCH;
	kvs_hnsw_v2_index_set_seed(&index, 0x9d000000ULL + count);

	index.reverse_prune_diag.enabled = 1;
	index.reverse_prune_diag.sample_interval = sample_interval;
	index.reverse_prune_diag.pair_table = pair_table;
	index.reverse_prune_diag.pair_table_capacity = pair_table_capacity;

	if (kvs_hnsw_v2_index_reserve(&index, count) != KVS_VECTOR_OK) {
		fprintf(stderr, "reverse prune profile reserve failed count=%zu\n", count);
		failed = 1;
		goto done;
	}

	printf(
		"hnsw_v2_reverse_prune_profile_build_start count=%zu dim=%zu "
		"ef_construction=%zu sample_interval=%zu pair_table_capacity=%zu\n",
		count, dim, index.ef_construction, sample_interval, pair_table_capacity);
	fflush(stdout);

	build_start_us = now_us();
	for (i = 0; i < count; i++) {
		fill_random_unit_vector(base + i * dim, dim, 0xabc00000ULL + i);
		if (kvs_hnsw_v2_index_add(
			    &index,
			    1000000ULL + i,
			    base + i * dim,
			    dim) != KVS_VECTOR_OK) {
			fprintf(stderr, "reverse prune profile add failed count=%zu i=%zu\n",
				count, i);
			failed = 1;
			goto done;
		}
	}
	build_us = now_us() - build_start_us;
	kvs_hnsw_v2_graph_stats_refresh(&index);
	kvs_hnsw_v2_memory_refresh(&index);

	reverse_score_total =
		index.build_stats.reverse_prune_direct_score_count +
		index.build_stats.reverse_prune_neighbor_select_distance_compute_count;
	estimated_repeated =
		index.reverse_prune_diag.sampled_repeated_pair_score_count *
		sample_interval;
	estimated_cacheable_direct =
		index.build_stats.reverse_prune_candidate_score_known_recompute_count +
		index.reverse_prune_diag.sampled_direct_existing_repeated_count *
			sample_interval;
	estimated_cacheable_neighbor_select =
		index.reverse_prune_diag.sampled_neighbor_select_pairwise_repeated_count *
		sample_interval;
	estimated_reusable =
		estimated_cacheable_direct + estimated_cacheable_neighbor_select;
	if (estimated_reusable > reverse_score_total) {
		estimated_reusable = reverse_score_total;
	}
	non_cacheable = reverse_score_total > estimated_reusable ?
		reverse_score_total - estimated_reusable : 0;
	estimated_reusable_ratio = reverse_score_total ?
		(double)estimated_reusable / (double)reverse_score_total : 0.0;
	reverse_candidate_avg = index.build_stats.reverse_prune_call_count ?
		(double)index.build_stats.reverse_prune_candidate_count_total /
		(double)index.build_stats.reverse_prune_call_count : 0.0;
	reverse_degree_avg = index.build_stats.reverse_prune_call_count ?
		(double)index.build_stats.reverse_prune_existing_degree_total /
		(double)index.build_stats.reverse_prune_call_count : 0.0;

	printf(
		"hnsw_v2_reverse_prune_profile_build count=%zu dim=%zu "
		"build_us=%llu add_total_us=%llu construction_layer_search_us=%llu "
		"neighbor_select_us=%llu reverse_prune_us=%llu "
		"build_distance_compute_count=%llu "
		"construction_distance_compute_count=%llu "
		"neighbor_select_distance_compute_count=%llu "
		"reverse_prune_call_count=%llu "
		"reverse_prune_direct_score_count=%llu "
		"reverse_prune_existing_score_count=%llu "
		"reverse_prune_new_score_count=%llu "
		"reverse_prune_neighbor_select_call_count=%llu "
		"reverse_prune_neighbor_select_distance_count=%llu "
		"reverse_prune_candidate_avg=%.2f reverse_prune_candidate_p50=%llu "
		"reverse_prune_candidate_p95=%llu "
		"reverse_prune_existing_degree_avg=%.2f "
		"reverse_prune_existing_degree_p50=%llu "
		"reverse_prune_existing_degree_p95=%llu "
		"reverse_prune_rewrite_count=%llu reverse_prune_reject_count=%llu "
		"candidate_score_already_known_recomputed=%llu "
		"known_score_reuse_count=%llu "
		"known_score_reuse_miss_count=%llu "
		"direct_score_saved_count=%llu "
		"pair_cache_lookup_count=%llu "
		"pair_cache_hit_count=%llu "
		"pair_cache_miss_count=%llu "
		"pair_cache_insert_count=%llu "
		"pair_cache_collision_count=%llu "
		"pair_score_saved_count=%llu "
		"old_slot_existing_neighbors_rescored=%llu "
		"new_slot_score_recomputed=%llu "
		"neighbor_select_pairwise_recomputed=%llu "
		"cacheable_direct_score_count=%llu "
		"cacheable_neighbor_select_score_count=%llu "
		"non_cacheable_score_count=%llu "
		"estimated_reusable_distance_count=%llu "
		"estimated_reusable_ratio=%.4f "
		"estimated_build_us_reduction_upper_bound=%.2f "
		"sampled_reverse_prune_call_count=%llu "
		"sampled_pair_observation_count=%llu "
		"sampled_unique_pair_score_count=%llu "
		"sampled_repeated_pair_score_count=%llu "
		"sampled_direct_existing_score_count=%llu "
		"sampled_direct_existing_repeated_count=%llu "
		"sampled_neighbor_select_pairwise_score_count=%llu "
		"sampled_neighbor_select_pairwise_repeated_count=%llu "
		"pair_table_used=%zu pair_table_saturated=%llu "
		"reverse_attempt=%llu reverse_accepted=%llu reverse_rejected=%llu "
		"reverse_prune=%llu duplicate=%llu overflow=%llu "
		"level0_degree_avg=%.2f level0_degree_p50=%u level0_degree_p95=%u "
		"level0_degree_max=%u upper_degree_avg=%.2f upper_degree_p95=%u "
		"memory_bytes=%zu\n",
		count,
		dim,
		(unsigned long long)build_us,
		(unsigned long long)index.build_stats.add_total_us,
		(unsigned long long)index.build_stats.construction_layer_search_us,
		(unsigned long long)index.build_stats.neighbor_select_us,
		(unsigned long long)index.build_stats.reverse_prune_us,
		(unsigned long long)index.build_stats.distance_compute_count,
		(unsigned long long)index.build_stats.construction_distance_compute_count,
		(unsigned long long)index.build_stats.neighbor_select_distance_compute_count,
		(unsigned long long)index.build_stats.reverse_prune_call_count,
		(unsigned long long)index.build_stats.reverse_prune_direct_score_count,
		(unsigned long long)index.build_stats.reverse_prune_existing_score_count,
		(unsigned long long)index.build_stats.reverse_prune_new_score_count,
		(unsigned long long)index.build_stats.reverse_prune_neighbor_select_call_count,
		(unsigned long long)index.build_stats.reverse_prune_neighbor_select_distance_compute_count,
		reverse_candidate_avg,
		(unsigned long long)percentile_hist_u64(
			index.build_stats.reverse_prune_candidate_count_hist,
			KVS_HNSW_V2_REVERSE_PRUNE_HIST_BUCKETS,
			index.build_stats.reverse_prune_call_count,
			50.0),
		(unsigned long long)percentile_hist_u64(
			index.build_stats.reverse_prune_candidate_count_hist,
			KVS_HNSW_V2_REVERSE_PRUNE_HIST_BUCKETS,
			index.build_stats.reverse_prune_call_count,
			95.0),
		reverse_degree_avg,
		(unsigned long long)percentile_hist_u64(
			index.build_stats.reverse_prune_existing_degree_hist,
			KVS_HNSW_V2_REVERSE_PRUNE_HIST_BUCKETS,
			index.build_stats.reverse_prune_call_count,
			50.0),
		(unsigned long long)percentile_hist_u64(
			index.build_stats.reverse_prune_existing_degree_hist,
			KVS_HNSW_V2_REVERSE_PRUNE_HIST_BUCKETS,
			index.build_stats.reverse_prune_call_count,
			95.0),
		(unsigned long long)index.build_stats.reverse_prune_rewrite_count,
		(unsigned long long)index.build_stats.reverse_prune_reject_count,
		(unsigned long long)index.build_stats.reverse_prune_candidate_score_known_recompute_count,
		(unsigned long long)index.build_stats.reverse_prune_known_score_reuse_count,
		(unsigned long long)index.build_stats.reverse_prune_known_score_reuse_miss_count,
		(unsigned long long)index.build_stats.reverse_prune_direct_score_saved_count,
		(unsigned long long)index.build_stats.reverse_prune_pair_cache_lookup_count,
		(unsigned long long)index.build_stats.reverse_prune_pair_cache_hit_count,
		(unsigned long long)index.build_stats.reverse_prune_pair_cache_miss_count,
		(unsigned long long)index.build_stats.reverse_prune_pair_cache_insert_count,
		(unsigned long long)index.build_stats.reverse_prune_pair_cache_collision_count,
		(unsigned long long)index.build_stats.reverse_prune_pair_score_saved_count,
		(unsigned long long)index.build_stats.reverse_prune_existing_score_count,
		(unsigned long long)index.build_stats.reverse_prune_new_score_count,
		(unsigned long long)index.build_stats.reverse_prune_neighbor_select_distance_compute_count,
		(unsigned long long)estimated_cacheable_direct,
		(unsigned long long)estimated_cacheable_neighbor_select,
		(unsigned long long)non_cacheable,
		(unsigned long long)estimated_reusable,
		estimated_reusable_ratio,
		(double)index.build_stats.reverse_prune_us * estimated_reusable_ratio,
		(unsigned long long)index.reverse_prune_diag.sampled_reverse_prune_call_count,
		(unsigned long long)index.reverse_prune_diag.sampled_score_pair_observation_count,
		(unsigned long long)index.reverse_prune_diag.sampled_unique_pair_score_count,
		(unsigned long long)index.reverse_prune_diag.sampled_repeated_pair_score_count,
		(unsigned long long)index.reverse_prune_diag.sampled_direct_existing_score_count,
		(unsigned long long)index.reverse_prune_diag.sampled_direct_existing_repeated_count,
		(unsigned long long)index.reverse_prune_diag.sampled_neighbor_select_pairwise_score_count,
		(unsigned long long)index.reverse_prune_diag.sampled_neighbor_select_pairwise_repeated_count,
		index.reverse_prune_diag.pair_table_used,
		(unsigned long long)index.reverse_prune_diag.pair_table_saturated_count,
		(unsigned long long)index.graph_stats.reverse_edge_attempt_count,
		(unsigned long long)index.graph_stats.reverse_edge_accepted_count,
		(unsigned long long)index.graph_stats.reverse_edge_rejected_count,
		(unsigned long long)index.graph_stats.reverse_prune_count,
		(unsigned long long)index.graph_stats.duplicate_edge_count,
		(unsigned long long)index.graph_stats.degree_overflow_count,
		index.graph_stats.level0_degree.avg,
		index.graph_stats.level0_degree.p50,
		index.graph_stats.level0_degree.p95,
		index.graph_stats.level0_degree.max,
		index.graph_stats.upper_degree.avg,
		index.graph_stats.upper_degree.p95,
		index.memory.total_bytes);

	for (i = 0; i < queries; i++) {
		fill_random_unit_vector(queries_data + i * dim, dim, 0xdad00000ULL + i);
		exact_topk_ip(
			base,
			count,
			dim,
			index.deleted_flags,
			queries_data + i * dim,
			topk,
			exact + i * topk,
			&exact_counts[i]);
	}

	if (run_search_profile(
		    &index, exact, exact_counts, queries_data,
		    dim, topk, queries, 256, &ef256) != 0 ||
	    run_search_profile(
		    &index, exact, exact_counts, queries_data,
		    dim, topk, queries, 512, &ef512) != 0) {
		failed = 1;
		goto done;
	}

	if (count == 1000 && ef256.recall < 0.99) {
		fprintf(stderr, "reverse prune profile 1K ef=256 recall gate failed %.4f\n",
			ef256.recall);
		failed = 1;
	}
	if (count == 10000 && ef256.recall < 0.98) {
		fprintf(stderr, "reverse prune profile 10K ef=256 recall gate failed %.4f\n",
			ef256.recall);
		failed = 1;
	}
	if (count == 10000 && ef512.recall < 0.99) {
		fprintf(stderr, "reverse prune profile 10K ef=512 recall gate failed %.4f\n",
			ef512.recall);
		failed = 1;
	}
	if (ef512.recall + 1e-9 < ef256.recall) {
		fprintf(stderr, "reverse prune profile ef sensitivity regressed count=%zu\n",
			count);
		failed = 1;
	}
	if (index.graph_stats.duplicate_edge_count != 0 ||
	    index.graph_stats.degree_overflow_count != 0) {
		fprintf(stderr, "reverse prune profile graph health failed count=%zu\n",
			count);
		failed = 1;
	}

done:
	kvs_hnsw_v2_index_destroy(&index);
	free(base);
	free(queries_data);
	free(exact);
	free(exact_counts);
	free(pair_table);
	return failed;
}

int main(void) {
	int failed = 0;

	memset(&g_cfg, 0, sizeof(g_cfg));
	kvs_config_init_default(&g_cfg);
	g_cfg.mem_backend = KVS_MEM_BACKEND_SYSTEM;
	snprintf(g_cfg.vector_distance_backend, sizeof(g_cfg.vector_distance_backend), "%s", "auto");
	mem_set_backend(KVS_MEM_BACKEND_SYSTEM);

	failed |= run_count_profile(1000, 512, 10, 50);
	failed |= run_count_profile(5000, 512, 10, 50);
	failed |= run_count_profile(10000, 512, 10, 50);

	if (failed) {
		return 1;
	}

	printf("vector_core_hnsw_v2_reverse_prune_profile_test PASS\n");
	return 0;
}
