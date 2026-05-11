#define KVS_VECTOR_INTERNAL
#include "kvs_vector.h"

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
	info->index_ready = state->flat_index.ready;
	info->needs_rebuild = state->flat_index.needs_rebuild;
}

//初始化全局状态
int kvs_vector_init(void) {
	int ret;

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

	// ret = kvs_vector_flat_index_init(
	// 	&g_vector_state.flat_index,
	// 	KVS_VECTOR_DEFAULT_DIMENSION
	// );
	// if (ret != KVS_VECTOR_OK) {
	// 	goto fail;
	// }

	ret = kvs_vector_embedding_provider_init(&g_vector_state.embedding_provider);
	if (ret != KVS_VECTOR_OK) {
		goto fail;
	}

	ret = kvs_vector_flat_index_init(
		&g_vector_state.flat_index,
		g_vector_state.embedding_provider.grpc_healthy &&
		g_vector_state.embedding_provider.grpc_dimension > 0
			? (size_t)g_vector_state.embedding_provider.grpc_dimension
			: KVS_VECTOR_DEFAULT_DIMENSION
	);
	if (ret != KVS_VECTOR_OK) {
		goto fail;
	}

	g_vector_state.initialized = 1;

	KVS_LOG_INFO(
		KVS_VECTOR_LOG_MODULE,
		"vector module initialized provider=%s dimension=%zu capacity=%zu",
		kvs_vector_embedding_provider_name(),
		g_vector_state.flat_index.dimension,
		g_vector_state.store.capacity
	);

	return KVS_VECTOR_OK;

fail:
	KVS_LOG_ERROR(
		KVS_VECTOR_LOG_MODULE,
		"vector module init failed ret=%d",
		ret
	);
	kvs_vector_embedding_provider_destroy(&g_vector_state.embedding_provider);
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
	kvs_vector_flat_index_destroy(&g_vector_state.flat_index);
	kvs_vector_id_map_destroy(&g_vector_state.id_map);
	kvs_vector_store_destroy(&g_vector_state.store);

	memset(&g_vector_state, 0, sizeof(g_vector_state));
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
	 * 第 9 步：VSET 成功后同步写入 flat index。
	 */
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

	if (was_deleted && store->deleted_records > 0) {
		store->deleted_records--;
	}

	store->active_records++;
	store->total_records = store->active_records + store->deleted_records;

	KVS_LOG_DEBUG(
		KVS_VECTOR_LOG_MODULE,
		"vector set success question_len=%zu scope_len=%zu embedding_dim=%d active_records=%zu",
		question_len,
		scope_len,
		free_slot->embedding_dim,
		store->active_records
	);

	return KVS_VECTOR_OK;
}

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
	kvs_vector_flat_index_t *index;
	kvs_vector_topk_heap_t heap;
	float *query_embedding = NULL;
	int query_dim = 0;
	size_t i;
	size_t limit;
	int ret;
	float effective_threshold;
	int heap_initialized = 0;

	memset(&heap, 0, sizeof(heap));

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

	if (!query || query_len == 0 || !results || results_capacity == 0 || !results_count) {
		return KVS_VECTOR_ERR_INVALID_ARG;
	}

	if (!scope) {
		scope_len = 0;
	}

	if (topk <= 0) {
		topk = 1;
	}

	limit = (size_t)topk;
	if (limit > results_capacity) {
		limit = results_capacity;
	}

	effective_threshold = threshold;
	if (effective_threshold <= 0.0f) {
		effective_threshold = 0.5f;
	}

	ret = kvs_vector_embedding_generate(query, query_len, &query_embedding, &query_dim);
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

	ret = kvs_vector_topk_heap_init(&heap, limit);
	if (ret != KVS_VECTOR_OK) {
		kvs_free(query_embedding);
		return ret;
	}
	heap_initialized = 1;

	index = &g_vector_state.flat_index;

	if (!index->vectors || !index->record_indices || index->size == 0 || index->dimension == 0) {
		kvs_vector_topk_heap_destroy(&heap);
		kvs_free(query_embedding);
		return KVS_VECTOR_OK;
	}

	for (i = 0; i < index->size; ++i) {
		size_t record_index = index->record_indices[i];
		float *indexed_vector;
		kvs_vector_record_t *record;
		float vector_score;
		float text_score;
		float score;

		if (record_index >= g_vector_state.store.capacity) {
			continue;
		}

		record = &g_vector_state.store.records[record_index];

		if (index->record_ids && index->record_ids[i] != record->id) {
			continue;
		}

		if (!kvs_vector_record_scope_match(record, scope, scope_len)) {
			continue;
		}

		if ((size_t)query_dim != index->dimension) {
			KVS_LOG_DEBUG(
				KVS_VECTOR_LOG_MODULE,
				"vector search skip dimension mismatch query_dim=%d index_dimension=%zu record_index=%zu",
				query_dim,
				index->dimension,
				record_index
			);
			continue;
		}

		indexed_vector = index->vectors + (i * index->dimension);

		vector_score = kvs_vector_dot_product(
			query_embedding,
			indexed_vector,
			index->dimension
		);

		text_score = kvs_vector_record_score(record, query, query_len);
		score = vector_score > text_score ? vector_score : text_score;

		if (score <= 0.0f) {
			continue;
		}

		if (score < effective_threshold) {
			continue;
		}

		ret = kvs_vector_topk_heap_push(
			&heap,
			record_index,
			score
		);
		if (ret != KVS_VECTOR_OK) {
			kvs_vector_topk_heap_destroy(&heap);
			kvs_free(query_embedding);
			return ret;
		}
	}

	ret = kvs_vector_topk_heap_export_desc(
		&heap,
		results,
		results_capacity,
		results_count,
		&g_vector_state.store
	);

	if (heap_initialized) {
		kvs_vector_topk_heap_destroy(&heap);
	}

	if (ret != KVS_VECTOR_OK) {
		kvs_free(query_embedding);
		return ret;
	}

	KVS_LOG_DEBUG(
		KVS_VECTOR_LOG_MODULE,
		"vector search success query_len=%zu scope_len=%zu topk=%d threshold=%.6f result_count=%zu query_dim=%d",
		query_len,
		scope_len,
		topk,
		threshold,
		*results_count,
		query_dim
	);

	kvs_free(query_embedding);

	return KVS_VECTOR_OK;
}

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

	return KVS_VECTOR_OK;
}

