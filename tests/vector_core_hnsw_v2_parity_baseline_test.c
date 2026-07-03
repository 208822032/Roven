#include "kvs_config.h"
#include "kvs_memory.h"
#include "kvs_memory_internal.h"
#include "vector/kvs_vector_flat.h"
#include "vector/kvs_vector_hnsw_v2.h"

#ifdef ENABLE_HNSWLIB
#include "kvs_vector_hnswlib_backend.h"
#endif

#include <math.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/time.h>

extern kvs_config_t g_cfg;

#define P1_DIM 512
#define P1_TOPK 10
#define P1_BASE_RECORD_ID 1000000ULL
#define P1_MAX_EFS 3

typedef struct p1_exact_item_s {
	uint64_t record_id;
	float score;
} p1_exact_item_t;

typedef struct p1_dataset_s {
	size_t count;
	size_t queries;
	float *base;
	float *query;
	p1_exact_item_t *exact;
	size_t *exact_counts;
	size_t oracle_mismatch_count;
	uint64_t oracle_us;
} p1_dataset_t;

typedef struct p1_profile_s {
	const char *name;
	size_t M;
	size_t M0;
	size_t ef_construction;
	size_t efs[P1_MAX_EFS];
	size_t ef_count;
} p1_profile_t;

typedef struct p1_query_stats_s {
	uint64_t elapsed_us;
	uint64_t visited;
	uint64_t distance;
} p1_query_stats_t;

static uint64_t p1_now_us(void) {
	struct timeval tv;

	gettimeofday(&tv, NULL);
	return (uint64_t)tv.tv_sec * 1000000ULL + (uint64_t)tv.tv_usec;
}

static uint32_t p1_rng_next(uint64_t *state) {
	uint64_t z;

	*state += 0x9e3779b97f4a7c15ULL;
	z = *state;
	z = (z ^ (z >> 30)) * 0xbf58476d1ce4e5b9ULL;
	z = (z ^ (z >> 27)) * 0x94d049bb133111ebULL;
	z ^= z >> 31;
	return (uint32_t)(z >> 32);
}

