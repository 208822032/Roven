#define KVS_VECTOR_INTERNAL
#include "vector/kvs_vector_rebuild_shadow.h"

#include "kvs_memory.h"
#include "vector/kvs_vector_hnsw_v2.h"
#include "vector/kvs_vector_hnsw_v2_snapshot.h"
#include "vector/kvs_vector_manager.h"

#include <stdint.h>
#include <stdio.h>
#include <string.h>

typedef struct kvs_vector_rebuild_shadow_v2_summary_s {
	uint64_t active_count;
	uint64_t deleted_count;
	uint64_t record_generation;
	uint64_t record_id_checksum;
	uint64_t vector_checksum;
} kvs_vector_rebuild_shadow_v2_summary_t;

typedef struct kvs_vector_rebuild_shadow_v2_stats_s {
	size_t missing_delete_count;
	size_t replayed_delta_count;
} kvs_vector_rebuild_shadow_v2_stats_t;

static kvs_vector_rebuild_shadow_v2_stats_t g_shadow_v2_stats;

static void kvs_vector_rebuild_shadow_v2_set_reason(
	char *reason,
	size_t reason_size,
	const char *message
) {
	if (!reason || reason_size == 0) {
		return;
	}
	snprintf(reason, reason_size, "%s", message ? message : "");
}

static uint64_t kvs_vector_rebuild_shadow_v2_fnv_update(
	uint64_t hash,
	const void *data,
	size_t len
) {
	const unsigned char *bytes = (const unsigned char *)data;
	size_t i;

	if (!bytes && len > 0) {
		return hash;
	}
	for (i = 0; i < len; i++) {
		hash ^= (uint64_t)bytes[i];
		hash *= 1099511628211ull;
	}
	return hash;
}

static uint64_t kvs_vector_rebuild_shadow_v2_fnv_update_u64(
	uint64_t hash,
	uint64_t value
) {
	unsigned char bytes[8];
	size_t i;

	for (i = 0; i < sizeof(bytes); i++) {
		bytes[i] = (unsigned char)((value >> (i * 8)) & 0xffu);
	}
	return kvs_vector_rebuild_shadow_v2_fnv_update(hash, bytes, sizeof(bytes));
}

static uint64_t kvs_vector_rebuild_shadow_v2_fnv_update_u32(
	uint64_t hash,
	uint32_t value
) {
	unsigned char bytes[4];
	size_t i;

	for (i = 0; i < sizeof(bytes); i++) {
		bytes[i] = (unsigned char)((value >> (i * 8)) & 0xffu);
	}
	return kvs_vector_rebuild_shadow_v2_fnv_update(hash, bytes, sizeof(bytes));
}

static uint64_t kvs_vector_rebuild_shadow_v2_record_id_fingerprint(
	uint64_t record_id
) {
	uint64_t hash = 1469598103934665603ull;

	return kvs_vector_rebuild_shadow_v2_fnv_update_u64(hash, record_id);
}

static uint64_t kvs_vector_rebuild_shadow_v2_vector_fingerprint(
	uint64_t record_id,
	const float *vector,
	size_t dim
) {
	uint64_t hash = 1469598103934665603ull;

	hash = kvs_vector_rebuild_shadow_v2_fnv_update_u64(hash, record_id);
	hash = kvs_vector_rebuild_shadow_v2_fnv_update_u32(hash, (uint32_t)dim);
	hash = kvs_vector_rebuild_shadow_v2_fnv_update(
		hash,
		vector,
		dim * sizeof(float)
	);
	return hash;
}

static void kvs_vector_rebuild_shadow_v2_summary_mix_active(
	kvs_vector_rebuild_shadow_v2_summary_t *summary,
	uint64_t record_id,
	const float *vector,
	size_t dim
) {
	if (!summary) {
		return;
	}

	summary->active_count++;
	summary->record_id_checksum +=
		kvs_vector_rebuild_shadow_v2_record_id_fingerprint(record_id);
	summary->vector_checksum += kvs_vector_rebuild_shadow_v2_vector_fingerprint(
		record_id,
		vector,
		dim
	);
}

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
	if (g_vector_state.rebuild.active_index == &g_vector_state.current_index &&
	    g_vector_state.current_index.impl == &g_vector_state.core_hnsw_v2_index &&
	    g_vector_state.index_type == KVS_VECTOR_INDEX_CORE_HNSW_V2) {
		return &g_vector_state.core_hnsw_v2_index;
	}

	if (g_vector_state.rebuild.active_destroy_fn !=
	    kvs_vector_rebuild_shadow_v2_destroy_index) {
		return NULL;
	}

	return (kvs_hnsw_v2_index_t *)g_vector_state.rebuild.active_index;
}

