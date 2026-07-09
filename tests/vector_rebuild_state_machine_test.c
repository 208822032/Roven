#include "kvs_config.h"
#include "kvs_memory.h"
#include "kvs_memory_internal.h"
#include "vector/kvs_vector_rebuild.h"

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

static int g_destroy_count;
static void *g_last_destroyed;

static void test_destroy_index(void *index) {
	g_destroy_count++;
	g_last_destroyed = index;
}

static void reset_destroy_spy(void) {
	g_destroy_count = 0;
	g_last_destroyed = NULL;
}

static int expect_error_contains(
	const kvs_vector_rebuild_context_t *ctx,
	const char *needle,
	const char *label
) {
	return expect_int(
		ctx &&
			ctx->error_code != 0 &&
			needle &&
			strstr(ctx->error_message, needle) != NULL,
		label
	);
}

static int expect_state(
	const kvs_vector_rebuild_context_t *ctx,
	kvs_vector_rebuild_state_t state,
	const char *label
) {
	return expect_int(ctx && ctx->state == state, label);
}

static int expect_illegal_no_state_change(
	kvs_vector_rebuild_context_t *ctx,
	int (*fn)(kvs_vector_rebuild_context_t *),
	kvs_vector_rebuild_state_t expected_state,
	const char *label
) {
	kvs_vector_rebuild_state_t before = ctx->state;
	int ret = fn(ctx);

	return expect_int(ret != KVS_VECTOR_OK, label) |
	       expect_int(ctx->state == before && ctx->state == expected_state,
	                  "illegal transition preserves state");
}

static int run_ownership_cleanup_checks(void) {
	kvs_vector_rebuild_context_t ctx;
	int failed = 0;

	failed |= expect_int(
		kvs_vector_rebuild_context_init(&ctx) == KVS_VECTOR_OK,
		"ownership init"
	);

	reset_destroy_spy();
	ctx.active_index = (void *)0xabc0;
	ctx.active_destroy_fn = NULL;
	failed |= expect_int(
		kvs_vector_rebuild_reset_idle(&ctx) == KVS_VECTOR_OK,
		"borrowed active reset succeeds"
	);
	failed |= expect_int(g_destroy_count == 0, "borrowed active reset does not destroy");
	failed |= expect_int(ctx.active_index == NULL, "borrowed active reset detaches pointer");

	reset_destroy_spy();
	ctx.active_index = (void *)0xabc1;
	ctx.active_destroy_fn = NULL;
	failed |= expect_int(
		kvs_vector_rebuild_begin(&ctx, (void *)0x1, (void *)0x2) == KVS_VECTOR_OK,
		"borrowed active begin succeeds"
	);
	failed |= expect_int(g_destroy_count == 0, "borrowed active begin does not destroy");
	failed |= expect_int(ctx.active_index == (void *)0x1, "begin stores new active placeholder");
	failed |= expect_int(
		kvs_vector_rebuild_cancel(&ctx) == KVS_VECTOR_OK,
		"borrowed active cancel succeeds"
	);
	failed |= expect_int(
		kvs_vector_rebuild_reset_idle(&ctx) == KVS_VECTOR_OK,
		"borrowed active cancel reset succeeds"
	);
	failed |= expect_int(g_destroy_count == 0, "borrowed active cancel reset does not destroy");

	reset_destroy_spy();
	ctx.active_index = (void *)0xabc2;
	ctx.active_destroy_fn = test_destroy_index;
	failed |= expect_int(
		kvs_vector_rebuild_reset_idle(&ctx) == KVS_VECTOR_OK,
		"owned active reset succeeds"
	);
	failed |= expect_int(g_destroy_count == 1, "owned active reset destroys once");
	failed |= expect_int(g_last_destroyed == (void *)0xabc2, "owned active reset target");

	kvs_vector_rebuild_context_destroy(&ctx);
	return failed;
}

