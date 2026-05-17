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

static void *kvs_vector_async_worker_main(void *arg) {
	kvs_vector_async_dispatcher_t *dispatcher;
	void *item = NULL;

	dispatcher = (kvs_vector_async_dispatcher_t *)arg;
	if (!dispatcher) {
		return NULL;
	}

	for (;;) {
		kvs_vector_async_task_t *task = NULL;

		pthread_mutex_lock(&dispatcher->mu);

		while (!dispatcher->stopping &&
		       msg_queue_pop(&dispatcher->queue, &item) != 0) {
			pthread_cond_wait(&dispatcher->cond, &dispatcher->mu);
		}

		if (dispatcher->stopping) {
			pthread_mutex_unlock(&dispatcher->mu);
			break;
		}

		pthread_mutex_unlock(&dispatcher->mu);

		task = (kvs_vector_async_task_t *)item;
		if (!task) {
			continue;
		}

		switch (task->type) {
			case KVS_VECTOR_ASYNC_TASK_STOP:
				task->result = KVS_VECTOR_OK;
				break;

			case KVS_VECTOR_ASYNC_TASK_BATCH_SET:
            {
                kvs_vector_batch_set_payload_t *payload;

                payload = (kvs_vector_batch_set_payload_t *)task->payload;
                if (!payload) {
                    task->result = KVS_VECTOR_ERR_INVALID_ARG;
                    break;
                }

                task->result = kvs_vector_set_batch_internal(
                    payload->questions,
                    payload->question_lens,
                    payload->answers,
                    payload->answer_lens,
                    payload->scopes,
                    payload->scope_lens,
                    payload->count,
                    &task->success_count,
                    &task->failed_count
                );
                break;
            }

			default:
				task->result = KVS_VECTOR_ERR_INVALID_ARG;
				break;
		}

		pthread_mutex_lock(&task->mu);
		task->done = 1;
		pthread_cond_signal(&task->cond);
		pthread_mutex_unlock(&task->mu);
	}

	return NULL;
}

void kvs_vector_async_task_init(
	kvs_vector_async_task_t *task,
	kvs_vector_async_task_type_t type,
	void *payload
) {
	if (!task) {
		return;
	}

	memset(task, 0, sizeof(*task));

	task->type = type;
	task->payload = payload;
	task->result = KVS_VECTOR_ERR;
	task->success_count = 0;
	task->failed_count = 0;
	task->done = 0;

	pthread_mutex_init(&task->mu, NULL);
	pthread_cond_init(&task->cond, NULL);
}

void kvs_vector_async_task_destroy(kvs_vector_async_task_t *task) {
	if (!task) {
		return;
	}

	pthread_cond_destroy(&task->cond);
	pthread_mutex_destroy(&task->mu);

	memset(task, 0, sizeof(*task));
}

int kvs_vector_async_dispatcher_init(
	kvs_vector_async_dispatcher_t *dispatcher,
	size_t queue_capacity,
	size_t worker_count
) {
	size_t i;

	if (!dispatcher || queue_capacity == 0 || worker_count == 0) {
		return KVS_VECTOR_ERR_INVALID_ARG;
	}

	memset(dispatcher, 0, sizeof(*dispatcher));

	if (msg_queue_init(&dispatcher->queue, (int)queue_capacity) != 0) {
		return KVS_VECTOR_ERR;
	}

	pthread_mutex_init(&dispatcher->mu, NULL);
	pthread_cond_init(&dispatcher->cond, NULL);

	dispatcher->workers = (pthread_t *)kvs_malloc(worker_count * sizeof(pthread_t));
	if (!dispatcher->workers) {
		msg_queue_destroy(&dispatcher->queue);
		pthread_cond_destroy(&dispatcher->cond);
		pthread_mutex_destroy(&dispatcher->mu);
		return KVS_VECTOR_ERR;
	}

	memset(dispatcher->workers, 0, worker_count * sizeof(pthread_t));

	dispatcher->worker_count = worker_count;
	dispatcher->stopping = 0;

	for (i = 0; i < worker_count; ++i) {
		if (pthread_create(
				&dispatcher->workers[i],
				NULL,
				kvs_vector_async_worker_main,
				dispatcher) != 0) {
			dispatcher->stopping = 1;
			pthread_cond_broadcast(&dispatcher->cond);

			for (size_t j = 0; j < i; ++j) {
				pthread_join(dispatcher->workers[j], NULL);
			}

			kvs_free(dispatcher->workers);
			dispatcher->workers = NULL;
			msg_queue_destroy(&dispatcher->queue);
			pthread_cond_destroy(&dispatcher->cond);
			pthread_mutex_destroy(&dispatcher->mu);
			return KVS_VECTOR_ERR;
		}
	}

	dispatcher->initialized = 1;

	KVS_LOG_INFO(
		KVS_VECTOR_LOG_MODULE,
		"vector async dispatcher initialized queue_capacity=%zu worker_count=%zu",
		queue_capacity,
		worker_count
	);

	return KVS_VECTOR_OK;
}

