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

static int expect_int(int cond, const char *label) {
	if (!cond) {
		fprintf(stderr, "FAIL: %s\n", label);
		return 1;
	}
	return 0;
}

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

static float exact_metric_score(
	int metric,
	const float *lhs,
	const float *rhs,
	size_t dim
) {
	if (metric == KVS_VECTOR_METRIC_L2) {
		size_t i;
		float sum = 0.0f;

		for (i = 0; i < dim; i++) {
			float diff = lhs[i] - rhs[i];
			sum += diff * diff;
		}
		return -sum;
	}
	if (metric == KVS_VECTOR_METRIC_COSINE) {
		size_t i;
		float dot = 0.0f;
		float ln = 0.0f;
		float rn = 0.0f;

		for (i = 0; i < dim; i++) {
			dot += lhs[i] * rhs[i];
			ln += lhs[i] * lhs[i];
			rn += rhs[i] * rhs[i];
		}
		if (ln <= 0.0f || rn <= 0.0f) {
			return 0.0f;
		}
		return dot / (sqrtf(ln) * sqrtf(rn));
	}
	return exact_score_ip(lhs, rhs, dim);
}

static void sort_exact_desc(
	exact_item_t *items,
	size_t count
) {
	size_t i;

	for (i = 1; i < count; i++) {
		exact_item_t item = items[i];
		size_t j = i;

		while (j > 0 && items[j - 1].score < item.score) {
			items[j] = items[j - 1];
			j--;
		}
		items[j] = item;
	}
}

