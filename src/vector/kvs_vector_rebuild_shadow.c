#define KVS_VECTOR_INTERNAL
#include "vector/kvs_vector_rebuild_shadow.h"

#include "kvs_memory.h"
#include "vector/kvs_vector_hnsw_v2.h"
#include "vector/kvs_vector_manager.h"

#include <string.h>

typedef struct kvs_vector_rebuild_shadow_v2_stats_s {
	size_t missing_delete_count;
	size_t replayed_delta_count;
} kvs_vector_rebuild_shadow_v2_stats_t;

static kvs_vector_rebuild_shadow_v2_stats_t g_shadow_v2_stats;

static void kvs_vector_rebuild_shadow_v2_destroy_index(void *shadow_index) {
	kvs_hnsw_v2_index_t *index = (kvs_hnsw_v2_index_t *)shadow_index;

	if (!index) {
		return;
	}

	kvs_hnsw_v2_index_destroy(index);
	kvs_free(index);
}

static kvs_hnsw_v2_index_t *kvs_vector_rebuild_shadow_v2_index(void) {
	return (kvs_hnsw_v2_index_t *)g_vector_state.rebuild.shadow_index;
}

static kvs_hnsw_v2_index_t *kvs_vector_rebuild_active_v2_index(void) {
	if (g_vector_state.rebuild.active_destroy_fn !=
	    kvs_vector_rebuild_shadow_v2_destroy_index) {
		return NULL;
	}

	return (kvs_hnsw_v2_index_t *)g_vector_state.rebuild.active_index;
}

static int kvs_vector_rebuild_shadow_v2_fail(
	int error_code,
	const char *message
) {
	int ret;

	if (g_vector_state.rebuild.state == KVS_VECTOR_REBUILD_IDLE ||
	    g_vector_state.rebuild.state == KVS_VECTOR_REBUILD_FAILED ||
	    g_vector_state.rebuild.state == KVS_VECTOR_REBUILD_CANCELLED) {
		return error_code;
	}

	ret = kvs_vector_rebuild_fail(
		&g_vector_state.rebuild,
		error_code,
		message
	);
	return ret == KVS_VECTOR_OK ? error_code : ret;
}

static int kvs_vector_rebuild_shadow_v2_build_cb(
	uint64_t record_id,
	const float *embedding,
	int embedding_dim,
	void *arg
) {
	kvs_hnsw_v2_index_t *index = (kvs_hnsw_v2_index_t *)arg;

	if (!index || !embedding || embedding_dim <= 0) {
		return KVS_VECTOR_ERR_INVALID_ARG;
	}

	return kvs_hnsw_v2_index_add(
		index,
		record_id,
		embedding,
		(size_t)embedding_dim
	);
}

int kvs_vector_rebuild_shadow_v2_init_for_test(void) {
	kvs_hnsw_v2_index_t *index;
	size_t dim;
	size_t capacity;
	int ret;

	if (!g_vector_state.initialized) {
		return KVS_VECTOR_ERR_NOT_INITIALIZED;
	}
	if (g_vector_state.rebuild.state != KVS_VECTOR_REBUILD_IDLE) {
		return KVS_VECTOR_ERR;
	}

	index = (kvs_hnsw_v2_index_t *)kvs_malloc(sizeof(*index));
	if (!index) {
		return KVS_VECTOR_ERR;
	}
	memset(index, 0, sizeof(*index));

	dim = kvs_vector_embedding_provider_dimension();
	if (dim == 0) {
		dim = KVS_VECTOR_DEFAULT_DIMENSION;
	}

	ret = kvs_hnsw_v2_index_init(index, dim, g_vector_state.metric_type);
	if (ret != KVS_VECTOR_OK) {
		kvs_free(index);
		return ret;
	}

	capacity = g_vector_state.store.capacity +
		g_vector_state.rebuild.delta_count + 16;
	if (capacity < g_vector_state.store.capacity * 2) {
		capacity = g_vector_state.store.capacity * 2;
	}
	if (capacity == 0) {
		capacity = 16;
	}

	ret = kvs_hnsw_v2_index_reserve(index, capacity);
	if (ret != KVS_VECTOR_OK) {
		kvs_hnsw_v2_index_destroy(index);
		kvs_free(index);
		return ret;
	}

	ret = kvs_vector_rebuild_begin(
		&g_vector_state.rebuild,
		&g_vector_state.current_index,
		index
	);
	if (ret != KVS_VECTOR_OK) {
		kvs_hnsw_v2_index_destroy(index);
		kvs_free(index);
		return ret;
	}
	g_vector_state.rebuild.shadow_destroy_fn =
		kvs_vector_rebuild_shadow_v2_destroy_index;
	g_shadow_v2_stats.missing_delete_count = 0;
	g_shadow_v2_stats.replayed_delta_count = 0;
	return KVS_VECTOR_OK;
}

