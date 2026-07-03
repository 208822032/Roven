#define KVS_VECTOR_INTERNAL
#include "kvs_vector.h"
#include "kvs_vector_distance.h"
#include "vector/kvs_vector_rebuild.h"

#ifdef ENABLE_FAISS_BASELINE
#include "kvs_vector_faiss_backend.h"
#endif

#include "kvs_memory.h"

#include <stdio.h>
#include <stdlib.h>
#include <time.h>

kvs_vector_state_t g_vector_state;
static kvs_vector_vset_commit_stats_t g_vset_commit_stats;
size_t g_faiss_add_count;
size_t g_faiss_search_count;
size_t g_hnswlib_add_count;
size_t g_hnswlib_search_count;

static __thread uint64_t g_vsearch_current_resolve_us;

static uint64_t kvs_vector_stats_add(uint64_t *field, uint64_t value) {
	return __atomic_add_fetch(field, value, __ATOMIC_RELAXED);
}

static uint64_t kvs_vector_stats_load(const uint64_t *field) {
	return __atomic_load_n(field, __ATOMIC_RELAXED);
}

static void kvs_vector_stats_update_max(uint64_t *field, uint64_t value) {
	uint64_t old_value = __atomic_load_n(field, __ATOMIC_RELAXED);

	while (value > old_value) {
		if (__atomic_compare_exchange_n(field,
		                                &old_value,
		                                value,
		                                0,
		                                __ATOMIC_RELAXED,
		                                __ATOMIC_RELAXED)) {
			break;
		}
	}
}

static void kvs_vector_manager_mark_rebuild_capture_failed(
	int error_code,
	const char *message
) {
	if (!kvs_vector_rebuild_is_capture_active(&g_vector_state.rebuild)) {
		return;
	}

	if (kvs_vector_rebuild_fail(
		    &g_vector_state.rebuild,
		    error_code,
		    message
	    ) != KVS_VECTOR_OK) {
		g_vector_state.rebuild.state = KVS_VECTOR_REBUILD_FAILED;
		g_vector_state.rebuild.error_code = error_code;
		if (message && message[0]) {
			snprintf(
				g_vector_state.rebuild.error_message,
				sizeof(g_vector_state.rebuild.error_message),
				"%s",
				message
			);
		} else {
			g_vector_state.rebuild.error_message[0] = '\0';
		}
		g_vector_state.rebuild.cancel_requested = 0;
	}
}

int kvs_vector_manager_capture_upsert_delta(
	uint64_t record_id,
	const float *embedding,
	int embedding_dim
) {
	int ret;

	if (!kvs_vector_rebuild_is_capture_active(&g_vector_state.rebuild)) {
		return KVS_VECTOR_OK;
	}
	if (!embedding || embedding_dim <= 0) {
		kvs_vector_manager_mark_rebuild_capture_failed(
			KVS_VECTOR_ERR_INVALID_ARG,
			"invalid upsert delta"
		);
		return KVS_VECTOR_ERR_INVALID_ARG;
	}

	ret = kvs_vector_rebuild_append_upsert_delta(
		&g_vector_state.rebuild,
		record_id,
		embedding,
		(size_t)embedding_dim
	);
	if (ret != KVS_VECTOR_OK) {
		kvs_vector_manager_mark_rebuild_capture_failed(
			ret,
			"append upsert delta failed"
		);
	}
	return ret;
}

int kvs_vector_manager_capture_delete_delta(
	uint64_t record_id
) {
	int ret;

	if (!kvs_vector_rebuild_is_capture_active(&g_vector_state.rebuild)) {
		return KVS_VECTOR_OK;
	}

	ret = kvs_vector_rebuild_append_delete_delta(
		&g_vector_state.rebuild,
		record_id
	);
	if (ret != KVS_VECTOR_OK) {
		kvs_vector_manager_mark_rebuild_capture_failed(
			ret,
			"append delete delta failed"
		);
	}
	return ret;
}

int kvs_vector_manager_rebuild_capture_begin_for_test(void) {
	if (!g_vector_state.initialized) {
		return KVS_VECTOR_ERR_NOT_INITIALIZED;
	}
	return kvs_vector_rebuild_begin(
		&g_vector_state.rebuild,
		&g_vector_state.current_index,
		(void *)1
	);
}

int kvs_vector_manager_rebuild_capture_stop_for_test(void) {
	if (!g_vector_state.initialized) {
		return KVS_VECTOR_ERR_NOT_INITIALIZED;
	}
	if (kvs_vector_rebuild_is_capture_active(&g_vector_state.rebuild)) {
		return kvs_vector_rebuild_cancel(&g_vector_state.rebuild);
	}
	return kvs_vector_rebuild_reset_idle(&g_vector_state.rebuild);
}

static uint64_t kvs_vector_now_us(void) {
	struct timespec ts;

	if (clock_gettime(CLOCK_MONOTONIC, &ts) != 0) {
		return 0;
	}
	return (uint64_t)ts.tv_sec * 1000000ull + (uint64_t)ts.tv_nsec / 1000ull;
}

static int kvs_vector_cmp_u16(
	const void *lhs,
	const void *rhs
) {
	uint16_t a = *(const uint16_t *)lhs;
	uint16_t b = *(const uint16_t *)rhs;

	return (a > b) - (a < b);
}

static size_t kvs_vector_hnsw_context_pool_memory_bytes(
	const kvs_hnsw_search_context_pool_t *pool
) {
	size_t per_context;

	if (!pool || !pool->contexts || pool->count == 0) {
		return 0;
	}

	per_context = sizeof(kvs_hnsw_search_context_t);
	per_context += pool->context_capacity * sizeof(kvs_vector_heap_item_t) * 2;
	per_context += pool->context_capacity * sizeof(uint32_t);
	per_context += pool->context_capacity * sizeof(float);
	per_context += pool->context_capacity * sizeof(kvs_vector_hit_t);

	return pool->count * per_context;
}

static void kvs_vector_hnsw_fill_layout_audit(
	const kvs_hnsw_index_t *hnsw,
	kvs_vector_info_t *info
) {
	size_t level0_cache_bytes;
	size_t upper_slots;
	size_t slot_state_bytes;

	if (!hnsw || !info) {
		return;
	}

	info->core_hnsw_memory_vectors_bytes =
		hnsw->capacity * hnsw->dim * sizeof(float);
	info->core_hnsw_memory_level0_neighbors_bytes =
		kvs_hnsw_graph_level0_bytes(hnsw->capacity, hnsw->M0);
	info->core_hnsw_memory_level0_scores_bytes =
		hnsw->capacity * hnsw->M0 * sizeof(float);
	upper_slots = hnsw->capacity * KVS_HNSW_MAX_LEVEL;
	info->core_hnsw_memory_upper_neighbors_bytes =
		upper_slots * hnsw->M * sizeof(uint32_t);
	info->core_hnsw_memory_visited_tags_bytes =
		hnsw->capacity * sizeof(uint32_t);
	info->core_hnsw_memory_context_bytes =
		kvs_vector_hnsw_context_pool_memory_bytes(&hnsw->search_context_pool);

	level0_cache_bytes = hnsw->capacity * sizeof(uint16_t);
	level0_cache_bytes += hnsw->capacity * sizeof(float);
	level0_cache_bytes += hnsw->capacity * sizeof(uint8_t);

	slot_state_bytes = hnsw->capacity * sizeof(uint8_t);
	slot_state_bytes += hnsw->capacity * sizeof(uint8_t);
	slot_state_bytes += hnsw->capacity * sizeof(uint64_t);
	slot_state_bytes += hnsw->record_id_to_slot.capacity *
		sizeof(kvs_vector_id_slot_entry_t);
	slot_state_bytes += hnsw->capacity * sizeof(uint16_t);
	slot_state_bytes += level0_cache_bytes;
	slot_state_bytes += hnsw->capacity * sizeof(size_t);
	slot_state_bytes += upper_slots * sizeof(uint16_t);
	info->core_hnsw_memory_slot_state_bytes = slot_state_bytes;

	if (hnsw->level0_counts && hnsw->size > 0) {
		uint16_t *counts;
		uint64_t total = 0;
		size_t i;

		counts = (uint16_t *)kvs_malloc(hnsw->size * sizeof(uint16_t));
		if (counts) {
			for (i = 0; i < hnsw->size; i++) {
				counts[i] = hnsw->level0_counts[i];
				total += counts[i];
			}
			qsort(counts, hnsw->size, sizeof(uint16_t), kvs_vector_cmp_u16);
			info->core_hnsw_level0_count_min = counts[0];
			info->core_hnsw_level0_count_avg = total / (uint64_t)hnsw->size;
			info->core_hnsw_level0_count_p50 = counts[hnsw->size / 2];
			info->core_hnsw_level0_count_p95 =
				counts[(hnsw->size * 95) / 100];
			info->core_hnsw_level0_count_max = counts[hnsw->size - 1];
			kvs_free(counts);
		}
	}
}

static void kvs_vector_record_timing(
	uint64_t *count,
	uint64_t *total_us,
	uint64_t *max_us,
	uint64_t elapsed_us
) {
	kvs_vector_stats_add(count, 1);
	kvs_vector_stats_add(total_us, elapsed_us);
	kvs_vector_stats_update_max(max_us, elapsed_us);
}

void kvs_vector_vset_commit_stats_get(kvs_vector_vset_commit_stats_t *out) {
	if (!out) {
		return;
	}
	out->vset_commit_count = kvs_vector_stats_load(&g_vset_commit_stats.vset_commit_count);
	out->vset_commit_total_us = kvs_vector_stats_load(&g_vset_commit_stats.vset_commit_total_us);
	out->vset_commit_max_us = kvs_vector_stats_load(&g_vset_commit_stats.vset_commit_max_us);
	out->vset_record_lookup_count = kvs_vector_stats_load(&g_vset_commit_stats.vset_record_lookup_count);
	out->vset_record_lookup_total_us = kvs_vector_stats_load(&g_vset_commit_stats.vset_record_lookup_total_us);
	out->vset_record_lookup_max_us = kvs_vector_stats_load(&g_vset_commit_stats.vset_record_lookup_max_us);
	out->vset_record_insert_or_update_count = kvs_vector_stats_load(&g_vset_commit_stats.vset_record_insert_or_update_count);
	out->vset_record_insert_or_update_total_us = kvs_vector_stats_load(&g_vset_commit_stats.vset_record_insert_or_update_total_us);
	out->vset_record_insert_or_update_max_us = kvs_vector_stats_load(&g_vset_commit_stats.vset_record_insert_or_update_max_us);
	out->vset_vector_copy_count = kvs_vector_stats_load(&g_vset_commit_stats.vset_vector_copy_count);
	out->vset_vector_copy_total_us = kvs_vector_stats_load(&g_vset_commit_stats.vset_vector_copy_total_us);
	out->vset_vector_copy_max_us = kvs_vector_stats_load(&g_vset_commit_stats.vset_vector_copy_max_us);
	out->vset_flat_index_update_count = kvs_vector_stats_load(&g_vset_commit_stats.vset_flat_index_update_count);
	out->vset_flat_index_update_total_us = kvs_vector_stats_load(&g_vset_commit_stats.vset_flat_index_update_total_us);
	out->vset_flat_index_update_max_us = kvs_vector_stats_load(&g_vset_commit_stats.vset_flat_index_update_max_us);
	out->vset_backend_add_count = kvs_vector_stats_load(&g_vset_commit_stats.vset_backend_add_count);
	out->vset_backend_add_total_us = kvs_vector_stats_load(&g_vset_commit_stats.vset_backend_add_total_us);
	out->vset_backend_add_max_us = kvs_vector_stats_load(&g_vset_commit_stats.vset_backend_add_max_us);
	out->vset_id_map_update_count = kvs_vector_stats_load(&g_vset_commit_stats.vset_id_map_update_count);
	out->vset_id_map_update_total_us = kvs_vector_stats_load(&g_vset_commit_stats.vset_id_map_update_total_us);
	out->vset_id_map_update_max_us = kvs_vector_stats_load(&g_vset_commit_stats.vset_id_map_update_max_us);
	out->vset_scope_answer_question_update_count = kvs_vector_stats_load(&g_vset_commit_stats.vset_scope_answer_question_update_count);
	out->vset_scope_answer_question_update_total_us = kvs_vector_stats_load(&g_vset_commit_stats.vset_scope_answer_question_update_total_us);
	out->vset_scope_answer_question_update_max_us = kvs_vector_stats_load(&g_vset_commit_stats.vset_scope_answer_question_update_max_us);
	out->vset_lock_wait_count = kvs_vector_stats_load(&g_vset_commit_stats.vset_lock_wait_count);
	out->vset_lock_wait_total_us = kvs_vector_stats_load(&g_vset_commit_stats.vset_lock_wait_total_us);
	out->vset_lock_wait_max_us = kvs_vector_stats_load(&g_vset_commit_stats.vset_lock_wait_max_us);
	out->vset_lock_hold_count = kvs_vector_stats_load(&g_vset_commit_stats.vset_lock_hold_count);
	out->vset_lock_hold_total_us = kvs_vector_stats_load(&g_vset_commit_stats.vset_lock_hold_total_us);
	out->vset_lock_hold_max_us = kvs_vector_stats_load(&g_vset_commit_stats.vset_lock_hold_max_us);
	out->vset_aof_append_count = kvs_vector_stats_load(&g_vset_commit_stats.vset_aof_append_count);
	out->vset_aof_append_total_us = kvs_vector_stats_load(&g_vset_commit_stats.vset_aof_append_total_us);
	out->vset_aof_append_max_us = kvs_vector_stats_load(&g_vset_commit_stats.vset_aof_append_max_us);
	out->vset_replication_count = kvs_vector_stats_load(&g_vset_commit_stats.vset_replication_count);
	out->vset_replication_total_us = kvs_vector_stats_load(&g_vset_commit_stats.vset_replication_total_us);
	out->vset_replication_max_us = kvs_vector_stats_load(&g_vset_commit_stats.vset_replication_max_us);
	out->vset_dirty_mark_count = kvs_vector_stats_load(&g_vset_commit_stats.vset_dirty_mark_count);
	out->vset_dirty_mark_total_us = kvs_vector_stats_load(&g_vset_commit_stats.vset_dirty_mark_total_us);
	out->vset_dirty_mark_max_us = kvs_vector_stats_load(&g_vset_commit_stats.vset_dirty_mark_max_us);
	out->vset_response_build_count = kvs_vector_stats_load(&g_vset_commit_stats.vset_response_build_count);
	out->vset_response_build_total_us = kvs_vector_stats_load(&g_vset_commit_stats.vset_response_build_total_us);
	out->vset_response_build_max_us = kvs_vector_stats_load(&g_vset_commit_stats.vset_response_build_max_us);
	out->vset_response_push_count = kvs_vector_stats_load(&g_vset_commit_stats.vset_response_push_count);
	out->vset_response_push_total_us = kvs_vector_stats_load(&g_vset_commit_stats.vset_response_push_total_us);
	out->vset_response_push_max_us = kvs_vector_stats_load(&g_vset_commit_stats.vset_response_push_max_us);
	out->vsearch_search_count = kvs_vector_stats_load(&g_vset_commit_stats.vsearch_search_count);
	out->vsearch_search_total_us = kvs_vector_stats_load(&g_vset_commit_stats.vsearch_search_total_us);
	out->vsearch_search_max_us = kvs_vector_stats_load(&g_vset_commit_stats.vsearch_search_max_us);
	out->vsearch_backend_search_count = kvs_vector_stats_load(&g_vset_commit_stats.vsearch_backend_search_count);
	out->vsearch_backend_search_total_us = kvs_vector_stats_load(&g_vset_commit_stats.vsearch_backend_search_total_us);
	out->vsearch_backend_search_max_us = kvs_vector_stats_load(&g_vset_commit_stats.vsearch_backend_search_max_us);
	out->vsearch_manager_resolve_count = kvs_vector_stats_load(&g_vset_commit_stats.vsearch_manager_resolve_count);
	out->vsearch_manager_resolve_total_us = kvs_vector_stats_load(&g_vset_commit_stats.vsearch_manager_resolve_total_us);
	out->vsearch_manager_resolve_max_us = kvs_vector_stats_load(&g_vset_commit_stats.vsearch_manager_resolve_max_us);
	out->vsearch_raw_hit_count = kvs_vector_stats_load(&g_vset_commit_stats.vsearch_raw_hit_count);
	out->vsearch_resolved_hit_count = kvs_vector_stats_load(&g_vset_commit_stats.vsearch_resolved_hit_count);
	out->vsearch_filtered_hit_count = kvs_vector_stats_load(&g_vset_commit_stats.vsearch_filtered_hit_count);
	out->vsearch_response_build_count = kvs_vector_stats_load(&g_vset_commit_stats.vsearch_response_build_count);
	out->vsearch_response_build_total_us = kvs_vector_stats_load(&g_vset_commit_stats.vsearch_response_build_total_us);
	out->vsearch_response_build_max_us = kvs_vector_stats_load(&g_vset_commit_stats.vsearch_response_build_max_us);
	out->vsearch_response_push_count = kvs_vector_stats_load(&g_vset_commit_stats.vsearch_response_push_count);
	out->vsearch_response_push_total_us = kvs_vector_stats_load(&g_vset_commit_stats.vsearch_response_push_total_us);
	out->vsearch_response_push_max_us = kvs_vector_stats_load(&g_vset_commit_stats.vsearch_response_push_max_us);
	out->flat_search_core_count = kvs_vector_stats_load(&g_vset_commit_stats.flat_search_core_count);
	out->flat_search_core_total_us = kvs_vector_stats_load(&g_vset_commit_stats.flat_search_core_total_us);
	out->flat_search_core_max_us = kvs_vector_stats_load(&g_vset_commit_stats.flat_search_core_max_us);
	out->flat_search_export_count = kvs_vector_stats_load(&g_vset_commit_stats.flat_search_export_count);
	out->flat_search_export_total_us = kvs_vector_stats_load(&g_vset_commit_stats.flat_search_export_total_us);
	out->flat_search_export_max_us = kvs_vector_stats_load(&g_vset_commit_stats.flat_search_export_max_us);
	out->hnsw_search_core_count = kvs_vector_stats_load(&g_vset_commit_stats.hnsw_search_core_count);
	out->hnsw_search_core_total_us = kvs_vector_stats_load(&g_vset_commit_stats.hnsw_search_core_total_us);
	out->hnsw_search_core_max_us = kvs_vector_stats_load(&g_vset_commit_stats.hnsw_search_core_max_us);
	out->hnsw_search_export_count = kvs_vector_stats_load(&g_vset_commit_stats.hnsw_search_export_count);
	out->hnsw_search_export_total_us = kvs_vector_stats_load(&g_vset_commit_stats.hnsw_search_export_total_us);
	out->hnsw_search_export_max_us = kvs_vector_stats_load(&g_vset_commit_stats.hnsw_search_export_max_us);
}

void kvs_vector_vset_record_lock_wait(uint64_t elapsed_us) {
	kvs_vector_record_timing(
		&g_vset_commit_stats.vset_lock_wait_count,
		&g_vset_commit_stats.vset_lock_wait_total_us,
		&g_vset_commit_stats.vset_lock_wait_max_us,
		elapsed_us
	);
}

void kvs_vector_vset_record_lock_hold(uint64_t elapsed_us) {
	kvs_vector_record_timing(
		&g_vset_commit_stats.vset_lock_hold_count,
		&g_vset_commit_stats.vset_lock_hold_total_us,
		&g_vset_commit_stats.vset_lock_hold_max_us,
		elapsed_us
	);
}

void kvs_vector_vset_record_aof_append(uint64_t elapsed_us) {
	kvs_vector_record_timing(
		&g_vset_commit_stats.vset_aof_append_count,
		&g_vset_commit_stats.vset_aof_append_total_us,
		&g_vset_commit_stats.vset_aof_append_max_us,
		elapsed_us
	);
}

void kvs_vector_vset_record_replication(uint64_t elapsed_us) {
	kvs_vector_record_timing(
		&g_vset_commit_stats.vset_replication_count,
		&g_vset_commit_stats.vset_replication_total_us,
		&g_vset_commit_stats.vset_replication_max_us,
		elapsed_us
	);
}

void kvs_vector_vset_record_dirty_mark(uint64_t elapsed_us) {
	kvs_vector_record_timing(
		&g_vset_commit_stats.vset_dirty_mark_count,
		&g_vset_commit_stats.vset_dirty_mark_total_us,
		&g_vset_commit_stats.vset_dirty_mark_max_us,
		elapsed_us
	);
}

void kvs_vector_vset_record_response_build(uint64_t elapsed_us) {
	kvs_vector_record_timing(
		&g_vset_commit_stats.vset_response_build_count,
		&g_vset_commit_stats.vset_response_build_total_us,
		&g_vset_commit_stats.vset_response_build_max_us,
		elapsed_us
	);
}

void kvs_vector_vset_record_response_push(uint64_t elapsed_us) {
	kvs_vector_record_timing(
		&g_vset_commit_stats.vset_response_push_count,
		&g_vset_commit_stats.vset_response_push_total_us,
		&g_vset_commit_stats.vset_response_push_max_us,
		elapsed_us
	);
}

void kvs_vector_vsearch_record_search(uint64_t elapsed_us) {
	kvs_vector_record_timing(
		&g_vset_commit_stats.vsearch_search_count,
		&g_vset_commit_stats.vsearch_search_total_us,
		&g_vset_commit_stats.vsearch_search_max_us,
		elapsed_us
	);
}

void kvs_vector_vsearch_record_backend_search(uint64_t elapsed_us) {
	kvs_vector_record_timing(
		&g_vset_commit_stats.vsearch_backend_search_count,
		&g_vset_commit_stats.vsearch_backend_search_total_us,
		&g_vset_commit_stats.vsearch_backend_search_max_us,
		elapsed_us
	);
}

void kvs_vector_vsearch_record_manager_resolve(
	uint64_t elapsed_us,
	size_t raw_hit_count,
	size_t resolved_hit_count
) {
	kvs_vector_record_timing(
		&g_vset_commit_stats.vsearch_manager_resolve_count,
		&g_vset_commit_stats.vsearch_manager_resolve_total_us,
		&g_vset_commit_stats.vsearch_manager_resolve_max_us,
		elapsed_us
	);
	kvs_vector_stats_add(&g_vset_commit_stats.vsearch_raw_hit_count, raw_hit_count);
	kvs_vector_stats_add(&g_vset_commit_stats.vsearch_resolved_hit_count, resolved_hit_count);
	if (raw_hit_count > resolved_hit_count) {
		kvs_vector_stats_add(
			&g_vset_commit_stats.vsearch_filtered_hit_count,
			raw_hit_count - resolved_hit_count
		);
	}
}

void kvs_vector_vsearch_record_response_build(uint64_t elapsed_us) {
	kvs_vector_record_timing(
		&g_vset_commit_stats.vsearch_response_build_count,
		&g_vset_commit_stats.vsearch_response_build_total_us,
		&g_vset_commit_stats.vsearch_response_build_max_us,
		elapsed_us
	);
}

void kvs_vector_vsearch_record_response_push(uint64_t elapsed_us) {
	kvs_vector_record_timing(
		&g_vset_commit_stats.vsearch_response_push_count,
		&g_vset_commit_stats.vsearch_response_push_total_us,
		&g_vset_commit_stats.vsearch_response_push_max_us,
		elapsed_us
	);
}

void kvs_vector_flat_record_search_core(uint64_t elapsed_us) {
	kvs_vector_record_timing(
		&g_vset_commit_stats.flat_search_core_count,
		&g_vset_commit_stats.flat_search_core_total_us,
		&g_vset_commit_stats.flat_search_core_max_us,
		elapsed_us
	);
}

void kvs_vector_flat_record_search_export(uint64_t elapsed_us) {
	kvs_vector_record_timing(
		&g_vset_commit_stats.flat_search_export_count,
		&g_vset_commit_stats.flat_search_export_total_us,
		&g_vset_commit_stats.flat_search_export_max_us,
		elapsed_us
	);
}