static void exact_topk(
	const float *base,
	size_t count,
	size_t dim,
	int metric,
	const uint8_t *deleted,
	const float *query,
	size_t topk,
	exact_item_t *out,
	size_t *out_count
) {
	exact_item_t *items;
	size_t i;
	size_t n = 0;

	*out_count = 0;
	items = (exact_item_t *)malloc(count * sizeof(exact_item_t));
	if (!items) {
		return;
	}

	for (i = 0; i < count; i++) {
		if (deleted && deleted[i]) {
			continue;
		}
		items[n].record_id = 1000000ULL + i;
		items[n].score = exact_metric_score(
			metric,
			query,
			base + i * dim,
			dim
		);
		n++;
	}

	sort_exact_desc(items, n);
	if (topk > n) {
		topk = n;
	}
	memcpy(out, items, topk * sizeof(exact_item_t));
	*out_count = topk;
	free(items);
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

static int test_basic_search(void) {
	kvs_hnsw_v2_index_t index;
	kvs_vector_hit_t hits[8];
	float vector[4] = {1.0f, 0.0f, 0.0f, 0.0f};
	float query[4] = {1.0f, 0.0f, 0.0f, 0.0f};
	size_t out_count = 99;
	int failed = 0;

	memset(&index, 0, sizeof(index));
	failed |= expect_int(
		kvs_hnsw_v2_index_init(&index, 4, KVS_VECTOR_METRIC_IP) == KVS_VECTOR_OK,
		"basic init"
	);
	index.M = 4;
	index.M0 = 8;
	failed |= expect_int(kvs_hnsw_v2_index_reserve(&index, 8) == KVS_VECTOR_OK, "basic reserve");
	failed |= expect_int(
		kvs_hnsw_v2_index_search(&index, query, 4, 3, 16, hits, &out_count) ==
			KVS_VECTOR_OK,
		"empty search ok"
	);
	failed |= expect_int(out_count == 0, "empty search count");

	failed |= expect_int(
		kvs_hnsw_v2_index_add(&index, 7, vector, 4) == KVS_VECTOR_OK,
		"single add"
	);
	failed |= expect_int(
		kvs_hnsw_v2_index_search(&index, query, 4, 3, 1, hits, &out_count) ==
			KVS_VECTOR_OK,
		"single search"
	);
	failed |= expect_int(out_count == 1, "single count");
	failed |= expect_int(hits[0].record_id == 7, "single record id");
	failed |= expect_int(hits[0].score > 0.99f, "single score");
	failed |= expect_int(
		kvs_hnsw_v2_index_search(&index, query, 3, 1, 1, hits, &out_count) ==
			KVS_VECTOR_ERR_INVALID_ARG,
		"dim mismatch reject"
	);

	kvs_hnsw_v2_index_destroy(&index);
	return failed;
}

static int test_deleted_and_order(void) {
	kvs_hnsw_v2_index_t index;
	kvs_vector_hit_t hits[8];
	float vectors[4][4] = {
		{1.0f, 0.0f, 0.0f, 0.0f},
		{0.9f, 0.1f, 0.0f, 0.0f},
		{0.8f, 0.2f, 0.0f, 0.0f},
		{-1.0f, 0.0f, 0.0f, 0.0f}
	};
	float query[4] = {1.0f, 0.0f, 0.0f, 0.0f};
	size_t out_count = 0;
	size_t i;
	int failed = 0;

	memset(&index, 0, sizeof(index));
	failed |= expect_int(
		kvs_hnsw_v2_index_init(&index, 4, KVS_VECTOR_METRIC_IP) == KVS_VECTOR_OK,
		"deleted init"
	);
	index.M = 4;
	index.M0 = 8;
	kvs_hnsw_v2_index_set_seed(&index, 42);
	failed |= expect_int(kvs_hnsw_v2_index_reserve(&index, 4) == KVS_VECTOR_OK, "deleted reserve");
	for (i = 0; i < 4; i++) {
		failed |= expect_int(
			kvs_hnsw_v2_index_add(&index, 100 + i, vectors[i], 4) ==
				KVS_VECTOR_OK,
			"deleted add"
		);
	}
	index.deleted_flags[0] = 1;
	index.deleted_count = 1;
	failed |= expect_int(
		kvs_hnsw_v2_index_search(&index, query, 4, 10, 16, hits, &out_count) ==
			KVS_VECTOR_OK,
		"deleted search"
	);
	failed |= expect_int(out_count == 3, "topk larger than active size");
	failed |= expect_int(hits_descending(hits, out_count), "result descending");
	for (i = 0; i < out_count; i++) {
		failed |= expect_int(hits[i].record_id != 100, "deleted result skipped");
	}

	kvs_hnsw_v2_index_destroy(&index);
	return failed;
}

static int run_recall_queries(
	kvs_hnsw_v2_index_t *index,
	const float *base,
	size_t count,
	size_t dim,
	size_t topk,
	size_t queries,
	size_t ef_search,
	double *out_recall,
	uint64_t *out_search_avg_us
) {
	float *query;
	exact_item_t *exact;
	kvs_vector_hit_t *hits;
	size_t q;
	double recall_sum = 0.0;
	uint64_t search_total_us = 0;
	int failed = 0;

	query = (float *)malloc(dim * sizeof(float));
	exact = (exact_item_t *)malloc(topk * sizeof(exact_item_t));
	hits = (kvs_vector_hit_t *)malloc(topk * sizeof(kvs_vector_hit_t));
	if (!query || !exact || !hits) {
		free(query);
		free(exact);
		free(hits);
		return 1;
	}

	for (q = 0; !failed && q < queries; q++) {
		size_t exact_count = 0;
		size_t hit_count = 0;
		uint64_t start;
		int ret;

		fill_random_unit_vector(query, dim, 0xdad00000ULL + q);
		exact_topk(
			base,
			count,
			dim,
			KVS_VECTOR_METRIC_IP,
			index->deleted_flags,
			query,
			topk,
			exact,
			&exact_count
		);
		start = now_us();
		ret = kvs_hnsw_v2_index_search(
			index,
			query,
			dim,
			topk,
			ef_search,
			hits,
			&hit_count
		);
		search_total_us += now_us() - start;
		if (ret != KVS_VECTOR_OK) {
			fprintf(stderr, "search failed count=%zu q=%zu ret=%d\n", count, q, ret);
			failed = 1;
			break;
		}
		if (!hits_descending(hits, hit_count)) {
			fprintf(stderr, "hits not descending count=%zu q=%zu\n", count, q);
			failed = 1;
			break;
		}
		recall_sum += recall_at_k(exact, exact_count, hits, hit_count);
	}

	if (!failed) {
		*out_recall = recall_sum / (double)queries;
		*out_search_avg_us = queries ? search_total_us / queries : 0;
	}

	free(query);
	free(exact);
	free(hits);
	return failed;
}

static int run_recall_case(
	size_t count,
	size_t dim,
	size_t topk,
	size_t queries,
	size_t ef_search,
	double *out_recall,
	uint64_t *out_search_avg_us
) {
	kvs_hnsw_v2_index_t index;
	float *base;
	size_t i;
	int failed = 0;
	uint64_t build_start_us;
	uint64_t build_us = 0;

	memset(&index, 0, sizeof(index));
	base = (float *)malloc(count * dim * sizeof(float));
	if (!base) {
		return 1;
	}

	failed |= expect_int(
		kvs_hnsw_v2_index_init(&index, dim, KVS_VECTOR_METRIC_IP) == KVS_VECTOR_OK,
		"recall init"
	);
	index.M = KVS_HNSW_V2_DEFAULT_M;
	index.M0 = KVS_HNSW_V2_DEFAULT_M0;
	index.ef_construction = KVS_HNSW_V2_DEFAULT_EF_CONSTRUCTION;
	index.ef_search = KVS_HNSW_V2_DEFAULT_EF_SEARCH;
	kvs_hnsw_v2_index_set_seed(&index, 0x9d000000ULL + count);
	failed |= expect_int(kvs_hnsw_v2_index_reserve(&index, count) == KVS_VECTOR_OK, "recall reserve");

	printf("hnsw_v2_build_start count=%zu dim=%zu\n", count, dim);
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
			fprintf(stderr, "recall add failed i=%zu count=%zu\n", i, count);
			failed = 1;
			break;
		}
	}
	build_us = now_us() - build_start_us;
	printf("hnsw_v2_build_done count=%zu dim=%zu build_us=%llu\n", count, dim, (unsigned long long)build_us);
	fflush(stdout);

	kvs_hnsw_v2_graph_stats_refresh(&index);
	if (!failed) {
		failed = run_recall_queries(
			&index,
			base,
			count,
			dim,
			topk,
			queries,
			ef_search,
			out_recall,
			out_search_avg_us
		);
	}

	if (!failed) {
		printf(
			"hnsw_v2_recall count=%zu dim=%zu topk=%zu queries=%zu ef=%zu "
			"recall@%zu=%.4f build_us=%llu search_avg_us=%llu "
			"visited_avg=%.2f distance_avg=%.2f candidate_push=%llu "
			"candidate_pop=%llu candidate_skip=%llu top_push=%llu "
			"top_replace=%llu early_stop=%llu deleted_skip=%llu "
			"level0_degree=min:%u avg:%.2f p50:%u p95:%u max:%u "
			"isolated=%llu memory_bytes=%zu\n",
			count,
			dim,
			topk,
			queries,
			ef_search,
			topk,
			*out_recall,
			(unsigned long long)build_us,
			(unsigned long long)*out_search_avg_us,
			index.search_stats.search_query_count ?
				(double)index.search_stats.search_visited_count_total /
					(double)index.search_stats.search_query_count : 0.0,
			index.search_stats.search_query_count ?
				(double)index.search_stats.search_distance_compute_count_total /
					(double)index.search_stats.search_query_count : 0.0,
			(unsigned long long)index.search_stats.search_candidate_push_count,
			(unsigned long long)index.search_stats.search_candidate_pop_count,
			(unsigned long long)index.search_stats.search_candidate_skip_count,
			(unsigned long long)index.search_stats.search_top_push_count,
			(unsigned long long)index.search_stats.search_top_replace_count,
			(unsigned long long)index.search_stats.search_early_stop_count,
			(unsigned long long)index.search_stats.search_deleted_skip_count,
			index.graph_stats.level0_degree.min,
			index.graph_stats.level0_degree.avg,
			index.graph_stats.level0_degree.p50,
			index.graph_stats.level0_degree.p95,
			index.graph_stats.level0_degree.max,
			(unsigned long long)index.graph_stats.isolated_node_count,
			index.memory.total_bytes
		);
	}

	kvs_hnsw_v2_index_destroy(&index);
	free(base);
	return failed;
}

