#pragma once

#include <stddef.h>
#include <stdint.h>

/*
 * Generic heap for vector_core.
 *
 * This component only handles:
 *   record_id / slot / score
 *
 * It must not depend on:
 *   question / answer / scope / TTL / metadata / manager store
 */

typedef struct kvs_vector_heap_item_s {
	uint64_t record_id;
	uint32_t slot;
	float score;
} kvs_vector_heap_item_t;

typedef int (*kvs_vector_heap_less_fn)(
	const kvs_vector_heap_item_t *lhs,
	const kvs_vector_heap_item_t *rhs
);

typedef struct kvs_vector_heap_s {
	kvs_vector_heap_item_t *items;
	size_t size;
	size_t capacity;
	kvs_vector_heap_less_fn less;
} kvs_vector_heap_t;

int kvs_vector_heap_min_score_less(
	const kvs_vector_heap_item_t *lhs,
	const kvs_vector_heap_item_t *rhs
);

int kvs_vector_heap_max_score_less(
	const kvs_vector_heap_item_t *lhs,
	const kvs_vector_heap_item_t *rhs
);

int kvs_vector_heap_init(
	kvs_vector_heap_t *heap,
	size_t capacity,
	kvs_vector_heap_less_fn less
);

void kvs_vector_heap_destroy(
	kvs_vector_heap_t *heap
);

void kvs_vector_heap_reset(
	kvs_vector_heap_t *heap
);

int kvs_vector_heap_push(
	kvs_vector_heap_t *heap,
	kvs_vector_heap_item_t item
);

int kvs_vector_heap_pop(
	kvs_vector_heap_t *heap,
	kvs_vector_heap_item_t *out
);

const kvs_vector_heap_item_t *kvs_vector_heap_top(
	const kvs_vector_heap_t *heap
);

int kvs_vector_heap_replace_top(
	kvs_vector_heap_t *heap,
	kvs_vector_heap_item_t item
);

int kvs_vector_heap_empty(
	const kvs_vector_heap_t *heap
);