/* Internal helper for core_hnsw_v2; stored under include/vector only to keep headers out of src/. */
#ifndef KVS_VECTOR_HNSW_V2_HEAP_H
#define KVS_VECTOR_HNSW_V2_HEAP_H

#include "vector/kvs_vector_hnsw_v2.h"

#include <stddef.h>
#include <stdint.h>

static inline int kvs_hnsw_v2_candidate_better(
	const kvs_hnsw_v2_candidate_t *lhs,
	const kvs_hnsw_v2_candidate_t *rhs
) {
	if (lhs->score > rhs->score) {
		return 1;
	}
	if (lhs->score < rhs->score) {
		return 0;
	}
	return lhs->slot < rhs->slot;
}

static inline int kvs_hnsw_v2_candidate_worse(
	const kvs_hnsw_v2_candidate_t *lhs,
	const kvs_hnsw_v2_candidate_t *rhs
) {
	if (lhs->score < rhs->score) {
		return 1;
	}
	if (lhs->score > rhs->score) {
		return 0;
	}
	return lhs->slot > rhs->slot;
}

static inline void kvs_hnsw_v2_heap_swap(
	kvs_hnsw_v2_candidate_t *lhs,
	kvs_hnsw_v2_candidate_t *rhs
) {
	kvs_hnsw_v2_candidate_t tmp = *lhs;

	*lhs = *rhs;
	*rhs = tmp;
}

static inline int kvs_hnsw_v2_candidate_heap_push(
	kvs_hnsw_v2_candidate_t *heap,
	size_t *count,
	size_t capacity,
	kvs_hnsw_v2_candidate_t item,
	uint64_t *sift_count
) {
	size_t pos;

	if (!heap || !count || *count >= capacity) {
		return KVS_VECTOR_ERR;
	}

	pos = *count;
	heap[pos] = item;
	(*count)++;
	while (pos > 0) {
		size_t parent = (pos - 1) / 2;

		if (!kvs_hnsw_v2_candidate_better(&heap[pos], &heap[parent])) {
			break;
		}
		kvs_hnsw_v2_heap_swap(&heap[pos], &heap[parent]);
		if (sift_count) {
			(*sift_count)++;
		}
		pos = parent;
	}

	return KVS_VECTOR_OK;
}

static inline int kvs_hnsw_v2_candidate_heap_pop_best(
	kvs_hnsw_v2_candidate_t *heap,
	size_t *count,
	kvs_hnsw_v2_candidate_t *out,
	uint64_t *sift_count
) {
	size_t pos = 0;

	if (!heap || !count || !out || *count == 0) {
		return KVS_VECTOR_ERR;
	}

	*out = heap[0];
	(*count)--;
	if (*count == 0) {
		return KVS_VECTOR_OK;
	}

	heap[0] = heap[*count];
	for (;;) {
		size_t left = pos * 2 + 1;
		size_t right = left + 1;
		size_t best = pos;

		if (left < *count &&
		    kvs_hnsw_v2_candidate_better(&heap[left], &heap[best])) {
			best = left;
		}
		if (right < *count &&
		    kvs_hnsw_v2_candidate_better(&heap[right], &heap[best])) {
			best = right;
		}
		if (best == pos) {
			break;
		}
		kvs_hnsw_v2_heap_swap(&heap[pos], &heap[best]);
		if (sift_count) {
			(*sift_count)++;
		}
		pos = best;
	}

	return KVS_VECTOR_OK;
}

static inline int kvs_hnsw_v2_top_heap_better_than_lower_bound(
	const kvs_hnsw_v2_candidate_t *item,
	const kvs_hnsw_v2_candidate_t *lower_bound
) {
	return kvs_hnsw_v2_candidate_better(item, lower_bound);
}

static inline float kvs_hnsw_v2_top_heap_lower_bound(
	const kvs_hnsw_v2_candidate_t *heap,
	size_t count
) {
	if (!heap || count == 0) {
		return -3.40282346638528859812e38F;
	}
	return heap[0].score;
}

static inline void kvs_hnsw_v2_top_heap_sift_up(
	kvs_hnsw_v2_candidate_t *heap,
	size_t pos,
	uint64_t *sift_count
) {
	while (pos > 0) {
		size_t parent = (pos - 1) / 2;

		if (!kvs_hnsw_v2_candidate_worse(&heap[pos], &heap[parent])) {
			break;
		}
		kvs_hnsw_v2_heap_swap(&heap[pos], &heap[parent]);
		if (sift_count) {
			(*sift_count)++;
		}
		pos = parent;
	}
}

static inline void kvs_hnsw_v2_top_heap_sift_down(
	kvs_hnsw_v2_candidate_t *heap,
	size_t count,
	size_t pos,
	uint64_t *sift_count
) {
	for (;;) {
		size_t left = pos * 2 + 1;
		size_t right = left + 1;
		size_t worst = pos;

		if (left < count &&
		    kvs_hnsw_v2_candidate_worse(&heap[left], &heap[worst])) {
			worst = left;
		}
		if (right < count &&
		    kvs_hnsw_v2_candidate_worse(&heap[right], &heap[worst])) {
			worst = right;
		}
		if (worst == pos) {
			break;
		}
		kvs_hnsw_v2_heap_swap(&heap[pos], &heap[worst]);
		if (sift_count) {
			(*sift_count)++;
		}
		pos = worst;
	}
}

static inline int kvs_hnsw_v2_top_heap_add_bounded(
	kvs_hnsw_v2_candidate_t *heap,
	size_t *count,
	size_t capacity,
	kvs_hnsw_v2_candidate_t item,
	int *did_replace,
	uint64_t *sift_count
) {
	if (did_replace) {
		*did_replace = 0;
	}
	if (!heap || !count || capacity == 0) {
		return KVS_VECTOR_ERR_INVALID_ARG;
	}

	if (*count < capacity) {
		size_t pos = *count;

		heap[pos] = item;
		(*count)++;
		kvs_hnsw_v2_top_heap_sift_up(heap, pos, sift_count);
		return KVS_VECTOR_OK;
	}

	if (!kvs_hnsw_v2_top_heap_better_than_lower_bound(&item, &heap[0])) {
		return KVS_VECTOR_ERR;
	}

	heap[0] = item;
	kvs_hnsw_v2_top_heap_sift_down(heap, *count, 0, sift_count);
	if (did_replace) {
		*did_replace = 1;
	}
	return KVS_VECTOR_OK;
}

static inline void kvs_hnsw_v2_heap_sort_desc(
	kvs_hnsw_v2_candidate_t *items,
	size_t count
) {
	size_t i;

	if (!items || count < 2) {
		return;
	}

	for (i = 1; i < count; i++) {
		kvs_hnsw_v2_candidate_t item = items[i];
		size_t j = i;

		while (j > 0 &&
		       kvs_hnsw_v2_candidate_better(&item, &items[j - 1])) {
			items[j] = items[j - 1];
			j--;
		}
		items[j] = item;
	}
}

#endif /* KVS_VECTOR_HNSW_V2_HEAP_H */
