#!/usr/bin/env bash
set -euo pipefail

ROOT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
cd "${ROOT_DIR}"

run_suite() {
  local label="$1"
  local weak="$2"

  echo "stage12 correctness mode=${label}: begin"
  (
    if [[ "${weak}" == "1" ]]; then
      export KVLITE_ROUTE_C_WEAK_RWLOCK=1
    else
      unset KVLITE_ROUTE_C_WEAK_RWLOCK
    fi
    tests/rwlock_weakening_gate_stage11.sh
    tests/full_executor_coverage_closeout_stage10_14.sh
    tests/ntyco_executor_stage10_13.sh
    tests/ebpf_replication_executor_stage10_12.sh
    tests/ttl_scanner_owner_stage10_10.sh
    tests/shutdown_owner_stage10_9.sh
    tests/slave_connect_owner_stage10_9a.sh
    tests/replication_fallback_stage10_8.sh
    bash tests/replicated_command_task_stage10_3.sh
    tests/ttl_executor_stage10_1.sh
    bash tests/lazy_expire_stage10_2.sh
    bash tests/master_full_sync_stage10_5.sh
    bash tests/rdb_snapshot_stage10_6.sh
    bash tests/aof_rewrite_snapshot_stage10_7.sh
  )
  echo "stage12 correctness mode=${label}: ok"
}

run_suite "real-lock" 0
run_suite "weak-lock" 1

echo "Stage 12 weak rwlock correctness suite passed"