int main(void) {
	kvs_vector_rebuild_context_t ctx;
	float vector_a[4] = {1.0f, 2.0f, 3.0f, 4.0f};
	float vector_b[2] = {5.0f, 6.0f};
	uint64_t generation0;
	uint64_t generation1;
	int failed = 0;

	memset(&g_cfg, 0, sizeof(g_cfg));
	kvs_config_init_default(&g_cfg);
	g_cfg.mem_backend = KVS_MEM_BACKEND_SYSTEM;
	mem_set_backend(KVS_MEM_BACKEND_SYSTEM);
	mem_init();

	memset(&ctx, 0xaa, sizeof(ctx));
	failed |= expect_int(
		kvs_vector_rebuild_context_init(&ctx) == KVS_VECTOR_OK,
		"context init succeeds"
	);
	failed |= expect_state(&ctx, KVS_VECTOR_REBUILD_IDLE, "initial state idle");
	failed |= expect_int(ctx.delta_count == 0, "initial delta count zero");
	failed |= expect_int(ctx.current_generation == 0, "initial generation zero");

	failed |= expect_int(
		kvs_vector_rebuild_append_upsert_delta(&ctx, 1001, vector_a, 4) == KVS_VECTOR_OK,
		"append upsert succeeds"
	);
	failed |= expect_int(ctx.delta_count == 1, "upsert increments delta count");
	failed |= expect_int(ctx.delta_log[0].op == KVS_VECTOR_REBUILD_DELTA_UPSERT, "upsert op stored");
	failed |= expect_int(ctx.delta_log[0].record_id == 1001, "upsert record_id stored");
	failed |= expect_int(ctx.delta_log[0].dim == 4, "upsert dim stored");
	failed |= expect_int(ctx.delta_log[0].vector != vector_a, "upsert copies vector");
	failed |= expect_int(ctx.delta_log[0].vector[2] == vector_a[2], "upsert vector copy value");
	generation0 = ctx.delta_log[0].generation;

	failed |= expect_int(
		kvs_vector_rebuild_append_delete_delta(&ctx, 1002) == KVS_VECTOR_OK,
		"append delete succeeds"
	);
	failed |= expect_int(ctx.delta_count == 2, "delete increments delta count");
	failed |= expect_int(ctx.delta_log[1].op == KVS_VECTOR_REBUILD_DELTA_DELETE, "delete op stored");
	failed |= expect_int(ctx.delta_log[1].record_id == 1002, "delete record_id stored");
	failed |= expect_int(ctx.delta_log[1].vector == NULL, "delete stores no vector");
	failed |= expect_int(ctx.delta_log[1].dim == 0, "delete dim zero");
	generation1 = ctx.delta_log[1].generation;
	failed |= expect_int(generation1 > generation0, "generation monotonic after delete");

	failed |= expect_int(
		kvs_vector_rebuild_append_upsert_delta(&ctx, 1003, NULL, 4) != KVS_VECTOR_OK,
		"append upsert rejects NULL vector"
	);
	failed |= expect_int(ctx.delta_count == 2, "failed append preserves delta count");
	failed |= expect_int(ctx.current_generation == generation1, "failed append preserves generation");

	kvs_vector_rebuild_delta_clear(&ctx);
	failed |= expect_int(kvs_vector_rebuild_delta_count(&ctx) == 0, "clear delta log count zero");
	failed |= expect_int(ctx.delta_log == NULL, "clear delta log pointer");

	failed |= expect_int(
		kvs_vector_rebuild_begin(&ctx, (void *)0x1, (void *)0x2) == KVS_VECTOR_OK,
		"idle to building succeeds"
	);
	failed |= expect_state(&ctx, KVS_VECTOR_REBUILD_BUILDING, "state building");
	failed |= expect_int(ctx.active_index == (void *)0x1, "active placeholder stored");
	failed |= expect_int(ctx.shadow_index == (void *)0x2, "shadow placeholder stored");
	failed |= expect_int(ctx.rebuild_generation == ctx.current_generation, "rebuild generation captured");

	failed |= expect_illegal_no_state_change(
		&ctx,
		kvs_vector_rebuild_enter_swapping,
		KVS_VECTOR_REBUILD_BUILDING,
		"building to swapping rejected"
	);
	failed |= expect_error_contains(
		&ctx,
		"enter_swapping from building",
		"illegal transition records reason"
	);

	failed |= expect_int(
		kvs_vector_rebuild_enter_replaying_delta(&ctx) == KVS_VECTOR_OK,
		"building to replaying_delta succeeds"
	);
	failed |= expect_state(&ctx, KVS_VECTOR_REBUILD_REPLAYING_DELTA, "state replaying_delta");
	failed |= expect_int(ctx.error_code == 0, "successful transition clears transient error");
	failed |= expect_int(
		kvs_vector_rebuild_enter_ready_to_swap(&ctx) == KVS_VECTOR_OK,
		"replaying_delta to ready_to_swap succeeds"
	);
	failed |= expect_state(&ctx, KVS_VECTOR_REBUILD_READY_TO_SWAP, "state ready_to_swap");
	failed |= expect_int(
		kvs_vector_rebuild_enter_swapping(&ctx) == KVS_VECTOR_OK,
		"ready_to_swap to swapping succeeds"
	);
	failed |= expect_state(&ctx, KVS_VECTOR_REBUILD_SWAPPING, "state swapping");
	failed |= expect_int(
		kvs_vector_rebuild_finish_swap(&ctx, (void *)0x3, (void *)0x1) == KVS_VECTOR_OK,
		"swapping to idle succeeds"
	);
	failed |= expect_state(&ctx, KVS_VECTOR_REBUILD_IDLE, "state idle after finish");
	failed |= expect_int(ctx.active_index == (void *)0x3, "new active placeholder stored");
	failed |= expect_int(ctx.retired_index == (void *)0x1, "retired placeholder stored");
	failed |= expect_int(ctx.shadow_index == NULL, "shadow placeholder cleared");

	failed |= expect_illegal_no_state_change(
		&ctx,
		kvs_vector_rebuild_enter_swapping,
		KVS_VECTOR_REBUILD_IDLE,
		"idle to swapping rejected"
	);

	failed |= expect_int(
		kvs_vector_rebuild_begin(&ctx, (void *)0x4, (void *)0x5) == KVS_VECTOR_OK,
		"second begin succeeds"
	);
	failed |= expect_int(
		kvs_vector_rebuild_begin(&ctx, (void *)0x6, (void *)0x7) != KVS_VECTOR_OK,
		"repeated begin while building rejected"
	);
	failed |= expect_state(&ctx, KVS_VECTOR_REBUILD_BUILDING, "repeated begin preserves building");
	failed |= expect_error_contains(
		&ctx,
		"begin from building",
		"repeated begin records reason"
	);
	failed |= expect_int(
		kvs_vector_rebuild_fail(&ctx, -77, "synthetic failure") == KVS_VECTOR_OK,
		"fail transition succeeds"
	);
	failed |= expect_state(&ctx, KVS_VECTOR_REBUILD_FAILED, "state failed");
	failed |= expect_int(ctx.error_code == -77, "fail records error code");
	failed |= expect_int(strcmp(ctx.error_message, "synthetic failure") == 0, "fail records message");
	failed |= expect_int(
		kvs_vector_rebuild_fail(&ctx, -88, "second failure") != KVS_VECTOR_OK,
		"repeated fail rejected"
	);
	failed |= expect_int(ctx.error_code == -77, "repeated fail preserves first error code");
	failed |= expect_int(
		strcmp(ctx.error_message, "synthetic failure") == 0,
		"repeated fail preserves first message"
	);
	failed |= expect_int(
		kvs_vector_rebuild_begin(&ctx, (void *)0x8, (void *)0x9) != KVS_VECTOR_OK,
		"begin from failed rejected until reset"
	);
	failed |= expect_state(&ctx, KVS_VECTOR_REBUILD_FAILED, "begin from failed preserves state");
	failed |= expect_illegal_no_state_change(
		&ctx,
		kvs_vector_rebuild_enter_swapping,
		KVS_VECTOR_REBUILD_FAILED,
		"failed to swapping rejected"
	);
	failed |= expect_int(
		kvs_vector_rebuild_reset_idle(&ctx) == KVS_VECTOR_OK,
		"failed cleanup succeeds"
	);
	failed |= expect_state(&ctx, KVS_VECTOR_REBUILD_IDLE, "failed cleanup returns idle");
	failed |= expect_int(ctx.error_code == 0, "cleanup clears error code");

	failed |= expect_int(
		kvs_vector_rebuild_begin(&ctx, (void *)0x6, (void *)0x7) == KVS_VECTOR_OK,
		"third begin succeeds"
	);
	failed |= expect_int(
		kvs_vector_rebuild_append_upsert_delta(&ctx, 2001, vector_b, 2) == KVS_VECTOR_OK,
		"append delta before cancel succeeds"
	);
	failed |= expect_int(
		kvs_vector_rebuild_cancel(&ctx) == KVS_VECTOR_OK,
		"cancel transition succeeds"
	);
	failed |= expect_state(&ctx, KVS_VECTOR_REBUILD_CANCELLED, "state cancelled");
	failed |= expect_int(ctx.cancel_requested == 1, "cancel flag set");
	failed |= expect_error_contains(&ctx, "rebuild cancelled", "cancel records reason");
	failed |= expect_int(
		kvs_vector_rebuild_cancel(&ctx) != KVS_VECTOR_OK,
		"repeated cancel rejected"
	);
	failed |= expect_state(&ctx, KVS_VECTOR_REBUILD_CANCELLED, "repeated cancel preserves state");
	failed |= expect_error_contains(
		&ctx,
		"rebuild cancelled",
		"repeated cancel preserves reason"
	);
	failed |= expect_int(
		kvs_vector_rebuild_enter_replaying_delta(&ctx) != KVS_VECTOR_OK,
		"cancelled progress rejected"
	);
	failed |= expect_state(&ctx, KVS_VECTOR_REBUILD_CANCELLED, "cancelled progress preserves state");
	failed |= expect_illegal_no_state_change(
		&ctx,
		kvs_vector_rebuild_enter_swapping,
		KVS_VECTOR_REBUILD_CANCELLED,
		"cancelled to swapping rejected"
	);
	failed |= expect_int(
		kvs_vector_rebuild_reset_idle(&ctx) == KVS_VECTOR_OK,
		"cancel cleanup succeeds"
	);
	failed |= expect_state(&ctx, KVS_VECTOR_REBUILD_IDLE, "cancel cleanup returns idle");
	failed |= expect_int(ctx.delta_count == 0, "cancel cleanup clears deltas");
	failed |= expect_int(ctx.cancel_requested == 0, "cancel cleanup clears flag");

	kvs_vector_rebuild_context_destroy(&ctx);
	failed |= expect_state(&ctx, KVS_VECTOR_REBUILD_IDLE, "destroy leaves idle");
	failed |= expect_int(ctx.delta_log == NULL, "destroy clears delta log");
	failed |= run_ownership_cleanup_checks();

	mem_destroy();

	if (failed) {
		fprintf(stderr, "vector_rebuild_state_machine_test FAILED\n");
		return 1;
	}

	printf("vector_rebuild_state_machine_test PASS\n");
	return 0;
}
