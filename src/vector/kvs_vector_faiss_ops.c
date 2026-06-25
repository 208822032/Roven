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

typedef struct faiss_rebuild_ctx_s {
    kvs_faiss_index_t *faiss;
} faiss_rebuild_ctx_t;

static int faiss_rebuild_add_record(
    uint64_t record_id,
    const float *embedding,
    int embedding_dim,
    void *arg
) {
    faiss_rebuild_ctx_t *ctx = (faiss_rebuild_ctx_t *)arg;

    if (!ctx || !ctx->faiss) {
        return KVS_VECTOR_ERR_INVALID_ARG;
    }

    if (kvs_faiss_index_add(ctx->faiss,
            (size_t)record_id, embedding, embedding_dim) != 0)
        return KVS_VECTOR_ERR;

    return KVS_VECTOR_OK;
}

int kvs_vector_faiss_hnsw_ops_add(
    kvs_vector_index_t *index,
    size_t record_index,
    const float *embedding,
    int embedding_dim
) {
    uint64_t record_id;
    int ret;

    if (!index || !index->impl) return KVS_VECTOR_ERR_INVALID_ARG;

    ret = kvs_vector_manager_get_active_record_id_by_index(record_index, &record_id);
    if (ret != KVS_VECTOR_OK)
        return ret;

    if (kvs_faiss_index_add((kvs_faiss_index_t *)index->impl,
            (size_t)record_id, embedding, embedding_dim) != 0)
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
    size_t limit, i;
    int nfound;
    size_t *faiss_indices = NULL;
    float *faiss_scores = NULL;
    kvs_vector_hit_t *hits = NULL;
    size_t hit_count = 0;
    int ret;
    float effective_threshold;

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
    hits = (kvs_vector_hit_t *)kvs_malloc(limit * sizeof(kvs_vector_hit_t));
    if (!hits) {
        kvs_free(faiss_scores);
        kvs_free(faiss_indices);
        return KVS_VECTOR_ERR;
    }

    nfound = kvs_faiss_index_search((kvs_faiss_index_t *)index->impl,
        query_embedding, query_dim, (int)limit, faiss_indices, faiss_scores);
    if (nfound < 0) {
        kvs_free(hits);
        kvs_free(faiss_scores);
        kvs_free(faiss_indices);
        return KVS_VECTOR_ERR;
    }
    __atomic_add_fetch(&g_faiss_search_count, 1, __ATOMIC_RELAXED);

    for (i = 0; i < (size_t)nfound; ++i) {
        hits[hit_count].record_id = (uint64_t)faiss_indices[i];
        hits[hit_count].score = faiss_scores[i];
        hit_count++;
    }

    kvs_free(faiss_scores);
    kvs_free(faiss_indices);

    ret = kvs_vector_manager_resolve_hits(hits, hit_count, params->scope,
        params->scope_len, effective_threshold, results, results_capacity,
        results_count);
    kvs_free(hits);
    return ret;
}

int kvs_vector_faiss_hnsw_ops_rebuild(kvs_vector_index_t *index) {
    faiss_rebuild_ctx_t ctx;

    if (!index || !index->impl) return KVS_VECTOR_ERR_INVALID_ARG;
    if (kvs_faiss_index_reset((kvs_faiss_index_t *)index->impl) != 0)
        return KVS_VECTOR_ERR;

    ctx.faiss = (kvs_faiss_index_t *)index->impl;
    return kvs_vector_manager_for_each_active_embedding(faiss_rebuild_add_record, &ctx);
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