int kvs_vector_rebuild_shadow_v2_destroy_for_test(void) {
	if (!g_vector_state.initialized) {
		return KVS_VECTOR_ERR_NOT_INITIALIZED;
	}

	if (g_vector_state.rebuild.state == KVS_VECTOR_REBUILD_BUILDING ||
	    g_vector_state.rebuild.state == KVS_VECTOR_REBUILD_REPLAYING_DELTA ||
	    g_vector_state.rebuild.state == KVS_VECTOR_REBUILD_READY_TO_SWAP ||
	    g_vector_state.rebuild.state == KVS_VECTOR_REBUILD_SWAPPING) {
		(void)kvs_vector_rebuild_cancel(&g_vector_state.rebuild);
	}

	if (g_vector_state.rebuild.state == KVS_VECTOR_REBUILD_FAILED ||
	    g_vector_state.rebuild.state == KVS_VECTOR_REBUILD_CANCELLED) {
		return kvs_vector_rebuild_reset_idle(&g_vector_state.rebuild);
	}

	if (g_vector_state.rebuild.shadow_index &&
	    g_vector_state.rebuild.shadow_destroy_fn) {
		g_vector_state.rebuild.shadow_destroy_fn(
			g_vector_state.rebuild.shadow_index
		);
		g_vector_state.rebuild.shadow_index = NULL;
		g_vector_state.rebuild.shadow_destroy_fn = NULL;
	}
	g_shadow_v2_stats.missing_delete_count = 0;
	g_shadow_v2_stats.replayed_delta_count = 0;
	return kvs_vector_rebuild_reset_idle(&g_vector_state.rebuild);
}

int kvs_vector_rebuild_shadow_v2_build_from_active_records_for_test(void) {
	kvs_hnsw_v2_index_t *index;
	int ret;

	if (!g_vector_state.initialized) {
		return KVS_VECTOR_ERR_NOT_INITIALIZED;
	}
	if (g_vector_state.rebuild.state != KVS_VECTOR_REBUILD_BUILDING) {
		return KVS_VECTOR_ERR;
	}

	index = kvs_vector_rebuild_shadow_v2_index();
	if (!index) {
		return kvs_vector_rebuild_shadow_v2_fail(
			KVS_VECTOR_ERR_NOT_INITIALIZED,
			"shadow v2 index missing"
		);
	}

	ret = kvs_vector_manager_for_each_active_embedding(
		kvs_vector_rebuild_shadow_v2_build_cb,
		index
	);
	if (ret != KVS_VECTOR_OK) {
		return kvs_vector_rebuild_shadow_v2_fail(
			ret,
			"shadow baseline build failed"
		);
	}

	g_vector_state.rebuild.progress_done = g_vector_state.store.active_records;
	g_vector_state.rebuild.progress_total = g_vector_state.store.active_records;
	return kvs_vector_rebuild_enter_replaying_delta(&g_vector_state.rebuild);
}

