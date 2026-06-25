#include "vector/kvs_vector_heap.h"

#include "kvs_vector.h"
#include "kvs_memory.h"

#include <string.h>

int kvs_vector_heap_min_score_less(
	const kvs_vector_heap_item_t *lhs,
	const kvs_vector_heap_item_t *rhs
) {
	if (!lhs || !rhs) {
		return 0;
	}

	return lhs->score < rhs->score;
}

int kvs_vector_heap_max_score_less(
	const kvs_vector_heap_item_t *lhs,
	const kvs_vector_heap_item_t *rhs
) {
	if (!lhs || !rhs) {
		return 0;
	}

	return lhs->score > rhs->score;
}

static void kvs_vector_heap_swap(
	kvs_vector_heap_item_t *lhs,
	kvs_vector_heap_item_t *rhs
) {
	kvs_vector_heap_item_t tmp;

	tmp = *lhs;
	*lhs = *rhs;
	*rhs = tmp;
}

static void kvs_vector_heap_sift_up(
	kvs_vector_heap_t *heap,
	size_t index
) {
	while (index > 0) {
		size_t parent = (index - 1) / 2;

		if (!heap->less(&heap->items[index], &heap->items[parent])) {
			break;
		}

		kvs_vector_heap_swap(&heap->items[index], &heap->items[parent]);
		index = parent;
	}
}

static void kvs_vector_heap_sift_down(
	kvs_vector_heap_t *heap,
	size_t index
) {
	for (;;) {
		size_t left = index * 2 + 1;
		size_t right = index * 2 + 2;
		size_t best = index;

		if (left < heap->size &&
		    heap->less(&heap->items[left], &heap->items[best])) {
			best = left;
		}

		if (right < heap->size &&
		    heap->less(&heap->items[right], &heap->items[best])) {
			best = right;
		}

		if (best == index) {
			break;
		}

		kvs_vector_heap_swap(&heap->items[index], &heap->items[best]);
		index = best;
	}
}

int kvs_vector_heap_init(
	kvs_vector_heap_t *heap,
	size_t capacity,
	kvs_vector_heap_less_fn less
) {
	if (!heap || !less || capacity == 0) {
		return KVS_VECTOR_ERR_INVALID_ARG;
	}

	memset(heap, 0, sizeof(*heap));

	heap->items = (kvs_vector_heap_item_t *)kvs_malloc(
		capacity * sizeof(kvs_vector_heap_item_t)
	);
	if (!heap->items) {
		return KVS_VECTOR_ERR;
	}

	memset(heap->items, 0, capacity * sizeof(kvs_vector_heap_item_t));

	heap->size = 0;
	heap->capacity = capacity;
	heap->less = less;

	return KVS_VECTOR_OK;
}

void kvs_vector_heap_destroy(
	kvs_vector_heap_t *heap
) {
	if (!heap) {
		return;
	}

	kvs_free(heap->items);
	memset(heap, 0, sizeof(*heap));
}

void kvs_vector_heap_reset(
	kvs_vector_heap_t *heap
) {
	if (!heap) {
		return;
	}

	heap->size = 0;
}

int kvs_vector_heap_push(
	kvs_vector_heap_t *heap,
	kvs_vector_heap_item_t item
) {
	if (!heap || !heap->items || !heap->less) {
		return KVS_VECTOR_ERR_INVALID_ARG;
	}

	if (heap->size >= heap->capacity) {
		return KVS_VECTOR_ERR;
	}

	heap->items[heap->size] = item;
	kvs_vector_heap_sift_up(heap, heap->size);
	heap->size++;

	return KVS_VECTOR_OK;
}

int kvs_vector_heap_pop(
	kvs_vector_heap_t *heap,
	kvs_vector_heap_item_t *out
) {
	if (!heap || !heap->items || !heap->less || !out) {
		return KVS_VECTOR_ERR_INVALID_ARG;
	}

	if (heap->size == 0) {
		return KVS_VECTOR_ERR;
	}

	*out = heap->items[0];

	heap->size--;
	if (heap->size > 0) {
		heap->items[0] = heap->items[heap->size];
		kvs_vector_heap_sift_down(heap, 0);
	}

	return KVS_VECTOR_OK;
}

const kvs_vector_heap_item_t *kvs_vector_heap_top(
	const kvs_vector_heap_t *heap
) {
	if (!heap || !heap->items || heap->size == 0) {
		return NULL;
	}

	return &heap->items[0];
}

int kvs_vector_heap_replace_top(
	kvs_vector_heap_t *heap,
	kvs_vector_heap_item_t item
) {
	if (!heap || !heap->items || !heap->less) {
		return KVS_VECTOR_ERR_INVALID_ARG;
	}

	if (heap->size == 0) {
		return kvs_vector_heap_push(heap, item);
	}

	heap->items[0] = item;
	kvs_vector_heap_sift_down(heap, 0);

	return KVS_VECTOR_OK;
}

int kvs_vector_heap_empty(
	const kvs_vector_heap_t *heap
) {
	if (!heap) {
		return 1;
	}

	return heap->size == 0;
}