static void p1_fill_random_unit_vector(float *vector, size_t dim, uint64_t seed) {
	uint64_t state = seed;
	float norm = 0.0f;
	size_t i;

	for (i = 0; i < dim; i++) {
		uint32_t raw = p1_rng_next(&state);
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

static float p1_ip_score(const float *lhs, const float *rhs, size_t dim) {
	float score = 0.0f;
	size_t i;

	for (i = 0; i < dim; i++) {
		score += lhs[i] * rhs[i];
	}
	return score;
}

static int p1_exact_better(
	float lhs_score,
	uint64_t lhs_id,
	float rhs_score,
	uint64_t rhs_id
) {
	if (lhs_score > rhs_score) {
		return 1;
	}
	if (lhs_score < rhs_score) {
		return 0;
	}
	return lhs_id < rhs_id;
}

static void p1_direct_exact_topk(
	const float *base,
	size_t count,
	const float *query,
	p1_exact_item_t *out,
	size_t *out_count
) {
	size_t n = 0;
	size_t i;

	for (i = 0; i < count; i++) {
		float score = p1_ip_score(query, base + i * P1_DIM, P1_DIM);
		uint64_t record_id = P1_BASE_RECORD_ID + (uint64_t)i;
		size_t pos;

		if (n == P1_TOPK &&
		    !p1_exact_better(score, record_id, out[n - 1].score, out[n - 1].record_id)) {
			continue;
		}

		if (n < P1_TOPK) {
			n++;
		}
		pos = n - 1;
		while (pos > 0 &&
		       p1_exact_better(score, record_id, out[pos - 1].score, out[pos - 1].record_id)) {
			out[pos] = out[pos - 1];
			pos--;
		}
		out[pos].record_id = record_id;
		out[pos].score = score;
	}

	*out_count = n;
}

static int p1_hits_descending(const kvs_vector_hit_t *hits, size_t count) {
	size_t i;

	for (i = 1; i < count; i++) {
		if (hits[i - 1].score < hits[i].score) {
			return 0;
		}
	}
	return 1;
}

static int p1_external_descending(const size_t *ids, const float *scores, size_t count) {
	size_t i;

	(void)ids;
	for (i = 1; i < count; i++) {
		if (scores[i - 1] < scores[i]) {
			return 0;
		}
	}
	return 1;
}

static double p1_recall_hits(
	const p1_exact_item_t *exact,
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

static double p1_recall_external(
	const p1_exact_item_t *exact,
	size_t exact_count,
	const size_t *ids,
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
			if ((uint64_t)ids[j] == exact[i].record_id) {
				matched++;
				break;
			}
		}
	}
	return (double)matched / (double)exact_count;
}

static int p1_exact_same_ids(
	const p1_exact_item_t *exact,
	size_t exact_count,
	const kvs_vector_hit_t *flat,
	size_t flat_count
) {
	size_t i;

	if (exact_count != flat_count) {
		return 0;
	}
	for (i = 0; i < exact_count; i++) {
		if (exact[i].record_id != flat[i].record_id) {
			return 0;
		}
	}
	return 1;
}

static int p1_cmp_u64(const void *lhs, const void *rhs) {
	uint64_t a = *(const uint64_t *)lhs;
	uint64_t b = *(const uint64_t *)rhs;

	return (a > b) - (a < b);
}

static uint64_t p1_percentile_u64(const uint64_t *values, size_t count, double pct) {
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
	qsort(copy, count, sizeof(*copy), p1_cmp_u64);
	idx = (size_t)((pct / 100.0) * (double)count);
	if (idx >= count) {
		idx = count - 1;
	}
	value = copy[idx];
	free(copy);
	return value;
}

static double p1_avg_u64(const uint64_t *values, size_t count) {
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

static int p1_dataset_init(p1_dataset_t *ds, size_t count, size_t queries) {
	kvs_flat_index_t flat;
	kvs_vector_hit_t flat_hits[P1_TOPK];
	uint64_t start_us;
	size_t i;
	int failed = 0;

	memset(ds, 0, sizeof(*ds));
	memset(&flat, 0, sizeof(flat));

	ds->count = count;
	ds->queries = queries;
	ds->base = (float *)malloc(count * P1_DIM * sizeof(*ds->base));
	ds->query = (float *)malloc(queries * P1_DIM * sizeof(*ds->query));
	ds->exact = (p1_exact_item_t *)calloc(queries * P1_TOPK, sizeof(*ds->exact));
	ds->exact_counts = (size_t *)calloc(queries, sizeof(*ds->exact_counts));
	if (!ds->base || !ds->query || !ds->exact || !ds->exact_counts) {
		fprintf(stderr, "hnsw_p1_dataset_alloc_failed count=%zu queries=%zu\n", count, queries);
		return 1;
	}

	for (i = 0; i < count; i++) {
		p1_fill_random_unit_vector(
			ds->base + i * P1_DIM,
			P1_DIM,
			0xabc00000ULL + (uint64_t)i
		);
	}
	for (i = 0; i < queries; i++) {
		p1_fill_random_unit_vector(
			ds->query + i * P1_DIM,
			P1_DIM,
			0xdad00000ULL + (uint64_t)i
		);
	}

	if (kvs_flat_index_init(&flat, P1_DIM, KVS_VECTOR_METRIC_IP) != KVS_VECTOR_OK ||
	    kvs_flat_index_reserve(&flat, count) != KVS_VECTOR_OK) {
		fprintf(stderr, "hnsw_p1_core_flat_init_failed count=%zu\n", count);
		return 1;
	}

	for (i = 0; i < count; i++) {
		if (kvs_flat_index_add(
			    &flat,
			    P1_BASE_RECORD_ID + (uint64_t)i,
			    ds->base + i * P1_DIM,
			    P1_DIM
		    ) != KVS_VECTOR_OK) {
			fprintf(stderr, "hnsw_p1_core_flat_add_failed count=%zu i=%zu\n", count, i);
			failed = 1;
			goto done;
		}
	}

	start_us = p1_now_us();
	for (i = 0; i < queries; i++) {
		size_t exact_count = 0;
		size_t flat_count = 0;

		p1_direct_exact_topk(
			ds->base,
			count,
			ds->query + i * P1_DIM,
			ds->exact + i * P1_TOPK,
			&exact_count
		);
		ds->exact_counts[i] = exact_count;

		if (kvs_flat_index_search(
			    &flat,
			    ds->query + i * P1_DIM,
			    P1_DIM,
			    P1_TOPK,
			    flat_hits,
			    &flat_count
		    ) != KVS_VECTOR_OK ||
		    !p1_hits_descending(flat_hits, flat_count) ||
		    !p1_exact_same_ids(ds->exact + i * P1_TOPK, exact_count, flat_hits, flat_count)) {
			ds->oracle_mismatch_count++;
		}
	}
	ds->oracle_us = p1_now_us() - start_us;

	printf(
		"hnsw_p1_oracle count=%zu dim=%u topk=%u queries=%zu "
		"metric=ip_normalized direct_exact=1 core_flat_crosscheck=1 "
		"mismatch=%zu oracle_us=%llu\n",
		count,
		P1_DIM,
		P1_TOPK,
		queries,
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

static void p1_dataset_destroy(p1_dataset_t *ds) {
	if (!ds) {
		return;
	}
	free(ds->base);
	free(ds->query);
	free(ds->exact);
	free(ds->exact_counts);
	memset(ds, 0, sizeof(*ds));
}

static int p1_run_v2_profile(const p1_dataset_t *ds, const p1_profile_t *profile) {
	kvs_hnsw_v2_index_t index;
	kvs_vector_hit_t *hits;
	p1_query_stats_t *stats;
	uint64_t *elapsed;
	uint64_t *visited;
	uint64_t *distance;
	uint64_t build_start_us;
	uint64_t build_us;
	size_t i;
	size_t ef_idx;
	int failed = 0;

	memset(&index, 0, sizeof(index));
	hits = (kvs_vector_hit_t *)malloc(P1_TOPK * sizeof(*hits));
	stats = (p1_query_stats_t *)calloc(ds->queries, sizeof(*stats));
	elapsed = (uint64_t *)malloc(ds->queries * sizeof(*elapsed));
	visited = (uint64_t *)malloc(ds->queries * sizeof(*visited));
	distance = (uint64_t *)malloc(ds->queries * sizeof(*distance));
	if (!hits || !stats || !elapsed || !visited || !distance) {
		failed = 1;
		goto done;
	}

	if (kvs_hnsw_v2_index_init(&index, P1_DIM, KVS_VECTOR_METRIC_IP) != KVS_VECTOR_OK) {
		fprintf(stderr, "hnsw_p1_v2_init_failed count=%zu profile=%s\n", ds->count, profile->name);
		failed = 1;
		goto done;
	}
	index.M = profile->M;
	index.M0 = profile->M0;
	index.ef_construction = profile->ef_construction;
	index.ef_search = KVS_HNSW_V2_DEFAULT_EF_SEARCH;
	kvs_hnsw_v2_index_set_seed(&index, 0x9d000000ULL + (uint64_t)ds->count +
		(uint64_t)profile->M * 17ULL + (uint64_t)profile->ef_construction * 31ULL);
	if (kvs_hnsw_v2_index_reserve(&index, ds->count) != KVS_VECTOR_OK) {
		fprintf(stderr, "hnsw_p1_v2_reserve_failed count=%zu profile=%s\n", ds->count, profile->name);
		failed = 1;
		goto done;
	}

	printf(
		"hnsw_p1_build_start backend=core_hnsw_v2 count=%zu profile=%s "
		"M=%zu M0=%zu efConstruction=%zu\n",
		ds->count,
		profile->name,
		profile->M,
		profile->M0,
		profile->ef_construction
	);
	fflush(stdout);

	build_start_us = p1_now_us();
	for (i = 0; i < ds->count; i++) {
		if (kvs_hnsw_v2_index_add(
			    &index,
			    P1_BASE_RECORD_ID + (uint64_t)i,
			    ds->base + i * P1_DIM,
			    P1_DIM
		    ) != KVS_VECTOR_OK) {
			fprintf(stderr, "hnsw_p1_v2_add_failed count=%zu profile=%s i=%zu\n",
				ds->count, profile->name, i);
			failed = 1;
			goto done;
		}
	}
	build_us = p1_now_us() - build_start_us;
	kvs_hnsw_v2_graph_stats_refresh(&index);
	kvs_hnsw_v2_memory_refresh(&index);

	printf(
		"hnsw_p1_graph backend=core_hnsw_v2 count=%zu profile=%s "
		"build_us=%llu memory_bytes=%zu max_level=%u entry_point_level=%u "
		"level0_degree_min=%u level0_degree_avg=%.2f level0_degree_p50=%u "
		"level0_degree_p95=%u level0_degree_max=%u upper_degree_min=%u "
		"upper_degree_avg=%.2f upper_degree_p50=%u upper_degree_p95=%u "
		"upper_degree_max=%u isolated=%llu duplicate_edge=%llu degree_overflow=%llu "
		"selected=%llu reverse_prune=%llu\n",
		ds->count,
		profile->name,
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

	{
		double prev_recall = -1.0;
		for (ef_idx = 0; ef_idx < profile->ef_count; ef_idx++) {
			size_t ef = profile->efs[ef_idx];
			double recall_sum = 0.0;
			double recall;
			size_t q;

			memset(&index.search_stats, 0, sizeof(index.search_stats));
			for (q = 0; q < ds->queries; q++) {
				size_t hit_count = 0;
				uint64_t start_us = p1_now_us();
				int ret = kvs_hnsw_v2_index_search(
					&index,
					ds->query + q * P1_DIM,
					P1_DIM,
					P1_TOPK,
					ef,
					hits,
					&hit_count
				);
				stats[q].elapsed_us = p1_now_us() - start_us;
				if (ret != KVS_VECTOR_OK || !p1_hits_descending(hits, hit_count)) {
					fprintf(stderr,
						"hnsw_p1_v2_search_failed count=%zu profile=%s ef=%zu q=%zu ret=%d\n",
						ds->count, profile->name, ef, q, ret);
					failed = 1;
					goto done;
				}
				stats[q].visited = index.search_stats.last_visited_count;
				stats[q].distance = index.search_stats.last_distance_compute_count;
				recall_sum += p1_recall_hits(
					ds->exact + q * P1_TOPK,
					ds->exact_counts[q],
					hits,
					hit_count
				);
			}

			for (q = 0; q < ds->queries; q++) {
				elapsed[q] = stats[q].elapsed_us;
				visited[q] = stats[q].visited;
				distance[q] = stats[q].distance;
			}
			recall = recall_sum / (double)ds->queries;
			printf(
				"hnsw_p1_result backend=core_hnsw_v2 count=%zu dim=%u topk=%u "
				"queries=%zu profile=%s M=%zu M0=%zu efConstruction=%zu "
				"efSearch=%zu build_us=%llu search_avg_us=%.2f search_p50_us=%llu "
				"search_p95_us=%llu recall@10=%.4f memory_bytes=%zu memory_source=measured "
				"visited_avg=%.2f visited_p95=%llu distance_avg=%.2f distance_p95=%llu "
				"duplicate_edge=%llu degree_overflow=%llu monotonic_vs_prev=%s\n",
				ds->count,
				P1_DIM,
				P1_TOPK,
				ds->queries,
				profile->name,
				profile->M,
				profile->M0,
				profile->ef_construction,
				ef,
				(unsigned long long)build_us,
				p1_avg_u64(elapsed, ds->queries),
				(unsigned long long)p1_percentile_u64(elapsed, ds->queries, 50.0),
				(unsigned long long)p1_percentile_u64(elapsed, ds->queries, 95.0),
				recall,
				index.memory.total_bytes,
				p1_avg_u64(visited, ds->queries),
				(unsigned long long)p1_percentile_u64(visited, ds->queries, 95.0),
				p1_avg_u64(distance, ds->queries),
				(unsigned long long)p1_percentile_u64(distance, ds->queries, 95.0),
				(unsigned long long)index.graph_stats.duplicate_edge_count,
				(unsigned long long)index.graph_stats.degree_overflow_count,
				(prev_recall < 0.0 || recall + 0.000001 >= prev_recall) ? "yes" : "no"
			);
			if (prev_recall >= 0.0 && recall + 0.000001 < prev_recall) {
				fprintf(stderr,
					"hnsw_p1_v2_non_monotonic count=%zu profile=%s prev=%.4f current=%.4f ef=%zu\n",
					ds->count, profile->name, prev_recall, recall, ef);
				failed = 1;
				goto done;
			}
			prev_recall = recall;
		}
	}

done:
	kvs_hnsw_v2_index_destroy(&index);
	free(hits);
	free(stats);
	free(elapsed);
	free(visited);
	free(distance);
	return failed;
}

#ifdef ENABLE_HNSWLIB
static int p1_run_hnswlib_profile(const p1_dataset_t *ds, const p1_profile_t *profile) {
	kvs_hnswlib_index_t *index = NULL;
	size_t *ids = NULL;
	float *scores = NULL;
	uint64_t *elapsed = NULL;
	uint64_t build_start_us;
	uint64_t build_us;
	size_t i;
	size_t ef_idx;
	int failed = 0;

	ids = (size_t *)malloc(P1_TOPK * sizeof(*ids));
	scores = (float *)malloc(P1_TOPK * sizeof(*scores));
	elapsed = (uint64_t *)malloc(ds->queries * sizeof(*elapsed));
	if (!ids || !scores || !elapsed) {
		failed = 1;
		goto done;
	}

	printf(
		"hnsw_p1_build_start backend=hnswlib count=%zu profile=%s "
		"M=%zu M0=%zu efConstruction=%zu\n",
		ds->count,
		profile->name,
		profile->M,
		profile->M0,
		profile->ef_construction
	);
	fflush(stdout);

	index = kvs_hnswlib_index_create(
		P1_DIM,
		ds->count,
		profile->M,
		profile->ef_construction
	);
	if (!index) {
		fprintf(stderr, "hnsw_p1_hnswlib_create_failed count=%zu profile=%s\n",
			ds->count, profile->name);
		failed = 1;
		goto done;
	}

	build_start_us = p1_now_us();
	for (i = 0; i < ds->count; i++) {
		if (kvs_hnswlib_index_add(
			    index,
			    (size_t)(P1_BASE_RECORD_ID + (uint64_t)i),
			    ds->base + i * P1_DIM,
			    P1_DIM
		    ) != 0) {
			fprintf(stderr, "hnsw_p1_hnswlib_add_failed count=%zu profile=%s i=%zu\n",
				ds->count, profile->name, i);
			failed = 1;
			goto done;
		}
	}
	build_us = p1_now_us() - build_start_us;

	for (ef_idx = 0; ef_idx < profile->ef_count; ef_idx++) {
		size_t ef = profile->efs[ef_idx];
		double recall_sum = 0.0;
		double recall;
		size_t q;

		kvs_hnswlib_index_set_ef(index, ef);
		for (q = 0; q < ds->queries; q++) {
			int ret;
			uint64_t start_us = p1_now_us();

			ret = kvs_hnswlib_index_search(
				index,
				ds->query + q * P1_DIM,
				P1_DIM,
				P1_TOPK,
				ids,
				scores
			);
			elapsed[q] = p1_now_us() - start_us;
			if (ret < 0 || !p1_external_descending(ids, scores, (size_t)ret)) {
				fprintf(stderr,
					"hnsw_p1_hnswlib_search_failed count=%zu profile=%s ef=%zu q=%zu ret=%d\n",
					ds->count, profile->name, ef, q, ret);
				failed = 1;
				goto done;
			}
			recall_sum += p1_recall_external(
				ds->exact + q * P1_TOPK,
				ds->exact_counts[q],
				ids,
				(size_t)ret
			);
		}
		recall = recall_sum / (double)ds->queries;
		printf(
			"hnsw_p1_result backend=hnswlib count=%zu dim=%u topk=%u "
			"queries=%zu profile=%s M=%zu M0=%zu efConstruction=%zu "
			"efSearch=%zu build_us=%llu search_avg_us=%.2f search_p50_us=%llu "
			"search_p95_us=%llu recall@10=%.4f memory_bytes=0 "
			"memory_source=unsupported visited_avg=0 visited_p95=0 "
			"distance_avg=0 distance_p95=0 duplicate_edge=unsupported "
			"degree_overflow=unsupported\n",
			ds->count,
			P1_DIM,
			P1_TOPK,
			ds->queries,
			profile->name,
			profile->M,
			profile->M0,
			profile->ef_construction,
			ef,
			(unsigned long long)build_us,
			p1_avg_u64(elapsed, ds->queries),
			(unsigned long long)p1_percentile_u64(elapsed, ds->queries, 50.0),
			(unsigned long long)p1_percentile_u64(elapsed, ds->queries, 95.0),
			recall
		);
	}

done:
	if (index) {
		kvs_hnswlib_index_destroy(index);
	}
	free(ids);
	free(scores);
	free(elapsed);
	return failed;
}
#endif

static void p1_print_faiss_status(void) {
#ifdef ENABLE_FAISS_BASELINE
	printf(
		"hnsw_p1_faiss_status status=partial reason=faiss_c_abi_has_no_efSearch_setter "
		"next_required_work=add_test_only_faiss_hnsw_direct_cpp_harness_or_c_abi_efSearch_setter\n"
	);
#else
	printf(
		"hnsw_p1_faiss_status status=missing reason=ENABLE_FAISS_BASELINE_not_compiled\n"
	);
#endif
}

static void p1_print_hnswlib_status(int run_hnswlib) {
#ifdef ENABLE_HNSWLIB
	if (run_hnswlib) {
		printf("hnsw_p1_hnswlib_status status=enabled reason=HNSW_P1_EXTERNAL=1\n");
	} else {
		printf(
			"hnsw_p1_hnswlib_status status=available_not_run "
			"reason=HNSW_P1_EXTERNAL_not_1 next_required_work=run_explicit_external_parity\n"
		);
	}
#else
	(void)run_hnswlib;
	printf("hnsw_p1_hnswlib_status status=missing reason=ENABLE_HNSWLIB_not_compiled\n");
#endif
}

static int p1_run_case(
	size_t count,
	size_t queries,
	const char *mode,
	const p1_profile_t *profiles,
	size_t profile_count,
	int run_hnswlib
) {
	p1_dataset_t ds;
	size_t i;
	int failed = 0;

	if (p1_dataset_init(&ds, count, queries) != 0) {
		return 1;
	}

	printf(
		"hnsw_p1_case_start mode=%s count=%zu dim=%u topk=%u queries=%zu "
		"profile_count=%zu hnswlib=%s\n",
		mode,
		count,
		P1_DIM,
		P1_TOPK,
		queries,
		profile_count,
#ifdef ENABLE_HNSWLIB
		run_hnswlib ? "enabled" : "disabled"
#else
		"missing"
#endif
	);

	for (i = 0; i < profile_count; i++) {
		failed |= p1_run_v2_profile(&ds, &profiles[i]);
		if (failed) {
			break;
		}
#ifdef ENABLE_HNSWLIB
		if (run_hnswlib) {
			failed |= p1_run_hnswlib_profile(&ds, &profiles[i]);
			if (failed) {
				break;
			}
		}
#else
		(void)run_hnswlib;
#endif
	}

	p1_dataset_destroy(&ds);
	return failed;
}

static int p1_mode_is(const char *mode, const char *expected) {
	return mode && strcmp(mode, expected) == 0;
}

static size_t p1_env_size_or(const char *name, size_t fallback) {
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

int main(void) {
	static const p1_profile_t baseline = {
		"baseline",
		32,
		64,
		256,
		{256, 512, 1024},
		3
	};
	static const p1_profile_t balanced = {
		"balanced",
		48,
		96,
		512,
		{512, 1024, 0},
		2
	};
	static const p1_profile_t high_recall = {
		"high_recall",
		64,
		128,
		1024,
		{1024, 2048, 0},
		2
	};
	const char *mode = getenv("HNSW_P1_MODE");
	const char *external = getenv("HNSW_P1_EXTERNAL");
	int run_hnswlib = 0;
	int failed = 0;

	if (!mode || !*mode) {
		mode = "quick";
	}
	if (external && strcmp(external, "1") == 0) {
		run_hnswlib = 1;
	}

	memset(&g_cfg, 0, sizeof(g_cfg));
	kvs_config_init_default(&g_cfg);
	g_cfg.mem_backend = KVS_MEM_BACKEND_SYSTEM;
	snprintf(g_cfg.vector_distance_backend, sizeof(g_cfg.vector_distance_backend), "%s", "auto");
	mem_set_backend(KVS_MEM_BACKEND_SYSTEM);

	printf(
		"hnsw_p1_start mode=%s metric=ip_normalized dim=%u topk=%u seed=fixed "
		"default_queries=quick:10K100_50K200 full:10K100_50K200_100K50\n",
		mode,
		P1_DIM,
		P1_TOPK
	);
	p1_print_faiss_status();
	p1_print_hnswlib_status(run_hnswlib);

	if (p1_mode_is(mode, "smoke")) {
		p1_profile_t smoke_profile = baseline;
		smoke_profile.efs[0] = 128;
		smoke_profile.efs[1] = 256;
		smoke_profile.efs[2] = 0;
		smoke_profile.ef_count = 2;
		failed |= p1_run_case(
			p1_env_size_or("HNSW_P1_SMOKE_COUNT", 2000),
			p1_env_size_or("HNSW_P1_SMOKE_QUERIES", 20),
			mode,
			&smoke_profile,
			1,
			0
		);
	} else if (p1_mode_is(mode, "quick")) {
		const p1_profile_t profiles_10k[] = {baseline};
		const p1_profile_t profiles_50k[] = {baseline, balanced};

		failed |= p1_run_case(
			p1_env_size_or("HNSW_P1_10K_COUNT", 10000),
			p1_env_size_or("HNSW_P1_10K_QUERIES", 100),
			mode,
			profiles_10k,
			sizeof(profiles_10k) / sizeof(profiles_10k[0]),
			0
		);
		if (!failed) {
			failed |= p1_run_case(
				p1_env_size_or("HNSW_P1_50K_COUNT", 50000),
				p1_env_size_or("HNSW_P1_50K_QUERIES", 200),
				mode,
				profiles_50k,
				sizeof(profiles_50k) / sizeof(profiles_50k[0]),
				run_hnswlib
			);
		}
	} else if (p1_mode_is(mode, "full")) {
		const p1_profile_t profiles_10k[] = {baseline};
		const p1_profile_t profiles_50k[] = {baseline, balanced, high_recall};
		const p1_profile_t profiles_100k[] = {baseline};

		failed |= p1_run_case(
			p1_env_size_or("HNSW_P1_10K_COUNT", 10000),
			p1_env_size_or("HNSW_P1_10K_QUERIES", 100),
			mode,
			profiles_10k,
			sizeof(profiles_10k) / sizeof(profiles_10k[0]),
			0
		);
		if (!failed) {
			failed |= p1_run_case(
				p1_env_size_or("HNSW_P1_50K_COUNT", 50000),
				p1_env_size_or("HNSW_P1_50K_QUERIES", 200),
				mode,
				profiles_50k,
				sizeof(profiles_50k) / sizeof(profiles_50k[0]),
				run_hnswlib
			);
		}
		if (!failed) {
			failed |= p1_run_case(
				p1_env_size_or("HNSW_P1_100K_COUNT", 100000),
				p1_env_size_or("HNSW_P1_100K_QUERIES", 50),
				mode,
				profiles_100k,
				sizeof(profiles_100k) / sizeof(profiles_100k[0]),
				run_hnswlib
			);
		}
	} else {
		fprintf(stderr, "unknown HNSW_P1_MODE=%s (expected smoke, quick, full)\n", mode);
		failed = 1;
	}

	if (failed) {
		fprintf(stderr, "vector_core_hnsw_v2_parity_baseline_test FAILED\n");
		return 1;
	}
	printf("vector_core_hnsw_v2_parity_baseline_test PASS\n");
	return 0;
}
