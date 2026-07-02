#include "kvs_config.h"
#include "kvs_memory.h"
#include "kvs_memory_internal.h"
#include "vector/kvs_vector_hnsw_v2.h"

#include <stdint.h>
#include <stdio.h>
#include <string.h>

extern kvs_config_t g_cfg;

static int expect_int(int cond, const char *label) {
	if (!cond) {
		fprintf(stderr, "FAIL: %s\n", label);
		return 1;
	}
	return 0;
}

static int find_hit(
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

static int hits_are_active(
	const kvs_vector_hit_t *hits,
	size_t count,
	const uint64_t *deleted_ids,
	size_t deleted_count
) {
	size_t i;
	size_t j;

	for (i = 0; i < count; i++) {
		for (j = 0; j < deleted_count; j++) {
			if (hits[i].record_id == deleted_ids[j]) {
				return 0;
			}
		}
	}
	return 1;
}

static int hits_descending(
	const kvs_vector_hit_t *hits,
	size_t count
) {
	size_t i;

	for (i = 1; i < count; i++) {
		if (hits[i - 1].score < hits[i].score) {
			return 0;
		}
	}
	return 1;
}

static int add_fixture(
	kvs_hnsw_v2_index_t *index
) {
	static const float vectors[5][4] = {
		{1.00f, 0.00f, 0.00f, 0.00f},
		{0.95f, 0.05f, 0.00f, 0.00f},
		{0.90f, 0.10f, 0.00f, 0.00f},
		{0.10f, 0.90f, 0.00f, 0.00f},
		{-1.0f, 0.00f, 0.00f, 0.00f}
	};
	size_t i;

	for (i = 0; i < 5; i++) {
		if (kvs_hnsw_v2_index_add(index, 1000 + i, vectors[i], 4) !=
		    KVS_VECTOR_OK) {
			return 1;
		}
	}
	return 0;
}

int main(void) {
	kvs_hnsw_v2_index_t index;
	kvs_vector_core_info_t info;
	kvs_vector_hit_t hits[8];
	float query[4] = {1.0f, 0.0f, 0.0f, 0.0f};
	uint64_t deleted_ids[5];
	uint32_t slot = UINT32_MAX;
	size_t out_count = 0;
	size_t i;
	int failed = 0;

	memset(&g_cfg, 0, sizeof(g_cfg));
	kvs_config_init_default(&g_cfg);
	g_cfg.mem_backend = KVS_MEM_BACKEND_SYSTEM;
	mem_set_backend(KVS_MEM_BACKEND_SYSTEM);

	memset(&index, 0, sizeof(index));
	failed |= expect_int(
		kvs_hnsw_v2_index_init(&index, 4, KVS_VECTOR_METRIC_IP) ==
			KVS_VECTOR_OK,
		"init"
	);
	index.M = 4;
	index.M0 = 8;
	index.ef_construction = 32;
	index.ef_search = 16;
	kvs_hnsw_v2_index_set_seed(&index, 0x9f2001ULL);
	failed |= expect_int(
		kvs_hnsw_v2_index_reserve(&index, 8) == KVS_VECTOR_OK,
		"reserve"
	);
	failed |= expect_int(add_fixture(&index) == 0, "add fixture");
	failed |= expect_int(index.size == 5, "size after add");
	failed |= expect_int(index.deleted_count == 0, "initial deleted count");
	failed |= expect_int(
		kvs_hnsw_v2_index_info(&index, &info) == KVS_VECTOR_OK,
		"info initial"
	);
	failed |= expect_int(info.deleted_count == 0, "info initial deleted");

	failed |= expect_int(
		kvs_hnsw_v2_index_mark_delete(NULL, 1000) ==
			KVS_VECTOR_ERR_INVALID_ARG,
		"mark_delete rejects null"
	);
	failed |= expect_int(
		kvs_hnsw_v2_index_mark_delete(&index, 1000) == KVS_VECTOR_OK,
		"mark_delete nearest"
	);
	failed |= expect_int(index.deleted_count == 1, "deleted count increments");
	failed |= expect_int(index.needs_rebuild == 0, "delete does not trigger rebuild");
	failed |= expect_int(
		kvs_hnsw_v2_index_mark_delete(&index, 1000) == KVS_VECTOR_OK,
		"repeated delete idempotent"
	);
	failed |= expect_int(index.deleted_count == 1, "repeated delete count stable");
	failed |= expect_int(
		kvs_hnsw_v2_index_mark_delete(&index, 9999) ==
			KVS_VECTOR_ERR_NOT_FOUND,
		"missing delete not found"
	);
	failed |= expect_int(index.deleted_count == 1, "missing delete count stable");
	failed |= expect_int(
		kvs_vector_id_slot_map_get(&index.record_id_to_slot, 1000, &slot) ==
			KVS_VECTOR_OK,
		"deleted id map retained"
	);
	failed |= expect_int(slot < index.size, "deleted slot in range");
	failed |= expect_int(index.slot_to_record_id[slot] == 1000, "deleted slot id retained");
	failed |= expect_int(index.deleted_flags[slot] == 1, "deleted flag set");

	failed |= expect_int(
		kvs_hnsw_v2_index_search(&index, query, 4, 3, 16, hits, &out_count) ==
			KVS_VECTOR_OK,
		"search after nearest delete"
	);
	failed |= expect_int(out_count == 3, "search returns active hits");
	failed |= expect_int(hits_descending(hits, out_count), "hits descending");
	failed |= expect_int(!find_hit(hits, out_count, 1000), "deleted nearest filtered");
	failed |= expect_int(hits[0].record_id == 1001, "next active nearest returned");

	failed |= expect_int(
		kvs_hnsw_v2_index_mark_delete(&index, 1002) == KVS_VECTOR_OK,
		"delete another"
	);
	failed |= expect_int(
		kvs_hnsw_v2_index_mark_delete(&index, 1004) == KVS_VECTOR_OK,
		"delete far"
	);
	deleted_ids[0] = 1000;
	deleted_ids[1] = 1002;
	deleted_ids[2] = 1004;
	failed |= expect_int(
		kvs_hnsw_v2_index_search(&index, query, 4, 5, 16, hits, &out_count) ==
			KVS_VECTOR_OK,
		"search after multiple deletes"
	);
	failed |= expect_int(out_count == 2, "multiple deletes active count");
	failed |= expect_int(
		hits_are_active(hits, out_count, deleted_ids, 3),
		"multiple deletes active only"
	);

	for (i = 0; i < 5; i++) {
		(void)kvs_hnsw_v2_index_mark_delete(&index, 1000 + i);
	}
	failed |= expect_int(index.deleted_count == 5, "all deleted count");
	failed |= expect_int(
		kvs_hnsw_v2_index_search(&index, query, 4, 5, 16, hits, &out_count) ==
			KVS_VECTOR_OK,
		"search all deleted"
	);
	failed |= expect_int(out_count == 0, "all deleted empty result");

	failed |= expect_int(
		kvs_hnsw_v2_index_reset(&index) == KVS_VECTOR_OK,
		"reset"
	);
	failed |= expect_int(index.deleted_count == 0, "reset clears deleted count");
	failed |= expect_int(index.needs_rebuild == 0, "reset clears rebuild flag");
	failed |= expect_int(index.size == 0, "reset clears size");
	failed |= expect_int(
		kvs_vector_id_slot_map_get(&index.record_id_to_slot, 1000, &slot) ==
			KVS_VECTOR_ERR_NOT_FOUND,
		"reset clears id map"
	);

	{
		float original[4] = {1.0f, 0.0f, 0.0f, 0.0f};
		float updated[4] = {0.0f, 1.0f, 0.0f, 0.0f};
		float query_updated[4] = {0.0f, 1.0f, 0.0f, 0.0f};
		uint32_t old_slot = UINT32_MAX;
		uint32_t new_slot = UINT32_MAX;

		failed |= expect_int(
			kvs_hnsw_v2_index_add(&index, 2000, original, 4) ==
				KVS_VECTOR_OK,
			"readd fixture add original"
		);
		failed |= expect_int(
			kvs_hnsw_v2_index_add(&index, 2000, updated, 4) !=
				KVS_VECTOR_OK,
			"active duplicate add rejected"
		);
		failed |= expect_int(
			kvs_vector_id_slot_map_get(&index.record_id_to_slot, 2000, &old_slot) ==
				KVS_VECTOR_OK,
			"readd old slot lookup"
		);
		failed |= expect_int(
			kvs_hnsw_v2_index_mark_delete(&index, 2000) == KVS_VECTOR_OK,
			"readd mark delete original"
		);
		failed |= expect_int(
			kvs_hnsw_v2_index_add(&index, 2000, updated, 4) ==
				KVS_VECTOR_OK,
			"readd after tombstone succeeds"
		);
		failed |= expect_int(index.deleted_count == 1, "readd deleted count keeps tombstone");
		failed |= expect_int(
			kvs_vector_id_slot_map_get(&index.record_id_to_slot, 2000, &new_slot) ==
				KVS_VECTOR_OK,
			"readd new slot lookup"
		);
		failed |= expect_int(new_slot != old_slot, "readd map points to new slot");
		failed |= expect_int(index.deleted_flags[old_slot] == 1, "readd old slot tombstone");
		failed |= expect_int(index.deleted_flags[new_slot] == 0, "readd new slot active");
		failed |= expect_int(
			kvs_hnsw_v2_index_search(
				&index,
				query_updated,
				4,
				1,
				16,
				hits,
				&out_count
			) == KVS_VECTOR_OK,
			"readd search updated vector"
		);
		failed |= expect_int(out_count == 1, "readd search one active hit");
		failed |= expect_int(hits[0].record_id == 2000, "readd returns latest id");
	}

	kvs_hnsw_v2_index_destroy(&index);
	failed |= expect_int(index.deleted_count == 0, "destroy clears deleted count");
	failed |= expect_int(index.deleted_flags == NULL, "destroy clears deleted flags");

	if (failed) {
		return 1;
	}

	printf("vector_core_hnsw_v2_tombstone_test PASS\n");
	return 0;
}
