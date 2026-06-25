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

static size_t kvs_hnsw_layer_capacity(
	const kvs_hnsw_index_t *index,
	uint8_t level
) {
	return level == 0 ? index->M0 : index->M;
}

static int kvs_hnsw_slot_active_at_level(
	const kvs_hnsw_index_t *index,
	uint32_t slot,
	uint8_t level
) {
	return (size_t)slot < index->size &&
		!index->deleted_flags[slot] &&
		index->levels[slot] >= level;
}

static int kvs_hnsw_layer_has_neighbor(
	kvs_hnsw_index_t *index,
	uint32_t slot,
	uint32_t neighbor,
	uint8_t level
) {
	uint32_t *neighbors;
	size_t count;
	size_t i;

	if (!index || (size_t)slot >= index->size) {
		return 0;
	}

	neighbors = kvs_hnsw_layer_neighbors(index, slot, level);
	count = *kvs_hnsw_layer_count_ptr(index, slot, level);

	for (i = 0; i < count; i++) {
		if (neighbors[i] == neighbor) {
			return 1;
		}
	}

	return 0;
}

static void kvs_hnsw_layer_remove_neighbor(
	kvs_hnsw_index_t *index,
	uint32_t slot,
	uint32_t neighbor,
	uint8_t level
) {
	uint32_t *neighbors;
	uint16_t *count_ptr;
	size_t count;
	size_t i;

	if (!index || (size_t)slot >= index->size) {
		return;
	}

	neighbors = kvs_hnsw_layer_neighbors(index, slot, level);
	count_ptr = kvs_hnsw_layer_count_ptr(index, slot, level);
	count = *count_ptr;

	for (i = 0; i < count; i++) {
		if (neighbors[i] != neighbor) {
			continue;
		}

		if (i + 1 < count) {
			memmove(
				&neighbors[i],
				&neighbors[i + 1],
				(count - i - 1) * sizeof(uint32_t)
			);
		}
		neighbors[count - 1] = 0;
		*count_ptr = (uint16_t)(count - 1);
		return;
	}
}

static int kvs_hnsw_layer_append_or_replace(
	kvs_hnsw_index_t *index,
	uint32_t slot,
	uint32_t neighbor,
	uint8_t level,
	int *out_accepted,
	uint32_t *out_removed
) {
	uint32_t *neighbors;
	uint16_t *count_ptr;
	size_t count;
	size_t capacity;
	size_t worst_pos = 0;
	float worst_score = 0.0f;
	const float *slot_vec;
	const float *neighbor_vec;
	float neighbor_score;
	size_t i;

	if (!index || slot == neighbor ||
	    !kvs_hnsw_slot_active_at_level(index, slot, level) ||
	    !kvs_hnsw_slot_active_at_level(index, neighbor, level)) {
		return KVS_VECTOR_ERR_INVALID_ARG;
	}

	if (out_accepted) {
		*out_accepted = 0;
	}
	if (out_removed) {
		*out_removed = UINT32_MAX;
	}

	if (kvs_hnsw_layer_has_neighbor(index, slot, neighbor, level)) {
		if (out_accepted) {
			*out_accepted = 1;
		}
		return KVS_VECTOR_OK;
	}

	neighbors = kvs_hnsw_layer_neighbors(index, slot, level);
	count_ptr = kvs_hnsw_layer_count_ptr(index, slot, level);
	count = *count_ptr;
	capacity = kvs_hnsw_layer_capacity(index, level);
	slot_vec = kvs_hnsw_slot_vector(index, slot);
	neighbor_vec = kvs_hnsw_slot_vector(index, neighbor);
	neighbor_score = kvs_hnsw_metric_score(index, slot_vec, neighbor_vec);

	if (count < capacity) {
		neighbors[count] = neighbor;
		*count_ptr = (uint16_t)(count + 1);
		if (out_accepted) {
			*out_accepted = 1;
		}
		return KVS_VECTOR_OK;
	}

	for (i = 0; i < count; i++) {
		const float *current_vec = kvs_hnsw_slot_vector(index, neighbors[i]);
		float score = kvs_hnsw_metric_score(index, slot_vec, current_vec);

		if (i == 0 || score < worst_score) {
			worst_score = score;
			worst_pos = i;
		}
	}

	if (neighbor_score > worst_score) {
		if (out_removed) {
			*out_removed = neighbors[worst_pos];
		}
		neighbors[worst_pos] = neighbor;
		if (out_accepted) {
			*out_accepted = 1;
		}
	}

	return KVS_VECTOR_OK;
}

