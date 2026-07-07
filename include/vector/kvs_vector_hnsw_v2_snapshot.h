#pragma once

#include <stddef.h>

#include "vector/kvs_vector_hnsw_v2.h"

int kvs_hnsw_v2_index_serialize_file(
	const kvs_hnsw_v2_index_t *index,
	const char *path
);

int kvs_hnsw_v2_index_deserialize_file(
	kvs_hnsw_v2_index_t *index,
	const char *path
);

int kvs_hnsw_v2_index_validate_basic(
	const kvs_hnsw_v2_index_t *index,
	char *reason,
	size_t reason_size
);