//重建接口
int kvs_vector_index_rebuild(void) {
	int ret;

	ret = kvs_vector_validate_initialized();
	if (ret != KVS_VECTOR_OK) {
		return ret;
	}

	KVS_LOG_INFO(
		KVS_VECTOR_LOG_MODULE,
		"vector index rebuild start active_records=%zu deleted_records=%zu dimension=%zu",
		g_vector_state.store.active_records,
		g_vector_state.store.deleted_records,
		g_vector_state.flat_index.dimension
	);

	ret = kvs_vector_store_compact(&g_vector_state.store);
	if (ret != KVS_VECTOR_OK) {
		KVS_LOG_ERROR(
			KVS_VECTOR_LOG_MODULE,
			"vector index rebuild compact failed ret=%d",
			ret
		);
		return ret;
	}

	ret = kvs_vector_id_map_rebuild_from_store();
	if (ret != KVS_VECTOR_OK) {
		KVS_LOG_ERROR(
			KVS_VECTOR_LOG_MODULE,
			"vector index rebuild id map failed ret=%d",
			ret
		);
		return ret;
	}

	ret = kvs_vector_rebuild_flat_index_from_store();
	if (ret != KVS_VECTOR_OK) {
		KVS_LOG_ERROR(
			KVS_VECTOR_LOG_MODULE,
			"vector index rebuild flat index failed ret=%d",
			ret
		);
		return ret;
	}

	KVS_LOG_INFO(
		KVS_VECTOR_LOG_MODULE,
		"vector index rebuild success active_records=%zu index_size=%zu dimension=%zu",
		g_vector_state.store.active_records,
		g_vector_state.flat_index.size,
		g_vector_state.flat_index.dimension
	);

	return KVS_VECTOR_OK;
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