#include "vector/kvs_vector_hnsw_v2.h"

#include "kvs_hnsw_v2_heap.h"
#include "kvs_memory.h"

#include <stdint.h>
#include <stdlib.h>
#include <string.h>
#include <sys/time.h>

static uint64_t kvs_hnsw_v2_now_us(void) {
	struct timeval tv;

	gettimeofday(&tv, NULL);
	return (uint64_t)tv.tv_sec * 1000000ULL + (uint64_t)tv.tv_usec;
}

static int kvs_hnsw_v2_context_ensure(
	kvs_hnsw_v2_search_context_t *ctx,
	size_t candidate_capacity,
	size_t top_capacity,
	size_t scratch_capacity
) {
	kvs_hnsw_v2_candidate_t *items;

	if (!ctx) {
		return KVS_VECTOR_ERR_INVALID_ARG;
	}

	if (candidate_capacity > ctx->candidates_capacity) {
		items = (kvs_hnsw_v2_candidate_t *)kvs_malloc(
			candidate_capacity * sizeof(kvs_hnsw_v2_candidate_t)
		);
		if (!items) {
			return KVS_VECTOR_ERR;
		}
		kvs_free(ctx->candidates);
		ctx->candidates = items;
		ctx->candidates_capacity = candidate_capacity;
	}

	if (top_capacity > ctx->top_capacity) {
		items = (kvs_hnsw_v2_candidate_t *)kvs_malloc(
			top_capacity * sizeof(kvs_hnsw_v2_candidate_t)
		);
		if (!items) {
			return KVS_VECTOR_ERR;
		}
		kvs_free(ctx->top);
		ctx->top = items;
		ctx->top_capacity = top_capacity;
	}

	if (scratch_capacity > ctx->scratch_capacity) {
		items = (kvs_hnsw_v2_candidate_t *)kvs_malloc(
			scratch_capacity * sizeof(kvs_hnsw_v2_candidate_t)
		);
		if (!items) {
			return KVS_VECTOR_ERR;
		}
		kvs_free(ctx->scratch);
		ctx->scratch = items;
		ctx->scratch_capacity = scratch_capacity;
	}

	return KVS_VECTOR_OK;
}

static void kvs_hnsw_v2_next_visit_tag(
	kvs_hnsw_v2_index_t *index
) {
	if (!index || !index->visited_tags) {
		return;
	}

	index->current_visit_tag++;
	if (index->current_visit_tag == 0) {
		memset(index->visited_tags, 0, index->capacity * sizeof(uint32_t));
		index->current_visit_tag = 1;
	}
}

static int kvs_hnsw_v2_mark_visited(
	kvs_hnsw_v2_index_t *index,
	uint32_t slot,
	kvs_hnsw_v2_search_query_stats_t *stats
) {
	if (!index || !index->visited_tags || slot >= index->size) {
		return 0;
	}

	if (index->visited_tags[slot] == index->current_visit_tag) {
		return 0;
	}

	index->visited_tags[slot] = index->current_visit_tag;
	if (stats) {
		stats->visited_count++;
	}
	return 1;
}

static uint32_t kvs_hnsw_v2_greedy_layer(
	kvs_hnsw_v2_index_t *index,
	const float *query,
	uint32_t entry,
	uint8_t level,
	kvs_hnsw_v2_search_query_stats_t *stats
) {
	uint32_t current = entry;
	float current_score;
	int changed = 1;

	current_score = kvs_hnsw_v2_metric_score(
		index,
		query,
		kvs_hnsw_v2_slot_vector(index, current)
	);
	if (stats) {
		stats->distance_compute_count++;
		stats->metric_score_call_count++;
		stats->upper_distance_compute_count++;
	}

	while (changed) {
		uint32_t *neighbors;
		uint16_t *count_ptr;
		uint16_t i;

		changed = 0;
		neighbors = kvs_hnsw_v2_layer_neighbors(index, current, level);
		count_ptr = kvs_hnsw_v2_layer_count_ptr(index, current, level);
		if (!neighbors || !count_ptr) {
			break;
		}

		for (i = 0; i < *count_ptr; i++) {
			uint32_t neighbor = neighbors[i];
			const float *neighbor_vec;
			float score;

			if (neighbor >= index->size) {
				if (stats) {
					stats->deleted_skip_count++;
				}
				continue;
			}
			if (index->levels[neighbor] < level) {
				continue;
			}

			neighbor_vec = kvs_hnsw_v2_slot_vector(index, neighbor);
			if (!neighbor_vec) {
				continue;
			}

			score = kvs_hnsw_v2_metric_score(index, query, neighbor_vec);
			if (stats) {
				stats->distance_compute_count++;
				stats->metric_score_call_count++;
				stats->upper_distance_compute_count++;
			}
			if (score > current_score) {
				current = neighbor;
				current_score = score;
				changed = 1;
				if (stats) {
					stats->upper_greedy_step_count++;
				}
			}
		}
	}

	return current;
}

