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

typedef struct query_profile_s {
	uint64_t search_us;
	uint64_t visited;
	uint64_t distance;
	uint64_t level0_scan;
	uint64_t candidate_push;
	uint64_t candidate_pop;
	uint64_t candidate_skip;
	uint64_t top_push;
	uint64_t top_replace;
	uint64_t early_stop;
	uint64_t upper_greedy_us;
	uint64_t level0_search_us;
	uint64_t candidate_select_us;
	uint64_t top_maintenance_us;
	uint64_t result_sort_us;
	uint64_t context_us;
} query_profile_t;

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

static double avg_profile(
	const query_profile_t *profiles,
	size_t count,
	uint64_t (*get_value)(const query_profile_t *)
) {
	uint64_t sum = 0;
	size_t i;

	if (!profiles || !get_value || count == 0) {
		return 0.0;
	}
	for (i = 0; i < count; i++) {
		sum += get_value(&profiles[i]);
	}
	return (double)sum / (double)count;
}

static void copy_metric(
	const query_profile_t *profiles,
	size_t count,
	uint64_t (*get_value)(const query_profile_t *),
	uint64_t *out
) {
	size_t i;

	for (i = 0; i < count; i++) {
		out[i] = get_value(&profiles[i]);
	}
}

static uint64_t get_search_us(const query_profile_t *p) { return p->search_us; }
static uint64_t get_visited(const query_profile_t *p) { return p->visited; }
static uint64_t get_distance(const query_profile_t *p) { return p->distance; }
static uint64_t get_level0_scan(const query_profile_t *p) { return p->level0_scan; }
static uint64_t get_candidate_push(const query_profile_t *p) { return p->candidate_push; }
static uint64_t get_candidate_pop(const query_profile_t *p) { return p->candidate_pop; }
static uint64_t get_candidate_skip(const query_profile_t *p) { return p->candidate_skip; }
static uint64_t get_top_push(const query_profile_t *p) { return p->top_push; }
static uint64_t get_top_replace(const query_profile_t *p) { return p->top_replace; }
static uint64_t get_upper_greedy_us(const query_profile_t *p) { return p->upper_greedy_us; }
static uint64_t get_level0_search_us(const query_profile_t *p) { return p->level0_search_us; }
static uint64_t get_candidate_select_us(const query_profile_t *p) { return p->candidate_select_us; }
static uint64_t get_top_maintenance_us(const query_profile_t *p) { return p->top_maintenance_us; }
static uint64_t get_result_sort_us(const query_profile_t *p) { return p->result_sort_us; }
static uint64_t get_context_us(const query_profile_t *p) { return p->context_us; }

