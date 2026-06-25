#include "vector/kvs_vector_hnsw.h"

#include "kvs_vector.h"

#include <math.h>
#include <string.h>

static float kvs_hnsw_dot_score(
	const float *lhs,
	const float *rhs,
	size_t dim
) {
	float score = 0.0f;
	size_t i;

	for (i = 0; i < dim; i++) {
		score += lhs[i] * rhs[i];
	}

	return score;
}

static float kvs_hnsw_l2_score(
	const float *lhs,
	const float *rhs,
	size_t dim
) {
	float distance = 0.0f;
	size_t i;

	for (i = 0; i < dim; i++) {
		float diff = lhs[i] - rhs[i];
		distance += diff * diff;
	}

	return -distance;
}

static float kvs_hnsw_cosine_score(
	const float *lhs,
	const float *rhs,
	size_t dim
) {
	float dot = 0.0f;
	float lhs_norm = 0.0f;
	float rhs_norm = 0.0f;
	size_t i;

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

static float kvs_hnsw_metric_score(
	const kvs_hnsw_index_t *index,
	const float *lhs,
	const float *rhs
) {
	if (index->distance) {
		return index->distance(lhs, rhs, index->dim);
	}

	switch (index->metric) {
	case KVS_VECTOR_METRIC_L2:
		return kvs_hnsw_l2_score(lhs, rhs, index->dim);
	case KVS_VECTOR_METRIC_COSINE:
		return kvs_hnsw_cosine_score(lhs, rhs, index->dim);
	case KVS_VECTOR_METRIC_IP:
	default:
		return kvs_hnsw_dot_score(lhs, rhs, index->dim);
	}
}

static const float *kvs_hnsw_slot_vector(
	const kvs_hnsw_index_t *index,
	uint32_t slot
) {
	return index->vectors + ((size_t)slot * index->dim);
}

static uint32_t *kvs_hnsw_layer_neighbors(
	kvs_hnsw_index_t *index,
	uint32_t slot,
	uint8_t level
) {
	if (level == 0) {
		return index->level0_neighbors + ((size_t)slot * index->M0);
	}

	return index->upper_neighbors +
		(((size_t)slot * KVS_HNSW_MAX_LEVEL + level) * index->M);
}

static uint16_t *kvs_hnsw_layer_count_ptr(
	kvs_hnsw_index_t *index,
	uint32_t slot,
	uint8_t level
) {
	if (level == 0) {
		return &index->level0_counts[slot];
	}

	return &index->upper_counts[(size_t)slot * KVS_HNSW_MAX_LEVEL + level];
}

static int kvs_hnsw_slot_exists_at_level(
	const kvs_hnsw_index_t *index,
	uint32_t slot,
	uint8_t level
) {
	return (size_t)slot < index->size &&
		index->levels[slot] >= level;
}

static int kvs_hnsw_slot_searchable_at_level(
	const kvs_hnsw_index_t *index,
	uint32_t slot,
	uint8_t level
) {
	return kvs_hnsw_slot_exists_at_level(index, slot, level) &&
		!index->deleted_flags[slot];
}

static uint32_t kvs_hnsw_greedy_search_layer(
	kvs_hnsw_index_t *index,
	uint32_t entry,
	const float *query,
	uint8_t level
) {
	uint32_t current = entry;
	float current_score;
	int improved = 1;

	if (!kvs_hnsw_slot_exists_at_level(index, entry, level)) {
		return entry;
	}

	current_score = kvs_hnsw_metric_score(
		index,
		query,
		kvs_hnsw_slot_vector(index, current)
	);

	while (improved) {
		uint32_t *neighbors = kvs_hnsw_layer_neighbors(index, current, level);
		size_t count = *kvs_hnsw_layer_count_ptr(index, current, level);
		size_t i;

		improved = 0;
		for (i = 0; i < count; i++) {
			uint32_t neighbor = neighbors[i];
			float score;

			if (!kvs_hnsw_slot_searchable_at_level(index, neighbor, level)) {
				continue;
			}

			score = kvs_hnsw_metric_score(
				index,
				query,
				kvs_hnsw_slot_vector(index, neighbor)
			);
			if (score > current_score) {
				current = neighbor;
				current_score = score;
				improved = 1;
			}
		}
	}

	return current;
}

static void kvs_hnsw_top_heap_offer(
	kvs_vector_heap_t *heap,
	kvs_vector_heap_item_t item
) {
	const kvs_vector_heap_item_t *top;

	if (heap->size < heap->capacity) {
		(void)kvs_vector_heap_push(heap, item);
		return;
	}

	top = kvs_vector_heap_top(heap);
	if (top && item.score > top->score) {
		(void)kvs_vector_heap_replace_top(heap, item);
	}
}

static int kvs_hnsw_level0_search(
	kvs_hnsw_index_t *index,
	kvs_hnsw_search_context_t *ctx,
	uint32_t entry,
	const float *query,
	size_t topk,
	kvs_vector_hit_t *out,
	size_t *out_count
) {
	kvs_vector_heap_item_t item;
	uint32_t visit_tag;
	size_t ef;
	size_t hit_count = 0;

	ef = index->ef_search;
	if (ef < topk) {
		ef = topk;
	}
	if (ef > ctx->capacity) {
		ef = ctx->capacity;
	}

	kvs_hnsw_context_reset(ctx);
	visit_tag = kvs_hnsw_next_visit_tag(index);

	if (!kvs_hnsw_slot_exists_at_level(index, entry, 0)) {
		*out_count = 0;
		return KVS_VECTOR_OK;
	}

	index->visited_tags[entry] = visit_tag;
	item.record_id = index->slot_to_record_id[entry];
	item.slot = entry;
	item.score = kvs_hnsw_metric_score(
		index,
		query,
		kvs_hnsw_slot_vector(index, entry)
	);

	if (kvs_vector_heap_push(&ctx->candidate_heap, item) != KVS_VECTOR_OK) {
		return KVS_VECTOR_ERR;
	}
	if (!index->deleted_flags[entry]) {
		kvs_hnsw_top_heap_offer(&ctx->top_heap, item);
	}

	while (!kvs_vector_heap_empty(&ctx->candidate_heap)) {
		kvs_vector_heap_item_t current;
		const kvs_vector_heap_item_t *worst_top;
		uint32_t *neighbors;
		size_t neighbor_count;
		size_t i;

		if (kvs_vector_heap_pop(&ctx->candidate_heap, &current) != KVS_VECTOR_OK) {
			break;
		}

		worst_top = kvs_vector_heap_top(&ctx->top_heap);
		if (worst_top && ctx->top_heap.size >= ef &&
		    current.score < worst_top->score) {
			break;
		}

		neighbors = kvs_hnsw_layer_neighbors(index, current.slot, 0);
		neighbor_count = *kvs_hnsw_layer_count_ptr(index, current.slot, 0);
		for (i = 0; i < neighbor_count; i++) {
			uint32_t neighbor = neighbors[i];
			kvs_vector_heap_item_t next;

			if (!kvs_hnsw_slot_exists_at_level(index, neighbor, 0) ||
			    index->visited_tags[neighbor] == visit_tag) {
				continue;
			}
			index->visited_tags[neighbor] = visit_tag;

			next.record_id = index->slot_to_record_id[neighbor];
			next.slot = neighbor;
			next.score = kvs_hnsw_metric_score(
				index,
				query,
				kvs_hnsw_slot_vector(index, neighbor)
			);

			worst_top = kvs_vector_heap_top(&ctx->top_heap);
			if (ctx->top_heap.size < ef || !worst_top ||
			    next.score > worst_top->score) {
				if (ctx->candidate_heap.size < ctx->candidate_heap.capacity) {
					(void)kvs_vector_heap_push(&ctx->candidate_heap, next);
				}
				if (!index->deleted_flags[neighbor]) {
					kvs_hnsw_top_heap_offer(&ctx->top_heap, next);
				}
			}
		}
	}

	while (!kvs_vector_heap_empty(&ctx->top_heap) && hit_count < ctx->capacity) {
		kvs_vector_heap_item_t top;
		if (kvs_vector_heap_pop(&ctx->top_heap, &top) != KVS_VECTOR_OK) {
			break;
		}
		ctx->hits[hit_count].record_id = top.record_id;
		ctx->hits[hit_count].score = top.score;
		hit_count++;
	}

	for (size_t i = 1; i < hit_count; i++) {
		kvs_vector_hit_t hit = ctx->hits[i];
		size_t j = i;

		while (j > 0 && ctx->hits[j - 1].score < hit.score) {
			ctx->hits[j] = ctx->hits[j - 1];
			j--;
		}
		ctx->hits[j] = hit;
	}

	if (hit_count > topk) {
		hit_count = topk;
	}
	memcpy(out, ctx->hits, hit_count * sizeof(kvs_vector_hit_t));
	*out_count = hit_count;
	return KVS_VECTOR_OK;
}

int kvs_hnsw_index_search(
	kvs_hnsw_index_t *index,
	const float *query,
	size_t dim,
	size_t topk,
	kvs_vector_hit_t *out,
	size_t *out_count
) {
	kvs_hnsw_search_context_t *ctx;
	uint32_t curr;
	int level;
	int ret;

	if (!index || !query || !out || !out_count || dim == 0 || topk == 0) {
		return KVS_VECTOR_ERR_INVALID_ARG;
	}

	*out_count = 0;

	if (!index->ready || index->dim == 0 || dim != index->dim) {
		return KVS_VECTOR_ERR_INVALID_ARG;
	}

	if (!index->has_entry || index->size == 0) {
		return KVS_VECTOR_OK;
	}

	if (!index->vectors || !index->levels || !index->deleted_flags ||
	    !index->slot_to_record_id || !index->level0_neighbors ||
	    !index->level0_counts || !index->upper_neighbors ||
	    !index->upper_counts || !index->visited_tags) {
		return KVS_VECTOR_ERR_INVALID_ARG;
	}

	ctx = kvs_hnsw_context_acquire(&index->search_context_pool);
	if (!ctx) {
		return KVS_VECTOR_ERR;
	}

	curr = index->entry_point;
	for (level = (int)index->max_level; level >= 1; level--) {
		curr = kvs_hnsw_greedy_search_layer(index, curr, query, (uint8_t)level);
	}

	ret = kvs_hnsw_level0_search(index, ctx, curr, query, topk, out, out_count);
	kvs_hnsw_context_release(&index->search_context_pool, ctx);
	return ret;
}