static int kvs_hnsw_v2_search_level0(
	kvs_hnsw_v2_index_t *index,
	const float *query,
	uint32_t entry,
	size_t ef_search,
	size_t candidate_capacity,
	kvs_hnsw_v2_search_context_t *ctx,
	kvs_hnsw_v2_search_query_stats_t *stats,
	size_t *out_top_count
) {
	kvs_hnsw_v2_candidate_t item;
	size_t candidate_count = 0;
	size_t top_count = 0;
	float entry_score;
	uint64_t start_us;
	uint64_t heap_sift_count = 0;

	if (!index || !query || !ctx || !out_top_count || ef_search == 0) {
		return KVS_VECTOR_ERR_INVALID_ARG;
	}

	entry_score = kvs_hnsw_v2_metric_score(
		index,
		query,
		kvs_hnsw_v2_slot_vector(index, entry)
	);
	if (stats) {
		stats->distance_compute_count++;
		stats->metric_score_call_count++;
		stats->level0_distance_compute_count++;
	}
	item.slot = entry;
	item.score = entry_score;
	kvs_hnsw_v2_mark_visited(index, entry, stats);
	start_us = kvs_hnsw_v2_now_us();
	if (kvs_hnsw_v2_candidate_heap_push(
		    ctx->candidates,
		    &candidate_count,
		    candidate_capacity,
		    item,
		    &heap_sift_count
	    ) != KVS_VECTOR_OK) {
		return KVS_VECTOR_ERR;
	}
	if (stats) {
		stats->candidate_push_count++;
		stats->candidate_select_us += kvs_hnsw_v2_now_us() - start_us;
	}
	if (!index->deleted_flags[entry]) {
		start_us = kvs_hnsw_v2_now_us();
		if (kvs_hnsw_v2_top_heap_add_bounded(
			    ctx->top,
			    &top_count,
			    ef_search,
			    item,
			    NULL,
			    &heap_sift_count
		    ) == KVS_VECTOR_OK) {
			if (stats) {
				stats->top_push_count++;
				stats->top_maintenance_us += kvs_hnsw_v2_now_us() - start_us;
			}
		} else if (stats) {
			stats->top_maintenance_us += kvs_hnsw_v2_now_us() - start_us;
		}
	} else if (stats) {
		stats->deleted_skip_count++;
	}

	while (candidate_count > 0) {
		kvs_hnsw_v2_candidate_t current;
		uint32_t *neighbors;
		uint16_t *count_ptr;
		uint16_t i;
		float lower_bound;

		start_us = kvs_hnsw_v2_now_us();
		if (kvs_hnsw_v2_candidate_heap_pop_best(
			    ctx->candidates,
			    &candidate_count,
			    &current,
			    &heap_sift_count
		    ) != KVS_VECTOR_OK) {
			break;
		}
		if (stats) {
			stats->candidate_pop_count++;
			stats->candidate_select_us += kvs_hnsw_v2_now_us() - start_us;
		}

		lower_bound = kvs_hnsw_v2_top_heap_lower_bound(ctx->top, top_count);
		if (top_count >= ef_search && current.score < lower_bound) {
			if (stats) {
				stats->early_stop_count++;
			}
			break;
		}

		neighbors = kvs_hnsw_v2_layer_neighbors(index, current.slot, 0);
		count_ptr = kvs_hnsw_v2_layer_count_ptr(index, current.slot, 0);
		if (!neighbors || !count_ptr) {
			continue;
		}

		for (i = 0; i < *count_ptr; i++) {
			uint32_t neighbor = neighbors[i];
			const float *neighbor_vec;
			float score;

			if (stats) {
				stats->level0_neighbor_scan_count++;
			}
			if (neighbor >= index->size) {
				continue;
			}
			if (!kvs_hnsw_v2_mark_visited(index, neighbor, stats)) {
				if (stats) {
					stats->candidate_skip_count++;
				}
				continue;
			}

			neighbor_vec = kvs_hnsw_v2_slot_vector(index, neighbor);
			if (!neighbor_vec) {
				continue;
			}
			score = kvs_hnsw_v2_metric_score(index, query, neighbor_vec);
			if (stats) {
				stats->distance_compute_count++;
				stats->metric_score_call_count++;
				stats->level0_distance_compute_count++;
			}

			lower_bound = kvs_hnsw_v2_top_heap_lower_bound(ctx->top, top_count);
			if (top_count < ef_search || score > lower_bound) {
				kvs_hnsw_v2_candidate_t candidate;
				int top_replaced = 0;
				int candidate_is_deleted;

				candidate.slot = neighbor;
				candidate.score = score;
				candidate_is_deleted = index->deleted_flags[neighbor] != 0;
				start_us = kvs_hnsw_v2_now_us();
				if (kvs_hnsw_v2_candidate_heap_push(
					    ctx->candidates,
					    &candidate_count,
					    candidate_capacity,
					    candidate,
					    &heap_sift_count
				    ) == KVS_VECTOR_OK) {
					if (stats) {
						stats->candidate_push_count++;
					}
				}
				if (stats) {
					stats->candidate_select_us += kvs_hnsw_v2_now_us() - start_us;
				}
				if (candidate_is_deleted) {
					if (stats) {
						stats->deleted_skip_count++;
					}
					continue;
				}
				start_us = kvs_hnsw_v2_now_us();
				if (kvs_hnsw_v2_top_heap_add_bounded(
					    ctx->top,
					    &top_count,
					    ef_search,
					    candidate,
					    &top_replaced,
					    &heap_sift_count
				    ) == KVS_VECTOR_OK) {
					if (stats) {
						if (top_replaced) {
							stats->top_replace_count++;
						} else {
							stats->top_push_count++;
						}
						stats->top_maintenance_us +=
							kvs_hnsw_v2_now_us() - start_us;
					}
				} else if (stats) {
					stats->top_maintenance_us +=
						kvs_hnsw_v2_now_us() - start_us;
				}
			} else if (stats) {
				stats->candidate_skip_count++;
			}
		}
	}

	*out_top_count = top_count;
	return KVS_VECTOR_OK;
}