static int run_ef_profile(
	kvs_hnsw_v2_index_t *index,
	const float *base,
	const float *queries_data,
	const exact_item_t *exact,
	const size_t *exact_counts,
	size_t count,
	size_t dim,
	size_t topk,
	size_t queries,
	size_t ef_search
) {
	query_profile_t *profiles;
	uint64_t *scratch;
	kvs_vector_hit_t *hits;
	double recall_sum = 0.0;
	size_t q;
	int failed = 0;

	(void)base;
	profiles = (query_profile_t *)calloc(queries, sizeof(*profiles));
	scratch = (uint64_t *)malloc(queries * sizeof(uint64_t));
	hits = (kvs_vector_hit_t *)malloc(topk * sizeof(kvs_vector_hit_t));
	if (!profiles || !scratch || !hits) {
		free(profiles);
		free(scratch);
		free(hits);
		return 1;
	}

	memset(&index->search_stats, 0, sizeof(index->search_stats));
	for (q = 0; q < queries; q++) {
		const float *query = queries_data + q * dim;
		size_t hit_count = 0;
		uint64_t start_us;
		int ret;

		start_us = now_us();
		ret = kvs_hnsw_v2_index_search(
			index,
			query,
			dim,
			topk,
			ef_search,
			hits,
			&hit_count
		);
		profiles[q].search_us = now_us() - start_us;
		if (ret != KVS_VECTOR_OK) {
			fprintf(stderr, "profile search failed count=%zu ef=%zu q=%zu ret=%d\n",
				count, ef_search, q, ret);
			failed = 1;
			break;
		}
		if (!hits_descending(hits, hit_count)) {
			fprintf(stderr, "profile hits not descending count=%zu ef=%zu q=%zu\n",
				count, ef_search, q);
			failed = 1;
			break;
		}

		profiles[q].visited = index->search_stats.last_visited_count;
		profiles[q].distance = index->search_stats.last_distance_compute_count;
		profiles[q].level0_scan =
			index->search_stats.last_level0_neighbor_scan_count;
		profiles[q].candidate_push = index->search_stats.last_candidate_push_count;
		profiles[q].candidate_pop = index->search_stats.last_candidate_pop_count;
		profiles[q].candidate_skip = index->search_stats.last_candidate_skip_count;
		profiles[q].top_push = index->search_stats.last_top_push_count;
		profiles[q].top_replace = index->search_stats.last_top_replace_count;
		profiles[q].early_stop = index->search_stats.last_early_stop_count;
		profiles[q].upper_greedy_us = index->search_stats.last_upper_greedy_us;
		profiles[q].level0_search_us = index->search_stats.last_level0_search_us;
		profiles[q].candidate_select_us =
			index->search_stats.last_candidate_select_us;
		profiles[q].top_maintenance_us =
			index->search_stats.last_top_maintenance_us;
		profiles[q].result_sort_us = index->search_stats.last_result_sort_us;
		profiles[q].context_us =
			index->search_stats.last_context_acquire_release_us;
		recall_sum += recall_at_k(
			exact + q * topk,
			exact_counts[q],
			hits,
			hit_count
		);
	}

	if (!failed) {
		double recall = recall_sum / (double)queries;
		double search_avg = avg_profile(profiles, queries, get_search_us);
		double visited_avg = avg_profile(profiles, queries, get_visited);
		double distance_avg = avg_profile(profiles, queries, get_distance);
		double level0_scan_avg = avg_profile(profiles, queries, get_level0_scan);
		double candidate_push_avg =
			avg_profile(profiles, queries, get_candidate_push);
		double candidate_pop_avg =
			avg_profile(profiles, queries, get_candidate_pop);
		double candidate_skip_avg =
			avg_profile(profiles, queries, get_candidate_skip);
		double top_push_avg = avg_profile(profiles, queries, get_top_push);
		double top_replace_avg = avg_profile(profiles, queries, get_top_replace);
		double upper_greedy_avg =
			avg_profile(profiles, queries, get_upper_greedy_us);
		double level0_us_avg =
			avg_profile(profiles, queries, get_level0_search_us);
		double candidate_select_avg =
			avg_profile(profiles, queries, get_candidate_select_us);
		double top_maintenance_avg =
			avg_profile(profiles, queries, get_top_maintenance_us);
		double result_sort_avg =
			avg_profile(profiles, queries, get_result_sort_us);
		double context_avg = avg_profile(profiles, queries, get_context_us);

		copy_metric(profiles, queries, get_search_us, scratch);
		printf(
			"hnsw_v2_profile_result count=%zu dim=%zu topk=%zu queries=%zu ef=%zu "
			"recall@%zu=%.4f search_avg_us=%.2f search_p50_us=%llu "
			"search_p95_us=%llu search_p99_us=%llu ",
			count,
			dim,
			topk,
			queries,
			ef_search,
			topk,
			recall,
			search_avg,
			(unsigned long long)percentile_u64(scratch, queries, 50.0),
			(unsigned long long)percentile_u64(scratch, queries, 95.0),
			(unsigned long long)percentile_u64(scratch, queries, 99.0)
		);
		copy_metric(profiles, queries, get_visited, scratch);
		printf(
			"visited_avg=%.2f visited_p50=%llu visited_p95=%llu visited_p99=%llu ",
			visited_avg,
			(unsigned long long)percentile_u64(scratch, queries, 50.0),
			(unsigned long long)percentile_u64(scratch, queries, 95.0),
			(unsigned long long)percentile_u64(scratch, queries, 99.0)
		);
		copy_metric(profiles, queries, get_distance, scratch);
		printf(
			"distance_avg=%.2f distance_p50=%llu distance_p95=%llu distance_p99=%llu ",
			distance_avg,
			(unsigned long long)percentile_u64(scratch, queries, 50.0),
			(unsigned long long)percentile_u64(scratch, queries, 95.0),
			(unsigned long long)percentile_u64(scratch, queries, 99.0)
		);
		copy_metric(profiles, queries, get_level0_scan, scratch);
		printf(
			"level0_scan_avg=%.2f level0_scan_p50=%llu level0_scan_p95=%llu "
			"level0_scan_p99=%llu ",
			level0_scan_avg,
			(unsigned long long)percentile_u64(scratch, queries, 50.0),
			(unsigned long long)percentile_u64(scratch, queries, 95.0),
			(unsigned long long)percentile_u64(scratch, queries, 99.0)
		);
		printf(
			"upper_greedy_us_avg=%.2f level0_search_us_avg=%.2f "
			"candidate_select_us_avg=%.2f top_maintenance_us_avg=%.2f "
			"result_sort_us_avg=%.2f context_us_avg=%.2f "
			"candidate_push_avg=%.2f candidate_pop_avg=%.2f "
			"candidate_skip_avg=%.2f top_push_avg=%.2f top_replace_avg=%.2f "
			"early_stop_total=%llu\n",
			upper_greedy_avg,
			level0_us_avg,
			candidate_select_avg,
			top_maintenance_avg,
			result_sort_avg,
			context_avg,
			candidate_push_avg,
			candidate_pop_avg,
			candidate_skip_avg,
			top_push_avg,
			top_replace_avg,
			(unsigned long long)index->search_stats.search_early_stop_count
		);
	}

	free(profiles);
	free(scratch);
	free(hits);
	return failed;
}