static int kvs_hnsw_layer_would_accept(
	kvs_hnsw_index_t *index,
	uint32_t slot,
	uint32_t neighbor,
	uint8_t level
) {
	uint32_t *neighbors;
	size_t count;
	size_t capacity;
	float worst_score = 0.0f;
	float neighbor_score;
	const float *slot_vec;
	const float *neighbor_vec;
	size_t i;

	if (!index || slot == neighbor ||
	    !kvs_hnsw_slot_active_at_level(index, slot, level) ||
	    !kvs_hnsw_slot_active_at_level(index, neighbor, level)) {
		return 0;
	}

	if (kvs_hnsw_layer_has_neighbor(index, slot, neighbor, level)) {
		return 1;
	}

	count = *kvs_hnsw_layer_count_ptr(index, slot, level);
	capacity = kvs_hnsw_layer_capacity(index, level);
	if (count < capacity) {
		return 1;
	}

	neighbors = kvs_hnsw_layer_neighbors(index, slot, level);
	slot_vec = kvs_hnsw_slot_vector(index, slot);
	neighbor_vec = kvs_hnsw_slot_vector(index, neighbor);
	neighbor_score = kvs_hnsw_metric_score(index, slot_vec, neighbor_vec);

	for (i = 0; i < count; i++) {
		const float *current_vec = kvs_hnsw_slot_vector(index, neighbors[i]);
		float score = kvs_hnsw_metric_score(index, slot_vec, current_vec);

		if (i == 0 || score < worst_score) {
			worst_score = score;
		}
	}

	return neighbor_score > worst_score;
}