static int kvs_vector_rebuild_shadow_v2_replay_upsert(
	kvs_hnsw_v2_index_t *index,
	const kvs_vector_rebuild_delta_entry_t *entry
) {
	uint32_t slot;
	int ret;

	if (!entry->vector || entry->dim == 0) {
		return KVS_VECTOR_ERR_INVALID_ARG;
	}

	ret = kvs_vector_id_slot_map_get(
		&index->record_id_to_slot,
		entry->record_id,
		&slot
	);
	if (ret == KVS_VECTOR_OK) {
		ret = kvs_hnsw_v2_index_mark_delete(index, entry->record_id);
		if (ret != KVS_VECTOR_OK) {
			return ret;
		}
	} else if (ret != KVS_VECTOR_ERR_NOT_FOUND) {
		return ret;
	}

	return kvs_hnsw_v2_index_add(
		index,
		entry->record_id,
		entry->vector,
		entry->dim
	);
}

static int kvs_vector_rebuild_shadow_v2_replay_delete(
	kvs_hnsw_v2_index_t *index,
	const kvs_vector_rebuild_delta_entry_t *entry
) {
	int ret;

	ret = kvs_hnsw_v2_index_mark_delete(index, entry->record_id);
	if (ret == KVS_VECTOR_ERR_NOT_FOUND) {
		g_shadow_v2_stats.missing_delete_count++;
		return KVS_VECTOR_OK;
	}

	return ret;
}

int kvs_vector_rebuild_shadow_v2_replay_deltas_for_test(void) {
	kvs_hnsw_v2_index_t *index;
	size_t i;
	uint64_t previous_generation = 0;
	int ret;

	if (!g_vector_state.initialized) {
		return KVS_VECTOR_ERR_NOT_INITIALIZED;
	}
	if (g_vector_state.rebuild.state != KVS_VECTOR_REBUILD_REPLAYING_DELTA) {
		return KVS_VECTOR_ERR;
	}

	index = kvs_vector_rebuild_shadow_v2_index();
	if (!index) {
		return kvs_vector_rebuild_shadow_v2_fail(
			KVS_VECTOR_ERR_NOT_INITIALIZED,
			"shadow v2 index missing"
		);
	}

	for (i = 0; i < g_vector_state.rebuild.delta_count; i++) {
		const kvs_vector_rebuild_delta_entry_t *entry =
			&g_vector_state.rebuild.delta_log[i];

		if (i > 0 && entry->generation <= previous_generation) {
			return kvs_vector_rebuild_shadow_v2_fail(
				KVS_VECTOR_ERR,
				"delta generation order violation"
			);
		}
		previous_generation = entry->generation;

		if (entry->op == KVS_VECTOR_REBUILD_DELTA_UPSERT) {
			ret = kvs_vector_rebuild_shadow_v2_replay_upsert(index, entry);
		} else if (entry->op == KVS_VECTOR_REBUILD_DELTA_DELETE) {
			ret = kvs_vector_rebuild_shadow_v2_replay_delete(index, entry);
		} else {
			ret = KVS_VECTOR_ERR_INVALID_ARG;
		}

		if (ret != KVS_VECTOR_OK) {
			return kvs_vector_rebuild_shadow_v2_fail(
				ret,
				"shadow delta replay failed"
			);
		}
		g_shadow_v2_stats.replayed_delta_count++;
	}

	return kvs_vector_rebuild_enter_ready_to_swap(&g_vector_state.rebuild);
}

int kvs_vector_rebuild_shadow_v2_search_for_test(
	const float *query,
	size_t dim,
	size_t topk,
	size_t ef_search,
	kvs_vector_hit_t *hits,
	size_t *out_count
) {
	kvs_hnsw_v2_index_t *index;

	if (!g_vector_state.initialized) {
		return KVS_VECTOR_ERR_NOT_INITIALIZED;
	}

	index = kvs_vector_rebuild_shadow_v2_index();
	if (!index) {
		return KVS_VECTOR_ERR_NOT_INITIALIZED;
	}

	return kvs_hnsw_v2_index_search(
		index,
		query,
		dim,
		topk,
		ef_search,
		hits,
		out_count
	);
}

