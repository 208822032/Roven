#!/usr/bin/env bash
set -euo pipefail

ROOT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
cd "${ROOT_DIR}"

python3 - <<'PY'
import re
import subprocess
from pathlib import Path

ROOT = Path.cwd()

def read(rel):
    return (ROOT / rel).read_text(encoding="utf-8")

def require(cond, msg):
    if not cond:
        raise AssertionError(msg)

def find_func(src, name):
    pattern = re.compile(rf"(^|\n)[^\n;]*\b{re.escape(name)}\s*\(")
    for match in pattern.finditer(src):
        line_end = src.find("\n", match.start() + 1)
        if line_end < 0:
            line_end = len(src)
        line = src[match.start():line_end]
        if ";" not in line and line.strip().startswith(("static ", "int ", "void ", "void*")):
            brace = src.find("{", match.start())
            require(brace >= 0, f"missing body for {name}")
            depth = 0
            for idx in range(brace, len(src)):
                if src[idx] == "{":
                    depth += 1
                elif src[idx] == "}":
                    depth -= 1
                    if depth == 0:
                        return src[match.start():idx + 1]
    raise AssertionError(f"missing function {name}")

owner_h = read("include/kvs_owner_lock.h")
owner_c = read("src/core/kvs_owner_lock.c")
task = read("src/core/kvs_command_task.c")
executor = read("src/core/kvs_command_executor.c")
kvstore = read("src/core/kvstore.c")
aof = read("src/persistence/kvs_aof.c")
aof_rewrite = read("src/persistence/kvs_aof_rewrite.c")
doc = read("docs/route_c_perf_arch/62_stage11_rwlock_wrapper_takeover_and_weakening_gate.md")

for symbol in [
    "kvs_owner_rwlock_rdlock",
    "kvs_owner_rwlock_wrlock",
    "kvs_owner_rwlock_unlock",
    "kvs_owner_lock_weak_mode_enabled",
    "kvs_owner_lock_set_weak_mode",
    "kvs_owner_lock_get_stats",
]:
    require(symbol in owner_h, f"owner lock header missing {symbol}")
    require(symbol in owner_c, f"owner lock source missing {symbol}")

for token in [
    "return pthread_rwlock_rdlock(lock)",
    "return pthread_rwlock_wrlock(lock)",
    "return pthread_rwlock_unlock(lock)",
]:
    require(token in owner_c, f"default/fallback real pthread path missing: {token}")

require("KVLITE_ROUTE_C_WEAK_RWLOCK" in owner_c,
        "weak lock mode must require explicit environment switch")
require("g_owner_weak_mode = -1" in owner_c,
        "weak mode must default to auto/env disabled, not enabled")
require("g_owner_weak_noop_depth" in owner_c,
        "weak no-op path must pair lock/unlock per thread")
require("KVS_LOG_WARN" in owner_c and "non-executor" in owner_c,
        "non-executor weak calls must warn and avoid silent no-op")
require("weak_fallback_calls" in owner_h and "strict_assertion_failures" in owner_h,
        "owner assertion/warning stats missing")
require("owner rwlock stats:" in executor and
        "weak_noop_calls" in executor and
        "weak_fallback_calls" in executor,
        "executor shutdown must expose owner rwlock stats for Stage 12")
require("pthread_equal(g_owner_executor_thread, pthread_self())" in owner_c,
        "executor owner assertion must compare current thread")
require("kvs_owner_lock_set_executor_thread();" in executor,
        "executor thread must set owner thread id")

for forbidden in [
    "pthread_rwlock_rdlock(&g_kv_data_rwlock)",
    "pthread_rwlock_wrlock(&g_kv_data_rwlock)",
    "pthread_rwlock_unlock(&g_kv_data_rwlock)",
]:
    require(forbidden not in task,
            f"kvs_command_task.c still contains naked target lock call: {forbidden}")

executor_exceptions = find_func(kvstore, "dest_kvengine")
require("pthread_rwlock_wrlock(&g_kv_data_rwlock)" in executor_exceptions and
        "pthread_rwlock_unlock(&g_kv_data_rwlock)" in executor_exceptions,
        "destroy boundary must retain real naked pthread lock")

for forbidden in [
    "pthread_rwlock_rdlock(&g_kv_data_rwlock)",
    "pthread_rwlock_wrlock(&g_kv_data_rwlock)",
    "pthread_rwlock_unlock(&g_kv_data_rwlock)",
]:
    require(forbidden not in executor,
            f"kvs_command_executor.c still contains naked target lock call: {forbidden}")

expected_sites = [
    "command_task.execute.read",
    "command_task.execute.write",
    "command_task.lazy_expire.retry",
    "executor.replicated.write",
    "executor.ttl_expire.write",
    "executor.ttl_scan.write",
    "executor.full_sync_snapshot.read",
    "executor.rdb_snapshot.read",
    "executor.aof_rewrite_snapshot.read",
]
for site in expected_sites:
    require(site in task or site in executor, f"missing wrapper site {site}")

require("pthread_rwlock_t g_kv_data_rwlock = PTHREAD_RWLOCK_INITIALIZER" in kvstore,
        "g_kv_data_rwlock definition must remain")
require("pthread_rwlock_init(&g_kv_data_rwlock, NULL)" in kvstore,
        "g_kv_data_rwlock init must remain")
require("pthread_rwlock_destroy(&g_kv_data_rwlock)" in kvstore,
        "pthread_rwlock_destroy(&g_kv_data_rwlock) must remain")

require("aof_state.data_rwlock" in aof and "pthread_rwlock_rdlock(&aof_state.data_rwlock)" in aof,
        "aof_state.data_rwlock must not be replaced in kvs_aof.c")
require("aof_state.data_rwlock" in aof_rewrite and "pthread_rwlock_wrlock(&aof_state.data_rwlock)" in aof_rewrite,
        "aof_state.data_rwlock must not be replaced in kvs_aof_rewrite.c")

for phrase in [
    "不是无条件删除锁",
    "KVLITE_ROUTE_C_WEAK_RWLOCK=1",
    "默认仍真实调用 pthread_rwlock",
    "pthread_rwlock_destroy(&g_kv_data_rwlock)",
    "aof_state.data_rwlock",
    "STAGE12_READY",
]:
    require(phrase in doc, f"Stage 11 doc missing phrase: {phrase}")

staged = subprocess.check_output(["git", "diff", "--cached", "--name-only"], text=True)
for artifact in ["NtyCo/libntyco.a", "workspace/"]:
    require(artifact not in staged, f"artifact should not be staged for commit: {artifact}")

print("Stage 11 rwlock weakening gate static audit passed")
PY