void kvs_vector_async_dispatcher_destroy(
	kvs_vector_async_dispatcher_t *dispatcher
) {
	size_t i;

	if (!dispatcher || !dispatcher->initialized) {
		return;
	}

	pthread_mutex_lock(&dispatcher->mu);
	dispatcher->stopping = 1;
	pthread_cond_broadcast(&dispatcher->cond);
	pthread_mutex_unlock(&dispatcher->mu);

	for (i = 0; i < dispatcher->worker_count; ++i) {
		pthread_join(dispatcher->workers[i], NULL);
	}

	if (dispatcher->workers) {
		kvs_free(dispatcher->workers);
		dispatcher->workers = NULL;
	}

	msg_queue_destroy(&dispatcher->queue);

	pthread_cond_destroy(&dispatcher->cond);
	pthread_mutex_destroy(&dispatcher->mu);

	memset(dispatcher, 0, sizeof(*dispatcher));

	KVS_LOG_INFO(KVS_VECTOR_LOG_MODULE, "vector async dispatcher destroyed");
}

int kvs_vector_async_submit_and_wait(
	kvs_vector_async_dispatcher_t *dispatcher,
	kvs_vector_async_task_t *task
) {
	int ret;

	if (!dispatcher || !dispatcher->initialized || !task) {
		return KVS_VECTOR_ERR_INVALID_ARG;
	}

	pthread_mutex_lock(&dispatcher->mu);

	if (dispatcher->stopping) {
		pthread_mutex_unlock(&dispatcher->mu);
		return KVS_VECTOR_ERR;
	}

	ret = msg_queue_push(&dispatcher->queue, task);
	if (ret != 0) {
		pthread_mutex_unlock(&dispatcher->mu);
		return KVS_VECTOR_ERR;
	}

	pthread_cond_signal(&dispatcher->cond);
	pthread_mutex_unlock(&dispatcher->mu);

	pthread_mutex_lock(&task->mu);
	while (!task->done) {
		pthread_cond_wait(&task->cond, &task->mu);
	}
	pthread_mutex_unlock(&task->mu);

	return task->result;
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

static kvs_vector_index_type_t kvs_vector_index_type_from_config(void) {
	if (strcasecmp(g_cfg.vector_index_type, "hnsw") == 0) {
		return KVS_VECTOR_INDEX_TYPE_HNSW;
	}

	return KVS_VECTOR_INDEX_TYPE_FLAT;
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

static int kvs_vector_flat_ops_search_batch(
    kvs_vector_index_t *index,
    const float *query_embeddings,
    size_t query_count,
    int query_dim,
    const kvs_vector_index_batch_params_t *params,
    kvs_vector_search_result_t *results,
    size_t results_capacity_per_query,
    size_t *results_counts
) {
    if (!index || !index->impl || !params) {
        return KVS_VECTOR_ERR_INVALID_ARG;
    }

    return kvs_vector_flat_index_search_batch(
        (kvs_vector_flat_index_t *)index->impl,
        query_embeddings,
        query_count,
        query_dim,
        params->scope,
        params->scope_len,
        params->topk,
        params->threshold,
        index->metric,
        results,
        results_capacity_per_query,
        results_counts
    );
}

static int kvs_vector_hnsw_ops_search_batch(
    kvs_vector_index_t *index,
    const float *query_embeddings,
    size_t query_count,
    int query_dim,
    const kvs_vector_index_batch_params_t *params,
    kvs_vector_search_result_t *results,
    size_t results_capacity_per_query,
    size_t *results_counts
) {
    if (!index || !index->impl || !params) {
        return KVS_VECTOR_ERR_INVALID_ARG;
    }

    return kvs_vector_ann_index_search_batch(
        (kvs_vector_ann_index_t *)index->impl,
        query_embeddings,
        query_count,
        query_dim,
        params->scope,
        params->scope_len,
        params->topk,
        params->threshold,
        index->metric,
        results,
        results_capacity_per_query,
        results_counts
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
    .search_batch = kvs_vector_flat_ops_search_batch,
    .rebuild = kvs_vector_flat_ops_rebuild,
    .info = kvs_vector_index_ops_info,
    .destroy = kvs_vector_flat_ops_destroy,
};

static const kvs_vector_index_ops_t g_kvs_vector_hnsw_ops = {
    .add = kvs_vector_hnsw_ops_add,
    .search = kvs_vector_hnsw_ops_search,
    .search_batch = kvs_vector_hnsw_ops_search_batch,
    .rebuild = kvs_vector_hnsw_ops_rebuild,
    .info = kvs_vector_index_ops_info,
    .destroy = kvs_vector_hnsw_ops_destroy,
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

int kvs_vector_index_search_batch_current(
    const float *query_embeddings,
    size_t query_count,
    int query_dim,
    const char *scope,
    size_t scope_len,
    int topk,
    float threshold,
    kvs_vector_search_result_t *results,
    size_t results_capacity_per_query,
    size_t *results_counts
) {
    kvs_vector_index_t *index = &g_vector_state.current_index;
    kvs_vector_index_batch_params_t params;

    if (!index->ops || !index->ops->search_batch) {
        return KVS_VECTOR_ERR_NOT_INITIALIZED;
    }

    memset(&params, 0, sizeof(params));
    params.scope = scope;
    params.scope_len = scope_len;
    params.topk = topk;
    params.threshold = threshold;

    return index->ops->search_batch(
        index,
        query_embeddings,
        query_count,
        query_dim,
        &params,
        results,
        results_capacity_per_query,
        results_counts
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

	ret = kvs_vector_embedding_provider_init(&g_vector_state.embedding_provider);
	if (ret != KVS_VECTOR_OK) {
		goto fail;
	}

	ret = kvs_vector_flat_index_init(
		&g_vector_state.flat_index,
		KVS_VECTOR_DEFAULT_DIMENSION
	);
	if (ret != KVS_VECTOR_OK) {
		goto fail;
	}

	g_vector_state.index_type = kvs_vector_index_type_from_config();
	g_vector_state.metric_type = kvs_vector_metric_from_config();

	ret = kvs_vector_ann_index_init(
		&g_vector_state.ann_index,
		g_vector_state.flat_index.dimension,
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

	ret = kvs_vector_index_bind_current();
	if (ret != KVS_VECTOR_OK) {
		goto fail;
	}

    ret = kvs_vector_async_dispatcher_init(
		&g_vector_state.async_dispatcher,
		KVS_VECTOR_ASYNC_QUEUE_CAPACITY,
		KVS_VECTOR_ASYNC_WORKER_COUNT
	);
	if (ret != KVS_VECTOR_OK) {
		goto fail;
	}

	g_vector_state.initialized = 1;

	KVS_LOG_INFO(
		KVS_VECTOR_LOG_MODULE,
		"vector module initialized provider=%s dimension=%zu capacity=%zu index_type=%s metric=%s hnsw_m=%zu ef_construction=%zu ef_search=%zu",
		kvs_vector_embedding_provider_name(),
		g_vector_state.flat_index.dimension,
		g_vector_state.store.capacity,
		g_vector_state.index_type == KVS_VECTOR_INDEX_TYPE_HNSW ? "hnsw" : "flat",
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
	kvs_vector_async_dispatcher_destroy(&g_vector_state.async_dispatcher);  
	kvs_vector_embedding_provider_destroy(&g_vector_state.embedding_provider);
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

	kvs_vector_async_dispatcher_destroy(&g_vector_state.async_dispatcher);
	
	kvs_vector_embedding_provider_destroy(&g_vector_state.embedding_provider);
	kvs_vector_ann_index_destroy(&g_vector_state.ann_index);
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

	if (g_vector_state.index_type == KVS_VECTOR_INDEX_TYPE_HNSW) {
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
int kvs_vector_set_batch_internal(
	const char **questions,
	const size_t *question_lens,
	const char **answers,
	const size_t *answer_lens,
	const char **scopes,
	const size_t *scope_lens,
	size_t count,
	size_t *success_count,
	size_t *failed_count
) {
	kvs_vector_store_t *store;
	float *batch_embeddings = NULL;
	int embedding_dim = 0;
	size_t i;
	size_t success = 0;
	size_t failed = 0;
	size_t new_slot_need;
	int ret;

	if (success_count) {
		*success_count = 0;
	}

	if (failed_count) {
		*failed_count = 0;
	}

	ret = kvs_vector_validate_initialized();
	if (ret != KVS_VECTOR_OK) {
		return ret;
	}

	if (!questions || !question_lens || !answers || !answer_lens || count == 0) {
		return KVS_VECTOR_ERR_INVALID_ARG;
	}

	for (i = 0; i < count; ++i) {
		if (!questions[i] || question_lens[i] == 0 || !answers[i]) {
			return KVS_VECTOR_ERR_INVALID_ARG;
		}
	}

	/*
	* 一次性批量生成 embedding。
	*
	* 当前默认使用普通 batch，而不是 parallel batch。
	*
	* 原因：
	* 1. sentence-transformers / PyTorch 在 CPU 上本身会使用多线程；
	* 2. 客户端侧拆成多个并发请求会造成 CPU 线程竞争；
	* 3. 实测 parallel batch 的 10000 条写入吞吐低于普通 batch；
	* 4. 所以默认写入路径保持单次 batch encode，parallel 接口仅保留用于实验。
	*/
	ret = kvs_vector_embedding_generate_batch(
		questions,
		question_lens,
		count,
		&batch_embeddings,
		&embedding_dim
	);
	if (ret != KVS_VECTOR_OK) {
		KVS_LOG_ERROR(
			KVS_VECTOR_LOG_MODULE,
			"vector batch set embedding generate failed count=%zu ret=%d",
			count,
			ret
		);
		return ret;
	}

	if (!batch_embeddings || embedding_dim <= 0) {
		if (batch_embeddings) {
			kvs_free(batch_embeddings);
		}
		return KVS_VECTOR_ERR;
	}

	store = &g_vector_state.store;

	/*
	 * 预留 store/id_map/flat_index 容量，减少批量写入中的反复扩容。
	 * free list 中的 deleted 槽位可以复用，所以这里只做保守估算。
	 */
	new_slot_need = count;
	if (store->free_count >= count) {
		new_slot_need = 0;
	} else {
		new_slot_need = count - store->free_count;
	}

	if (new_slot_need > 0) {
		ret = kvs_vector_store_ensure_capacity(
			store,
			store->next_slot + new_slot_need
		);
		if (ret != KVS_VECTOR_OK) {
			kvs_free(batch_embeddings);
			return ret;
		}
	}

	ret = kvs_vector_id_map_reserve(
		&g_vector_state.id_map,
		(g_vector_state.id_map.size + count) * 2
	);
	if (ret != KVS_VECTOR_OK) {
		kvs_free(batch_embeddings);
		return ret;
	}

	ret = kvs_vector_flat_index_reserve(
		&g_vector_state.flat_index,
		g_vector_state.flat_index.size + count
	);
	if (ret != KVS_VECTOR_OK) {
		kvs_free(batch_embeddings);
		return ret;
	}

	for (i = 0; i < count; ++i) {
		kvs_vector_record_t *record;
		kvs_vector_record_t *free_slot;
		const char *scope = NULL;
		size_t scope_len = 0;
		size_t record_index;
		int was_deleted = 0;
		const float *embedding;

		if (scopes && scope_lens && scopes[i]) {
			scope = scopes[i];
			scope_len = scope_lens[i];
		}

		/*
		 * 已存在则只覆盖 answer。
		 * question 不变，embedding 不变。
		 */
		record = kvs_vector_find_record(
			questions[i],
			question_lens[i],
			scope,
			scope_len
		);
		if (record) {
			ret = kvs_vector_record_update_answer(
				record,
				answers[i],
				answer_lens[i]
			);
			if (ret == KVS_VECTOR_OK) {
				success++;
			} else {
				failed++;
			}
			continue;
		}

		free_slot = kvs_vector_find_free_slot();
		if (!free_slot) {
			failed++;
			continue;
		}

		was_deleted = (free_slot->state == KVS_VECTOR_RECORD_DELETED) ? 1 : 0;
		record_index = (size_t)(free_slot - store->records);
		embedding = batch_embeddings + (i * (size_t)embedding_dim);

		ret = kvs_vector_record_set_fields_with_embedding(
			free_slot,
			questions[i],
			question_lens[i],
			answers[i],
			answer_lens[i],
			scope,
			scope_len,
			embedding,
			embedding_dim
		);
		if (ret != KVS_VECTOR_OK) {
			failed++;
			continue;
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
			failed++;
			continue;
		}

		ret = kvs_vector_flat_index_add(
			&g_vector_state.flat_index,
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
			kvs_vector_record_destroy(free_slot);
			failed++;
			continue;
		}

		if (g_vector_state.index_type == KVS_VECTOR_INDEX_TYPE_HNSW) {
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
				kvs_vector_record_destroy(free_slot);
				failed++;
				continue;
			}
		}

		if (was_deleted && store->deleted_records > 0) {
			store->deleted_records--;
		}

		store->active_records++;
		store->total_records = store->active_records + store->deleted_records;
		success++;
	}

	kvs_free(batch_embeddings);

	if (success_count) {
		*success_count = success;
	}

	if (failed_count) {
		*failed_count = failed;
	}

	KVS_LOG_INFO(
		KVS_VECTOR_LOG_MODULE,
		"vector batch set finished count=%zu success=%zu failed=%zu active_records=%zu",
		count,
		success,
		failed,
		store->active_records
	);

	return failed == 0 ? KVS_VECTOR_OK : KVS_VECTOR_ERR;
}

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
int kvs_vector_set_batch_with_embeddings_internal(
	const char **questions,
	const size_t *question_lens,
	const char **answers,
	const size_t *answer_lens,
	const char **scopes,
	const size_t *scope_lens,
	const float *embeddings,
	int embedding_dim,
	size_t count,
	size_t *success_count,
	size_t *failed_count
) {
	kvs_vector_store_t *store;
	size_t i;
	size_t success = 0;
	size_t failed = 0;
	size_t new_slot_need;
	int ret;

	if (success_count) {
		*success_count = 0;
	}

	if (failed_count) {
		*failed_count = 0;
	}

	ret = kvs_vector_validate_initialized();
	if (ret != KVS_VECTOR_OK) {
		return ret;
	}

	if (!questions || !question_lens ||
	    !answers || !answer_lens ||
	    !embeddings || embedding_dim <= 0 ||
	    count == 0) {
		return KVS_VECTOR_ERR_INVALID_ARG;
	}

	for (i = 0; i < count; ++i) {
		if (!questions[i] || question_lens[i] == 0 || !answers[i]) {
			return KVS_VECTOR_ERR_INVALID_ARG;
		}
	}

	store = &g_vector_state.store;

	/*
	 * 纯存储压测仍然保持批量预留容量，
	 * 避免扩容成本干扰每条写入性能。
	 */
	new_slot_need = count;
	if (store->free_count >= count) {
		new_slot_need = 0;
	} else {
		new_slot_need = count - store->free_count;
	}

	if (new_slot_need > 0) {
		ret = kvs_vector_store_ensure_capacity(
			store,
			store->next_slot + new_slot_need
		);
		if (ret != KVS_VECTOR_OK) {
			return ret;
		}
	}

	ret = kvs_vector_id_map_reserve(
		&g_vector_state.id_map,
		(g_vector_state.id_map.size + count) * 2
	);
	if (ret != KVS_VECTOR_OK) {
		return ret;
	}

	ret = kvs_vector_flat_index_reserve(
		&g_vector_state.flat_index,
		g_vector_state.flat_index.size + count
	);
	if (ret != KVS_VECTOR_OK) {
		return ret;
	}

	for (i = 0; i < count; ++i) {
		kvs_vector_record_t *record;
		kvs_vector_record_t *free_slot;
		const char *scope = NULL;
		size_t scope_len = 0;
		size_t record_index;
		int was_deleted = 0;
		const float *embedding;

		if (scopes && scope_lens && scopes[i]) {
			scope = scopes[i];
			scope_len = scope_lens[i];
		}

		/*
		 * 如果已经存在，只覆盖 answer。
		 * 这里不重写 embedding，保持和 VSET / VBATCHSET 语义一致：
		 * question 不变时，向量不变。
		 */
		record = kvs_vector_find_record(
			questions[i],
			question_lens[i],
			scope,
			scope_len
		);
		if (record) {
			ret = kvs_vector_record_update_answer(
				record,
				answers[i],
				answer_lens[i]
			);
			if (ret == KVS_VECTOR_OK) {
				success++;
			} else {
				failed++;
			}
			continue;
		}

		free_slot = kvs_vector_find_free_slot();
		if (!free_slot) {
			failed++;
			continue;
		}

		was_deleted = (free_slot->state == KVS_VECTOR_RECORD_DELETED) ? 1 : 0;
		record_index = (size_t)(free_slot - store->records);
		embedding = embeddings + (i * (size_t)embedding_dim);

		ret = kvs_vector_record_set_fields_with_embedding(
			free_slot,
			questions[i],
			question_lens[i],
			answers[i],
			answer_lens[i],
			scope,
			scope_len,
			embedding,
			embedding_dim
		);
		if (ret != KVS_VECTOR_OK) {
			failed++;
			continue;
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
			failed++;
			continue;
		}

		ret = kvs_vector_flat_index_add(
			&g_vector_state.flat_index,
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
			kvs_vector_record_destroy(free_slot);
			failed++;
			continue;
		}

		if (g_vector_state.index_type == KVS_VECTOR_INDEX_TYPE_HNSW) {
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
				kvs_vector_record_destroy(free_slot);
				failed++;
				continue;
			}
		}

		if (was_deleted && store->deleted_records > 0) {
			store->deleted_records--;
		}

		store->active_records++;
		store->total_records = store->active_records + store->deleted_records;
		success++;
	}

	if (success_count) {
		*success_count = success;
	}

	if (failed_count) {
		*failed_count = failed;
	}

	KVS_LOG_INFO(
		KVS_VECTOR_LOG_MODULE,
		"vector store bench batch finished count=%zu success=%zu failed=%zu active_records=%zu",
		count,
		success,
		failed,
		store->active_records
	);

	return failed == 0 ? KVS_VECTOR_OK : KVS_VECTOR_ERR;
}



/*
 * 批量写入异步入口。
 *
 * 当前语义：
 * 1. 投递到 msg_queue；
 * 2. 后台 worker 执行；
 * 3. 当前调用线程等待完成；
 * 4. 返回时数据已经写入。
 */
int kvs_vector_set_batch_async(
	const char **questions,
	const size_t *question_lens,
	const char **answers,
	const size_t *answer_lens,
	const char **scopes,
	const size_t *scope_lens,
	size_t count,
	size_t *success_count,
	size_t *failed_count
) {
	kvs_vector_batch_set_payload_t payload;
	kvs_vector_async_task_t task;
	int ret;

	if (success_count) {
		*success_count = 0;
	}

	if (failed_count) {
		*failed_count = 0;
	}

	if (!questions || !question_lens || !answers || !answer_lens || count == 0) {
		return KVS_VECTOR_ERR_INVALID_ARG;
	}

	memset(&payload, 0, sizeof(payload));

	payload.questions = questions;
	payload.question_lens = question_lens;
	payload.answers = answers;
	payload.answer_lens = answer_lens;
	payload.scopes = scopes;
	payload.scope_lens = scope_lens;
	payload.count = count;

	kvs_vector_async_task_init(
		&task,
		KVS_VECTOR_ASYNC_TASK_BATCH_SET,
		&payload
	);

	ret = kvs_vector_async_submit_and_wait(
		&g_vector_state.async_dispatcher,
		&task
	);

	if (success_count) {
		*success_count = task.success_count;
	}

	if (failed_count) {
		*failed_count = task.failed_count;
	}

	kvs_vector_async_task_destroy(&task);

	return ret;
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
int kvs_vector_search_batch(
	const char **queries,
	const size_t *query_lens,
	size_t query_count,
	const char *scope,
	size_t scope_len,
	int topk,
	float threshold,
	kvs_vector_search_result_t *results,
	size_t results_capacity_per_query,
	size_t *results_counts
) {
	float *batch_embeddings = NULL;
	int embedding_dim = 0;
	size_t i;
	int ret;

	ret = kvs_vector_validate_initialized();
	if (ret != KVS_VECTOR_OK) {
		return ret;
	}

	if (!queries || !query_lens || query_count == 0 ||
	    !results || results_capacity_per_query == 0 || !results_counts) {
		return KVS_VECTOR_ERR_INVALID_ARG;
	}

	if (!scope) {
		scope_len = 0;
	}

	if (topk <= 0) {
		topk = 1;
	}

	for (i = 0; i < query_count; ++i) {
		results_counts[i] = 0;

		if (!queries[i] || query_lens[i] == 0) {
			return KVS_VECTOR_ERR_INVALID_ARG;
		}
	}

	kvs_vector_search_results_reset(
		results,
		query_count * results_capacity_per_query
	);

	ret = kvs_vector_embedding_generate_batch(
		queries,
		query_lens,
		query_count,
		&batch_embeddings,
		&embedding_dim
	);
	if (ret != KVS_VECTOR_OK) {
		KVS_LOG_ERROR(
			KVS_VECTOR_LOG_MODULE,
			"vector batch search embedding generate failed count=%zu ret=%d",
			query_count,
			ret
		);
		return ret;
	}

	if (!batch_embeddings || embedding_dim <= 0) {
		if (batch_embeddings) {
			kvs_free(batch_embeddings);
		}
		return KVS_VECTOR_ERR;
	}

	ret = kvs_vector_index_search_batch_current(
		batch_embeddings,
		query_count,
		embedding_dim,
		scope,
		scope_len,
		topk,
		threshold,
		results,
		results_capacity_per_query,
		results_counts
	);
	if (ret != KVS_VECTOR_OK) {
		KVS_LOG_ERROR(
			KVS_VECTOR_LOG_MODULE,
			"vector batch search failed count=%zu ret=%d",
			query_count,
			ret
		);
		kvs_free(batch_embeddings);
		return ret;
	}

	kvs_free(batch_embeddings);
	return KVS_VECTOR_OK;
}

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
int kvs_vector_search_with_embedding_internal(
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
	kvs_vector_flat_index_t *index;
	kvs_vector_topk_heap_t heap;
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

	if (!query_embedding ||
	    query_dim <= 0 ||
	    !results ||
	    results_capacity == 0 ||
	    !results_count) {
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
		effective_threshold = -1.0f;
	}

	return kvs_vector_index_search_current(
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

	ret = kvs_vector_index_bind_current();
	if (ret != KVS_VECTOR_OK) {
		return ret;
	}

	if (g_vector_state.index_type == KVS_VECTOR_INDEX_TYPE_HNSW) {
		ret = kvs_vector_index_rebuild_current();
		if (ret != KVS_VECTOR_OK) {
			return ret;
		}
	}

	ret = kvs_vector_index_bind_current();
	if (ret != KVS_VECTOR_OK) {
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