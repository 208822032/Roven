#define KVS_VECTOR_INTERNAL
#include "kvs_config.h"
#include "kvs_memory.h"
#include "kvs_memory_internal.h"
#include "kvs_vector.h"

#include <stdio.h>
#include <string.h>

extern kvs_config_t g_cfg;

#define DIM 64

static int expect_int(int cond, const char *label) {
	if (!cond) {
		fprintf(stderr, "FAIL: %s\n", label);
		return 1;
	}
	return 0;
}

static void reset_config(const char *index_type, int v2_enabled) {
	memset(&g_cfg, 0, sizeof(g_cfg));
	kvs_config_init_default(&g_cfg);
	g_cfg.mem_backend = KVS_MEM_BACKEND_SYSTEM;
	g_cfg.embedding_dim = DIM;
	g_cfg.vector_score_threshold = -1.0f;
	g_cfg.vector_hnsw_v2_experimental_enabled = v2_enabled;
	snprintf(g_cfg.vector_index_type, sizeof(g_cfg.vector_index_type), "%s", index_type);
}

static int commit_prepared_vector(
	const char *question,
	const char *answer,
	const char *scope,
	const float *vector
) {
	kvs_vector_set_prepared_t prepared;
	int ret;

	kvs_vector_set_prepared_reset(&prepared);
	ret = kvs_vector_set_prepare_with_embedding(
		question,
		strlen(question),
		answer,
		strlen(answer),
		scope,
		scope ? strlen(scope) : 0,
		vector,
		DIM,
		&prepared
	);
	if (ret != KVS_VECTOR_OK) {
		kvs_vector_set_prepared_destroy(&prepared);
		return ret;
	}

	ret = kvs_vector_set_commit_prepared(&prepared);
	kvs_vector_set_prepared_destroy(&prepared);
	return ret;
}

static int search_prepared_vector(
	const char *query,
	const char *scope,
	const float *vector,
	kvs_vector_search_result_t *results,
	size_t capacity,
	size_t *out_count
) {
	kvs_vector_search_prepared_t prepared;
	int ret;

	kvs_vector_search_prepared_reset(&prepared);
	ret = kvs_vector_search_prepare_with_embedding(
		query,
		strlen(query),
		scope,
		scope ? strlen(scope) : 0,
		vector,
		DIM,
		&prepared
	);
	if (ret != KVS_VECTOR_OK) {
		kvs_vector_search_prepared_destroy(&prepared);
		return ret;
	}

	ret = kvs_vector_search_with_prepared(
		&prepared,
		(int)capacity,
		-1.0f,
		results,
		capacity,
		out_count
	);
	kvs_vector_search_prepared_destroy(&prepared);
	return ret;
}

static int result_has_question(
	const kvs_vector_search_result_t *results,
	size_t count,
	const char *question
) {
	size_t i;
	size_t question_len = strlen(question);

	for (i = 0; i < count; i++) {
		if (results[i].question &&
		    results[i].question_len == question_len &&
		    memcmp(results[i].question, question, question_len) == 0) {
			return 1;
		}
	}
	return 0;
}

static int expect_explicit_v2_ignores_legacy_flag(void) {
	kvs_vector_info_t info;
	int failed = 0;

	reset_config("core_hnsw_v2", 0);
	failed |= expect_int(kvs_vector_init() == KVS_VECTOR_OK, "legacy flag off init");
	memset(&info, 0, sizeof(info));
	failed |= expect_int(kvs_vector_info(&info) == KVS_VECTOR_OK, "legacy flag off info");
	failed |= expect_int(
		info.index_type == KVS_VECTOR_INDEX_CORE_HNSW_V2,
		"legacy flag off still selects explicit core_hnsw_v2"
	);
	failed |= expect_int(
		info.core_hnsw_v2_experimental_enabled == 0,
		"legacy v2 experimental flag remains visible as 0"
	);
	kvs_vector_destroy();
	return failed;
}