void kvs_vector_hnsw_record_search_core(uint64_t elapsed_us) {
	kvs_vector_record_timing(
		&g_vset_commit_stats.hnsw_search_core_count,
		&g_vset_commit_stats.hnsw_search_core_total_us,
		&g_vset_commit_stats.hnsw_search_core_max_us,
		elapsed_us
	);
}

void kvs_vector_hnsw_record_search_export(uint64_t elapsed_us) {
	kvs_vector_record_timing(
		&g_vset_commit_stats.hnsw_search_export_count,
		&g_vset_commit_stats.hnsw_search_export_total_us,
		&g_vset_commit_stats.hnsw_search_export_max_us,
		elapsed_us
	);
}

// /*
//  * 重置 VINFO 统计结构。
//  *
//  * 当前返回空统计，保证 VINFO 后续接入时即使没有真实索引，
//  * 也不会读到未初始化字段。
//  */
// static void kvs_vector_info_reset(kvs_vector_info_t *info) {
//     if (!info) return;
//     memset(info, 0, sizeof(kvs_vector_info_t));

//  info->total_records = 0;
// 	info->active_records = 0;
// 	info->deleted_records = 0;
// 	info->capacity = 0;
// 	info->index_size = 0;
// 	info->dimension = 0;
// 	info->index_ready = 0;
// 	info->needs_rebuild = 0;
// }

/*
 * 统一检查 vector 模块是否已经初始化。
 */
static int kvs_vector_validate_initialized(void) {
	if (!g_vector_state.initialized) {
		return KVS_VECTOR_ERR_NOT_INITIALIZED;
	}

	return KVS_VECTOR_OK;
}

/*
 * 在线性 records 数组中查找 active record。
 *
 * 当前第 5 步先不使用 id_map。
 * 后续数据量变大后，再把这里替换成 hash lookup。
 */
static kvs_vector_record_t *kvs_vector_find_record(
	const char *question,
	size_t question_len,
	const char *scope,
	size_t scope_len
) {
	size_t record_index = 0;
	int ret;

	ret = kvs_vector_id_map_get(
		&g_vector_state.id_map,
		question,
		question_len,
		scope,
		scope_len,
		&record_index
	);
	if (ret != KVS_VECTOR_OK) {
		return NULL;
	}

	if (record_index >= g_vector_state.store.capacity) {
		return NULL;
	}

	if (!kvs_vector_record_match(
			&g_vector_state.store.records[record_index],
			question,
			question_len,
			scope,
			scope_len)) {
		return NULL;
	}

	return &g_vector_state.store.records[record_index];
}

/*
 * 查找可写入的空槽。
 *
 * 当前第 5 步还没有 VDEL，但这里兼容后续 DELETED 槽位复用。
 */
static kvs_vector_record_t *kvs_vector_find_free_slot(void) {
	kvs_vector_store_t *store = &g_vector_state.store;
	size_t record_index = 0;
	int pop_ret;
	int ret;

	if (!store->records || store->capacity == 0) {
		return NULL;
	}

	pop_ret = kvs_vector_store_free_list_pop(store, &record_index);
	if (pop_ret == 1) {
		return &store->records[record_index];
	}

	if (store->next_slot >= store->capacity) {
		ret = kvs_vector_store_ensure_capacity(store, store->next_slot + 1);
		if (ret != KVS_VECTOR_OK) {
			return NULL;
		}
	}

	record_index = store->next_slot++;
	if (record_index >= store->capacity) {
		return NULL;
	}

	return &store->records[record_index];
}

static kvs_vector_index_type_t kvs_vector_index_type_from_config(void) {
	if (strcasecmp(g_cfg.vector_index_type, "native_hnsw") == 0) {
		KVS_LOG_ERROR(KVS_VECTOR_LOG_MODULE, "native_hnsw has been removed; use hnswlib_hnsw instead");
		return -1;
	}
	if (strcasecmp(g_cfg.vector_index_type, "flat") == 0 ||
	    strcasecmp(g_cfg.vector_index_type, "core_flat") == 0) {
		return KVS_VECTOR_INDEX_CORE_FLAT;
	}
	if (strcasecmp(g_cfg.vector_index_type, "core_hnsw") == 0) {
		return KVS_VECTOR_INDEX_CORE_HNSW;
	}
	if (strcasecmp(g_cfg.vector_index_type, "core_hnsw_v2") == 0) {
		return KVS_VECTOR_INDEX_CORE_HNSW_V2;
	}
#ifdef ENABLE_HNSWLIB
	if (strcasecmp(g_cfg.vector_index_type, "hnsw") == 0 ||
	    strcasecmp(g_cfg.vector_index_type, "hnswlib_hnsw") == 0) {
		return KVS_VECTOR_INDEX_HNSWLIB_HNSW;
	}
#endif
	if (strcasecmp(g_cfg.vector_index_type, "hnswlib_hnsw") == 0) {
		KVS_LOG_ERROR(KVS_VECTOR_LOG_MODULE, "hnswlib backend requested but not compiled (ENABLE_HNSWLIB=0)");
	}

	if (strcasecmp(g_cfg.vector_index_type, "sq8_flat") == 0) {
		return KVS_VECTOR_INDEX_TYPE_SQ8_FLAT;
	}

#ifdef ENABLE_FAISS_BASELINE
	if (strcasecmp(g_cfg.vector_index_type, "faiss_hnsw") == 0) {
		return KVS_VECTOR_INDEX_FAISS_HNSW;
	}

	if (strcasecmp(g_cfg.vector_index_type, "faiss_flat") == 0) {
		return KVS_VECTOR_INDEX_FAISS_FLAT;
	}
#endif

	if (strcasecmp(g_cfg.vector_index_type, "faiss_hnsw") == 0 ||
	    strcasecmp(g_cfg.vector_index_type, "faiss_flat") == 0) {
		KVS_LOG_ERROR(KVS_VECTOR_LOG_MODULE,
			"faiss backend requested but not compiled (ENABLE_FAISS_BASELINE=0)");
		/* return flat to let init continue; caller should detect this */
	}

	return KVS_VECTOR_INDEX_TYPE_FLAT;
}

static const char *kvs_vector_index_type_name(kvs_vector_index_type_t type) {
	switch (type) {
	case KVS_VECTOR_INDEX_TYPE_HNSW:
		return "hnsw";
	case KVS_VECTOR_INDEX_TYPE_SQ8_FLAT:
		return "sq8_flat";
	case KVS_VECTOR_INDEX_FAISS_HNSW:
		return "faiss_hnsw";
	case KVS_VECTOR_INDEX_FAISS_FLAT:
		return "faiss_flat";
	case KVS_VECTOR_INDEX_HNSWLIB_HNSW:
		return "hnswlib_hnsw";
	case KVS_VECTOR_INDEX_CORE_FLAT:
		return "core_flat";
	case KVS_VECTOR_INDEX_CORE_HNSW:
		return "core_hnsw";
	case KVS_VECTOR_INDEX_CORE_HNSW_V2:
		return "core_hnsw_v2";
	case KVS_VECTOR_INDEX_TYPE_FLAT:
	default:
		return "flat";
	}
}

static size_t kvs_vector_provider_dimension_or_default(void) {
	size_t dimension = kvs_vector_embedding_provider_dimension();

	if (dimension == 0) {
		return KVS_VECTOR_DEFAULT_DIMENSION;
	}

	return dimension;
}

static size_t kvs_vector_hnsw_param_or_default(int value, size_t def) {
	if (value <= 0) {
		return def;
	}

	return (size_t)value;
}

static size_t kvs_vector_hnsw_param_or_min_default(int value, size_t def) {
	if (value <= 0 || (size_t)value < def) {
		return def;
	}

	return (size_t)value;
}

static void kvs_vector_configure_core_hnsw_v2(kvs_hnsw_v2_index_t *index) {
	if (!index) {
		return;
	}

	index->M = kvs_vector_hnsw_param_or_min_default(
		g_cfg.vector_hnsw_m,
		KVS_HNSW_V2_DEFAULT_M
	);
	index->M0 = index->M * 2;
	if (index->M0 < KVS_HNSW_V2_DEFAULT_M0) {
		index->M0 = KVS_HNSW_V2_DEFAULT_M0;
	}
	index->ef_construction = kvs_vector_hnsw_param_or_min_default(
		g_cfg.vector_hnsw_ef_construction,
		KVS_HNSW_V2_DEFAULT_EF_CONSTRUCTION
	);
	index->ef_search = kvs_vector_hnsw_param_or_min_default(
		g_cfg.vector_hnsw_ef_search,
		KVS_HNSW_V2_DEFAULT_EF_SEARCH
	);
}

#ifdef ENABLE_FAISS_BASELINE
/*
 * Faiss backend integration hooks — weak defaults.
 *
 * When ENABLE_FAISS_BASELINE=1 and the Faiss backend is compiled in,
 * kvs_vector_faiss_ops.o overrides these with strong definitions pointing
 * to the real implementations.  When Faiss objects are not linked (e.g.
 * test binaries), these remain NULL and the Faiss code paths are skipped.
 */
__attribute__((weak))
const kvs_vector_index_ops_t *(*kvs_vector_faiss_hnsw_get_ops_ptr)(void);

__attribute__((weak))
void *(*kvs_vector_faiss_hnsw_create_ptr)(int kind, size_t dim, size_t M, size_t ef_construction);

__attribute__((weak))
void (*kvs_vector_faiss_hnsw_destroy_ptr)(void *handle);

__attribute__((weak))
size_t (*kvs_vector_faiss_ntotal_ptr)(const void *handle);
#endif

#ifdef ENABLE_HNSWLIB
/*
 * hnswlib backend integration hooks. Strong definitions are supplied by
 * kvs_vector_hnswlib_ops when the vendored backend is linked.
 */
__attribute__((weak))
const kvs_vector_index_ops_t *(*kvs_vector_hnswlib_get_ops_ptr)(void);

__attribute__((weak))
void *(*kvs_vector_hnswlib_create_ptr)(size_t dim, size_t max_elements, size_t M, size_t ef_construction);

__attribute__((weak))
void (*kvs_vector_hnswlib_destroy_ptr)(void *handle);

__attribute__((weak))
size_t (*kvs_vector_hnswlib_ntotal_ptr)(const void *handle);

__attribute__((weak))
size_t (*kvs_vector_hnswlib_max_elements_ptr)(const void *handle);

__attribute__((weak))
void (*kvs_vector_hnswlib_set_ef_ptr)(void *handle, size_t ef);
#endif

static int kvs_vector_flat_ops_add(
    kvs_vector_index_t *index,
    size_t record_index,
    const float *embedding,
    int embedding_dim
) {
    if (!index || !index->impl) {
        return KVS_VECTOR_ERR_INVALID_ARG;
    }

    return kvs_vector_flat_index_add(
        (kvs_vector_flat_index_t *)index->impl,
        record_index,
        embedding,
        embedding_dim
    );
}

static int kvs_vector_hnsw_ops_add(
    kvs_vector_index_t *index,
    size_t record_index,
    const float *embedding,
    int embedding_dim
) {
    if (!index || !index->impl) {
        return KVS_VECTOR_ERR_INVALID_ARG;
    }

    return kvs_vector_ann_index_add(
        (kvs_vector_ann_index_t *)index->impl,
        record_index,
        embedding,
        embedding_dim
    );
}

static int kvs_vector_flat_ops_search(
    kvs_vector_index_t *index,
    const float *query_embedding,
    int query_dim,
    const kvs_vector_index_search_params_t *params,
    kvs_vector_search_result_t *results,
    size_t results_capacity,
    size_t *results_count
) {
    if (!index || !index->impl || !params) {
        return KVS_VECTOR_ERR_INVALID_ARG;
    }

    return kvs_vector_flat_index_search(
        (kvs_vector_flat_index_t *)index->impl,
        query_embedding,
        query_dim,
        params->scope,
        params->scope_len,
        params->topk,
        params->threshold,
        index->metric,
        results,
        results_capacity,
        results_count
    );
}

static int kvs_vector_hnsw_ops_search(
    kvs_vector_index_t *index,
    const float *query_embedding,
    int query_dim,
    const kvs_vector_index_search_params_t *params,
    kvs_vector_search_result_t *results,
    size_t results_capacity,
    size_t *results_count
) {
    if (!index || !index->impl || !params) {
        return KVS_VECTOR_ERR_INVALID_ARG;
    }

    return kvs_vector_ann_index_search(
        (kvs_vector_ann_index_t *)index->impl,
        query_embedding,
        query_dim,
        params->scope,
        params->scope_len,
        params->topk,
        params->threshold,
        index->metric,
        results,
        results_capacity,
        results_count
    );
}

static int kvs_vector_flat_ops_rebuild(kvs_vector_index_t *index) {
    (void)index;
    return kvs_vector_rebuild_flat_index_from_store();
}

static int kvs_vector_hnsw_ops_rebuild(kvs_vector_index_t *index) {
    (void)index;
    return kvs_vector_rebuild_ann_index_from_store();
}

static int kvs_vector_index_ops_info(
    kvs_vector_index_t *index,
    kvs_vector_info_t *info
) {
    if (!index || !info) {
        return KVS_VECTOR_ERR_INVALID_ARG;
    }

    info->metric_type = (int)index->metric;
    return KVS_VECTOR_OK;
}

static void kvs_vector_flat_ops_destroy(kvs_vector_index_t *index) {
    if (!index || !index->impl) {
        return;
    }

    kvs_vector_flat_index_destroy((kvs_vector_flat_index_t *)index->impl);
}

static void kvs_vector_hnsw_ops_destroy(kvs_vector_index_t *index) {
    if (!index || !index->impl) {
        return;
    }

    kvs_vector_ann_index_destroy((kvs_vector_ann_index_t *)index->impl);
}

static const kvs_vector_index_ops_t g_kvs_vector_flat_ops = {
    .add = kvs_vector_flat_ops_add,
    .search = kvs_vector_flat_ops_search,
    .rebuild = kvs_vector_flat_ops_rebuild,
    .info = kvs_vector_index_ops_info,
    .destroy = kvs_vector_flat_ops_destroy,
};

static const kvs_vector_index_ops_t g_kvs_vector_hnsw_ops = {
    .add = kvs_vector_hnsw_ops_add,
    .search = kvs_vector_hnsw_ops_search,
    .rebuild = kvs_vector_hnsw_ops_rebuild,
    .info = kvs_vector_index_ops_info,
    .destroy = kvs_vector_hnsw_ops_destroy,
};

/********** core_flat adapter wrappers **********/

typedef struct kvs_vector_core_flat_rebuild_ctx_s {
	kvs_flat_index_t *index;
} kvs_vector_core_flat_rebuild_ctx_t;

static int kvs_vector_core_flat_rebuild_add_record(
	uint64_t record_id,
	const float *embedding,
	int embedding_dim,
	void *arg
) {
	kvs_vector_core_flat_rebuild_ctx_t *ctx =
		(kvs_vector_core_flat_rebuild_ctx_t *)arg;

	if (!ctx || !ctx->index || !embedding || embedding_dim <= 0) {
		return KVS_VECTOR_ERR_INVALID_ARG;
	}

	return kvs_flat_index_add(
		ctx->index,
		record_id,
		embedding,
		(size_t)embedding_dim
	);
}

static int kvs_vector_core_flat_ops_add(
	kvs_vector_index_t *index,
	size_t record_index,
	const float *embedding,
	int embedding_dim
) {
	uint64_t record_id = 0;
	int ret;

	if (!index || !index->impl || !embedding || embedding_dim <= 0) {
		return KVS_VECTOR_ERR_INVALID_ARG;
	}

	ret = kvs_vector_manager_get_active_record_id_by_index(
		record_index,
		&record_id
	);
	if (ret != KVS_VECTOR_OK) {
		return ret;
	}

	return kvs_flat_index_add(
		(kvs_flat_index_t *)index->impl,
		record_id,
		embedding,
		(size_t)embedding_dim
	);
}

static size_t kvs_vector_core_flat_raw_topk(
	const kvs_flat_index_t *flat,
	size_t requested_topk,
	size_t results_capacity
) {
	size_t raw_topk;
	size_t limit;

	if (requested_topk == 0) {
		requested_topk = 1;
	}

	raw_topk = requested_topk * 4;
	if (raw_topk < requested_topk + 32) {
		raw_topk = requested_topk + 32;
	}

	limit = flat ? flat->size : 0;
	if (limit == 0) {
		limit = results_capacity;
	}
	if (limit > 0 && raw_topk > limit) {
		raw_topk = limit;
	}
	if (raw_topk < requested_topk) {
		raw_topk = requested_topk;
	}

	return raw_topk;
}

static int kvs_vector_core_flat_ops_search(
	kvs_vector_index_t *index,
	const float *query_embedding,
	int query_dim,
	const kvs_vector_index_search_params_t *params,
	kvs_vector_search_result_t *results,
	size_t results_capacity,
	size_t *results_count
) {
	kvs_flat_index_t *flat;
	kvs_vector_hit_t *hits = NULL;
	size_t raw_topk;
	size_t hit_count = 0;
	size_t topk;
	float effective_threshold;
	int ret;

	if (results && results_capacity > 0) {
		kvs_vector_search_results_reset(results, results_capacity);
	}
	if (results_count) {
		*results_count = 0;
	}

	if (!index || !index->impl || !params || !query_embedding ||
	    query_dim <= 0 || !results || results_capacity == 0 || !results_count) {
		return KVS_VECTOR_ERR_INVALID_ARG;
	}

	flat = (kvs_flat_index_t *)index->impl;
	topk = params->topk > 0 ? (size_t)params->topk : 1;
	if (topk > results_capacity) {
		topk = results_capacity;
	}

	raw_topk = kvs_vector_core_flat_raw_topk(flat, topk, results_capacity);
	if (raw_topk == 0) {
		return KVS_VECTOR_OK;
	}

	hits = (kvs_vector_hit_t *)kvs_malloc(raw_topk * sizeof(kvs_vector_hit_t));
	if (!hits) {
		return KVS_VECTOR_ERR;
	}
	memset(hits, 0, raw_topk * sizeof(kvs_vector_hit_t));

	ret = kvs_flat_index_search(
		flat,
		query_embedding,
		(size_t)query_dim,
		raw_topk,
		hits,
		&hit_count
	);
	if (ret != KVS_VECTOR_OK) {
		kvs_free(hits);
		return ret;
	}

	effective_threshold = params->threshold;
	if (effective_threshold <= 0.0f) {
		effective_threshold = -1.0f;
	}

	ret = kvs_vector_manager_resolve_hits(
		hits,
		hit_count,
		params->scope,
		params->scope_len,
		effective_threshold,
		results,
		results_capacity,
		results_count
	);

	kvs_free(hits);
	return ret;
}

static int kvs_vector_core_flat_ops_rebuild(kvs_vector_index_t *index) {
	kvs_vector_core_flat_rebuild_ctx_t ctx;
	kvs_flat_index_t *flat;
	int ret;

	if (!index || !index->impl) {
		return KVS_VECTOR_ERR_INVALID_ARG;
	}

	flat = (kvs_flat_index_t *)index->impl;
	ret = kvs_flat_index_reset(flat);
	if (ret != KVS_VECTOR_OK) {
		return ret;
	}

	if (g_vector_state.store.active_records > 0) {
		ret = kvs_flat_index_reserve(flat, g_vector_state.store.active_records);
		if (ret != KVS_VECTOR_OK) {
			return ret;
		}
	}

	ctx.index = flat;
	return kvs_vector_manager_for_each_active_embedding(
		kvs_vector_core_flat_rebuild_add_record,
		&ctx
	);
}

static int kvs_vector_core_flat_ops_info(
	kvs_vector_index_t *index,
	kvs_vector_info_t *info
) {
	kvs_vector_core_info_t core_info;
	int ret;

	if (!index || !index->impl || !info) {
		return KVS_VECTOR_ERR_INVALID_ARG;
	}

	memset(&core_info, 0, sizeof(core_info));
	ret = kvs_flat_index_info((kvs_flat_index_t *)index->impl, &core_info);
	if (ret != KVS_VECTOR_OK) {
		return ret;
	}

	info->metric_type = core_info.metric_type;
	info->dimension = core_info.dim;
	info->index_ready = core_info.ready;
	info->needs_rebuild = core_info.needs_rebuild;
	info->core_flat_deleted_count = core_info.deleted_count;
	return KVS_VECTOR_OK;
}

static void kvs_vector_core_flat_ops_destroy(kvs_vector_index_t *index) {
	if (!index || !index->impl) {
		return;
	}

	kvs_flat_index_destroy((kvs_flat_index_t *)index->impl);
}

static const kvs_vector_index_ops_t g_kvs_vector_core_flat_ops = {
	.add = kvs_vector_core_flat_ops_add,
	.search = kvs_vector_core_flat_ops_search,
	.rebuild = kvs_vector_core_flat_ops_rebuild,
	.info = kvs_vector_core_flat_ops_info,
	.destroy = kvs_vector_core_flat_ops_destroy,
};

/********** legacy core_hnsw adapter wrappers **********/

typedef struct kvs_vector_core_hnsw_rebuild_ctx_s {
	kvs_hnsw_index_t *index;
} kvs_vector_core_hnsw_rebuild_ctx_t;

static int kvs_vector_core_hnsw_rebuild_add_record(
	uint64_t record_id,
	const float *embedding,
	int embedding_dim,
	void *arg
) {
	kvs_vector_core_hnsw_rebuild_ctx_t *ctx =
		(kvs_vector_core_hnsw_rebuild_ctx_t *)arg;
	uint64_t add_start_us;
	int ret;

	if (!ctx || !ctx->index || !embedding || embedding_dim <= 0) {
		return KVS_VECTOR_ERR_INVALID_ARG;
	}

	add_start_us = kvs_hnsw_profile_now_us();
	ret = kvs_hnsw_index_add(
		ctx->index,
		record_id,
		embedding,
		(size_t)embedding_dim
	);
	kvs_hnsw_rebuild_profile_record_add(
		ctx->index,
		kvs_hnsw_profile_now_us() - add_start_us
	);
	return ret;
}

static int kvs_vector_core_hnsw_ops_add(
	kvs_vector_index_t *index,
	size_t record_index,
	const float *embedding,
	int embedding_dim
) {
	kvs_hnsw_index_t *hnsw;
	uint64_t record_id = 0;
	size_t new_capacity;
	int ret;

	if (!index || !index->impl || !embedding || embedding_dim <= 0) {
		return KVS_VECTOR_ERR_INVALID_ARG;
	}

	hnsw = (kvs_hnsw_index_t *)index->impl;
	if (hnsw->size >= hnsw->capacity) {
		new_capacity = hnsw->capacity > 0 ? hnsw->capacity * 2 : KVS_VECTOR_INITIAL_RECORD_CAPACITY;
		if (new_capacity <= hnsw->size) {
			new_capacity = hnsw->size + 1;
		}
		ret = kvs_hnsw_index_reserve(hnsw, new_capacity);
		if (ret != KVS_VECTOR_OK) {
			return ret;
		}
	}

	ret = kvs_vector_manager_get_active_record_id_by_index(
		record_index,
		&record_id
	);
	if (ret != KVS_VECTOR_OK) {
		return ret;
	}

	return kvs_hnsw_index_add(
		hnsw,
		record_id,
		embedding,
		(size_t)embedding_dim
	);
}

static size_t kvs_vector_core_hnsw_raw_topk(
	const kvs_hnsw_index_t *hnsw,
	size_t requested_topk,
	size_t results_capacity
) {
	size_t raw_topk;
	size_t limit;

	if (requested_topk == 0) {
		requested_topk = 1;
	}

	raw_topk = requested_topk * 4;
	if (raw_topk < requested_topk + 32) {
		raw_topk = requested_topk + 32;
	}

	limit = hnsw ? hnsw->size : 0;
	if (limit == 0) {
		limit = results_capacity;
	}
	if (limit > 0 && raw_topk > limit) {
		raw_topk = limit;
	}
	if (raw_topk < requested_topk) {
		raw_topk = requested_topk;
	}
	if (hnsw && hnsw->search_context_pool.context_capacity > 0 &&
	    raw_topk > hnsw->search_context_pool.context_capacity) {
		raw_topk = hnsw->search_context_pool.context_capacity;
	}

	return raw_topk;
}

