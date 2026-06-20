/*
 * Faiss benchmark-only index backend — ops wrappers (C file).
 *
 * Compiled only when ENABLE_FAISS_BASELINE=1.
 * This file is separate from kvs_vector.c so that the Faiss symbols
 * do not leak into libkvstore.a and force C++ linkage on test binaries.
 * It is linked directly into the kvstore binary.
 */

#define KVS_VECTOR_INTERNAL
#include "kvs_vector.h"
#include "kvs_vector_faiss_backend.h"

/* Per-process counters exported from kvs_vector.c */
extern size_t g_faiss_add_count;
extern size_t g_faiss_search_count;

/* ── Ops wrappers ─────────────────────────────────────────────────────────── */

int kvs_vector_faiss_hnsw_ops_add(
    kvs_vector_index_t *index,
    size_t record_index,
    const float *embedding,
    int embedding_dim
) {
    if (!index || !index->impl) return KVS_VECTOR_ERR_INVALID_ARG;
    if (kvs_faiss_index_add((kvs_faiss_index_t *)index->impl,
            record_index, embedding, embedding_dim) != 0)
        return KVS_VECTOR_ERR;
    __atomic_add_fetch(&g_faiss_add_count, 1, __ATOMIC_RELAXED);
    return KVS_VECTOR_OK;
}

int kvs_vector_faiss_hnsw_ops_search(
    kvs_vector_index_t *index,
    const float *query_embedding,
    int query_dim,
    const kvs_vector_index_search_params_t *params,
    kvs_vector_search_result_t *results,
    size_t results_capacity,
    size_t *results_count
) {
    kvs_vector_topk_heap_t heap;
    size_t limit, i;
    int nfound;
    size_t *faiss_indices = NULL;
    float *faiss_scores = NULL;
    int ret;
    int heap_initialized = 0;
    float effective_threshold;

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

    faiss_indices = (size_t *)kvs_malloc(limit * sizeof(size_t));
    if (!faiss_indices) return KVS_VECTOR_ERR;
    faiss_scores = (float *)kvs_malloc(limit * sizeof(float));
    if (!faiss_scores) { kvs_free(faiss_indices); return KVS_VECTOR_ERR; }

    nfound = kvs_faiss_index_search((kvs_faiss_index_t *)index->impl,
        query_embedding, query_dim, (int)limit, faiss_indices, faiss_scores);
    if (nfound < 0) {
        kvs_free(faiss_scores); kvs_free(faiss_indices);
        return KVS_VECTOR_ERR;
    }
    __atomic_add_fetch(&g_faiss_search_count, 1, __ATOMIC_RELAXED);

    ret = kvs_vector_topk_heap_init(&heap, limit);
    if (ret != KVS_VECTOR_OK) {
        kvs_free(faiss_scores); kvs_free(faiss_indices);
        return ret;
    }
    heap_initialized = 1;

    for (i = 0; i < (size_t)nfound; ++i) {
        size_t record_index = faiss_indices[i];
        kvs_vector_record_t *record;
        float score = faiss_scores[i];

        if (score < effective_threshold) continue;
        if (record_index >= g_vector_state.store.capacity) continue;

        record = &g_vector_state.store.records[record_index];
        if (record->state != KVS_VECTOR_RECORD_ACTIVE || record->deleted)
            continue;
        if (!kvs_vector_record_scope_match(record, params->scope, params->scope_len))
            continue;

        kvs_vector_topk_heap_push(&heap, record_index, score);
    }

    kvs_free(faiss_scores);
    kvs_free(faiss_indices);

    ret = kvs_vector_topk_heap_export_desc(&heap, results, results_capacity,
        results_count, &g_vector_state.store);
    if (heap_initialized) kvs_vector_topk_heap_destroy(&heap);
    return ret;
}

int kvs_vector_faiss_hnsw_ops_rebuild(kvs_vector_index_t *index) {
    kvs_vector_store_t *store;
    size_t i;

    if (!index || !index->impl) return KVS_VECTOR_ERR_INVALID_ARG;
    if (kvs_faiss_index_reset((kvs_faiss_index_t *)index->impl) != 0)
        return KVS_VECTOR_ERR;

    store = &g_vector_state.store;
    for (i = 0; i < store->capacity; ++i) {
        kvs_vector_record_t *rec = &store->records[i];
        if (rec->state != KVS_VECTOR_RECORD_ACTIVE || rec->deleted) continue;
        if (!rec->embedding || rec->embedding_dim <= 0) continue;
        if (kvs_faiss_index_add((kvs_faiss_index_t *)index->impl,
                i, rec->embedding, rec->embedding_dim) != 0)
            return KVS_VECTOR_ERR;
    }
    return KVS_VECTOR_OK;
}

int kvs_vector_faiss_hnsw_ops_info(
    kvs_vector_index_t *index, kvs_vector_info_t *info)
{
    if (!index || !info) return KVS_VECTOR_ERR_INVALID_ARG;
    info->metric_type = (int)index->metric;
    info->faiss_ntotal = kvs_faiss_index_ntotal((kvs_faiss_index_t *)index->impl);
    info->faiss_add_count = g_faiss_add_count;
    info->faiss_search_count = g_faiss_search_count;
    return KVS_VECTOR_OK;
}

void kvs_vector_faiss_hnsw_ops_destroy(kvs_vector_index_t *index) {
    if (!index || !index->impl) return;
    kvs_faiss_index_destroy((kvs_faiss_index_t *)index->impl);
}

const kvs_vector_index_ops_t *kvs_vector_faiss_hnsw_get_ops(void) {
    static const kvs_vector_index_ops_t ops = {
        .add = kvs_vector_faiss_hnsw_ops_add,
        .search = kvs_vector_faiss_hnsw_ops_search,
        .rebuild = kvs_vector_faiss_hnsw_ops_rebuild,
        .info = kvs_vector_faiss_hnsw_ops_info,
        .destroy = kvs_vector_faiss_hnsw_ops_destroy,
    };
    return &ops;
}

void *kvs_vector_faiss_hnsw_create(int kind, size_t dim, size_t M, size_t ef_construction) {
    return kvs_faiss_index_create(kind, dim, M, ef_construction);
}

void kvs_vector_faiss_hnsw_destroy_handle(void *handle) {
    if (handle) kvs_faiss_index_destroy((kvs_faiss_index_t *)handle);
}

/*
 * Function pointer definitions — resolve the extern declarations in kvs_vector.c.
 */
const kvs_vector_index_ops_t *(*kvs_vector_faiss_hnsw_get_ops_ptr)(void) = kvs_vector_faiss_hnsw_get_ops;
void *(*kvs_vector_faiss_hnsw_create_ptr)(int, size_t, size_t, size_t) = kvs_vector_faiss_hnsw_create;
void (*kvs_vector_faiss_hnsw_destroy_ptr)(void *) = kvs_vector_faiss_hnsw_destroy_handle;
size_t (*kvs_vector_faiss_ntotal_ptr)(const void *) = (size_t (*)(const void *))kvs_faiss_index_ntotal;