static void kvs_hnsw_v2_record_search_stats(
	kvs_hnsw_v2_index_t *index,
	const kvs_hnsw_v2_search_query_stats_t *query_stats,
	size_t ef_search
) {
	kvs_hnsw_v2_search_stats_t *stats;

	if (!index || !query_stats) {
		return;
	}

	stats = &index->search_stats;
	stats->search_query_count++;
	stats->search_ef_search = ef_search;
	stats->search_visited_count_total += query_stats->visited_count;
	stats->search_distance_compute_count_total +=
		query_stats->distance_compute_count;
	stats->search_metric_score_call_count_total +=
		query_stats->metric_score_call_count;
	stats->search_upper_distance_compute_count_total +=
		query_stats->upper_distance_compute_count;
	stats->search_level0_distance_compute_count_total +=
		query_stats->level0_distance_compute_count;
	stats->search_candidate_push_count += query_stats->candidate_push_count;
	stats->search_candidate_pop_count += query_stats->candidate_pop_count;
	stats->search_candidate_skip_count += query_stats->candidate_skip_count;
	stats->search_top_push_count += query_stats->top_push_count;
	stats->search_top_replace_count += query_stats->top_replace_count;
	stats->search_early_stop_count += query_stats->early_stop_count;
	stats->search_deleted_skip_count += query_stats->deleted_skip_count;
	stats->search_upper_greedy_step_count += query_stats->upper_greedy_step_count;
	stats->search_level0_neighbor_scan_count +=
		query_stats->level0_neighbor_scan_count;
	stats->search_upper_greedy_us += query_stats->upper_greedy_us;
	stats->search_level0_search_us += query_stats->level0_search_us;
	stats->search_candidate_select_us += query_stats->candidate_select_us;
	stats->search_top_maintenance_us += query_stats->top_maintenance_us;
	stats->search_result_sort_us += query_stats->result_sort_us;
	stats->search_context_acquire_release_us +=
		query_stats->context_acquire_release_us;
	stats->search_total_us += query_stats->elapsed_us;
	stats->search_avg_us = stats->search_query_count ?
		stats->search_total_us / stats->search_query_count : 0;
	stats->last_visited_count = query_stats->visited_count;
	stats->last_distance_compute_count = query_stats->distance_compute_count;
	stats->last_metric_score_call_count = query_stats->metric_score_call_count;
	stats->last_upper_distance_compute_count =
		query_stats->upper_distance_compute_count;
	stats->last_level0_distance_compute_count =
		query_stats->level0_distance_compute_count;
	stats->last_candidate_push_count = query_stats->candidate_push_count;
	stats->last_candidate_pop_count = query_stats->candidate_pop_count;
	stats->last_candidate_skip_count = query_stats->candidate_skip_count;
	stats->last_top_push_count = query_stats->top_push_count;
	stats->last_top_replace_count = query_stats->top_replace_count;
	stats->last_early_stop_count = query_stats->early_stop_count;
	stats->last_deleted_skip_count = query_stats->deleted_skip_count;
	stats->last_upper_greedy_step_count = query_stats->upper_greedy_step_count;
	stats->last_level0_neighbor_scan_count =
		query_stats->level0_neighbor_scan_count;
	stats->last_upper_greedy_us = query_stats->upper_greedy_us;
	stats->last_level0_search_us = query_stats->level0_search_us;
	stats->last_candidate_select_us = query_stats->candidate_select_us;
	stats->last_top_maintenance_us = query_stats->top_maintenance_us;
	stats->last_result_sort_us = query_stats->result_sort_us;
	stats->last_context_acquire_release_us =
		query_stats->context_acquire_release_us;
}

