#include "kvs_config.h"
#include "kvs_memory.h"
#include "kvs_memory_internal.h"
#include "vector/kvs_vector_flat.h"
#include "vector/kvs_vector_hnsw_v2.h"

#include <math.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/time.h>

extern kvs_config_t g_cfg;

#define P2_COUNT 100000
#define P2_DIM 512
#define P2_TOPK 10
#define P2_BASE_RECORD_ID 1000000ULL
#define P2_DEFAULT_QUERIES 100
#define P2_M 48
#define P2_M0 96
#define P2_EF_CONSTRUCTION 512
#define P2_EF_SEARCH 1024

typedef struct p2_exact_item_s {
	uint64_t record_id;
	float score;
} p2_exact_item_t;

typedef struct p2_dataset_s {
	size_t count;
	size_t queries;
	float *base;
	float *query;
	p2_exact_item_t *exact;
	size_t *exact_counts;
	size_t oracle_mismatch_count;
	uint64_t oracle_us;
} p2_dataset_t;

static uint64_t p2_now_us(void) {
	struct timeval tv;

	gettimeofday(&tv, NULL);
	return (uint64_t)tv.tv_sec * 1000000ULL + (uint64_t)tv.tv_usec;
}

static uint32_t p2_rng_next(uint64_t *state) {
	uint64_t z;

	*state += 0x9e3779b97f4a7c15ULL;
	z = *state;
	z = (z ^ (z >> 30)) * 0xbf58476d1ce4e5b9ULL;
	z = (z ^ (z >> 27)) * 0x94d049bb133111ebULL;
	z ^= z >> 31;
	return (uint32_t)(z >> 32);
}

static void p2_fill_random_unit_vector(float *vector, size_t dim, uint64_t seed) {
	uint64_t state = seed;
	float norm = 0.0f;
	size_t i;

	for (i = 0; i < dim; i++) {
		uint32_t raw = p2_rng_next(&state);
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

static float p2_ip_score(const float *lhs, const float *rhs, size_t dim) {
	float score = 0.0f;
	size_t i;

	for (i = 0; i < dim; i++) {
		score += lhs[i] * rhs[i];
	}
	return score;
}

static int p2_better(float lhs_score, uint64_t lhs_id, float rhs_score, uint64_t rhs_id) {
	if (lhs_score > rhs_score) {
		return 1;
	}
	if (lhs_score < rhs_score) {
		return 0;
	}
	return lhs_id < rhs_id;
}

static void p2_direct_exact_topk(
	const float *base,
	size_t count,
	const float *query,
	p2_exact_item_t *out,
	size_t *out_count
) {
	size_t n = 0;
	size_t i;

	for (i = 0; i < count; i++) {
		float score = p2_ip_score(query, base + i * P2_DIM, P2_DIM);
		uint64_t record_id = P2_BASE_RECORD_ID + (uint64_t)i;
		size_t pos;

		if (n == P2_TOPK &&
		    !p2_better(score, record_id, out[n - 1].score, out[n - 1].record_id)) {
			continue;
		}

		if (n < P2_TOPK) {
			n++;
		}
		pos = n - 1;
		while (pos > 0 &&
		       p2_better(score, record_id, out[pos - 1].score, out[pos - 1].record_id)) {
			out[pos] = out[pos - 1];
			pos--;
		}
		out[pos].record_id = record_id;
		out[pos].score = score;
	}

	*out_count = n;
}

static int p2_hits_descending(const kvs_vector_hit_t *hits, size_t count) {
	size_t i;

	for (i = 1; i < count; i++) {
		if (hits[i - 1].score < hits[i].score) {
			return 0;
		}
	}
	return 1;
}

static int p2_exact_same_ids(
	const p2_exact_item_t *exact,
	size_t exact_count,
	const kvs_vector_hit_t *hits,
	size_t hit_count
) {
	size_t i;

	if (exact_count != hit_count) {
		return 0;
	}
	for (i = 0; i < exact_count; i++) {
		if (exact[i].record_id != hits[i].record_id) {
			return 0;
		}
	}
	return 1;
}

static double p2_recall_hits(
	const p2_exact_item_t *exact,
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
			if (hits[j].record_id == exact[i].record_id) {
				matched++;
				break;
			}
		}
	}
	return (double)matched / (double)exact_count;
}

