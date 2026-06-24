#define KVS_VECTOR_INTERNAL
#include "kvs_vector.h"
#include "kvs_vector_hnswlib_backend.h"

extern size_t g_hnswlib_add_count;
extern size_t g_hnswlib_search_count;

static int hnswlib_ops_add(
    kvs_vector_index_t *index,
    size_t record_index,
    const float *embedding,
    int embedding_dim
) {
    kvs_hnswlib_index_t *hnsw;
    size_t cur_ntotal, cur_max;
    if (!index || !index->impl) return KVS_VECTOR_ERR_INVALID_ARG;

    hnsw = (kvs_hnswlib_index_t *)index->impl;
    cur_ntotal = kvs_hnswlib_index_ntotal(hnsw);
    cur_max    = kvs_hnswlib_index_get_max_elements(hnsw);

    /* Auto-resize when the hnswlib index is full */
    if (cur_ntotal >= cur_max) {
        size_t new_max = cur_max * 2;
        if (new_max < 1024) new_max = 1024;
        if (kvs_hnswlib_index_resize(hnsw, new_max) != 0)
            return KVS_VECTOR_ERR;
    }

    if (kvs_hnswlib_index_add(hnsw,
            record_index, embedding, embedding_dim) != 0)
        return KVS_VECTOR_ERR;

    __atomic_add_fetch(&g_hnswlib_add_count, 1, __ATOMIC_RELAXED);
    return KVS_VECTOR_OK;
}

static int hnswlib_ops_search(
    kvs_vector_index_t *index,
    const float *query_embedding,
    int query_dim,
    const kvs_vector_index_search_params_t *params,
    kvs_vector_search_result_t *results,
    size_t results_capacity,
    size_t *results_count
) {
    kvs_vector_topk_heap_t heap;
    size_t limit;
    int nfound, i;
    size_t *labels = NULL;
    float *scores = NULL;
    int ret;
    int heap_initialized = 0;
    float effective_threshold;
    size_t effective_ef;

    memset(&heap, 0, sizeof(heap));
    if (results && results_capacity > 0)
        kvs_vector_search_results_reset(results, results_capacity);
    if (results_count) *results_count = 0;

    if (!index || !index->impl || !params || !query_embedding ||
        query_dim <= 0 || !results || results_capacity == 0 || !results_count)
        return KVS_VECTOR_ERR_INVALID_ARG;

    limit = (size_t)params->topk;
    if (limit > results_capacity) limit = results_capacity;
    if (limit == 0) return KVS_VECTOR_OK;

    effective_threshold = params->threshold;
    if (effective_threshold <= 0.0f) effective_threshold = -1.0f;

    labels = (size_t *)kvs_malloc(limit * sizeof(size_t));
    if (!labels) return KVS_VECTOR_ERR;
    scores = (float *)kvs_malloc(limit * sizeof(float));
    if (!scores) { kvs_free(labels); return KVS_VECTOR_ERR; }

    effective_ef = g_vector_state.ann_index.ef_search ?
        g_vector_state.ann_index.ef_search : KVS_VECTOR_HNSW_EF_SEARCH;
    if (effective_ef < limit) {
        effective_ef = limit;
    }
    kvs_hnswlib_index_set_ef((kvs_hnswlib_index_t *)index->impl, effective_ef);

    nfound = kvs_hnswlib_index_search((kvs_hnswlib_index_t *)index->impl,
        query_embedding, query_dim, (int)limit, labels, scores);
    if (nfound < 0) {
        kvs_free(scores); kvs_free(labels);
        return KVS_VECTOR_ERR;
    }
    __atomic_add_fetch(&g_hnswlib_search_count, 1, __ATOMIC_RELAXED);

    ret = kvs_vector_topk_heap_init(&heap, limit);
    if (ret != KVS_VECTOR_OK) {
        kvs_free(scores); kvs_free(labels);
        return ret;
    }
    heap_initialized = 1;

    for (i = 0; i < nfound; ++i) {
        size_t record_index = labels[i];
        kvs_vector_record_t *record;
        float score = scores[i];

        if (score < effective_threshold) continue;
        if (record_index >= g_vector_state.store.capacity) continue;

        record = &g_vector_state.store.records[record_index];
        if (record->state != KVS_VECTOR_RECORD_ACTIVE || record->deleted)
            continue;
        if (!kvs_vector_record_scope_match(record, params->scope, params->scope_len))
            continue;

        kvs_vector_topk_heap_push(&heap, record_index, score);
    }

    kvs_free(scores);
    kvs_free(labels);

    ret = kvs_vector_topk_heap_export_desc(&heap, results, results_capacity,
        results_count, &g_vector_state.store);
    if (heap_initialized) kvs_vector_topk_heap_destroy(&heap);
    return ret;
}

