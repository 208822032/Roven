#define KVS_VECTOR_INTERNAL
#include "kvs_config.h"
#include "kvs_memory.h"
#include "kvs_memory_internal.h"
#include "kvs_vector.h"
#include "vector/kvs_vector_rebuild.h"

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

static int expect_float_eq(float actual, float expected, const char *label) {
	float diff = actual - expected;

	if (diff < 0.0f) {
		diff = -diff;
	}
	return expect_int(diff < 0.00001f, label);
}

static void fill_vector(float *vector, int dim, float seed) {
	int i;

	for (i = 0; i < dim; i++) {
		vector[i] = seed + (float)i * 0.001f;
	}
}

static int commit_prepared_vector(
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

static int search_one(
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

static int expect_capture_state_policy(void) {
	kvs_vector_rebuild_context_t ctx;
	int failed = 0;

	memset(&ctx, 0, sizeof(ctx));
	failed |= expect_int(
		kvs_vector_rebuild_context_init(&ctx) == KVS_VECTOR_OK,
		"policy context init"
	);

	ctx.state = KVS_VECTOR_REBUILD_IDLE;
	failed |= expect_int(
		!kvs_vector_rebuild_is_capture_active(&ctx),
		"idle capture inactive"
	);
	ctx.state = KVS_VECTOR_REBUILD_BUILDING;
	failed |= expect_int(
		kvs_vector_rebuild_is_capture_active(&ctx),
		"building capture active"
	);
	ctx.state = KVS_VECTOR_REBUILD_REPLAYING_DELTA;
	failed |= expect_int(
		kvs_vector_rebuild_is_capture_active(&ctx),
		"replaying_delta capture active"
	);
	ctx.state = KVS_VECTOR_REBUILD_READY_TO_SWAP;
	failed |= expect_int(
		kvs_vector_rebuild_is_capture_active(&ctx),
		"ready_to_swap capture active"
	);
	ctx.state = KVS_VECTOR_REBUILD_SWAPPING;
	failed |= expect_int(
		!kvs_vector_rebuild_is_capture_active(&ctx),
		"swapping capture inactive"
	);
	ctx.state = KVS_VECTOR_REBUILD_FAILED;
	failed |= expect_int(
		!kvs_vector_rebuild_is_capture_active(&ctx),
		"failed capture inactive"
	);
	ctx.state = KVS_VECTOR_REBUILD_CANCELLED;
	failed |= expect_int(
		!kvs_vector_rebuild_is_capture_active(&ctx),
		"cancelled capture inactive"
	);

	kvs_vector_rebuild_context_destroy(&ctx);
	return failed;
}

static int expect_bottom_append_failure(void) {
	kvs_vector_rebuild_context_t ctx;
	float vector[4] = {1.0f, 2.0f, 3.0f, 4.0f};
	int failed = 0;

	memset(&ctx, 0, sizeof(ctx));
	failed |= expect_int(
		kvs_vector_rebuild_context_init(&ctx) == KVS_VECTOR_OK,
		"failure context init"
	);
	ctx.current_generation = UINT64_MAX;
	failed |= expect_int(
		kvs_vector_rebuild_append_upsert_delta(&ctx, 9001, vector, 4) !=
			KVS_VECTOR_OK,
		"generation overflow append fails"
	);
	failed |= expect_int(ctx.delta_count == 0, "failed append preserves delta count");
	failed |= expect_int(
		ctx.current_generation == UINT64_MAX,
		"failed append preserves generation"
	);
	kvs_vector_rebuild_context_destroy(&ctx);
	return failed;
}

int main(void) {
	enum { DIM = 64 };
	float vec_idle[DIM];
	float vec_a[DIM];
	float vec_b[DIM];
	float vec_manual[DIM];
	kvs_vector_rebuild_context_t *ctx;
	kvs_vector_search_result_t result;
	uint64_t record_id = 0;
	uint64_t generation0;
	uint64_t generation1;
	uint64_t generation2;
	uint64_t generation3;
	size_t before_search_delta_count;
	size_t before_inactive_count;
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

	fill_vector(vec_idle, DIM, 0.10f);
	fill_vector(vec_a, DIM, 1.00f);
	fill_vector(vec_b, DIM, 2.00f);
	fill_vector(vec_manual, DIM, 3.00f);

	failed |= expect_capture_state_policy();
	failed |= expect_bottom_append_failure();

	failed |= expect_int(kvs_vector_init() == KVS_VECTOR_OK, "vector init");
	ctx = &g_vector_state.rebuild;
	failed |= expect_int(ctx->state == KVS_VECTOR_REBUILD_IDLE, "initial rebuild idle");
	failed |= expect_int(ctx->delta_count == 0, "initial manager delta count");

	failed |= expect_int(
		commit_prepared_vector("idle-upsert", "answer0", "scopeA", vec_idle, DIM) ==
			KVS_VECTOR_OK,
		"idle VSET succeeds"
	);
	failed |= expect_int(ctx->delta_count == 0, "idle VSET does not capture");
	failed |= expect_int(kvs_vector_index_rebuild() == KVS_VECTOR_OK, "VINDEX rebuild succeeds");
	failed |= expect_int(ctx->delta_count == 0, "VINDEX rebuild does not capture");

	failed |= expect_int(
		kvs_vector_manager_rebuild_capture_begin_for_test() == KVS_VECTOR_OK,
		"test hook begins capture"
	);
	failed |= expect_int(
		ctx->state == KVS_VECTOR_REBUILD_BUILDING,
		"capture state building"
	);

	failed |= expect_int(
		commit_prepared_vector("active-upsert", "answer1", "scopeA", vec_a, DIM) ==
			KVS_VECTOR_OK,
		"active VSET succeeds"
	);
	failed |= expect_int(ctx->delta_count == 1, "active VSET captures one delta");
	failed |= expect_int(
		ctx->delta_log[0].op == KVS_VECTOR_REBUILD_DELTA_UPSERT,
		"first delta op upsert"
	);
	record_id = ctx->delta_log[0].record_id;
	failed |= expect_int(record_id != 0, "upsert delta has stable record_id");
	failed |= expect_int(ctx->delta_log[0].dim == DIM, "upsert delta stores dim");
	failed |= expect_int(
		ctx->delta_log[0].vector != NULL && ctx->delta_log[0].vector != vec_a,
		"upsert delta copies vector"
	);
	failed |= expect_float_eq(ctx->delta_log[0].vector[7], vec_a[7], "upsert vector copy value");
	generation0 = ctx->delta_log[0].generation;
	vec_a[7] = 123.0f;
	failed |= expect_int(
		ctx->delta_log[0].vector[7] != vec_a[7],
		"upsert delta is independent of caller vector"
	);

	before_search_delta_count = ctx->delta_count;
	failed |= expect_int(
		search_one("active-upsert", "scopeA", ctx->delta_log[0].vector, DIM, &result) ==
			KVS_VECTOR_OK,
		"VSEARCH with capture active succeeds"
	);
	failed |= expect_int(
		result.question_len == strlen("active-upsert") &&
			memcmp(result.question, "active-upsert", result.question_len) == 0,
		"VSEARCH returns expected record"
	);
	failed |= expect_int(
		ctx->delta_count == before_search_delta_count,
		"VSEARCH does not append delta"
	);

	failed |= expect_int(
		commit_prepared_vector("active-upsert", "answer2", "scopeA", vec_b, DIM) ==
			KVS_VECTOR_OK,
		"repeated VSET succeeds"
	);
	failed |= expect_int(ctx->delta_count == 2, "repeated VSET captures another delta");
	failed |= expect_int(
		ctx->delta_log[1].op == KVS_VECTOR_REBUILD_DELTA_UPSERT,
		"second delta op upsert"
	);
	failed |= expect_int(
		ctx->delta_log[1].record_id == record_id,
		"repeated VSET keeps same record_id"
	);
	generation1 = ctx->delta_log[1].generation;
	failed |= expect_int(generation1 > generation0, "generation increases after repeated VSET");

	failed |= expect_int(
		kvs_vector_del("active-upsert", strlen("active-upsert"), "scopeA", strlen("scopeA")) ==
			KVS_VECTOR_OK,
		"VDEL succeeds"
	);
	failed |= expect_int(ctx->delta_count == 3, "VDEL captures delete delta");
	failed |= expect_int(
		ctx->delta_log[2].op == KVS_VECTOR_REBUILD_DELTA_DELETE,
		"third delta op delete"
	);
	failed |= expect_int(
		ctx->delta_log[2].record_id == record_id,
		"delete delta record_id matches"
	);
	failed |= expect_int(ctx->delta_log[2].vector == NULL, "delete delta has no vector");
	failed |= expect_int(ctx->delta_log[2].dim == 0, "delete delta dim zero");
	generation2 = ctx->delta_log[2].generation;
	failed |= expect_int(generation2 > generation1, "generation increases after delete");

	failed |= expect_int(
		kvs_vector_manager_capture_delete_delta(record_id) == KVS_VECTOR_OK,
		"duplicate delete delta capture succeeds"
	);
	failed |= expect_int(ctx->delta_count == 4, "duplicate delete delta is not merged");
	failed |= expect_int(
		ctx->delta_log[3].op == KVS_VECTOR_REBUILD_DELTA_DELETE,
		"duplicate delete op stored"
	);
	failed |= expect_int(
		ctx->delta_log[3].record_id == record_id,
		"duplicate delete record_id stored"
	);
	generation3 = ctx->delta_log[3].generation;
	failed |= expect_int(generation3 > generation2, "generation increases after duplicate delete");

	failed |= expect_int(
		kvs_vector_manager_rebuild_capture_stop_for_test() == KVS_VECTOR_OK,
		"test hook stops capture"
	);
	failed |= expect_int(
		!kvs_vector_rebuild_is_capture_active(ctx),
		"capture inactive after stop"
	);
	before_inactive_count = ctx->delta_count;
	failed |= expect_int(
		commit_prepared_vector("inactive-upsert", "answer3", "scopeA", vec_b, DIM) ==
			KVS_VECTOR_OK,
		"inactive VSET succeeds"
	);
	failed |= expect_int(
		ctx->delta_count == before_inactive_count,
		"inactive VSET does not capture"
	);

	failed |= expect_int(kvs_vector_reset_for_load() == KVS_VECTOR_OK, "reset for load succeeds");
	failed |= expect_int(
		g_vector_state.rebuild.state == KVS_VECTOR_REBUILD_IDLE,
		"reset for load returns rebuild idle"
	);
	failed |= expect_int(
		g_vector_state.rebuild.delta_count == 0,
		"reset for load clears delta log"
	);

	failed |= expect_int(kvs_vector_index_rebuild() == KVS_VECTOR_OK, "VINDEX rebuild after reset succeeds");
	failed |= expect_int(
		kvs_vector_manager_rebuild_capture_begin_for_test() == KVS_VECTOR_OK,
		"begin capture before destroy"
	);
	failed |= expect_int(
		kvs_vector_manager_capture_upsert_delta(7777, vec_manual, DIM) == KVS_VECTOR_OK,
		"manual upsert capture before destroy"
	);
	failed |= expect_int(
		g_vector_state.rebuild.delta_count == 1,
		"manual capture creates delta before destroy"
	);

	kvs_vector_destroy();
	failed |= expect_int(g_vector_state.rebuild.delta_log == NULL, "destroy clears delta log pointer");
	failed |= expect_int(g_vector_state.rebuild.delta_count == 0, "destroy clears delta count");
	failed |= expect_int(g_vector_state.initialized == 0, "destroy clears initialized");

	mem_destroy();

	if (failed) {
		fprintf(stderr, "vector_rebuild_delta_capture_test FAILED\n");
		return 1;
	}

	printf("vector_rebuild_delta_capture_test PASS\n");
	return 0;
}
