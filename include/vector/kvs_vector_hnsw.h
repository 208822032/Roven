#pragma once

#include <stddef.h>
#include <stdint.h>

#include "kvs_vector.h"
#include "vector/kvs_vector_core.h"
#include "vector/kvs_vector_heap.h"
#include "vector/kvs_vector_id_slot_map.h"

/*
 * KVLite core/hnsw index skeleton.
 *
 * This is a vector_core implementation boundary.
 *
 * It only owns:
 *   record_id + vector + graph/search buffers
 *
 * It must not depend on:
 *   question / answer / scope / TTL / metadata / manager store
 *
 * Manager remains responsible for record lookup, business filtering, and
 * response assembly. This stage intentionally does not implement add/search.
 */

#define KVS_HNSW_DEFAULT_M 16
#define KVS_HNSW_DEFAULT_M0 32
#define KVS_HNSW_DEFAULT_EF_CONSTRUCTION 128
#define KVS_HNSW_DEFAULT_EF_SEARCH 128
#define KVS_HNSW_DEFAULT_CONTEXT_COUNT 2
#define KVS_HNSW_MAX_LEVEL 32

typedef float (*kvs_hnsw_distance_fn)(
	const float *lhs,
	const float *rhs,
	size_t dim
);

typedef struct kvs_hnsw_random_s {
	uint64_t state;
} kvs_hnsw_random_t;

typedef struct kvs_hnsw_search_context_s {
	kvs_vector_heap_t candidate_heap;
	kvs_vector_heap_t top_heap;
	uint32_t *neighbor_buffer;
	float *distance_buffer;
	kvs_vector_hit_t *hits;
	size_t capacity;
	int in_use;
} kvs_hnsw_search_context_t;

typedef struct kvs_hnsw_search_context_pool_s {
	kvs_hnsw_search_context_t *contexts;
	size_t count;
	size_t context_capacity;
} kvs_hnsw_search_context_pool_t;

typedef struct kvs_hnsw_index_s {
	size_t dim;
	int metric;

	size_t M;
	size_t M0;
	size_t ef_construction;
	size_t ef_search;

	size_t capacity;
	size_t size;

	float *vectors;
	uint8_t *levels;
	uint8_t *deleted_flags;
	uint64_t *slot_to_record_id;
	kvs_vector_id_slot_map_t record_id_to_slot;

	uint32_t *level0_neighbors;
	uint16_t *level0_counts;

	size_t *upper_offsets;
	uint32_t *upper_neighbors;
	uint16_t *upper_counts;

	uint32_t entry_point;
	int has_entry;
	uint8_t max_level;

	uint32_t *visited_tags;
	uint32_t current_visit_tag;

	kvs_hnsw_random_t rng;
	kvs_hnsw_search_context_pool_t search_context_pool;
	kvs_hnsw_distance_fn distance;

	int ready;
	int needs_rebuild;
	size_t deleted_count;
} kvs_hnsw_index_t;

int kvs_hnsw_index_init(
	kvs_hnsw_index_t *index,
	size_t dim,
	int metric
);

int kvs_hnsw_index_reserve(
	kvs_hnsw_index_t *index,
	size_t capacity
);

int kvs_hnsw_index_reset(
	kvs_hnsw_index_t *index
);

int kvs_hnsw_index_info(
	kvs_hnsw_index_t *index,
	kvs_vector_core_info_t *out
);

int kvs_hnsw_index_add(
	kvs_hnsw_index_t *index,
	uint64_t record_id,
	const float *vector,
	size_t dim
);

int kvs_hnsw_index_search(
	kvs_hnsw_index_t *index,
	const float *query,
	size_t dim,
	size_t topk,
	kvs_vector_hit_t *out,
	size_t *out_count
);

int kvs_hnsw_index_mark_delete(
	kvs_hnsw_index_t *index,
	uint64_t record_id
);

void kvs_hnsw_index_destroy(
	kvs_hnsw_index_t *index
);

int kvs_hnsw_context_pool_init(
	kvs_hnsw_search_context_pool_t *pool,
	size_t count,
	size_t context_capacity
);

void kvs_hnsw_context_pool_destroy(
	kvs_hnsw_search_context_pool_t *pool
);

kvs_hnsw_search_context_t *kvs_hnsw_context_acquire(
	kvs_hnsw_search_context_pool_t *pool
);

void kvs_hnsw_context_release(
	kvs_hnsw_search_context_pool_t *pool,
	kvs_hnsw_search_context_t *ctx
);

void kvs_hnsw_context_reset(
	kvs_hnsw_search_context_t *ctx
);

uint32_t kvs_hnsw_next_visit_tag(
	kvs_hnsw_index_t *index
);

size_t kvs_hnsw_graph_level0_bytes(
	size_t capacity,
	size_t m0
);

void kvs_hnsw_random_init(
	kvs_hnsw_random_t *rng,
	uint64_t seed
);

uint32_t kvs_hnsw_random_next_u32(
	kvs_hnsw_random_t *rng
);

float kvs_hnsw_random_next_unit(
	kvs_hnsw_random_t *rng
);

uint8_t kvs_hnsw_random_level(
	kvs_hnsw_random_t *rng,
	size_t M
);

uint8_t kvs_hnsw_index_next_level(
	kvs_hnsw_index_t *index
);
