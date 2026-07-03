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

#define SCALE_DIM 512
#define SCALE_TOPK 10

typedef struct exact_item_s {
	uint64_t record_id;
	float score;
} exact_item_t;

typedef struct query_profile_s {
	uint64_t search_us;
	uint64_t visited;
	uint64_t distance;
} query_profile_t;

typedef struct scale_case_s {
	size_t count;
	size_t queries;
	const char *mode;
	uint64_t build_threshold_us;
	int enforce_recall_gate;
} scale_case_t;

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

static void fill_random_unit_vector(float *vector, size_t dim, uint64_t seed) {
	uint64_t state = seed;
	float norm = 0.0f;
	size_t i;

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

static float exact_score_ip(const float *lhs, const float *rhs, size_t dim) {
	float sum = 0.0f;
	size_t i;

	for (i = 0; i < dim; i++) {
		sum += lhs[i] * rhs[i];
	}
	return sum;
}

static void exact_topk_ip(
	const float *base,
	size_t count,
	size_t dim,
	const float *query,
	size_t topk,
	exact_item_t *out,
	size_t *out_count
) {
	size_t n = 0;
	size_t i;

	*out_count = 0;
	for (i = 0; i < count; i++) {
		float score = exact_score_ip(query, base + i * dim, dim);
		exact_item_t item;
		size_t pos;

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
	size_t hit = 0;
	size_t i;
	size_t j;

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

static int hits_descending(const kvs_vector_hit_t *hits, size_t count) {
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

static uint64_t percentile_u64(uint64_t *values, size_t count, double pct) {
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

static double avg_u64(const uint64_t *values, size_t count) {
	uint64_t sum = 0;
	size_t i;

	if (!values || count == 0) {
		return 0.0;
	}
	for (i = 0; i < count; i++) {
		sum += values[i];
	}
	return (double)sum / (double)count;
}

static int run_search_case(
	kvs_hnsw_v2_index_t *index,
	const float *base,
	size_t count,
	size_t dim,
	size_t topk,
	size_t queries,
	size_t ef_search,
	double *out_recall
) {
	query_profile_t *profiles;
	uint64_t *scratch;
	kvs_vector_hit_t *hits;
	exact_item_t *exact;
	float *query;
	double recall_sum = 0.0;
	int failed = 0;
	size_t q;

	profiles = (query_profile_t *)calloc(queries, sizeof(*profiles));
	scratch = (uint64_t *)malloc(queries * sizeof(*scratch));
	hits = (kvs_vector_hit_t *)malloc(topk * sizeof(*hits));
	exact = (exact_item_t *)calloc(topk, sizeof(*exact));
	query = (float *)malloc(dim * sizeof(*query));
	if (!profiles || !scratch || !hits || !exact || !query) {
		failed = 1;
		goto done;
	}

	memset(&index->search_stats, 0, sizeof(index->search_stats));
	for (q = 0; q < queries; q++) {
		size_t exact_count = 0;
		size_t hit_count = 0;
		uint64_t start_us;
		int ret;

		fill_random_unit_vector(query, dim, 0xdad00000ULL + q);
		exact_topk_ip(base, count, dim, query, topk, exact, &exact_count);

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
			fprintf(stderr, "scale search failed count=%zu ef=%zu q=%zu ret=%d\n",
				count, ef_search, q, ret);
			failed = 1;
			break;
		}
		if (!hits_descending(hits, hit_count)) {
			fprintf(stderr, "scale hits not descending count=%zu ef=%zu q=%zu\n",
				count, ef_search, q);
			failed = 1;
			break;
		}
		profiles[q].visited = index->search_stats.last_visited_count;
		profiles[q].distance = index->search_stats.last_distance_compute_count;
		recall_sum += recall_at_k(exact, exact_count, hits, hit_count);
	}

	if (!failed) {
		double recall = recall_sum / (double)queries;
		uint64_t *search_values = scratch;
		uint64_t *visited_values = (uint64_t *)malloc(queries * sizeof(uint64_t));
		uint64_t *distance_values = (uint64_t *)malloc(queries * sizeof(uint64_t));
		size_t i;

		if (!visited_values || !distance_values) {
			free(visited_values);
			free(distance_values);
			failed = 1;
			goto done;
		}
		for (i = 0; i < queries; i++) {
			search_values[i] = profiles[i].search_us;
			visited_values[i] = profiles[i].visited;
			distance_values[i] = profiles[i].distance;
		}
		printf(
			"hnsw_v2_scale_result count=%zu dim=%zu topk=%zu queries=%zu "
			"ef=%zu recall@%zu=%.4f search_avg_us=%.2f "
			"search_p50_us=%llu search_p95_us=%llu visited_avg=%.2f "
			"distance_avg=%.2f\n",
			count,
			dim,
			topk,
			queries,
			ef_search,
			topk,
			recall,
			avg_u64(search_values, queries),
			(unsigned long long)percentile_u64(search_values, queries, 50.0),
			(unsigned long long)percentile_u64(search_values, queries, 95.0),
			avg_u64(visited_values, queries),
			avg_u64(distance_values, queries)
		);
		*out_recall = recall;
		free(visited_values);
		free(distance_values);
	}

done:
	free(profiles);
	free(scratch);
	free(hits);
	free(exact);
	free(query);
	return failed;
}

static int run_scale_case(const scale_case_t *tc) {
	static const size_t efs[] = {128, 256, 512};
	kvs_hnsw_v2_index_t index;
	float *base = NULL;
	double recalls[3] = {0.0, 0.0, 0.0};
	uint64_t build_start_us;
	uint64_t build_us;
	size_t i;
	int failed = 0;

	memset(&index, 0, sizeof(index));
	base = (float *)malloc(tc->count * SCALE_DIM * sizeof(*base));
	if (!base) {
		fprintf(stderr, "scale base allocation failed count=%zu\n", tc->count);
		return 1;
	}
	if (kvs_hnsw_v2_index_init(&index, SCALE_DIM, KVS_VECTOR_METRIC_IP) !=
	    KVS_VECTOR_OK) {
		fprintf(stderr, "scale init failed count=%zu\n", tc->count);
		failed = 1;
		goto done;
	}
	index.M = KVS_HNSW_V2_DEFAULT_M;
	index.M0 = KVS_HNSW_V2_DEFAULT_M0;
	index.ef_construction = KVS_HNSW_V2_DEFAULT_EF_CONSTRUCTION;
	index.ef_search = KVS_HNSW_V2_DEFAULT_EF_SEARCH;
	kvs_hnsw_v2_index_set_seed(&index, 0x9d000000ULL + tc->count);
	if (kvs_hnsw_v2_index_reserve(&index, tc->count) != KVS_VECTOR_OK) {
		fprintf(stderr, "scale reserve failed count=%zu\n", tc->count);
		failed = 1;
		goto done;
	}

	printf(
		"hnsw_v2_scale_build_start count=%zu dim=%u mode=%s queries=%zu\n",
		tc->count,
		SCALE_DIM,
		tc->mode,
		tc->queries
	);
	fflush(stdout);
	build_start_us = now_us();
	for (i = 0; i < tc->count; i++) {
		fill_random_unit_vector(base + i * SCALE_DIM, SCALE_DIM,
			0xabc00000ULL + i);
		if (kvs_hnsw_v2_index_add(
			    &index,
			    1000000ULL + i,
			    base + i * SCALE_DIM,
			    SCALE_DIM
		    ) != KVS_VECTOR_OK) {
			fprintf(stderr, "scale add failed count=%zu i=%zu\n",
				tc->count, i);
			failed = 1;
			goto done;
		}
	}
	build_us = now_us() - build_start_us;
	kvs_hnsw_v2_graph_stats_refresh(&index);
	kvs_hnsw_v2_memory_refresh(&index);

	printf(
		"hnsw_v2_scale_build count=%zu dim=%u mode=%s build_us=%llu "
		"memory_bytes=%zu deleted_count=%zu active_count=%zu "
		"level0_degree_avg=%.2f duplicate_edge=%llu degree_overflow=%llu "
		"build_threshold_us=%llu build_over_threshold=%s\n",
		tc->count,
		SCALE_DIM,
		tc->mode,
		(unsigned long long)build_us,
		index.memory.total_bytes,
		index.deleted_count,
		index.size - index.deleted_count,
		index.graph_stats.level0_degree.avg,
		(unsigned long long)index.graph_stats.duplicate_edge_count,
		(unsigned long long)index.graph_stats.degree_overflow_count,
		(unsigned long long)tc->build_threshold_us,
		build_us > tc->build_threshold_us ? "yes" : "no"
	);

	if (index.deleted_count != 0 ||
	    index.size != tc->count ||
	    index.graph_stats.duplicate_edge_count != 0 ||
	    index.graph_stats.degree_overflow_count != 0) {
		fprintf(stderr, "scale graph invariant failed count=%zu\n", tc->count);
		failed = 1;
		goto done;
	}

	for (i = 0; i < sizeof(efs) / sizeof(efs[0]); i++) {
		if (run_search_case(
			    &index,
			    base,
			    tc->count,
			    SCALE_DIM,
			    SCALE_TOPK,
			    tc->queries,
			    efs[i],
			    &recalls[i]
		    ) != 0) {
			failed = 1;
			goto done;
		}
	}

	printf(
		"hnsw_v2_scale_sensitivity count=%zu dim=%u ef128=%.4f "
		"ef256=%.4f ef512=%.4f monotonic=%s\n",
		tc->count,
		SCALE_DIM,
		recalls[0],
		recalls[1],
		recalls[2],
		(recalls[0] <= recalls[1] + 0.000001 &&
		 recalls[1] <= recalls[2] + 0.000001) ? "yes" : "no"
	);

	if (tc->enforce_recall_gate) {
		if (recalls[1] + 0.000001 < 0.98 ||
		    recalls[2] + 0.000001 < 0.99 ||
		    recalls[0] > recalls[1] + 0.000001 ||
		    recalls[1] > recalls[2] + 0.000001) {
			fprintf(stderr,
				"scale recall gate failed count=%zu ef128=%.4f ef256=%.4f ef512=%.4f\n",
				tc->count, recalls[0], recalls[1], recalls[2]);
			failed = 1;
		}
	}

done:
	kvs_hnsw_v2_index_destroy(&index);
	free(base);
	return failed;
}

int main(void) {
	static const scale_case_t cases[] = {
		{10000, 50, "full", 120000000ULL, 1},
		{50000, 20, "sampled", 600000000ULL, 0},
		{100000, 5, "smoke", 1800000000ULL, 0}
	};
	size_t i;
	int failed = 0;

	memset(&g_cfg, 0, sizeof(g_cfg));
	kvs_config_init_default(&g_cfg);
	g_cfg.mem_backend = KVS_MEM_BACKEND_SYSTEM;
	snprintf(g_cfg.vector_distance_backend,
		sizeof(g_cfg.vector_distance_backend), "%s", "auto");
	mem_set_backend(KVS_MEM_BACKEND_SYSTEM);

	for (i = 0; i < sizeof(cases) / sizeof(cases[0]); i++) {
		failed |= run_scale_case(&cases[i]);
		if (failed) {
			break;
		}
	}

	if (failed) {
		fprintf(stderr, "vector_core_hnsw_v2_scale_stability_test FAILED\n");
		return 1;
	}
	printf("vector_core_hnsw_v2_scale_stability_test PASS\n");
	return 0;
}