static int kvs_vector_core_hnsw_ops_search(
	kvs_vector_index_t *index,
	const float *query_embedding,
	int query_dim,
	const kvs_vector_index_search_params_t *params,
	kvs_vector_search_result_t *results,
	size_t results_capacity,
	size_t *results_count
) {
	kvs_hnsw_index_t *hnsw;
	kvs_vector_hit_t *hits = NULL;
	size_t raw_topk;
	size_t hit_count = 0;
	size_t topk;
	float effective_threshold;
	int ret;

	if (results && results_capacity > 0) {
		kvs_vector_search_results_reset(results, results_capacity);
	}
	if (results_count) {
		*results_count = 0;
	}

	if (!index || !index->impl || !params || !query_embedding ||
	    query_dim <= 0 || !results || results_capacity == 0 || !results_count) {
		return KVS_VECTOR_ERR_INVALID_ARG;
	}

	hnsw = (kvs_hnsw_index_t *)index->impl;
	topk = params->topk > 0 ? (size_t)params->topk : 1;
	if (topk > results_capacity) {
		topk = results_capacity;
	}

	raw_topk = kvs_vector_core_hnsw_raw_topk(hnsw, topk, results_capacity);
	if (raw_topk == 0) {
		return KVS_VECTOR_OK;
	}

	hits = (kvs_vector_hit_t *)kvs_malloc(raw_topk * sizeof(kvs_vector_hit_t));
	if (!hits) {
		return KVS_VECTOR_ERR;
	}
	memset(hits, 0, raw_topk * sizeof(kvs_vector_hit_t));

	ret = kvs_hnsw_index_search(
		hnsw,
		query_embedding,
		(size_t)query_dim,
		raw_topk,
		hits,
		&hit_count
	);
	if (ret != KVS_VECTOR_OK) {
		kvs_free(hits);
		return ret;
	}

	effective_threshold = params->threshold;
	if (effective_threshold <= 0.0f) {
		effective_threshold = -1.0f;
	}

	ret = kvs_vector_manager_resolve_hits(
		hits,
		hit_count,
		params->scope,
		params->scope_len,
		effective_threshold,
		results,
		results_capacity,
		results_count
	);

	kvs_free(hits);
	return ret;
}

static int kvs_vector_core_hnsw_ops_rebuild(kvs_vector_index_t *index) {
	kvs_vector_core_hnsw_rebuild_ctx_t ctx;
	kvs_hnsw_index_t *hnsw;
	uint64_t total_start_us;
	uint64_t scan_start_us;
	uint64_t scan_total_us;
	uint64_t reset_start_us;
	uint64_t reserve_start_us;
	int ret;

	if (!index || !index->impl) {
		return KVS_VECTOR_ERR_INVALID_ARG;
	}

	hnsw = (kvs_hnsw_index_t *)index->impl;
	kvs_hnsw_rebuild_profile_reset(hnsw);
	total_start_us = kvs_hnsw_profile_now_us();

	reset_start_us = kvs_hnsw_profile_now_us();
	ret = kvs_hnsw_index_reset(hnsw);
	hnsw->rebuild_profile.rebuild_core_reset_us =
		kvs_hnsw_profile_now_us() - reset_start_us;
	if (ret != KVS_VECTOR_OK) {
		hnsw->rebuild_profile.rebuild_total_us =
			kvs_hnsw_profile_now_us() - total_start_us;
		return ret;
	}

	if (g_vector_state.store.active_records > 0) {
		reserve_start_us = kvs_hnsw_profile_now_us();
		ret = kvs_hnsw_index_reserve(hnsw, g_vector_state.store.active_records);
		hnsw->rebuild_profile.rebuild_core_reserve_us =
			kvs_hnsw_profile_now_us() - reserve_start_us;
		if (ret != KVS_VECTOR_OK) {
			hnsw->rebuild_profile.rebuild_total_us =
				kvs_hnsw_profile_now_us() - total_start_us;
			return ret;
		}
	}

	ctx.index = hnsw;
	scan_start_us = kvs_hnsw_profile_now_us();
	ret = kvs_vector_manager_for_each_active_embedding(
		kvs_vector_core_hnsw_rebuild_add_record,
		&ctx
	);
	scan_total_us = kvs_hnsw_profile_now_us() - scan_start_us;
	hnsw->rebuild_profile.rebuild_manager_scan_us =
		scan_total_us > hnsw->rebuild_profile.rebuild_core_add_total_us ?
			scan_total_us - hnsw->rebuild_profile.rebuild_core_add_total_us : 0;
	hnsw->rebuild_profile.rebuild_total_us =
		kvs_hnsw_profile_now_us() - total_start_us;
	return ret;
}

static int kvs_vector_core_hnsw_ops_info(
	kvs_vector_index_t *index,
	kvs_vector_info_t *info
) {
	kvs_hnsw_index_t *hnsw;
	kvs_vector_core_info_t core_info;
	int ret;

	if (!index || !index->impl || !info) {
		return KVS_VECTOR_ERR_INVALID_ARG;
	}

	hnsw = (kvs_hnsw_index_t *)index->impl;
	memset(&core_info, 0, sizeof(core_info));
	ret = kvs_hnsw_index_info(hnsw, &core_info);
	if (ret != KVS_VECTOR_OK) {
		return ret;
	}

	info->metric_type = core_info.metric_type;
	info->dimension = core_info.dim;
	info->index_ready = core_info.ready;
	info->needs_rebuild = core_info.needs_rebuild;
	info->index_memory_bytes = core_info.memory_bytes;
	info->core_hnsw_size = core_info.size;
	info->core_hnsw_capacity = core_info.capacity;
	info->core_hnsw_m = hnsw->M;
	info->core_hnsw_m0 = hnsw->M0;
	info->core_hnsw_ef_construction = hnsw->ef_construction;
	info->core_hnsw_ef_search = hnsw->ef_search;
	info->core_hnsw_max_level = hnsw->has_entry ? (int)hnsw->max_level : 0;
	info->core_hnsw_deleted_count = core_info.deleted_count;
	info->rebuild_total_us = hnsw->rebuild_profile.rebuild_total_us;
	info->rebuild_manager_scan_us = hnsw->rebuild_profile.rebuild_manager_scan_us;
	info->rebuild_core_reset_us = hnsw->rebuild_profile.rebuild_core_reset_us;
	info->rebuild_core_reserve_us = hnsw->rebuild_profile.rebuild_core_reserve_us;
	info->rebuild_core_add_total_us = hnsw->rebuild_profile.rebuild_core_add_total_us;
	info->rebuild_core_add_avg_us =
		hnsw->rebuild_profile.rebuild_core_add_count ?
			hnsw->rebuild_profile.rebuild_core_add_total_us /
				hnsw->rebuild_profile.rebuild_core_add_count : 0;
	info->rebuild_core_add_max_us = hnsw->rebuild_profile.rebuild_core_add_max_us;
	info->rebuild_core_add_count = hnsw->rebuild_profile.rebuild_core_add_count;
	info->hnsw_add_random_level_us = hnsw->rebuild_profile.hnsw_add_random_level_us;
	info->hnsw_add_greedy_search_us = hnsw->rebuild_profile.hnsw_add_greedy_search_us;
	info->hnsw_add_layer_search_us = hnsw->rebuild_profile.hnsw_add_layer_search_us;
	info->hnsw_add_neighbor_select_us = hnsw->rebuild_profile.hnsw_add_neighbor_select_us;
	info->hnsw_add_link_us = hnsw->rebuild_profile.hnsw_add_link_us;
	info->hnsw_add_vector_copy_us = hnsw->rebuild_profile.hnsw_add_vector_copy_us;
	info->add_distance_compute_count = hnsw->rebuild_profile.add_distance_compute_count;
	info->add_neighbor_visit_count = hnsw->rebuild_profile.add_neighbor_visit_count;
	info->add_visited_skip_count = hnsw->rebuild_profile.add_visited_skip_count;
	info->add_active_check_count = hnsw->rebuild_profile.add_active_check_count;
	info->add_top_heap_push_count = hnsw->rebuild_profile.add_top_heap_push_count;
	info->add_top_heap_replace_count = hnsw->rebuild_profile.add_top_heap_replace_count;
	info->add_candidate_push_count = hnsw->rebuild_profile.add_candidate_push_count;
	info->add_level0_neighbor_scan_count = hnsw->rebuild_profile.add_level0_neighbor_scan_count;
	info->add_upper_neighbor_scan_count = hnsw->rebuild_profile.add_upper_neighbor_scan_count;
	info->layer_search_call_count = hnsw->rebuild_profile.layer_search_call_count;
	info->layer_search_level0_call_count = hnsw->rebuild_profile.layer_search_level0_call_count;
	info->layer_search_upper_call_count = hnsw->rebuild_profile.layer_search_upper_call_count;
	info->layer_search_level0_us = hnsw->rebuild_profile.layer_search_level0_us;
	info->layer_search_upper_us = hnsw->rebuild_profile.layer_search_upper_us;
	info->layer_search_distance_compute_count = hnsw->rebuild_profile.layer_search_distance_compute_count;
	info->layer_search_visited_skip_count = hnsw->rebuild_profile.layer_search_visited_skip_count;
	info->layer_search_neighbor_visit_count = hnsw->rebuild_profile.layer_search_neighbor_visit_count;
	info->layer_search_level0_neighbor_scan_count = hnsw->rebuild_profile.layer_search_level0_neighbor_scan_count;
	info->layer_search_upper_neighbor_scan_count = hnsw->rebuild_profile.layer_search_upper_neighbor_scan_count;
	info->layer_search_candidate_heap_push_count = hnsw->rebuild_profile.layer_search_candidate_heap_push_count;
	info->layer_search_candidate_heap_pop_count = hnsw->rebuild_profile.layer_search_candidate_heap_pop_count;
	info->layer_search_candidate_heap_replace_count = hnsw->rebuild_profile.layer_search_candidate_heap_replace_count;
	info->layer_search_top_heap_push_count = hnsw->rebuild_profile.layer_search_top_heap_push_count;
	info->layer_search_top_heap_replace_count = hnsw->rebuild_profile.layer_search_top_heap_replace_count;
	info->layer_search_lower_bound_update_count = hnsw->rebuild_profile.layer_search_lower_bound_update_count;
	info->layer_search_deleted_active_check_count = hnsw->rebuild_profile.layer_search_deleted_active_check_count;
	info->layer_search_unique_visit_count = hnsw->rebuild_profile.layer_search_unique_visit_count;
	info->layer_search_repeated_visit_count = hnsw->rebuild_profile.layer_search_repeated_visit_count;
	info->layer_search_visited_tag_access_count = hnsw->rebuild_profile.layer_search_visited_tag_access_count;
	info->layer_search_slot_state_touch_count = hnsw->rebuild_profile.layer_search_slot_state_touch_count;
	info->layer_search_level0_unique_visit_count = hnsw->rebuild_profile.layer_search_level0_unique_visit_count;
	info->layer_search_level0_repeated_visit_count = hnsw->rebuild_profile.layer_search_level0_repeated_visit_count;
	info->layer_search_level0_visited_tag_access_count = hnsw->rebuild_profile.layer_search_level0_visited_tag_access_count;
	info->layer_search_level0_slot_state_touch_count = hnsw->rebuild_profile.layer_search_level0_slot_state_touch_count;
	info->layer_search_level0_neighbor_row_access_count = hnsw->rebuild_profile.layer_search_level0_neighbor_row_access_count;
	info->layer_search_level0_neighbor_row_sequential_count = hnsw->rebuild_profile.layer_search_level0_neighbor_row_sequential_count;
	info->layer_search_level0_neighbor_row_random_count = hnsw->rebuild_profile.layer_search_level0_neighbor_row_random_count;
	info->layer_search_level0_neighbor_slot_sequential_count = hnsw->rebuild_profile.layer_search_level0_neighbor_slot_sequential_count;
	info->layer_search_level0_neighbor_slot_random_count = hnsw->rebuild_profile.layer_search_level0_neighbor_slot_random_count;
	info->layer_search_level0_candidate_row_expansion_count = hnsw->rebuild_profile.layer_search_level0_candidate_row_expansion_count;
	info->layer_search_level0_distance_compute_count = hnsw->rebuild_profile.layer_search_level0_distance_compute_count;
	info->layer_search_upper_distance_compute_count = hnsw->rebuild_profile.layer_search_upper_distance_compute_count;
	info->layer_search_level0_deleted_active_check_count = hnsw->rebuild_profile.layer_search_level0_deleted_active_check_count;
	info->layer_search_upper_deleted_active_check_count = hnsw->rebuild_profile.layer_search_upper_deleted_active_check_count;
	info->layer_search_candidate_heap_skip_count = hnsw->rebuild_profile.layer_search_candidate_heap_skip_count;
	info->layer_search_candidate_heap_full_skip_count = hnsw->rebuild_profile.layer_search_candidate_heap_full_skip_count;
	info->layer_search_top_heap_skip_count = hnsw->rebuild_profile.layer_search_top_heap_skip_count;
	info->layer_search_level0_candidate_heap_push_count = hnsw->rebuild_profile.layer_search_level0_candidate_heap_push_count;
	info->layer_search_level0_candidate_heap_pop_count = hnsw->rebuild_profile.layer_search_level0_candidate_heap_pop_count;
	info->layer_search_level0_candidate_heap_skip_count = hnsw->rebuild_profile.layer_search_level0_candidate_heap_skip_count;
	info->layer_search_level0_top_heap_push_count = hnsw->rebuild_profile.layer_search_level0_top_heap_push_count;
	info->layer_search_level0_top_heap_replace_count = hnsw->rebuild_profile.layer_search_level0_top_heap_replace_count;
	info->layer_search_level0_top_heap_skip_count = hnsw->rebuild_profile.layer_search_level0_top_heap_skip_count;
	info->layer_search_level0_lower_bound_update_count = hnsw->rebuild_profile.layer_search_level0_lower_bound_update_count;
	kvs_vector_hnsw_fill_layout_audit(hnsw, info);
	info->neighbor_select_call_count = hnsw->rebuild_profile.neighbor_select_call_count;
	info->neighbor_select_level0_call_count = hnsw->rebuild_profile.neighbor_select_level0_call_count;
	info->neighbor_select_upper_call_count = hnsw->rebuild_profile.neighbor_select_upper_call_count;
	info->neighbor_select_candidate_total = hnsw->rebuild_profile.neighbor_select_candidate_total;
	info->neighbor_select_candidate_max = hnsw->rebuild_profile.neighbor_select_candidate_max;
	info->neighbor_select_selected_total = hnsw->rebuild_profile.neighbor_select_selected_total;
	info->neighbor_select_selected_max = hnsw->rebuild_profile.neighbor_select_selected_max;
	info->neighbor_select_reject_count = hnsw->rebuild_profile.neighbor_select_reject_count;
	info->neighbor_select_heuristic_prune_count = hnsw->rebuild_profile.neighbor_select_heuristic_prune_count;
	info->neighbor_select_distance_recompute_count = hnsw->rebuild_profile.neighbor_select_distance_recompute_count;
	info->neighbor_select_reused_candidate_score_count = hnsw->rebuild_profile.neighbor_select_reused_candidate_score_count;
	info->neighbor_select_duplicate_scan_count = hnsw->rebuild_profile.neighbor_select_duplicate_scan_count;
	info->neighbor_select_worst_scan_count = hnsw->rebuild_profile.neighbor_select_worst_scan_count;
	info->neighbor_select_worst_cache_hit_count = hnsw->rebuild_profile.neighbor_select_worst_cache_hit_count;
	info->neighbor_select_level0_candidate_total = hnsw->rebuild_profile.neighbor_select_level0_candidate_total;
	info->neighbor_select_upper_candidate_total = hnsw->rebuild_profile.neighbor_select_upper_candidate_total;
	info->neighbor_select_level0_selected_total = hnsw->rebuild_profile.neighbor_select_level0_selected_total;
	info->neighbor_select_upper_selected_total = hnsw->rebuild_profile.neighbor_select_upper_selected_total;
	info->neighbor_select_level0_reject_count = hnsw->rebuild_profile.neighbor_select_level0_reject_count;
	info->neighbor_select_upper_reject_count = hnsw->rebuild_profile.neighbor_select_upper_reject_count;
	info->neighbor_select_level0_heuristic_prune_count = hnsw->rebuild_profile.neighbor_select_level0_heuristic_prune_count;
	info->neighbor_select_upper_heuristic_prune_count = hnsw->rebuild_profile.neighbor_select_upper_heuristic_prune_count;
	info->neighbor_select_level0_distance_recompute_count = hnsw->rebuild_profile.neighbor_select_level0_distance_recompute_count;
	info->neighbor_select_upper_distance_recompute_count = hnsw->rebuild_profile.neighbor_select_upper_distance_recompute_count;
	info->neighbor_select_level0_reused_candidate_score_count = hnsw->rebuild_profile.neighbor_select_level0_reused_candidate_score_count;
	info->neighbor_select_upper_reused_candidate_score_count = hnsw->rebuild_profile.neighbor_select_upper_reused_candidate_score_count;
	info->neighbor_select_level0_duplicate_scan_count = hnsw->rebuild_profile.neighbor_select_level0_duplicate_scan_count;
	info->neighbor_select_upper_duplicate_scan_count = hnsw->rebuild_profile.neighbor_select_upper_duplicate_scan_count;
	info->neighbor_select_level0_worst_scan_count = hnsw->rebuild_profile.neighbor_select_level0_worst_scan_count;
	info->neighbor_select_upper_worst_scan_count = hnsw->rebuild_profile.neighbor_select_upper_worst_scan_count;
	info->neighbor_select_level0_worst_cache_hit_count = hnsw->rebuild_profile.neighbor_select_level0_worst_cache_hit_count;
	info->neighbor_select_upper_worst_cache_hit_count = hnsw->rebuild_profile.neighbor_select_upper_worst_cache_hit_count;
	info->bidirectional_selected_neighbor_count = hnsw->rebuild_profile.bidirectional_selected_neighbor_count;
	info->bidirectional_forward_edge_attempt_count = hnsw->rebuild_profile.bidirectional_forward_edge_attempt_count;
	info->bidirectional_forward_edge_added_count = hnsw->rebuild_profile.bidirectional_forward_edge_added_count;
	info->bidirectional_reverse_edge_attempt_count = hnsw->rebuild_profile.bidirectional_reverse_edge_attempt_count;
	info->bidirectional_reverse_edge_accepted_count = hnsw->rebuild_profile.bidirectional_reverse_edge_accepted_count;
	info->bidirectional_reverse_edge_rejected_count = hnsw->rebuild_profile.bidirectional_reverse_edge_rejected_count;
	info->bidirectional_forward_edge_skipped_due_to_reverse_reject_count =
		hnsw->rebuild_profile.bidirectional_forward_edge_skipped_due_to_reverse_reject_count;
	return KVS_VECTOR_OK;
}

static void kvs_vector_core_hnsw_ops_destroy(kvs_vector_index_t *index) {
	if (!index || !index->impl) {
		return;
	}

	kvs_hnsw_index_destroy((kvs_hnsw_index_t *)index->impl);
}

static const kvs_vector_index_ops_t g_kvs_vector_core_hnsw_ops = {
	.add = kvs_vector_core_hnsw_ops_add,
	.search = kvs_vector_core_hnsw_ops_search,
	.rebuild = kvs_vector_core_hnsw_ops_rebuild,
	.info = kvs_vector_core_hnsw_ops_info,
	.destroy = kvs_vector_core_hnsw_ops_destroy,
};

/********** core_hnsw_v2 default ANN adapter wrappers **********/

typedef struct kvs_vector_core_hnsw_v2_rebuild_ctx_s {
	kvs_hnsw_v2_index_t *index;
} kvs_vector_core_hnsw_v2_rebuild_ctx_t;

static int kvs_vector_core_hnsw_v2_rebuild_add_record(
	uint64_t record_id,
	const float *embedding,
	int embedding_dim,
	void *arg
) {
	kvs_vector_core_hnsw_v2_rebuild_ctx_t *ctx =
		(kvs_vector_core_hnsw_v2_rebuild_ctx_t *)arg;

	if (!ctx || !ctx->index || !embedding || embedding_dim <= 0) {
		return KVS_VECTOR_ERR_INVALID_ARG;
	}

	return kvs_hnsw_v2_index_add(
		ctx->index,
		record_id,
		embedding,
		(size_t)embedding_dim
	);
}

static int kvs_vector_core_hnsw_v2_ensure_add_capacity(
	kvs_hnsw_v2_index_t *hnsw
) {
	size_t new_capacity;

	if (!hnsw) {
		return KVS_VECTOR_ERR_INVALID_ARG;
	}
	if (hnsw->size < hnsw->capacity) {
		return KVS_VECTOR_OK;
	}

	new_capacity = hnsw->capacity > 0 ?
		hnsw->capacity * 2 : KVS_VECTOR_INITIAL_RECORD_CAPACITY;
	if (new_capacity <= hnsw->size) {
		new_capacity = hnsw->size + 1;
	}
	return kvs_hnsw_v2_index_reserve(hnsw, new_capacity);
}

static int kvs_vector_core_hnsw_v2_ops_add(
	kvs_vector_index_t *index,
	size_t record_index,
	const float *embedding,
	int embedding_dim
) {
	kvs_hnsw_v2_index_t *hnsw;
	uint64_t record_id = 0;
	int ret;

	if (!index || !index->impl || !embedding || embedding_dim <= 0) {
		return KVS_VECTOR_ERR_INVALID_ARG;
	}

	hnsw = (kvs_hnsw_v2_index_t *)index->impl;
	ret = kvs_vector_core_hnsw_v2_ensure_add_capacity(hnsw);
	if (ret != KVS_VECTOR_OK) {
		return ret;
	}

	ret = kvs_vector_manager_get_active_record_id_by_index(
		record_index,
		&record_id
	);
	if (ret != KVS_VECTOR_OK) {
		return ret;
	}

	return kvs_hnsw_v2_index_add(
		hnsw,
		record_id,
		embedding,
		(size_t)embedding_dim
	);
}

static size_t kvs_vector_core_hnsw_v2_raw_topk(
	const kvs_hnsw_v2_index_t *hnsw,
	size_t requested_topk,
	size_t results_capacity
) {
	size_t raw_topk;
	size_t limit;

	if (requested_topk == 0) {
		requested_topk = 1;
	}

	raw_topk = requested_topk * 4;
	if (raw_topk < requested_topk + 32) {
		raw_topk = requested_topk + 32;
	}

	limit = hnsw ? hnsw->size : 0;
	if (limit == 0) {
		limit = results_capacity;
	}
	if (limit > 0 && raw_topk > limit) {
		raw_topk = limit;
	}
	if (raw_topk < requested_topk) {
		raw_topk = requested_topk;
	}

	return raw_topk;
}

static int kvs_vector_core_hnsw_v2_ops_search(
	kvs_vector_index_t *index,
	const float *query_embedding,
	int query_dim,
	const kvs_vector_index_search_params_t *params,
	kvs_vector_search_result_t *results,
	size_t results_capacity,
	size_t *results_count
) {
	kvs_hnsw_v2_index_t *hnsw;
	kvs_vector_hit_t *hits = NULL;
	size_t raw_topk;
	size_t hit_count = 0;
	size_t topk;
	size_t ef_search;
	float effective_threshold;
	int ret;

	if (results && results_capacity > 0) {
		kvs_vector_search_results_reset(results, results_capacity);
	}
	if (results_count) {
		*results_count = 0;
	}

	if (!index || !index->impl || !params || !query_embedding ||
	    query_dim <= 0 || !results || results_capacity == 0 || !results_count) {
		return KVS_VECTOR_ERR_INVALID_ARG;
	}

	hnsw = (kvs_hnsw_v2_index_t *)index->impl;
	topk = params->topk > 0 ? (size_t)params->topk : 1;
	if (topk > results_capacity) {
		topk = results_capacity;
	}

	raw_topk = kvs_vector_core_hnsw_v2_raw_topk(hnsw, topk, results_capacity);
	if (raw_topk == 0) {
		return KVS_VECTOR_OK;
	}

	hits = (kvs_vector_hit_t *)kvs_malloc(raw_topk * sizeof(kvs_vector_hit_t));
	if (!hits) {
		return KVS_VECTOR_ERR;
	}
	memset(hits, 0, raw_topk * sizeof(kvs_vector_hit_t));

	ef_search = hnsw->ef_search;
	if (ef_search < raw_topk) {
		ef_search = raw_topk;
	}

	ret = kvs_hnsw_v2_index_search(
		hnsw,
		query_embedding,
		(size_t)query_dim,
		raw_topk,
		ef_search,
		hits,
		&hit_count
	);
	if (ret != KVS_VECTOR_OK) {
		kvs_free(hits);
		return ret;
	}

	effective_threshold = params->threshold;
	if (effective_threshold <= 0.0f) {
		effective_threshold = -1.0f;
	}

	ret = kvs_vector_manager_resolve_hits(
		hits,
		hit_count,
		params->scope,
		params->scope_len,
		effective_threshold,
		results,
		results_capacity,
		results_count
	);

	kvs_free(hits);
	return ret;
}