static int kvs_hnsw_add_bidirectional_edge(
	kvs_hnsw_index_t *index,
	uint32_t a,
	uint32_t b,
	uint8_t level
) {
	int accepted_ab = 0;
	int accepted_ba = 0;
	uint32_t removed_ab = UINT32_MAX;
	uint32_t removed_ba = UINT32_MAX;
	int ret;

	if (a == b) {
		return KVS_VECTOR_OK;
	}

	if (!kvs_hnsw_layer_would_accept(index, a, b, level) ||
	    !kvs_hnsw_layer_would_accept(index, b, a, level)) {
		return KVS_VECTOR_OK;
	}

	ret = kvs_hnsw_layer_append_or_replace(
		index,
		a,
		b,
		level,
		&accepted_ab,
		&removed_ab
	);
	if (ret != KVS_VECTOR_OK) {
		return ret;
	}

	ret = kvs_hnsw_layer_append_or_replace(
		index,
		b,
		a,
		level,
		&accepted_ba,
		&removed_ba
	);
	if (ret != KVS_VECTOR_OK) {
		return ret;
	}

	if (removed_ab != UINT32_MAX) {
		kvs_hnsw_layer_remove_neighbor(index, removed_ab, a, level);
	}
	if (removed_ba != UINT32_MAX) {
		kvs_hnsw_layer_remove_neighbor(index, removed_ba, b, level);
	}

	return KVS_VECTOR_OK;
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

	if (!kvs_hnsw_slot_active_at_level(index, entry, level)) {
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

			if (!kvs_hnsw_slot_active_at_level(index, neighbor, level)) {
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

static int kvs_hnsw_search_layer_for_add(
	kvs_hnsw_index_t *index,
	kvs_hnsw_search_context_t *ctx,
	uint32_t entry,
	const float *query,
	uint8_t level,
	size_t *out_count
) {
	kvs_vector_heap_item_t item;
	uint32_t visit_tag;
	size_t count = 0;

	if (!index || !ctx || !out_count ||
	    !kvs_hnsw_slot_active_at_level(index, entry, level)) {
		return KVS_VECTOR_ERR_INVALID_ARG;
	}

	kvs_hnsw_context_reset(ctx);
	visit_tag = kvs_hnsw_next_visit_tag(index);
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
	if (kvs_vector_heap_push(&ctx->top_heap, item) != KVS_VECTOR_OK) {
		return KVS_VECTOR_ERR;
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
		if (worst_top && ctx->top_heap.size >= index->ef_construction &&
		    current.score < worst_top->score) {
			break;
		}

		neighbors = kvs_hnsw_layer_neighbors(index, current.slot, level);
		neighbor_count = *kvs_hnsw_layer_count_ptr(index, current.slot, level);

		for (i = 0; i < neighbor_count; i++) {
			uint32_t neighbor = neighbors[i];
			kvs_vector_heap_item_t next;

			if (!kvs_hnsw_slot_active_at_level(index, neighbor, level) ||
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
			if (ctx->top_heap.size < index->ef_construction ||
			    !worst_top || next.score > worst_top->score) {
				kvs_hnsw_top_heap_offer(&ctx->top_heap, next);
				if (ctx->candidate_heap.size < ctx->candidate_heap.capacity) {
					(void)kvs_vector_heap_push(&ctx->candidate_heap, next);
				}
			}
		}
	}

	while (!kvs_vector_heap_empty(&ctx->top_heap) && count < ctx->capacity) {
		kvs_vector_heap_item_t top;
		if (kvs_vector_heap_pop(&ctx->top_heap, &top) != KVS_VECTOR_OK) {
			break;
		}
		ctx->neighbor_buffer[count] = top.slot;
		ctx->distance_buffer[count] = top.score;
		count++;
	}

	for (size_t i = 1; i < count; i++) {
		uint32_t slot = ctx->neighbor_buffer[i];
		float score = ctx->distance_buffer[i];
		size_t j = i;

		while (j > 0 && ctx->distance_buffer[j - 1] < score) {
			ctx->neighbor_buffer[j] = ctx->neighbor_buffer[j - 1];
			ctx->distance_buffer[j] = ctx->distance_buffer[j - 1];
			j--;
		}
		ctx->neighbor_buffer[j] = slot;
		ctx->distance_buffer[j] = score;
	}

	*out_count = count;
	return KVS_VECTOR_OK;
}

static int kvs_hnsw_connect_layer_from_context(
	kvs_hnsw_index_t *index,
	uint32_t slot,
	kvs_hnsw_search_context_t *ctx,
	size_t candidate_count,
	uint8_t level
) {
	size_t limit = candidate_count;
	size_t capacity = kvs_hnsw_layer_capacity(index, level);
	size_t i;

	if (limit > capacity) {
		limit = capacity;
	}

	for (i = 0; i < limit; i++) {
		uint32_t neighbor = ctx->neighbor_buffer[i];
		int ret;

		if (neighbor == slot) {
			continue;
		}

		ret = kvs_hnsw_add_bidirectional_edge(index, slot, neighbor, level);
		if (ret != KVS_VECTOR_OK) {
			return ret;
		}
	}

	return KVS_VECTOR_OK;
}

static void kvs_hnsw_clear_slot_edges(
	kvs_hnsw_index_t *index,
	uint32_t slot
) {
	if ((size_t)slot >= index->capacity) {
		return;
	}

	memset(
		index->level0_neighbors + ((size_t)slot * index->M0),
		0,
		index->M0 * sizeof(uint32_t)
	);
	index->level0_counts[slot] = 0;

	memset(
		index->upper_neighbors +
			((size_t)slot * KVS_HNSW_MAX_LEVEL * index->M),
		0,
		KVS_HNSW_MAX_LEVEL * index->M * sizeof(uint32_t)
	);
	memset(
		index->upper_counts + ((size_t)slot * KVS_HNSW_MAX_LEVEL),
		0,
		KVS_HNSW_MAX_LEVEL * sizeof(uint16_t)
	);
}

int kvs_hnsw_index_add(
	kvs_hnsw_index_t *index,
	uint64_t record_id,
	const float *vector,
	size_t dim
) {
	kvs_hnsw_search_context_t *ctx = NULL;
	uint32_t existing_slot = 0;
	uint32_t slot;
	uint32_t curr;
	uint8_t new_level;
	uint8_t old_max_level;
	uint8_t connect_start_level;
	int level;
	size_t candidate_count = 0;
	int ret;

	if (!index || !vector || dim == 0) {
		return KVS_VECTOR_ERR_INVALID_ARG;
	}

	if (!index->ready || index->dim == 0 || dim != index->dim) {
		return KVS_VECTOR_ERR_INVALID_ARG;
	}

	if (!index->vectors || !index->levels || !index->deleted_flags ||
	    !index->slot_to_record_id || !index->level0_neighbors ||
	    !index->level0_counts || !index->upper_neighbors ||
	    !index->upper_counts || !index->record_id_to_slot.entries) {
		return KVS_VECTOR_ERR_INVALID_ARG;
	}

	ret = kvs_vector_id_slot_map_get(
		&index->record_id_to_slot,
		record_id,
		&existing_slot
	);
	if (ret == KVS_VECTOR_OK) {
		if ((size_t)existing_slot >= index->size) {
			return KVS_VECTOR_ERR;
		}

		memcpy(
			index->vectors + ((size_t)existing_slot * index->dim),
			vector,
			index->dim * sizeof(float)
		);

		if (index->deleted_flags[existing_slot]) {
			index->deleted_flags[existing_slot] = 0;
			if (index->deleted_count > 0) {
				index->deleted_count--;
			}
		}

		index->slot_to_record_id[existing_slot] = record_id;
		return KVS_VECTOR_OK;
	}
	if (ret != KVS_VECTOR_ERR_NOT_FOUND) {
		return ret;
	}

	if (index->size >= index->capacity || index->size > UINT32_MAX) {
		return KVS_VECTOR_ERR;
	}

	slot = (uint32_t)index->size;
	new_level = kvs_hnsw_index_next_level(index);
	if (new_level >= KVS_HNSW_MAX_LEVEL) {
		new_level = KVS_HNSW_MAX_LEVEL - 1;
	}
	old_max_level = index->max_level;
	curr = index->entry_point;

	memcpy(
		index->vectors + ((size_t)slot * index->dim),
		vector,
		index->dim * sizeof(float)
	);
	index->levels[slot] = new_level;
	index->deleted_flags[slot] = 0;
	index->slot_to_record_id[slot] = record_id;
	kvs_hnsw_clear_slot_edges(index, slot);

	ret = kvs_vector_id_slot_map_put(&index->record_id_to_slot, record_id, slot);
	if (ret != KVS_VECTOR_OK) {
		memset(index->vectors + ((size_t)slot * index->dim), 0, index->dim * sizeof(float));
		index->levels[slot] = 0;
		index->deleted_flags[slot] = 0;
		index->slot_to_record_id[slot] = 0;
		kvs_hnsw_clear_slot_edges(index, slot);
		return ret;
	}

	index->size++;

	if (!index->has_entry) {
		index->entry_point = slot;
		index->has_entry = 1;
		index->max_level = new_level;
		index->ready = 1;
		index->needs_rebuild = 0;
		return KVS_VECTOR_OK;
	}

	ctx = kvs_hnsw_context_acquire(&index->search_context_pool);
	if (!ctx) {
		index->needs_rebuild = 1;
		return KVS_VECTOR_ERR;
	}

	for (level = (int)old_max_level; level > (int)new_level; level--) {
		curr = kvs_hnsw_greedy_search_layer(index, curr, vector, (uint8_t)level);
	}

	connect_start_level = new_level < old_max_level ? new_level : old_max_level;
	for (level = (int)connect_start_level; level >= 0; level--) {
		ret = kvs_hnsw_search_layer_for_add(
			index,
			ctx,
			curr,
			vector,
			(uint8_t)level,
			&candidate_count
		);
		if (ret != KVS_VECTOR_OK) {
			kvs_hnsw_context_release(&index->search_context_pool, ctx);
			index->needs_rebuild = 1;
			return ret;
		}

		ret = kvs_hnsw_connect_layer_from_context(
			index,
			slot,
			ctx,
			candidate_count,
			(uint8_t)level
		);
		if (ret != KVS_VECTOR_OK) {
			kvs_hnsw_context_release(&index->search_context_pool, ctx);
			index->needs_rebuild = 1;
			return ret;
		}

		if (candidate_count > 0) {
			curr = ctx->neighbor_buffer[0];
		}
	}

	kvs_hnsw_context_release(&index->search_context_pool, ctx);

	if (new_level > old_max_level) {
		index->entry_point = slot;
		index->max_level = new_level;
	}

	index->ready = 1;
	index->needs_rebuild = 0;
	return KVS_VECTOR_OK;
}
