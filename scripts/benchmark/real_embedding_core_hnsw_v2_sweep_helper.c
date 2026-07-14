/*
 * Thin C ABI wrapper around core_hnsw_v2 for the C++ B-scale-3 sweep runner.
 * This keeps KVLite C headers out of the FAISS C++ translation unit.
 */

#define _POSIX_C_SOURCE 200809L

#include <stdint.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#include <openssl/evp.h>

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
	uint64_t add_count;
	uint64_t add_total_us;
	uint64_t random_level_us;
	uint64_t vector_write_us;
	uint64_t id_slot_map_us;
	uint64_t construction_layer_search_us;
	uint64_t neighbor_select_us;
	uint64_t forward_link_us;
	uint64_t reverse_link_us;
	uint64_t reverse_prune_us;
	uint64_t graph_stats_refresh_us;
	uint64_t memory_refresh_us;
	uint64_t distance_compute_count;
	uint64_t construction_distance_compute_count;
	uint64_t neighbor_select_distance_compute_count;
	uint64_t reverse_prune_distance_compute_count;
	uint64_t reverse_prune_neighbor_select_distance_compute_count;
	uint64_t temporary_allocation_count;
	uint64_t temporary_free_count;
	uint64_t temporary_allocation_bytes;
	uint64_t reverse_prune_known_score_reuse_count;
	uint64_t reverse_prune_pair_cache_lookup_count;
	uint64_t reverse_prune_pair_cache_hit_count;
	uint64_t reverse_prune_pair_cache_miss_count;
	uint64_t reverse_prune_pair_cache_collision_count;
	uint64_t reverse_prune_pair_score_saved_count;
	uint64_t reserve_count;
	uint64_t resize_count;
	char graph_sha256[65];
} bench_core_hnsw_v2_stats_t;

typedef struct bench_core_search_work_s {
	uint64_t distance_compute_count;
	uint64_t visited_node_count;
	uint64_t neighbor_scan_count;
	uint64_t final_top_count;
	uint64_t final_sort_us;
} bench_core_search_work_t;

typedef struct bench_core_handle_s {
	kvs_hnsw_v2_index_t *index;
	kvs_vector_hit_t *reusable_hits;
	size_t reusable_hits_capacity;
	int reuse_hit_buffer;
} bench_core_handle_t;

static uint64_t bench_now_us(void) {
	struct timespec ts;
	clock_gettime(CLOCK_MONOTONIC, &ts);
	return (uint64_t)ts.tv_sec * 1000000ull + (uint64_t)ts.tv_nsec / 1000ull;
}

static int bench_digest_u64(EVP_MD_CTX *ctx, uint64_t value) {
	return EVP_DigestUpdate(ctx, &value, sizeof(value)) == 1 ? 0 : -1;
}

static int bench_core_graph_sha256(
	const kvs_hnsw_v2_index_t *index,
	char out[65]
) {
	EVP_MD_CTX *ctx = NULL;
	unsigned char digest[EVP_MAX_MD_SIZE];
	unsigned int digest_len = 0;
	size_t slot;
	int ret = -1;

	if (!index || !out) {
		return -1;
	}
	ctx = EVP_MD_CTX_new();
	if (!ctx || EVP_DigestInit_ex(ctx, EVP_sha256(), NULL) != 1) {
		goto done;
	}
	if (bench_digest_u64(ctx, index->dim) != 0 ||
	    bench_digest_u64(ctx, (uint64_t)(uint32_t)index->metric) != 0 ||
	    bench_digest_u64(ctx, index->M) != 0 ||
	    bench_digest_u64(ctx, index->M0) != 0 ||
	    bench_digest_u64(ctx, index->ef_construction) != 0 ||
	    bench_digest_u64(ctx, index->size) != 0 ||
	    bench_digest_u64(ctx, index->entry_point) != 0 ||
	    bench_digest_u64(ctx, index->max_level) != 0) {
		goto done;
	}
	for (slot = 0; slot < index->size; slot++) {
		uint8_t max_level = index->levels[slot];
		uint8_t level;

		if (bench_digest_u64(ctx, slot) != 0 ||
		    bench_digest_u64(ctx, index->slot_to_record_id[slot]) != 0 ||
		    bench_digest_u64(ctx, max_level) != 0 ||
		    bench_digest_u64(ctx, index->deleted_flags[slot]) != 0) {
			goto done;
		}
		for (level = 0; level <= max_level; level++) {
			uint16_t *count_ptr = kvs_hnsw_v2_layer_count_ptr(
				(kvs_hnsw_v2_index_t *)index, (uint32_t)slot, level);
			uint32_t *neighbors = kvs_hnsw_v2_layer_neighbors(
				(kvs_hnsw_v2_index_t *)index, (uint32_t)slot, level);
			size_t i;

			if (!count_ptr || !neighbors ||
			    bench_digest_u64(ctx, level) != 0 ||
			    bench_digest_u64(ctx, *count_ptr) != 0) {
				goto done;
			}
			for (i = 0; i < *count_ptr; i++) {
				if (bench_digest_u64(ctx, neighbors[i]) != 0) {
					goto done;
				}
			}
		}
	}
	if (EVP_DigestFinal_ex(ctx, digest, &digest_len) != 1 || digest_len != 32) {
		goto done;
	}
	for (unsigned int i = 0; i < digest_len; i++) {
		snprintf(out + i * 2, 3, "%02x", digest[i]);
	}
	out[64] = '\0';
	ret = 0;

done:
	EVP_MD_CTX_free(ctx);
	return ret;
}