static int kvs_vector_core_hnsw_v2_ops_rebuild(kvs_vector_index_t *index) {
	kvs_vector_core_hnsw_v2_rebuild_ctx_t ctx;
	kvs_hnsw_v2_index_t *hnsw;
	int ret;

	if (!index || !index->impl) {
		return KVS_VECTOR_ERR_INVALID_ARG;
	}

	hnsw = (kvs_hnsw_v2_index_t *)index->impl;
	ret = kvs_hnsw_v2_index_reset(hnsw);
	if (ret != KVS_VECTOR_OK) {
		return ret;
	}
	kvs_vector_configure_core_hnsw_v2(hnsw);

	if (g_vector_state.store.active_records > 0) {
		ret = kvs_hnsw_v2_index_reserve(
			hnsw,
			g_vector_state.store.active_records
		);
		if (ret != KVS_VECTOR_OK) {
			return ret;
		}
	}

	ctx.index = hnsw;
	ret = kvs_vector_manager_for_each_active_embedding(
		kvs_vector_core_hnsw_v2_rebuild_add_record,
		&ctx
	);
	if (ret != KVS_VECTOR_OK) {
		hnsw->needs_rebuild = 1;
		return ret;
	}

	kvs_hnsw_v2_graph_stats_refresh(hnsw);
	kvs_hnsw_v2_memory_refresh(hnsw);
	hnsw->needs_rebuild = 0;
	return KVS_VECTOR_OK;
}

static int kvs_vector_core_hnsw_v2_ops_info(
	kvs_vector_index_t *index,
	kvs_vector_info_t *info
) {
	kvs_hnsw_v2_index_t *hnsw;
	kvs_vector_core_info_t core_info;
	int ret;

	if (!index || !index->impl || !info) {
		return KVS_VECTOR_ERR_INVALID_ARG;
	}

	hnsw = (kvs_hnsw_v2_index_t *)index->impl;
	kvs_hnsw_v2_memory_refresh(hnsw);
	memset(&core_info, 0, sizeof(core_info));
	ret = kvs_hnsw_v2_index_info(hnsw, &core_info);
	if (ret != KVS_VECTOR_OK) {
		return ret;
	}

	info->metric_type = core_info.metric_type;
	info->dimension = core_info.dim;
	info->index_ready = core_info.ready;
	info->needs_rebuild = core_info.needs_rebuild;
	info->index_memory_bytes = core_info.memory_bytes;
	info->core_hnsw_v2_size = core_info.size;
	info->core_hnsw_v2_capacity = core_info.capacity;
	info->core_hnsw_v2_m = hnsw->M;
	info->core_hnsw_v2_m0 = hnsw->M0;
	info->core_hnsw_v2_ef_construction = hnsw->ef_construction;
	info->core_hnsw_v2_ef_search = hnsw->ef_search;
	info->core_hnsw_v2_max_level = hnsw->has_entry ? (int)hnsw->max_level : 0;
	info->core_hnsw_v2_deleted_count = core_info.deleted_count;
	info->core_hnsw_v2_active_count =
		core_info.size >= core_info.deleted_count ?
			core_info.size - core_info.deleted_count : 0;
	info->core_hnsw_v2_memory_bytes = core_info.memory_bytes;
	info->core_hnsw_v2_ready = core_info.ready;
	return KVS_VECTOR_OK;
}

static void kvs_vector_core_hnsw_v2_ops_destroy(kvs_vector_index_t *index) {
	if (!index || !index->impl) {
		return;
	}

	kvs_hnsw_v2_index_destroy((kvs_hnsw_v2_index_t *)index->impl);
}

static const kvs_vector_index_ops_t g_kvs_vector_core_hnsw_v2_ops = {
	.add = kvs_vector_core_hnsw_v2_ops_add,
	.search = kvs_vector_core_hnsw_v2_ops_search,
	.rebuild = kvs_vector_core_hnsw_v2_ops_rebuild,
	.info = kvs_vector_core_hnsw_v2_ops_info,
	.destroy = kvs_vector_core_hnsw_v2_ops_destroy,
};

/********** SQ8 Flat ops wrappers **********/

static int kvs_vector_sq8_flat_ops_add(
    kvs_vector_index_t *index,
    size_t record_index,
    const float *embedding,
    int embedding_dim
) {
    if (!index || !index->impl) return KVS_VECTOR_ERR_INVALID_ARG;

    return kvs_vector_sq8_flat_index_add(
        (kvs_vector_sq8_flat_index_t *)index->impl,
        record_index, embedding, embedding_dim);
}

static int kvs_vector_sq8_flat_ops_search(
    kvs_vector_index_t *index,
    const float *query_embedding,
    int query_dim,
    const kvs_vector_index_search_params_t *params,
    kvs_vector_search_result_t *results,
    size_t results_capacity,
    size_t *results_count
) {
    if (!index || !index->impl || !params) return KVS_VECTOR_ERR_INVALID_ARG;

    return kvs_vector_sq8_flat_index_search(
        (kvs_vector_sq8_flat_index_t *)index->impl,
        query_embedding, query_dim,
        params->scope, params->scope_len,
        params->topk, params->threshold,
        index->metric,
        results, results_capacity, results_count);
}

static int kvs_vector_sq8_flat_ops_rebuild(kvs_vector_index_t *index)
{
    (void)index;
    return kvs_vector_sq8_flat_index_rebuild(&g_vector_state.sq8_flat_index);
}

static int kvs_vector_sq8_flat_ops_info(
    kvs_vector_index_t *index,
    kvs_vector_info_t *info
) {
    if (!index || !info) return KVS_VECTOR_ERR_INVALID_ARG;

    info->metric_type = (int)index->metric;
    return kvs_vector_sq8_flat_index_info(&g_vector_state.sq8_flat_index, info);
}

static void kvs_vector_sq8_flat_ops_destroy(kvs_vector_index_t *index)
{
    if (!index || !index->impl) return;
    kvs_vector_sq8_flat_index_destroy((kvs_vector_sq8_flat_index_t *)index->impl);
}

static const kvs_vector_index_ops_t g_kvs_vector_sq8_flat_ops = {
    .add = kvs_vector_sq8_flat_ops_add,
    .search = kvs_vector_sq8_flat_ops_search,
    .rebuild = kvs_vector_sq8_flat_ops_rebuild,
    .info = kvs_vector_sq8_flat_ops_info,
    .destroy = kvs_vector_sq8_flat_ops_destroy,
};

#ifdef ENABLE_FAISS_BASELINE
/* g_kvs_vector_faiss_hnsw_ops is defined in kvs_vector_faiss_ops.c */
#endif

int kvs_vector_index_bind_current(void) {
    kvs_vector_index_t *index = &g_vector_state.current_index;

    memset(index, 0, sizeof(*index));

    index->type = g_vector_state.index_type;
    index->metric = g_vector_state.metric_type;

    if (g_vector_state.index_type == KVS_VECTOR_INDEX_TYPE_HNSW) {
        index->dimension = g_vector_state.ann_index.dimension;
        index->impl = &g_vector_state.ann_index;
        index->ops = &g_kvs_vector_hnsw_ops;
    } else if (g_vector_state.index_type == KVS_VECTOR_INDEX_CORE_FLAT) {
        index->dimension = g_vector_state.core_flat_index.dim;
        index->impl = &g_vector_state.core_flat_index;
        index->ops = &g_kvs_vector_core_flat_ops;
    } else if (g_vector_state.index_type == KVS_VECTOR_INDEX_CORE_HNSW) {
        index->dimension = g_vector_state.core_hnsw_index.dim;
        index->impl = &g_vector_state.core_hnsw_index;
        index->ops = &g_kvs_vector_core_hnsw_ops;
    } else if (g_vector_state.index_type == KVS_VECTOR_INDEX_CORE_HNSW_V2) {
        index->dimension = g_vector_state.core_hnsw_v2_index.dim;
        index->impl = &g_vector_state.core_hnsw_v2_index;
        index->ops = &g_kvs_vector_core_hnsw_v2_ops;
#ifdef ENABLE_HNSWLIB
    } else if (g_vector_state.index_type == KVS_VECTOR_INDEX_HNSWLIB_HNSW &&
               kvs_vector_hnswlib_get_ops_ptr) {
        index->dimension = kvs_vector_provider_dimension_or_default();
        index->impl = g_vector_state.hnswlib_backend_handle;
        index->ops = kvs_vector_hnswlib_get_ops_ptr();
#endif
    } else if (g_vector_state.index_type == KVS_VECTOR_INDEX_TYPE_SQ8_FLAT) {
        index->dimension = g_vector_state.sq8_flat_index.dimension;
        index->impl = &g_vector_state.sq8_flat_index;
        index->ops = &g_kvs_vector_sq8_flat_ops;
#ifdef ENABLE_FAISS_BASELINE
    } else if ((g_vector_state.index_type == KVS_VECTOR_INDEX_FAISS_HNSW ||
                g_vector_state.index_type == KVS_VECTOR_INDEX_FAISS_FLAT)
               && kvs_vector_faiss_hnsw_get_ops_ptr) {
        index->dimension = kvs_vector_provider_dimension_or_default();
        index->impl = g_vector_state.faiss_backend_handle;
        index->ops = kvs_vector_faiss_hnsw_get_ops_ptr();
#endif
    } else {
        index->dimension = g_vector_state.flat_index.dimension;
        index->impl = &g_vector_state.flat_index;
        index->ops = &g_kvs_vector_flat_ops;
    }

    return KVS_VECTOR_OK;
}

int kvs_vector_index_add_current(
    size_t record_index,
    const float *embedding,
    int embedding_dim
) {
    kvs_vector_index_t *index = &g_vector_state.current_index;

    if (!index->ops || !index->ops->add) {
        return KVS_VECTOR_ERR_NOT_INITIALIZED;
    }

    return index->ops->add(index, record_index, embedding, embedding_dim);
}

int kvs_vector_index_search_current(
    const float *query_embedding,
    int query_dim,
    const char *scope,
    size_t scope_len,
    int topk,
    float threshold,
    kvs_vector_search_result_t *results,
    size_t results_capacity,
    size_t *results_count
) {
	kvs_vector_index_t *index = &g_vector_state.current_index;
	kvs_vector_index_search_params_t params;
	uint64_t start_us;
	uint64_t end_us;
	uint64_t elapsed_us;
	uint64_t resolve_us;
	int ret;

    if (!index->ops || !index->ops->search) {
        return KVS_VECTOR_ERR_NOT_INITIALIZED;
    }

    memset(&params, 0, sizeof(params));
    params.scope = scope;
    params.scope_len = scope_len;
	params.topk = topk;
	params.threshold = threshold;

	g_vsearch_current_resolve_us = 0;
	start_us = kvs_vector_now_us();
	ret = index->ops->search(
        index,
        query_embedding,
        query_dim,
        &params,
        results,
        results_capacity,
        results_count
    );
	end_us = kvs_vector_now_us();
	elapsed_us = end_us >= start_us ? end_us - start_us : 0;
	resolve_us = g_vsearch_current_resolve_us;
	kvs_vector_vsearch_record_backend_search(
		elapsed_us >= resolve_us ? elapsed_us - resolve_us : 0
	);
	g_vsearch_current_resolve_us = 0;
	return ret;
}

int kvs_vector_index_rebuild_current(void) {
    kvs_vector_index_t *index = &g_vector_state.current_index;

    if (!index->ops || !index->ops->rebuild) {
        return KVS_VECTOR_ERR_NOT_INITIALIZED;
    }

    return index->ops->rebuild(index);
}

/*
 * 从内部状态生成 VINFO。
 *
 * 稳定语义：
 *
 * total_records:
 *   当前 vector store 中的逻辑记录数。
 *   等于 active_records + deleted_records。
 *   它不是历史累计写入数。
 *
 * active_records:
 *   当前可被 VGET / VSEARCH 命中的记录数。
 *
 * deleted_records:
 *   当前 tombstone 记录数。
 *
 * index_size:
 *   当前逻辑可检索向量数量。
 *   等于 active_records。
 *
 * 注意：
 * flat_index.size 是索引物理条目数，VDEL 后可能包含 stale entry。
 * 对外 VINFO 不暴露这个物理值，避免用户看到 index_size 和 active_records 不一致。
 */
static void kvs_vector_info_from_state(
	kvs_vector_info_t *info,
	const kvs_vector_state_t *state
) {
	size_t active_records;
	size_t deleted_records;

	if (!info) {
		return;
	}

	memset(info, 0, sizeof(*info));

	if (!state || !state->initialized) {
		info->total_records = 0;
		info->active_records = 0;
		info->deleted_records = 0;
		info->capacity = 0;
		info->index_size = 0;
		info->dimension = 0;
		info->index_ready = 0;
		info->needs_rebuild = 0;
		return;
	}

	active_records = state->store.active_records;
	deleted_records = state->store.deleted_records;

	info->active_records = active_records;
	info->deleted_records = deleted_records;
	info->total_records = active_records + deleted_records;
	info->capacity = state->store.capacity;

	/*
	 * 对外 index_size 采用逻辑语义：
	 * 当前可检索向量数量 = active_records。
	 *
	 * 这样 VDEL 后即使 flat_index 内部还有 stale entry，
	 * VINFO 也不会暴露容易误解的物理数量。
	 */
	info->index_size = active_records;
	info->dimension = state->flat_index.dimension;

	info->index_type = (int)state->index_type;
	info->metric_type = (int)state->metric_type;

	if (state->index_type == KVS_VECTOR_INDEX_TYPE_HNSW ||
	    state->index_type == KVS_VECTOR_INDEX_HNSWLIB_HNSW) {
		info->index_ready = state->ann_index.ready;
		info->needs_rebuild =
			state->flat_index.needs_rebuild ||
			state->ann_index.needs_rebuild;

		info->ann_size = state->ann_index.size;
		info->ann_m = state->ann_index.m;
		info->ann_ef_construction = state->ann_index.ef_construction;
		info->ann_ef_search = state->ann_index.ef_search;
		info->ann_max_level = state->ann_index.max_level;
		info->ann_ready = state->ann_index.ready;

		info->ann_size = state->ann_index.size;
		info->ann_m = state->ann_index.m;
		info->ann_ef_construction = state->ann_index.ef_construction;
		info->ann_ef_search = state->ann_index.ef_search;
		info->ann_max_level = state->ann_index.max_level;
		info->ann_ready = state->ann_index.ready;

	} else if (state->index_type == KVS_VECTOR_INDEX_TYPE_SQ8_FLAT) {
		info->index_ready = state->sq8_flat_index.ready;
		info->needs_rebuild = state->sq8_flat_index.needs_rebuild;

		info->ann_size = state->ann_index.size;
		info->ann_m = state->ann_index.m;
		info->ann_ef_construction = state->ann_index.ef_construction;
		info->ann_ef_search = state->ann_index.ef_search;
		info->ann_max_level = state->ann_index.max_level;
		info->ann_ready = state->ann_index.ready;

		kvs_vector_sq8_flat_index_info(&state->sq8_flat_index, info);
	} else if (state->index_type == KVS_VECTOR_INDEX_CORE_FLAT) {
		kvs_vector_core_info_t core_info;

		memset(&core_info, 0, sizeof(core_info));
		if (kvs_flat_index_info((kvs_flat_index_t *)&state->core_flat_index, &core_info) == KVS_VECTOR_OK) {
			info->index_ready = core_info.ready;
			info->needs_rebuild = core_info.needs_rebuild;
			info->dimension = core_info.dim;
			info->metric_type = core_info.metric_type;
			info->index_memory_bytes = core_info.memory_bytes;
			info->core_flat_deleted_count = core_info.deleted_count;
		}
	} else if (state->index_type == KVS_VECTOR_INDEX_CORE_HNSW) {
		kvs_vector_core_info_t core_info;
		const kvs_hnsw_index_t *hnsw = &state->core_hnsw_index;

		memset(&core_info, 0, sizeof(core_info));
		if (kvs_hnsw_index_info((kvs_hnsw_index_t *)hnsw, &core_info) == KVS_VECTOR_OK) {
			info->index_ready = core_info.ready;
			info->needs_rebuild = core_info.needs_rebuild;
			info->dimension = core_info.dim;
			info->metric_type = core_info.metric_type;
			info->index_memory_bytes = core_info.memory_bytes;
			info->core_hnsw_size = core_info.size;
			info->core_hnsw_capacity = core_info.capacity;
			info->core_hnsw_m = hnsw->M;
			info->core_hnsw_m0 = hnsw->M0;
			info->core_hnsw_ef_construction = hnsw->ef_construction;
			info->core_hnsw_ef_search = hnsw->ef_search;
			info->core_hnsw_max_level = hnsw->has_entry ? (int)hnsw->max_level : 0;
			info->core_hnsw_deleted_count = core_info.deleted_count;
		}
	} else if (state->index_type == KVS_VECTOR_INDEX_CORE_HNSW_V2) {
		kvs_vector_core_info_t core_info;
		kvs_hnsw_v2_index_t *hnsw =
			(kvs_hnsw_v2_index_t *)&state->core_hnsw_v2_index;

		kvs_hnsw_v2_memory_refresh(hnsw);
		memset(&core_info, 0, sizeof(core_info));
		if (kvs_hnsw_v2_index_info(hnsw, &core_info) == KVS_VECTOR_OK) {
			info->index_ready = core_info.ready;
			info->needs_rebuild = core_info.needs_rebuild;
			info->dimension = core_info.dim;
			info->metric_type = core_info.metric_type;
			info->index_memory_bytes = core_info.memory_bytes;
			info->core_hnsw_v2_size = core_info.size;
			info->core_hnsw_v2_capacity = core_info.capacity;
			info->core_hnsw_v2_m = hnsw->M;
			info->core_hnsw_v2_m0 = hnsw->M0;
			info->core_hnsw_v2_ef_construction = hnsw->ef_construction;
			info->core_hnsw_v2_ef_search = hnsw->ef_search;
			info->core_hnsw_v2_max_level =
				hnsw->has_entry ? (int)hnsw->max_level : 0;
			info->core_hnsw_v2_deleted_count = core_info.deleted_count;
			info->core_hnsw_v2_active_count =
				core_info.size >= core_info.deleted_count ?
					core_info.size - core_info.deleted_count : 0;
			info->core_hnsw_v2_memory_bytes = core_info.memory_bytes;
			info->core_hnsw_v2_ready = core_info.ready;
		}
	} else {
		info->index_ready = state->flat_index.ready;
		info->needs_rebuild = state->flat_index.needs_rebuild;

		info->ann_size = state->ann_index.size;
		info->ann_m = state->ann_index.m;
		info->ann_ef_construction = state->ann_index.ef_construction;
		info->ann_ef_search = state->ann_index.ef_search;
		info->ann_max_level = state->ann_index.max_level;
		info->ann_ready = state->ann_index.ready;
	}

	info->flat_physical_size = state->flat_index.size;

	/* Stage 8: runtime stats */
	info->core_hnsw_v2_experimental_enabled =
		g_cfg.vector_hnsw_v2_experimental_enabled ? 1 : 0;
	info->last_rebuild_status = state->last_rebuild_status;
	info->last_rebuild_ms = state->last_rebuild_ms;
	info->rebuild_state = kvs_vector_rebuild_state_name(state->rebuild.state);
	info->rebuild_delta_count = state->rebuild.delta_count;
	info->rebuild_error_code = state->rebuild.error_code;
	info->rebuild_error_message = state->rebuild.error_message;
	info->rebuild_shadow_present = state->rebuild.shadow_index ? 1 : 0;
	info->rebuild_retired_present = state->rebuild.retired_index ? 1 : 0;
	info->vector_memory_bytes = kvs_vector_estimate_record_memory_bytes();
	info->index_memory_bytes = kvs_vector_estimate_current_index_memory_bytes();
	info->total_vector_memory_bytes = info->vector_memory_bytes + info->index_memory_bytes;

	info->vsearch_total_count =
		kvs_vector_stats_load(&g_vset_commit_stats.vsearch_search_count);
	info->vsearch_total_us =
		kvs_vector_stats_load(&g_vset_commit_stats.vsearch_search_total_us);
	info->vsearch_total_max_us =
		kvs_vector_stats_load(&g_vset_commit_stats.vsearch_search_max_us);
	info->vsearch_backend_search_count =
		kvs_vector_stats_load(&g_vset_commit_stats.vsearch_backend_search_count);
	info->vsearch_backend_search_total_us =
		kvs_vector_stats_load(&g_vset_commit_stats.vsearch_backend_search_total_us);
	info->vsearch_backend_search_max_us =
		kvs_vector_stats_load(&g_vset_commit_stats.vsearch_backend_search_max_us);
	info->vsearch_manager_resolve_count =
		kvs_vector_stats_load(&g_vset_commit_stats.vsearch_manager_resolve_count);
	info->vsearch_manager_resolve_total_us =
		kvs_vector_stats_load(&g_vset_commit_stats.vsearch_manager_resolve_total_us);
	info->vsearch_manager_resolve_max_us =
		kvs_vector_stats_load(&g_vset_commit_stats.vsearch_manager_resolve_max_us);
	info->vsearch_response_assembly_count =
		kvs_vector_stats_load(&g_vset_commit_stats.vsearch_response_build_count);
	info->vsearch_response_assembly_total_us =
		kvs_vector_stats_load(&g_vset_commit_stats.vsearch_response_build_total_us);
	info->vsearch_response_assembly_max_us =
		kvs_vector_stats_load(&g_vset_commit_stats.vsearch_response_build_max_us);
	info->vsearch_raw_hit_count =
		kvs_vector_stats_load(&g_vset_commit_stats.vsearch_raw_hit_count);
	info->vsearch_resolved_hit_count =
		kvs_vector_stats_load(&g_vset_commit_stats.vsearch_resolved_hit_count);
	info->vsearch_filtered_hit_count =
		kvs_vector_stats_load(&g_vset_commit_stats.vsearch_filtered_hit_count);

#ifdef ENABLE_FAISS_BASELINE
	info->faiss_backend_enabled = 1;
	info->faiss_add_count = g_faiss_add_count;
	info->faiss_search_count = g_faiss_search_count;
	info->faiss_ntotal = (kvs_vector_faiss_ntotal_ptr && state->faiss_backend_handle)
		? kvs_vector_faiss_ntotal_ptr(state->faiss_backend_handle) : 0;
#else
	info->faiss_backend_enabled = 0;
	info->faiss_ntotal = 0;
	info->faiss_add_count = 0;
	info->faiss_search_count = 0;
#endif

#ifdef ENABLE_HNSWLIB
	info->hnswlib_backend_enabled = 1;
	info->hnswlib_ntotal = (kvs_vector_hnswlib_ntotal_ptr && state->hnswlib_backend_handle)
		? kvs_vector_hnswlib_ntotal_ptr(state->hnswlib_backend_handle) : 0;
	info->hnswlib_capacity = (kvs_vector_hnswlib_max_elements_ptr && state->hnswlib_backend_handle)
		? kvs_vector_hnswlib_max_elements_ptr(state->hnswlib_backend_handle) : 0;
	info->hnswlib_m = state->ann_index.m;
	info->hnswlib_ef_construction = state->ann_index.ef_construction;
	info->hnswlib_ef_search = state->ann_index.ef_search;
	info->hnswlib_add_count = g_hnswlib_add_count;
	info->hnswlib_search_count = g_hnswlib_search_count;
#else
	info->hnswlib_backend_enabled = 0;
	info->hnswlib_ntotal = 0;
	info->hnswlib_capacity = 0;
	info->hnswlib_m = 0;
	info->hnswlib_ef_construction = 0;
	info->hnswlib_ef_search = 0;
	info->hnswlib_add_count = 0;
	info->hnswlib_search_count = 0;
#endif
}