static int kvs_vector_rebuild_publish_shadow_v2_locked(void) {
	kvs_vector_rebuild_context_t *ctx = &g_vector_state.rebuild;
	kvs_hnsw_v2_index_t *shadow;
	kvs_hnsw_v2_index_t *retired;
	int ret;

	if (ctx->state != KVS_VECTOR_REBUILD_READY_TO_SWAP) {
		return kvs_vector_rebuild_promote_shadow(ctx);
	}
	if (ctx->retired_index) {
		return kvs_vector_rebuild_fail(
			ctx,
			KVS_VECTOR_ERR,
			"publish shadow retired index already present"
		);
	}
	if (ctx->shadow_destroy_fn != kvs_vector_rebuild_shadow_v2_destroy_index) {
		return kvs_vector_rebuild_fail(
			ctx,
			KVS_VECTOR_ERR,
			"publish shadow is not core_hnsw_v2"
		);
	}
	if (g_vector_state.current_index.impl != &g_vector_state.core_hnsw_v2_index ||
	    g_vector_state.current_index.type != KVS_VECTOR_INDEX_CORE_HNSW_V2) {
		return kvs_vector_rebuild_fail(
			ctx,
			KVS_VECTOR_ERR,
			"publish active is not product core_hnsw_v2"
		);
	}

	shadow = (kvs_hnsw_v2_index_t *)ctx->shadow_index;
	if (!shadow) {
		return kvs_vector_rebuild_fail(
			ctx,
			KVS_VECTOR_ERR_NOT_INITIALIZED,
			"publish shadow missing core_hnsw_v2 index"
		);
	}

	retired = (kvs_hnsw_v2_index_t *)kvs_malloc(sizeof(*retired));
	if (!retired) {
		return kvs_vector_rebuild_fail(
			ctx,
			KVS_VECTOR_ERR,
			"publish retired allocation failed"
		);
	}

	ret = kvs_vector_rebuild_enter_swapping(ctx);
	if (ret != KVS_VECTOR_OK) {
		kvs_free(retired);
		return ret;
	}

	*retired = g_vector_state.core_hnsw_v2_index;
	g_vector_state.core_hnsw_v2_index = *shadow;
	memset(shadow, 0, sizeof(*shadow));
	kvs_free(shadow);
	ctx->shadow_index = NULL;
	ctx->shadow_destroy_fn = NULL;

	ret = kvs_vector_rebuild_finish_swap(
		ctx,
		&g_vector_state.current_index,
		retired
	);
	if (ret != KVS_VECTOR_OK) {
		/*
		 * finish_swap cannot fail after enter_swapping, but keep the
		 * product active published and retain retired ownership if it ever
		 * does. Returning the failure makes the partial state observable.
		 */
		ctx->retired_index = retired;
		ctx->retired_destroy_fn = kvs_vector_rebuild_shadow_v2_destroy_index;
		return ret;
	}

	ctx->retired_destroy_fn = kvs_vector_rebuild_shadow_v2_destroy_index;
	return KVS_VECTOR_OK;
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

	if (g_vector_state.rebuild.cancel_requested) {
		return KVS_VECTOR_ERR;
	}
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

static int kvs_vector_rebuild_shadow_v2_collect_manager_summary(
	size_t dim,
	kvs_vector_rebuild_shadow_v2_summary_t *summary,
	char *reason,
	size_t reason_size
) {
	kvs_vector_store_t *store = &g_vector_state.store;
	size_t i;

	if (!summary || dim == 0) {
		kvs_vector_rebuild_shadow_v2_set_reason(
			reason,
			reason_size,
			"invalid manager summary argument"
		);
		return KVS_VECTOR_ERR_INVALID_ARG;
	}

	memset(summary, 0, sizeof(*summary));
	for (i = 0; i < store->capacity; i++) {
		kvs_vector_record_t *record = &store->records[i];

		if (record->state == KVS_VECTOR_RECORD_ACTIVE && !record->deleted) {
			if (!record->embedding ||
			    record->embedding_dim != (int)dim) {
				kvs_vector_rebuild_shadow_v2_set_reason(
					reason,
					reason_size,
					"manager active vector mismatch"
				);
				return KVS_VECTOR_ERR;
			}

			kvs_vector_rebuild_shadow_v2_summary_mix_active(
				summary,
				record->id,
				record->embedding,
				dim
			);
			if (record->generation > summary->record_generation) {
				summary->record_generation = record->generation;
			}
			continue;
		}

		if (record->state == KVS_VECTOR_RECORD_DELETED || record->deleted) {
			summary->deleted_count++;
			if (record->generation > summary->record_generation) {
				summary->record_generation = record->generation;
			}
		}
	}

	if (summary->active_count != (uint64_t)store->active_records ||
	    summary->deleted_count != (uint64_t)store->deleted_records) {
		kvs_vector_rebuild_shadow_v2_set_reason(
			reason,
			reason_size,
			"manager record count mismatch"
		);
		return KVS_VECTOR_ERR;
	}

	kvs_vector_rebuild_shadow_v2_set_reason(reason, reason_size, "ok");
	return KVS_VECTOR_OK;
}

static int kvs_vector_rebuild_shadow_v2_collect_shadow_summary(
	const kvs_hnsw_v2_index_t *index,
	kvs_vector_rebuild_shadow_v2_summary_t *summary,
	char *reason,
	size_t reason_size
) {
	uint64_t deleted_count = 0;
	size_t i;

	if (!index || !summary) {
		kvs_vector_rebuild_shadow_v2_set_reason(
			reason,
			reason_size,
			"invalid shadow summary argument"
		);
		return KVS_VECTOR_ERR_INVALID_ARG;
	}

	memset(summary, 0, sizeof(*summary));
	for (i = 0; i < index->size; i++) {
		kvs_vector_record_t *record = NULL;
		const float *shadow_vector;
		uint64_t record_id;
		size_t vector_bytes;

		if (index->deleted_flags && index->deleted_flags[i]) {
			deleted_count++;
			continue;
		}

		record_id = index->slot_to_record_id[i];
		if (kvs_vector_manager_get_record_by_id(record_id, &record) !=
		    KVS_VECTOR_OK ||
		    !record ||
		    record->state != KVS_VECTOR_RECORD_ACTIVE ||
		    record->deleted) {
			kvs_vector_rebuild_shadow_v2_set_reason(
				reason,
				reason_size,
				"shadow active id not in manager"
			);
			return KVS_VECTOR_ERR;
		}
		if (!record->embedding || record->embedding_dim != (int)index->dim) {
			kvs_vector_rebuild_shadow_v2_set_reason(
				reason,
				reason_size,
				"shadow manager vector mismatch"
			);
			return KVS_VECTOR_ERR;
		}

		shadow_vector = kvs_hnsw_v2_slot_vector(index, (uint32_t)i);
		vector_bytes = index->dim * sizeof(float);
		if (!shadow_vector ||
		    memcmp(shadow_vector, record->embedding, vector_bytes) != 0) {
			kvs_vector_rebuild_shadow_v2_set_reason(
				reason,
				reason_size,
				"shadow vector mismatch"
			);
			return KVS_VECTOR_ERR;
		}

		kvs_vector_rebuild_shadow_v2_summary_mix_active(
			summary,
			record_id,
			shadow_vector,
			index->dim
		);
		if (record->generation > summary->record_generation) {
			summary->record_generation = record->generation;
		}
	}

	summary->deleted_count = deleted_count;
	if (deleted_count != (uint64_t)index->deleted_count) {
		kvs_vector_rebuild_shadow_v2_set_reason(
			reason,
			reason_size,
			"shadow deleted count mismatch"
		);
		return KVS_VECTOR_ERR;
	}

	kvs_vector_rebuild_shadow_v2_set_reason(reason, reason_size, "ok");
	return KVS_VECTOR_OK;
}

static int kvs_vector_rebuild_shadow_v2_summary_equal(
	const kvs_vector_rebuild_shadow_v2_summary_t *lhs,
	const kvs_vector_rebuild_shadow_v2_summary_t *rhs
) {
	return lhs && rhs &&
	       lhs->active_count == rhs->active_count &&
	       lhs->deleted_count == rhs->deleted_count &&
	       lhs->record_generation == rhs->record_generation &&
	       lhs->record_id_checksum == rhs->record_id_checksum &&
	       lhs->vector_checksum == rhs->vector_checksum;
}

static int kvs_vector_rebuild_shadow_v2_find_hit(
	const kvs_vector_hit_t *hits,
	size_t count,
	uint64_t record_id
) {
	size_t i;

	for (i = 0; i < count; i++) {
		if (hits[i].record_id == record_id) {
			return 1;
		}
	}
	return 0;
}

static int kvs_vector_rebuild_shadow_v2_search_sanity(
	kvs_hnsw_v2_index_t *index,
	uint64_t active_count,
	char *reason,
	size_t reason_size
) {
	kvs_vector_store_t *store = &g_vector_state.store;
	size_t checked = 0;
	size_t topk;
	size_t ef_search;
	size_t i;

	if (!index) {
		kvs_vector_rebuild_shadow_v2_set_reason(
			reason,
			reason_size,
			"invalid search sanity argument"
		);
		return KVS_VECTOR_ERR_INVALID_ARG;
	}
	if (active_count == 0) {
		kvs_vector_rebuild_shadow_v2_set_reason(reason, reason_size, "ok");
		return KVS_VECTOR_OK;
	}

	topk = active_count < 10 ? (size_t)active_count : 10;
	ef_search = index->ef_search;
	if (ef_search < topk) {
		ef_search = topk;
	}
	if (ef_search < 32) {
		ef_search = 32;
	}

	for (i = 0; i < store->capacity; i++) {
		kvs_vector_record_t *record = &store->records[i];
		kvs_vector_hit_t hits[10];
		size_t out_count = 0;
		int ret;

		if (record->state != KVS_VECTOR_RECORD_ACTIVE || record->deleted) {
			continue;
		}
		if (!record->embedding || record->embedding_dim != (int)index->dim) {
			kvs_vector_rebuild_shadow_v2_set_reason(
				reason,
				reason_size,
				"search sanity manager vector mismatch"
			);
			return KVS_VECTOR_ERR;
		}

		memset(hits, 0, sizeof(hits));
		ret = kvs_hnsw_v2_index_search(
			index,
			record->embedding,
			index->dim,
			topk,
			ef_search,
			hits,
			&out_count
		);
		if (ret != KVS_VECTOR_OK) {
			kvs_vector_rebuild_shadow_v2_set_reason(
				reason,
				reason_size,
				"shadow search sanity failed"
			);
			return ret;
		}
		if (!kvs_vector_rebuild_shadow_v2_find_hit(
			    hits,
			    out_count,
			    record->id
		    )) {
			kvs_vector_rebuild_shadow_v2_set_reason(
				reason,
				reason_size,
				"shadow search sanity missing active record"
			);
			return KVS_VECTOR_ERR;
		}

		checked++;
		if (checked >= 3) {
			break;
		}
	}

	kvs_vector_rebuild_shadow_v2_set_reason(reason, reason_size, "ok");
	return KVS_VECTOR_OK;
}

static int kvs_vector_rebuild_shadow_v2_validate_final(
	kvs_hnsw_v2_index_t *index,
	char *reason,
	size_t reason_size
) {
	kvs_vector_rebuild_shadow_v2_summary_t manager_before;
	kvs_vector_rebuild_shadow_v2_summary_t manager_after;
	kvs_vector_rebuild_shadow_v2_summary_t shadow;
	char graph_reason[128];
	int ret;

	if (!index) {
		kvs_vector_rebuild_shadow_v2_set_reason(
			reason,
			reason_size,
			"shadow v2 index missing"
		);
		return KVS_VECTOR_ERR_NOT_INITIALIZED;
	}

	graph_reason[0] = '\0';
	ret = kvs_hnsw_v2_index_validate_basic(
		index,
		graph_reason,
		sizeof(graph_reason)
	);
	if (ret != KVS_VECTOR_OK) {
		if (reason && reason_size > 0) {
			snprintf(
				reason,
				reason_size,
				"shadow graph validation failed: %s",
				graph_reason[0] ? graph_reason : "unknown"
			);
		}
		return ret;
	}

	ret = kvs_vector_rebuild_shadow_v2_collect_manager_summary(
		index->dim,
		&manager_before,
		reason,
		reason_size
	);
	if (ret != KVS_VECTOR_OK) {
		return ret;
	}

	ret = kvs_vector_rebuild_shadow_v2_collect_shadow_summary(
		index,
		&shadow,
		reason,
		reason_size
	);
	if (ret != KVS_VECTOR_OK) {
		return ret;
	}

	if (shadow.active_count != manager_before.active_count) {
		kvs_vector_rebuild_shadow_v2_set_reason(
			reason,
			reason_size,
			"shadow active count mismatch"
		);
		return KVS_VECTOR_ERR;
	}
	if (shadow.record_id_checksum != manager_before.record_id_checksum) {
		kvs_vector_rebuild_shadow_v2_set_reason(
			reason,
			reason_size,
			"shadow record_id checksum mismatch"
		);
		return KVS_VECTOR_ERR;
	}
	if (shadow.vector_checksum != manager_before.vector_checksum) {
		kvs_vector_rebuild_shadow_v2_set_reason(
			reason,
			reason_size,
			"shadow vector checksum mismatch"
		);
		return KVS_VECTOR_ERR;
	}

	ret = kvs_vector_rebuild_shadow_v2_search_sanity(
		index,
		manager_before.active_count,
		reason,
		reason_size
	);
	if (ret != KVS_VECTOR_OK) {
		return ret;
	}

	ret = kvs_vector_rebuild_shadow_v2_collect_manager_summary(
		index->dim,
		&manager_after,
		reason,
		reason_size
	);
	if (ret != KVS_VECTOR_OK) {
		return ret;
	}
	if (!kvs_vector_rebuild_shadow_v2_summary_equal(
		    &manager_before,
		    &manager_after
	    )) {
		kvs_vector_rebuild_shadow_v2_set_reason(
			reason,
			reason_size,
			"manager records changed during final validation"
		);
		return KVS_VECTOR_ERR;
	}

	kvs_vector_rebuild_shadow_v2_set_reason(reason, reason_size, "ok");
	return KVS_VECTOR_OK;
}

int kvs_vector_rebuild_shadow_v2_init(void) {
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
	index->M = g_vector_state.core_hnsw_v2_index.M;
	index->M0 = g_vector_state.core_hnsw_v2_index.M0;
	index->ef_construction = g_vector_state.core_hnsw_v2_index.ef_construction;
	index->ef_search = g_vector_state.core_hnsw_v2_index.ef_search;

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

int kvs_vector_rebuild_shadow_v2_init_for_test(void) {
	return kvs_vector_rebuild_shadow_v2_init();
}

int kvs_vector_rebuild_shadow_v2_destroy(void) {
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

int kvs_vector_rebuild_shadow_v2_destroy_for_test(void) {
	return kvs_vector_rebuild_shadow_v2_destroy();
}

int kvs_vector_rebuild_shadow_v2_build_from_active_records(void) {
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
		if (g_vector_state.rebuild.cancel_requested) {
			return kvs_vector_rebuild_cancel(&g_vector_state.rebuild);
		}
		return kvs_vector_rebuild_shadow_v2_fail(
			ret,
			"shadow baseline build failed"
		);
	}

	g_vector_state.rebuild.progress_done = g_vector_state.store.active_records;
	g_vector_state.rebuild.progress_total = g_vector_state.store.active_records;
	return kvs_vector_rebuild_enter_replaying_delta(&g_vector_state.rebuild);
}

int kvs_vector_rebuild_shadow_v2_build_from_active_records_for_test(void) {
	return kvs_vector_rebuild_shadow_v2_build_from_active_records();
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

int kvs_vector_rebuild_shadow_v2_replay_deltas(void) {
	kvs_hnsw_v2_index_t *index;
	char reason[128];
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

		if (g_vector_state.rebuild.cancel_requested) {
			return kvs_vector_rebuild_cancel(&g_vector_state.rebuild);
		}
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

	if (g_vector_state.rebuild.cancel_requested) {
		return kvs_vector_rebuild_cancel(&g_vector_state.rebuild);
	}

	reason[0] = '\0';
	ret = kvs_vector_rebuild_shadow_v2_validate_final(
		index,
		reason,
		sizeof(reason)
	);
	if (ret != KVS_VECTOR_OK) {
		return kvs_vector_rebuild_shadow_v2_fail(
			ret,
			reason[0] ? reason : "shadow final validation failed"
		);
	}

	return kvs_vector_rebuild_enter_ready_to_swap(&g_vector_state.rebuild);
}

int kvs_vector_rebuild_shadow_v2_replay_deltas_for_test(void) {
	return kvs_vector_rebuild_shadow_v2_replay_deltas();
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

int kvs_vector_rebuild_publish_ready_shadow(void) {
	int ret;

	if (!g_vector_state.initialized) {
		return KVS_VECTOR_ERR_NOT_INITIALIZED;
	}

	ret = kvs_vector_active_index_acquire_write();
	if (ret != KVS_VECTOR_OK) {
		return ret;
	}
	if (g_vector_state.index_type == KVS_VECTOR_INDEX_CORE_HNSW_V2) {
		ret = kvs_vector_rebuild_publish_shadow_v2_locked();
	} else {
		ret = kvs_vector_rebuild_promote_shadow(&g_vector_state.rebuild);
	}
	kvs_vector_active_index_release_write();
	if (ret != KVS_VECTOR_OK) {
		return ret;
	}

	return KVS_VECTOR_OK;
}

int kvs_vector_rebuild_swap_ready_shadow_for_test(void) {
	kvs_hnsw_v2_index_t *promoted;
	char reason[128];
	int ret;

	ret = kvs_vector_rebuild_publish_ready_shadow();
	if (ret != KVS_VECTOR_OK) {
		return ret;
	}

	if (g_vector_state.index_type != KVS_VECTOR_INDEX_CORE_HNSW_V2) {
		return KVS_VECTOR_OK;
	}

	promoted = kvs_vector_rebuild_active_v2_index();
	if (!promoted) {
		return KVS_VECTOR_OK;
	}

	reason[0] = '\0';
	if (kvs_vector_index_snapshot_write_hnsw_v2_index(
		    &g_vector_state,
		    promoted,
		    reason,
		    sizeof(reason)
	    ) == KVS_VECTOR_OK) {
		KVS_LOG_INFO(
			KVS_VECTOR_LOG_MODULE,
			"vector index snapshot written source=shadow_publish path=%s bytes=%llu checksum=%llu",
			g_vector_state.snapshot_status.last_snapshot_path,
			(unsigned long long)g_vector_state.snapshot_status.last_snapshot_bytes,
			(unsigned long long)g_vector_state.snapshot_status.last_snapshot_checksum
		);
		return KVS_VECTOR_OK;
	}

	KVS_LOG_WARN(
		KVS_VECTOR_LOG_MODULE,
		"vector index snapshot write skipped source=shadow_publish reason=%s",
		reason[0] ? reason : "unknown"
	);
	return KVS_VECTOR_OK;
}

int kvs_vector_rebuild_retire_cleanup_safe(void) {
	int ret;

	if (!g_vector_state.initialized) {
		return KVS_VECTOR_ERR_NOT_INITIALIZED;
	}

	ret = kvs_vector_active_index_acquire_write();
	if (ret != KVS_VECTOR_OK) {
		return ret;
	}
	ret = kvs_vector_rebuild_retire_cleanup(&g_vector_state.rebuild);
	kvs_vector_active_index_release_write();
	return ret;
}

int kvs_vector_rebuild_retire_cleanup_for_test(void) {
	return kvs_vector_rebuild_retire_cleanup_safe();
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
