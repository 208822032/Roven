/*
 * Thin C ABI wrapper around core_hnsw_v2 for the C++ B-scale-3 sweep runner.
 * This keeps KVLite C headers out of the FAISS C++ translation unit.
 */

#define _POSIX_C_SOURCE 200809L

#include <stdint.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#include "kvs_config.h"
#include "kvs_memory.h"
#include "kvs_memory_internal.h"
#include "vector/kvs_vector_hnsw_v2.h"

extern kvs_config_t g_cfg;

typedef struct bench_core_hit_s {
	uint64_t record_id;
	float score;
} bench_core_hit_t;

typedef struct bench_core_hnsw_v2_stats_s {
	uint64_t M;
	uint64_t M0;
	uint64_t ef_construction;
	uint64_t ef_search;
	uint64_t memory_bytes;
	uint64_t size;
	uint64_t active_count;
	uint64_t deleted_count;
	uint64_t capacity;
	uint64_t duplicate_edge_count;
	uint64_t degree_overflow_count;
} bench_core_hnsw_v2_stats_t;

static uint64_t bench_now_us(void) {
	struct timespec ts;
	clock_gettime(CLOCK_MONOTONIC, &ts);
	return (uint64_t)ts.tv_sec * 1000000ull + (uint64_t)ts.tv_nsec / 1000ull;
}

int bench_core_hnsw_v2_build(
	size_t dim,
	size_t count,
	const float *base,
	const uint64_t *insertion_order,
	uint64_t seed,
	void **out_handle,
	bench_core_hnsw_v2_stats_t *out_stats,
	uint64_t *out_build_us
) {
	kvs_hnsw_v2_index_t *index = NULL;
	kvs_vector_core_info_t info;
	uint64_t t0;

	if (!base || !insertion_order || !out_handle || !out_stats || dim == 0) {
		return -1;
	}
	memset(out_stats, 0, sizeof(*out_stats));
	*out_handle = NULL;
	if (out_build_us) {
		*out_build_us = 0;
	}

	memset(&g_cfg, 0, sizeof(g_cfg));
	kvs_config_init_default(&g_cfg);
	g_cfg.mem_backend = KVS_MEM_BACKEND_SYSTEM;
	snprintf(g_cfg.vector_distance_backend, sizeof(g_cfg.vector_distance_backend), "%s", "auto");
	mem_set_backend(KVS_MEM_BACKEND_SYSTEM);

	index = calloc(1, sizeof(*index));
	if (!index) {
		return -1;
	}
	t0 = bench_now_us();
	if (kvs_hnsw_v2_index_init(index, dim, KVS_VECTOR_METRIC_IP) != KVS_VECTOR_OK) {
		free(index);
		return -1;
	}
	kvs_hnsw_v2_index_set_seed(index, seed);
	if (kvs_hnsw_v2_index_reserve(index, count) != KVS_VECTOR_OK) {
		kvs_hnsw_v2_index_destroy(index);
		free(index);
		return -1;
	}
	for (size_t i = 0; i < count; i++) {
		uint64_t rid = insertion_order[i];
		if (rid >= count ||
		    kvs_hnsw_v2_index_add(index, rid, base + rid * dim, dim) != KVS_VECTOR_OK) {
			kvs_hnsw_v2_index_destroy(index);
			free(index);
			return -1;
		}
	}
	if (out_build_us) {
		*out_build_us = bench_now_us() - t0;
	}

	kvs_hnsw_v2_graph_stats_refresh(index);
	kvs_hnsw_v2_memory_refresh(index);
	memset(&info, 0, sizeof(info));
	if (kvs_hnsw_v2_index_info(index, &info) != KVS_VECTOR_OK) {
		kvs_hnsw_v2_index_destroy(index);
		free(index);
		return -1;
	}
	out_stats->M = (uint64_t)index->M;
	out_stats->M0 = (uint64_t)index->M0;
	out_stats->ef_construction = (uint64_t)index->ef_construction;
	out_stats->ef_search = (uint64_t)index->ef_search;
	out_stats->memory_bytes = (uint64_t)info.memory_bytes;
	out_stats->size = (uint64_t)info.size;
	out_stats->deleted_count = (uint64_t)info.deleted_count;
	out_stats->active_count = out_stats->size - out_stats->deleted_count;
	out_stats->capacity = (uint64_t)info.capacity;
	out_stats->duplicate_edge_count = index->graph_stats.duplicate_edge_count;
	out_stats->degree_overflow_count = index->graph_stats.degree_overflow_count;
	*out_handle = index;
	return 0;
}

int bench_core_hnsw_v2_search(
	void *handle,
	const float *query,
	size_t dim,
	size_t topk,
	size_t ef_search,
	bench_core_hit_t *hits,
	size_t *out_count
) {
	kvs_hnsw_v2_index_t *index = (kvs_hnsw_v2_index_t *)handle;
	kvs_vector_hit_t *tmp = NULL;
	size_t count = 0;
	int rc;

	if (!index || !query || !hits || !out_count || topk == 0) {
		return -1;
	}
	tmp = calloc(topk, sizeof(*tmp));
	if (!tmp) {
		return -1;
	}
	rc = kvs_hnsw_v2_index_search(index, query, dim, topk, ef_search, tmp, &count);
	if (rc != KVS_VECTOR_OK || count != topk) {
		free(tmp);
		return -1;
	}
	for (size_t i = 0; i < count; i++) {
		hits[i].record_id = tmp[i].record_id;
		hits[i].score = tmp[i].score;
	}
	*out_count = count;
	free(tmp);
	return 0;
}

void bench_core_hnsw_v2_destroy(void *handle) {
	kvs_hnsw_v2_index_t *index = (kvs_hnsw_v2_index_t *)handle;
	if (index) {
		kvs_hnsw_v2_index_destroy(index);
		free(index);
	}
}