/********** Stage 8: memory estimation **********/

size_t kvs_vector_estimate_record_memory_bytes(void) {
	kvs_vector_store_t *store = &g_vector_state.store;
	size_t total = 0;
	size_t i;

	if (!store || !store->records || store->capacity == 0) return 0;

	for (i = 0; i < store->capacity; ++i) {
		kvs_vector_record_t *rec = &store->records[i];
		if (rec->state == KVS_VECTOR_RECORD_EMPTY) continue;

		/* question + answer + normalized_question + scope strings */
		if (rec->question && rec->question_len > 0)
			total += rec->question_len + 1;
		if (rec->answer && rec->answer_len > 0)
			total += rec->answer_len + 1;
		if (rec->normalized_question && rec->normalized_question_len > 0)
			total += rec->normalized_question_len + 1;
		if (rec->scope && rec->scope_len > 0)
			total += rec->scope_len + 1;
		/* embedding (float array) */
		if (rec->embedding && rec->embedding_dim > 0)
			total += (size_t)rec->embedding_dim * sizeof(float);
		/* struct overhead */
		total += sizeof(kvs_vector_record_t);
	}

	/* store metadata */
	total += store->capacity * sizeof(kvs_vector_record_t);
	if (store->free_capacity > 0)
		total += store->free_capacity * sizeof(size_t);

	/* id_map */
	if (g_vector_state.id_map.capacity > 0)
		total += g_vector_state.id_map.capacity * sizeof(kvs_vector_id_entry_t);

	return total;
}

size_t kvs_vector_estimate_flat_memory_bytes(void) {
	const kvs_vector_flat_index_t *idx = &g_vector_state.flat_index;
	size_t total = 0;

	if (idx->vectors && idx->capacity > 0)
		total += idx->capacity * idx->dimension * sizeof(float);
	if (idx->record_indices && idx->capacity > 0)
		total += idx->capacity * sizeof(size_t);
	if (idx->record_ids && idx->capacity > 0)
		total += idx->capacity * sizeof(uint64_t);

	return total;
}

size_t kvs_vector_estimate_hnsw_memory_bytes(void) {
	const kvs_vector_ann_index_t *idx = &g_vector_state.ann_index;
	size_t total = 0;
	size_t i;

	total += idx->capacity * sizeof(kvs_vector_ann_node_t);

	for (i = 0; i < idx->size; ++i) {
		const kvs_vector_ann_node_t *node = &idx->nodes[i];
		int level;
		if (!node->active) continue;

		if (node->neighbors) {
			for (level = 0; level <= node->level; ++level) {
				if (node->neighbors[level] && node->neighbor_capacities[level] > 0)
					total += node->neighbor_capacities[level] * sizeof(size_t);
			}
		}
		if (node->neighbor_counts)
			total += sizeof(size_t) * (size_t)(node->level + 1);
	}

	return total;
}

size_t kvs_vector_estimate_sq8_flat_memory_bytes(void) {
	const kvs_vector_sq8_flat_index_t *idx = &g_vector_state.sq8_flat_index;
	size_t total = 0;

	if (idx->codes && idx->capacity > 0)
		total += idx->capacity * idx->dimension;
	if (idx->scales && idx->capacity > 0)
		total += idx->capacity * sizeof(float);
	if (idx->record_indices && idx->capacity > 0)
		total += idx->capacity * sizeof(size_t);
	if (idx->record_ids && idx->capacity > 0)
		total += idx->capacity * sizeof(uint64_t);

	return total;
}

static size_t kvs_vector_estimate_core_flat_memory_bytes(void) {
	kvs_vector_core_info_t info;

	memset(&info, 0, sizeof(info));
	if (kvs_flat_index_info(&g_vector_state.core_flat_index, &info) != KVS_VECTOR_OK) {
		return 0;
	}

	return info.memory_bytes;
}

static size_t kvs_vector_estimate_core_hnsw_memory_bytes(void) {
	kvs_vector_core_info_t info;

	memset(&info, 0, sizeof(info));
	if (kvs_hnsw_index_info(&g_vector_state.core_hnsw_index, &info) != KVS_VECTOR_OK) {
		return 0;
	}

	return info.memory_bytes;
}

static size_t kvs_vector_estimate_core_hnsw_v2_memory_bytes(void) {
	kvs_vector_core_info_t info;

	kvs_hnsw_v2_memory_refresh(&g_vector_state.core_hnsw_v2_index);
	memset(&info, 0, sizeof(info));
	if (kvs_hnsw_v2_index_info(&g_vector_state.core_hnsw_v2_index, &info) !=
	    KVS_VECTOR_OK) {
		return 0;
	}

	return info.memory_bytes;
}

size_t kvs_vector_estimate_current_index_memory_bytes(void) {
	switch (g_vector_state.index_type) {
		case KVS_VECTOR_INDEX_TYPE_FLAT:
			return kvs_vector_estimate_flat_memory_bytes();
		case KVS_VECTOR_INDEX_CORE_FLAT:
			return kvs_vector_estimate_flat_memory_bytes() +
			       kvs_vector_estimate_core_flat_memory_bytes();
		case KVS_VECTOR_INDEX_CORE_HNSW:
			return kvs_vector_estimate_flat_memory_bytes() +
			       kvs_vector_estimate_core_hnsw_memory_bytes();
		case KVS_VECTOR_INDEX_CORE_HNSW_V2:
			return kvs_vector_estimate_flat_memory_bytes() +
			       kvs_vector_estimate_core_hnsw_v2_memory_bytes();
		case KVS_VECTOR_INDEX_TYPE_HNSW:
			return kvs_vector_estimate_flat_memory_bytes() +
			       kvs_vector_estimate_hnsw_memory_bytes();
		case KVS_VECTOR_INDEX_HNSWLIB_HNSW:
			return kvs_vector_estimate_flat_memory_bytes();
		case KVS_VECTOR_INDEX_TYPE_SQ8_FLAT:
			return kvs_vector_estimate_sq8_flat_memory_bytes();
		default:
			return 0;
	}
}

static size_t kvs_vector_estimate_faiss_backend_memory_bytes(
	const kvs_vector_info_t *info
) {
	size_t ntotal;
	size_t dim;
	size_t total;

	if (!info) {
		return 0;
	}

	ntotal = info->faiss_ntotal;
	dim = info->dimension ? info->dimension : g_vector_state.flat_index.dimension;
	if (ntotal == 0 || dim == 0) {
		return 0;
	}

	total = ntotal * dim * sizeof(float);
	total += ntotal * sizeof(uint64_t);

	if (g_vector_state.index_type == KVS_VECTOR_INDEX_FAISS_HNSW) {
		size_t m = g_vector_state.ann_index.m ? g_vector_state.ann_index.m : KVS_VECTOR_HNSW_M;
		total += ntotal * m * 2 * sizeof(uint32_t);
	}

	return total;
}

static size_t kvs_vector_estimate_hnswlib_backend_memory_bytes(
	const kvs_vector_info_t *info
) {
	size_t capacity;
	size_t dim;
	size_t m;

	if (!info) {
		return 0;
	}

	capacity = info->hnswlib_capacity ? info->hnswlib_capacity : info->hnswlib_ntotal;
	dim = info->dimension ? info->dimension : g_vector_state.flat_index.dimension;
	m = info->hnswlib_m ? info->hnswlib_m : KVS_VECTOR_HNSW_M;
	if (capacity == 0 || dim == 0) {
		return 0;
	}

	return capacity * dim * sizeof(float) +
	       capacity * m * 2 * sizeof(uint32_t) +
	       capacity * (sizeof(uint64_t) + sizeof(uint32_t) + sizeof(uint8_t));
}

static void kvs_vector_fill_backend_stats(kvs_vector_info_t *info) {
	if (!info) {
		return;
	}

	info->backend_name = kvs_vector_index_type_name((kvs_vector_index_type_t)info->index_type);
	info->backend_size = info->index_size;
	info->backend_capacity = info->capacity;
	info->backend_dim = info->dimension;
	info->backend_metric = kvs_vector_metric_name((kvs_vector_metric_type_t)info->metric_type);
	info->backend_memory_bytes = info->index_memory_bytes;
	info->backend_memory_bytes_source = "estimated";
	info->backend_deleted_count = info->deleted_records;
	info->backend_needs_rebuild = info->needs_rebuild;
	info->backend_resize_count = "n/a";
	info->backend_reserve_count = "n/a";
	info->backend_resize_count_source = "unsupported";
	info->backend_reserve_count_source = "unsupported";

	switch ((kvs_vector_index_type_t)info->index_type) {
	case KVS_VECTOR_INDEX_CORE_FLAT:
		info->backend_size = g_vector_state.core_flat_index.size;
		info->backend_capacity = g_vector_state.core_flat_index.capacity;
		info->backend_deleted_count = info->core_flat_deleted_count;
		info->backend_memory_bytes = kvs_vector_estimate_core_flat_memory_bytes();
		info->backend_memory_bytes_source = "measured";
		break;
	case KVS_VECTOR_INDEX_CORE_HNSW:
		info->backend_size = info->core_hnsw_size;
		info->backend_capacity = info->core_hnsw_capacity;
		info->backend_deleted_count = info->core_hnsw_deleted_count;
		info->backend_memory_bytes = kvs_vector_estimate_core_hnsw_memory_bytes();
		info->backend_memory_bytes_source = "measured";
		break;
	case KVS_VECTOR_INDEX_CORE_HNSW_V2:
		info->backend_size = info->core_hnsw_v2_active_count;
		info->backend_capacity = info->core_hnsw_v2_capacity;
		info->backend_deleted_count = info->core_hnsw_v2_deleted_count;
		info->backend_memory_bytes = kvs_vector_estimate_core_hnsw_v2_memory_bytes();
		info->backend_memory_bytes_source = "measured";
		break;
	case KVS_VECTOR_INDEX_HNSWLIB_HNSW:
		info->backend_size = info->hnswlib_ntotal;
		info->backend_capacity = info->hnswlib_capacity;
		info->backend_deleted_count = 0;
		info->backend_memory_bytes = kvs_vector_estimate_hnswlib_backend_memory_bytes(info);
		info->backend_memory_bytes_source =
			info->backend_memory_bytes > 0 ? "estimated" : "unsupported";
		break;
	case KVS_VECTOR_INDEX_FAISS_FLAT:
	case KVS_VECTOR_INDEX_FAISS_HNSW:
		info->backend_size = info->faiss_ntotal;
		info->backend_capacity = info->faiss_ntotal;
		info->backend_deleted_count = 0;
		info->backend_memory_bytes = kvs_vector_estimate_faiss_backend_memory_bytes(info);
		info->backend_memory_bytes_source =
			info->backend_memory_bytes > 0 ? "estimated" : "unsupported";
		break;
	case KVS_VECTOR_INDEX_TYPE_SQ8_FLAT:
		info->backend_size = info->sq8_size;
		info->backend_capacity = info->capacity;
		info->backend_dim = info->sq8_dimension ? info->sq8_dimension : info->dimension;
		info->backend_memory_bytes = info->sq8_memory_bytes;
		info->backend_memory_bytes_source = "estimated";
		break;
	case KVS_VECTOR_INDEX_TYPE_HNSW:
		info->backend_size = info->ann_size;
		info->backend_capacity = g_vector_state.ann_index.capacity;
		info->backend_deleted_count = info->deleted_records;
		info->backend_memory_bytes = kvs_vector_estimate_hnsw_memory_bytes();
		info->backend_memory_bytes_source = "estimated";
		break;
	case KVS_VECTOR_INDEX_TYPE_FLAT:
	default:
		info->backend_size = g_vector_state.flat_index.size;
		info->backend_capacity = g_vector_state.flat_index.capacity;
		info->backend_deleted_count = info->deleted_records;
		info->backend_memory_bytes = kvs_vector_estimate_flat_memory_bytes();
		info->backend_memory_bytes_source = "estimated";
		break;
	}
}

//初始化全局状态
int kvs_vector_init(void) {
	int ret;
	size_t dimension;

	if (g_vector_state.initialized) {
		KVS_LOG_DEBUG(KVS_VECTOR_LOG_MODULE, "vector module already initialized");
		return KVS_VECTOR_OK;
	}

	KVS_LOG_INFO(KVS_VECTOR_LOG_MODULE, "initializing vector module");

	memset(&g_vector_state, 0, sizeof(g_vector_state));

	ret = kvs_vector_store_init(&g_vector_state.store);
	if (ret != KVS_VECTOR_OK) {
		goto fail;
	}

	ret = kvs_vector_store_reserve(
		&g_vector_state.store,
		KVS_VECTOR_INITIAL_RECORD_CAPACITY
	);
	if (ret != KVS_VECTOR_OK) {
		goto fail;
	}

	ret = kvs_vector_id_map_init(&g_vector_state.id_map);
	if (ret != KVS_VECTOR_OK) {
		goto fail;
	}

	ret = kvs_vector_id_map_reserve(
		&g_vector_state.id_map,
		KVS_VECTOR_INITIAL_ID_MAP_CAPACITY
	);
	if (ret != KVS_VECTOR_OK) {
		goto fail;
	}

	ret = kvs_vector_rebuild_context_init(&g_vector_state.rebuild);
	if (ret != KVS_VECTOR_OK) {
		goto fail;
	}

	ret = kvs_vector_embedding_provider_init(&g_vector_state.embedding_provider);
	if (ret != KVS_VECTOR_OK) {
		goto fail;
	}

	ret = kvs_vector_distance_init();
	if (ret != KVS_VECTOR_OK) {
		goto fail;
	}

	dimension = kvs_vector_provider_dimension_or_default();

	/*
	 * core_flat remains available as an exact baseline / fallback. The
	 * semantic-cache ANN default is core_hnsw_v2 after Stage 9G2.
	 */
	ret = kvs_vector_flat_index_init(
		&g_vector_state.flat_index,
		dimension
	);
	if (ret != KVS_VECTOR_OK) {
		goto fail;
	}

	g_vector_state.index_type = kvs_vector_index_type_from_config();
	g_vector_state.metric_type = kvs_vector_metric_from_config();

	ret = kvs_flat_index_init(
		&g_vector_state.core_flat_index,
		dimension,
		g_vector_state.metric_type
	);
	if (ret != KVS_VECTOR_OK) {
		goto fail;
	}

	ret = kvs_hnsw_index_init(
		&g_vector_state.core_hnsw_index,
		dimension,
		g_vector_state.metric_type
	);
	if (ret != KVS_VECTOR_OK) {
		goto fail;
	}
	g_vector_state.core_hnsw_index.M =
		kvs_vector_hnsw_param_or_default(g_cfg.vector_hnsw_m, KVS_VECTOR_HNSW_M);
	g_vector_state.core_hnsw_index.M0 = g_vector_state.core_hnsw_index.M * 2;
	g_vector_state.core_hnsw_index.ef_construction =
		kvs_vector_hnsw_param_or_default(
			g_cfg.vector_hnsw_ef_construction,
			KVS_VECTOR_HNSW_EF_CONSTRUCTION
		);
	g_vector_state.core_hnsw_index.ef_search =
		kvs_vector_hnsw_param_or_default(
			g_cfg.vector_hnsw_ef_search,
			KVS_VECTOR_HNSW_EF_SEARCH
		);
	ret = kvs_hnsw_index_reserve(
		&g_vector_state.core_hnsw_index,
		g_vector_state.store.capacity
	);
	if (ret != KVS_VECTOR_OK) {
		goto fail;
	}

	ret = kvs_hnsw_v2_index_init(
		&g_vector_state.core_hnsw_v2_index,
		dimension,
		g_vector_state.metric_type
	);
	if (ret != KVS_VECTOR_OK) {
		goto fail;
	}
	kvs_vector_configure_core_hnsw_v2(&g_vector_state.core_hnsw_v2_index);
	ret = kvs_hnsw_v2_index_reserve(
		&g_vector_state.core_hnsw_v2_index,
		g_vector_state.store.capacity
	);
	if (ret != KVS_VECTOR_OK) {
		goto fail;
	}

	ret = kvs_vector_ann_index_init(
		&g_vector_state.ann_index,
		dimension,
		kvs_vector_hnsw_param_or_default(
			g_cfg.vector_hnsw_m,
			KVS_VECTOR_HNSW_M
		),
		kvs_vector_hnsw_param_or_default(
			g_cfg.vector_hnsw_ef_construction,
			KVS_VECTOR_HNSW_EF_CONSTRUCTION
		),
		kvs_vector_hnsw_param_or_default(
			g_cfg.vector_hnsw_ef_search,
			KVS_VECTOR_HNSW_EF_SEARCH
		)
	);
	if (ret != KVS_VECTOR_OK) {
		goto fail;
	}

	if (ret != KVS_VECTOR_OK) {
		goto fail;
	}

	/*
	 * SQ8 flat is kept as an experimental compressed index. A failure here
	 * should not prevent the HNSW semantic-cache path from starting.
	 */
	ret = kvs_vector_sq8_flat_index_init(
		&g_vector_state.sq8_flat_index,
		dimension
	);
	if (ret != KVS_VECTOR_OK &&
	    g_vector_state.index_type == KVS_VECTOR_INDEX_TYPE_SQ8_FLAT) {
		goto fail;
	} else if (ret != KVS_VECTOR_OK) {
		KVS_LOG_WARN(
			KVS_VECTOR_LOG_MODULE,
			"experimental sq8_flat init failed ret=%d, continuing with HNSW path",
			ret
		);
		memset(&g_vector_state.sq8_flat_index, 0, sizeof(g_vector_state.sq8_flat_index));
	}

#ifdef ENABLE_FAISS_BASELINE
	if ((g_vector_state.index_type == KVS_VECTOR_INDEX_FAISS_HNSW ||
	     g_vector_state.index_type == KVS_VECTOR_INDEX_FAISS_FLAT)
	    && kvs_vector_faiss_hnsw_create_ptr) {
		int faiss_kind = (g_vector_state.index_type == KVS_VECTOR_INDEX_FAISS_HNSW)
			? KVS_FAISS_INDEX_HNSW : KVS_FAISS_INDEX_FLAT_IP;
		g_vector_state.faiss_backend_handle = kvs_vector_faiss_hnsw_create_ptr(
			faiss_kind,
			dimension,
			(size_t)g_cfg.vector_hnsw_m,
			(size_t)g_cfg.vector_hnsw_ef_construction
		);
		if (!g_vector_state.faiss_backend_handle) {
			KVS_LOG_ERROR(KVS_VECTOR_LOG_MODULE,
				"faiss backend init failed type=%d", g_vector_state.index_type);
			ret = KVS_VECTOR_ERR;
			goto fail;
		}
		KVS_LOG_INFO(KVS_VECTOR_LOG_MODULE,
			"faiss backend initialized type=%d dim=%zu",
			g_vector_state.index_type, dimension);
	}
#endif

#ifdef ENABLE_HNSWLIB
	if (g_vector_state.index_type == KVS_VECTOR_INDEX_HNSWLIB_HNSW &&
	    kvs_vector_hnswlib_create_ptr) {
		size_t max_elements = g_vector_state.store.capacity;
		size_t configured_capacity = g_cfg.vector_hnsw_initial_capacity > 0
			? (size_t)g_cfg.vector_hnsw_initial_capacity : 0;
		if (max_elements < KVS_VECTOR_INITIAL_RECORD_CAPACITY) {
			max_elements = KVS_VECTOR_INITIAL_RECORD_CAPACITY;
		}
		if (configured_capacity > max_elements) {
			max_elements = configured_capacity;
		}
		g_vector_state.hnswlib_backend_handle = kvs_vector_hnswlib_create_ptr(
			dimension,
			max_elements,
			kvs_vector_hnsw_param_or_default(g_cfg.vector_hnsw_m, KVS_VECTOR_HNSW_M),
			kvs_vector_hnsw_param_or_default(g_cfg.vector_hnsw_ef_construction, KVS_VECTOR_HNSW_EF_CONSTRUCTION)
		);
		if (!g_vector_state.hnswlib_backend_handle) {
			KVS_LOG_ERROR(KVS_VECTOR_LOG_MODULE, "hnswlib backend init failed");
			ret = KVS_VECTOR_ERR;
			goto fail;
		}
		if (kvs_vector_hnswlib_set_ef_ptr) {
			kvs_vector_hnswlib_set_ef_ptr(
				g_vector_state.hnswlib_backend_handle,
				g_vector_state.ann_index.ef_search
			);
		}
	}
#endif

	ret = kvs_vector_index_bind_current();
	if (ret != KVS_VECTOR_OK) {
		goto fail;
	}

	if (ret != KVS_VECTOR_OK) {
		goto fail;
	}

	g_vector_state.initialized = 1;

	KVS_LOG_INFO(
		KVS_VECTOR_LOG_MODULE,
		"vector module initialized provider=%s distance_backend=%s dimension=%zu capacity=%zu index_type=%s metric=%s hnsw_m=%zu ef_construction=%zu ef_search=%zu hnsw_initial_capacity=%d",
		kvs_vector_embedding_provider_name(),
		kvs_vector_distance_backend_name(),
		g_vector_state.flat_index.dimension,
		g_vector_state.store.capacity,
		kvs_vector_index_type_name(g_vector_state.index_type),
		kvs_vector_metric_name(g_vector_state.metric_type),
		g_vector_state.ann_index.m,
		g_vector_state.ann_index.ef_construction,
		g_vector_state.ann_index.ef_search,
		g_cfg.vector_hnsw_initial_capacity
	);

	return KVS_VECTOR_OK;

fail:
	KVS_LOG_ERROR(
		KVS_VECTOR_LOG_MODULE,
		"vector module init failed ret=%d",
		ret
	);
#ifdef ENABLE_HNSWLIB
	if (g_vector_state.hnswlib_backend_handle && kvs_vector_hnswlib_destroy_ptr) {
		kvs_vector_hnswlib_destroy_ptr(g_vector_state.hnswlib_backend_handle);
		g_vector_state.hnswlib_backend_handle = NULL;
	}
#endif
#ifdef ENABLE_FAISS_BASELINE
	if (g_vector_state.faiss_backend_handle && kvs_vector_faiss_hnsw_destroy_ptr) {
		kvs_vector_faiss_hnsw_destroy_ptr(g_vector_state.faiss_backend_handle);
		g_vector_state.faiss_backend_handle = NULL;
	}
#endif
	kvs_vector_embedding_provider_destroy(&g_vector_state.embedding_provider);
	kvs_vector_sq8_flat_index_destroy(&g_vector_state.sq8_flat_index);
	kvs_hnsw_v2_index_destroy(&g_vector_state.core_hnsw_v2_index);
	kvs_hnsw_index_destroy(&g_vector_state.core_hnsw_index);
	kvs_flat_index_destroy(&g_vector_state.core_flat_index);
	kvs_vector_ann_index_destroy(&g_vector_state.ann_index);
	kvs_vector_flat_index_destroy(&g_vector_state.flat_index);
#ifdef ENABLE_HNSWLIB
	if (g_vector_state.hnswlib_backend_handle && kvs_vector_hnswlib_destroy_ptr) {
		kvs_vector_hnswlib_destroy_ptr(g_vector_state.hnswlib_backend_handle);
		g_vector_state.hnswlib_backend_handle = NULL;
	}
#endif
	kvs_vector_id_map_destroy(&g_vector_state.id_map);
	kvs_vector_rebuild_context_destroy(&g_vector_state.rebuild);
	kvs_vector_store_destroy(&g_vector_state.store);
	memset(&g_vector_state, 0, sizeof(g_vector_state));

	return ret;
}