int bench_core_hnsw_v2_build(
	size_t dim,
	size_t count,
	const float *base,
	const uint64_t *insertion_order,
	uint64_t seed,
	int build_profiling_enabled,
	void **out_handle,
	bench_core_hnsw_v2_stats_t *out_stats,
	uint64_t *out_build_us
) {
	kvs_hnsw_v2_index_t *index = NULL;
	bench_core_handle_t *handle = NULL;
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

	handle = calloc(1, sizeof(*handle));
	index = calloc(1, sizeof(*index));
	if (!handle || !index) {
		free(handle);
		free(index);
		return -1;
	}
	t0 = bench_now_us();
	if (kvs_hnsw_v2_index_init(index, dim, KVS_VECTOR_METRIC_IP) != KVS_VECTOR_OK) {
		free(index);
		free(handle);
		return -1;
	}
	kvs_hnsw_v2_index_set_seed(index, seed);
	kvs_hnsw_v2_index_set_build_profiling(index, build_profiling_enabled);
	if (kvs_hnsw_v2_index_reserve(index, count) != KVS_VECTOR_OK) {
		kvs_hnsw_v2_index_destroy(index);
		free(index);
		free(handle);
		return -1;
	}
	for (size_t i = 0; i < count; i++) {
		uint64_t rid = insertion_order[i];
		if (rid >= count ||
		    kvs_hnsw_v2_index_add(index, rid, base + rid * dim, dim) != KVS_VECTOR_OK) {
			kvs_hnsw_v2_index_destroy(index);
			free(index);
			free(handle);
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
		free(handle);
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
	out_stats->add_count = index->build_stats.add_count;
	out_stats->add_total_us = index->build_stats.add_total_us;
	out_stats->random_level_us = index->build_stats.random_level_us;
	out_stats->vector_write_us = index->build_stats.vector_write_us;
	out_stats->id_slot_map_us = index->build_stats.id_slot_map_us;
	out_stats->construction_layer_search_us = index->build_stats.construction_layer_search_us;
	out_stats->neighbor_select_us = index->build_stats.neighbor_select_us;
	out_stats->forward_link_us = index->build_stats.forward_link_us;
	out_stats->reverse_link_us = index->build_stats.reverse_link_us;
	out_stats->reverse_prune_us = index->build_stats.reverse_prune_us;
	out_stats->graph_stats_refresh_us = index->build_stats.graph_stats_refresh_us;
	out_stats->memory_refresh_us = index->build_stats.memory_refresh_us;
	out_stats->distance_compute_count = index->build_stats.distance_compute_count;
	out_stats->construction_distance_compute_count =
		index->build_stats.construction_distance_compute_count;
	out_stats->neighbor_select_distance_compute_count =
		index->build_stats.neighbor_select_distance_compute_count;
	out_stats->reverse_prune_distance_compute_count =
		index->build_stats.reverse_prune_distance_compute_count;
	out_stats->reverse_prune_neighbor_select_distance_compute_count =
		index->build_stats.reverse_prune_neighbor_select_distance_compute_count;
	out_stats->temporary_allocation_count = index->build_stats.temporary_allocation_count;
	out_stats->temporary_free_count = index->build_stats.temporary_free_count;
	out_stats->temporary_allocation_bytes = index->build_stats.temporary_allocation_bytes;
	out_stats->reverse_prune_known_score_reuse_count =
		index->build_stats.reverse_prune_known_score_reuse_count;
	out_stats->reverse_prune_pair_cache_lookup_count =
		index->build_stats.reverse_prune_pair_cache_lookup_count;
	out_stats->reverse_prune_pair_cache_hit_count =
		index->build_stats.reverse_prune_pair_cache_hit_count;
	out_stats->reverse_prune_pair_cache_miss_count =
		index->build_stats.reverse_prune_pair_cache_miss_count;
	out_stats->reverse_prune_pair_cache_collision_count =
		index->build_stats.reverse_prune_pair_cache_collision_count;
	out_stats->reverse_prune_pair_score_saved_count =
		index->build_stats.reverse_prune_pair_score_saved_count;
	out_stats->reserve_count = 1;
	out_stats->resize_count = 0;
	if (bench_core_graph_sha256(index, out_stats->graph_sha256) != 0) {
		kvs_hnsw_v2_index_destroy(index);
		free(index);
		free(handle);
		return -1;
	}
	handle->index = index;
	*out_handle = handle;
	return 0;
}

int bench_core_hnsw_v2_configure_search(
	void *opaque,
	int profiling_enabled,
	int reuse_hit_buffer,
	size_t topk
) {
	bench_core_handle_t *handle = (bench_core_handle_t *)opaque;
	kvs_vector_hit_t *hits;

	if (!handle || !handle->index || topk == 0) {
		return -1;
	}
	kvs_hnsw_v2_index_set_search_profiling(handle->index, profiling_enabled);
	handle->reuse_hit_buffer = reuse_hit_buffer ? 1 : 0;
	if (!handle->reuse_hit_buffer || handle->reusable_hits_capacity >= topk) {
		return 0;
	}
	hits = calloc(topk, sizeof(*hits));
	if (!hits) {
		return -1;
	}
	free(handle->reusable_hits);
	handle->reusable_hits = hits;
	handle->reusable_hits_capacity = topk;
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
	bench_core_handle_t *core = (bench_core_handle_t *)handle;
	kvs_hnsw_v2_index_t *index;
	kvs_vector_hit_t *tmp = NULL;
	size_t count = 0;
	int rc;

	if (!core || !core->index || !query || !hits || !out_count || topk == 0) {
		return -1;
	}
	index = core->index;
	if (core->reuse_hit_buffer) {
		if (core->reusable_hits_capacity < topk || !core->reusable_hits) {
			return -1;
		}
		tmp = core->reusable_hits;
	} else {
		tmp = calloc(topk, sizeof(*tmp));
		if (!tmp) {
			return -1;
		}
	}
	rc = kvs_hnsw_v2_index_search(index, query, dim, topk, ef_search, tmp, &count);
	if (rc != KVS_VECTOR_OK || count != topk) {
		if (!core->reuse_hit_buffer) {
			free(tmp);
		}
		return -1;
	}
	for (size_t i = 0; i < count; i++) {
		hits[i].record_id = tmp[i].record_id;
		hits[i].score = tmp[i].score;
	}
	*out_count = count;
	if (!core->reuse_hit_buffer) {
		free(tmp);
	}
	return 0;
}

int bench_core_hnsw_v2_last_search_work(
	void *opaque,
	bench_core_search_work_t *out
) {
	bench_core_handle_t *handle = (bench_core_handle_t *)opaque;
	kvs_hnsw_v2_search_stats_t *stats;

	if (!handle || !handle->index || !out) {
		return -1;
	}
	stats = &handle->index->search_stats;
	out->distance_compute_count = stats->last_distance_compute_count;
	out->visited_node_count = stats->last_visited_count;
	out->neighbor_scan_count = stats->last_level0_neighbor_scan_count;
	out->final_top_count = stats->last_final_top_count;
	out->final_sort_us = stats->last_result_sort_us;
	return 0;
}

void bench_core_hnsw_v2_destroy(void *handle) {
	bench_core_handle_t *core = (bench_core_handle_t *)handle;
	if (core) {
		if (core->index) {
			kvs_hnsw_v2_index_destroy(core->index);
			free(core->index);
		}
		free(core->reusable_hits);
		free(core);
	}
}
