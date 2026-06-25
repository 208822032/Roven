#pragma once

#include <stddef.h>
#include <stdint.h>

#include "kvs_vector.h"
#include "vector/kvs_vector_core.h"
#include "vector/kvs_vector_id_slot_map.h"

/*
 * KVLite core/flat exact index.
 *
 * This is a vector_core implementation.
 *
 * It only handles:
 *   record_id + vector -> record_id + score
 *
 * It must not depend on:
 *   question / answer / scope / TTL / metadata / manager store
 *
 * Manager is responsible for:
 *   record lookup
 *   scope filtering
 *   TTL filtering
 *   deleted filtering at product semantics layer
 *   threshold filtering
 *   response assembly
 */

typedef float (*kvs_vector_distance_fn)(
	const float *lhs,
	const float *rhs,
	size_t dim
);

typedef struct kvs_flat_index_s {
	size_t dim;
	int metric;

	size_t capacity;
	size_t size;

	float *vectors;
	uint64_t *slot_to_record_id;
	uint8_t *deleted_flags;

	kvs_vector_id_slot_map_t id_to_slot;

	kvs_vector_distance_fn distance;

	int ready;
	int needs_rebuild;
	size_t deleted_count;
} kvs_flat_index_t;

int kvs_flat_index_init(
	kvs_flat_index_t *index,
	size_t dim,
	int metric
);

void kvs_flat_index_destroy(
	kvs_flat_index_t *index
);

int kvs_flat_index_reserve(
	kvs_flat_index_t *index,
	size_t capacity
);

int kvs_flat_index_add(
	kvs_flat_index_t *index,
	uint64_t record_id,
	const float *vector,
	size_t dim
);

int kvs_flat_index_search(
	kvs_flat_index_t *index,
	const float *query,
	size_t dim,
	size_t topk,
	kvs_vector_hit_t *out,
	size_t *out_count
);

int kvs_flat_index_mark_delete(
	kvs_flat_index_t *index,
	uint64_t record_id
);

int kvs_flat_index_reset(
	kvs_flat_index_t *index
);

int kvs_flat_index_info(
	kvs_flat_index_t *index,
	kvs_vector_core_info_t *out
);