static int hnswlib_ops_rebuild(kvs_vector_index_t *index) {
    kvs_vector_store_t *store;
    size_t i;

    if (!index || !index->impl) return KVS_VECTOR_ERR_INVALID_ARG;
    if (kvs_hnswlib_index_reset((kvs_hnswlib_index_t *)index->impl) != 0)
        return KVS_VECTOR_ERR;

    store = &g_vector_state.store;
    for (i = 0; i < store->capacity; ++i) {
        kvs_vector_record_t *rec = &store->records[i];
        if (rec->state != KVS_VECTOR_RECORD_ACTIVE || rec->deleted) continue;
        if (!rec->embedding || rec->embedding_dim <= 0) continue;
        if (kvs_hnswlib_index_add((kvs_hnswlib_index_t *)index->impl,
                i, rec->embedding, rec->embedding_dim) != 0)
            return KVS_VECTOR_ERR;
    }
    return KVS_VECTOR_OK;
}

static int hnswlib_ops_info(kvs_vector_index_t *index, kvs_vector_info_t *info) {
    kvs_hnswlib_index_stats_t stats;

    if (!index || !info) return KVS_VECTOR_ERR_INVALID_ARG;
    info->metric_type = (int)index->metric;
    memset(&stats, 0, sizeof(stats));
    if (kvs_hnswlib_index_get_stats((kvs_hnswlib_index_t *)index->impl, &stats) == 0) {
        info->hnswlib_ntotal = stats.ntotal;
        info->hnswlib_capacity = stats.max_elements;
        info->hnswlib_m = stats.M;
        info->hnswlib_ef_construction = stats.ef_construction;
        info->hnswlib_ef_search = stats.ef_search;
    } else {
        info->hnswlib_ntotal = kvs_hnswlib_index_ntotal(
            (kvs_hnswlib_index_t *)index->impl);
    }
    info->hnswlib_add_count = g_hnswlib_add_count;
    info->hnswlib_search_count = g_hnswlib_search_count;
    return KVS_VECTOR_OK;
}

static void hnswlib_ops_destroy(kvs_vector_index_t *index) {
    if (!index || !index->impl) return;
    kvs_hnswlib_index_destroy((kvs_hnswlib_index_t *)index->impl);
}

const kvs_vector_index_ops_t *kvs_vector_hnswlib_hnsw_get_ops(void) {
    static const kvs_vector_index_ops_t ops = {
        .add = hnswlib_ops_add,
        .search = hnswlib_ops_search,
        .rebuild = hnswlib_ops_rebuild,
        .info = hnswlib_ops_info,
        .destroy = hnswlib_ops_destroy,
    };
    return &ops;
}

void *kvs_vector_hnswlib_create(size_t dim, size_t max_elements, size_t M, size_t ef_construction) {
    return kvs_hnswlib_index_create(dim, max_elements, M, ef_construction);
}

void kvs_vector_hnswlib_destroy_handle(void *handle) {
    if (handle) kvs_hnswlib_index_destroy((kvs_hnswlib_index_t *)handle);
}

void kvs_vector_hnswlib_set_ef_handle(void *handle, size_t ef) {
    if (handle) kvs_hnswlib_index_set_ef((kvs_hnswlib_index_t *)handle, ef);
}

/* Strong function pointer definitions override weak defaults in kvs_vector.c */
const kvs_vector_index_ops_t *(*kvs_vector_hnswlib_get_ops_ptr)(void) = kvs_vector_hnswlib_hnsw_get_ops;
void *(*kvs_vector_hnswlib_create_ptr)(size_t, size_t, size_t, size_t) = kvs_vector_hnswlib_create;
void (*kvs_vector_hnswlib_destroy_ptr)(void *) = kvs_vector_hnswlib_destroy_handle;
size_t (*kvs_vector_hnswlib_ntotal_ptr)(const void *) = (size_t (*)(const void *))kvs_hnswlib_index_ntotal;
void (*kvs_vector_hnswlib_set_ef_ptr)(void *, size_t) = kvs_vector_hnswlib_set_ef_handle;
