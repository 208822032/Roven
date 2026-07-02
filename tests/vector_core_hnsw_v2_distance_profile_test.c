#include "kvs_config.h"
#include "kvs_memory.h"
#include "kvs_memory_internal.h"
#include "kvs_vector_distance.h"
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
	uint64_t metric_score_calls;
	uint64_t upper_distance;
	uint64_t level0_distance;
	uint64_t distance_us_est;
	uint64_t level0_scan;
} query_profile_t;

typedef struct case_summary_s {
	int valid;
	size_t count;
	size_t dim;
	char requested_backend[32];
	char active_backend[32];
	double distance_us_per_call;
	uint64_t build_us;
	uint64_t build_distance_compute_count;
	double build_distance_us_est;
	double build_distance_ratio;
	double recall_256;
	double recall_512;
	double search_avg_256;
	double search_avg_512;
	double search_distance_us_avg_256;
	double search_distance_us_avg_512;
	size_t memory_bytes;
} case_summary_t;

static volatile float g_distance_profile_sink;

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

static uint64_t get_search_us(const query_profile_t *p) { return p->search_us; }
static uint64_t get_visited(const query_profile_t *p) { return p->visited; }
static uint64_t get_distance(const query_profile_t *p) { return p->distance; }
static uint64_t get_metric_score_calls(const query_profile_t *p) { return p->metric_score_calls; }
static uint64_t get_upper_distance(const query_profile_t *p) { return p->upper_distance; }
static uint64_t get_level0_distance(const query_profile_t *p) { return p->level0_distance; }
static uint64_t get_distance_us_est(const query_profile_t *p) { return p->distance_us_est; }

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

static double calibrate_distance_us_per_call(
	const float *base,
	const float *queries,
	size_t count,
	size_t dim,
	size_t query_count
) {
	size_t iterations;
	size_t i;
	uint64_t start_us;
	uint64_t elapsed_us;
	float sink = 0.0f;

	if (!base || !queries || count == 0 || dim == 0 || query_count == 0) {
		return 0.0;
	}

	iterations = dim >= 768 ? 30000 : 50000;
	start_us = now_us();
	for (i = 0; i < iterations; i++) {
		size_t q = i % query_count;
		size_t slot = (i * 2654435761u) % count;
		sink += kvs_vector_distance_dot(
			queries + q * dim,
			base + slot * dim,
			dim
		);
	}
	elapsed_us = now_us() - start_us;
	g_distance_profile_sink += sink;
	if (elapsed_us == 0) {
		return 0.0;
	}
	return (double)elapsed_us / (double)iterations;
}

static int set_distance_backend(
	const char *backend,
	char *active,
	size_t active_len
) {
	snprintf(g_cfg.vector_distance_backend,
		sizeof(g_cfg.vector_distance_backend),
		"%s",
		backend ? backend : "auto");
	if (kvs_vector_distance_init() != KVS_VECTOR_OK) {
		return KVS_VECTOR_ERR;
	}
	snprintf(active, active_len, "%s", kvs_vector_distance_backend_name());
	return KVS_VECTOR_OK;
}