static int run_count_profile(
	size_t count,
	size_t dim,
	size_t topk,
	size_t queries
) {
	static const size_t efs[] = {128, 256, 512};
	kvs_hnsw_v2_index_t index;
	float *base;
	float *query_data;
	exact_item_t *exact;
	size_t *exact_counts;
	size_t i;
	int failed = 0;
	uint64_t build_start_us;
	uint64_t build_us;

	memset(&index, 0, sizeof(index));
	base = (float *)malloc(count * dim * sizeof(float));
	query_data = (float *)malloc(queries * dim * sizeof(float));
	exact = (exact_item_t *)calloc(queries * topk, sizeof(*exact));
	exact_counts = (size_t *)calloc(queries, sizeof(*exact_counts));
	if (!base || !query_data || !exact || !exact_counts) {
		free(base);
		free(query_data);
		free(exact);
		free(exact_counts);
		return 1;
	}

	if (kvs_hnsw_v2_index_init(&index, dim, KVS_VECTOR_METRIC_IP) != KVS_VECTOR_OK) {
		fprintf(stderr, "profile init failed count=%zu dim=%zu\n", count, dim);
		failed = 1;
		goto done;
	}
	index.M = KVS_HNSW_V2_DEFAULT_M;
	index.M0 = KVS_HNSW_V2_DEFAULT_M0;
	index.ef_construction = KVS_HNSW_V2_DEFAULT_EF_CONSTRUCTION;
	index.ef_search = KVS_HNSW_V2_DEFAULT_EF_SEARCH;
	kvs_hnsw_v2_index_set_seed(&index, 0x9d000000ULL + count);
	if (kvs_hnsw_v2_index_reserve(&index, count) != KVS_VECTOR_OK) {
		fprintf(stderr, "profile reserve failed count=%zu\n", count);
		failed = 1;
		goto done;
	}

	printf("hnsw_v2_profile_build_start count=%zu dim=%zu\n", count, dim);
	fflush(stdout);
	build_start_us = now_us();
	for (i = 0; i < count; i++) {
		fill_random_unit_vector(base + i * dim, dim, 0xabc00000ULL + i);
		if (kvs_hnsw_v2_index_add(
			    &index,
			    1000000ULL + i,
			    base + i * dim,
			    dim
		    ) != KVS_VECTOR_OK) {
			fprintf(stderr, "profile add failed count=%zu i=%zu\n", count, i);
			failed = 1;
			goto done;
		}
	}
	build_us = now_us() - build_start_us;
	kvs_hnsw_v2_graph_stats_refresh(&index);
	kvs_hnsw_v2_memory_refresh(&index);
	printf(
		"hnsw_v2_profile_build count=%zu dim=%zu build_us=%llu "
		"add_total_us=%llu add_count=%llu random_level_us=%llu "
		"vector_write_us=%llu id_slot_map_us=%llu candidate_collect_us=%llu "
		"construction_layer_search_us=%llu neighbor_select_us=%llu "
		"forward_link_us=%llu reverse_link_us=%llu reverse_prune_us=%llu "
		"graph_stats_refresh_us=%llu memory_refresh_us=%llu "
		"build_distance_compute_count=%llu candidate_collect_scan_count=%llu "
		"candidate_collect_kept_count=%llu construction_candidate_push=%llu "
		"construction_candidate_pop=%llu construction_candidate_skip=%llu "
		"construction_top_push=%llu construction_top_replace=%llu "
		"construction_top_skip=%llu construction_heap_sift=%llu "
		"construction_visited=%llu construction_neighbor_scan_count=%llu "
		"construction_distance_compute_count=%llu "
		"construction_upper_distance_compute_count=%llu "
		"construction_level0_distance_compute_count=%llu "
		"neighbor_select_distance_compute_count=%llu "
		"reverse_prune_distance_compute_count=%llu "
		"reverse_prune_neighbor_select_distance_compute_count=%llu "
		"reverse_prune_call_count=%llu "
		"reverse_prune_direct_score_count=%llu "
		"reverse_prune_existing_score_count=%llu "
		"reverse_prune_new_score_count=%llu "
		"reverse_prune_neighbor_select_call_count=%llu "
		"reverse_prune_candidate_avg=%.2f reverse_prune_candidate_p50=%llu "
		"reverse_prune_candidate_p95=%llu "
		"reverse_prune_existing_degree_avg=%.2f "
		"reverse_prune_existing_degree_p50=%llu "
		"reverse_prune_existing_degree_p95=%llu "
		"reverse_prune_rewrite_count=%llu reverse_prune_reject_count=%llu "
		"reverse_prune_candidate_score_known_recompute_count=%llu "
		"reverse_prune_known_score_reuse_count=%llu "
		"reverse_prune_known_score_reuse_miss_count=%llu "
		"reverse_prune_direct_score_saved_count=%llu "
		"reverse_prune_pair_cache_lookup_count=%llu "
		"reverse_prune_pair_cache_hit_count=%llu "
		"reverse_prune_pair_cache_miss_count=%llu "
		"reverse_prune_pair_cache_insert_count=%llu "
		"reverse_prune_pair_cache_collision_count=%llu "
		"reverse_prune_pair_score_saved_count=%llu "
		"selected=%llu pruned=%llu "
		"reverse_attempt=%llu reverse_accepted=%llu reverse_rejected=%llu "
		"reverse_prune=%llu duplicate=%llu overflow=%llu "
		"level0_degree_avg=%.2f level0_degree_p50=%u level0_degree_p95=%u "
		"level0_degree_max=%u upper_degree_avg=%.2f upper_degree_p95=%u "
		"memory_bytes=%zu\n",
		count,
		dim,
		(unsigned long long)build_us,
		(unsigned long long)index.build_stats.add_total_us,
		(unsigned long long)index.build_stats.add_count,
		(unsigned long long)index.build_stats.random_level_us,
		(unsigned long long)index.build_stats.vector_write_us,
		(unsigned long long)index.build_stats.id_slot_map_us,
		(unsigned long long)index.build_stats.candidate_collect_us,
		(unsigned long long)index.build_stats.construction_layer_search_us,
		(unsigned long long)index.build_stats.neighbor_select_us,
		(unsigned long long)index.build_stats.forward_link_us,
		(unsigned long long)index.build_stats.reverse_link_us,
		(unsigned long long)index.build_stats.reverse_prune_us,
		(unsigned long long)index.build_stats.graph_stats_refresh_us,
		(unsigned long long)index.build_stats.memory_refresh_us,
		(unsigned long long)index.build_stats.distance_compute_count,
		(unsigned long long)index.build_stats.candidate_collect_scan_count,
		(unsigned long long)index.build_stats.candidate_collect_kept_count,
		(unsigned long long)index.build_stats.construction_candidate_push_count,
		(unsigned long long)index.build_stats.construction_candidate_pop_count,
		(unsigned long long)index.build_stats.construction_candidate_skip_count,
		(unsigned long long)index.build_stats.construction_top_push_count,
		(unsigned long long)index.build_stats.construction_top_replace_count,
		(unsigned long long)index.build_stats.construction_top_skip_count,
		(unsigned long long)index.build_stats.construction_heap_sift_count,
		(unsigned long long)index.build_stats.construction_visited_count,
		(unsigned long long)index.build_stats.construction_neighbor_scan_count,
		(unsigned long long)index.build_stats.construction_distance_compute_count,
		(unsigned long long)index.build_stats.construction_upper_distance_compute_count,
		(unsigned long long)index.build_stats.construction_level0_distance_compute_count,
		(unsigned long long)index.build_stats.neighbor_select_distance_compute_count,
		(unsigned long long)index.build_stats.reverse_prune_distance_compute_count,
		(unsigned long long)index.build_stats.reverse_prune_neighbor_select_distance_compute_count,
		(unsigned long long)index.build_stats.reverse_prune_call_count,
		(unsigned long long)index.build_stats.reverse_prune_direct_score_count,
		(unsigned long long)index.build_stats.reverse_prune_existing_score_count,
		(unsigned long long)index.build_stats.reverse_prune_new_score_count,
		(unsigned long long)index.build_stats.reverse_prune_neighbor_select_call_count,
		index.build_stats.reverse_prune_call_count ?
			(double)index.build_stats.reverse_prune_candidate_count_total /
			(double)index.build_stats.reverse_prune_call_count : 0.0,
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
		index.build_stats.reverse_prune_call_count ?
			(double)index.build_stats.reverse_prune_existing_degree_total /
			(double)index.build_stats.reverse_prune_call_count : 0.0,
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
		(unsigned long long)index.graph_stats.selected_neighbor_count,
		(unsigned long long)index.graph_stats.neighbor_select_pruned_count,
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
		index.memory.total_bytes
	);

	for (i = 0; i < queries; i++) {
		fill_random_unit_vector(query_data + i * dim, dim, 0xdad00000ULL + i);
		exact_topk_ip(
			base,
			count,
			dim,
			index.deleted_flags,
			query_data + i * dim,
			topk,
			exact + i * topk,
			&exact_counts[i]
		);
	}

	for (i = 0; i < sizeof(efs) / sizeof(efs[0]); i++) {
		if (run_ef_profile(
			    &index,
			    base,
			    query_data,
			    exact,
			    exact_counts,
			    count,
			    dim,
			    topk,
			    queries,
			    efs[i]
		    ) != 0) {
			failed = 1;
			break;
		}
	}

done:
	kvs_hnsw_v2_index_destroy(&index);
	free(base);
	free(query_data);
	free(exact);
	free(exact_counts);
	return failed;
}

