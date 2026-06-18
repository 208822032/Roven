#!/usr/bin/env bash
set -euo pipefail

ROOT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
cd "${ROOT_DIR}"

python3 - <<'PY'
import re
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

reactor = read("src/network/reactor.c")
proactor = read("src/network/proactor.c")
ntyco = read("src/network/ntyco.c")
ttl_bg = read("src/core/kvs_ttl_background.c")
executor = read("src/core/kvs_command_executor.c")
task = read("src/core/kvs_command_task.c")
kvstore = read("src/core/kvstore.c")
dist = read("src/distributed/distributed.c")
rdb = read("src/persistence/kvs_rdb.c")
aof = read("src/persistence/kvs_aof_rewrite.c")
globals_h = read("include/kvs_globals.h")

reactor_submit = find_func(reactor, "reactor_submit_async_command")
reactor_fallback = find_func(reactor, "reactor_handle_replication_fallback")
reactor_submit_task = find_func(reactor, "reactor_submit_command_task")
proactor_submit = find_func(proactor, "proactor_submit_async_command")
proactor_fallback = find_func(proactor, "proactor_handle_replication_fallback")
proactor_submit_task = find_func(proactor, "proactor_submit_command_task")
proactor_fail = find_func(proactor, "proactor_fail_async_command")
ntyco_process = find_func(ntyco, "ntyco_process_received_data")
ntyco_submit_task = find_func(ntyco, "ntyco_submit_command_task")
ttl_thread = find_func(ttl_bg, "ttl_scan_thread_main")
executor_stop = find_func(executor, "kvs_command_executor_stop")
execute_repl = find_func(executor, "kvs_command_executor_execute_repl")
execute_ttl = find_func(executor, "kvs_command_executor_execute_ttl")
execute_ttl_scan = find_func(executor, "kvs_command_executor_execute_ttl_scan")
execute_full_sync = find_func(executor, "kvs_command_executor_execute_full_sync_snapshot")
execute_rdb = find_func(executor, "kvs_command_executor_execute_rdb_snapshot")
execute_aof = find_func(executor, "kvs_command_executor_execute_aof_snapshot")
slave_connect = find_func(dist, "slave_connect_master")
slave_sync = find_func(dist, "slave_sync_thread")
rdb_save = find_func(rdb, "kvs_rdb_save")
aof_bg = find_func(aof, "aof_rewrite_background")

for label, body in [
    ("reactor submit", reactor_submit),
    ("reactor fallback", reactor_fallback),
    ("proactor submit", proactor_submit),
    ("proactor fallback", proactor_fallback),
    ("ntyco process", ntyco_process),
    ("ntyco submit", ntyco_submit_task),
]:
    require("kvs_filter_protocol" not in body, f"{label} must not call kvs_filter_protocol")
    require("kvs_command_task_execute_sync" not in body, f"{label} must not call execute_sync")
    require("net_session_process_resp" not in body, f"{label} must not use synchronous session handler loop")

require("kvs_command_executor_submit(task)" in reactor_submit_task,
        "reactor normal path must submit command_task")
require("kvs_command_executor_submit(task)" in proactor_submit_task,
        "proactor normal path must submit command_task")
require("kvs_command_executor_submit(task)" in ntyco_submit_task,
        "ntyco normal path must submit command_task")
require("kvs_command_executor_submit_replicated(" in reactor_fallback,
        "reactor eBPF replicated apply must submit replicated task")
require("kvs_command_executor_submit_replicated(" in proactor_fallback,
        "proactor eBPF replicated apply must submit replicated task")
require("kvs_command_executor_submit_replicated(" in ntyco_process,
        "ntyco replicated apply must submit replicated task")
require("p_process_received_data" not in proactor_fail and
        "p_process_received_data" not in proactor_submit,
        "proactor async failure/submit must not fallback to sync handler")

for forbidden in [
    "g_kv_data_rwlock",
    "pthread_rwlock_rdlock",
    "pthread_rwlock_wrlock",
    "pthread_rwlock_unlock",
    "kvs_get_global_array",
    "kvs_get_global_hash",
    "kvs_get_global_rbtree",
    "kvs_get_global_skiptable",
    "ttl_expire_observed_key",
]:
    require(forbidden not in ttl_bg, f"TTL background scanner must not reference {forbidden}")
require("kvs_command_executor_submit_ttl_scan(32)" in ttl_thread,
        "TTL scanner must submit ttl_scan_task")

require("kvs_command_executor_build_full_sync_snapshot(&snapshot)" in slave_sync,
        "master full sync must use executor snapshot")
require("kvs_command_executor_build_rdb_snapshot(&snapshot)" in rdb_save,
        "RDB save must use executor snapshot")
require("kvs_command_executor_build_aof_rewrite_snapshot(&snapshot)" in aof_bg,
        "AOF rewrite must use executor snapshot")
require("kvs_command_executor_submit_replicated(" in slave_connect,
        "TCP slave apply must use replicated command task")

for name, body in [
    ("replicated task", execute_repl),
    ("ttl expire task", execute_ttl),
    ("ttl scan task", execute_ttl_scan),
    ("full sync snapshot", execute_full_sync),
    ("rdb snapshot", execute_rdb),
    ("aof snapshot", execute_aof),
]:
    require("g_kv_data_rwlock" in body, f"{name} must still retain real lock boundary")
    require("kvs_owner_rwlock_" in body, f"{name} must use owner rwlock wrapper")

for token in [
    "g_executor.repl_head = NULL",
    "g_executor.ttl_head = NULL",
    "g_executor.ttl_scan_head = NULL",
    "g_executor.snapshot_head = NULL",
    "g_executor.rdb_snapshot_head = NULL",
    "g_executor.aof_snapshot_head = NULL",
]:
    require(token in executor_stop, f"executor stop missing drain/drop token {token}")

require("extern pthread_rwlock_t g_kv_data_rwlock" in globals_h,
        "g_kv_data_rwlock declaration missing")
require("pthread_rwlock_t g_kv_data_rwlock = PTHREAD_RWLOCK_INITIALIZER" in kvstore,
        "g_kv_data_rwlock definition missing")
require("pthread_rwlock_destroy(&g_kv_data_rwlock)" in kvstore,
        "g_kv_data_rwlock destroy must remain")

for path in ["NtyCo/libntyco.a", "workspace"]:
    require(not (ROOT / path).is_file() or path not in read(".gitignore"),
            "build artifact audit placeholder")

print("Stage 10.14 full executor coverage closeout static audit passed")
PY