static int expect_enabled_product_path(void) {
	const float vec_a[DIM] = {1.0f, 0.0f, 0.0f, 0.0f};
	const float vec_b[DIM] = {0.0f, 1.0f, 0.0f, 0.0f};
	const float vec_c[DIM] = {0.0f, 0.0f, 1.0f, 0.0f};
	const float vec_a2[DIM] = {0.0f, 0.0f, 0.0f, 1.0f};
	kvs_vector_search_result_t results[4];
	kvs_vector_search_result_t get_result;
	kvs_vector_info_t info;
	size_t count = 0;
	int failed = 0;

	reset_config("core_hnsw_v2", 1);
	failed |= expect_int(kvs_vector_init() == KVS_VECTOR_OK, "enabled init");

	memset(&info, 0, sizeof(info));
	failed |= expect_int(kvs_vector_info(&info) == KVS_VECTOR_OK, "enabled info initial");
	failed |= expect_int(
		info.index_type == KVS_VECTOR_INDEX_CORE_HNSW_V2,
		"enabled selects core_hnsw_v2"
	);
	failed |= expect_int(info.core_hnsw_v2_experimental_enabled == 1, "v2 flag visible");

	failed |= expect_int(
		commit_prepared_vector("alpha", "answer-alpha-1", "scopeA", vec_a) ==
			KVS_VECTOR_OK,
		"v2 VSET alpha"
	);
	failed |= expect_int(
		commit_prepared_vector("beta", "answer-beta", "scopeA", vec_b) ==
			KVS_VECTOR_OK,
		"v2 VSET beta"
	);
	failed |= expect_int(
		commit_prepared_vector("other", "answer-other", "scopeB", vec_c) ==
			KVS_VECTOR_OK,
		"v2 VSET other scope"
	);

	kvs_vector_search_results_reset(results, 4);
	failed |= expect_int(
		search_prepared_vector("alpha", "scopeA", vec_a, results, 4, &count) ==
			KVS_VECTOR_OK,
		"v2 VSEARCH alpha"
	);
	failed |= expect_int(result_has_question(results, count, "alpha"), "alpha searchable");
	failed |= expect_int(
		!result_has_question(results, count, "other"),
		"manager scope filter remains active"
	);

	failed |= expect_int(
		commit_prepared_vector("alpha", "answer-alpha-2", "scopeA", vec_a2) ==
			KVS_VECTOR_OK,
		"v2 prepared same-record tombstone re-add"
	);
	kvs_vector_search_result_reset(&get_result);
	failed |= expect_int(
		kvs_vector_get("alpha", strlen("alpha"), "scopeA", strlen("scopeA"), &get_result) ==
			KVS_VECTOR_OK,
		"VGET updated alpha"
	);
	failed |= expect_int(
		get_result.answer &&
			get_result.answer_len == strlen("answer-alpha-2") &&
			memcmp(get_result.answer, "answer-alpha-2", get_result.answer_len) == 0,
		"updated answer visible"
	);
	kvs_vector_search_results_reset(results, 4);
	failed |= expect_int(
		search_prepared_vector("alpha-new", "scopeA", vec_a2, results, 4, &count) ==
			KVS_VECTOR_OK,
		"v2 VSEARCH updated vector"
	);
	failed |= expect_int(result_has_question(results, count, "alpha"), "updated vector searchable");

	failed |= expect_int(
		kvs_vector_del("alpha", strlen("alpha"), "scopeA", strlen("scopeA")) ==
			KVS_VECTOR_OK,
		"v2 VDEL alpha"
	);
	(void)kvs_vector_del("alpha", strlen("alpha"), "scopeA", strlen("scopeA"));
	kvs_vector_search_results_reset(results, 4);
	failed |= expect_int(
		search_prepared_vector("alpha-new", "scopeA", vec_a2, results, 4, &count) ==
			KVS_VECTOR_OK,
		"v2 VSEARCH after delete"
	);
	failed |= expect_int(!result_has_question(results, count, "alpha"), "deleted alpha filtered");

	failed |= expect_int(kvs_vector_index_rebuild() == KVS_VECTOR_OK, "v2 VINDEX rebuild");
	memset(&info, 0, sizeof(info));
	failed |= expect_int(kvs_vector_info(&info) == KVS_VECTOR_OK, "v2 info after rebuild");
	failed |= expect_int(info.core_hnsw_v2_deleted_count == 0, "rebuild clears v2 tombstones");
	kvs_vector_search_results_reset(results, 4);
	failed |= expect_int(
		search_prepared_vector("beta", "scopeA", vec_b, results, 4, &count) ==
			KVS_VECTOR_OK,
		"v2 VSEARCH after rebuild"
	);
	failed |= expect_int(result_has_question(results, count, "beta"), "active beta after rebuild");
	failed |= expect_int(!result_has_question(results, count, "alpha"), "deleted alpha absent after rebuild");

	kvs_vector_destroy();
	return failed;
}

static int expect_rebuild_after_load_like_path(void) {
	const float vec_a[DIM] = {0.25f, 0.5f, 0.0f, 0.0f};
	const float vec_b[DIM] = {0.0f, 0.5f, 0.25f, 0.0f};
	kvs_vector_search_result_t results[2];
	size_t count = 0;
	int failed = 0;

	reset_config("core_hnsw_v2", 1);
	failed |= expect_int(kvs_vector_init() == KVS_VECTOR_OK, "load-like init");
	failed |= expect_int(
		commit_prepared_vector("load-a", "answer-a", "scopeL", vec_a) ==
			KVS_VECTOR_OK,
		"load-like VSET a"
	);
	failed |= expect_int(
		commit_prepared_vector("load-b", "answer-b", "scopeL", vec_b) ==
			KVS_VECTOR_OK,
		"load-like VSET b"
	);
	failed |= expect_int(kvs_vector_rebuild_after_load() == KVS_VECTOR_OK, "v2 rebuild after load");
	kvs_vector_search_results_reset(results, 2);
	failed |= expect_int(
		search_prepared_vector("load-a", "scopeL", vec_a, results, 2, &count) ==
			KVS_VECTOR_OK,
		"load-like search"
	);
	failed |= expect_int(result_has_question(results, count, "load-a"), "load-like active searchable");
	kvs_vector_destroy();
	return failed;
}

int main(void) {
	int failed = 0;

	mem_set_backend(KVS_MEM_BACKEND_SYSTEM);
	mem_init();

	failed |= expect_explicit_v2_ignores_legacy_flag();
	failed |= expect_enabled_product_path();
	failed |= expect_rebuild_after_load_like_path();

	mem_destroy();

	if (failed) {
		fprintf(stderr, "vector_core_hnsw_v2_product_integration_test FAILED\n");
		return 1;
	}

	printf("vector_core_hnsw_v2_product_integration_test PASS\n");
	return 0;
}
