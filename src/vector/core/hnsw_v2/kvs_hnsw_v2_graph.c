#include "vector/kvs_vector_hnsw_v2.h"

#include "kvs_vector_distance.h"

#include <stdlib.h>
#include <string.h>

const float *kvs_hnsw_v2_slot_vector(
	const kvs_hnsw_v2_index_t *index,
	uint32_t slot
) {
	if (!index || !index->vectors || slot >= index->capacity) {
		return NULL;
	}

	return index->vectors + ((size_t)slot * index->vector_stride);
}

uint32_t *kvs_hnsw_v2_layer_neighbors(
	kvs_hnsw_v2_index_t *index,
	uint32_t slot,
	uint8_t level
) {
	if (!index || slot >= index->capacity) {
		return NULL;
	}

	if (level == 0) {
		if (!index->level0_neighbors) {
			return NULL;
		}
		return index->level0_neighbors + ((size_t)slot * index->M0);
	}

	if (!index->upper_neighbors || level >= KVS_HNSW_V2_MAX_LEVEL) {
		return NULL;
	}

	return index->upper_neighbors +
		index->upper_offsets[slot] +
		((size_t)(level - 1) * index->M);
}

uint16_t *kvs_hnsw_v2_layer_count_ptr(
	kvs_hnsw_v2_index_t *index,
	uint32_t slot,
	uint8_t level
) {
	if (!index || slot >= index->capacity) {
		return NULL;
	}

	if (level == 0) {
		return index->level0_counts ? &index->level0_counts[slot] : NULL;
	}

	if (!index->upper_counts || level >= KVS_HNSW_V2_MAX_LEVEL) {
		return NULL;
	}

	return &index->upper_counts[
		(size_t)slot * (KVS_HNSW_V2_MAX_LEVEL - 1) + (size_t)(level - 1)
	];
}

size_t kvs_hnsw_v2_layer_capacity(
	const kvs_hnsw_v2_index_t *index,
	uint8_t level
) {
	if (!index) {
		return 0;
	}

	return level == 0 ? index->M0 : index->M;
}

int kvs_hnsw_v2_layer_has_edge(
	kvs_hnsw_v2_index_t *index,
	uint32_t from,
	uint32_t to,
	uint8_t level
) {
	uint32_t *neighbors;
	uint16_t *count_ptr;
	uint16_t i;

	neighbors = kvs_hnsw_v2_layer_neighbors(index, from, level);
	count_ptr = kvs_hnsw_v2_layer_count_ptr(index, from, level);
	if (!neighbors || !count_ptr) {
		return 0;
	}

	for (i = 0; i < *count_ptr; i++) {
		if (neighbors[i] == to) {
			return 1;
		}
	}

	return 0;
}

int kvs_hnsw_v2_layer_append_edge(
	kvs_hnsw_v2_index_t *index,
	uint32_t from,
	uint32_t to,
	uint8_t level
) {
	uint32_t *neighbors;
	uint16_t *count_ptr;
	size_t capacity;

	if (!index || from == to) {
		return KVS_VECTOR_ERR_INVALID_ARG;
	}

	neighbors = kvs_hnsw_v2_layer_neighbors(index, from, level);
	count_ptr = kvs_hnsw_v2_layer_count_ptr(index, from, level);
	capacity = kvs_hnsw_v2_layer_capacity(index, level);
	if (!neighbors || !count_ptr || capacity == 0) {
		return KVS_VECTOR_ERR_INVALID_ARG;
	}

	if (kvs_hnsw_v2_layer_has_edge(index, from, to, level)) {
		index->graph_stats.duplicate_edge_count++;
		return KVS_VECTOR_ERR;
	}

	if ((size_t)*count_ptr >= capacity) {
		index->graph_stats.degree_overflow_count++;
		return KVS_VECTOR_ERR;
	}

	neighbors[*count_ptr] = to;
	(*count_ptr)++;
	return KVS_VECTOR_OK;
}

float kvs_hnsw_v2_metric_score(
	const kvs_hnsw_v2_index_t *index,
	const float *lhs,
	const float *rhs
) {
	if (!index || !lhs || !rhs) {
		return 0.0f;
	}

	switch (index->metric) {
	case KVS_VECTOR_METRIC_L2:
		return -kvs_vector_distance_squared_l2(lhs, rhs, index->dim);
	case KVS_VECTOR_METRIC_COSINE:
		return kvs_vector_distance_cosine_score(lhs, rhs, index->dim);
	case KVS_VECTOR_METRIC_IP:
	default:
		return kvs_vector_distance_dot(lhs, rhs, index->dim);
	}
}

static void kvs_hnsw_v2_sort_u16(
	uint16_t *values,
	size_t count
) {
	size_t i;
	size_t j;

	for (i = 1; i < count; i++) {
		uint16_t value = values[i];
		j = i;
		while (j > 0 && values[j - 1] > value) {
			values[j] = values[j - 1];
			j--;
		}
		values[j] = value;
	}
}

