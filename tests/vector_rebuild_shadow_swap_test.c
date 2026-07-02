#define KVS_VECTOR_INTERNAL
#include "kvs_config.h"
#include "kvs_memory.h"
#include "kvs_memory_internal.h"
#include "kvs_vector.h"
#include "vector/kvs_vector_manager.h"
#include "vector/kvs_vector_rebuild_shadow.h"

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
	const float *vector,
	int dim,
	const char *expected_question,
	const char *label
) {
	kvs_vector_search_prepared_t prepared;
	kvs_vector_search_result_t result;
	size_t result_count = 0;
	int ret;

	kvs_vector_search_prepared_reset(&prepared);
	kvs_vector_search_result_reset(&result);
	ret = kvs_vector_search_prepare_with_embedding(
		"swap-product-check",
		strlen("swap-product-check"),
		"scopeA",
		strlen("scopeA"),
		vector,
		dim,
		&prepared
	);
	if (ret != KVS_VECTOR_OK) {
		kvs_vector_search_prepared_destroy(&prepared);
		fprintf(stderr, "FAIL: %s prepare ret=%d\n", label, ret);
		return 1;
	}

	ret = kvs_vector_search_with_prepared(
		&prepared,
		1,
		-1.0f,
		&result,
		1,
		&result_count
	);
	kvs_vector_search_prepared_destroy(&prepared);
	if (ret != KVS_VECTOR_OK || result_count != 1) {
		fprintf(stderr, "FAIL: %s search ret=%d count=%zu\n", label, ret, result_count);
		return 1;
	}

	return expect_int(
		result.question_len == strlen(expected_question) &&
			memcmp(result.question, expected_question, result.question_len) == 0,
		label
	);
}

