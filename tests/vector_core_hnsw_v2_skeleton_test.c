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

static size_t expected_total(
	const kvs_hnsw_v2_index_t *index
) {
	size_t total = 0;

	total += index->memory.vectors_bytes;
	total += index->memory.slot_to_record_id_bytes;
	total += index->memory.record_id_to_slot_bytes;
	total += index->memory.levels_bytes;
	total += index->memory.deleted_flags_bytes;
	total += index->memory.visited_tags_bytes;
	total += index->memory.level0_graph_bytes;
	total += index->memory.upper_graph_bytes;
	total += index->memory.context_bytes;

	return total;
}

int main(void) {
	kvs_hnsw_v2_index_t index;
	kvs_vector_core_info_t info;
	float *old_vectors;
	size_t old_capacity;
	size_t old_memory;
	int failed = 0;

	memset(&g_cfg, 0, sizeof(g_cfg));
	kvs_config_init_default(&g_cfg);
	g_cfg.mem_backend = KVS_MEM_BACKEND_SYSTEM;
	mem_set_backend(KVS_MEM_BACKEND_SYSTEM);

	memset(&index, 0, sizeof(index));

	failed |= expect_int(
		kvs_hnsw_v2_index_init(NULL, 16, KVS_VECTOR_METRIC_IP) ==
			KVS_VECTOR_ERR_INVALID_ARG,
		"init rejects NULL"
	);
	failed |= expect_int(
		kvs_hnsw_v2_index_init(&index, 0, KVS_VECTOR_METRIC_IP) ==
			KVS_VECTOR_ERR_INVALID_ARG,
		"init rejects zero dim"
	);
	failed |= expect_int(
		kvs_hnsw_v2_index_init(&index, 17, KVS_VECTOR_METRIC_IP) ==
			KVS_VECTOR_OK,
		"init succeeds"
	);
	failed |= expect_int(index.dim == 17, "dim stored");
	failed |= expect_int(index.metric == KVS_VECTOR_METRIC_IP, "metric stored");
	failed |= expect_int(index.M == KVS_HNSW_V2_DEFAULT_M, "M default");
	failed |= expect_int(index.M0 == KVS_HNSW_V2_DEFAULT_M0, "M0 default");
	failed |= expect_int(
		index.ef_construction == KVS_HNSW_V2_DEFAULT_EF_CONSTRUCTION,
		"ef_construction default"
	);
	failed |= expect_int(
		index.ef_search == KVS_HNSW_V2_DEFAULT_EF_SEARCH,
		"ef_search default"
	);
	failed |= expect_int(index.vector_stride >= index.dim, "stride covers dim");
	failed |= expect_int(index.ready == 1, "ready after init");
	failed |= expect_int(index.capacity == 0, "zero capacity after init");

	index.M = 24;
	index.M0 = 48;
	index.ef_construction = 96;
	index.ef_search = 192;

	failed |= expect_int(
		kvs_hnsw_v2_index_reserve(&index, 0) == KVS_VECTOR_ERR_INVALID_ARG,
		"reserve rejects zero capacity"
	);
	failed |= expect_int(
		kvs_hnsw_v2_index_reserve(&index, 128) == KVS_VECTOR_OK,
		"reserve succeeds"
	);
	failed |= expect_int(index.capacity == 128, "capacity after reserve");
	failed |= expect_int(index.M == 24, "custom M retained");
	failed |= expect_int(index.M0 == 48, "custom M0 retained");
	failed |= expect_int(index.ef_construction == 96, "custom ef_construction retained");
	failed |= expect_int(index.ef_search == 192, "custom ef_search retained");
	failed |= expect_int(index.size == 0, "size remains zero");
	failed |= expect_int(index.vectors != NULL, "vectors allocated");
	failed |= expect_int(
		((uintptr_t)index.vectors % KVS_HNSW_V2_VECTOR_ALIGNMENT) == 0,
		"vectors aligned"
	);
	failed |= expect_int(index.slot_to_record_id != NULL, "slot map allocated");
	failed |= expect_int(index.record_id_to_slot.entries != NULL, "id-slot map allocated");
	failed |= expect_int(index.levels != NULL, "levels allocated");
	failed |= expect_int(index.deleted_flags != NULL, "deleted flags allocated");
	failed |= expect_int(index.visited_tags != NULL, "visited tags allocated");
	failed |= expect_int(index.level0_neighbors != NULL, "level0 graph allocated");
	failed |= expect_int(index.level0_counts != NULL, "level0 counts allocated");
	failed |= expect_int(index.upper_offsets != NULL, "upper offsets allocated");
	failed |= expect_int(index.upper_counts != NULL, "upper counts allocated");
	failed |= expect_int(index.context_pool.contexts != NULL, "search contexts allocated");
	failed |= expect_int(index.memory.vectors_bytes == 128 * index.vector_stride * sizeof(float), "vector bytes");
	failed |= expect_int(index.memory.slot_to_record_id_bytes == 128 * sizeof(uint64_t), "slot bytes");
	failed |= expect_int(index.memory.levels_bytes == 128 * sizeof(uint8_t), "level bytes");
	failed |= expect_int(index.memory.deleted_flags_bytes == 128 * sizeof(uint8_t), "deleted bytes");
	failed |= expect_int(index.memory.visited_tags_bytes == 128 * sizeof(uint32_t), "visited bytes");
	failed |= expect_int(index.memory.level0_graph_bytes == 128 * index.M0 * sizeof(uint32_t) + 128 * sizeof(uint16_t), "level0 bytes");
	failed |= expect_int(
		index.memory.upper_graph_bytes ==
			128 * sizeof(size_t) +
			128 * (KVS_HNSW_V2_MAX_LEVEL - 1) * sizeof(uint16_t) +
			128 * (KVS_HNSW_V2_MAX_LEVEL - 1) * index.M * sizeof(uint32_t),
		"upper graph bytes"
	);
	failed |= expect_int(
		index.memory.context_bytes ==
			KVS_HNSW_V2_DEFAULT_CONTEXT_COUNT * sizeof(kvs_hnsw_v2_search_context_t),
		"context bytes"
	);
	failed |= expect_int(index.memory.total_bytes == expected_total(&index), "total memory");

	failed |= expect_int(
		kvs_hnsw_v2_index_info(&index, &info) == KVS_VECTOR_OK,
		"info succeeds"
	);
	failed |= expect_int(info.capacity == index.capacity, "info capacity");
	failed |= expect_int(info.size == 0, "info size");
	failed |= expect_int(info.dim == index.dim, "info dim");
	failed |= expect_int(info.metric_type == index.metric, "info metric");
	failed |= expect_int(info.ready == 1, "info ready");
	failed |= expect_int(info.memory_bytes == index.memory.total_bytes, "info memory");

	old_vectors = index.vectors;
	old_capacity = index.capacity;
	old_memory = index.memory.total_bytes;
	failed |= expect_int(
		kvs_hnsw_v2_index_reserve(&index, 64) == KVS_VECTOR_OK,
		"smaller repeated reserve succeeds"
	);
	failed |= expect_int(index.capacity == old_capacity, "smaller reserve keeps capacity");
	failed |= expect_int(index.vectors == old_vectors, "smaller reserve keeps storage");
	failed |= expect_int(index.memory.total_bytes == old_memory, "smaller reserve keeps memory");

	index.size = 12;
	index.deleted_count = 3;
	index.entry_point = 7;
	index.has_entry = 1;
	index.max_level = 4;
	index.current_visit_tag = 99;
	index.slot_to_record_id[0] = 123;
	index.levels[0] = 2;
	index.deleted_flags[0] = 1;
	index.visited_tags[0] = 99;
	index.level0_neighbors[0] = 42;
	index.level0_counts[0] = 1;
	failed |= expect_int(
		kvs_hnsw_v2_index_reset(&index) == KVS_VECTOR_OK,
		"reset succeeds"
	);
	failed |= expect_int(index.capacity == old_capacity, "reset keeps capacity");
	failed |= expect_int(index.vectors == old_vectors, "reset keeps storage");
	failed |= expect_int(index.size == 0, "reset size");
	failed |= expect_int(index.deleted_count == 0, "reset deleted count");
	failed |= expect_int(index.has_entry == 0, "reset entry");
	failed |= expect_int(index.max_level == 0, "reset max level");
	failed |= expect_int(index.current_visit_tag == 1, "reset visit tag");
	failed |= expect_int(index.slot_to_record_id[0] == 0, "reset slot ids");
	failed |= expect_int(index.levels[0] == 0, "reset levels");
	failed |= expect_int(index.deleted_flags[0] == 0, "reset deleted flags");
	failed |= expect_int(index.visited_tags[0] == 0, "reset visited tags");
	failed |= expect_int(index.level0_neighbors[0] == 0, "reset graph");
	failed |= expect_int(index.level0_counts[0] == 0, "reset counts");

	failed |= expect_int(
		kvs_hnsw_v2_index_reserve(&index, 256) == KVS_VECTOR_OK,
		"larger repeated reserve succeeds"
	);
	failed |= expect_int(index.capacity == 256, "larger reserve capacity");
	failed |= expect_int(index.memory.total_bytes > old_memory, "larger reserve memory grows");
	failed |= expect_int(
		((uintptr_t)index.vectors % KVS_HNSW_V2_VECTOR_ALIGNMENT) == 0,
		"larger reserve vectors aligned"
	);

	kvs_hnsw_v2_index_destroy(&index);
	failed |= expect_int(index.ready == 0, "destroy clears ready");
	failed |= expect_int(index.capacity == 0, "destroy clears capacity");
	failed |= expect_int(index.vectors == NULL, "destroy clears vectors");
	failed |= expect_int(index.record_id_to_slot.entries == NULL, "destroy clears id-slot map");
	failed |= expect_int(index.memory.total_bytes == 0, "destroy clears memory");

	if (failed) {
		return 1;
	}

	printf("vector_core_hnsw_v2_skeleton_test PASS\n");
	return 0;
}
