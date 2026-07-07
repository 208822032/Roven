#pragma once

#include <stddef.h>

#include "kvs_vector.h"

/*
 * Legacy-compatible backend adapter shell.
 *
 * Current product backends are core_flat and core_hnsw_v2.  This header keeps
 * only the small manager-side ops wrapper still used to bind those backends.
 * Removed native flat, ANN-lite HNSW, SQ8, FAISS, and hnswlib implementations
 * must not be reintroduced here.
 */

typedef struct kvs_vector_index_search_params_s {
	const char *scope;
	size_t scope_len;
	int topk;
	float threshold;
} kvs_vector_index_search_params_t;

struct kvs_vector_index_s;

typedef struct kvs_vector_index_ops_s {
	int (*add)(
		struct kvs_vector_index_s *index,
		size_t record_index,
		const float *embedding,
		int embedding_dim
	);

	int (*search)(
		struct kvs_vector_index_s *index,
		const float *query_embedding,
		int query_dim,
		const kvs_vector_index_search_params_t *params,
		kvs_vector_search_result_t *results,
		size_t results_capacity,
		size_t *results_count
	);

	int (*rebuild)(struct kvs_vector_index_s *index);

	int (*info)(
		struct kvs_vector_index_s *index,
		kvs_vector_info_t *info
	);

	void (*destroy)(struct kvs_vector_index_s *index);
} kvs_vector_index_ops_t;

typedef struct kvs_vector_index_s {
	kvs_vector_index_type_t type;
	kvs_vector_metric_type_t metric;
	size_t dimension;
	void *impl;
	const kvs_vector_index_ops_t *ops;
} kvs_vector_index_t;

float kvs_vector_metric_score(
	const float *lhs,
	const float *rhs,
	size_t dim,
	kvs_vector_metric_type_t metric
);

const char *kvs_vector_metric_name(kvs_vector_metric_type_t metric);

kvs_vector_metric_type_t kvs_vector_metric_from_config(void);

int kvs_vector_index_bind_current(void);

int kvs_vector_index_add_current(
	size_t record_index,
	const float *embedding,
	int embedding_dim
);

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
);

int kvs_vector_index_rebuild_current(void);

size_t kvs_vector_estimate_record_memory_bytes(void);

size_t kvs_vector_estimate_current_index_memory_bytes(void);