//销毁全局状态
void kvs_vector_destroy(void) {
	if (!g_vector_state.initialized) {
		return;
	}
	kvs_vector_embedding_provider_destroy(&g_vector_state.embedding_provider);
	kvs_vector_sq8_flat_index_destroy(&g_vector_state.sq8_flat_index);
	kvs_hnsw_v2_index_destroy(&g_vector_state.core_hnsw_v2_index);
	kvs_hnsw_index_destroy(&g_vector_state.core_hnsw_index);
	kvs_flat_index_destroy(&g_vector_state.core_flat_index);
	kvs_vector_ann_index_destroy(&g_vector_state.ann_index);
	kvs_vector_flat_index_destroy(&g_vector_state.flat_index);
#ifdef ENABLE_FAISS_BASELINE
	if (g_vector_state.faiss_backend_handle && kvs_vector_faiss_hnsw_destroy_ptr) {
		kvs_vector_faiss_hnsw_destroy_ptr(g_vector_state.faiss_backend_handle);
		g_vector_state.faiss_backend_handle = NULL;
	}
#endif
#ifdef ENABLE_HNSWLIB
	if (g_vector_state.hnswlib_backend_handle && kvs_vector_hnswlib_destroy_ptr) {
		kvs_vector_hnswlib_destroy_ptr(g_vector_state.hnswlib_backend_handle);
		g_vector_state.hnswlib_backend_handle = NULL;
	}
#endif
	kvs_vector_id_map_destroy(&g_vector_state.id_map);
	kvs_vector_rebuild_context_destroy(&g_vector_state.rebuild);
	kvs_vector_store_destroy(&g_vector_state.store);

	memset(&g_vector_state, 0, sizeof(g_vector_state));
}

int kvs_vector_reset_for_load(void) {
	int ret;
	size_t dimension = kvs_vector_provider_dimension_or_default();

	if (!g_vector_state.initialized) {
		return KVS_VECTOR_ERR_NOT_INITIALIZED;
	}

	kvs_vector_sq8_flat_index_destroy(&g_vector_state.sq8_flat_index);
	kvs_hnsw_v2_index_destroy(&g_vector_state.core_hnsw_v2_index);
	kvs_hnsw_index_destroy(&g_vector_state.core_hnsw_index);
	kvs_flat_index_destroy(&g_vector_state.core_flat_index);
	kvs_vector_ann_index_destroy(&g_vector_state.ann_index);
	kvs_vector_flat_index_destroy(&g_vector_state.flat_index);
	kvs_vector_id_map_destroy(&g_vector_state.id_map);
	kvs_vector_store_destroy(&g_vector_state.store);

	ret = kvs_vector_store_init(&g_vector_state.store);
	if (ret != KVS_VECTOR_OK) return ret;

	ret = kvs_vector_store_reserve(&g_vector_state.store, KVS_VECTOR_INITIAL_RECORD_CAPACITY);
	if (ret != KVS_VECTOR_OK) return ret;

	ret = kvs_vector_id_map_init(&g_vector_state.id_map);
	if (ret != KVS_VECTOR_OK) return ret;

	ret = kvs_vector_id_map_reserve(&g_vector_state.id_map, KVS_VECTOR_INITIAL_ID_MAP_CAPACITY);
	if (ret != KVS_VECTOR_OK) return ret;

	kvs_vector_rebuild_context_destroy(&g_vector_state.rebuild);
	ret = kvs_vector_rebuild_context_init(&g_vector_state.rebuild);
	if (ret != KVS_VECTOR_OK) return ret;

	ret = kvs_vector_flat_index_init(&g_vector_state.flat_index, dimension);
	if (ret != KVS_VECTOR_OK) return ret;

	ret = kvs_flat_index_init(
		&g_vector_state.core_flat_index,
		dimension,
		g_vector_state.metric_type
	);
	if (ret != KVS_VECTOR_OK) return ret;

	ret = kvs_hnsw_index_init(
		&g_vector_state.core_hnsw_index,
		dimension,
		g_vector_state.metric_type
	);
	if (ret != KVS_VECTOR_OK) return ret;
	g_vector_state.core_hnsw_index.M =
		kvs_vector_hnsw_param_or_default(g_cfg.vector_hnsw_m, KVS_VECTOR_HNSW_M);
	g_vector_state.core_hnsw_index.M0 = g_vector_state.core_hnsw_index.M * 2;
	g_vector_state.core_hnsw_index.ef_construction =
		kvs_vector_hnsw_param_or_default(
			g_cfg.vector_hnsw_ef_construction,
			KVS_VECTOR_HNSW_EF_CONSTRUCTION
		);
	g_vector_state.core_hnsw_index.ef_search =
		kvs_vector_hnsw_param_or_default(
			g_cfg.vector_hnsw_ef_search,
			KVS_VECTOR_HNSW_EF_SEARCH
		);
	ret = kvs_hnsw_index_reserve(
		&g_vector_state.core_hnsw_index,
		g_vector_state.store.capacity
	);
	if (ret != KVS_VECTOR_OK) return ret;

	ret = kvs_hnsw_v2_index_init(
		&g_vector_state.core_hnsw_v2_index,
		dimension,
		g_vector_state.metric_type
	);
	if (ret != KVS_VECTOR_OK) return ret;
	kvs_vector_configure_core_hnsw_v2(&g_vector_state.core_hnsw_v2_index);
	ret = kvs_hnsw_v2_index_reserve(
		&g_vector_state.core_hnsw_v2_index,
		g_vector_state.store.capacity
	);
	if (ret != KVS_VECTOR_OK) return ret;

	ret = kvs_vector_ann_index_init(
		&g_vector_state.ann_index,
		dimension,
		kvs_vector_hnsw_param_or_default(g_cfg.vector_hnsw_m, KVS_VECTOR_HNSW_M),
		kvs_vector_hnsw_param_or_default(g_cfg.vector_hnsw_ef_construction, KVS_VECTOR_HNSW_EF_CONSTRUCTION),
		kvs_vector_hnsw_param_or_default(g_cfg.vector_hnsw_ef_search, KVS_VECTOR_HNSW_EF_SEARCH)
	);
	if (ret != KVS_VECTOR_OK) return ret;

	if (ret != KVS_VECTOR_OK) return ret;

	ret = kvs_vector_sq8_flat_index_init(
		&g_vector_state.sq8_flat_index,
		dimension
	);
	if (ret != KVS_VECTOR_OK &&
	    g_vector_state.index_type == KVS_VECTOR_INDEX_TYPE_SQ8_FLAT) {
		return ret;
	} else if (ret != KVS_VECTOR_OK) {
		memset(&g_vector_state.sq8_flat_index, 0, sizeof(g_vector_state.sq8_flat_index));
	}

#ifdef ENABLE_HNSWLIB
	if (g_vector_state.index_type == KVS_VECTOR_INDEX_HNSWLIB_HNSW &&
	    kvs_vector_hnswlib_create_ptr) {
		size_t max_elements = g_vector_state.store.capacity;
		size_t configured_capacity = g_cfg.vector_hnsw_initial_capacity > 0
			? (size_t)g_cfg.vector_hnsw_initial_capacity : 0;
		if (configured_capacity > max_elements) {
			max_elements = configured_capacity;
		}
		g_vector_state.hnswlib_backend_handle = kvs_vector_hnswlib_create_ptr(
			dimension,
			max_elements,
			kvs_vector_hnsw_param_or_default(g_cfg.vector_hnsw_m, KVS_VECTOR_HNSW_M),
			kvs_vector_hnsw_param_or_default(g_cfg.vector_hnsw_ef_construction, KVS_VECTOR_HNSW_EF_CONSTRUCTION)
		);
		if (!g_vector_state.hnswlib_backend_handle) {
			return KVS_VECTOR_ERR;
		}
		if (kvs_vector_hnswlib_set_ef_ptr) {
			kvs_vector_hnswlib_set_ef_ptr(
				g_vector_state.hnswlib_backend_handle,
				g_vector_state.ann_index.ef_search
			);
		}
	}
#endif

	return kvs_vector_index_bind_current();
}

int kvs_vector_restore_record_with_embedding(
	size_t record_index,
	uint64_t record_id,
	int state,
	int deleted,
	const char *question,
	size_t question_len,
	const char *answer,
	size_t answer_len,
	const char *scope,
	size_t scope_len,
	const char *normalized_question,
	size_t normalized_question_len,
	const float *embedding,
	int embedding_dim,
	int64_t created_at_ms,
	int64_t updated_at_ms,
	int64_t expire_at_ms,
	uint64_t generation,
	uint32_t embedding_model_version
) {
	kvs_vector_store_t *store = &g_vector_state.store;
	kvs_vector_record_t *record;
	char *question_copy = NULL;
	char *answer_copy = NULL;
	char *scope_copy = NULL;
	char *normalized_copy = NULL;
	float *embedding_copy = NULL;
	size_t copied_len = 0;
	size_t embedding_bytes;
	int ret;

	if (!question || question_len == 0 || !answer) {
		return KVS_VECTOR_ERR_INVALID_ARG;
	}

	if (record_index + 1 > store->capacity) {
		ret = kvs_vector_store_ensure_capacity(store, record_index + 1);
		if (ret != KVS_VECTOR_OK) return ret;
	}

	record = &store->records[record_index];
	kvs_vector_record_destroy(record);

	question_copy = kvs_vector_dup_string(question, question_len, &copied_len);
	if (!question_copy) return KVS_VECTOR_ERR;

	answer_copy = kvs_vector_dup_string(answer, answer_len, &copied_len);
	if (!answer_copy) {
		kvs_free(question_copy);
		return KVS_VECTOR_ERR;
	}

	if (normalized_question && normalized_question_len > 0) {
		normalized_copy = kvs_vector_dup_string(normalized_question, normalized_question_len, &copied_len);
	} else {
		normalized_copy = kvs_vector_dup_string(question, question_len, &copied_len);
	}
	if (!normalized_copy) {
		kvs_free(question_copy);
		kvs_free(answer_copy);
		return KVS_VECTOR_ERR;
	}

	if (scope && scope_len > 0) {
		scope_copy = kvs_vector_dup_string(scope, scope_len, &copied_len);
		if (!scope_copy) {
			kvs_free(question_copy);
			kvs_free(answer_copy);
			kvs_free(normalized_copy);
			return KVS_VECTOR_ERR;
		}
	}

	if (embedding && embedding_dim > 0) {
		embedding_bytes = (size_t)embedding_dim * sizeof(float);
		embedding_copy = (float *)kvs_malloc(embedding_bytes);
		if (!embedding_copy) {
			kvs_free(question_copy);
			kvs_free(answer_copy);
			kvs_free(normalized_copy);
			if (scope_copy) kvs_free(scope_copy);
			return KVS_VECTOR_ERR;
		}
		memcpy(embedding_copy, embedding, embedding_bytes);
	}

	record->question = question_copy;
	record->question_len = question_len;
	record->answer = answer_copy;
	record->answer_len = answer_len;
	record->normalized_question = normalized_copy;
	record->normalized_question_len = normalized_question_len > 0 ? normalized_question_len : question_len;
	record->scope = scope_copy;
	record->scope_len = (scope && scope_len > 0) ? scope_len : 0;
	record->embedding = embedding_copy;
	record->embedding_dim = embedding_dim;
	record->question_hash = kvs_vector_hash_key(question, question_len);

	record->id = record_id;
	record->state = state;
	record->deleted = deleted;
	record->created_at_ms = created_at_ms;
	record->updated_at_ms = updated_at_ms;

	record->expire_at_ms = expire_at_ms;
	record->generation = generation;
	record->embedding_model_version = embedding_model_version;

	if (store->next_slot <= record_index) {
		store->next_slot = record_index + 1;
	}

	if (store->next_id <= record_id) {
		store->next_id = record_id + 1;
	}

	if (state == KVS_VECTOR_RECORD_ACTIVE && !deleted) {
		store->active_records++;
		ret = kvs_vector_id_map_put(
			&g_vector_state.id_map,
			record->question,
			record->question_len,
			record->scope,
			record->scope_len,
			record_index
		);
		if (ret != KVS_VECTOR_OK) {
			return ret;
		}
		ret = kvs_vector_record_id_map_put(
			&store->record_id_map,
			record->id,
			record_index
		);
		if (ret != KVS_VECTOR_OK) {
			kvs_vector_id_map_remove(
				&g_vector_state.id_map,
				record->question,
				record->question_len,
				record->scope,
				record->scope_len
			);
			return ret;
		}
	} else if (state == KVS_VECTOR_RECORD_DELETED || deleted) {
		store->deleted_records++;
		kvs_vector_store_free_list_push(store, record_index);
	}

	store->total_records = store->active_records + store->deleted_records;

	return KVS_VECTOR_OK;
}

int kvs_vector_rebuild_after_load(void) {
	int ret;

	if (!g_vector_state.initialized) {
		return KVS_VECTOR_ERR_NOT_INITIALIZED;
	}

	ret = kvs_vector_id_map_rebuild_from_store();
	if (ret != KVS_VECTOR_OK) return ret;

	ret = kvs_vector_rebuild_flat_index_from_store();
	if (ret != KVS_VECTOR_OK) return ret;

	if (g_vector_state.index_type == KVS_VECTOR_INDEX_TYPE_HNSW) {
		ret = kvs_vector_rebuild_ann_index_from_store();
		if (ret != KVS_VECTOR_OK) return ret;
	}


	if (g_vector_state.index_type == KVS_VECTOR_INDEX_TYPE_SQ8_FLAT) {
		ret = kvs_vector_sq8_flat_index_rebuild(&g_vector_state.sq8_flat_index);
		if (ret != KVS_VECTOR_OK) return ret;
	}

	ret = kvs_vector_index_bind_current();
	if (ret != KVS_VECTOR_OK) return ret;

	if (g_vector_state.index_type == KVS_VECTOR_INDEX_CORE_FLAT &&
	    g_vector_state.current_index.ops &&
	    g_vector_state.current_index.ops->rebuild) {
		ret = g_vector_state.current_index.ops->rebuild(&g_vector_state.current_index);
		if (ret != KVS_VECTOR_OK) return ret;
	}

	if (g_vector_state.index_type == KVS_VECTOR_INDEX_CORE_HNSW &&
	    g_vector_state.current_index.ops &&
	    g_vector_state.current_index.ops->rebuild) {
		ret = g_vector_state.current_index.ops->rebuild(&g_vector_state.current_index);
		if (ret != KVS_VECTOR_OK) return ret;
	}

	if (g_vector_state.index_type == KVS_VECTOR_INDEX_CORE_HNSW_V2 &&
	    g_vector_state.current_index.ops &&
	    g_vector_state.current_index.ops->rebuild) {
		ret = g_vector_state.current_index.ops->rebuild(&g_vector_state.current_index);
		if (ret != KVS_VECTOR_OK) return ret;
	}

	if (g_vector_state.index_type == KVS_VECTOR_INDEX_HNSWLIB_HNSW &&
	    g_vector_state.current_index.ops &&
	    g_vector_state.current_index.ops->rebuild) {
		ret = g_vector_state.current_index.ops->rebuild(&g_vector_state.current_index);
		if (ret != KVS_VECTOR_OK) return ret;
	}

	return KVS_VECTOR_OK;
}

void kvs_vector_set_prepared_reset(kvs_vector_set_prepared_t *prepared) {
	if (!prepared) {
		return;
	}
	memset(prepared, 0, sizeof(*prepared));
}

void kvs_vector_set_prepared_destroy(kvs_vector_set_prepared_t *prepared) {
	if (!prepared) {
		return;
	}
	if (prepared->question) {
		kvs_free(prepared->question);
	}
	if (prepared->answer) {
		kvs_free(prepared->answer);
	}
	if (prepared->scope) {
		kvs_free(prepared->scope);
	}
	if (prepared->embedding) {
		kvs_free(prepared->embedding);
	}
	kvs_vector_set_prepared_reset(prepared);
}

void kvs_vector_search_prepared_reset(kvs_vector_search_prepared_t *prepared) {
	if (!prepared) {
		return;
	}
	memset(prepared, 0, sizeof(*prepared));
}

void kvs_vector_search_prepared_destroy(kvs_vector_search_prepared_t *prepared) {
	if (!prepared) {
		return;
	}
	if (prepared->query) {
		kvs_free(prepared->query);
	}
	if (prepared->scope) {
		kvs_free(prepared->scope);
	}
	if (prepared->embedding) {
		kvs_free(prepared->embedding);
	}
	kvs_vector_search_prepared_reset(prepared);
}

int kvs_vector_set_prepare_with_embedding(
	const char *question,
	size_t question_len,
	const char *answer,
	size_t answer_len,
	const char *scope,
	size_t scope_len,
	const float *embedding,
	int embedding_dim,
	kvs_vector_set_prepared_t *prepared
) {
	size_t copied_len = 0;
	size_t embedding_bytes;
	int ret;

	if (!prepared) {
		return KVS_VECTOR_ERR_INVALID_ARG;
	}
	kvs_vector_set_prepared_reset(prepared);

	ret = kvs_vector_validate_initialized();
	if (ret != KVS_VECTOR_OK) {
		return ret;
	}

	if (!question || question_len == 0 || !answer ||
	    !embedding || embedding_dim <= 0) {
		return KVS_VECTOR_ERR_INVALID_ARG;
	}
	if (!scope) {
		scope_len = 0;
	}

	prepared->question = kvs_vector_dup_string(question, question_len, &copied_len);
	if (!prepared->question) {
		goto oom;
	}
	prepared->question_len = question_len;

	prepared->answer = kvs_vector_dup_string(answer, answer_len, &copied_len);
	if (!prepared->answer) {
		goto oom;
	}
	prepared->answer_len = answer_len;

	if (scope && scope_len > 0) {
		prepared->scope = kvs_vector_dup_string(scope, scope_len, &copied_len);
		if (!prepared->scope) {
			goto oom;
		}
		prepared->scope_len = scope_len;
	}

	embedding_bytes = (size_t)embedding_dim * sizeof(*prepared->embedding);
	prepared->embedding = (float *)kvs_malloc(embedding_bytes);
	if (!prepared->embedding) {
		goto oom;
	}
	memcpy(prepared->embedding, embedding, embedding_bytes);
	prepared->embedding_dim = embedding_dim;

	return KVS_VECTOR_OK;

oom:
	kvs_vector_set_prepared_destroy(prepared);
	return KVS_VECTOR_ERR;
}

int kvs_vector_set_prepare(
	const char *question,
	size_t question_len,
	const char *answer,
	size_t answer_len,
	const char *scope,
	size_t scope_len,
	kvs_vector_set_prepared_t *prepared
) {
	size_t copied_len = 0;
	int ret;

	if (!prepared) {
		return KVS_VECTOR_ERR_INVALID_ARG;
	}
	kvs_vector_set_prepared_reset(prepared);

	ret = kvs_vector_validate_initialized();
	if (ret != KVS_VECTOR_OK) {
		return ret;
	}

	if (!question || question_len == 0 || !answer) {
		return KVS_VECTOR_ERR_INVALID_ARG;
	}
	if (!scope) {
		scope_len = 0;
	}

	prepared->question = kvs_vector_dup_string(question, question_len, &copied_len);
	if (!prepared->question) {
		goto oom;
	}
	prepared->question_len = question_len;

	prepared->answer = kvs_vector_dup_string(answer, answer_len, &copied_len);
	if (!prepared->answer) {
		goto oom;
	}
	prepared->answer_len = answer_len;

	if (scope && scope_len > 0) {
		prepared->scope = kvs_vector_dup_string(scope, scope_len, &copied_len);
		if (!prepared->scope) {
			goto oom;
		}
		prepared->scope_len = scope_len;
	}

	ret = kvs_vector_embedding_generate(
		prepared->question,
		prepared->question_len,
		&prepared->embedding,
		&prepared->embedding_dim
	);
	if (ret != KVS_VECTOR_OK) {
		KVS_LOG_ERROR(
			KVS_VECTOR_LOG_MODULE,
			"vector set prepare embedding failed question_len=%zu scope_len=%zu ret=%d",
			question_len,
			scope_len,
			ret
		);
		kvs_vector_set_prepared_destroy(prepared);
		return ret;
	}

	return KVS_VECTOR_OK;

oom:
	kvs_vector_set_prepared_destroy(prepared);
	return KVS_VECTOR_ERR;
}

int kvs_vector_search_prepare_with_embedding(
	const char *query,
	size_t query_len,
	const char *scope,
	size_t scope_len,
	const float *embedding,
	int embedding_dim,
	kvs_vector_search_prepared_t *prepared
) {
	size_t copied_len = 0;
	size_t embedding_bytes;
	int ret;

	if (!prepared) {
		return KVS_VECTOR_ERR_INVALID_ARG;
	}
	kvs_vector_search_prepared_reset(prepared);

	ret = kvs_vector_validate_initialized();
	if (ret != KVS_VECTOR_OK) {
		return ret;
	}

	if (!query || query_len == 0 || !embedding || embedding_dim <= 0) {
		return KVS_VECTOR_ERR_INVALID_ARG;
	}
	if (!scope) {
		scope_len = 0;
	}

	prepared->query = kvs_vector_dup_string(query, query_len, &copied_len);
	if (!prepared->query) {
		goto oom;
	}
	prepared->query_len = query_len;

	if (scope && scope_len > 0) {
		prepared->scope = kvs_vector_dup_string(scope, scope_len, &copied_len);
		if (!prepared->scope) {
			goto oom;
		}
		prepared->scope_len = scope_len;
	}

	embedding_bytes = (size_t)embedding_dim * sizeof(*prepared->embedding);
	prepared->embedding = (float *)kvs_malloc(embedding_bytes);
	if (!prepared->embedding) {
		goto oom;
	}
	memcpy(prepared->embedding, embedding, embedding_bytes);
	prepared->embedding_dim = embedding_dim;

	return KVS_VECTOR_OK;

oom:
	kvs_vector_search_prepared_destroy(prepared);
	return KVS_VECTOR_ERR;
}

int kvs_vector_search_prepare(
	const char *query,
	size_t query_len,
	const char *scope,
	size_t scope_len,
	kvs_vector_search_prepared_t *prepared
) {
	size_t copied_len = 0;
	int ret;

	if (!prepared) {
		return KVS_VECTOR_ERR_INVALID_ARG;
	}
	kvs_vector_search_prepared_reset(prepared);

	ret = kvs_vector_validate_initialized();
	if (ret != KVS_VECTOR_OK) {
		return ret;
	}

	if (!query || query_len == 0) {
		return KVS_VECTOR_ERR_INVALID_ARG;
	}
	if (!scope) {
		scope_len = 0;
	}

	prepared->query = kvs_vector_dup_string(query, query_len, &copied_len);
	if (!prepared->query) {
		goto oom;
	}
	prepared->query_len = query_len;

	if (scope && scope_len > 0) {
		prepared->scope = kvs_vector_dup_string(scope, scope_len, &copied_len);
		if (!prepared->scope) {
			goto oom;
		}
		prepared->scope_len = scope_len;
	}

	ret = kvs_vector_embedding_generate(
		prepared->query,
		prepared->query_len,
		&prepared->embedding,
		&prepared->embedding_dim
	);
	if (ret != KVS_VECTOR_OK) {
		KVS_LOG_ERROR(
			KVS_VECTOR_LOG_MODULE,
			"vector search prepare embedding failed query_len=%zu scope_len=%zu ret=%d",
			query_len,
			scope_len,
			ret
		);
		kvs_vector_search_prepared_destroy(prepared);
		return ret;
	}

	return KVS_VECTOR_OK;

oom:
	kvs_vector_search_prepared_destroy(prepared);
	return KVS_VECTOR_ERR;
}

