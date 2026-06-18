#!/usr/bin/env bash
set -euo pipefail

ROOT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
cd "${ROOT_DIR}"

python3 - <<'PY'
from pathlib import Path

ROOT = Path.cwd()

def read(rel):
    return (ROOT / rel).read_text(encoding="utf-8")

def require(cond, msg):
    if not cond:
        raise AssertionError(msg)

owner_h = read("include/kvs_owner_lock.h")
owner_c = read("src/core/kvs_owner_lock.c")
executor = read("src/core/kvs_command_executor.c")
kvstore = read("src/core/kvstore.c")
makefile = read("Makefile")

for symbol in [
    "kvs_owner_rwlock_rdlock",
    "kvs_owner_rwlock_wrlock",
    "kvs_owner_rwlock_unlock",
    "kvs_owner_lock_set_executor_thread",
    "kvs_owner_lock_is_executor_thread",
]:
    require(symbol in owner_h, f"owner lock header missing {symbol}")
    require(symbol in owner_c, f"owner lock source missing {symbol}")

require("return pthread_rwlock_rdlock(lock)" in owner_c,
        "rdlock wrapper must call real pthread rwlock")
require("return pthread_rwlock_wrlock(lock)" in owner_c,
        "wrlock wrapper must call real pthread rwlock")
require("return pthread_rwlock_unlock(lock)" in owner_c,
        "unlock wrapper must call real pthread rwlock")
require("pthread_equal(g_owner_executor_thread, pthread_self())" in owner_c,
        "owner assertion helper must compare executor thread")
require("kvs_owner_lock_set_executor_thread();" in executor,
        "executor thread must record owner thread id")
require("#include \"kvs_owner_lock.h\"" in executor,
        "executor must include owner lock wrapper")
require("kvs_owner_rwlock_wrlock(&g_kv_data_rwlock, \"executor.ttl_scan.write\")" in executor,
        "ttl_scan示范点必须使用 wrapper")
require("kvs_owner_rwlock_unlock(&g_kv_data_rwlock, \"executor.ttl_scan.write\")" in executor,
        "ttl_scan unlock 示范点必须使用 wrapper")
require("$(SRC_CORE)/kvs_owner_lock.c" in makefile,
        "owner lock source must be compiled")
require("pthread_rwlock_t g_kv_data_rwlock = PTHREAD_RWLOCK_INITIALIZER" in kvstore,
        "g_kv_data_rwlock must still exist")
require("pthread_rwlock_destroy(&g_kv_data_rwlock)" in kvstore,
        "pthread_rwlock_destroy must remain")
require("KVLITE_ROUTE_C_WEAK_RWLOCK" in owner_c,
        "weak rwlock mode must require explicit environment switch")
require("g_owner_weak_noop_depth" in owner_c,
        "weak rwlock no-op must track per-thread lock/unlock pairing")

print("Stage 11.0 rwlock wrapper owner assertion static audit passed")
PY
