#include "vector/kvs_vector_hnsw.h"

size_t kvs_hnsw_graph_level0_bytes(
	size_t capacity,
	size_t m0
) {
	return capacity * m0 * sizeof(uint32_t);
}
