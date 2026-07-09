#define KVS_VECTOR_INTERNAL
#include "kvs_config.h"
#include "kvs_memory.h"
#include "kvs_memory_internal.h"
#include "kvs_vector.h"
#include "vector/kvs_vector_manager.h"
#include "vector/kvs_vector_rebuild_shadow.h"

#include <math.h>
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

static void one_hot(float *vector, int dim, int hot) {
	int i;

	for (i = 0; i < dim; i++) {
		vector[i] = 0.0f;
	}
	if (hot >= 0 && hot < dim) {
		vector[hot] = 1.0f;
	}
}

static int commit_vector(
	const char *question,
	const char *answer,
	const char *scope,
	const float *vector,
	int dim
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
		dim,
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

static int product_search_top1(
	const char *query,
	const char *scope,
	const float *vector,
	int dim,
	kvs_vector_search_result_t *result
) {
	kvs_vector_search_prepared_t prepared;
	size_t result_count = 0;
	int ret;

	kvs_vector_search_prepared_reset(&prepared);
	kvs_vector_search_result_reset(result);
	ret = kvs_vector_search_prepare_with_embedding(
		query,
		strlen(query),
		scope,
		scope ? strlen(scope) : 0,
		vector,
		dim,
		&prepared
	);
	if (ret != KVS_VECTOR_OK) {
		kvs_vector_search_prepared_destroy(&prepared);
		return ret;
	}

	ret = kvs_vector_search_with_prepared(
		&prepared,
		1,
		-1.0f,
		result,
		1,
		&result_count
	);
	kvs_vector_search_prepared_destroy(&prepared);
	if (ret != KVS_VECTOR_OK) {
		return ret;
	}
	return result_count == 1 ? KVS_VECTOR_OK : KVS_VECTOR_ERR_NOT_FOUND;
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

static int expect_rebuild_error_contains(const char *needle, const char *label) {
	return expect_int(
		needle &&
			g_vector_state.rebuild.error_code != 0 &&
			strstr(g_vector_state.rebuild.error_message, needle) != NULL,
		label
	);
}

static uint64_t record_id_for_question(
	const char *question,
	const char *scope
) {
	kvs_vector_store_t *store = &g_vector_state.store;
	size_t scope_len = scope ? strlen(scope) : 0;
	size_t question_len = strlen(question);
	size_t i;

	for (i = 0; i < store->capacity; i++) {
		kvs_vector_record_t *record = &store->records[i];

		if (record->state != KVS_VECTOR_RECORD_ACTIVE || record->deleted) {
			continue;
		}
		if (record->question_len != question_len ||
		    memcmp(record->question, question, question_len) != 0) {
			continue;
		}
		if (record->scope_len != scope_len) {
			continue;
		}
		if (scope_len > 0 &&
		    memcmp(record->scope, scope, scope_len) != 0) {
			continue;
		}
		return record->id;
	}
	return 0;
}

static kvs_vector_record_t *record_by_id(uint64_t record_id) {
	kvs_vector_store_t *store = &g_vector_state.store;
	size_t i;

	for (i = 0; i < store->capacity; i++) {
		kvs_vector_record_t *record = &store->records[i];

		if (record->state == KVS_VECTOR_RECORD_ACTIVE &&
		    !record->deleted &&
		    record->id == record_id) {
			return record;
		}
	}
	return NULL;
}

static int test_only_update_record_embedding_and_capture(
	uint64_t record_id,
	const float *vector,
	int dim
) {
	kvs_vector_record_t *record = record_by_id(record_id);

	if (!record || !record->embedding || record->embedding_dim != dim) {
		return KVS_VECTOR_ERR_NOT_FOUND;
	}

	memcpy(record->embedding, vector, (size_t)dim * sizeof(float));
	record->generation = ++g_vector_state.generation;
	return kvs_vector_manager_capture_upsert_delta(record_id, vector, dim);
}

static uint64_t exact_active_top1_record_id(const float *query, int dim) {
	kvs_vector_store_t *store = &g_vector_state.store;
	uint64_t best_id = 0;
	float best_score = -INFINITY;
	size_t i;

	for (i = 0; i < store->capacity; i++) {
		kvs_vector_record_t *record = &store->records[i];
		float score = 0.0f;
		int d;

		if (record->state != KVS_VECTOR_RECORD_ACTIVE || record->deleted ||
		    !record->embedding || record->embedding_dim != dim) {
			continue;
		}

		for (d = 0; d < dim; d++) {
			score += record->embedding[d] * query[d];
		}
		if (best_id == 0 || score > best_score ||
		    (score == best_score && record->id < best_id)) {
			best_score = score;
			best_id = record->id;
		}
	}

	return best_id;
}

static int expect_shadow_top1(
	const float *query,
	int dim,
	uint64_t expected_record_id,
	const char *label
) {
	kvs_vector_hit_t hits[8];
	size_t out_count = 0;
	int ret;

	memset(hits, 0, sizeof(hits));
	ret = kvs_vector_rebuild_shadow_v2_search_for_test(
		query,
		(size_t)dim,
		4,
		32,
		hits,
		&out_count
	);
	if (ret != KVS_VECTOR_OK) {
		fprintf(stderr, "FAIL: %s search ret=%d\n", label, ret);
		return 1;
	}
	if (expected_record_id == 0) {
		return expect_int(out_count == 0, label);
	}
	return expect_int(
		out_count > 0 && hits[0].record_id == expected_record_id,
		label
	);
}

static int expect_product_top1_question(
	const float *query,
	int dim,
	const char *expected_question,
	const char *label
) {
	kvs_vector_search_result_t result;
	int ret;

	ret = product_search_top1("product-check", "scopeA", query, dim, &result);
	if (ret != KVS_VECTOR_OK) {
		fprintf(stderr, "FAIL: %s product search ret=%d\n", label, ret);
		return 1;
	}

	return expect_int(
		result.question_len == strlen(expected_question) &&
			memcmp(result.question, expected_question, result.question_len) == 0,
		label
	);
}

static int run_all_deleted_harness(void) {
	enum { DIM = 64 };
	float vec[DIM];
	kvs_vector_hit_t hits[4];
	size_t out_count = 0;
	uint64_t id;
	int failed = 0;

	failed |= expect_int(
		kvs_vector_reset_for_load() == KVS_VECTOR_OK,
		"all-deleted reset"
	);
	one_hot(vec, DIM, 4);
	failed |= expect_int(
		commit_vector("all-A", "answer", "scopeA", vec, DIM) == KVS_VECTOR_OK,
		"all-deleted insert"
	);
	id = record_id_for_question("all-A", "scopeA");
	failed |= expect_int(id != 0, "all-deleted id exists");

	failed |= expect_int(
		kvs_vector_rebuild_shadow_v2_init_for_test() == KVS_VECTOR_OK,
		"all-deleted shadow init"
	);
	failed |= expect_int(
		kvs_vector_rebuild_shadow_v2_build_from_active_records_for_test() ==
			KVS_VECTOR_OK,
		"all-deleted baseline build"
	);
	failed |= expect_int(
		kvs_vector_del("all-A", strlen("all-A"), "scopeA", strlen("scopeA")) ==
			KVS_VECTOR_OK,
		"all-deleted VDEL captures"
	);
	failed |= expect_int(
		kvs_vector_rebuild_shadow_v2_replay_deltas_for_test() == KVS_VECTOR_OK,
		"all-deleted replay"
	);
	memset(hits, 0, sizeof(hits));
	failed |= expect_int(
		kvs_vector_rebuild_shadow_v2_search_for_test(
			vec,
			DIM,
			4,
			32,
			hits,
			&out_count
		) == KVS_VECTOR_OK,
		"all-deleted shadow search succeeds"
	);
	failed |= expect_int(out_count == 0, "all-deleted shadow result empty");
	failed |= expect_int(
		kvs_vector_rebuild_shadow_v2_destroy_for_test() == KVS_VECTOR_OK,
		"all-deleted cleanup"
	);
	return failed;
}

static int run_failure_cleanup_harness(void) {
	enum { DIM = 64 };
	float vec[DIM];
	int failed = 0;

	failed |= expect_int(
		kvs_vector_reset_for_load() == KVS_VECTOR_OK,
		"failure reset"
	);
	one_hot(vec, DIM, 5);
	failed |= expect_int(
		commit_vector("fail-A", "answer", "scopeA", vec, DIM) == KVS_VECTOR_OK,
		"failure insert"
	);
	failed |= expect_int(
		kvs_vector_rebuild_shadow_v2_init_for_test() == KVS_VECTOR_OK,
		"failure shadow init"
	);
	failed |= expect_int(
		kvs_vector_rebuild_shadow_v2_build_from_active_records_for_test() ==
			KVS_VECTOR_OK,
		"failure baseline build"
	);
	failed |= expect_int(
		kvs_vector_manager_capture_upsert_delta(12345, vec, DIM) ==
			KVS_VECTOR_OK,
		"failure delta one"
	);
	failed |= expect_int(
		kvs_vector_manager_capture_upsert_delta(12346, vec, DIM) ==
			KVS_VECTOR_OK,
		"failure delta two"
	);
	g_vector_state.rebuild.delta_log[0].generation = 9999;
	g_vector_state.rebuild.delta_log[1].generation = 1;
	failed |= expect_int(
		kvs_vector_rebuild_shadow_v2_replay_deltas_for_test() != KVS_VECTOR_OK,
		"failure replay detects generation order"
	);
	failed |= expect_int(
		g_vector_state.rebuild.state == KVS_VECTOR_REBUILD_FAILED,
		"failure enters failed state"
	);
	failed |= expect_int(
		!kvs_vector_rebuild_shadow_v2_shadow_exists_for_test(),
		"failure destroys shadow"
	);
	failed |= expect_product_top1_question(
		vec,
		DIM,
		"fail-A",
		"failure active product index unchanged"
	);
	failed |= expect_int(
		kvs_vector_rebuild_shadow_v2_destroy_for_test() == KVS_VECTOR_OK,
		"failure cleanup returns idle"
	);
	failed |= expect_int(
		g_vector_state.rebuild.state == KVS_VECTOR_REBUILD_IDLE,
		"failure cleanup idle"
	);
	return failed;
}

static int run_final_validation_failure_harness(void) {
	enum { DIM = 64 };
	float base_vec[DIM];
	float fake_vec[DIM];
	const uint64_t fake_record_id = 4242424242ULL;
	int failed = 0;

	failed |= expect_int(
		kvs_vector_reset_for_load() == KVS_VECTOR_OK,
		"validation failure reset"
	);
	one_hot(base_vec, DIM, 11);
	one_hot(fake_vec, DIM, 12);
	failed |= expect_int(
		commit_vector("validation-base", "answer", "scopeA", base_vec, DIM) ==
			KVS_VECTOR_OK,
		"validation failure insert base"
	);
	failed |= expect_int(
		kvs_vector_rebuild_shadow_v2_init_for_test() == KVS_VECTOR_OK,
		"validation failure shadow init"
	);
	failed |= expect_int(
		kvs_vector_rebuild_shadow_v2_build_from_active_records_for_test() ==
			KVS_VECTOR_OK,
		"validation failure baseline build"
	);
	failed |= expect_int(
		kvs_vector_manager_capture_upsert_delta(
			fake_record_id,
			fake_vec,
			DIM
		) == KVS_VECTOR_OK,
		"validation failure fake delta append"
	);
	failed |= expect_int(
		kvs_vector_rebuild_shadow_v2_replay_deltas_for_test() != KVS_VECTOR_OK,
		"validation failure replay rejected"
	);
	failed |= expect_int(
		g_vector_state.rebuild.state == KVS_VECTOR_REBUILD_FAILED,
		"validation failure enters failed"
	);
	failed |= expect_rebuild_error_contains(
		"shadow active id not in manager",
		"validation failure reason observable"
	);
	failed |= expect_int(
		!kvs_vector_rebuild_shadow_v2_shadow_exists_for_test(),
		"validation failure destroys shadow"
	);
	failed |= expect_int(
		kvs_vector_rebuild_swap_ready_shadow_for_test() != KVS_VECTOR_OK,
		"validation failure cannot publish"
	);
	failed |= expect_product_top1_question(
		base_vec,
		DIM,
		"validation-base",
		"validation failure product active unchanged"
	);
	failed |= expect_int(
		kvs_vector_rebuild_shadow_v2_destroy_for_test() == KVS_VECTOR_OK,
		"validation failure cleanup"
	);
	failed |= expect_int(
		g_vector_state.rebuild.state == KVS_VECTOR_REBUILD_IDLE,
		"validation failure cleanup idle"
	);
	return failed;
}

static int run_pre_scan_capture_boundary_harness(void) {
	enum { DIM = 64 };
	float vec_base[DIM];
	float vec_delta[DIM];
	uint64_t delta_id;
	uint64_t rebuild_generation_start;
	size_t replayed_count = 0;
	int failed = 0;

	failed |= expect_int(
		kvs_vector_reset_for_load() == KVS_VECTOR_OK,
		"pre-scan boundary reset"
	);
	one_hot(vec_base, DIM, 7);
	one_hot(vec_delta, DIM, 8);
	failed |= expect_int(
		commit_vector("boundary-base", "answer", "scopeA", vec_base, DIM) ==
			KVS_VECTOR_OK,
		"pre-scan boundary insert base"
	);
	failed |= expect_int(
		kvs_vector_rebuild_shadow_v2_init_for_test() == KVS_VECTOR_OK,
		"pre-scan boundary shadow init"
	);
	rebuild_generation_start = g_vector_state.rebuild.rebuild_generation;
	failed |= expect_int(
		kvs_vector_rebuild_is_capture_active(&g_vector_state.rebuild),
		"pre-scan boundary capture active before full scan"
	);
	failed |= expect_int(
		commit_vector("boundary-delta", "answer", "scopeA", vec_delta, DIM) ==
			KVS_VECTOR_OK,
		"pre-scan boundary VSET succeeds"
	);
	delta_id = record_id_for_question("boundary-delta", "scopeA");
	failed |= expect_int(delta_id != 0, "pre-scan boundary delta id exists");
	failed |= expect_int(
		g_vector_state.rebuild.delta_count == 1,
		"pre-scan boundary delta captured before full scan"
	);
	failed |= expect_int(
		g_vector_state.rebuild.delta_log[0].generation >
			rebuild_generation_start,
		"pre-scan boundary delta generation follows start"
	);
	failed |= expect_int(
		kvs_vector_rebuild_shadow_v2_build_from_active_records_for_test() ==
			KVS_VECTOR_OK,
		"pre-scan boundary baseline build"
	);
	failed |= expect_int(
		kvs_vector_rebuild_shadow_v2_replay_deltas_for_test() == KVS_VECTOR_OK,
		"pre-scan boundary replay"
	);
	failed |= expect_int(
		kvs_vector_rebuild_shadow_v2_replayed_delta_count_for_test(
			&replayed_count
		) == KVS_VECTOR_OK,
		"pre-scan boundary replayed count query"
	);
	failed |= expect_int(replayed_count == 1, "pre-scan boundary one delta replayed");
	failed |= expect_shadow_top1(
		vec_delta,
		DIM,
		delta_id,
		"pre-scan boundary shadow finds delta record"
	);
	failed |= expect_int(
		kvs_vector_rebuild_shadow_v2_destroy_for_test() == KVS_VECTOR_OK,
		"pre-scan boundary cleanup"
	);
	return failed;
}

int main(void) {
	enum { DIM = 64 };
	float vec_a0[DIM];
	float vec_a1[DIM];
	float vec_a2[DIM];
	float vec_b[DIM];
	float vec_c[DIM];
	float vec_d[DIM];
	float vec_missing[DIM];
	kvs_vector_core_info_t info;
	kvs_vector_hit_t hits[8];
	size_t out_count = 0;
	size_t active_count = 0;
	size_t missing_delete_count = 0;
	size_t replayed_count = 0;
	uint64_t id_a;
	uint64_t id_b;
	uint64_t id_c;
	uint64_t id_d;
	uint64_t exact_id;
	size_t delta_count_before_replay;
	int failed = 0;

	memset(&g_cfg, 0, sizeof(g_cfg));
	kvs_config_init_default(&g_cfg);
	g_cfg.mem_backend = KVS_MEM_BACKEND_SYSTEM;
	snprintf(
		g_cfg.vector_index_type,
		sizeof(g_cfg.vector_index_type),
		"%s",
		"core_flat"
	);
	mem_set_backend(KVS_MEM_BACKEND_SYSTEM);
	mem_init();

	one_hot(vec_a0, DIM, 0);
	one_hot(vec_a1, DIM, 10);
	one_hot(vec_a2, DIM, 20);
	one_hot(vec_b, DIM, 1);
	one_hot(vec_c, DIM, 2);
	one_hot(vec_d, DIM, 3);
	one_hot(vec_missing, DIM, 6);

	failed |= expect_int(kvs_vector_init() == KVS_VECTOR_OK, "vector init");
	failed |= expect_int(
		commit_vector("A", "answer-A0", "scopeA", vec_a0, DIM) == KVS_VECTOR_OK,
		"insert A"
	);
	failed |= expect_int(
		commit_vector("B", "answer-B", "scopeA", vec_b, DIM) == KVS_VECTOR_OK,
		"insert B"
	);
	failed |= expect_int(
		commit_vector("C", "answer-C", "scopeA", vec_c, DIM) == KVS_VECTOR_OK,
		"insert C"
	);
	failed |= expect_int(kvs_vector_index_rebuild() == KVS_VECTOR_OK, "product rebuild");

	id_a = record_id_for_question("A", "scopeA");
	id_b = record_id_for_question("B", "scopeA");
	id_c = record_id_for_question("C", "scopeA");
	failed |= expect_int(id_a != 0 && id_b != 0 && id_c != 0, "initial ids exist");

	failed |= expect_int(
		kvs_vector_rebuild_shadow_v2_init_for_test() == KVS_VECTOR_OK,
		"shadow init"
	);
	failed |= expect_int(
		g_vector_state.rebuild.state == KVS_VECTOR_REBUILD_BUILDING,
		"state building"
	);
	failed |= expect_int(kvs_vector_rebuild_shadow_v2_shadow_exists_for_test(), "shadow exists");
	failed |= expect_int(
		kvs_vector_rebuild_shadow_v2_build_from_active_records_for_test() ==
			KVS_VECTOR_OK,
		"baseline build"
	);
	failed |= expect_int(
		g_vector_state.rebuild.state == KVS_VECTOR_REBUILD_REPLAYING_DELTA,
		"state replaying_delta"
	);
	failed |= expect_shadow_top1(vec_a0, DIM, id_a, "baseline finds A");
	failed |= expect_shadow_top1(vec_b, DIM, id_b, "baseline finds B");

	failed |= expect_int(
		commit_vector("D", "answer-D", "scopeA", vec_d, DIM) == KVS_VECTOR_OK,
		"capture insert D"
	);
	id_d = record_id_for_question("D", "scopeA");
	failed |= expect_int(id_d != 0, "D id exists");
	failed |= expect_int(
		test_only_update_record_embedding_and_capture(id_a, vec_a1, DIM) ==
			KVS_VECTOR_OK,
		"capture update A once"
	);
	failed |= expect_int(
		kvs_vector_del("B", strlen("B"), "scopeA", strlen("scopeA")) ==
			KVS_VECTOR_OK,
		"capture delete B"
	);
	failed |= expect_int(
		test_only_update_record_embedding_and_capture(id_a, vec_a2, DIM) ==
			KVS_VECTOR_OK,
		"capture update A twice"
	);
	failed |= expect_int(
		kvs_vector_manager_capture_delete_delta(999999) == KVS_VECTOR_OK,
		"manual missing delete delta"
	);
	delta_count_before_replay = g_vector_state.rebuild.delta_count;
	failed |= expect_int(delta_count_before_replay == 5, "expected five deltas");

	failed |= expect_int(
		kvs_vector_rebuild_shadow_v2_replay_deltas_for_test() == KVS_VECTOR_OK,
		"delta replay"
	);
	failed |= expect_int(
		g_vector_state.rebuild.state == KVS_VECTOR_REBUILD_READY_TO_SWAP,
		"state ready_to_swap"
	);
	failed |= expect_int(
		kvs_vector_rebuild_shadow_v2_replayed_delta_count_for_test(&replayed_count) ==
			KVS_VECTOR_OK,
		"replayed count query"
	);
	failed |= expect_int(replayed_count == delta_count_before_replay, "all deltas replayed");
	failed |= expect_int(
		kvs_vector_rebuild_shadow_v2_missing_delete_count_for_test(
			&missing_delete_count
		) == KVS_VECTOR_OK,
		"missing delete count query"
	);
	failed |= expect_int(missing_delete_count == 1, "missing delete counted");

	failed |= expect_int(
		kvs_vector_rebuild_shadow_v2_info_for_test(&info) == KVS_VECTOR_OK,
		"shadow info"
	);
	failed |= expect_int(info.size == 6, "shadow physical slots include tombstones");
	failed |= expect_int(info.deleted_count == 3, "shadow deleted count includes old A and B");
	failed |= expect_int(
		kvs_vector_rebuild_shadow_v2_active_record_count_for_test(&active_count) ==
			KVS_VECTOR_OK,
		"manager active count query"
	);
	failed |= expect_int(active_count == 3, "manager active count after replay inputs");
	failed |= expect_int(
		info.size >= info.deleted_count &&
			info.size - info.deleted_count == active_count,
		"shadow visible count matches manager active count"
	);

	exact_id = exact_active_top1_record_id(vec_a2, DIM);
	failed |= expect_int(exact_id == id_a, "exact oracle latest A");
	failed |= expect_shadow_top1(vec_a2, DIM, id_a, "shadow latest A wins");
	failed |= expect_shadow_top1(vec_d, DIM, id_d, "shadow finds D");
	memset(hits, 0, sizeof(hits));
	failed |= expect_int(
		kvs_vector_rebuild_shadow_v2_search_for_test(
			vec_b,
			DIM,
			8,
			32,
			hits,
			&out_count
		) == KVS_VECTOR_OK,
		"shadow search deleted B"
	);
	failed |= expect_int(!find_hit(hits, out_count, id_b), "shadow does not return B");

	failed |= expect_product_top1_question(
		vec_d,
		DIM,
		"D",
		"VSEARCH still reads current product index"
	);
	failed |= expect_int(
		kvs_vector_index_rebuild() == KVS_VECTOR_OK,
		"VINDEX runtime behavior still works"
	);
	failed |= expect_int(
		g_vector_state.rebuild.state == KVS_VECTOR_REBUILD_READY_TO_SWAP,
		"VINDEX does not consume shadow state"
	);
	failed |= expect_int(
		kvs_vector_rebuild_shadow_v2_destroy_for_test() == KVS_VECTOR_OK,
		"shadow cleanup"
	);
	failed |= expect_int(
		g_vector_state.rebuild.state == KVS_VECTOR_REBUILD_IDLE,
		"cleanup idle"
	);
	failed |= expect_int(!kvs_vector_rebuild_shadow_v2_shadow_exists_for_test(), "shadow cleared");

	failed |= run_all_deleted_harness();
	failed |= run_failure_cleanup_harness();
	failed |= run_final_validation_failure_harness();
	failed |= run_pre_scan_capture_boundary_harness();

	failed |= expect_int(
		kvs_vector_reset_for_load() == KVS_VECTOR_OK,
		"reset-for-load cleanup"
	);
	failed |= expect_int(
		g_vector_state.rebuild.state == KVS_VECTOR_REBUILD_IDLE,
		"reset-for-load rebuild idle"
	);
	failed |= expect_int(
		g_vector_state.rebuild.shadow_index == NULL,
		"reset-for-load shadow cleared"
	);

	kvs_vector_destroy();
	failed |= expect_int(
		g_vector_state.rebuild.shadow_index == NULL,
		"destroy shadow cleared"
	);
	failed |= expect_int(g_vector_state.initialized == 0, "destroy clears initialized");
	mem_destroy();

	if (failed) {
		fprintf(stderr, "vector_rebuild_shadow_replay_test FAILED\n");
		return 1;
	}

	printf("vector_rebuild_shadow_replay_test PASS\n");
	return 0;
}
