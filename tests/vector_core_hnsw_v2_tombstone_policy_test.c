#include "kvs_config.h"
#include "kvs_memory.h"
#include "kvs_memory_internal.h"
#include "vector/kvs_vector_hnsw_v2.h"

#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

extern kvs_config_t g_cfg;

#define POLICY_COUNT 1000
#define POLICY_DIM 32
#define POLICY_TOPK 10

typedef struct tombstone_policy_s {
	double ratio;
	int recommend_rebuild;
	int strong_recommend_rebuild;
} tombstone_policy_t;

static int expect_int(int cond, const char *label) {
	if (!cond) {
		fprintf(stderr, "FAIL: %s\n", label);
		return 1;
	}
	return 0;
}

static uint32_t rng_next(uint64_t *state) {
	uint64_t z;

	*state += 0x9e3779b97f4a7c15ULL;
	z = *state;
	z = (z ^ (z >> 30)) * 0xbf58476d1ce4e5b9ULL;
	z = (z ^ (z >> 27)) * 0x94d049bb133111ebULL;
	z ^= z >> 31;
	return (uint32_t)(z >> 32);
}

static void fill_random_vector(float *vector, size_t dim, uint64_t seed) {
	uint64_t state = seed;
	size_t i;

	for (i = 0; i < dim; i++) {
		uint32_t raw = rng_next(&state);
		vector[i] = (float)((int)(raw % 20001u) - 10000) / 10000.0f;
	}
}

static tombstone_policy_t eval_policy(size_t size, size_t deleted_count) {
	tombstone_policy_t policy;
	size_t denom = size ? size : 1;

	policy.ratio = (double)deleted_count / (double)denom;
	policy.recommend_rebuild = policy.ratio >= 0.30;
	policy.strong_recommend_rebuild = policy.ratio >= 0.50;
	return policy;
}

static int hits_exclude_deleted(
	const kvs_vector_hit_t *hits,
	size_t hit_count,
	const uint8_t *deleted_by_ordinal
) {
	size_t i;

	for (i = 0; i < hit_count; i++) {
		uint64_t record_id = hits[i].record_id;
		size_t ordinal;

		if (record_id < 1000000ULL) {
			return 0;
		}
		ordinal = (size_t)(record_id - 1000000ULL);
		if (ordinal >= POLICY_COUNT) {
			return 0;
		}
		if (deleted_by_ordinal[ordinal]) {
			return 0;
		}
	}
	return 1;
}

static int search_active_only(
	kvs_hnsw_v2_index_t *index,
	const float *query,
	const uint8_t *deleted_by_ordinal,
	const char *label
) {
	kvs_vector_hit_t hits[POLICY_TOPK];
	size_t hit_count = 0;
	int ret;

	ret = kvs_hnsw_v2_index_search(
		index,
		query,
		POLICY_DIM,
		POLICY_TOPK,
		KVS_HNSW_V2_DEFAULT_EF_SEARCH,
		hits,
		&hit_count
	);
	if (ret != KVS_VECTOR_OK) {
		fprintf(stderr, "FAIL: %s search ret=%d\n", label, ret);
		return 1;
	}
	if (!hits_exclude_deleted(hits, hit_count, deleted_by_ordinal)) {
		fprintf(stderr, "FAIL: %s returned tombstone\n", label);
		return 1;
	}
	return 0;
}

static int add_active_records(
	kvs_hnsw_v2_index_t *index,
	const float *vectors,
	const uint8_t *deleted_by_ordinal,
	size_t *active_count
) {
	size_t i;

	*active_count = 0;
	for (i = 0; i < POLICY_COUNT; i++) {
		if (deleted_by_ordinal[i]) {
			continue;
		}
		if (kvs_hnsw_v2_index_add(
			    index,
			    1000000ULL + i,
			    vectors + i * POLICY_DIM,
			    POLICY_DIM
		    ) != KVS_VECTOR_OK) {
			fprintf(stderr, "FAIL: rebuild add active i=%zu\n", i);
			return 1;
		}
		(*active_count)++;
	}
	return 0;
}

