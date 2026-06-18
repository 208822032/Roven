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

kvstore = read("src/core/kvstore.c")
reactor = read("src/network/reactor.c")
proactor = read("src/network/proactor.c")
ntyco = read("src/network/ntyco.c")
ttl_bg = read("src/core/kvs_ttl_background.c")
executor_h = read("include/kvs_command_executor.h")
executor = read("src/core/kvs_command_executor.c")
task = read("src/core/kvs_command_task.c")
dist = read("src/distributed/distributed.c")
rdb = read("src/persistence/kvs_rdb.c")
aof_rewrite = read("src/persistence/kvs_aof_rewrite.c")
doc = read("docs/route_c_perf_arch/56_stage10_11_rwlock_removal_readiness_reaudit.md")

main = find_func(kvstore, "main")
reactor_handler = find_func(reactor, "reactor_session_handler")
reactor_submit = find_func(reactor, "reactor_submit_async_command")
proactor_handler = find_func(proactor, "proactor_session_handler")
proactor_process = find_func(proactor, "p_process_received_data")
proactor_submit = find_func(proactor, "proactor_submit_async_command")
proactor_fail = find_func(proactor, "proactor_fail_async_command")
ttl_thread = find_func(ttl_bg, "ttl_scan_thread_main")
executor_stop = find_func(executor, "kvs_command_executor_stop")
execute_sync = find_func(task, "kvs_command_task_execute_sync")
execute_repl = find_func(executor, "kvs_command_executor_execute_repl")
execute_ttl_scan = find_func(executor, "kvs_command_executor_execute_ttl_scan")
build_full_sync = find_func(executor, "kvs_command_executor_build_full_sync_snapshot")
build_rdb = find_func(executor, "kvs_command_executor_build_rdb_snapshot")
build_aof = find_func(executor, "kvs_command_executor_build_aof_rewrite_snapshot")
slave_sync = find_func(dist, "slave_sync_thread")
slave_connect = find_func(dist, "slave_connect_master")
rdb_save = find_func(rdb, "kvs_rdb_save")
aof_bg = find_func(aof_rewrite, "aof_rewrite_background")

require("network_mode=ntyco runs in legacy sync mode" in main, "ntyco legacy warning missing")
require("Commands bypass command executor" in main, "ntyco bypass warning missing")
require("rwlock removal is NOT supported in this mode" in main, "ntyco no-rwlock-removal warning missing")
require("ntyco_start(g_cfg.port, kvs_filter_protocol)" in main, "ntyco no longer uses sync kvs_filter_protocol handler")

for handler_name, handler in [("reactor", reactor_handler), ("proactor", proactor_handler)]:
    require("REPLSYNC" in handler, f"{handler_name} REPLSYNC handler missing")
    require("ext->is_replication = 1" in handler, f"{handler_name} replication marker missing")
    require("legacy synchronous replication path" in handler, f"{handler_name} eBPF legacy warning missing")
    require("rwlock removal is not supported" in handler, f"{handler_name} eBPF no-rwlock-removal warning missing")
    require("set_sync_context(2)" in handler, f"{handler_name} replication sync context missing")

require("ext->is_replication || reactor_command_is_replsync(session)" in reactor_submit,
        "reactor replication fallback branch missing")
require("net_session_process_resp(fd, session, reactor_session_handler, ext)" in reactor_submit,
        "reactor replication fallback no longer uses synchronous handler")

require("ext->is_replication || proactor_command_is_replsync(session)" in proactor_submit,
        "proactor replication fallback branch missing")
require("int processed = p_process_received_data(ext)" in proactor_submit,
        "proactor replication fallback no longer uses p_process_received_data")
require("net_session_process_resp" in proactor_process and "proactor_session_handler" in proactor_process,
        "proactor p_process_received_data no longer executes sync handler")
require("return p_process_received_data(ext) < 0 ? -1 : 1;" not in proactor_submit,
        "proactor async failure still falls back to p_process_received_data")
require("p_process_received_data" not in proactor_fail,
        "proactor fail-fast helper must not call p_process_received_data")
require("proactor executor submit failed fast without synchronous fallback" in proactor_submit,
        "proactor submit failure fail-fast log missing")