static void kvs_hnsw_v2_fill_degree_stats(
	uint16_t *values,
	size_t count,
	kvs_hnsw_v2_degree_stats_t *out
) {
	uint64_t sum = 0;
	size_t i;

	if (!out) {
		return;
	}
	memset(out, 0, sizeof(*out));
	if (!values || count == 0) {
		return;
	}

	kvs_hnsw_v2_sort_u16(values, count);
	for (i = 0; i < count; i++) {
		sum += values[i];
	}

	out->min = values[0];
	out->p50 = values[count / 2];
	out->p95 = values[(count * 95) / 100 >= count ? count - 1 : (count * 95) / 100];
	out->max = values[count - 1];
	out->avg = (double)sum / (double)count;
}

void kvs_hnsw_v2_graph_stats_refresh(
	kvs_hnsw_v2_index_t *index
) {
	kvs_hnsw_v2_graph_stats_t counters;
	uint16_t stack_values[256];
	uint16_t *level0_values = stack_values;
	uint16_t *upper_values = NULL;
	size_t upper_count = 0;
	size_t i;
	size_t level;
	uint64_t isolated = 0;

	if (!index) {
		return;
	}

	counters = index->graph_stats;
	memset(&index->graph_stats, 0, sizeof(index->graph_stats));

	index->graph_stats.duplicate_edge_count = counters.duplicate_edge_count;
	index->graph_stats.degree_overflow_count = counters.degree_overflow_count;
	index->graph_stats.selected_neighbor_count = counters.selected_neighbor_count;
	index->graph_stats.forward_edge_added_count = counters.forward_edge_added_count;
	index->graph_stats.reverse_edge_attempt_count = counters.reverse_edge_attempt_count;
	index->graph_stats.reverse_edge_accepted_count = counters.reverse_edge_accepted_count;
	index->graph_stats.reverse_edge_rejected_count = counters.reverse_edge_rejected_count;
	index->graph_stats.reverse_prune_count = counters.reverse_prune_count;
	index->graph_stats.neighbor_select_call_count = counters.neighbor_select_call_count;
	index->graph_stats.neighbor_select_candidate_count = counters.neighbor_select_candidate_count;
	index->graph_stats.neighbor_select_selected_count = counters.neighbor_select_selected_count;
	index->graph_stats.neighbor_select_pruned_count = counters.neighbor_select_pruned_count;
	index->graph_stats.neighbor_select_duplicate_count = counters.neighbor_select_duplicate_count;
	index->graph_stats.neighbor_select_rejected_count = counters.neighbor_select_rejected_count;

	index->graph_stats.size = index->size;
	index->graph_stats.max_level = index->max_level;
	index->graph_stats.entry_point = index->entry_point;
	index->graph_stats.has_entry = index->has_entry;

	if (index->size > 256) {
		level0_values = NULL;
	}

	for (i = 0; i < index->size; i++) {
		uint8_t node_level = index->levels ? index->levels[i] : 0;
		uint16_t degree0 = index->level0_counts ? index->level0_counts[i] : 0;

		if (node_level >= KVS_HNSW_V2_MAX_LEVEL) {
			node_level = KVS_HNSW_V2_MAX_LEVEL - 1;
		}
		index->graph_stats.level_distribution[node_level]++;
		if (level0_values) {
			level0_values[i] = degree0;
		}
		if (degree0 == 0 && index->size > 1) {
			isolated++;
		}
		for (level = 1; level <= node_level; level++) {
			uint16_t *count_ptr = kvs_hnsw_v2_layer_count_ptr(
				index,
				(uint32_t)i,
				(uint8_t)level
			);
			if (count_ptr) {
				upper_count++;
			}
		}
	}

	if (!level0_values && index->size > 0) {
		level0_values = (uint16_t *)malloc(index->size * sizeof(uint16_t));
		if (level0_values) {
			for (i = 0; i < index->size; i++) {
				level0_values[i] = index->level0_counts[i];
			}
		}
	}

	if (upper_count > 0) {
		upper_values = (uint16_t *)malloc(upper_count * sizeof(uint16_t));
	}
	if (upper_values) {
		size_t pos = 0;
		for (i = 0; i < index->size; i++) {
			uint8_t node_level = index->levels ? index->levels[i] : 0;
			for (level = 1; level <= node_level; level++) {
				uint16_t *count_ptr = kvs_hnsw_v2_layer_count_ptr(
					index,
					(uint32_t)i,
					(uint8_t)level
				);
				if (count_ptr) {
					upper_values[pos++] = *count_ptr;
				}
			}
		}
	}

	kvs_hnsw_v2_fill_degree_stats(
		level0_values,
		index->size,
		&index->graph_stats.level0_degree
	);
	kvs_hnsw_v2_fill_degree_stats(
		upper_values,
		upper_count,
		&index->graph_stats.upper_degree
	);
	index->graph_stats.isolated_node_count = isolated;

	if (level0_values && level0_values != stack_values) {
		free(level0_values);
	}
	free(upper_values);
}