int main(void) {
	kvs_hnsw_v2_index_t index;
	kvs_vector_core_info_t info;
	float *vectors = NULL;
	uint8_t deleted_by_ordinal[POLICY_COUNT];
	tombstone_policy_t policy;
	size_t active_count = 0;
	size_t i;
	int failed = 0;

	memset(&g_cfg, 0, sizeof(g_cfg));
	kvs_config_init_default(&g_cfg);
	g_cfg.mem_backend = KVS_MEM_BACKEND_SYSTEM;
	mem_set_backend(KVS_MEM_BACKEND_SYSTEM);

	memset(&index, 0, sizeof(index));
	memset(deleted_by_ordinal, 0, sizeof(deleted_by_ordinal));
	vectors = (float *)malloc(POLICY_COUNT * POLICY_DIM * sizeof(*vectors));
	failed |= expect_int(vectors != NULL, "allocate vectors");
	if (failed) {
		goto done;
	}

	failed |= expect_int(
		kvs_hnsw_v2_index_init(&index, POLICY_DIM, KVS_VECTOR_METRIC_IP) ==
			KVS_VECTOR_OK,
		"init"
	);
	index.M = KVS_HNSW_V2_DEFAULT_M;
	index.M0 = KVS_HNSW_V2_DEFAULT_M0;
	index.ef_construction = KVS_HNSW_V2_DEFAULT_EF_CONSTRUCTION;
	index.ef_search = KVS_HNSW_V2_DEFAULT_EF_SEARCH;
	kvs_hnsw_v2_index_set_seed(&index, 0x9f3100ULL);
	failed |= expect_int(
		kvs_hnsw_v2_index_reserve(&index, POLICY_COUNT) == KVS_VECTOR_OK,
		"reserve"
	);
	if (failed) {
		goto done;
	}

	for (i = 0; i < POLICY_COUNT; i++) {
		fill_random_vector(vectors + i * POLICY_DIM, POLICY_DIM, 0xbbb00000ULL + i);
		failed |= expect_int(
			kvs_hnsw_v2_index_add(
				&index,
				1000000ULL + i,
				vectors + i * POLICY_DIM,
				POLICY_DIM
			) == KVS_VECTOR_OK,
			"add fixture"
		);
		if (failed) {
			goto done;
		}
	}

	for (i = 0; i < 300; i++) {
		failed |= expect_int(
			kvs_hnsw_v2_index_mark_delete(&index, 1000000ULL + i) ==
				KVS_VECTOR_OK,
			"delete to recommend threshold"
		);
		deleted_by_ordinal[i] = 1;
	}
	policy = eval_policy(index.size, index.deleted_count);
	active_count = index.size - index.deleted_count;
	printf(
		"hnsw_v2_tombstone_policy stage=recommend size=%zu deleted=%zu "
		"active=%zu ratio=%.4f recommend=%d strong=%d\n",
		index.size,
		index.deleted_count,
		active_count,
		policy.ratio,
		policy.recommend_rebuild,
		policy.strong_recommend_rebuild
	);
	failed |= expect_int(index.deleted_count == 300, "recommend deleted count");
	failed |= expect_int(active_count == 700, "recommend active count");
	failed |= expect_int(policy.recommend_rebuild == 1, "recommend rebuild");
	failed |= expect_int(policy.strong_recommend_rebuild == 0, "not strong yet");
	failed |= search_active_only(
		&index,
		vectors + 300 * POLICY_DIM,
		deleted_by_ordinal,
		"recommend"
	);

	for (i = 300; i < 500; i++) {
		failed |= expect_int(
			kvs_hnsw_v2_index_mark_delete(&index, 1000000ULL + i) ==
				KVS_VECTOR_OK,
			"delete to strong threshold"
		);
		deleted_by_ordinal[i] = 1;
	}
	policy = eval_policy(index.size, index.deleted_count);
	active_count = index.size - index.deleted_count;
	printf(
		"hnsw_v2_tombstone_policy stage=strong size=%zu deleted=%zu "
		"active=%zu ratio=%.4f recommend=%d strong=%d\n",
		index.size,
		index.deleted_count,
		active_count,
		policy.ratio,
		policy.recommend_rebuild,
		policy.strong_recommend_rebuild
	);
	failed |= expect_int(index.deleted_count == 500, "strong deleted count");
	failed |= expect_int(active_count == 500, "strong active count");
	failed |= expect_int(policy.recommend_rebuild == 1, "strong recommend rebuild");
	failed |= expect_int(policy.strong_recommend_rebuild == 1, "strong warning");
	failed |= search_active_only(
		&index,
		vectors + 500 * POLICY_DIM,
		deleted_by_ordinal,
		"strong"
	);

	failed |= expect_int(kvs_hnsw_v2_index_reset(&index) == KVS_VECTOR_OK, "reset");
	failed |= expect_int(index.deleted_count == 0, "reset clears tombstones");
	failed |= expect_int(index.size == 0, "reset clears size");
	failed |= expect_int(add_active_records(
		&index,
		vectors,
		deleted_by_ordinal,
		&active_count
	) == 0, "rebuild active records");
	kvs_hnsw_v2_graph_stats_refresh(&index);
	kvs_hnsw_v2_memory_refresh(&index);
	memset(&info, 0, sizeof(info));
	failed |= expect_int(kvs_hnsw_v2_index_info(&index, &info) == KVS_VECTOR_OK,
		"info after rebuild");
	policy = eval_policy(index.size, index.deleted_count);
	printf(
		"hnsw_v2_tombstone_policy stage=after_rebuild size=%zu deleted=%zu "
		"active=%zu ratio=%.4f recommend=%d strong=%d info_deleted=%zu "
		"memory_bytes=%zu\n",
		index.size,
		index.deleted_count,
		index.size - index.deleted_count,
		policy.ratio,
		policy.recommend_rebuild,
		policy.strong_recommend_rebuild,
		info.deleted_count,
		info.memory_bytes
	);
	failed |= expect_int(active_count == 500, "rebuild active count");
	failed |= expect_int(index.size == 500, "rebuild size active only");
	failed |= expect_int(index.deleted_count == 0, "rebuild tombstones cleared");
	failed |= expect_int(info.deleted_count == 0, "info tombstones cleared");
	failed |= expect_int(policy.recommend_rebuild == 0, "policy clear after rebuild");
	failed |= search_active_only(
		&index,
		vectors + 500 * POLICY_DIM,
		deleted_by_ordinal,
		"after rebuild"
	);

done:
	kvs_hnsw_v2_index_destroy(&index);
	free(vectors);
	if (failed) {
		fprintf(stderr, "vector_core_hnsw_v2_tombstone_policy_test FAILED\n");
		return 1;
	}
	printf("vector_core_hnsw_v2_tombstone_policy_test PASS\n");
	return 0;
}