for forbidden in [
    "g_kv_data_rwlock",
    "pthread_rwlock_rdlock",
    "pthread_rwlock_wrlock",
    "kvs_get_global_array",
    "kvs_get_global_hash",
    "kvs_get_global_rbtree",
    "kvs_get_global_skiptable",
    "kv_is_expired",
]:
    require(forbidden not in ttl_bg, f"TTL background scanner still directly references {forbidden}")
require("kvs_command_executor_submit_ttl_scan(32)" in ttl_thread,
        "TTL scanner no longer submits ttl_scan_task")
require("typedef struct kvs_ttl_scan_task_s" in executor_h,
        "ttl_scan_task type missing")
require("int kvs_command_executor_submit_ttl_scan(int budget)" in executor_h,
        "ttl_scan submit API missing")
require("ttl_scan_task = g_executor.ttl_scan_head" in executor_stop and
        "ttl_scan_queue_depth" in executor_stop and
        "ttl_scan_task_dropped" in executor_stop,
        "executor stop no longer drains/drops ttl_scan_task")
require("kvs_command_executor_execute_ttl_scan(ttl_scan_task)" in executor,
        "executor no longer consumes ttl_scan_task")
require("pthread_rwlock_wrlock(&g_kv_data_rwlock)" in execute_ttl_scan,
        "ttl_scan_task no longer uses retained write lock inside executor")

require("KVS_CMD_RETRY_WITH_WRITE_LOCK" in execute_sync,
        "command task lazy expire retry path missing")
require("pthread_rwlock_rdlock(&g_kv_data_rwlock)" in task and
        "pthread_rwlock_wrlock(&g_kv_data_rwlock)" in task,
        "command task retained lock sites missing")

require("kvs_command_executor_submit_replicated(" in slave_connect,
        "TCP slave apply no longer submits replicated command task")
require("pthread_rwlock_wrlock(&g_kv_data_rwlock)" in execute_repl,
        "replicated command task no longer uses retained write lock in executor")

require("kvs_command_executor_build_full_sync_snapshot(&snapshot)" in slave_sync,
        "master full sync no longer uses executor snapshot")
require("pthread_rwlock_rdlock(&g_kv_data_rwlock)" in build_full_sync,
        "full sync snapshot retained read lock missing")

require("kvs_command_executor_build_rdb_snapshot(&snapshot)" in rdb_save,
        "RDB save no longer uses executor snapshot")
require("pthread_rwlock_rdlock(&g_kv_data_rwlock)" in build_rdb,
        "RDB snapshot retained read lock missing")

require("kvs_command_executor_build_aof_rewrite_snapshot(&snapshot)" in aof_bg,
        "AOF rewrite no longer uses executor snapshot")
require("pthread_rwlock_rdlock(&g_kv_data_rwlock)" in build_aof,
        "AOF snapshot retained read lock missing")

shutdown_pos = main.index("shutdown requested, saving data")
ttl_stop_pos = main.index("kvs_ttl_background_stop()", shutdown_pos)
aof_pos = main.index("aof_fsync(1)", shutdown_pos)
rdb_pos = main.index("kvs_rdb_save(g_cfg.rdb_filename)", shutdown_pos)
executor_stop_pos = main.index("kvs_command_executor_stop()", shutdown_pos)
destroy_pos = main.index("dest_kvengine()", shutdown_pos)
require(shutdown_pos < ttl_stop_pos < aof_pos < rdb_pos < executor_stop_pos < destroy_pos,
        "shutdown order no longer matches stop TTL -> final save -> stop executor -> destroy")

for phrase in [
    "CONDITIONALLY_READY",
    "g_kv_data_rwlock",
    "仍然必须保留",
    "ntyco",
]:
    require(phrase in doc, f"Stage 10.11 doc missing phrase: {phrase}")
for phrase in [
    "eBPF REPLSYNC/is_replication fallback",
    "lock-retained-but-owner-safe",
    "TTL scanner 不再直接访问 live store",
    "Proactor normal async build/submit failure 已 fail-fast",
]:
    require(phrase in doc, f"Stage 10.11 doc missing phrase: {phrase}")

print("Stage 10.11 rwlock removal readiness static audit passed")
PY