static int run_search_profile(
	kvs_hnsw_v2_index_t *index,
	const float *queries_data,
	const exact_item_t *exact,
	const size_t *exact_counts,
	size_t count,
	size_t dim,
	size_t topk,
	size_t queries,
	size_t ef_search,
	double distance_us_per_call,
	const char *backend,
	double *out_recall,
	double *out_search_avg,
	double *out_distance_us_avg
) {
	query_profile_t *profiles;
	uint64_t *scratch;
	kvs_vector_hit_t *hits;
	double recall_sum = 0.0;
	size_t q;
	int failed = 0;

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
			fprintf(stderr,
				"distance profile search failed count=%zu dim=%zu ef=%zu q=%zu ret=%d\n",
				count, dim, ef_search, q, ret);
			failed = 1;
			break;
		}
		if (!hits_descending(hits, hit_count)) {
			fprintf(stderr,
				"distance profile hits not descending count=%zu dim=%zu ef=%zu q=%zu\n",
				count, dim, ef_search, q);
			failed = 1;
			break;
		}

		profiles[q].visited = index->search_stats.last_visited_count;
		profiles[q].distance = index->search_stats.last_distance_compute_count;
		profiles[q].metric_score_calls =
			index->search_stats.last_metric_score_call_count;
		profiles[q].upper_distance =
			index->search_stats.last_upper_distance_compute_count;
		profiles[q].level0_distance =
			index->search_stats.last_level0_distance_compute_count;
		profiles[q].distance_us_est = (uint64_t)(
			(distance_us_per_call * (double)profiles[q].distance) + 0.5
		);
		profiles[q].level0_scan =
			index->search_stats.last_level0_neighbor_scan_count;

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
		double metric_score_avg =
			avg_profile(profiles, queries, get_metric_score_calls);
		double upper_distance_avg =
			avg_profile(profiles, queries, get_upper_distance);
		double level0_distance_avg =
			avg_profile(profiles, queries, get_level0_distance);
		double distance_us_avg =
			avg_profile(profiles, queries, get_distance_us_est);
		double distance_ratio = search_avg > 0.0 ?
			distance_us_avg / search_avg : 0.0;
		double level0_vs_visited_delta =
			level0_distance_avg - visited_avg;
		double metric_vs_distance_delta =
			metric_score_avg - distance_avg;

		if (out_recall) {
			*out_recall = recall;
		}
		if (out_search_avg) {
			*out_search_avg = search_avg;
		}
		if (out_distance_us_avg) {
			*out_distance_us_avg = distance_us_avg;
		}

		copy_metric(profiles, queries, get_search_us, scratch);
		printf(
			"hnsw_v2_distance_search count=%zu dim=%zu backend=%s topk=%zu "
			"queries=%zu ef=%zu recall@%zu=%.4f search_avg_us=%.2f "
			"search_p50_us=%llu search_p95_us=%llu search_p99_us=%llu ",
			count,
			dim,
			backend,
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
		copy_metric(profiles, queries, get_distance, scratch);
		printf(
			"distance_avg=%.2f distance_p50=%llu distance_p95=%llu "
			"distance_p99=%llu ",
			distance_avg,
			(unsigned long long)percentile_u64(scratch, queries, 50.0),
			(unsigned long long)percentile_u64(scratch, queries, 95.0),
			(unsigned long long)percentile_u64(scratch, queries, 99.0)
		);
		copy_metric(profiles, queries, get_distance_us_est, scratch);
		printf(
			"distance_us_avg_est=%.2f distance_us_p50_est=%llu "
			"distance_us_p95_est=%llu distance_us_p99_est=%llu "
			"distance_us_ratio_est=%.4f ",
			distance_us_avg,
			(unsigned long long)percentile_u64(scratch, queries, 50.0),
			(unsigned long long)percentile_u64(scratch, queries, 95.0),
			(unsigned long long)percentile_u64(scratch, queries, 99.0),
			distance_ratio
		);
		printf(
			"visited_avg=%.2f metric_score_call_avg=%.2f "
			"metric_minus_distance_avg=%.2f "
			"upper_distance_avg=%.2f level0_distance_avg=%.2f "
			"level0_distance_minus_visited_avg=%.2f "
			"level0_scan_avg=%.2f upper_greedy_us_avg=%.2f "
			"level0_search_us_avg=%.2f candidate_select_us_avg=%.2f "
			"top_maintenance_us_avg=%.2f\n",
			visited_avg,
			metric_score_avg,
			metric_vs_distance_delta,
			upper_distance_avg,
			level0_distance_avg,
			level0_vs_visited_delta,
			(double)index->search_stats.search_level0_neighbor_scan_count /
				(double)queries,
			(double)index->search_stats.search_upper_greedy_us / (double)queries,
			(double)index->search_stats.search_level0_search_us / (double)queries,
			(double)index->search_stats.search_candidate_select_us / (double)queries,
			(double)index->search_stats.search_top_maintenance_us / (double)queries
		);
	}

	free(profiles);
	free(scratch);
	free(hits);
	return failed;
}