int kvs_hnsw_v2_index_search(
	kvs_hnsw_v2_index_t *index,
	const float *query,
	size_t dim,
	size_t topk,
	size_t ef_search,
	kvs_vector_hit_t *hits,
	size_t *out_count
) {
	kvs_hnsw_v2_search_context_t *ctx;
	kvs_hnsw_v2_search_query_stats_t query_stats;
	uint64_t start_us;
	uint64_t total_start_us;
	uint32_t entry;
	size_t effective_ef;
	size_t candidate_capacity;
	size_t top_count = 0;
	size_t result_count;
	size_t i;
	int ret;

	if (out_count) {
		*out_count = 0;
	}
	if (!index || !query || !hits || !out_count || dim != index->dim) {
		return KVS_VECTOR_ERR_INVALID_ARG;
	}
	if (!index->ready) {
		return KVS_VECTOR_ERR_NOT_INITIALIZED;
	}
	if (topk == 0) {
		return KVS_VECTOR_OK;
	}
	if (!index->has_entry || index->size == 0) {
		return KVS_VECTOR_OK;
	}
	if (!index->context_pool.contexts || index->context_pool.count == 0) {
		return KVS_VECTOR_ERR_NOT_INITIALIZED;
	}

	memset(&query_stats, 0, sizeof(query_stats));
	total_start_us = kvs_hnsw_v2_now_us();
	effective_ef = ef_search == 0 ? index->ef_search : ef_search;
	if (effective_ef < topk) {
		effective_ef = topk;
	}
	if (effective_ef > index->size) {
		effective_ef = index->size;
	}
	if (effective_ef == 0) {
		return KVS_VECTOR_OK;
	}
	candidate_capacity = index->size;

	ctx = &index->context_pool.contexts[0];
	start_us = kvs_hnsw_v2_now_us();
	ret = kvs_hnsw_v2_context_ensure(
		ctx,
		candidate_capacity,
		effective_ef,
		effective_ef
	);
	if (ret != KVS_VECTOR_OK) {
		return ret;
	}
	query_stats.context_acquire_release_us += kvs_hnsw_v2_now_us() - start_us;

	start_us = kvs_hnsw_v2_now_us();
	kvs_hnsw_v2_next_visit_tag(index);
	query_stats.context_acquire_release_us += kvs_hnsw_v2_now_us() - start_us;

	entry = index->entry_point;
	if (index->deleted_count >= index->size) {
		return KVS_VECTOR_OK;
	}
	if (entry >= index->size) {
		return KVS_VECTOR_OK;
	}

	if (index->max_level > 0) {
		uint8_t level;

		for (level = index->max_level; level > 0; level--) {
			start_us = kvs_hnsw_v2_now_us();
			entry = kvs_hnsw_v2_greedy_layer(
				index,
				query,
				entry,
				level,
				&query_stats
			);
			query_stats.upper_greedy_us += kvs_hnsw_v2_now_us() - start_us;
		}
	}

	start_us = kvs_hnsw_v2_now_us();
	ret = kvs_hnsw_v2_search_level0(
		index,
		query,
		entry,
		effective_ef,
		candidate_capacity,
		ctx,
		&query_stats,
		&top_count
	);
	if (ret != KVS_VECTOR_OK) {
		return ret;
	}
	query_stats.level0_search_us += kvs_hnsw_v2_now_us() - start_us;

	start_us = kvs_hnsw_v2_now_us();
	kvs_hnsw_v2_heap_sort_desc(ctx->top, top_count);
	query_stats.result_sort_us += kvs_hnsw_v2_now_us() - start_us;
	result_count = 0;
	for (i = 0; i < top_count && result_count < topk; i++) {
		uint32_t slot = ctx->top[i].slot;

		if (slot >= index->size || index->deleted_flags[slot]) {
			query_stats.deleted_skip_count++;
			continue;
		}
		hits[result_count].record_id = index->slot_to_record_id[slot];
		hits[result_count].score = ctx->top[i].score;
		result_count++;
	}

	query_stats.elapsed_us = kvs_hnsw_v2_now_us() - total_start_us;
	kvs_hnsw_v2_record_search_stats(index, &query_stats, effective_ef);
	*out_count = result_count;
	return KVS_VECTOR_OK;
}