static int p2_cmp_u64(const void *lhs, const void *rhs) {
	uint64_t a = *(const uint64_t *)lhs;
	uint64_t b = *(const uint64_t *)rhs;

	return (a > b) - (a < b);
}

static uint64_t p2_percentile_u64(const uint64_t *values, size_t count, double pct) {
	uint64_t *copy;
	uint64_t value;
	size_t idx;

	if (!values || count == 0) {
		return 0;
	}
	copy = (uint64_t *)malloc(count * sizeof(*copy));
	if (!copy) {
		return 0;
	}
	memcpy(copy, values, count * sizeof(*copy));
	qsort(copy, count, sizeof(*copy), p2_cmp_u64);
	idx = (size_t)((pct / 100.0) * (double)count);
	if (idx >= count) {
		idx = count - 1;
	}
	value = copy[idx];
	free(copy);
	return value;
}

static double p2_avg_u64(const uint64_t *values, size_t count) {
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

static size_t p2_env_size_or(const char *name, size_t fallback) {
	const char *value = getenv(name);
	char *end = NULL;
	unsigned long long parsed;

	if (!value || !*value) {
		return fallback;
	}
	parsed = strtoull(value, &end, 10);
	if (!end || *end != '\0' || parsed == 0ULL) {
		return fallback;
	}
	return (size_t)parsed;
}

static int p2_dataset_init(p2_dataset_t *ds, size_t queries) {
	kvs_flat_index_t flat;
	kvs_vector_hit_t flat_hits[P2_TOPK];
	uint64_t start_us;
	size_t i;
	int failed = 0;

	memset(ds, 0, sizeof(*ds));
	memset(&flat, 0, sizeof(flat));

	ds->count = P2_COUNT;
	ds->queries = queries;
	ds->base = (float *)malloc(ds->count * P2_DIM * sizeof(*ds->base));
	ds->query = (float *)malloc(ds->queries * P2_DIM * sizeof(*ds->query));
	ds->exact = (p2_exact_item_t *)calloc(ds->queries * P2_TOPK, sizeof(*ds->exact));
	ds->exact_counts = (size_t *)calloc(ds->queries, sizeof(*ds->exact_counts));
	if (!ds->base || !ds->query || !ds->exact || !ds->exact_counts) {
		fprintf(stderr, "hnsw_p2_alloc_failed count=%zu queries=%zu\n",
			ds->count, ds->queries);
		return 1;
	}

	for (i = 0; i < ds->count; i++) {
		p2_fill_random_unit_vector(
			ds->base + i * P2_DIM,
			P2_DIM,
			0xabc00000ULL + (uint64_t)i
		);
	}
	for (i = 0; i < ds->queries; i++) {
		p2_fill_random_unit_vector(
			ds->query + i * P2_DIM,
			P2_DIM,
			0xdad00000ULL + (uint64_t)i
		);
	}

	if (kvs_flat_index_init(&flat, P2_DIM, KVS_VECTOR_METRIC_IP) != KVS_VECTOR_OK ||
	    kvs_flat_index_reserve(&flat, ds->count) != KVS_VECTOR_OK) {
		fprintf(stderr, "hnsw_p2_core_flat_init_failed count=%zu\n", ds->count);
		failed = 1;
		goto done;
	}
	for (i = 0; i < ds->count; i++) {
		if (kvs_flat_index_add(
			    &flat,
			    P2_BASE_RECORD_ID + (uint64_t)i,
			    ds->base + i * P2_DIM,
			    P2_DIM
		    ) != KVS_VECTOR_OK) {
			fprintf(stderr, "hnsw_p2_core_flat_add_failed i=%zu\n", i);
			failed = 1;
			goto done;
		}
	}

	start_us = p2_now_us();
	for (i = 0; i < ds->queries; i++) {
		size_t exact_count = 0;
		size_t flat_count = 0;

		p2_direct_exact_topk(
			ds->base,
			ds->count,
			ds->query + i * P2_DIM,
			ds->exact + i * P2_TOPK,
			&exact_count
		);
		ds->exact_counts[i] = exact_count;

		if (kvs_flat_index_search(
			    &flat,
			    ds->query + i * P2_DIM,
			    P2_DIM,
			    P2_TOPK,
			    flat_hits,
			    &flat_count
		    ) != KVS_VECTOR_OK ||
		    !p2_hits_descending(flat_hits, flat_count) ||
		    !p2_exact_same_ids(ds->exact + i * P2_TOPK,
			    exact_count, flat_hits, flat_count)) {
			ds->oracle_mismatch_count++;
		}
	}
	ds->oracle_us = p2_now_us() - start_us;

	printf(
		"hnsw_p2_oracle count=%zu dim=%u topk=%u queries=%zu "
		"metric=ip_normalized direct_exact=1 core_flat_crosscheck=1 "
		"mismatch=%zu oracle_us=%llu\n",
		ds->count,
		P2_DIM,
		P2_TOPK,
		ds->queries,
		ds->oracle_mismatch_count,
		(unsigned long long)ds->oracle_us
	);
	if (ds->oracle_mismatch_count != 0) {
		failed = 1;
	}

done:
	kvs_flat_index_destroy(&flat);
	return failed;
}

static void p2_dataset_destroy(p2_dataset_t *ds) {
	if (!ds) {
		return;
	}
	free(ds->base);
	free(ds->query);
	free(ds->exact);
	free(ds->exact_counts);
	memset(ds, 0, sizeof(*ds));
}

static int p2_run_gate(const p2_dataset_t *ds) {
	kvs_hnsw_v2_index_t index;
	kvs_vector_hit_t *hits = NULL;
	uint64_t *elapsed = NULL;
	uint64_t *visited = NULL;
	uint64_t *distance = NULL;
	uint64_t build_start_us;
	uint64_t build_us;
	double recall_sum = 0.0;
	double recall;
	size_t q;
	size_t i;
	int failed = 0;

	memset(&index, 0, sizeof(index));
	hits = (kvs_vector_hit_t *)malloc(P2_TOPK * sizeof(*hits));
	elapsed = (uint64_t *)malloc(ds->queries * sizeof(*elapsed));
	visited = (uint64_t *)malloc(ds->queries * sizeof(*visited));
	distance = (uint64_t *)malloc(ds->queries * sizeof(*distance));
	if (!hits || !elapsed || !visited || !distance) {
		fprintf(stderr, "hnsw_p2_gate_alloc_failed queries=%zu\n", ds->queries);
		failed = 1;
		goto done;
	}

	if (kvs_hnsw_v2_index_init(&index, P2_DIM, KVS_VECTOR_METRIC_IP) != KVS_VECTOR_OK) {
		fprintf(stderr, "hnsw_p2_v2_init_failed\n");
		failed = 1;
		goto done;
	}
	index.M = P2_M;
	index.M0 = P2_M0;
	index.ef_construction = P2_EF_CONSTRUCTION;
	index.ef_search = P2_EF_SEARCH;
	kvs_hnsw_v2_index_set_seed(&index, 0x9d000000ULL + (uint64_t)ds->count +
		(uint64_t)P2_M * 17ULL + (uint64_t)P2_EF_CONSTRUCTION * 31ULL);
	if (kvs_hnsw_v2_index_reserve(&index, ds->count) != KVS_VECTOR_OK) {
		fprintf(stderr, "hnsw_p2_v2_reserve_failed count=%zu\n", ds->count);
		failed = 1;
		goto done;
	}

	printf(
		"hnsw_p2_build_start backend=core_hnsw_v2 count=%zu dim=%u "
		"profile=balanced M=%u M0=%u efConstruction=%u efSearch=%u\n",
		ds->count,
		P2_DIM,
		P2_M,
		P2_M0,
		P2_EF_CONSTRUCTION,
		P2_EF_SEARCH
	);
	fflush(stdout);

	build_start_us = p2_now_us();
	for (i = 0; i < ds->count; i++) {
		if (kvs_hnsw_v2_index_add(
			    &index,
			    P2_BASE_RECORD_ID + (uint64_t)i,
			    ds->base + i * P2_DIM,
			    P2_DIM
		    ) != KVS_VECTOR_OK) {
			fprintf(stderr, "hnsw_p2_v2_add_failed i=%zu\n", i);
			failed = 1;
			goto done;
		}
	}
	build_us = p2_now_us() - build_start_us;
	kvs_hnsw_v2_graph_stats_refresh(&index);
	kvs_hnsw_v2_memory_refresh(&index);

	printf(
		"hnsw_p2_graph backend=core_hnsw_v2 count=%zu profile=balanced "
		"build_us=%llu memory_bytes=%zu max_level=%u entry_point_level=%u "
		"level0_degree_min=%u level0_degree_avg=%.2f level0_degree_p50=%u "
		"level0_degree_p95=%u level0_degree_max=%u upper_degree_min=%u "
		"upper_degree_avg=%.2f upper_degree_p50=%u upper_degree_p95=%u "
		"upper_degree_max=%u isolated=%llu duplicate_edge=%llu "
		"degree_overflow=%llu selected=%llu reverse_prune=%llu\n",
		ds->count,
		(unsigned long long)build_us,
		index.memory.total_bytes,
		(unsigned)index.graph_stats.max_level,
		index.has_entry ? (unsigned)index.levels[index.entry_point] : 0U,
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
		(unsigned long long)index.graph_stats.isolated_node_count,
		(unsigned long long)index.graph_stats.duplicate_edge_count,
		(unsigned long long)index.graph_stats.degree_overflow_count,
		(unsigned long long)index.graph_stats.selected_neighbor_count,
		(unsigned long long)index.graph_stats.reverse_prune_count
	);

	if (index.graph_stats.duplicate_edge_count != 0 ||
	    index.graph_stats.degree_overflow_count != 0) {
		failed = 1;
		goto done;
	}

	memset(&index.search_stats, 0, sizeof(index.search_stats));
	for (q = 0; q < ds->queries; q++) {
		size_t hit_count = 0;
		uint64_t start_us = p2_now_us();
		int ret = kvs_hnsw_v2_index_search(
			&index,
			ds->query + q * P2_DIM,
			P2_DIM,
			P2_TOPK,
			P2_EF_SEARCH,
			hits,
			&hit_count
		);
		elapsed[q] = p2_now_us() - start_us;
		if (ret != KVS_VECTOR_OK || !p2_hits_descending(hits, hit_count)) {
			fprintf(stderr, "hnsw_p2_v2_search_failed q=%zu ret=%d\n", q, ret);
			failed = 1;
			goto done;
		}
		visited[q] = index.search_stats.last_visited_count;
		distance[q] = index.search_stats.last_distance_compute_count;
		recall_sum += p2_recall_hits(
			ds->exact + q * P2_TOPK,
			ds->exact_counts[q],
			hits,
			hit_count
		);
	}

	recall = recall_sum / (double)ds->queries;
	printf(
		"hnsw_p2_result backend=core_hnsw_v2 count=%zu dim=%u topk=%u "
		"queries=%zu profile=balanced M=%u M0=%u efConstruction=%u "
		"efSearch=%u recall@10=%.4f build_us=%llu memory_bytes=%zu "
		"search_avg_us=%.2f search_p50_us=%llu search_p95_us=%llu "
		"visited_avg=%.2f distance_avg=%.2f level0_degree_avg=%.2f "
		"upper_degree_avg=%.2f duplicate_edge=%llu degree_overflow=%llu "
		"oracle_mismatch=%zu\n",
		ds->count,
		P2_DIM,
		P2_TOPK,
		ds->queries,
		P2_M,
		P2_M0,
		P2_EF_CONSTRUCTION,
		P2_EF_SEARCH,
		recall,
		(unsigned long long)build_us,
		index.memory.total_bytes,
		p2_avg_u64(elapsed, ds->queries),
		(unsigned long long)p2_percentile_u64(elapsed, ds->queries, 50.0),
		(unsigned long long)p2_percentile_u64(elapsed, ds->queries, 95.0),
		p2_avg_u64(visited, ds->queries),
		p2_avg_u64(distance, ds->queries),
		index.graph_stats.level0_degree.avg,
		index.graph_stats.upper_degree.avg,
		(unsigned long long)index.graph_stats.duplicate_edge_count,
		(unsigned long long)index.graph_stats.degree_overflow_count,
		ds->oracle_mismatch_count
	);

	printf(
		"hnsw_p2_decision 100k_balanced_gate=%s recall_at_10=%.4f "
		"default_param_candidate=%s build_cost_risk=high next_stage=%s\n",
		(recall + 0.000001 >= 0.95) ? "pass" : "fail",
		recall,
		(recall + 0.000001 >= 0.95) ? "balanced" : "unchanged",
		(recall + 0.000001 >= 0.95) ? "HNSW-P5" : "high_recall-gate"
	);

	if (recall + 0.000001 < 0.95) {
		failed = 1;
	}

done:
	kvs_hnsw_v2_index_destroy(&index);
	free(hits);
	free(elapsed);
	free(visited);
	free(distance);
	return failed;
}

int main(void) {
	p2_dataset_t ds;
	size_t queries;
	int failed = 0;

	memset(&g_cfg, 0, sizeof(g_cfg));
	kvs_config_init_default(&g_cfg);
	g_cfg.mem_backend = KVS_MEM_BACKEND_SYSTEM;
	snprintf(g_cfg.vector_distance_backend, sizeof(g_cfg.vector_distance_backend), "%s", "auto");
	mem_set_backend(KVS_MEM_BACKEND_SYSTEM);

	queries = p2_env_size_or("HNSW_P2_QUERIES", P2_DEFAULT_QUERIES);
	printf(
		"hnsw_p2_start count=%u dim=%u topk=%u queries=%zu "
		"metric=ip_normalized M=%u M0=%u efConstruction=%u efSearch=%u seed=fixed\n",
		P2_COUNT,
		P2_DIM,
		P2_TOPK,
		queries,
		P2_M,
		P2_M0,
		P2_EF_CONSTRUCTION,
		P2_EF_SEARCH
	);
	fflush(stdout);

	if (p2_dataset_init(&ds, queries) != 0) {
		p2_dataset_destroy(&ds);
		fprintf(stderr, "vector_core_hnsw_v2_100k_balanced_gate_test FAILED\n");
		return 1;
	}
	if (ds.oracle_mismatch_count != 0) {
		p2_dataset_destroy(&ds);
		fprintf(stderr, "vector_core_hnsw_v2_100k_balanced_gate_test FAILED\n");
		return 1;
	}

	failed = p2_run_gate(&ds);
	p2_dataset_destroy(&ds);

	if (failed) {
		fprintf(stderr, "vector_core_hnsw_v2_100k_balanced_gate_test FAILED\n");
		return 1;
	}

	printf("vector_core_hnsw_v2_100k_balanced_gate_test PASS\n");
	return 0;
}