static int run_case(
	const char *requested_backend,
	size_t count,
	size_t dim,
	size_t topk,
	size_t queries,
	case_summary_t *summary
) {
	kvs_hnsw_v2_index_t index;
	float *base;
	float *query_data;
	exact_item_t *exact;
	size_t *exact_counts;
	size_t i;
	int failed = 0;
	uint64_t build_start_us;
	uint64_t build_us;
	uint64_t generate_start_us;
	uint64_t dataset_generate_us;
	uint64_t exact_start_us;
	uint64_t exact_us;
	char active_backend[32];
	double distance_us_per_call;
	double recall_256 = 0.0;
	double recall_512 = 0.0;
	double search_avg_256 = 0.0;
	double search_avg_512 = 0.0;
	double search_distance_us_avg_256 = 0.0;
	double search_distance_us_avg_512 = 0.0;
	uint64_t construction_ns_distance;
	uint64_t build_distance_non_construction;
	double build_distance_us_est;
	double build_distance_ratio;

	memset(&index, 0, sizeof(index));
	if (summary) {
		memset(summary, 0, sizeof(*summary));
	}

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

	if (set_distance_backend(requested_backend, active_backend,
		    sizeof(active_backend)) != KVS_VECTOR_OK) {
		fprintf(stderr, "distance backend init failed backend=%s\n",
			requested_backend);
		failed = 1;
		goto done;
	}

	generate_start_us = now_us();
	for (i = 0; i < count; i++) {
		fill_random_unit_vector(base + i * dim, dim, 0xabc00000ULL + i);
	}
	for (i = 0; i < queries; i++) {
		fill_random_unit_vector(query_data + i * dim, dim, 0xdad00000ULL + i);
	}
	dataset_generate_us = now_us() - generate_start_us;

	distance_us_per_call =
		calibrate_distance_us_per_call(base, query_data, count, dim, queries);

	if (kvs_hnsw_v2_index_init(&index, dim, KVS_VECTOR_METRIC_IP) != KVS_VECTOR_OK) {
		fprintf(stderr, "distance profile init failed count=%zu dim=%zu\n",
			count, dim);
		failed = 1;
		goto done;
	}
	index.M = KVS_HNSW_V2_DEFAULT_M;
	index.M0 = KVS_HNSW_V2_DEFAULT_M0;
	index.ef_construction = KVS_HNSW_V2_DEFAULT_EF_CONSTRUCTION;
	index.ef_search = KVS_HNSW_V2_DEFAULT_EF_SEARCH;
	kvs_hnsw_v2_index_set_seed(&index, 0x9d000000ULL + count);
	if (kvs_hnsw_v2_index_reserve(&index, count) != KVS_VECTOR_OK) {
		fprintf(stderr, "distance profile reserve failed count=%zu dim=%zu\n",
			count, dim);
		failed = 1;
		goto done;
	}

	printf(
		"hnsw_v2_distance_build_start count=%zu dim=%zu requested_backend=%s "
		"active_backend=%s ef_construction=%zu calibration_us_per_call=%.6f "
		"dataset_generate_us=%llu\n",
		count,
		dim,
		requested_backend,
		active_backend,
		index.ef_construction,
		distance_us_per_call,
		(unsigned long long)dataset_generate_us
	);
	fflush(stdout);

	build_start_us = now_us();
	for (i = 0; i < count; i++) {
		if (kvs_hnsw_v2_index_add(
			    &index,
			    1000000ULL + i,
			    base + i * dim,
			    dim
		    ) != KVS_VECTOR_OK) {
			fprintf(stderr,
				"distance profile add failed count=%zu dim=%zu i=%zu\n",
				count, dim, i);
			failed = 1;
			goto done;
		}
	}
	build_us = now_us() - build_start_us;
	kvs_hnsw_v2_graph_stats_refresh(&index);
	kvs_hnsw_v2_memory_refresh(&index);

	construction_ns_distance =
		index.build_stats.neighbor_select_distance_compute_count -
		index.build_stats.reverse_prune_neighbor_select_distance_compute_count;
	build_distance_non_construction =
		index.build_stats.distance_compute_count -
		index.build_stats.construction_distance_compute_count;
	build_distance_us_est =
		(double)index.build_stats.distance_compute_count * distance_us_per_call;
	build_distance_ratio = build_us > 0 ?
		build_distance_us_est / (double)build_us : 0.0;

	printf(
		"hnsw_v2_distance_build count=%zu dim=%zu backend=%s build_us=%llu "
		"add_total_us=%llu construction_layer_search_us=%llu "
		"neighbor_select_us=%llu reverse_prune_us=%llu "
		"build_distance_compute_count=%llu build_distance_us_est=%.2f "
		"build_distance_ratio_est=%.4f "
		"construction_distance_compute_count=%llu "
		"construction_upper_distance_compute_count=%llu "
		"construction_level0_distance_compute_count=%llu "
		"neighbor_select_distance_compute_count=%llu "
		"construction_neighbor_select_distance_compute_count=%llu "
		"reverse_prune_distance_compute_count=%llu "
		"reverse_prune_neighbor_select_distance_compute_count=%llu "
		"build_distance_non_construction_count=%llu "
		"construction_neighbor_scan_count=%llu "
		"selected=%llu pruned=%llu reverse_prune=%llu "
		"duplicate=%llu overflow=%llu level0_degree_avg=%.2f "
		"level0_degree_p95=%u upper_degree_avg=%.2f upper_degree_p95=%u "
		"memory_bytes=%zu\n",
		count,
		dim,
		active_backend,
		(unsigned long long)build_us,
		(unsigned long long)index.build_stats.add_total_us,
		(unsigned long long)index.build_stats.construction_layer_search_us,
		(unsigned long long)index.build_stats.neighbor_select_us,
		(unsigned long long)index.build_stats.reverse_prune_us,
		(unsigned long long)index.build_stats.distance_compute_count,
		build_distance_us_est,
		build_distance_ratio,
		(unsigned long long)index.build_stats.construction_distance_compute_count,
		(unsigned long long)index.build_stats.construction_upper_distance_compute_count,
		(unsigned long long)index.build_stats.construction_level0_distance_compute_count,
		(unsigned long long)index.build_stats.neighbor_select_distance_compute_count,
		(unsigned long long)construction_ns_distance,
		(unsigned long long)index.build_stats.reverse_prune_distance_compute_count,
		(unsigned long long)index.build_stats.reverse_prune_neighbor_select_distance_compute_count,
		(unsigned long long)build_distance_non_construction,
		(unsigned long long)index.build_stats.construction_neighbor_scan_count,
		(unsigned long long)index.graph_stats.selected_neighbor_count,
		(unsigned long long)index.graph_stats.neighbor_select_pruned_count,
		(unsigned long long)index.graph_stats.reverse_prune_count,
		(unsigned long long)index.graph_stats.duplicate_edge_count,
		(unsigned long long)index.graph_stats.degree_overflow_count,
		index.graph_stats.level0_degree.avg,
		index.graph_stats.level0_degree.p95,
		index.graph_stats.upper_degree.avg,
		index.graph_stats.upper_degree.p95,
		index.memory.total_bytes
	);

	exact_start_us = now_us();
	for (i = 0; i < queries; i++) {
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
	exact_us = now_us() - exact_start_us;
	printf(
		"hnsw_v2_distance_exact count=%zu dim=%zu backend=%s exact_us=%llu\n",
		count,
		dim,
		active_backend,
		(unsigned long long)exact_us
	);

	if (run_search_profile(
		    &index,
		    query_data,
		    exact,
		    exact_counts,
		    count,
		    dim,
		    topk,
		    queries,
		    256,
		    distance_us_per_call,
		    active_backend,
		    &recall_256,
		    &search_avg_256,
		    &search_distance_us_avg_256
	    ) != 0) {
		failed = 1;
		goto done;
	}
	if (run_search_profile(
		    &index,
		    query_data,
		    exact,
		    exact_counts,
		    count,
		    dim,
		    topk,
		    queries,
		    512,
		    distance_us_per_call,
		    active_backend,
		    &recall_512,
		    &search_avg_512,
		    &search_distance_us_avg_512
	    ) != 0) {
		failed = 1;
		goto done;
	}

	printf(
		"hnsw_v2_distance_case_summary count=%zu dim=%zu requested_backend=%s "
		"active_backend=%s build_us=%llu search_avg_us_ef256=%.2f "
		"search_avg_us_ef512=%.2f recall_ef256=%.4f recall_ef512=%.4f "
		"build_distance_ratio_est=%.4f search_distance_ratio_ef256_est=%.4f "
		"search_distance_ratio_ef512_est=%.4f memory_bytes=%zu\n",
		count,
		dim,
		requested_backend,
		active_backend,
		(unsigned long long)build_us,
		search_avg_256,
		search_avg_512,
		recall_256,
		recall_512,
		build_distance_ratio,
		search_avg_256 > 0.0 ? search_distance_us_avg_256 / search_avg_256 : 0.0,
		search_avg_512 > 0.0 ? search_distance_us_avg_512 / search_avg_512 : 0.0,
		index.memory.total_bytes
	);

	if (summary) {
		summary->valid = 1;
		summary->count = count;
		summary->dim = dim;
		snprintf(summary->requested_backend, sizeof(summary->requested_backend),
			"%s", requested_backend);
		snprintf(summary->active_backend, sizeof(summary->active_backend),
			"%s", active_backend);
		summary->distance_us_per_call = distance_us_per_call;
		summary->build_us = build_us;
		summary->build_distance_compute_count =
			index.build_stats.distance_compute_count;
		summary->build_distance_us_est = build_distance_us_est;
		summary->build_distance_ratio = build_distance_ratio;
		summary->recall_256 = recall_256;
		summary->recall_512 = recall_512;
		summary->search_avg_256 = search_avg_256;
		summary->search_avg_512 = search_avg_512;
		summary->search_distance_us_avg_256 = search_distance_us_avg_256;
		summary->search_distance_us_avg_512 = search_distance_us_avg_512;
		summary->memory_bytes = index.memory.total_bytes;
	}

	if (recall_512 + 0.000001 < recall_256) {
		fprintf(stderr,
			"distance profile ef sensitivity regressed count=%zu dim=%zu "
			"backend=%s recall256=%.4f recall512=%.4f\n",
			count, dim, active_backend, recall_256, recall_512);
		failed = 1;
	}

done:
	kvs_hnsw_v2_index_destroy(&index);
	free(base);
	free(query_data);
	free(exact);
	free(exact_counts);
	return failed;
}

int main(void) {
	static const size_t dims[] = {384, 512, 768, 1024};
	case_summary_t auto_512;
	case_summary_t fallback_512;
	case_summary_t avx2_512;
	size_t i;
	int failed = 0;

	memset(&g_cfg, 0, sizeof(g_cfg));
	kvs_config_init_default(&g_cfg);
	g_cfg.mem_backend = KVS_MEM_BACKEND_SYSTEM;
	mem_set_backend(KVS_MEM_BACKEND_SYSTEM);

	memset(&auto_512, 0, sizeof(auto_512));
	memset(&fallback_512, 0, sizeof(fallback_512));
	memset(&avx2_512, 0, sizeof(avx2_512));

	for (i = 0; i < sizeof(dims) / sizeof(dims[0]); i++) {
		case_summary_t summary;

		if (run_case("auto", 10000, dims[i], 10, 50, &summary) != 0) {
			failed = 1;
			break;
		}
		if (dims[i] == 512) {
			auto_512 = summary;
			if (summary.recall_256 < 0.98 || summary.recall_512 < 0.99) {
				fprintf(stderr,
					"distance profile recall gate failed auto 10K/512 "
					"recall256=%.4f recall512=%.4f\n",
					summary.recall_256,
					summary.recall_512);
				failed = 1;
				break;
			}
		}
	}

	if (!failed) {
		failed |= run_case("fallback", 10000, 512, 10, 50, &fallback_512);
	}
	if (!failed && strcmp(auto_512.active_backend, "avx2") == 0) {
		avx2_512 = auto_512;
	} else if (!failed) {
		failed |= run_case("avx2", 10000, 512, 10, 50, &avx2_512);
	}

	if (!failed && fallback_512.valid && avx2_512.valid) {
		int avx2_available = strcmp(avx2_512.active_backend, "avx2") == 0;
		printf(
			"hnsw_v2_distance_backend_compare count=10000 dim=512 "
			"fallback_active=%s avx2_active=%s avx2_available=%d "
			"fallback_build_us=%llu avx2_build_us=%llu "
			"fallback_search_avg_us_ef256=%.2f avx2_search_avg_us_ef256=%.2f "
			"fallback_distance_us_per_call=%.6f avx2_distance_us_per_call=%.6f "
			"fallback_recall_ef256=%.4f avx2_recall_ef256=%.4f "
			"fallback_recall_ef512=%.4f avx2_recall_ef512=%.4f\n",
			fallback_512.active_backend,
			avx2_512.active_backend,
			avx2_available,
			(unsigned long long)fallback_512.build_us,
			(unsigned long long)avx2_512.build_us,
			fallback_512.search_avg_256,
			avx2_512.search_avg_256,
			fallback_512.distance_us_per_call,
			avx2_512.distance_us_per_call,
			fallback_512.recall_256,
			avx2_512.recall_256,
			fallback_512.recall_512,
			avx2_512.recall_512
		);
	}

	if (failed) {
		return 1;
	}

	printf("vector_core_hnsw_v2_distance_profile_test PASS\n");
	return 0;
}
