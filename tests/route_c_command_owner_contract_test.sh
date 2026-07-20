#!/usr/bin/env bash
set -euo pipefail

ROOT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
TMP_DIR="${TMPDIR:-/tmp}/kvlite-command-owner-contract-$$"
DEBUG_BIN="$TMP_DIR/owner-contract-debug"
RELEASE_BIN="$TMP_DIR/owner-contract-release"

cleanup() {
    rm -rf "$TMP_DIR"
}
trap cleanup EXIT

mkdir -p "$TMP_DIR"
cd "$ROOT_DIR"
ulimit -c 0

COMMON_FLAGS=(
    -std=c11
    -D_XOPEN_SOURCE=700
    -Wall
    -Wextra
    -Werror
    -pthread
    -Iinclude
)
SOURCES=(
    tests/route_c_command_owner_contract_test.c
    src/core/kvs_owner_lock.c
)

cc "${COMMON_FLAGS[@]}" -DKVS_COMMAND_OWNER_ASSERTIONS=1 \
    "${SOURCES[@]}" -o "$DEBUG_BIN"
cc "${COMMON_FLAGS[@]}" "${SOURCES[@]}" -o "$RELEASE_BIN"

for mode in startup_ok runtime_ok shutdown_ok legacy_ok; do
    "$DEBUG_BIN" "$mode"
done

expect_abort() {
    local mode="$1"
    local log="$TMP_DIR/$mode.log"
    local status

    set +e
    { "$DEBUG_BIN" "$mode" >"$log" 2>&1; } 2>/dev/null
    status=$?
    set -e
    if [[ "$status" -eq 0 ]]; then
        printf '[FAIL] expected owner assertion failure: %s\n' "$mode" >&2
        return 1
    fi
    if ! rg -q 'command owner assertion failed' "$log"; then
        printf '[FAIL] missing owner assertion diagnostic: %s status=%s\n' \
            "$mode" "$status" >&2
        return 1
    fi
}

expect_abort non_owner_runtime
expect_abort legacy_mutation_denied

# Assertions compile to no-ops unless a debug/test build enables them.
"$RELEASE_BIN" unregistered

# Current runtime producers must submit immutable tasks instead of mutating DB state.
rg -q 'kvs_command_executor_submit_ttl_scan' src/core/kvs_ttl_background.c
! rg -q 'ttl_expire_observed_key|handle_(set|del)|kvs_vector_(set|del)' \
    src/core/kvs_ttl_background.c

rg -q 'kvs_command_task_prepare' src/core/kvs_embedding_worker_pool.c
rg -q 'kvs_command_executor_submit_prepared' src/core/kvs_embedding_worker_pool.c
! rg -q 'kvs_vector_set_commit_prepared|kvs_vector_del|kvs_vector_index_add_current' \
    src/core/kvs_embedding_worker_pool.c

rg -q 'kvs_command_executor_submit_replicated' src/distributed/distributed.c
! rg -q 'kvs_command_executor_apply_repl_store|kvs_vector_set_commit_prepared' \
    src/distributed/distributed.c

for source in src/network/reactor.c src/network/proactor.c src/network/ntyco.c; do
    rg -q 'kvs_command_executor_submit' "$source"
    ! rg -q 'kvs_vector_set_commit_prepared|ttl_expire_observed_key' "$source"
done

# Required assertion boundaries and the single scoped legacy exception stay visible.
rg -q 'KVS_COMMAND_OWNER_ASSERT_MUTATION\("command_task.execute.write"\)' \
    src/core/kvs_command_task.c
rg -q 'KVS_COMMAND_OWNER_ASSERT_MUTATION\("executor.replicated.apply"\)' \
    src/core/kvs_command_executor.c
rg -q 'KVS_COMMAND_OWNER_ASSERT_MUTATION\("executor.ttl_scan.apply"\)' \
    src/core/kvs_command_executor.c
rg -q 'KVS_COMMAND_OWNER_ASSERT_MUTATION\("vector.vset.commit"\)' \
    src/vector/kvs_vector.c
rg -q 'KVS_COMMAND_OWNER_ASSERT_MUTATION\("vector.vdel.commit"\)' \
    src/vector/kvs_vector.c
rg -q 'KVS_COMMAND_OWNER_ASSERT_MUTATION\("vector.record_id.allocate"\)' \
    src/vector/kvs_vector.c
rg -q 'KVS_COMMAND_OWNER_ASSERT_MUTATION\("vector.active_index.add"\)' \
    src/vector/kvs_vector.c
rg -q 'KVS_COMMAND_OWNER_ASSERT_MUTATION\("vector.rebuild_delta.upsert"\)' \
    src/vector/kvs_vector.c
rg -q 'kvs_command_owner_register_legacy_async_rebuild_thread' \
    src/vector/kvs_vector.c

printf '[PASS] Route C command owner contract\n'