static int kvs_vector_record_take_prepared_fields(
	kvs_vector_record_t *record,
	kvs_vector_set_prepared_t *prepared
) {
	char *normalized_copy = NULL;
	int64_t now_ms;

	if (!record || !prepared || !prepared->question ||
	    prepared->question_len == 0 || !prepared->answer ||
	    !prepared->embedding || prepared->embedding_dim <= 0) {
		return KVS_VECTOR_ERR_INVALID_ARG;
	}

	normalized_copy = kvs_vector_dup_string(
		prepared->question,
		prepared->question_len,
		NULL
	);
	if (!normalized_copy) {
		return KVS_VECTOR_ERR;
	}

	kvs_vector_record_destroy(record);

	now_ms = kv_now_ms();

	record->id = g_vector_state.store.next_id++;
	record->question_hash = kvs_vector_hash_key(
		prepared->question,
		prepared->question_len
	);
	record->question = prepared->question;
	record->question_len = prepared->question_len;
	record->answer = prepared->answer;
	record->answer_len = prepared->answer_len;
	record->normalized_question = normalized_copy;
	record->normalized_question_len = prepared->question_len;
	record->scope = prepared->scope;
	record->scope_len = prepared->scope_len;
	record->embedding = prepared->embedding;
	record->embedding_dim = prepared->embedding_dim;
	record->state = KVS_VECTOR_RECORD_ACTIVE;
	record->deleted = 0;
	record->created_at_ms = now_ms;
	record->updated_at_ms = now_ms;

	record->expire_at_ms = KV_TTL_NO_EXPIRE;
	record->generation = g_vector_state.generation;
	record->embedding_model_version = kvs_vector_embedding_current_model_version();

	prepared->question = NULL;
	prepared->answer = NULL;
	prepared->scope = NULL;
	prepared->embedding = NULL;
	kvs_vector_set_prepared_reset(prepared);

	return KVS_VECTOR_OK;
}

static int kvs_vector_core_hnsw_v2_update_existing_record(
	kvs_vector_record_t *record,
	size_t record_index,
	kvs_vector_set_prepared_t *prepared
) {
	kvs_vector_store_t *store = &g_vector_state.store;
	uint64_t old_record_id;
	int ret;

	if (!record || !prepared || record_index >= store->capacity) {
		return KVS_VECTOR_ERR_INVALID_ARG;
	}

	old_record_id = record->id;
	ret = kvs_hnsw_v2_index_mark_delete(
		&g_vector_state.core_hnsw_v2_index,
		old_record_id
	);
	if (ret == KVS_VECTOR_ERR_NOT_FOUND) {
		g_vector_state.core_hnsw_v2_index.needs_rebuild = 1;
	} else if (ret != KVS_VECTOR_OK) {
		g_vector_state.core_hnsw_v2_index.needs_rebuild = 1;
		return ret;
	}

	ret = kvs_vector_record_take_prepared_fields(record, prepared);
	if (ret != KVS_VECTOR_OK) {
		g_vector_state.core_hnsw_v2_index.needs_rebuild = 1;
		return ret;
	}

	ret = kvs_vector_record_id_map_remove(
		&store->record_id_map,
		old_record_id
	);
	if (ret != KVS_VECTOR_OK && ret != KVS_VECTOR_ERR_NOT_FOUND) {
		g_vector_state.core_hnsw_v2_index.needs_rebuild = 1;
		return ret;
	}
	ret = kvs_vector_record_id_map_put(
		&store->record_id_map,
		record->id,
		record_index
	);
	if (ret != KVS_VECTOR_OK) {
		g_vector_state.core_hnsw_v2_index.needs_rebuild = 1;
		return ret;
	}

	ret = kvs_vector_flat_index_add(
		&g_vector_state.flat_index,
		record_index,
		record->embedding,
		record->embedding_dim
	);
	if (ret != KVS_VECTOR_OK) {
		g_vector_state.core_hnsw_v2_index.needs_rebuild = 1;
		return ret;
	}

	ret = kvs_vector_core_hnsw_v2_ensure_add_capacity(
		&g_vector_state.core_hnsw_v2_index
	);
	if (ret != KVS_VECTOR_OK) {
		g_vector_state.core_hnsw_v2_index.needs_rebuild = 1;
		return ret;
	}
	ret = kvs_hnsw_v2_index_add(
		&g_vector_state.core_hnsw_v2_index,
		record->id,
		record->embedding,
		(size_t)record->embedding_dim
	);
	if (ret != KVS_VECTOR_OK) {
		g_vector_state.core_hnsw_v2_index.needs_rebuild = 1;
		return ret;
	}

	g_vector_state.generation++;
	(void)kvs_vector_manager_capture_upsert_delta(
		record->id,
		record->embedding,
		record->embedding_dim
	);
	return KVS_VECTOR_OK;
}

//写入接口
int kvs_vector_set(const char* question, size_t question_len,
                    const char* answer, size_t answer_len,
                    const char* scope, size_t scope_len) {
	kvs_vector_record_t *record;
	kvs_vector_record_t *free_slot;
	kvs_vector_store_t *store;
	size_t record_index;
	int was_deleted = 0;
	int ret;

	ret = kvs_vector_validate_initialized();
	if (ret != KVS_VECTOR_OK) {
		return ret;
	}

	if (!question || question_len == 0 || !answer) {
		return KVS_VECTOR_ERR_INVALID_ARG;
	}

	if (!scope) {
		scope_len = 0;
	}

	/*
	 * 如果同 question + scope 已存在，只覆盖 answer。
	 * question 没变，所以 embedding / flat index 不需要更新。
	 */
	record = kvs_vector_find_record(question, question_len, scope, scope_len);
	if (record) {
		ret = kvs_vector_record_update_answer(record, answer, answer_len);
		if (ret != KVS_VECTOR_OK) {
			KVS_LOG_ERROR(
				KVS_VECTOR_LOG_MODULE,
				"vector set overwrite failed question_len=%zu scope_len=%zu ret=%d",
				question_len,
				scope_len,
				ret
			);
		} else {
			(void)kvs_vector_manager_capture_upsert_delta(
				record->id,
				record->embedding,
				record->embedding_dim
			);
		}
		return ret;
	}

	store = &g_vector_state.store;

	free_slot = kvs_vector_find_free_slot();
	if (!free_slot) {
		ret = kvs_vector_store_ensure_capacity(store, store->capacity + 1);
		if (ret != KVS_VECTOR_OK) {
			return ret;
		}

		free_slot = kvs_vector_find_free_slot();
		if (!free_slot) {
			return KVS_VECTOR_ERR;
		}
	}

	was_deleted = (free_slot->state == KVS_VECTOR_RECORD_DELETED) ? 1 : 0;
	record_index = (size_t)(free_slot - store->records);

	ret = kvs_vector_record_set_fields(
		free_slot,
		question,
		question_len,
		answer,
		answer_len,
		scope,
		scope_len
	);
	if (ret != KVS_VECTOR_OK) {
		KVS_LOG_ERROR(
			KVS_VECTOR_LOG_MODULE,
			"vector set failed while preparing record question_len=%zu answer_len=%zu scope_len=%zu ret=%d",
			question_len,
			answer_len,
			scope_len,
			ret
		);

		if (was_deleted && store->deleted_records > 0) {
			store->deleted_records--;
		}
		return ret;
	}

	//id_map 更新必须在 record 字段设置成功后，因为它依赖 question + scope 生成 hash。
	ret = kvs_vector_id_map_put(
		&g_vector_state.id_map,
		free_slot->question,
		free_slot->question_len,
		free_slot->scope,
		free_slot->scope_len,
		record_index
	);
	if (ret != KVS_VECTOR_OK) {
		KVS_LOG_ERROR(
			KVS_VECTOR_LOG_MODULE,
			"vector set failed while updating id map record_index=%zu question_len=%zu scope_len=%zu ret=%d",
			record_index,
			question_len,
			scope_len,
			ret
		);

		kvs_vector_record_destroy(free_slot);
		return ret;
	}

	ret = kvs_vector_record_id_map_put(
		&store->record_id_map,
		free_slot->id,
		record_index
	);
	if (ret != KVS_VECTOR_OK) {
		kvs_vector_id_map_remove(
			&g_vector_state.id_map,
			free_slot->question,
			free_slot->question_len,
			free_slot->scope,
			free_slot->scope_len
		);
		kvs_vector_record_destroy(free_slot);
		return ret;
	}

	/*
	 * Keep flat as the exact baseline / unit-test oracle for ANN recall checks.
	 * The production semantic-cache path inserts into current_index below.
	 */
	if (g_vector_state.index_type != KVS_VECTOR_INDEX_TYPE_SQ8_FLAT) {
		ret = kvs_vector_flat_index_add(
			&g_vector_state.flat_index,
			record_index,
			free_slot->embedding,
			free_slot->embedding_dim
		);
		if (ret != KVS_VECTOR_OK) {
			KVS_LOG_ERROR(
				KVS_VECTOR_LOG_MODULE,
				"vector set failed while adding flat index record_index=%zu embedding_dim=%d index_dimension=%zu ret=%d",
				record_index,
				free_slot->embedding_dim,
				g_vector_state.flat_index.dimension,
				ret
			);

			kvs_vector_id_map_remove(
				&g_vector_state.id_map,
				free_slot->question,
				free_slot->question_len,
				free_slot->scope,
				free_slot->scope_len
			);
			kvs_vector_record_id_map_remove(
				&store->record_id_map,
				free_slot->id
			);
			kvs_vector_record_destroy(free_slot);
			return ret;
		}
	}

	if (g_vector_state.index_type != KVS_VECTOR_INDEX_TYPE_FLAT) {
		ret = kvs_vector_index_add_current(
			record_index,
			free_slot->embedding,
			free_slot->embedding_dim
		);
		if (ret != KVS_VECTOR_OK) {
			KVS_LOG_ERROR(
				KVS_VECTOR_LOG_MODULE,
				"vector set failed while adding current index record_index=%zu embedding_dim=%d ret=%d",
				record_index,
				free_slot->embedding_dim,
				ret
			);

			kvs_vector_id_map_remove(
				&g_vector_state.id_map,
				free_slot->question,
				free_slot->question_len,
				free_slot->scope,
				free_slot->scope_len
			);
			kvs_vector_record_id_map_remove(
				&store->record_id_map,
				free_slot->id
			);
			kvs_vector_record_destroy(free_slot);
			return ret;
		}
	}

	if (was_deleted && store->deleted_records > 0) {
		store->deleted_records--;
	}

	store->active_records++;
	store->total_records = store->active_records + store->deleted_records;

	g_vector_state.generation++;

	(void)kvs_vector_manager_capture_upsert_delta(
		free_slot->id,
		free_slot->embedding,
		free_slot->embedding_dim
	);

	/*
	 * CLEANUP_CANDIDATE:
	 * Too noisy during batch import. Re-enable only for single-record debugging.
	 */
	/*
	KVS_LOG_DEBUG(
		KVS_VECTOR_LOG_MODULE,
		"vector set success question_len=%zu scope_len=%zu embedding_dim=%d active_records=%zu",
		question_len,
		scope_len,
		free_slot->embedding_dim,
		store->active_records
	);
	*/

	return KVS_VECTOR_OK;
}

int kvs_vector_set_commit_prepared(kvs_vector_set_prepared_t *prepared) {
	kvs_vector_record_t *record;
	kvs_vector_record_t *free_slot;
	kvs_vector_store_t *store;
	size_t record_index;
	int was_deleted = 0;
	int ret;
	uint64_t commit_start_us = kvs_vector_now_us();
	uint64_t start_us;
	uint64_t end_us;

	ret = kvs_vector_validate_initialized();
	if (ret != KVS_VECTOR_OK) {
		kvs_vector_set_prepared_destroy(prepared);
		kvs_vector_record_timing(
			&g_vset_commit_stats.vset_commit_count,
			&g_vset_commit_stats.vset_commit_total_us,
			&g_vset_commit_stats.vset_commit_max_us,
			kvs_vector_now_us() >= commit_start_us ?
				kvs_vector_now_us() - commit_start_us : 0
		);
		return ret;
	}

	if (!prepared || !prepared->question || prepared->question_len == 0 ||
	    !prepared->answer || !prepared->embedding || prepared->embedding_dim <= 0) {
		kvs_vector_set_prepared_destroy(prepared);
		kvs_vector_record_timing(
			&g_vset_commit_stats.vset_commit_count,
			&g_vset_commit_stats.vset_commit_total_us,
			&g_vset_commit_stats.vset_commit_max_us,
			kvs_vector_now_us() >= commit_start_us ?
				kvs_vector_now_us() - commit_start_us : 0
		);
		return KVS_VECTOR_ERR_INVALID_ARG;
	}

	/*
	 * Stage-1 commit rechecks question+scope under the write lock. If another
	 * VSET inserted the same key while embedding was prepared, keep existing
	 * semantics: update answer only and discard the unused embedding.
	 */
	start_us = kvs_vector_now_us();
	record = kvs_vector_find_record(
		prepared->question,
		prepared->question_len,
		prepared->scope,
		prepared->scope_len
	);
	end_us = kvs_vector_now_us();
	kvs_vector_record_timing(
		&g_vset_commit_stats.vset_record_lookup_count,
		&g_vset_commit_stats.vset_record_lookup_total_us,
		&g_vset_commit_stats.vset_record_lookup_max_us,
		end_us >= start_us ? end_us - start_us : 0
	);
	if (record) {
		size_t existing_record_index =
			(size_t)(record - g_vector_state.store.records);

		if (g_vector_state.index_type == KVS_VECTOR_INDEX_CORE_HNSW_V2) {
			ret = kvs_vector_core_hnsw_v2_update_existing_record(
				record,
				existing_record_index,
				prepared
			);
			kvs_vector_record_timing(
				&g_vset_commit_stats.vset_record_insert_or_update_count,
				&g_vset_commit_stats.vset_record_insert_or_update_total_us,
				&g_vset_commit_stats.vset_record_insert_or_update_max_us,
				kvs_vector_now_us() >= start_us ?
					kvs_vector_now_us() - start_us : 0
			);
			kvs_vector_record_timing(
				&g_vset_commit_stats.vset_backend_add_count,
				&g_vset_commit_stats.vset_backend_add_total_us,
				&g_vset_commit_stats.vset_backend_add_max_us,
				kvs_vector_now_us() >= start_us ?
					kvs_vector_now_us() - start_us : 0
			);
			kvs_vector_record_timing(
				&g_vset_commit_stats.vset_commit_count,
				&g_vset_commit_stats.vset_commit_total_us,
				&g_vset_commit_stats.vset_commit_max_us,
				kvs_vector_now_us() >= commit_start_us ?
					kvs_vector_now_us() - commit_start_us : 0
			);
			if (ret != KVS_VECTOR_OK) {
				kvs_vector_set_prepared_destroy(prepared);
			}
			return ret;
		}

		start_us = kvs_vector_now_us();
		ret = kvs_vector_record_update_answer(
			record,
			prepared->answer,
			prepared->answer_len
		);
		end_us = kvs_vector_now_us();
		kvs_vector_record_timing(
			&g_vset_commit_stats.vset_record_insert_or_update_count,
			&g_vset_commit_stats.vset_record_insert_or_update_total_us,
			&g_vset_commit_stats.vset_record_insert_or_update_max_us,
			end_us >= start_us ? end_us - start_us : 0
		);
		kvs_vector_record_timing(
			&g_vset_commit_stats.vset_scope_answer_question_update_count,
			&g_vset_commit_stats.vset_scope_answer_question_update_total_us,
			&g_vset_commit_stats.vset_scope_answer_question_update_max_us,
			end_us >= start_us ? end_us - start_us : 0
		);
		kvs_vector_set_prepared_destroy(prepared);
		end_us = kvs_vector_now_us();
		kvs_vector_record_timing(
			&g_vset_commit_stats.vset_commit_count,
			&g_vset_commit_stats.vset_commit_total_us,
			&g_vset_commit_stats.vset_commit_max_us,
			end_us >= commit_start_us ? end_us - commit_start_us : 0
		);
		if (ret == KVS_VECTOR_OK) {
			(void)kvs_vector_manager_capture_upsert_delta(
				record->id,
				record->embedding,
				record->embedding_dim
			);
		}
		return ret;
	}

	store = &g_vector_state.store;

	start_us = kvs_vector_now_us();
	free_slot = kvs_vector_find_free_slot();
	if (!free_slot) {
		ret = kvs_vector_store_ensure_capacity(store, store->capacity + 1);
		if (ret != KVS_VECTOR_OK) {
			kvs_vector_set_prepared_destroy(prepared);
			end_us = kvs_vector_now_us();
			kvs_vector_record_timing(
				&g_vset_commit_stats.vset_record_insert_or_update_count,
				&g_vset_commit_stats.vset_record_insert_or_update_total_us,
				&g_vset_commit_stats.vset_record_insert_or_update_max_us,
				end_us >= start_us ? end_us - start_us : 0
			);
			kvs_vector_record_timing(
				&g_vset_commit_stats.vset_commit_count,
				&g_vset_commit_stats.vset_commit_total_us,
				&g_vset_commit_stats.vset_commit_max_us,
				end_us >= commit_start_us ? end_us - commit_start_us : 0
			);
			return ret;
		}

		free_slot = kvs_vector_find_free_slot();
		if (!free_slot) {
			kvs_vector_set_prepared_destroy(prepared);
			end_us = kvs_vector_now_us();
			kvs_vector_record_timing(
				&g_vset_commit_stats.vset_record_insert_or_update_count,
				&g_vset_commit_stats.vset_record_insert_or_update_total_us,
				&g_vset_commit_stats.vset_record_insert_or_update_max_us,
				end_us >= start_us ? end_us - start_us : 0
			);
			kvs_vector_record_timing(
				&g_vset_commit_stats.vset_commit_count,
				&g_vset_commit_stats.vset_commit_total_us,
				&g_vset_commit_stats.vset_commit_max_us,
				end_us >= commit_start_us ? end_us - commit_start_us : 0
			);
			return KVS_VECTOR_ERR;
		}
	}

	was_deleted = (free_slot->state == KVS_VECTOR_RECORD_DELETED) ? 1 : 0;
	record_index = (size_t)(free_slot - store->records);
	end_us = kvs_vector_now_us();
	kvs_vector_record_timing(
		&g_vset_commit_stats.vset_record_insert_or_update_count,
		&g_vset_commit_stats.vset_record_insert_or_update_total_us,
		&g_vset_commit_stats.vset_record_insert_or_update_max_us,
		end_us >= start_us ? end_us - start_us : 0
	);

	start_us = kvs_vector_now_us();
	ret = kvs_vector_record_take_prepared_fields(free_slot, prepared);
	end_us = kvs_vector_now_us();
	kvs_vector_record_timing(
		&g_vset_commit_stats.vset_vector_copy_count,
		&g_vset_commit_stats.vset_vector_copy_total_us,
		&g_vset_commit_stats.vset_vector_copy_max_us,
		end_us >= start_us ? end_us - start_us : 0
	);
	kvs_vector_record_timing(
		&g_vset_commit_stats.vset_scope_answer_question_update_count,
		&g_vset_commit_stats.vset_scope_answer_question_update_total_us,
		&g_vset_commit_stats.vset_scope_answer_question_update_max_us,
		end_us >= start_us ? end_us - start_us : 0
	);
	if (ret != KVS_VECTOR_OK) {
		if (was_deleted && store->deleted_records > 0) {
			store->deleted_records--;
		}
		kvs_vector_set_prepared_destroy(prepared);
		kvs_vector_record_timing(
			&g_vset_commit_stats.vset_commit_count,
			&g_vset_commit_stats.vset_commit_total_us,
			&g_vset_commit_stats.vset_commit_max_us,
			end_us >= commit_start_us ? end_us - commit_start_us : 0
		);
		return ret;
	}

	start_us = kvs_vector_now_us();
	ret = kvs_vector_id_map_put(
		&g_vector_state.id_map,
		free_slot->question,
		free_slot->question_len,
		free_slot->scope,
		free_slot->scope_len,
		record_index
	);
	end_us = kvs_vector_now_us();
	kvs_vector_record_timing(
		&g_vset_commit_stats.vset_id_map_update_count,
		&g_vset_commit_stats.vset_id_map_update_total_us,
		&g_vset_commit_stats.vset_id_map_update_max_us,
		end_us >= start_us ? end_us - start_us : 0
	);
	if (ret != KVS_VECTOR_OK) {
		kvs_vector_record_destroy(free_slot);
		kvs_vector_record_timing(
			&g_vset_commit_stats.vset_commit_count,
			&g_vset_commit_stats.vset_commit_total_us,
			&g_vset_commit_stats.vset_commit_max_us,
			end_us >= commit_start_us ? end_us - commit_start_us : 0
		);
		return ret;
	}

	ret = kvs_vector_record_id_map_put(
		&store->record_id_map,
		free_slot->id,
		record_index
	);
	if (ret != KVS_VECTOR_OK) {
		kvs_vector_id_map_remove(
			&g_vector_state.id_map,
			free_slot->question,
			free_slot->question_len,
			free_slot->scope,
			free_slot->scope_len
		);
		kvs_vector_record_destroy(free_slot);
		kvs_vector_record_timing(
			&g_vset_commit_stats.vset_commit_count,
			&g_vset_commit_stats.vset_commit_total_us,
			&g_vset_commit_stats.vset_commit_max_us,
			end_us >= commit_start_us ? end_us - commit_start_us : 0
		);
		return ret;
	}

	if (g_vector_state.index_type != KVS_VECTOR_INDEX_TYPE_SQ8_FLAT) {
		start_us = kvs_vector_now_us();
		ret = kvs_vector_flat_index_add(
			&g_vector_state.flat_index,
			record_index,
			free_slot->embedding,
			free_slot->embedding_dim
		);
		end_us = kvs_vector_now_us();
		kvs_vector_record_timing(
			&g_vset_commit_stats.vset_flat_index_update_count,
			&g_vset_commit_stats.vset_flat_index_update_total_us,
			&g_vset_commit_stats.vset_flat_index_update_max_us,
			end_us >= start_us ? end_us - start_us : 0
		);
		if (ret != KVS_VECTOR_OK) {
			kvs_vector_id_map_remove(
				&g_vector_state.id_map,
				free_slot->question,
				free_slot->question_len,
				free_slot->scope,
				free_slot->scope_len
			);
			kvs_vector_record_id_map_remove(
				&store->record_id_map,
				free_slot->id
			);
			kvs_vector_record_destroy(free_slot);
			kvs_vector_record_timing(
				&g_vset_commit_stats.vset_commit_count,
				&g_vset_commit_stats.vset_commit_total_us,
				&g_vset_commit_stats.vset_commit_max_us,
				end_us >= commit_start_us ? end_us - commit_start_us : 0
			);
			return ret;
		}
	}

	if (g_vector_state.index_type != KVS_VECTOR_INDEX_TYPE_FLAT) {
		start_us = kvs_vector_now_us();
		ret = kvs_vector_index_add_current(
			record_index,
			free_slot->embedding,
			free_slot->embedding_dim
		);
		end_us = kvs_vector_now_us();
		kvs_vector_record_timing(
			&g_vset_commit_stats.vset_backend_add_count,
			&g_vset_commit_stats.vset_backend_add_total_us,
			&g_vset_commit_stats.vset_backend_add_max_us,
			end_us >= start_us ? end_us - start_us : 0
		);
		if (ret != KVS_VECTOR_OK) {
			kvs_vector_id_map_remove(
				&g_vector_state.id_map,
				free_slot->question,
				free_slot->question_len,
				free_slot->scope,
				free_slot->scope_len
			);
			kvs_vector_record_id_map_remove(
				&store->record_id_map,
				free_slot->id
			);
			kvs_vector_record_destroy(free_slot);
			kvs_vector_record_timing(
				&g_vset_commit_stats.vset_commit_count,
				&g_vset_commit_stats.vset_commit_total_us,
				&g_vset_commit_stats.vset_commit_max_us,
				end_us >= commit_start_us ? end_us - commit_start_us : 0
			);
			return ret;
		}
	}

	start_us = kvs_vector_now_us();
	if (was_deleted && store->deleted_records > 0) {
		store->deleted_records--;
	}

	store->active_records++;
	store->total_records = store->active_records + store->deleted_records;
	end_us = kvs_vector_now_us();
	kvs_vector_record_timing(
		&g_vset_commit_stats.vset_record_insert_or_update_count,
		&g_vset_commit_stats.vset_record_insert_or_update_total_us,
		&g_vset_commit_stats.vset_record_insert_or_update_max_us,
		end_us >= start_us ? end_us - start_us : 0
	);
	kvs_vector_record_timing(
		&g_vset_commit_stats.vset_dirty_mark_count,
		&g_vset_commit_stats.vset_dirty_mark_total_us,
		&g_vset_commit_stats.vset_dirty_mark_max_us,
		end_us >= start_us ? end_us - start_us : 0
	);

	kvs_vector_record_timing(
		&g_vset_commit_stats.vset_commit_count,
		&g_vset_commit_stats.vset_commit_total_us,
		&g_vset_commit_stats.vset_commit_max_us,
		end_us >= commit_start_us ? end_us - commit_start_us : 0
	);

	g_vector_state.generation++;

	(void)kvs_vector_manager_capture_upsert_delta(
		free_slot->id,
		free_slot->embedding,
		free_slot->embedding_dim
	);

	return KVS_VECTOR_OK;
}