static uint64_t record_id_for_question(
	const char *question,
	const char *scope
) {
	kvs_vector_store_t *store = &g_vector_state.store;
	size_t question_len = strlen(question);
	size_t scope_len = scope ? strlen(scope) : 0;
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

static int expect_shadow_hit(
	int use_active,
	const float *query,
	int dim,
	uint64_t expected_id,
	const char *label
) {
	kvs_vector_hit_t hits[8];
	size_t out_count = 0;
	int ret;

	memset(hits, 0, sizeof(hits));
	if (use_active) {
		ret = kvs_vector_rebuild_shadow_v2_active_search_for_test(
			query,
			(size_t)dim,
			4,
			32,
			hits,
			&out_count
		);
	} else {
		ret = kvs_vector_rebuild_shadow_v2_search_for_test(
			query,
			(size_t)dim,
			4,
			32,
			hits,
			&out_count
		);
	}

	if (ret != KVS_VECTOR_OK) {
		fprintf(stderr, "FAIL: %s ret=%d\n", label, ret);
		return 1;
	}

	return expect_int(
		out_count > 0 && hits[0].record_id == expected_id,
		label
	);
}

static int reach_ready_with_delta(
	const float *base_vec,
	const float *delta_vec,
	int dim,
	uint64_t *out_delta_id
) {
	int ret;

	ret = commit_vector("swap-base", "base", "scopeA", base_vec, dim);
	if (ret != KVS_VECTOR_OK) {
		return ret;
	}
	ret = kvs_vector_index_rebuild();
	if (ret != KVS_VECTOR_OK) {
		return ret;
	}
	ret = kvs_vector_rebuild_shadow_v2_init_for_test();
	if (ret != KVS_VECTOR_OK) {
		return ret;
	}
	ret = kvs_vector_rebuild_shadow_v2_build_from_active_records_for_test();
	if (ret != KVS_VECTOR_OK) {
		return ret;
	}
	ret = commit_vector("swap-shadow-only", "shadow", "scopeA", delta_vec, dim);
	if (ret != KVS_VECTOR_OK) {
		return ret;
	}
	if (out_delta_id) {
		*out_delta_id = record_id_for_question("swap-shadow-only", "scopeA");
	}
	return kvs_vector_rebuild_shadow_v2_replay_deltas_for_test();
}

static int reach_ready_with_shadow_only_delta(
	const float *base_vec,
	const float *shadow_vec,
	int dim,
	uint64_t shadow_only_id
) {
	int ret;

	ret = commit_vector("swap-base", "base", "scopeA", base_vec, dim);
	if (ret != KVS_VECTOR_OK) {
		return ret;
	}
	ret = kvs_vector_index_rebuild();
	if (ret != KVS_VECTOR_OK) {
		return ret;
	}
	ret = kvs_vector_rebuild_shadow_v2_init_for_test();
	if (ret != KVS_VECTOR_OK) {
		return ret;
	}
	ret = kvs_vector_rebuild_shadow_v2_build_from_active_records_for_test();
	if (ret != KVS_VECTOR_OK) {
		return ret;
	}
	ret = kvs_vector_manager_capture_upsert_delta(
		shadow_only_id,
		shadow_vec,
		dim
	);
	if (ret != KVS_VECTOR_OK) {
		return ret;
	}

	return kvs_vector_rebuild_shadow_v2_replay_deltas_for_test();
}

static int run_successful_swap_harness(void) {
	enum { DIM = 64 };
	float base_vec[DIM];
	float delta_vec[DIM];
	uint64_t delta_id = 9000001;
	int failed = 0;

	failed |= expect_int(kvs_vector_reset_for_load() == KVS_VECTOR_OK, "success reset");
	one_hot(base_vec, DIM, 4);
	one_hot(delta_vec, DIM, 17);

	failed |= expect_int(
		reach_ready_with_shadow_only_delta(base_vec, delta_vec, DIM, delta_id) ==
			KVS_VECTOR_OK,
		"success reaches ready"
	);
	failed |= expect_int(
		g_vector_state.rebuild.state == KVS_VECTOR_REBUILD_READY_TO_SWAP,
		"success state ready"
	);
	failed |= expect_int(
		kvs_vector_rebuild_shadow_v2_shadow_exists_for_test(),
		"success shadow exists before swap"
	);
	failed |= expect_int(
		!kvs_vector_rebuild_shadow_v2_active_exists_for_test() ||
			g_vector_state.rebuild.active_index == &g_vector_state.current_index,
		"success active placeholder is product current before swap"
	);
	failed |= expect_shadow_hit(0, delta_vec, DIM, delta_id, "shadow sees delta before swap");

	failed |= expect_int(
		kvs_vector_rebuild_swap_ready_shadow_for_test() == KVS_VECTOR_OK,
		"ready shadow swap succeeds"
	);
	failed |= expect_int(
		g_vector_state.rebuild.state == KVS_VECTOR_REBUILD_IDLE,
		"swap completes to idle"
	);
	failed |= expect_int(
		!kvs_vector_rebuild_shadow_v2_shadow_exists_for_test(),
		"swap clears shadow"
	);
	failed |= expect_int(
		kvs_vector_rebuild_shadow_v2_active_exists_for_test(),
		"swap stores rebuild active"
	);
	failed |= expect_int(
		kvs_vector_rebuild_shadow_v2_retired_exists_for_test(),
		"swap stores retired old active placeholder"
	);
	failed |= expect_int(
		g_vector_state.rebuild.delta_count == 0,
		"swap clears delta log"
	);
	failed |= expect_shadow_hit(1, delta_vec, DIM, delta_id, "rebuild active sees promoted shadow");

	failed |= product_search_top1(
		delta_vec,
		DIM,
		"swap-base",
		"product VSEARCH still uses current index after swap"
	);
	failed |= expect_int(
		kvs_vector_index_rebuild() == KVS_VECTOR_OK,
		"VINDEX runtime still succeeds after swap"
	);
	failed |= expect_shadow_hit(1, delta_vec, DIM, delta_id, "VINDEX does not consume rebuild active");

	failed |= expect_int(
		kvs_vector_rebuild_retire_cleanup_for_test() == KVS_VECTOR_OK,
		"retired cleanup succeeds"
	);
	failed |= expect_int(
		!kvs_vector_rebuild_shadow_v2_retired_exists_for_test(),
		"retired cleanup clears retired"
	);
	failed |= expect_int(
		kvs_vector_rebuild_retire_cleanup_for_test() == KVS_VECTOR_OK,
		"retired cleanup idempotent"
	);

	failed |= expect_int(
		kvs_vector_reset_for_load() == KVS_VECTOR_OK,
		"reset-for-load after swap succeeds"
	);
	failed |= expect_int(
		g_vector_state.rebuild.active_index == NULL &&
			g_vector_state.rebuild.shadow_index == NULL &&
			g_vector_state.rebuild.retired_index == NULL,
		"reset-for-load clears active shadow retired"
	);

	return failed;
}

static int run_failure_before_swap_harness(void) {
	enum { DIM = 64 };
	float base_vec[DIM];
	float delta_vec[DIM];
	void *active_before;
	int failed = 0;

	failed |= expect_int(kvs_vector_reset_for_load() == KVS_VECTOR_OK, "failure reset");
	one_hot(base_vec, DIM, 8);
	one_hot(delta_vec, DIM, 19);
	failed |= expect_int(
		reach_ready_with_delta(base_vec, delta_vec, DIM, NULL) == KVS_VECTOR_OK,
		"failure reaches ready"
	);
	active_before = g_vector_state.rebuild.active_index;
	failed |= expect_int(active_before == &g_vector_state.current_index, "failure active is product placeholder");
	failed |= expect_int(
		kvs_vector_rebuild_force_swap_failure_for_test() == KVS_VECTOR_OK,
		"forced swap failure succeeds"
	);
	failed |= expect_int(
		g_vector_state.rebuild.state == KVS_VECTOR_REBUILD_FAILED,
		"forced failure enters failed"
	);
	failed |= expect_int(
		g_vector_state.rebuild.active_index == active_before,
		"forced failure preserves active placeholder"
	);
	failed |= expect_int(
		!kvs_vector_rebuild_shadow_v2_shadow_exists_for_test(),
		"forced failure destroys shadow"
	);
	failed |= expect_int(
		g_vector_state.rebuild.retired_index == NULL,
		"forced failure does not create retired"
	);
	failed |= expect_int(
		g_vector_state.rebuild.error_code == KVS_VECTOR_ERR,
		"forced failure records error"
	);
	failed |= product_search_top1(
		base_vec,
		DIM,
		"swap-base",
		"product index unchanged after forced failure"
	);
	failed |= expect_int(
		kvs_vector_rebuild_shadow_v2_destroy_for_test() == KVS_VECTOR_OK,
		"forced failure cleanup succeeds"
	);
	failed |= expect_int(
		g_vector_state.rebuild.state == KVS_VECTOR_REBUILD_IDLE,
		"forced failure cleanup idle"
	);

	return failed;
}

static int run_cancel_cleanup_harness(void) {
	enum { DIM = 64 };
	float base_vec[DIM];
	int failed = 0;

	failed |= expect_int(kvs_vector_reset_for_load() == KVS_VECTOR_OK, "cancel reset");
	one_hot(base_vec, DIM, 10);
	failed |= expect_int(
		commit_vector("cancel-base", "base", "scopeA", base_vec, DIM) ==
			KVS_VECTOR_OK,
		"cancel base insert"
	);
	failed |= expect_int(
		kvs_vector_rebuild_shadow_v2_init_for_test() == KVS_VECTOR_OK,
		"cancel shadow init"
	);
	failed |= expect_int(
		kvs_vector_rebuild_shadow_v2_shadow_exists_for_test(),
		"cancel shadow exists"
	);
	failed |= expect_int(
		kvs_vector_rebuild_shadow_v2_destroy_for_test() == KVS_VECTOR_OK,
		"cancel cleanup succeeds"
	);
	failed |= expect_int(
		g_vector_state.rebuild.state == KVS_VECTOR_REBUILD_IDLE,
		"cancel cleanup idle"
	);
	failed |= expect_int(
		g_vector_state.rebuild.shadow_index == NULL,
		"cancel cleanup clears shadow"
	);

	return failed;
}

static int run_destroy_cleanup_harness(void) {
	enum { DIM = 64 };
	float base_vec[DIM];
	float delta_vec[DIM];
	int failed = 0;

	failed |= expect_int(kvs_vector_reset_for_load() == KVS_VECTOR_OK, "destroy reset");
	one_hot(base_vec, DIM, 12);
	one_hot(delta_vec, DIM, 21);
	failed |= expect_int(
		reach_ready_with_delta(base_vec, delta_vec, DIM, NULL) == KVS_VECTOR_OK,
		"destroy reaches ready"
	);
	failed |= expect_int(
		kvs_vector_rebuild_swap_ready_shadow_for_test() == KVS_VECTOR_OK,
		"destroy swap succeeds"
	);
	failed |= expect_int(
		kvs_vector_rebuild_shadow_v2_active_exists_for_test(),
		"destroy has rebuild active before destroy"
	);
	kvs_vector_destroy();
	failed |= expect_int(g_vector_state.initialized == 0, "destroy clears initialized");
	failed |= expect_int(
		g_vector_state.rebuild.active_index == NULL &&
			g_vector_state.rebuild.shadow_index == NULL &&
			g_vector_state.rebuild.retired_index == NULL &&
			g_vector_state.rebuild.delta_log == NULL,
		"destroy clears active shadow retired deltas"
	);
	failed |= expect_int(kvs_vector_init() == KVS_VECTOR_OK, "reinit after destroy");

	return failed;
}

static int run_state_gate_harness(void) {
	enum { DIM = 64 };
	float vec[DIM];
	kvs_vector_rebuild_context_t *ctx = &g_vector_state.rebuild;
	int failed = 0;

	failed |= expect_int(kvs_vector_reset_for_load() == KVS_VECTOR_OK, "gate reset");
	one_hot(vec, DIM, 3);
	failed |= expect_int(
		kvs_vector_rebuild_swap_ready_shadow_for_test() != KVS_VECTOR_OK,
		"idle to swap rejected"
	);
	failed |= expect_int(ctx->state == KVS_VECTOR_REBUILD_IDLE, "idle rejected preserves state");

	failed |= expect_int(
		kvs_vector_rebuild_shadow_v2_init_for_test() == KVS_VECTOR_OK,
		"gate begin building"
	);
	failed |= expect_int(
		kvs_vector_rebuild_swap_ready_shadow_for_test() != KVS_VECTOR_OK,
		"building to swap rejected"
	);
	failed |= expect_int(ctx->state == KVS_VECTOR_REBUILD_BUILDING, "building rejected preserves state");

	failed |= expect_int(
		kvs_vector_rebuild_shadow_v2_build_from_active_records_for_test() ==
			KVS_VECTOR_OK,
		"gate enter replaying"
	);
	failed |= expect_int(
		kvs_vector_rebuild_swap_ready_shadow_for_test() != KVS_VECTOR_OK,
		"replaying to swap rejected"
	);
	failed |= expect_int(
		ctx->state == KVS_VECTOR_REBUILD_REPLAYING_DELTA,
		"replaying rejected preserves state"
	);

	failed |= expect_int(
		kvs_vector_rebuild_fail(ctx, KVS_VECTOR_ERR, "gate failure") ==
			KVS_VECTOR_OK,
		"gate fail"
	);
	failed |= expect_int(
		kvs_vector_rebuild_swap_ready_shadow_for_test() != KVS_VECTOR_OK,
		"failed to swap rejected"
	);
	failed |= expect_int(
		kvs_vector_rebuild_reset_idle(ctx) == KVS_VECTOR_OK,
		"gate failed reset"
	);

	failed |= expect_int(
		kvs_vector_rebuild_shadow_v2_init_for_test() == KVS_VECTOR_OK,
		"gate begin cancel"
	);
	failed |= expect_int(
		kvs_vector_rebuild_cancel(ctx) == KVS_VECTOR_OK,
		"gate cancel"
	);
	failed |= expect_int(
		kvs_vector_rebuild_swap_ready_shadow_for_test() != KVS_VECTOR_OK,
		"cancelled to swap rejected"
	);
	failed |= expect_int(
		kvs_vector_rebuild_reset_idle(ctx) == KVS_VECTOR_OK,
		"gate cancelled reset"
	);

	return failed;
}

int main(void) {
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

	failed |= expect_int(kvs_vector_init() == KVS_VECTOR_OK, "vector init");
	failed |= run_state_gate_harness();
	failed |= run_successful_swap_harness();
	failed |= run_failure_before_swap_harness();
	failed |= run_cancel_cleanup_harness();
	failed |= run_destroy_cleanup_harness();

	kvs_vector_destroy();
	failed |= expect_int(g_vector_state.initialized == 0, "final destroy initialized clear");
	mem_destroy();

	if (failed) {
		fprintf(stderr, "vector_rebuild_shadow_swap_test FAILED\n");
		return 1;
	}

	printf("vector_rebuild_shadow_swap_test PASS\n");
	return 0;
}
