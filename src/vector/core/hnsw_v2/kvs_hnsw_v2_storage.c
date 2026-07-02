#include "vector/kvs_vector_hnsw_v2.h"

/*
 * Stage 9B keeps storage allocation inside kvs_hnsw_v2_index.c so lifecycle
 * behavior is visible in one place. This translation unit reserves the v2
 * storage boundary for later graph/add stages without registering a backend.
 */