/*
 * 批量写入内部实现。
 *
 * 注意：
 * 这个函数在 vector async worker 中执行。
 * 当前保持 worker_count=1，避免多个 batch 同时修改 store/id_map/flat_index。
 */

/*
 * 纯存储层批量写入内部实现。
 *
 * 该函数用于压测 KVLite vector 存储层本身，不调用 embedding provider。
 *
 * 压测覆盖：
 * 1. question / answer / scope 字符串复制；
 * 2. record 分配和字段初始化；
 * 3. id_map 写入；
 * 4. flat_index 追加；
 * 5. 384 维 float 向量复制。
 *
 * embeddings 布局：
 *
 *   embeddings[i * embedding_dim + j]
 *
 * 注意：
 * 当前函数假设由单线程调用，和 kvs_vector_set_batch_internal 一样，
 * 不额外加锁。外层如果并发调用，需要先保护 g_vector_state。
 */

/*
 * 批量写入异步入口。
 *
 * 当前语义：
 * 1. 投递到 msg_queue；
 * 2. 后台 worker 执行；
 * 3. 当前调用线程等待完成；
 * 4. 返回时数据已经写入。
 */

//查询接口
int kvs_vector_get(
	const char *question,
	size_t question_len,
	const char *scope,
	size_t scope_len,
	kvs_vector_search_result_t *result
) {
	kvs_vector_record_t *record;
	int ret;

	if (result) {
		kvs_vector_search_result_reset(result);
	}

	ret = kvs_vector_validate_initialized();
	if (ret != KVS_VECTOR_OK) {
		return ret;
	}

	if (!question || question_len == 0) {
		return KVS_VECTOR_ERR_INVALID_ARG;
	}

	if (!scope) {
		scope_len = 0;
	}

	record = kvs_vector_find_record(question, question_len, scope, scope_len);
	if (!record) {
		return KVS_VECTOR_ERR_NOT_FOUND;
	}

	if (result) {
		kvs_vector_search_result_from_record(result, record);
	}

	return KVS_VECTOR_OK;
}

//检索接口
int kvs_vector_search(
	const char *query,
	size_t query_len,
	const char *scope,
	size_t scope_len,
	int topk,
	float threshold,
	kvs_vector_search_result_t *results,
	size_t results_capacity,
	size_t *results_count
) {
	float *query_embedding = NULL;
	int query_dim = 0;
	int ret;
	float effective_threshold;

	if (results && results_capacity > 0) {
		kvs_vector_search_results_reset(results, results_capacity);
	}

	if (results_count) {
		*results_count = 0;
	}

	ret = kvs_vector_validate_initialized();
	if (ret != KVS_VECTOR_OK) {
		return ret;
	}

	if (!query || query_len == 0 ||
	    !results || results_capacity == 0 || !results_count) {
		return KVS_VECTOR_ERR_INVALID_ARG;
	}

	if (!scope) {
		scope_len = 0;
	}

	if (topk <= 0) {
		topk = 1;
	}

	effective_threshold = threshold;
	if (effective_threshold <= 0.0f) {
		effective_threshold = -1.0f;
	}

	ret = kvs_vector_embedding_generate(
		query,
		query_len,
		&query_embedding,
		&query_dim
	);
	if (ret != KVS_VECTOR_OK) {
		KVS_LOG_ERROR(
			KVS_VECTOR_LOG_MODULE,
			"vector search embedding generate failed query_len=%zu scope_len=%zu ret=%d",
			query_len,
			scope_len,
			ret
		);
		return ret;
	}

	ret = kvs_vector_index_search_current(
		query_embedding,
		query_dim,
		scope,
		scope_len,
		topk,
		effective_threshold,
		results,
		results_capacity,
		results_count
	);

	kvs_free(query_embedding);
	return ret;
}

int kvs_vector_search_with_prepared(
	const kvs_vector_search_prepared_t *prepared,
	int topk,
	float threshold,
	kvs_vector_search_result_t *results,
	size_t results_capacity,
	size_t *results_count
) {
	int ret;
	float effective_threshold;

	if (results && results_capacity > 0) {
		kvs_vector_search_results_reset(results, results_capacity);
	}
	if (results_count) {
		*results_count = 0;
	}

	ret = kvs_vector_validate_initialized();
	if (ret != KVS_VECTOR_OK) {
		return ret;
	}

	if (!prepared || !prepared->query || prepared->query_len == 0 ||
	    !prepared->embedding || prepared->embedding_dim <= 0 ||
	    !results || results_capacity == 0 || !results_count) {
		return KVS_VECTOR_ERR_INVALID_ARG;
	}

	if (topk <= 0) {
		topk = 1;
	}

	effective_threshold = threshold;
	if (effective_threshold <= 0.0f) {
		effective_threshold = -1.0f;
	}

	return kvs_vector_index_search_current(
		prepared->embedding,
		prepared->embedding_dim,
		prepared->scope,
		prepared->scope_len,
		topk,
		effective_threshold,
		results,
		results_capacity,
		results_count
	);
}

/*
 * 批量检索接口。
 *
 * 关键优化点：
 * 1. 单条 kvs_vector_search() 每次都会调用一次 embedding provider；
 * 2. 每次调用会触发一次 embedding 生成；
 * 3. 批量入口先统一调用 kvs_vector_embedding_generate_batch()，
 *    然后对每条 query embedding 复用现有 HNSW / flat 搜索逻辑。
 *
 * results 布局：
 *   results[i * results_capacity_per_query + j]
 *
 * results_counts 布局：
 *   results_counts[i]
 */

/*
 * 纯搜索层压测接口。
 *
 * 该函数不生成 query embedding，而是直接使用调用方传入的 query_embedding。
 *
 * 目的：
 * 1. 绕过 embedding provider；
 * 2. 避免 hash provider 64 维和 mock 384 维 index 不一致；
 * 3. 单独压测 flat scan + dot product + topK heap 性能。
 */

//删除接口
int kvs_vector_del(
	const char *question,
	size_t question_len,
	const char *scope,
	size_t scope_len
) {
	kvs_vector_record_t *record;
	kvs_vector_store_t *store;
	size_t record_index;
	uint64_t record_id;
	int ret;

	ret = kvs_vector_validate_initialized();
	if (ret != KVS_VECTOR_OK) {
		return ret;
	}

	if (!question || question_len == 0) {
		return KVS_VECTOR_ERR_INVALID_ARG;
	}

	if (!scope) {
		scope_len = 0;
	}

	record = kvs_vector_find_record(question, question_len, scope, scope_len);
	if (!record) {
		return KVS_VECTOR_ERR_NOT_FOUND;
	}
	record_id = record->id;

	ret = kvs_vector_record_mark_deleted(record);
	if (ret != KVS_VECTOR_OK) {
		return ret;
	}

	if (g_vector_state.index_type == KVS_VECTOR_INDEX_CORE_FLAT) {
		ret = kvs_flat_index_mark_delete(
			&g_vector_state.core_flat_index,
			record_id
		);
		if (ret == KVS_VECTOR_ERR_NOT_FOUND) {
			g_vector_state.core_flat_index.needs_rebuild = 1;
		} else if (ret != KVS_VECTOR_OK) {
			KVS_LOG_DEBUG(
				KVS_VECTOR_LOG_MODULE,
				"core_flat mark_delete ignored record_id=%llu ret=%d",
				(unsigned long long)record_id,
				ret
			);
			g_vector_state.core_flat_index.needs_rebuild = 1;
		}
	}
	if (g_vector_state.index_type == KVS_VECTOR_INDEX_CORE_HNSW) {
		ret = kvs_hnsw_index_mark_delete(
			&g_vector_state.core_hnsw_index,
			record_id
		);
		if (ret == KVS_VECTOR_ERR_NOT_FOUND) {
			g_vector_state.core_hnsw_index.needs_rebuild = 1;
		} else if (ret != KVS_VECTOR_OK) {
			KVS_LOG_DEBUG(
				KVS_VECTOR_LOG_MODULE,
				"core_hnsw mark_delete ignored record_id=%llu ret=%d",
				(unsigned long long)record_id,
				ret
			);
			g_vector_state.core_hnsw_index.needs_rebuild = 1;
		}
	}
	if (g_vector_state.index_type == KVS_VECTOR_INDEX_CORE_HNSW_V2) {
		ret = kvs_hnsw_v2_index_mark_delete(
			&g_vector_state.core_hnsw_v2_index,
			record_id
		);
		if (ret == KVS_VECTOR_ERR_NOT_FOUND) {
			g_vector_state.core_hnsw_v2_index.needs_rebuild = 1;
		} else if (ret != KVS_VECTOR_OK) {
			KVS_LOG_DEBUG(
				KVS_VECTOR_LOG_MODULE,
				"core_hnsw_v2 mark_delete ignored record_id=%llu ret=%d",
				(unsigned long long)record_id,
				ret
			);
			g_vector_state.core_hnsw_v2_index.needs_rebuild = 1;
		}
	}

	ret = kvs_vector_id_map_remove(
		&g_vector_state.id_map,
		question,
		question_len,
		scope,
		scope_len
	);
	if (ret != KVS_VECTOR_OK && ret != KVS_VECTOR_ERR_NOT_FOUND) {
		KVS_LOG_ERROR(
			KVS_VECTOR_LOG_MODULE,
			"vector del failed while removing id map question_len=%zu scope_len=%zu ret=%d",
			question_len,
			scope_len,
			ret
		);
		return ret;
	}
	ret = kvs_vector_record_id_map_remove(
		&g_vector_state.store.record_id_map,
		record_id
	);
	if (ret != KVS_VECTOR_OK && ret != KVS_VECTOR_ERR_NOT_FOUND) {
		KVS_LOG_ERROR(
			KVS_VECTOR_LOG_MODULE,
			"vector del failed while removing record_id map record_id=%llu ret=%d",
			(unsigned long long)record_id,
			ret
		);
		return ret;
	}

	store = &g_vector_state.store;

	record_index = (size_t)(record - store->records);
	//放入空闲链表中
	ret = kvs_vector_store_free_list_push(store, record_index);
	if (ret != KVS_VECTOR_OK) {
		KVS_LOG_ERROR(
			KVS_VECTOR_LOG_MODULE,
			"vector del failed while pushing free list record_index=%zu ret=%d",
			record_index,
			ret
		);
		return ret;
	}

	if (store->active_records > 0) {
		store->active_records--;
	}

	store->deleted_records++;
	store->total_records = store->active_records + store->deleted_records;

	kvs_vector_flat_index_mark_stale(&g_vector_state.flat_index);
	g_vector_state.sq8_flat_index.needs_rebuild = 1;

	g_vector_state.generation++;

	(void)kvs_vector_manager_capture_delete_delta(record_id);

	return KVS_VECTOR_OK;
}

//重建接口 — safe rebuild with index swap
int kvs_vector_index_rebuild(void) {
	kvs_vector_flat_index_t temp_flat;
	kvs_vector_ann_index_t temp_ann;
	kvs_vector_sq8_flat_index_t temp_sq8;
	kvs_vector_store_t *store;
	size_t dimension;
	long long start_ms;
	long long elapsed_ms;
	int ret;
	int old_index_ready = 0;

	ret = kvs_vector_validate_initialized();
	if (ret != KVS_VECTOR_OK) return ret;

	start_ms = (long long)kv_now_ms();
	store = &g_vector_state.store;
	dimension = g_vector_state.flat_index.dimension;

	KVS_LOG_INFO(KVS_VECTOR_LOG_MODULE,
		"vector index safe rebuild start active=%zu deleted=%zu dim=%zu",
		store->active_records, store->deleted_records, dimension);

	/* Step 1: compact store and rebuild id_map (always needed). */
	ret = kvs_vector_store_compact(store);
	if (ret != KVS_VECTOR_OK) goto fail_rebuild;

	ret = kvs_vector_id_map_rebuild_from_store();
	if (ret != KVS_VECTOR_OK) goto fail_rebuild;

	/* Step 2: build temp flat index. */
	memset(&temp_flat, 0, sizeof(temp_flat));
	ret = kvs_vector_flat_index_init(&temp_flat, dimension);
	if (ret != KVS_VECTOR_OK) goto fail_rebuild;

	if (g_vector_state.index_type != KVS_VECTOR_INDEX_TYPE_SQ8_FLAT) {
		size_t i;
		/* reserve temp flat */
		if (store->active_records > 0) {
			ret = kvs_vector_flat_index_reserve(&temp_flat, store->active_records);
			if (ret != KVS_VECTOR_OK) goto fail_flat;
		}
		for (i = 0; i < store->capacity; ++i) {
			kvs_vector_record_t *rec = &store->records[i];
			if (rec->state != KVS_VECTOR_RECORD_ACTIVE || rec->deleted) continue;
			if (!rec->embedding || rec->embedding_dim <= 0) continue;
			ret = kvs_vector_flat_index_add(&temp_flat, i, rec->embedding, rec->embedding_dim);
			if (ret != KVS_VECTOR_OK) goto fail_flat;
		}
	}
	temp_flat.ready = 1;
	temp_flat.needs_rebuild = 0;

	/* Step 3: build temp type-specific index. */
	memset(&temp_ann, 0, sizeof(temp_ann));
	memset(&temp_sq8, 0, sizeof(temp_sq8));

	if (g_vector_state.index_type == KVS_VECTOR_INDEX_TYPE_HNSW) {
		size_t i;
		ret = kvs_vector_ann_index_init(&temp_ann, dimension,
			g_vector_state.ann_index.m, g_vector_state.ann_index.ef_construction,
			g_vector_state.ann_index.ef_search);
		if (ret != KVS_VECTOR_OK) goto fail_ann;
		for (i = 0; i < store->capacity; ++i) {
			kvs_vector_record_t *rec = &store->records[i];
			if (rec->state != KVS_VECTOR_RECORD_ACTIVE || rec->deleted) continue;
			if (!rec->embedding || rec->embedding_dim <= 0) continue;
			ret = kvs_vector_ann_index_add(&temp_ann, i, rec->embedding, rec->embedding_dim);
			if (ret != KVS_VECTOR_OK) goto fail_ann;
		}
		temp_ann.ready = 1;
		temp_ann.needs_rebuild = 0;


	} else if (g_vector_state.index_type == KVS_VECTOR_INDEX_TYPE_SQ8_FLAT) {
		ret = kvs_vector_sq8_flat_index_init(&temp_sq8, dimension);
		if (ret != KVS_VECTOR_OK) goto fail_sq8;
		ret = kvs_vector_sq8_flat_index_rebuild(&temp_sq8);
		if (ret != KVS_VECTOR_OK) goto fail_sq8;
	}

	/* Step 4: all temp indexes built successfully — swap them in. */
	if (g_vector_state.index_type != KVS_VECTOR_INDEX_TYPE_SQ8_FLAT) {
		kvs_vector_flat_index_destroy(&g_vector_state.flat_index);
		g_vector_state.flat_index = temp_flat;
	}

	switch (g_vector_state.index_type) {
		case KVS_VECTOR_INDEX_TYPE_HNSW:
			kvs_vector_ann_index_destroy(&g_vector_state.ann_index);
			g_vector_state.ann_index = temp_ann;
			break;
			break;
		case KVS_VECTOR_INDEX_TYPE_SQ8_FLAT:
			kvs_vector_sq8_flat_index_destroy(&g_vector_state.sq8_flat_index);
			g_vector_state.sq8_flat_index = temp_sq8;
			break;
		case KVS_VECTOR_INDEX_CORE_FLAT:
			ret = kvs_vector_core_flat_ops_rebuild(&g_vector_state.current_index);
			if (ret != KVS_VECTOR_OK) goto fail_rebuild;
			break;
		case KVS_VECTOR_INDEX_CORE_HNSW:
			ret = kvs_vector_core_hnsw_ops_rebuild(&g_vector_state.current_index);
			if (ret != KVS_VECTOR_OK) goto fail_rebuild;
			break;
		case KVS_VECTOR_INDEX_CORE_HNSW_V2:
			ret = kvs_vector_core_hnsw_v2_ops_rebuild(&g_vector_state.current_index);
			if (ret != KVS_VECTOR_OK) goto fail_rebuild;
			break;
		default:
			break;
	}

	ret = kvs_vector_index_bind_current();
	if (ret != KVS_VECTOR_OK) goto fail_rebuild;

	elapsed_ms = (long long)kv_now_ms() - start_ms;
	g_vector_state.last_rebuild_status = 1;
	g_vector_state.last_rebuild_ms = elapsed_ms;
	g_vector_state.last_rebuild_at_ms = (long long)kv_now_ms();

	KVS_LOG_INFO(KVS_VECTOR_LOG_MODULE,
		"vector index safe rebuild success active=%zu flat_size=%zu index_ready=%d elapsed=%lldms",
		store->active_records, g_vector_state.flat_index.size,
		old_index_ready, (long long)elapsed_ms);

	g_vector_state.generation++;

	return KVS_VECTOR_OK;

fail_ann:
	kvs_vector_ann_index_destroy(&temp_ann);
	goto fail_type_specific;
	goto fail_type_specific;
fail_sq8:
	kvs_vector_sq8_flat_index_destroy(&temp_sq8);
	goto fail_type_specific;
fail_type_specific:
fail_flat:
	kvs_vector_flat_index_destroy(&temp_flat);
fail_rebuild:
	elapsed_ms = (long long)kv_now_ms() - start_ms;
	g_vector_state.last_rebuild_status = -1;
	g_vector_state.last_rebuild_ms = elapsed_ms;
	g_vector_state.last_rebuild_at_ms = (long long)kv_now_ms();

	KVS_LOG_ERROR(KVS_VECTOR_LOG_MODULE,
		"vector index safe rebuild failed ret=%d elapsed=%lldms (old index preserved)",
		ret, (long long)elapsed_ms);

	return ret;
}

/*
 * VINFO 统计接口。
 *
 * 当前已经接入内部真实状态。
 * 即使没有任何数据，也会返回初始化后的 store/index 统计。
 */
int kvs_vector_info(kvs_vector_info_t *info) {
	if (!info) {
		return KVS_VECTOR_ERR_INVALID_ARG;
	}

	kvs_vector_info_from_state(info, &g_vector_state);
	if (g_vector_state.initialized &&
	    g_vector_state.current_index.ops &&
	    g_vector_state.current_index.ops->info) {
		g_vector_state.current_index.ops->info(&g_vector_state.current_index, info);
	}
	kvs_vector_fill_backend_stats(info);

	return KVS_VECTOR_OK;
}

/*
 * 清空单条搜索结果。
 *
 * 注意：
 * 当前不释放 question / answer / scope 指针。
 * result 字段所有权还没有对外定义。
 */
void kvs_vector_search_result_reset(kvs_vector_search_result_t *result) {
	if (!result) {
		return;
	}

	memset(result, 0, sizeof(*result));
}

/*
 * TTL filtering:
 * Keep current behavior unchanged in Stage 1C.
 * If legacy search path applies lazy TTL elsewhere, do not duplicate it here.
 * Move TTL filtering here in Step 1E when manager filtering is centralized.
 */

int kvs_vector_manager_resolve_hits(
	const kvs_vector_hit_t *hits,
	size_t hit_count,
	const char *scope,
	size_t scope_len,
	float threshold,
	kvs_vector_search_result_t *results,
	size_t results_capacity,
	size_t *results_count
) {
	size_t i;
	size_t out_count = 0;
	uint64_t start_us;
	uint64_t end_us;
	uint64_t elapsed_us;

	if (!hits || !results || !results_count) {
		return KVS_VECTOR_ERR_INVALID_ARG;
	}

	*results_count = 0;
	start_us = kvs_vector_now_us();

	for (i = 0; i < hit_count && out_count < results_capacity; i++) {
		const kvs_vector_hit_t *hit = &hits[i];
		kvs_vector_record_t *record = NULL;

		if (hit->score < threshold) {
			continue;
		}

		if (kvs_vector_manager_get_record_by_id(hit->record_id, &record) != KVS_VECTOR_OK) {
			continue;
		}

		if (!kvs_vector_record_scope_match(record, scope, scope_len)) {
			continue;
		}

		kvs_vector_search_result_from_record(&results[out_count], record);
		results[out_count].score = hit->score;
		out_count++;
	}

	*results_count = out_count;
	end_us = kvs_vector_now_us();
	elapsed_us = end_us >= start_us ? end_us - start_us : 0;
	g_vsearch_current_resolve_us += elapsed_us;
	kvs_vector_vsearch_record_manager_resolve(elapsed_us, hit_count, out_count);
	return KVS_VECTOR_OK;
}

int kvs_vector_manager_get_record_by_id(
	uint64_t record_id,
	kvs_vector_record_t **out_record
) {
	kvs_vector_store_t *store;
	kvs_vector_record_t *record;
	size_t record_index = 0;
	int ret;

	if (!out_record) {
		return KVS_VECTOR_ERR_INVALID_ARG;
	}

	*out_record = NULL;
	store = &g_vector_state.store;
	if (!store->records || store->capacity == 0) {
		return KVS_VECTOR_ERR_NOT_FOUND;
	}

	ret = kvs_vector_record_id_map_get(
		&store->record_id_map,
		record_id,
		&record_index
	);
	if (ret != KVS_VECTOR_OK) {
		return ret;
	}
	if (record_index >= store->capacity) {
		return KVS_VECTOR_ERR_NOT_FOUND;
	}

	record = &store->records[record_index];
	if (record->id != record_id) {
		return KVS_VECTOR_ERR_NOT_FOUND;
	}
	if (record->state != KVS_VECTOR_RECORD_ACTIVE || record->deleted) {
		return KVS_VECTOR_ERR_NOT_FOUND;
	}

	*out_record = record;
	return KVS_VECTOR_OK;
}

int kvs_vector_manager_get_active_record_id_by_index(
	size_t record_index,
	uint64_t *out_record_id
) {
	kvs_vector_record_t *record;

	if (!out_record_id) {
		return KVS_VECTOR_ERR_INVALID_ARG;
	}

	*out_record_id = 0;

	if (record_index >= g_vector_state.store.capacity) {
		return KVS_VECTOR_ERR_INVALID_ARG;
	}

	record = &g_vector_state.store.records[record_index];

	if (record->state != KVS_VECTOR_RECORD_ACTIVE || record->deleted) {
		return KVS_VECTOR_ERR_NOT_FOUND;
	}

	*out_record_id = record->id;
	return KVS_VECTOR_OK;
}

int kvs_vector_manager_for_each_active_embedding(
	kvs_vector_manager_active_embedding_iter_fn fn,
	void *arg
) {
	kvs_vector_store_t *store;
	size_t i;

	if (!fn) {
		return KVS_VECTOR_ERR_INVALID_ARG;
	}

	store = &g_vector_state.store;

	for (i = 0; i < store->capacity; i++) {
		kvs_vector_record_t *rec = &store->records[i];
		int ret;

		if (rec->state != KVS_VECTOR_RECORD_ACTIVE || rec->deleted) {
			continue;
		}

		if (!rec->embedding || rec->embedding_dim <= 0) {
			continue;
		}

		ret = fn(rec->id, rec->embedding, rec->embedding_dim, arg);
		if (ret != KVS_VECTOR_OK) {
			return ret;
		}
	}

	return KVS_VECTOR_OK;
}

//清空批量搜索结果
void kvs_vector_search_results_reset(
	kvs_vector_search_result_t *results,
	size_t count
) {
	size_t i;

	if (!results) {
		return;
	}

	for (i = 0; i < count; ++i) {
		kvs_vector_search_result_reset(&results[i]);
	}
}
