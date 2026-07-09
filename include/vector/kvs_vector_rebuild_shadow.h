#pragma once

#include <stddef.h>
#include <stdint.h>

#include "kvs_vector.h"
#include "vector/kvs_vector_core.h"

int kvs_vector_rebuild_shadow_v2_init(void);
int kvs_vector_rebuild_shadow_v2_destroy(void);
int kvs_vector_rebuild_shadow_v2_build_from_active_records(void);
int kvs_vector_rebuild_shadow_v2_replay_deltas(void);
int kvs_vector_rebuild_publish_ready_shadow(void);
int kvs_vector_rebuild_retire_cleanup_safe(void);

int kvs_vector_rebuild_shadow_v2_init_for_test(void);
int kvs_vector_rebuild_shadow_v2_destroy_for_test(void);
int kvs_vector_rebuild_shadow_v2_build_from_active_records_for_test(void);
int kvs_vector_rebuild_shadow_v2_replay_deltas_for_test(void);
int kvs_vector_rebuild_shadow_v2_search_for_test(
	const float *query,
	size_t dim,
	size_t topk,
	size_t ef_search,
	kvs_vector_hit_t *hits,
	size_t *out_count
);
int kvs_vector_rebuild_shadow_v2_info_for_test(kvs_vector_core_info_t *out);
int kvs_vector_rebuild_shadow_v2_active_record_count_for_test(size_t *out_count);
int kvs_vector_rebuild_shadow_v2_missing_delete_count_for_test(size_t *out_count);
int kvs_vector_rebuild_shadow_v2_replayed_delta_count_for_test(size_t *out_count);
int kvs_vector_rebuild_shadow_v2_shadow_exists_for_test(void);
int kvs_vector_rebuild_swap_ready_shadow_for_test(void);
int kvs_vector_rebuild_retire_cleanup_for_test(void);
int kvs_vector_rebuild_force_swap_failure_for_test(void);
int kvs_vector_rebuild_shadow_v2_active_exists_for_test(void);
int kvs_vector_rebuild_shadow_v2_retired_exists_for_test(void);
int kvs_vector_rebuild_shadow_v2_active_search_for_test(
	const float *query,
	size_t dim,
	size_t topk,
	size_t ef_search,
	kvs_vector_hit_t *hits,
	size_t *out_count
);
