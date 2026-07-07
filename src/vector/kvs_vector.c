#define KVS_VECTOR_INTERNAL
#include "kvs_vector.h"
#include "kvs_vector_distance.h"
#include "vector/kvs_vector_backend_resolve.h"
#include "vector/kvs_vector_hnsw_v2_snapshot.h"
#include "vector/kvs_vector_index_snapshot.h"
#include "vector/kvs_vector_rebuild.h"

#include "kvs_memory.h"

#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <errno.h>
#include <sys/stat.h>
#include <time.h>
#include <unistd.h>

kvs_vector_state_t g_vector_state;

typedef struct kvs_vector_profile_stats_s {
	uint64_t vsearch_search_count;
	uint64_t vsearch_search_total_us;
	uint64_t vsearch_search_max_us;
	uint64_t vsearch_backend_search_count;
	uint64_t vsearch_backend_search_total_us;
	uint64_t vsearch_backend_search_max_us;
	uint64_t vsearch_manager_resolve_count;
	uint64_t vsearch_manager_resolve_total_us;
	uint64_t vsearch_manager_resolve_max_us;
	uint64_t vsearch_raw_hit_count;
	uint64_t vsearch_resolved_hit_count;
	uint64_t vsearch_filtered_hit_count;
	uint64_t vsearch_response_build_count;
	uint64_t vsearch_response_build_total_us;
	uint64_t vsearch_response_build_max_us;
} kvs_vector_profile_stats_t;

static kvs_vector_profile_stats_t g_vector_profile_stats;

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

void kvs_vector_vsearch_record_search(uint64_t elapsed_us) {
	kvs_vector_record_timing(
		&g_vector_profile_stats.vsearch_search_count,
		&g_vector_profile_stats.vsearch_search_total_us,
		&g_vector_profile_stats.vsearch_search_max_us,
		elapsed_us
	);
}

void kvs_vector_vsearch_record_backend_search(uint64_t elapsed_us) {
	kvs_vector_record_timing(
		&g_vector_profile_stats.vsearch_backend_search_count,
		&g_vector_profile_stats.vsearch_backend_search_total_us,
		&g_vector_profile_stats.vsearch_backend_search_max_us,
		elapsed_us
	);
}

void kvs_vector_vsearch_record_manager_resolve(
	uint64_t elapsed_us,
	size_t raw_hit_count,
	size_t resolved_hit_count
) {
	kvs_vector_record_timing(
		&g_vector_profile_stats.vsearch_manager_resolve_count,
		&g_vector_profile_stats.vsearch_manager_resolve_total_us,
		&g_vector_profile_stats.vsearch_manager_resolve_max_us,
		elapsed_us
	);
	kvs_vector_stats_add(&g_vector_profile_stats.vsearch_raw_hit_count, raw_hit_count);
	kvs_vector_stats_add(&g_vector_profile_stats.vsearch_resolved_hit_count, resolved_hit_count);
	if (raw_hit_count > resolved_hit_count) {
		kvs_vector_stats_add(
			&g_vector_profile_stats.vsearch_filtered_hit_count,
			raw_hit_count - resolved_hit_count
		);
	}
}

void kvs_vector_vsearch_record_response_build(uint64_t elapsed_us) {
	kvs_vector_record_timing(
		&g_vector_profile_stats.vsearch_response_build_count,
		&g_vector_profile_stats.vsearch_response_build_total_us,
		&g_vector_profile_stats.vsearch_response_build_max_us,
		elapsed_us
	);
}

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

