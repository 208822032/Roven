#!/usr/bin/env bash
# Stage 12/13: Weak rwlock correctness gate
# Stage 13 update: default is now weak-lock.
#   - Default (unset or KVLITE_ROUTE_C_WEAK_RWLOCK=1): weak-lock mode
#   - KVLITE_ROUTE_C_WEAK_RWLOCK=0: real-lock rollback mode
set -euo pipefail

ROOT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
cd "${ROOT_DIR}"

run_suite() {
  local label="$1"
  local mode="$2"  # "weak" = default weak-lock, "real" = real-lock rollback

  echo "stage12/13 correctness mode=${label}: begin"
  (
    if [[ "${mode}" == "real" ]]; then
      # Stage 13: real-lock rollback requires explicit disable
      export KVLITE_ROUTE_C_WEAK_RWLOCK=0
    else
      # Stage 13: default is weak-lock (unset → weak, or explicit KVLITE_ROUTE_C_WEAK_RWLOCK=1)
      export KVLITE_ROUTE_C_WEAK_RWLOCK=1
    fi
    tests/rwlock_weakening_gate_stage11.sh
    tests/full_executor_coverage_closeout_stage10_14.sh
    tests/ntyco_executor_stage10_13.sh
    tests/ebpf_replication_executor_stage10_12.sh
    tests/ttl_scanner_owner_stage10_10.sh
    tests/shutdown_owner_stage10_9.sh
    tests/slave_connect_owner_stage10_9a.sh
    tests/shutdown_flag_atomic_stage12.sh
    tests/replication_fallback_stage10_8.sh
    bash tests/replicated_command_task_stage10_3.sh
    tests/ttl_executor_stage10_1.sh
    bash tests/lazy_expire_stage10_2.sh
    bash tests/master_full_sync_stage10_5.sh
    bash tests/rdb_snapshot_stage10_6.sh
    bash tests/aof_rewrite_snapshot_stage10_7.sh
    bash tests/aofrewrite_cow_snapshot_stage12.sh
    bash tests/ntyco_mixed_shutdown_stage12.sh
  )
  echo "stage12/13 correctness mode=${label}: ok"
}

run_suite "weak-lock (default)" "weak"
run_suite "real-lock (rollback)" "real"

echo "Stage 12/13 weak rwlock correctness suite passed"