#include "vector/kvs_vector_flat.h"
#include "vector/kvs_vector_heap.h"

#include "kvs_vector.h"
#include "kvs_memory.h"

#include <math.h>
#include <string.h>

static float kvs_flat_dot_score(
	const float *lhs,
	const float *rhs,
	size_t dim
) {
	size_t i;
	float score = 0.0f;

	for (i = 0; i < dim; i++) {
		score += lhs[i] * rhs[i];
	}

	return score;
}

static float kvs_flat_l2_score(
	const float *lhs,
	const float *rhs,
	size_t dim
) {
	size_t i;
	float dist = 0.0f;

	for (i = 0; i < dim; i++) {
		float d = lhs[i] - rhs[i];
		dist += d * d;
	}

	/*
	 * KVLite core convention:
	 *   larger score is better.
	 *
	 * For L2, smaller distance is better, so expose negative squared distance.
	 */
	return -dist;
}

static float kvs_flat_cosine_score(
	const float *lhs,
	const float *rhs,
	size_t dim
) {
	size_t i;
	float dot = 0.0f;
	float lhs_norm = 0.0f;
	float rhs_norm = 0.0f;

	for (i = 0; i < dim; i++) {
		dot += lhs[i] * rhs[i];
		lhs_norm += lhs[i] * lhs[i];
		rhs_norm += rhs[i] * rhs[i];
	}

	if (lhs_norm <= 0.0f || rhs_norm <= 0.0f) {
		return 0.0f;
	}

	return dot / (sqrtf(lhs_norm) * sqrtf(rhs_norm));
}

static float kvs_flat_metric_score(
	int metric,
	const float *lhs,
	const float *rhs,
	size_t dim
) {
	switch (metric) {
	case KVS_VECTOR_METRIC_L2:
		return kvs_flat_l2_score(lhs, rhs, dim);
	case KVS_VECTOR_METRIC_COSINE:
		return kvs_flat_cosine_score(lhs, rhs, dim);
	case KVS_VECTOR_METRIC_IP:
	default:
		return kvs_flat_dot_score(lhs, rhs, dim);
	}
}

int kvs_flat_index_search(
	kvs_flat_index_t *index,
	const float *query,
	size_t dim,
	size_t topk,
	kvs_vector_hit_t *out,
	size_t *out_count
) {
	kvs_vector_heap_t heap;
	kvs_vector_heap_item_t *tmp = NULL;
	size_t i;
	size_t n = 0;
	int ret;

	if (!out_count) {
		return KVS_VECTOR_ERR_INVALID_ARG;
	}

	*out_count = 0;

	if (!index || !query || !out || topk == 0) {
		return KVS_VECTOR_ERR_INVALID_ARG;
	}

	if (!index->ready || !index->vectors || !index->slot_to_record_id ||
	    !index->deleted_flags || dim != index->dim) {
		return KVS_VECTOR_ERR_INVALID_ARG;
	}

	memset(&heap, 0, sizeof(heap));

	ret = kvs_vector_heap_init(&heap, topk, kvs_vector_heap_min_score_less);
	if (ret != KVS_VECTOR_OK) {
		return ret;
	}

	for (i = 0; i < index->size; i++) {
		kvs_vector_heap_item_t item;
		const kvs_vector_heap_item_t *top;
		const float *vector;
		float score;

		if (index->deleted_flags[i]) {
			continue;
		}

		vector = index->vectors + (i * index->dim);
		score = kvs_flat_metric_score(index->metric, query, vector, index->dim);

		item.record_id = index->slot_to_record_id[i];
		item.slot = (uint32_t)i;
		item.score = score;

		if (heap.size < heap.capacity) {
			ret = kvs_vector_heap_push(&heap, item);
			if (ret != KVS_VECTOR_OK) {
				kvs_vector_heap_destroy(&heap);
				return ret;
			}
			continue;
		}

		top = kvs_vector_heap_top(&heap);
		if (top && item.score > top->score) {
			ret = kvs_vector_heap_replace_top(&heap, item);
			if (ret != KVS_VECTOR_OK) {
				kvs_vector_heap_destroy(&heap);
				return ret;
			}
		}
	}

	if (heap.size == 0) {
		kvs_vector_heap_destroy(&heap);
		return KVS_VECTOR_OK;
	}

	tmp = (kvs_vector_heap_item_t *)kvs_malloc(
		heap.size * sizeof(kvs_vector_heap_item_t)
	);
	if (!tmp) {
		kvs_vector_heap_destroy(&heap);
		return KVS_VECTOR_ERR;
	}

	while (!kvs_vector_heap_empty(&heap)) {
		ret = kvs_vector_heap_pop(&heap, &tmp[n]);
		if (ret != KVS_VECTOR_OK) {
			kvs_free(tmp);
			kvs_vector_heap_destroy(&heap);
			return ret;
		}
		n++;
	}

	/*
	 * Min-heap pop gives ascending score.
	 * Export in descending score order.
	 */
	for (i = 0; i < n; i++) {
		size_t src = n - 1 - i;

		out[i].record_id = tmp[src].record_id;
		out[i].score = tmp[src].score;
	}

	*out_count = n;

	kvs_free(tmp);
	kvs_vector_heap_destroy(&heap);
	return KVS_VECTOR_OK;
}