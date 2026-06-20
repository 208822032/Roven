#ifndef KVS_VECTOR_FAISS_BACKEND_H
#define KVS_VECTOR_FAISS_BACKEND_H

/*
 * Faiss benchmark-only index backend — C ABI wrapper.
 *
 * This is NOT a production path.  It exists solely to provide a
 * same-infrastructure performance baseline so that KVLite native
 * HNSW can be compared against Faiss HNSW while sharing the
 * identical record table, id_map, worker pool, executor, and
 * RESP response path.
 *
 * Compile-time guard: only available when ENABLE_FAISS_BASELINE=1.
 * Default builds do not link Faiss.
 */

#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/* Opaque handle — points to a Faiss index object (C++). */
typedef struct kvs_faiss_index_s kvs_faiss_index_t;

/*
 * Supported Faiss index types for the C ABI.
 */
typedef enum {
	KVS_FAISS_INDEX_FLAT_IP  = 0,
	KVS_FAISS_INDEX_HNSW     = 1,
} kvs_faiss_index_kind_t;

/*
 * Create a Faiss index.
 *
 *   kind   — KVS_FAISS_INDEX_FLAT_IP or KVS_FAISS_INDEX_HNSW
 *   dim    — vector dimension
 *   M      — HNSW M (ignored for flat)
 *   ef_construction — HNSW efConstruction (ignored for flat)
 *
 * Returns NULL on failure.
 */
kvs_faiss_index_t *kvs_faiss_index_create(
	int kind,
	size_t dim,
	size_t M,
	size_t ef_construction
);

/*
 * Add a vector to the index.
 *
 *   index        — opaque handle
 *   record_index — KVLite record index (used as Faiss id)
 *   embedding    — float vector data
 *   embedding_dim— vector dimension
 *
 * Returns 0 on success, non-zero on failure.
 */
int kvs_faiss_index_add(
	kvs_faiss_index_t *index,
	size_t record_index,
	const float *embedding,
	int embedding_dim
);

/*
 * Search the index for the top-k nearest neighbours.
 *
 *   query_embedding — query vector
 *   query_dim       — vector dimension
 *   topk            — number of results to return
 *   out_record_indices — output array (caller allocates, size >= topk)
 *   out_scores      — output array (caller allocates, size >= topk)
 *
 * Returns the actual number of results found (0 .. topk).
 * out_record_indices and out_scores are filled in descending score order.
 */
int kvs_faiss_index_search(
	kvs_faiss_index_t *index,
	const float *query_embedding,
	int query_dim,
	int topk,
	size_t *out_record_indices,
	float *out_scores
);

/*
 * Reset the index — removes all vectors.
 * Returns 0 on success.
 */
int kvs_faiss_index_reset(kvs_faiss_index_t *index);

/*
 * Destroy the index and free all resources.
 */
void kvs_faiss_index_destroy(kvs_faiss_index_t *index);

/*
 * Return the total number of vectors currently in the index.
 */
size_t kvs_faiss_index_ntotal(const kvs_faiss_index_t *index);

#ifdef __cplusplus
}
#endif

#endif /* KVS_VECTOR_FAISS_BACKEND_H */