int kvs_vector_rebuild_shadow_v2_info_for_test(kvs_vector_core_info_t *out) {
	kvs_hnsw_v2_index_t *index;

	if (!g_vector_state.initialized) {
		return KVS_VECTOR_ERR_NOT_INITIALIZED;
	}

	index = kvs_vector_rebuild_shadow_v2_index();
	if (!index) {
		return KVS_VECTOR_ERR_NOT_INITIALIZED;
	}

	return kvs_hnsw_v2_index_info(index, out);
}

int kvs_vector_rebuild_shadow_v2_active_record_count_for_test(size_t *out_count) {
	if (!g_vector_state.initialized) {
		return KVS_VECTOR_ERR_NOT_INITIALIZED;
	}
	if (!out_count) {
		return KVS_VECTOR_ERR_INVALID_ARG;
	}

	*out_count = g_vector_state.store.active_records;
	return KVS_VECTOR_OK;
}

int kvs_vector_rebuild_shadow_v2_missing_delete_count_for_test(size_t *out_count) {
	if (!out_count) {
		return KVS_VECTOR_ERR_INVALID_ARG;
	}

	*out_count = g_shadow_v2_stats.missing_delete_count;
	return KVS_VECTOR_OK;
}

int kvs_vector_rebuild_shadow_v2_replayed_delta_count_for_test(size_t *out_count) {
	if (!out_count) {
		return KVS_VECTOR_ERR_INVALID_ARG;
	}

	*out_count = g_shadow_v2_stats.replayed_delta_count;
	return KVS_VECTOR_OK;
}

int kvs_vector_rebuild_shadow_v2_shadow_exists_for_test(void) {
	return g_vector_state.rebuild.shadow_index != NULL;
}

int kvs_vector_rebuild_swap_ready_shadow_for_test(void) {
	if (!g_vector_state.initialized) {
		return KVS_VECTOR_ERR_NOT_INITIALIZED;
	}

	return kvs_vector_rebuild_promote_shadow(&g_vector_state.rebuild);
}

int kvs_vector_rebuild_retire_cleanup_for_test(void) {
	if (!g_vector_state.initialized) {
		return KVS_VECTOR_ERR_NOT_INITIALIZED;
	}

	return kvs_vector_rebuild_retire_cleanup(&g_vector_state.rebuild);
}

int kvs_vector_rebuild_force_swap_failure_for_test(void) {
	int ret;

	if (!g_vector_state.initialized) {
		return KVS_VECTOR_ERR_NOT_INITIALIZED;
	}

	if (g_vector_state.rebuild.state != KVS_VECTOR_REBUILD_READY_TO_SWAP) {
		return KVS_VECTOR_ERR;
	}

	ret = kvs_vector_rebuild_enter_swapping(&g_vector_state.rebuild);
	if (ret != KVS_VECTOR_OK) {
		return ret;
	}

	return kvs_vector_rebuild_fail(
		&g_vector_state.rebuild,
		KVS_VECTOR_ERR,
		"forced pre-publication swap failure"
	);
}

int kvs_vector_rebuild_shadow_v2_active_exists_for_test(void) {
	return g_vector_state.rebuild.active_index != NULL;
}

int kvs_vector_rebuild_shadow_v2_retired_exists_for_test(void) {
	return g_vector_state.rebuild.retired_index != NULL;
}

int kvs_vector_rebuild_shadow_v2_active_search_for_test(
	const float *query,
	size_t dim,
	size_t topk,
	size_t ef_search,
	kvs_vector_hit_t *hits,
	size_t *out_count
) {
	kvs_hnsw_v2_index_t *index;

	if (!g_vector_state.initialized) {
		return KVS_VECTOR_ERR_NOT_INITIALIZED;
	}

	index = kvs_vector_rebuild_active_v2_index();
	if (!index) {
		return KVS_VECTOR_ERR_NOT_INITIALIZED;
	}

	return kvs_hnsw_v2_index_search(
		index,
		query,
		dim,
		topk,
		ef_search,
		hits,
		out_count
	);
}
