#define KVS_VECTOR_INTERNAL
#include "kvs_vector.h"
#include "kvs_vector_distance.h"

kvs_vector_state_t g_vector_state;

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
	if (strcasecmp(g_cfg.vector_index_type, "hnsw") == 0) {
		return KVS_VECTOR_INDEX_TYPE_HNSW;
	}

	if (strcasecmp(g_cfg.vector_index_type, "sq8_flat") == 0) {
		return KVS_VECTOR_INDEX_TYPE_SQ8_FLAT;
	}

	return KVS_VECTOR_INDEX_TYPE_FLAT;
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

int kvs_vector_index_bind_current(void) {
    kvs_vector_index_t *index = &g_vector_state.current_index;

    memset(index, 0, sizeof(*index));

    index->type = g_vector_state.index_type;
    index->metric = g_vector_state.metric_type;

    if (g_vector_state.index_type == KVS_VECTOR_INDEX_TYPE_HNSW) {
        index->dimension = g_vector_state.ann_index.dimension;
        index->impl = &g_vector_state.ann_index;
        index->ops = &g_kvs_vector_hnsw_ops;
    } else if (g_vector_state.index_type == KVS_VECTOR_INDEX_TYPE_SQ8_FLAT) {
        index->dimension = g_vector_state.sq8_flat_index.dimension;
        index->impl = &g_vector_state.sq8_flat_index;
        index->ops = &g_kvs_vector_sq8_flat_ops;
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

    if (!index->ops || !index->ops->search) {
        return KVS_VECTOR_ERR_NOT_INITIALIZED;
    }

    memset(&params, 0, sizeof(params));
    params.scope = scope;
    params.scope_len = scope_len;
    params.topk = topk;
    params.threshold = threshold;

    return index->ops->search(
        index,
        query_embedding,
        query_dim,
        &params,
        results,
        results_capacity,
        results_count
    );
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

	if (state->index_type == KVS_VECTOR_INDEX_TYPE_HNSW) {
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
	info->last_rebuild_status = state->last_rebuild_status;
	info->last_rebuild_ms = state->last_rebuild_ms;
	info->vector_memory_bytes = kvs_vector_estimate_record_memory_bytes();
	info->index_memory_bytes = kvs_vector_estimate_current_index_memory_bytes();
	info->total_vector_memory_bytes = info->vector_memory_bytes + info->index_memory_bytes;
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

size_t kvs_vector_estimate_current_index_memory_bytes(void) {
	switch (g_vector_state.index_type) {
		case KVS_VECTOR_INDEX_TYPE_FLAT:
			return kvs_vector_estimate_flat_memory_bytes();
		case KVS_VECTOR_INDEX_TYPE_HNSW:
			return kvs_vector_estimate_flat_memory_bytes() +
			       kvs_vector_estimate_hnsw_memory_bytes();
		case KVS_VECTOR_INDEX_TYPE_SQ8_FLAT:
			return kvs_vector_estimate_sq8_flat_memory_bytes();
		default:
			return 0;
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
	 * Flat remains available as a debug / unit-test baseline. The formal
	 * semantic-cache path uses HNSW by default.
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
		"vector module initialized provider=%s distance_backend=%s dimension=%zu capacity=%zu index_type=%s metric=%s hnsw_m=%zu ef_construction=%zu ef_search=%zu",
		kvs_vector_embedding_provider_name(),
		kvs_vector_distance_backend_name(),
		g_vector_state.flat_index.dimension,
		g_vector_state.store.capacity,
		g_vector_state.index_type == KVS_VECTOR_INDEX_TYPE_HNSW ? "hnsw" :
			(g_vector_state.index_type == KVS_VECTOR_INDEX_TYPE_SQ8_FLAT ? "sq8_flat" : "flat"),
		kvs_vector_metric_name(g_vector_state.metric_type),
		g_vector_state.ann_index.m,
		g_vector_state.ann_index.ef_construction,
		g_vector_state.ann_index.ef_search
	);

	return KVS_VECTOR_OK;

fail:
	KVS_LOG_ERROR(
		KVS_VECTOR_LOG_MODULE,
		"vector module init failed ret=%d",
		ret
	);
	kvs_vector_embedding_provider_destroy(&g_vector_state.embedding_provider);
	kvs_vector_sq8_flat_index_destroy(&g_vector_state.sq8_flat_index);
	kvs_vector_ann_index_destroy(&g_vector_state.ann_index);
	kvs_vector_flat_index_destroy(&g_vector_state.flat_index);
	kvs_vector_id_map_destroy(&g_vector_state.id_map);
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
	kvs_vector_ann_index_destroy(&g_vector_state.ann_index);
	kvs_vector_flat_index_destroy(&g_vector_state.flat_index);
	kvs_vector_id_map_destroy(&g_vector_state.id_map);
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

	ret = kvs_vector_flat_index_init(&g_vector_state.flat_index, dimension);
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
	int64_t updated_at_ms
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

	if (store->next_slot <= record_index) {
		store->next_slot = record_index + 1;
	}

	if (store->next_id <= record_id) {
		store->next_id = record_id + 1;
	}

	if (state == KVS_VECTOR_RECORD_ACTIVE && !deleted) {
		store->active_records++;
		kvs_vector_id_map_put(
			&g_vector_state.id_map,
			record->question,
			record->question_len,
			record->scope,
			record->scope_len,
			record_index
		);
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

	return kvs_vector_index_bind_current();
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

	/*
	 * Keep flat as a debug / unit-test baseline for HNSW recall checks.
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
			kvs_vector_record_destroy(free_slot);
			return ret;
		}
	}

	if (was_deleted && store->deleted_records > 0) {
		store->deleted_records--;
	}

	store->active_records++;
	store->total_records = store->active_records + store->deleted_records;

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

	ret = kvs_vector_record_mark_deleted(record);
	if (ret != KVS_VECTOR_OK) {
		return ret;
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