/* ── Stage 9E1a: diagnostic profile for candidate-quality attribution ─ */

static int run_diag_profile(
	size_t count,
	size_t dim,
	size_t topk,
	size_t queries,
	size_t sample_interval)
{
	kvs_hnsw_v2_index_t index;
	float *base;
	float *query_data;
	exact_item_t *exact;
	size_t *exact_counts;
	size_t i;
	size_t q;
	int failed = 0;
	uint64_t build_start_us;
	uint64_t build_us;
	double recall_256 = 0.0;
	double recall_512 = 0.0;
	uint64_t search_avg_256 = 0;
	uint64_t search_avg_512 = 0;

	memset(&index, 0, sizeof(index));
	base = (float *)malloc(count * dim * sizeof(float));
	query_data = (float *)malloc(queries * dim * sizeof(float));
	exact = (exact_item_t *)calloc(queries * topk, sizeof(*exact));
	exact_counts = (size_t *)calloc(queries, sizeof(*exact_counts));
	if (!base || !query_data || !exact || !exact_counts) {
		free(base); free(query_data); free(exact); free(exact_counts);
		return 1;
	}

	if (kvs_hnsw_v2_index_init(&index, dim, KVS_VECTOR_METRIC_IP) != KVS_VECTOR_OK) {
		fprintf(stderr, "diag init failed count=%zu\n", count);
		failed = 1;
		goto done;
	}
	index.M = KVS_HNSW_V2_DEFAULT_M;
	index.M0 = KVS_HNSW_V2_DEFAULT_M0;
	index.ef_construction = KVS_HNSW_V2_DEFAULT_EF_CONSTRUCTION;
	index.ef_search = KVS_HNSW_V2_DEFAULT_EF_SEARCH;
	kvs_hnsw_v2_index_set_seed(&index, 0x9d000000ULL + count);

	/* Enable diagnostic mode */
	index.diag.enabled = 1;
	index.diag.sample_interval = sample_interval;

	if (kvs_hnsw_v2_index_reserve(&index, count) != KVS_VECTOR_OK) {
		fprintf(stderr, "diag reserve failed count=%zu\n", count);
		failed = 1;
		goto done;
	}

	printf("hnsw_v2_diag_build_start count=%zu dim=%zu sample_interval=%zu\n",
		count, dim, sample_interval);
	fflush(stdout);
	build_start_us = now_us();
	for (i = 0; i < count; i++) {
		fill_random_unit_vector(base + i * dim, dim, 0xabc00000ULL + i);
		if (kvs_hnsw_v2_index_add(
			    &index,
			    1000000ULL + i,
			    base + i * dim,
			    dim
		    ) != KVS_VECTOR_OK) {
			fprintf(stderr, "diag add failed count=%zu i=%zu\n", count, i);
			failed = 1;
			goto done;
		}
	}
	build_us = now_us() - build_start_us;
	kvs_hnsw_v2_graph_stats_refresh(&index);
	kvs_hnsw_v2_memory_refresh(&index);

	printf(
		"hnsw_v2_diag_build count=%zu dim=%zu build_us=%llu "
		"add_total_us=%llu construction_layer_search_us=%llu "
		"neighbor_select_us=%llu reverse_prune_us=%llu "
		"build_distance_compute=%llu construction_visited=%llu "
		"construction_neighbor_scan=%llu construction_distance=%llu\n",
		count, dim,
		(unsigned long long)build_us,
		(unsigned long long)index.build_stats.add_total_us,
		(unsigned long long)index.build_stats.construction_layer_search_us,
		(unsigned long long)index.build_stats.neighbor_select_us,
		(unsigned long long)index.build_stats.reverse_prune_us,
		(unsigned long long)index.build_stats.distance_compute_count,
		(unsigned long long)index.build_stats.construction_visited_count,
		(unsigned long long)index.build_stats.construction_neighbor_scan_count,
		(unsigned long long)index.build_stats.construction_distance_compute_count
	);

	/* Diagnostic output: candidate-quality attribution */
	{
		kvs_hnsw_v2_diag_t *d = &index.diag;
		double l0_recall_M0 = d->level0_layer_search_count ?
			d->level0_recall_at_M0_sum / (double)d->level0_layer_search_count : 0.0;
		double l0_recall_ef = d->level0_layer_search_count ?
			d->level0_recall_at_ef_sum / (double)d->level0_layer_search_count : 0.0;
		double up_recall_M = d->upper_layer_search_count ?
			d->upper_recall_at_M_sum / (double)d->upper_layer_search_count : 0.0;
		double up_recall_ef = d->upper_layer_search_count ?
			d->upper_recall_at_ef_sum / (double)d->upper_layer_search_count : 0.0;
		double ns_overlap_ratio = d->ns_diag_calls && d->ns_full_selected_total ?
			(double)d->ns_overlap_total / (double)d->ns_full_selected_total : 0.0;
		double upper_drift_avg = d->upper_greedy_diag_calls ?
			d->upper_drift_score_sum / (double)d->upper_greedy_diag_calls : 0.0;
		double l0_entry_ratio = d->level0_entry_diag_calls ?
			d->level0_entry_score_ratio_sum / (double)d->level0_entry_diag_calls : 0.0;
		double rp_score_ratio = d->rp_attempts_sampled ?
			d->rp_new_slot_score_ratio_sum / (double)d->rp_attempts_sampled : 0.0;

		printf(
			"hnsw_v2_diag_candidate_quality "
			"level0_search_count=%llu "
			"level0_exact_best_visited=%llu level0_exact_best_in_bounded=%llu "
			"level0_recall_at_M0=%.4f level0_recall_at_ef=%.4f "
			"upper_search_count=%llu "
			"upper_exact_best_visited=%llu upper_exact_best_in_bounded=%llu "
			"upper_recall_at_M=%.4f upper_recall_at_ef=%.4f\n",
			(unsigned long long)d->level0_layer_search_count,
			(unsigned long long)d->level0_exact_best_visited,
			(unsigned long long)d->level0_exact_best_in_bounded,
			l0_recall_M0, l0_recall_ef,
			(unsigned long long)d->upper_layer_search_count,
			(unsigned long long)d->upper_exact_best_visited,
			(unsigned long long)d->upper_exact_best_in_bounded,
			up_recall_M, up_recall_ef
		);

		printf(
			"hnsw_v2_diag_neighbor_select "
			"ns_calls=%llu ns_bounded_sel_avg=%.2f ns_full_sel_avg=%.2f "
			"ns_overlap_ratio=%.4f ns_pruned_was_exact_M=%llu "
			"ns_fallback=%llu\n",
			(unsigned long long)d->ns_diag_calls,
			d->ns_diag_calls ? (double)d->ns_bounded_selected_total / (double)d->ns_diag_calls : 0.0,
			d->ns_diag_calls ? (double)d->ns_full_selected_total / (double)d->ns_diag_calls : 0.0,
			ns_overlap_ratio,
			(unsigned long long)d->ns_pruned_was_in_exact_M,
			(unsigned long long)d->ns_fallback_count
		);

		printf(
			"hnsw_v2_diag_reverse_prune "
			"rp_attempts=%llu rp_rejected_exact_best=%llu "
			"rp_pruned_exact_best=%llu rp_score_ratio_avg=%.4f\n",
			(unsigned long long)d->rp_attempts_sampled,
			(unsigned long long)d->rp_rejected_was_exact_best,
			(unsigned long long)d->rp_pruned_was_exact_best,
			rp_score_ratio
		);

		printf(
			"hnsw_v2_diag_entry_drift "
			"upper_greedy_calls=%llu upper_drift_avg=%.6f "
			"upper_drift_slot_match=%llu "
			"level0_entry_calls=%llu level0_entry_score_ratio_avg=%.4f\n",
			(unsigned long long)d->upper_greedy_diag_calls,
			upper_drift_avg,
			(unsigned long long)d->upper_drift_slot_match,
			(unsigned long long)d->level0_entry_diag_calls,
			l0_entry_ratio
		);

		printf(
			"hnsw_v2_diag_graph "
			"selected=%llu forward=%llu reverse_attempt=%llu "
			"reverse_accepted=%llu reverse_rejected=%llu reverse_prune=%llu "
			"duplicate=%llu overflow=%llu "
			"level0_deg_avg=%.2f level0_deg_p50=%u level0_deg_p95=%u "
			"upper_deg_avg=%.2f upper_deg_p95=%u memory=%zu\n",
			(unsigned long long)index.graph_stats.selected_neighbor_count,
			(unsigned long long)index.graph_stats.forward_edge_added_count,
			(unsigned long long)index.graph_stats.reverse_edge_attempt_count,
			(unsigned long long)index.graph_stats.reverse_edge_accepted_count,
			(unsigned long long)index.graph_stats.reverse_edge_rejected_count,
			(unsigned long long)index.graph_stats.reverse_prune_count,
			(unsigned long long)index.graph_stats.duplicate_edge_count,
			(unsigned long long)index.graph_stats.degree_overflow_count,
			index.graph_stats.level0_degree.avg,
			index.graph_stats.level0_degree.p50,
			index.graph_stats.level0_degree.p95,
			index.graph_stats.upper_degree.avg,
			index.graph_stats.upper_degree.p95,
			index.memory.total_bytes
		);
	}

	/* Search recall at ef=256 and ef=512 */
	for (i = 0; i < queries; i++) {
		fill_random_unit_vector(query_data + i * dim, dim, 0xdad00000ULL + i);
		exact_topk_ip(
			base, count, dim, index.deleted_flags,
			query_data + i * dim, topk,
			exact + i * topk, &exact_counts[i]);
	}

	/* Disable diag for search (diag only applies to build) */
	index.diag.enabled = 0;

	{
		kvs_vector_hit_t *hits = (kvs_vector_hit_t *)malloc(topk * sizeof(kvs_vector_hit_t));
		double recall_sum = 0.0;
		uint64_t search_sum = 0;

		for (q = 0; q < queries; q++) {
			size_t hit_count = 0;
			uint64_t start = now_us();
			if (kvs_hnsw_v2_index_search(&index,
				    query_data + q * dim, dim, topk, 256,
				    hits, &hit_count) == KVS_VECTOR_OK) {
				search_sum += now_us() - start;
				recall_sum += recall_at_k(
					exact + q * topk, exact_counts[q], hits, hit_count);
			}
		}
		recall_256 = recall_sum / (double)queries;
		search_avg_256 = search_sum / queries;

		recall_sum = 0.0; search_sum = 0;
		for (q = 0; q < queries; q++) {
			size_t hit_count = 0;
			uint64_t start = now_us();
			if (kvs_hnsw_v2_index_search(&index,
				    query_data + q * dim, dim, topk, 512,
				    hits, &hit_count) == KVS_VECTOR_OK) {
				search_sum += now_us() - start;
				recall_sum += recall_at_k(
					exact + q * topk, exact_counts[q], hits, hit_count);
			}
		}
		recall_512 = recall_sum / (double)queries;
		search_avg_512 = search_sum / queries;
		free(hits);
	}

	printf(
		"hnsw_v2_diag_search count=%zu dim=%zu topk=%zu queries=%zu "
		"ef=256 recall@%zu=%.4f search_avg_us=%llu "
		"ef=512 recall@%zu=%.4f search_avg_us=%llu\n",
		count, dim, topk, queries,
		topk, recall_256, (unsigned long long)search_avg_256,
		topk, recall_512, (unsigned long long)search_avg_512
	);

done:
	kvs_hnsw_v2_index_destroy(&index);
	free(base);
	free(query_data);
	free(exact);
	free(exact_counts);
	return failed;
}

