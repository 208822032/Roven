#ifndef KVS_VECTOR_HNSWLIB_BACKEND_H
#define KVS_VECTOR_HNSWLIB_BACKEND_H

#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef struct kvs_hnswlib_index_s kvs_hnswlib_index_t;

typedef struct kvs_hnswlib_index_stats_s {
	size_t ntotal;
	size_t max_elements;
	size_t M;
	size_t ef_construction;
	size_t ef_search;
} kvs_hnswlib_index_stats_t;

kvs_hnswlib_index_t *kvs_hnswlib_index_create(
	size_t dim,
	size_t max_elements,
	size_t M,
	size_t ef_construction
);

int kvs_hnswlib_index_add(
	kvs_hnswlib_index_t *index,
	size_t record_index,
	const float *embedding,
	int embedding_dim
);

/*
 * Search for topk nearest neighbours.  Returns the actual number of
 * results (0..topk).  out_labels and out_scores are filled in
 * closer-first order with scores in KVLite "larger-is-better" convention.
 */
int kvs_hnswlib_index_search(
	kvs_hnswlib_index_t *index,
	const float *query_embedding,
	int query_dim,
	int topk,
	size_t *out_labels,
	float *out_scores
);

int kvs_hnswlib_index_reset(kvs_hnswlib_index_t *index);

void kvs_hnswlib_index_destroy(kvs_hnswlib_index_t *index);

size_t kvs_hnswlib_index_ntotal(const kvs_hnswlib_index_t *index);

size_t kvs_hnswlib_index_get_max_elements(const kvs_hnswlib_index_t *index);

int kvs_hnswlib_index_get_stats(
	const kvs_hnswlib_index_t *index,
	kvs_hnswlib_index_stats_t *stats
);

void kvs_hnswlib_index_set_ef(kvs_hnswlib_index_t *index, size_t ef);

int kvs_hnswlib_index_resize(kvs_hnswlib_index_t *index, size_t new_max_elements);

#ifdef __cplusplus
}
#endif

#endif /* KVS_VECTOR_HNSWLIB_BACKEND_H */