static int test_recall_gate(void) {
	double recall = 0.0;
	uint64_t search_avg_us = 0;
	int failed = 0;

	failed |= run_recall_case(1000, 512, 10, 50, 256, &recall, &search_avg_us);
	failed |= expect_int(recall >= 0.99, "1K recall gate");
	failed |= run_recall_case(5000, 512, 10, 50, 256, &recall, &search_avg_us);
	failed |= expect_int(recall >= 0.95, "5K recall gate");
	return failed;
}

static int test_ef_sensitivity(void) {
	size_t efs[] = {64, 128, 256, 512, 1024};
	kvs_hnsw_v2_index_t index;
	float *base;
	double prev = -1.0;
	int failed = 0;
	size_t i;
	size_t count = 10000;
	size_t dim = 512;
	size_t topk = 10;
	size_t queries = 50;
	uint64_t build_start_us;
	uint64_t build_us;

	memset(&index, 0, sizeof(index));
	base = (float *)malloc(count * dim * sizeof(float));
	if (!base) {
		return 1;
	}

	failed |= expect_int(
		kvs_hnsw_v2_index_init(&index, dim, KVS_VECTOR_METRIC_IP) == KVS_VECTOR_OK,
		"ef init"
	);
	index.M = KVS_HNSW_V2_DEFAULT_M;
	index.M0 = KVS_HNSW_V2_DEFAULT_M0;
	index.ef_construction = KVS_HNSW_V2_DEFAULT_EF_CONSTRUCTION;
	index.ef_search = KVS_HNSW_V2_DEFAULT_EF_SEARCH;
	kvs_hnsw_v2_index_set_seed(&index, 0x9d000000ULL + count);
	failed |= expect_int(kvs_hnsw_v2_index_reserve(&index, count) == KVS_VECTOR_OK, "ef reserve");
	printf("hnsw_v2_build_start count=%zu dim=%zu ef_sensitivity=1\n", count, dim);
	fflush(stdout);
	build_start_us = now_us();
	for (i = 0; !failed && i < count; i++) {
		fill_random_unit_vector(base + i * dim, dim, 0xabc00000ULL + i);
		if (kvs_hnsw_v2_index_add(
			    &index,
			    1000000ULL + i,
			    base + i * dim,
			    dim
		    ) != KVS_VECTOR_OK) {
			fprintf(stderr, "ef add failed i=%zu\n", i);
			failed = 1;
		}
	}
	build_us = now_us() - build_start_us;
	printf(
		"hnsw_v2_build_done count=%zu dim=%zu ef_sensitivity=1 build_us=%llu\n",
		count,
		dim,
		(unsigned long long)build_us
	);
	fflush(stdout);
	kvs_hnsw_v2_graph_stats_refresh(&index);

	for (i = 0; !failed && i < sizeof(efs) / sizeof(efs[0]); i++) {
		double recall = 0.0;
		uint64_t search_avg_us = 0;

		failed |= run_recall_queries(
			&index,
			base,
			count,
			dim,
			topk,
			queries,
			efs[i],
			&recall,
			&search_avg_us
		);
		printf(
			"hnsw_v2_ef_sensitivity count=10000 dim=512 topk=10 queries=50 ef=%zu recall@10=%.4f search_avg_us=%llu\n",
			efs[i],
			recall,
			(unsigned long long)search_avg_us
		);
		if (prev >= 0.0 && recall + 0.000001 < prev) {
			fprintf(
				stderr,
				"FAIL: ef recall decreased prev=%.4f current=%.4f ef=%zu\n",
				prev,
				recall,
				efs[i]
			);
			failed = 1;
		}
		if (efs[i] == 256) {
			failed |= expect_int(recall >= 0.98, "10K ef=256 recall gate");
		}
		if (efs[i] == 512) {
			failed |= expect_int(recall >= 0.99, "10K ef=512 recall gate");
		}
		prev = recall;
	}

	kvs_hnsw_v2_index_destroy(&index);
	free(base);
	return failed;
}

int main(void) {
	int failed = 0;

	memset(&g_cfg, 0, sizeof(g_cfg));
	kvs_config_init_default(&g_cfg);
	g_cfg.mem_backend = KVS_MEM_BACKEND_SYSTEM;
	snprintf(g_cfg.vector_distance_backend, sizeof(g_cfg.vector_distance_backend), "%s", "auto");
	mem_set_backend(KVS_MEM_BACKEND_SYSTEM);

	failed |= test_basic_search();
	failed |= test_deleted_and_order();
	failed |= test_recall_gate();
	failed |= test_ef_sensitivity();

	if (failed) {
		return 1;
	}

	printf("vector_core_hnsw_v2_search_recall_test PASS\n");
	return 0;
}