int kvs_vector_index_bind_current(void) {
    kvs_vector_index_t *index = &g_vector_state.current_index;
    kvs_vector_index_type_t index_type = g_vector_state.index_type;

    memset(index, 0, sizeof(*index));

    switch (index_type) {
    case KVS_VECTOR_INDEX_CORE_FLAT:
    case KVS_VECTOR_INDEX_CORE_HNSW_V2:
        break;
    case KVS_VECTOR_INDEX_TYPE_FLAT:
    case KVS_VECTOR_INDEX_TYPE_HNSW:
    case KVS_VECTOR_INDEX_TYPE_SQ8_FLAT:
    case KVS_VECTOR_INDEX_CORE_HNSW:
    case KVS_VECTOR_INDEX_HNSWLIB_HNSW:
    case KVS_VECTOR_INDEX_FAISS_HNSW:
    case KVS_VECTOR_INDEX_FAISS_FLAT:
    default:
        index_type = KVS_VECTOR_INDEX_CORE_FLAT;
        g_vector_state.index_type = index_type;
        break;
    }

    index->type = index_type;
    index->metric = g_vector_state.metric_type;

    if (index_type == KVS_VECTOR_INDEX_CORE_FLAT) {
        index->dimension = g_vector_state.core_flat_index.dim;
        index->impl = &g_vector_state.core_flat_index;
        index->ops = &g_kvs_vector_core_flat_ops;
    } else if (index_type == KVS_VECTOR_INDEX_CORE_HNSW_V2) {
        index->dimension = g_vector_state.core_hnsw_v2_index.dim;
        index->impl = &g_vector_state.core_hnsw_v2_index;
        index->ops = &g_kvs_vector_core_hnsw_v2_ops;
    } else {
        g_vector_state.index_type = KVS_VECTOR_INDEX_CORE_FLAT;
        index->type = KVS_VECTOR_INDEX_CORE_FLAT;
        index->dimension = g_vector_state.core_flat_index.dim;
        index->impl = &g_vector_state.core_flat_index;
        index->ops = &g_kvs_vector_core_flat_ops;
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
 * core_flat 的物理条目数在 VDEL 后可能包含 stale entry。
 * 对外 VINFO 的 index_size 使用 active_records，避免用户看到逻辑数量
 * 和可见记录数量不一致。
 */
static void kvs_vector_info_from_state(
	kvs_vector_info_t *info,
	const kvs_vector_state_t *state
) {
	size_t active_records;
	size_t deleted_records;
	size_t dimension;

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
	 */
	info->index_size = active_records;
	dimension = state->current_index.dimension;
	if (dimension == 0) {
		if (state->index_type == KVS_VECTOR_INDEX_CORE_HNSW_V2) {
			dimension = state->core_hnsw_v2_index.dim;
		} else {
			dimension = state->core_flat_index.dim;
		}
	}
	info->dimension = dimension;

	info->index_type = (int)state->index_type;
	info->metric_type = (int)state->metric_type;

	if (state->index_type == KVS_VECTOR_INDEX_CORE_FLAT) {
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
	}

	info->flat_physical_size = state->core_flat_index.size;

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
	info->snapshot_supported =
		state->index_type == KVS_VECTOR_INDEX_CORE_HNSW_V2 ? 1 : 0;
	if (info->snapshot_supported) {
		const kvs_vector_snapshot_status_t *snapshot =
			&state->snapshot_status;

		info->snapshot_load_attempted = snapshot->snapshot_load_attempted;
		info->snapshot_loaded = snapshot->snapshot_loaded;
		info->snapshot_valid = snapshot->snapshot_valid;
		info->loaded_from_snapshot = snapshot->loaded_from_snapshot;
		info->snapshot_fallback_rebuild_count =
			snapshot->fallback_rebuild_count;
		info->snapshot_last_error = snapshot->last_snapshot_error;
		info->snapshot_write_attempted = snapshot->snapshot_write_attempted;
		info->snapshot_written = snapshot->snapshot_written;
		info->snapshot_last_write_error =
			snapshot->last_snapshot_write_error;
		info->snapshot_last_write_started_ms =
			snapshot->last_snapshot_write_started_ms;
		info->snapshot_last_write_finished_ms =
			snapshot->last_snapshot_write_finished_ms;
		info->snapshot_last_bytes = snapshot->last_snapshot_bytes;
		info->snapshot_last_checksum = snapshot->last_snapshot_checksum;
	} else {
		info->snapshot_last_error = "";
		info->snapshot_last_write_error = "";
	}
	info->vector_memory_bytes = kvs_vector_estimate_record_memory_bytes();
	info->index_memory_bytes = kvs_vector_estimate_current_index_memory_bytes();
	info->total_vector_memory_bytes = info->vector_memory_bytes + info->index_memory_bytes;

	info->vsearch_total_count =
		kvs_vector_stats_load(&g_vector_profile_stats.vsearch_search_count);
	info->vsearch_total_us =
		kvs_vector_stats_load(&g_vector_profile_stats.vsearch_search_total_us);
	info->vsearch_total_max_us =
		kvs_vector_stats_load(&g_vector_profile_stats.vsearch_search_max_us);
	info->vsearch_backend_search_count =
		kvs_vector_stats_load(&g_vector_profile_stats.vsearch_backend_search_count);
	info->vsearch_backend_search_total_us =
		kvs_vector_stats_load(&g_vector_profile_stats.vsearch_backend_search_total_us);
	info->vsearch_backend_search_max_us =
		kvs_vector_stats_load(&g_vector_profile_stats.vsearch_backend_search_max_us);
	info->vsearch_manager_resolve_count =
		kvs_vector_stats_load(&g_vector_profile_stats.vsearch_manager_resolve_count);
	info->vsearch_manager_resolve_total_us =
		kvs_vector_stats_load(&g_vector_profile_stats.vsearch_manager_resolve_total_us);
	info->vsearch_manager_resolve_max_us =
		kvs_vector_stats_load(&g_vector_profile_stats.vsearch_manager_resolve_max_us);
	info->vsearch_response_assembly_count =
		kvs_vector_stats_load(&g_vector_profile_stats.vsearch_response_build_count);
	info->vsearch_response_assembly_total_us =
		kvs_vector_stats_load(&g_vector_profile_stats.vsearch_response_build_total_us);
	info->vsearch_response_assembly_max_us =
		kvs_vector_stats_load(&g_vector_profile_stats.vsearch_response_build_max_us);
	info->vsearch_raw_hit_count =
		kvs_vector_stats_load(&g_vector_profile_stats.vsearch_raw_hit_count);
	info->vsearch_resolved_hit_count =
		kvs_vector_stats_load(&g_vector_profile_stats.vsearch_resolved_hit_count);
	info->vsearch_filtered_hit_count =
		kvs_vector_stats_load(&g_vector_profile_stats.vsearch_filtered_hit_count);

	info->faiss_backend_enabled = 0;
	info->faiss_ntotal = 0;
	info->faiss_add_count = 0;
	info->faiss_search_count = 0;

	info->hnswlib_backend_enabled = 0;
	info->hnswlib_ntotal = 0;
	info->hnswlib_capacity = 0;
	info->hnswlib_m = 0;
	info->hnswlib_ef_construction = 0;
	info->hnswlib_ef_search = 0;
	info->hnswlib_add_count = 0;
	info->hnswlib_search_count = 0;
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

static size_t kvs_vector_estimate_core_flat_memory_bytes(void) {
	kvs_vector_core_info_t info;

	memset(&info, 0, sizeof(info));
	if (kvs_flat_index_info(&g_vector_state.core_flat_index, &info) != KVS_VECTOR_OK) {
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
		case KVS_VECTOR_INDEX_CORE_FLAT:
			return kvs_vector_estimate_core_flat_memory_bytes();
		case KVS_VECTOR_INDEX_CORE_HNSW_V2:
			return kvs_vector_estimate_core_hnsw_v2_memory_bytes();
		default:
			return 0;
	}
}

static void kvs_vector_fill_backend_stats(kvs_vector_info_t *info) {
	if (!info) {
		return;
	}

	info->backend_name = kvs_vector_backend_type_name((kvs_vector_index_type_t)info->index_type);
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
	case KVS_VECTOR_INDEX_CORE_HNSW_V2:
		info->backend_size = info->core_hnsw_v2_active_count;
		info->backend_capacity = info->core_hnsw_v2_capacity;
		info->backend_deleted_count = info->core_hnsw_v2_deleted_count;
		info->backend_memory_bytes = kvs_vector_estimate_core_hnsw_v2_memory_bytes();
		info->backend_memory_bytes_source = "measured";
		break;
	default:
		info->backend_memory_bytes = 0;
		info->backend_memory_bytes_source = "unsupported";
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

	g_vector_state.index_type =
		kvs_vector_backend_resolve_from_config(g_cfg.vector_index_type);
	g_vector_state.metric_type = kvs_vector_metric_from_config();

	/*
	 * core_flat remains available as an exact baseline / fallback. The
	 * semantic-cache ANN default is core_hnsw_v2 after Stage 9G2.
	 */
	ret = kvs_flat_index_init(
		&g_vector_state.core_flat_index,
		dimension,
		g_vector_state.metric_type
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

	ret = kvs_vector_index_bind_current();
	if (ret != KVS_VECTOR_OK) {
		goto fail;
	}

	g_vector_state.initialized = 1;

	KVS_LOG_INFO(
		KVS_VECTOR_LOG_MODULE,
		"vector module initialized provider=%s distance_backend=%s dimension=%zu capacity=%zu index_type=%s metric=%s hnsw_m=%zu ef_construction=%zu ef_search=%zu hnsw_initial_capacity=%d",
			kvs_vector_embedding_provider_name(),
			kvs_vector_distance_backend_name(),
			dimension,
			g_vector_state.store.capacity,
			kvs_vector_backend_type_name(g_vector_state.index_type),
			kvs_vector_metric_name(g_vector_state.metric_type),
			g_vector_state.core_hnsw_v2_index.M,
			g_vector_state.core_hnsw_v2_index.ef_construction,
			g_vector_state.core_hnsw_v2_index.ef_search,
			g_cfg.vector_hnsw_initial_capacity
		);

	return KVS_VECTOR_OK;

fail:
	KVS_LOG_ERROR(
		KVS_VECTOR_LOG_MODULE,
		"vector module init failed ret=%d",
		ret
	);
	kvs_vector_embedding_provider_destroy(&g_vector_state.embedding_provider);
	kvs_hnsw_v2_index_destroy(&g_vector_state.core_hnsw_v2_index);
	kvs_flat_index_destroy(&g_vector_state.core_flat_index);
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
	kvs_hnsw_v2_index_destroy(&g_vector_state.core_hnsw_v2_index);
	kvs_flat_index_destroy(&g_vector_state.core_flat_index);
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

	kvs_hnsw_v2_index_destroy(&g_vector_state.core_hnsw_v2_index);
	kvs_flat_index_destroy(&g_vector_state.core_flat_index);
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

	ret = kvs_flat_index_init(
		&g_vector_state.core_flat_index,
		dimension,
		g_vector_state.metric_type
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

static void kvs_vector_snapshot_status_reset(
	kvs_vector_snapshot_status_t *status
) {
	if (!status) {
		return;
	}
	memset(status, 0, sizeof(*status));
}

static void kvs_vector_snapshot_status_set_text(
	char *dst,
	size_t dst_size,
	const char *src
) {
	if (!dst || dst_size == 0) {
		return;
	}
	snprintf(dst, dst_size, "%s", src ? src : "");
}

static void kvs_vector_snapshot_set_reason(
	char *reason,
	size_t reason_size,
	const char *message
) {
	if (!reason || reason_size == 0) {
		return;
	}
	snprintf(reason, reason_size, "%s", message ? message : "");
}

static uint64_t kvs_vector_snapshot_fnv_update(
	uint64_t hash,
	const void *data,
	size_t len
) {
	const unsigned char *bytes = (const unsigned char *)data;
	size_t i;

	if (!bytes && len > 0) {
		return hash;
	}
	for (i = 0; i < len; i++) {
		hash ^= (uint64_t)bytes[i];
		hash *= 1099511628211ull;
	}
	return hash;
}

static uint64_t kvs_vector_snapshot_fnv_update_u64(
	uint64_t hash,
	uint64_t value
) {
	unsigned char bytes[8];
	size_t i;

	for (i = 0; i < sizeof(bytes); i++) {
		bytes[i] = (unsigned char)((value >> (i * 8)) & 0xffu);
	}
	return kvs_vector_snapshot_fnv_update(hash, bytes, sizeof(bytes));
}

static uint64_t kvs_vector_snapshot_fnv_update_u32(
	uint64_t hash,
	uint32_t value
) {
	unsigned char bytes[4];
	size_t i;

	for (i = 0; i < sizeof(bytes); i++) {
		bytes[i] = (unsigned char)((value >> (i * 8)) & 0xffu);
	}
	return kvs_vector_snapshot_fnv_update(hash, bytes, sizeof(bytes));
}

static int kvs_vector_snapshot_file_size_u64(
	const char *path,
	uint64_t *out_size
) {
	struct stat st;

	if (!path || !out_size) {
		return KVS_VECTOR_ERR_INVALID_ARG;
	}
	*out_size = 0;
	if (stat(path, &st) != 0 || st.st_size < 0) {
		return KVS_VECTOR_ERR_NOT_FOUND;
	}
	*out_size = (uint64_t)st.st_size;
	return KVS_VECTOR_OK;
}

static int kvs_vector_snapshot_path_with_suffix(
	const char *path,
	const char *suffix,
	char *buf,
	size_t size
) {
	int written;

	if (!path || !suffix || !buf || size == 0) {
		return KVS_VECTOR_ERR_INVALID_ARG;
	}
	written = snprintf(buf, size, "%s%s", path, suffix);
	if (written < 0 || (size_t)written >= size) {
		buf[0] = '\0';
		return KVS_VECTOR_ERR;
	}
	return KVS_VECTOR_OK;
}

static int kvs_vector_snapshot_unlink_if_exists(const char *path) {
	if (!path || path[0] == '\0') {
		return KVS_VECTOR_ERR_INVALID_ARG;
	}
	if (unlink(path) == 0 || errno == ENOENT) {
		return KVS_VECTOR_OK;
	}
	return KVS_VECTOR_ERR;
}

static int kvs_vector_snapshot_publish_files(
	const char *snapshot_tmp_path,
	const char *snapshot_path,
	const char *manifest_tmp_path,
	const char *manifest_path,
	char *reason,
	size_t reason_size
) {
	char snapshot_bak_path[512];
	char manifest_bak_path[512];
	int had_snapshot = 0;
	int had_manifest = 0;
	int snapshot_backed_up = 0;
	int manifest_backed_up = 0;
	int snapshot_published = 0;

	if (!snapshot_tmp_path || !snapshot_path ||
	    !manifest_tmp_path || !manifest_path) {
		kvs_vector_snapshot_set_reason(reason, reason_size, "invalid publish path");
		return KVS_VECTOR_ERR_INVALID_ARG;
	}
	if (kvs_vector_snapshot_path_with_suffix(
		    snapshot_path,
		    ".bak",
		    snapshot_bak_path,
		    sizeof(snapshot_bak_path)
	    ) != KVS_VECTOR_OK ||
	    kvs_vector_snapshot_path_with_suffix(
		    manifest_path,
		    ".bak",
		    manifest_bak_path,
		    sizeof(manifest_bak_path)
	    ) != KVS_VECTOR_OK) {
		kvs_vector_snapshot_set_reason(reason, reason_size, "backup path failed");
		return KVS_VECTOR_ERR;
	}

	if (kvs_vector_snapshot_unlink_if_exists(snapshot_bak_path) != KVS_VECTOR_OK ||
	    kvs_vector_snapshot_unlink_if_exists(manifest_bak_path) != KVS_VECTOR_OK) {
		kvs_vector_snapshot_set_reason(reason, reason_size, "backup cleanup failed");
		goto fail;
	}

	had_snapshot = access(snapshot_path, F_OK) == 0;
	had_manifest = access(manifest_path, F_OK) == 0;

	if (had_snapshot && rename(snapshot_path, snapshot_bak_path) != 0) {
		kvs_vector_snapshot_set_reason(reason, reason_size, "backup snapshot failed");
		goto fail;
	}
	snapshot_backed_up = had_snapshot;

	if (had_manifest && rename(manifest_path, manifest_bak_path) != 0) {
		kvs_vector_snapshot_set_reason(reason, reason_size, "backup manifest failed");
		goto fail;
	}
	manifest_backed_up = had_manifest;

	if (rename(snapshot_tmp_path, snapshot_path) != 0) {
		kvs_vector_snapshot_set_reason(reason, reason_size, "publish snapshot failed");
		goto fail;
	}
	snapshot_published = 1;

	if (rename(manifest_tmp_path, manifest_path) != 0) {
		kvs_vector_snapshot_set_reason(reason, reason_size, "publish manifest failed");
		goto fail;
	}

	(void)kvs_vector_snapshot_unlink_if_exists(snapshot_bak_path);
	(void)kvs_vector_snapshot_unlink_if_exists(manifest_bak_path);
	kvs_vector_snapshot_set_reason(reason, reason_size, "ok");
	return KVS_VECTOR_OK;

fail:
	if (snapshot_published) {
		(void)unlink(snapshot_path);
	}
	if (manifest_backed_up) {
		(void)rename(manifest_bak_path, manifest_path);
	}
	if (snapshot_backed_up) {
		(void)rename(snapshot_bak_path, snapshot_path);
	}
	(void)unlink(snapshot_tmp_path);
	(void)unlink(manifest_tmp_path);
	return KVS_VECTOR_ERR;
}

static int kvs_vector_index_snapshot_build_expected_manifest_for_hnsw(
	kvs_vector_state_t *state,
	const kvs_hnsw_v2_index_t *hnsw,
	const char *snapshot_path,
	kvs_vector_index_snapshot_manifest_t *manifest,
	char *reason,
	size_t reason_size
) {
	kvs_vector_store_t *store;
	uint64_t active_count = 0;
	uint64_t deleted_count = 0;
	uint64_t max_generation = 0;
	uint64_t record_id_checksum = 1469598103934665603ull;
	uint64_t vector_checksum = 1469598103934665603ull;
	uint64_t snapshot_size = 0;
	uint64_t snapshot_checksum = 0;
	size_t i;
	int ret;

	if (!state || !hnsw || !manifest ||
	    !snapshot_path || snapshot_path[0] == '\0') {
		kvs_vector_snapshot_set_reason(
			reason,
			reason_size,
			"invalid expected manifest argument"
		);
		return KVS_VECTOR_ERR_INVALID_ARG;
	}

	store = &state->store;
	ret = kvs_vector_index_snapshot_manifest_init(manifest);
	if (ret != 0) {
		kvs_vector_snapshot_set_reason(reason, reason_size, "manifest init failed");
		return KVS_VECTOR_ERR;
	}

	for (i = 0; i < store->capacity; i++) {
		kvs_vector_record_t *record = &store->records[i];

		if (record->state == KVS_VECTOR_RECORD_ACTIVE && !record->deleted) {
			size_t embedding_bytes;

			active_count++;
			if (record->generation > max_generation) {
				max_generation = record->generation;
			}
			record_id_checksum = kvs_vector_snapshot_fnv_update_u64(
				record_id_checksum,
				record->id
			);
			vector_checksum = kvs_vector_snapshot_fnv_update_u64(
				vector_checksum,
				record->id
			);
			vector_checksum = kvs_vector_snapshot_fnv_update_u32(
				vector_checksum,
				(uint32_t)record->embedding_dim
			);
			if (record->embedding && record->embedding_dim > 0) {
				embedding_bytes = (size_t)record->embedding_dim * sizeof(float);
				vector_checksum = kvs_vector_snapshot_fnv_update(
					vector_checksum,
					record->embedding,
					embedding_bytes
				);
			}
			continue;
		}

		if (record->state == KVS_VECTOR_RECORD_DELETED || record->deleted) {
			deleted_count++;
			if (record->generation > max_generation) {
				max_generation = record->generation;
			}
		}
	}

	if (active_count != (uint64_t)store->active_records ||
	    deleted_count != (uint64_t)store->deleted_records) {
		kvs_vector_snapshot_set_reason(
			reason,
			reason_size,
			"record count mismatch"
		);
		return KVS_VECTOR_ERR;
	}

	ret = kvs_vector_snapshot_file_size_u64(snapshot_path, &snapshot_size);
	if (ret != KVS_VECTOR_OK) {
		kvs_vector_snapshot_set_reason(reason, reason_size, "snapshot missing");
		return ret;
	}
	snapshot_checksum = kvs_vector_index_snapshot_checksum_file(snapshot_path);
	if (snapshot_checksum == 0) {
		kvs_vector_snapshot_set_reason(
			reason,
			reason_size,
			"snapshot checksum failed"
		);
		return KVS_VECTOR_ERR;
	}

	manifest->backend = (uint32_t)KVS_VECTOR_INDEX_CORE_HNSW_V2;
	manifest->dim = (uint32_t)hnsw->dim;
	manifest->metric = (uint32_t)state->metric_type;
	manifest->hnsw_m = (uint32_t)hnsw->M;
	manifest->hnsw_m0 = (uint32_t)hnsw->M0;
	manifest->hnsw_ef_construction = (uint32_t)hnsw->ef_construction;
	manifest->hnsw_ef_search = (uint32_t)hnsw->ef_search;
	manifest->active_count = active_count;
	manifest->deleted_count = deleted_count;
	manifest->record_generation = max_generation;
	manifest->record_id_checksum = record_id_checksum;
	manifest->vector_checksum = vector_checksum;
	manifest->snapshot_size = snapshot_size;
	manifest->snapshot_checksum = snapshot_checksum;
	kvs_vector_snapshot_set_reason(reason, reason_size, "ok");
	return KVS_VECTOR_OK;
}

int kvs_vector_index_snapshot_build_expected_manifest_after_records(
	kvs_vector_state_t *state,
	const char *snapshot_path,
	kvs_vector_index_snapshot_manifest_t *manifest,
	char *reason,
	size_t reason_size
) {
	if (!state) {
		kvs_vector_snapshot_set_reason(reason, reason_size, "invalid state");
		return KVS_VECTOR_ERR_INVALID_ARG;
	}

	return kvs_vector_index_snapshot_build_expected_manifest_for_hnsw(
		state,
		&state->core_hnsw_v2_index,
		snapshot_path,
		manifest,
		reason,
		reason_size
	);
}

static int kvs_vector_index_snapshot_validate_loaded_records(
	kvs_vector_state_t *state,
	char *reason,
	size_t reason_size
) {
	kvs_hnsw_v2_index_t *hnsw;
	uint64_t active_slots = 0;
	size_t i;

	if (!state) {
		kvs_vector_snapshot_set_reason(reason, reason_size, "invalid state");
		return KVS_VECTOR_ERR_INVALID_ARG;
	}
	hnsw = &state->core_hnsw_v2_index;
	if (!hnsw->ready || hnsw->dim == 0) {
		kvs_vector_snapshot_set_reason(reason, reason_size, "loaded index not ready");
		return KVS_VECTOR_ERR;
	}

	for (i = 0; i < hnsw->size; i++) {
		kvs_vector_record_t *record = NULL;
		const float *vector;
		uint64_t record_id;
		size_t vector_bytes;

		if (hnsw->deleted_flags && hnsw->deleted_flags[i]) {
			continue;
		}
		active_slots++;
		record_id = hnsw->slot_to_record_id[i];
		if (kvs_vector_manager_get_record_by_id(record_id, &record) !=
		    KVS_VECTOR_OK ||
		    !record || !record->embedding ||
		    record->embedding_dim != (int)hnsw->dim) {
			kvs_vector_snapshot_set_reason(
				reason,
				reason_size,
				"loaded record_id not in manager records"
			);
			return KVS_VECTOR_ERR;
		}
		vector = kvs_hnsw_v2_slot_vector(hnsw, (uint32_t)i);
		vector_bytes = hnsw->dim * sizeof(float);
		if (!vector || memcmp(vector, record->embedding, vector_bytes) != 0) {
			kvs_vector_snapshot_set_reason(
				reason,
				reason_size,
				"loaded vector mismatch"
			);
			return KVS_VECTOR_ERR;
		}
	}

	if (active_slots != (uint64_t)state->store.active_records) {
		kvs_vector_snapshot_set_reason(
			reason,
			reason_size,
			"loaded active count mismatch"
		);
		return KVS_VECTOR_ERR;
	}

	kvs_vector_snapshot_set_reason(reason, reason_size, "ok");
	return KVS_VECTOR_OK;
}

int kvs_vector_index_snapshot_status_copy(kvs_vector_snapshot_status_t *out) {
	if (!out) {
		return KVS_VECTOR_ERR_INVALID_ARG;
	}
	*out = g_vector_state.snapshot_status;
	return KVS_VECTOR_OK;
}

static void kvs_vector_snapshot_write_status_reset(
	kvs_vector_snapshot_status_t *status
) {
	if (!status) {
		return;
	}
	status->snapshot_write_attempted = 0;
	status->snapshot_written = 0;
	status->last_snapshot_write_error[0] = '\0';
	status->last_snapshot_write_started_ms = 0;
	status->last_snapshot_write_finished_ms = 0;
	status->last_snapshot_bytes = 0;
	status->last_snapshot_checksum = 0;
}

static void kvs_vector_snapshot_write_status_finish(
	kvs_vector_snapshot_status_t *status,
	int written,
	uint64_t bytes,
	uint64_t checksum,
	const char *message
) {
	if (!status) {
		return;
	}
	status->snapshot_written = written ? 1 : 0;
	status->last_snapshot_write_finished_ms = (uint64_t)kv_now_ms();
	status->last_snapshot_bytes = written ? bytes : 0;
	status->last_snapshot_checksum = written ? checksum : 0;
	kvs_vector_snapshot_status_set_text(
		status->last_snapshot_write_error,
		sizeof(status->last_snapshot_write_error),
		message
	);
}

static int kvs_vector_index_snapshot_write_core_hnsw_v2(
	kvs_vector_state_t *state,
	const kvs_hnsw_v2_index_t *hnsw,
	const char *manifest_path,
	const char *snapshot_path,
	char *reason,
	size_t reason_size
) {
	kvs_vector_index_snapshot_manifest_t manifest;
	kvs_vector_snapshot_status_t *status;
	char manifest_tmp_path[512];
	char snapshot_tmp_path[512];
	uint64_t snapshot_bytes = 0;
	uint64_t snapshot_checksum = 0;
	int ret;

	if (!state || !hnsw || !manifest_path || !snapshot_path ||
	    manifest_path[0] == '\0' || snapshot_path[0] == '\0') {
		kvs_vector_snapshot_set_reason(reason, reason_size, "invalid write argument");
		return KVS_VECTOR_ERR_INVALID_ARG;
	}

	status = &state->snapshot_status;
	kvs_vector_snapshot_write_status_reset(status);
	kvs_vector_snapshot_set_reason(reason, reason_size, "snapshot write not attempted");

	if (state->index_type != KVS_VECTOR_INDEX_CORE_HNSW_V2) {
		kvs_vector_snapshot_status_set_text(
			status->last_snapshot_write_error,
			sizeof(status->last_snapshot_write_error),
			"non-core_hnsw_v2 backend"
		);
		kvs_vector_snapshot_set_reason(reason, reason_size, "non-core_hnsw_v2 backend");
		return KVS_VECTOR_ERR_NOT_FOUND;
	}

	if (kvs_vector_snapshot_path_with_suffix(
		    snapshot_path,
		    ".tmp",
		    snapshot_tmp_path,
		    sizeof(snapshot_tmp_path)
	    ) != KVS_VECTOR_OK ||
	    kvs_vector_snapshot_path_with_suffix(
		    manifest_path,
		    ".tmp",
		    manifest_tmp_path,
		    sizeof(manifest_tmp_path)
	    ) != KVS_VECTOR_OK) {
		kvs_vector_snapshot_set_reason(reason, reason_size, "tmp path failed");
		kvs_vector_snapshot_status_set_text(
			status->last_snapshot_write_error,
			sizeof(status->last_snapshot_write_error),
			"tmp path failed"
		);
		return KVS_VECTOR_ERR;
	}

	status->snapshot_write_attempted = 1;
	status->last_snapshot_write_started_ms = (uint64_t)kv_now_ms();
	kvs_vector_snapshot_status_set_text(
		status->last_snapshot_manifest_path,
		sizeof(status->last_snapshot_manifest_path),
		manifest_path
	);
	kvs_vector_snapshot_status_set_text(
		status->last_snapshot_path,
		sizeof(status->last_snapshot_path),
		snapshot_path
	);

	ret = kvs_hnsw_v2_index_validate_basic(
		hnsw,
		reason,
		reason_size
	);
	if (ret != KVS_VECTOR_OK) {
		kvs_vector_snapshot_write_status_finish(status, 0, 0, 0, reason);
		return ret;
	}

	if (kvs_vector_snapshot_unlink_if_exists(snapshot_tmp_path) != KVS_VECTOR_OK ||
	    kvs_vector_snapshot_unlink_if_exists(manifest_tmp_path) != KVS_VECTOR_OK) {
		kvs_vector_snapshot_set_reason(reason, reason_size, "tmp cleanup failed");
		kvs_vector_snapshot_write_status_finish(status, 0, 0, 0, reason);
		return KVS_VECTOR_ERR;
	}

	ret = kvs_hnsw_v2_index_serialize_file(
		hnsw,
		snapshot_tmp_path
	);
	if (ret != KVS_VECTOR_OK) {
		kvs_vector_snapshot_set_reason(reason, reason_size, "snapshot serialize failed");
		kvs_vector_snapshot_write_status_finish(status, 0, 0, 0, reason);
		(void)unlink(snapshot_tmp_path);
		return ret;
	}

	ret = kvs_vector_index_snapshot_build_expected_manifest_for_hnsw(
		state,
		hnsw,
		snapshot_tmp_path,
		&manifest,
		reason,
		reason_size
	);
	if (ret != KVS_VECTOR_OK) {
		kvs_vector_snapshot_write_status_finish(status, 0, 0, 0, reason);
		(void)unlink(snapshot_tmp_path);
		return ret;
	}
	manifest.created_at_ms = (uint64_t)kv_now_ms();
	snapshot_bytes = manifest.snapshot_size;
	snapshot_checksum = manifest.snapshot_checksum;

	if (kvs_vector_index_snapshot_manifest_write_file(
		    manifest_tmp_path,
		    &manifest
	    ) != 0) {
		kvs_vector_snapshot_set_reason(reason, reason_size, "manifest write failed");
		kvs_vector_snapshot_write_status_finish(status, 0, 0, 0, reason);
		(void)unlink(snapshot_tmp_path);
		(void)unlink(manifest_tmp_path);
		return KVS_VECTOR_ERR;
	}

	ret = kvs_vector_snapshot_publish_files(
		snapshot_tmp_path,
		snapshot_path,
		manifest_tmp_path,
		manifest_path,
		reason,
		reason_size
	);
	if (ret != KVS_VECTOR_OK) {
		kvs_vector_snapshot_write_status_finish(status, 0, 0, 0, reason);
		return ret;
	}

	kvs_vector_snapshot_write_status_finish(
		status,
		1,
		snapshot_bytes,
		snapshot_checksum,
		"ok"
	);
	kvs_vector_snapshot_set_reason(reason, reason_size, "ok");
	return KVS_VECTOR_OK;
}

int kvs_vector_index_snapshot_write_hnsw_v2_index(
	kvs_vector_state_t *state,
	const kvs_hnsw_v2_index_t *hnsw,
	char *reason,
	size_t reason_size
) {
	char manifest_path[512];
	char snapshot_path[512];

	if (!state || !hnsw) {
		kvs_vector_snapshot_set_reason(reason, reason_size, "invalid state");
		return KVS_VECTOR_ERR_INVALID_ARG;
	}
	if (kvs_vector_index_snapshot_default_manifest_path(
		    manifest_path,
		    sizeof(manifest_path)
	    ) != 0 ||
	    kvs_vector_index_snapshot_default_snapshot_path(
		    snapshot_path,
		    sizeof(snapshot_path)
	    ) != 0) {
		kvs_vector_snapshot_set_reason(reason, reason_size, "snapshot path failed");
		return KVS_VECTOR_ERR;
	}
	return kvs_vector_index_snapshot_write_core_hnsw_v2(
		state,
		hnsw,
		manifest_path,
		snapshot_path,
		reason,
		reason_size
	);
}

int kvs_vector_index_snapshot_write_current(
	kvs_vector_state_t *state,
	char *reason,
	size_t reason_size
) {
	char manifest_path[512];
	char snapshot_path[512];

	if (!state) {
		kvs_vector_snapshot_set_reason(reason, reason_size, "invalid state");
		return KVS_VECTOR_ERR_INVALID_ARG;
	}
	if (kvs_vector_index_snapshot_default_manifest_path(
		    manifest_path,
		    sizeof(manifest_path)
	    ) != 0 ||
	    kvs_vector_index_snapshot_default_snapshot_path(
		    snapshot_path,
		    sizeof(snapshot_path)
	    ) != 0) {
		kvs_vector_snapshot_set_reason(reason, reason_size, "snapshot path failed");
		return KVS_VECTOR_ERR;
	}
	return kvs_vector_index_snapshot_write_core_hnsw_v2(
		state,
		&state->core_hnsw_v2_index,
		manifest_path,
		snapshot_path,
		reason,
		reason_size
	);
}

static void kvs_vector_index_snapshot_write_best_effort_after_rebuild(
	const char *source
) {
	char reason[128];
	int ret;

	if (g_vector_state.index_type != KVS_VECTOR_INDEX_CORE_HNSW_V2) {
		return;
	}

	reason[0] = '\0';
	ret = kvs_vector_index_snapshot_write_current(
		&g_vector_state,
		reason,
		sizeof(reason)
	);
	if (ret == KVS_VECTOR_OK) {
		KVS_LOG_INFO(
			KVS_VECTOR_LOG_MODULE,
			"vector index snapshot written source=%s path=%s bytes=%llu checksum=%llu",
			source ? source : "rebuild",
			g_vector_state.snapshot_status.last_snapshot_path,
			(unsigned long long)g_vector_state.snapshot_status.last_snapshot_bytes,
			(unsigned long long)g_vector_state.snapshot_status.last_snapshot_checksum
		);
		return;
	}

	KVS_LOG_WARN(
		KVS_VECTOR_LOG_MODULE,
		"vector index snapshot write skipped source=%s ret=%d reason=%s",
		source ? source : "rebuild",
		ret,
		reason[0] ? reason : "unknown"
	);
}

int kvs_vector_index_snapshot_try_load_after_records(
	kvs_vector_state_t *state,
	char *reason,
	size_t reason_size
) {
	kvs_vector_index_snapshot_manifest_t actual;
	kvs_vector_index_snapshot_manifest_t expected;
	kvs_vector_snapshot_status_t *status;
	char manifest_path[512];
	char snapshot_path[512];
	int ret;

	if (!state) {
		kvs_vector_snapshot_set_reason(reason, reason_size, "invalid state");
		return KVS_VECTOR_ERR_INVALID_ARG;
	}

	status = &state->snapshot_status;
	kvs_vector_snapshot_status_reset(status);
	kvs_vector_snapshot_set_reason(reason, reason_size, "snapshot not attempted");

	if (state->index_type != KVS_VECTOR_INDEX_CORE_HNSW_V2) {
		kvs_vector_snapshot_status_set_text(
			status->last_snapshot_error,
			sizeof(status->last_snapshot_error),
			"non-core_hnsw_v2 backend"
		);
		kvs_vector_snapshot_set_reason(reason, reason_size, "non-core_hnsw_v2 backend");
		return KVS_VECTOR_ERR_NOT_FOUND;
	}

	if (kvs_vector_index_snapshot_default_manifest_path(
		    manifest_path,
		    sizeof(manifest_path)
	    ) != 0 ||
	    kvs_vector_index_snapshot_default_snapshot_path(
		    snapshot_path,
		    sizeof(snapshot_path)
	    ) != 0) {
		kvs_vector_snapshot_status_set_text(
			status->last_snapshot_error,
			sizeof(status->last_snapshot_error),
			"snapshot path resolution failed"
		);
		kvs_vector_snapshot_set_reason(reason, reason_size, "snapshot path failed");
		return KVS_VECTOR_ERR;
	}

	status->snapshot_load_attempted = 1;
	kvs_vector_snapshot_status_set_text(
		status->last_snapshot_manifest_path,
		sizeof(status->last_snapshot_manifest_path),
		manifest_path
	);
	kvs_vector_snapshot_status_set_text(
		status->last_snapshot_path,
		sizeof(status->last_snapshot_path),
		snapshot_path
	);

	ret = kvs_vector_index_snapshot_manifest_read_file(manifest_path, &actual);
	if (ret != 0) {
		kvs_vector_snapshot_status_set_text(
			status->last_snapshot_error,
			sizeof(status->last_snapshot_error),
			"manifest missing or unreadable"
		);
		kvs_vector_snapshot_set_reason(reason, reason_size, "manifest missing");
		return KVS_VECTOR_ERR_NOT_FOUND;
	}

	ret = kvs_vector_index_snapshot_build_expected_manifest_after_records(
		state,
		snapshot_path,
		&expected,
		reason,
		reason_size
	);
	if (ret != KVS_VECTOR_OK) {
		kvs_vector_snapshot_status_set_text(
			status->last_snapshot_error,
			sizeof(status->last_snapshot_error),
			reason
		);
		return ret;
	}

	ret = kvs_vector_index_snapshot_manifest_validate(
		&actual,
		&expected,
		reason,
		reason_size
	);
	if (ret != 0) {
		kvs_vector_snapshot_status_set_text(
			status->last_snapshot_error,
			sizeof(status->last_snapshot_error),
			reason
		);
		return KVS_VECTOR_ERR;
	}
	status->snapshot_valid = 1;

	ret = kvs_hnsw_v2_index_deserialize_file(
		&state->core_hnsw_v2_index,
		snapshot_path
	);
	if (ret != KVS_VECTOR_OK) {
		kvs_vector_snapshot_status_set_text(
			status->last_snapshot_error,
			sizeof(status->last_snapshot_error),
			"snapshot deserialize failed"
		);
		kvs_vector_snapshot_set_reason(reason, reason_size, "snapshot deserialize failed");
		return ret;
	}

	ret = kvs_hnsw_v2_index_validate_basic(
		&state->core_hnsw_v2_index,
		reason,
		reason_size
	);
	if (ret != KVS_VECTOR_OK) {
		kvs_vector_snapshot_status_set_text(
			status->last_snapshot_error,
			sizeof(status->last_snapshot_error),
			reason
		);
		return ret;
	}

	ret = kvs_vector_index_snapshot_validate_loaded_records(
		state,
		reason,
		reason_size
	);
	if (ret != KVS_VECTOR_OK) {
		kvs_vector_snapshot_status_set_text(
			status->last_snapshot_error,
			sizeof(status->last_snapshot_error),
			reason
		);
		return ret;
	}

	ret = kvs_vector_index_bind_current();
	if (ret != KVS_VECTOR_OK) {
		kvs_vector_snapshot_status_set_text(
			status->last_snapshot_error,
			sizeof(status->last_snapshot_error),
			"bind loaded snapshot failed"
		);
		kvs_vector_snapshot_set_reason(reason, reason_size, "bind loaded snapshot failed");
		return ret;
	}

	status->snapshot_loaded = 1;
	status->loaded_from_snapshot = 1;
	kvs_vector_snapshot_status_set_text(
		status->last_snapshot_error,
		sizeof(status->last_snapshot_error),
		"ok"
	);
	kvs_vector_snapshot_set_reason(reason, reason_size, "ok");
	return KVS_VECTOR_OK;
}

int kvs_vector_rebuild_after_load(void) {
	int ret;
	char snapshot_reason[128];

	if (!g_vector_state.initialized) {
		return KVS_VECTOR_ERR_NOT_INITIALIZED;
	}

	kvs_vector_snapshot_status_reset(&g_vector_state.snapshot_status);

	ret = kvs_vector_id_map_rebuild_from_store();
	if (ret != KVS_VECTOR_OK) return ret;

	ret = kvs_vector_index_bind_current();
	if (ret != KVS_VECTOR_OK) return ret;

	if (g_vector_state.index_type == KVS_VECTOR_INDEX_CORE_HNSW_V2) {
		snapshot_reason[0] = '\0';
		ret = kvs_vector_index_snapshot_try_load_after_records(
			&g_vector_state,
			snapshot_reason,
			sizeof(snapshot_reason)
		);
		if (ret == KVS_VECTOR_OK) {
			KVS_LOG_INFO(
				KVS_VECTOR_LOG_MODULE,
				"vector index snapshot loaded path=%s",
				g_vector_state.snapshot_status.last_snapshot_path
			);
			return KVS_VECTOR_OK;
		}
		g_vector_state.snapshot_status.fallback_rebuild_count++;
		KVS_LOG_INFO(
			KVS_VECTOR_LOG_MODULE,
			"vector index snapshot unavailable, fallback rebuild reason=%s",
			snapshot_reason[0] ? snapshot_reason : "unknown"
		);
	}

	ret = kvs_vector_index_rebuild_current();
	if (ret == KVS_VECTOR_OK) {
		kvs_vector_index_snapshot_write_best_effort_after_rebuild(
			"rebuild_after_load_fallback"
		);
	}
	return ret;
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

	return KVS_VECTOR_OK;
}

int kvs_vector_set_commit_prepared(kvs_vector_set_prepared_t *prepared) {
	kvs_vector_record_t *record;
	kvs_vector_record_t *free_slot;
	kvs_vector_store_t *store;
	size_t record_index;
	int was_deleted = 0;
	int ret;

	ret = kvs_vector_validate_initialized();
	if (ret != KVS_VECTOR_OK) {
		kvs_vector_set_prepared_destroy(prepared);
		return ret;
	}

	if (!prepared || !prepared->question || prepared->question_len == 0 ||
	    !prepared->answer || !prepared->embedding || prepared->embedding_dim <= 0) {
		kvs_vector_set_prepared_destroy(prepared);
		return KVS_VECTOR_ERR_INVALID_ARG;
	}

	/*
	 * Stage-1 commit rechecks question+scope under the write lock. If another
	 * VSET inserted the same key while embedding was prepared, keep existing
	 * semantics: update answer only and discard the unused embedding.
	 */
	record = kvs_vector_find_record(
		prepared->question,
		prepared->question_len,
		prepared->scope,
		prepared->scope_len
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
			if (ret != KVS_VECTOR_OK) {
				kvs_vector_set_prepared_destroy(prepared);
			}
			return ret;
		}

		ret = kvs_vector_record_update_answer(
			record,
			prepared->answer,
			prepared->answer_len
		);
		kvs_vector_set_prepared_destroy(prepared);
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

	free_slot = kvs_vector_find_free_slot();
	if (!free_slot) {
		ret = kvs_vector_store_ensure_capacity(store, store->capacity + 1);
		if (ret != KVS_VECTOR_OK) {
			kvs_vector_set_prepared_destroy(prepared);
			return ret;
		}

		free_slot = kvs_vector_find_free_slot();
		if (!free_slot) {
			kvs_vector_set_prepared_destroy(prepared);
			return KVS_VECTOR_ERR;
		}
	}

	was_deleted = (free_slot->state == KVS_VECTOR_RECORD_DELETED) ? 1 : 0;
	record_index = (size_t)(free_slot - store->records);

	ret = kvs_vector_record_take_prepared_fields(free_slot, prepared);
	if (ret != KVS_VECTOR_OK) {
		if (was_deleted && store->deleted_records > 0) {
			store->deleted_records--;
		}
		kvs_vector_set_prepared_destroy(prepared);
		return ret;
	}

	ret = kvs_vector_id_map_put(
		&g_vector_state.id_map,
		free_slot->question,
		free_slot->question_len,
		free_slot->scope,
		free_slot->scope_len,
		record_index
	);
	if (ret != KVS_VECTOR_OK) {
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

	ret = kvs_vector_index_add_current(
		record_index,
		free_slot->embedding,
		free_slot->embedding_dim
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
		return ret;
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

	return KVS_VECTOR_OK;
}

/*
 * 批量写入内部实现。
 *
 * 注意：
 * 这个函数在 vector async worker 中执行。
 * 当前保持 worker_count=1，避免多个 batch 同时修改 store/id_map/current index。
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
 * 4. current index 追加；
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

	g_vector_state.generation++;

	(void)kvs_vector_manager_capture_delete_delta(record_id);

	return KVS_VECTOR_OK;
}

//重建接口 — safe rebuild with index swap
int kvs_vector_index_rebuild(void) {
	kvs_vector_store_t *store;
	long long start_ms;
	long long elapsed_ms;
	int ret;

	ret = kvs_vector_validate_initialized();
	if (ret != KVS_VECTOR_OK) return ret;

	start_ms = (long long)kv_now_ms();
	store = &g_vector_state.store;

	KVS_LOG_INFO(KVS_VECTOR_LOG_MODULE,
		"vector index safe rebuild start active=%zu deleted=%zu backend=%s",
		store->active_records,
		store->deleted_records,
		kvs_vector_backend_type_name(g_vector_state.index_type));

	/* Step 1: compact store and rebuild id_map (always needed). */
	ret = kvs_vector_store_compact(store);
	if (ret != KVS_VECTOR_OK) goto fail_rebuild;

	ret = kvs_vector_id_map_rebuild_from_store();
	if (ret != KVS_VECTOR_OK) goto fail_rebuild;

	ret = kvs_vector_index_bind_current();
	if (ret != KVS_VECTOR_OK) goto fail_rebuild;

	ret = kvs_vector_index_rebuild_current();
	if (ret != KVS_VECTOR_OK) goto fail_rebuild;

	elapsed_ms = (long long)kv_now_ms() - start_ms;
	g_vector_state.last_rebuild_status = 1;
	g_vector_state.last_rebuild_ms = elapsed_ms;
	g_vector_state.last_rebuild_at_ms = (long long)kv_now_ms();

	KVS_LOG_INFO(KVS_VECTOR_LOG_MODULE,
		"vector index safe rebuild success active=%zu backend=%s elapsed=%lldms",
		store->active_records,
		kvs_vector_backend_type_name(g_vector_state.index_type),
		(long long)elapsed_ms);

	g_vector_state.generation++;

	kvs_vector_index_snapshot_write_best_effort_after_rebuild("vindex_rebuild");

	return KVS_VECTOR_OK;

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