/* ── Stage 9E1a: ef_construction sensitivity ───────────────────── */

static int run_ef_construction_profile(
	size_t count,
	size_t dim,
	size_t topk,
	size_t queries,
	size_t ef_construction)
{
	kvs_hnsw_v2_index_t index;
	float *base;
	float *query_data;
	exact_item_t *exact;
	size_t *exact_counts;
	size_t i;
	int failed = 0;
	uint64_t build_start_us;
	uint64_t build_us;
	double recall_256 = 0.0;
	double recall_512 = 0.0;
	uint64_t search_avg_256 = 0;
	uint64_t search_avg_512 = 0;

	memset(&index, 0, sizeof(index));
	base = (float *)malloc(count * dim * sizeof(float));
	query_data = (float *)malloc(queries * dim * sizeof(float));
	exact = (exact_item_t *)calloc(queries * topk, sizeof(*exact));
	exact_counts = (size_t *)calloc(queries, sizeof(*exact_counts));
	if (!base || !query_data || !exact || !exact_counts) {
		free(base); free(query_data); free(exact); free(exact_counts);
		return 1;
	}

	if (kvs_hnsw_v2_index_init(&index, dim, KVS_VECTOR_METRIC_IP) != KVS_VECTOR_OK) {
		fprintf(stderr, "ef_construction init failed count=%zu\n", count);
		failed = 1;
		goto done;
	}
	index.M = KVS_HNSW_V2_DEFAULT_M;
	index.M0 = KVS_HNSW_V2_DEFAULT_M0;
	index.ef_construction = ef_construction;
	index.ef_search = KVS_HNSW_V2_DEFAULT_EF_SEARCH;
	kvs_hnsw_v2_index_set_seed(&index, 0x9d000000ULL + count);

	if (kvs_hnsw_v2_index_reserve(&index, count) != KVS_VECTOR_OK) {
		fprintf(stderr, "ef_construction reserve failed count=%zu\n", count);
		failed = 1;
		goto done;
	}

	printf("hnsw_v2_ef_construction_build_start count=%zu dim=%zu ef_construction=%zu\n",
		count, dim, ef_construction);
	fflush(stdout);
	build_start_us = now_us();
	for (i = 0; i < count; i++) {
		fill_random_unit_vector(base + i * dim, dim, 0xabc00000ULL + i);
		if (kvs_hnsw_v2_index_add(
			    &index,
			    1000000ULL + i,
			    base + i * dim,
			    dim
		    ) != KVS_VECTOR_OK) {
			fprintf(stderr, "ef_construction add failed count=%zu i=%zu\n", count, i);
			failed = 1;
			goto done;
		}
	}
	build_us = now_us() - build_start_us;
	kvs_hnsw_v2_graph_stats_refresh(&index);
	kvs_hnsw_v2_memory_refresh(&index);

	printf(
		"hnsw_v2_ef_construction_build count=%zu dim=%zu ef_construction=%zu "
		"build_us=%llu add_total_us=%llu "
		"construction_layer_search_us=%llu neighbor_select_us=%llu "
		"reverse_prune_us=%llu "
		"construction_visited=%llu construction_distance=%llu "
		"candidate_push=%llu candidate_pop=%llu candidate_skip=%llu "
		"top_push=%llu top_replace=%llu top_skip=%llu heap_sift=%llu\n",
		count, dim, ef_construction,
		(unsigned long long)build_us,
		(unsigned long long)index.build_stats.add_total_us,
		(unsigned long long)index.build_stats.construction_layer_search_us,
		(unsigned long long)index.build_stats.neighbor_select_us,
		(unsigned long long)index.build_stats.reverse_prune_us,
		(unsigned long long)index.build_stats.construction_visited_count,
		(unsigned long long)index.build_stats.construction_distance_compute_count,
		(unsigned long long)index.build_stats.construction_candidate_push_count,
		(unsigned long long)index.build_stats.construction_candidate_pop_count,
		(unsigned long long)index.build_stats.construction_candidate_skip_count,
		(unsigned long long)index.build_stats.construction_top_push_count,
		(unsigned long long)index.build_stats.construction_top_replace_count,
		(unsigned long long)index.build_stats.construction_top_skip_count,
		(unsigned long long)index.build_stats.construction_heap_sift_count
	);

	/* Exact search for recall computation */
	for (i = 0; i < queries; i++) {
		fill_random_unit_vector(query_data + i * dim, dim, 0xdad00000ULL + i);
		exact_topk_ip(
			base, count, dim, index.deleted_flags,
			query_data + i * dim, topk,
			exact + i * topk, &exact_counts[i]);
	}

	/* Search at ef=256 and ef=512 */
	{
		kvs_vector_hit_t *hits = (kvs_vector_hit_t *)malloc(topk * sizeof(kvs_vector_hit_t));
		size_t q;
		double recall_sum;
		uint64_t search_sum;

		recall_sum = 0.0; search_sum = 0;
		for (q = 0; q < queries; q++) {
			size_t hit_count = 0;
			uint64_t start = now_us();
			if (kvs_hnsw_v2_index_search(&index,
				    query_data + q * dim, dim, topk, 256,
				    hits, &hit_count) == KVS_VECTOR_OK) {
				search_sum += now_us() - start;
				recall_sum += recall_at_k(
					exact + q * topk, exact_counts[q], hits, hit_count);
			}
		}
		recall_256 = recall_sum / (double)queries;
		search_avg_256 = queries ? search_sum / queries : 0;

		recall_sum = 0.0; search_sum = 0;
		for (q = 0; q < queries; q++) {
			size_t hit_count = 0;
			uint64_t start = now_us();
			if (kvs_hnsw_v2_index_search(&index,
				    query_data + q * dim, dim, topk, 512,
				    hits, &hit_count) == KVS_VECTOR_OK) {
				search_sum += now_us() - start;
				recall_sum += recall_at_k(
					exact + q * topk, exact_counts[q], hits, hit_count);
			}
		}
		recall_512 = recall_sum / (double)queries;
		search_avg_512 = queries ? search_sum / queries : 0;
		free(hits);
	}

	printf(
		"hnsw_v2_ef_construction_result count=%zu dim=%zu topk=%zu queries=%zu "
		"ef_construction=%zu "
		"ef=256 recall@%zu=%.4f search_avg_us=%llu "
		"ef=512 recall@%zu=%.4f search_avg_us=%llu "
		"graph_selected=%llu reverse_prune=%llu "
		"level0_deg_avg=%.2f upper_deg_avg=%.2f memory=%zu "
		"neighbor_select_us=%llu\n",
		count, dim, topk, queries, ef_construction,
		topk, recall_256, (unsigned long long)search_avg_256,
		topk, recall_512, (unsigned long long)search_avg_512,
		(unsigned long long)index.graph_stats.selected_neighbor_count,
		(unsigned long long)index.graph_stats.reverse_prune_count,
		index.graph_stats.level0_degree.avg,
		index.graph_stats.upper_degree.avg,
		index.memory.total_bytes,
		(unsigned long long)index.build_stats.neighbor_select_us
	);

done:
	kvs_hnsw_v2_index_destroy(&index);
	free(base);
	free(query_data);
	free(exact);
	free(exact_counts);
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

	/* Stage 9E1a: diagnostic candidate-quality profile (1K, sample every add) */
	failed |= run_diag_profile(1000, 512, 10, 50, 1);

	if (failed) {
		return 1;
	}

	printf("vector_core_hnsw_v2_performance_profile_test PASS\n");
	return 0;
}
