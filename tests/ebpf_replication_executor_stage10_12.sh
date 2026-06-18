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
executor_h = read("include/kvs_command_executor.h")
executor = read("src/core/kvs_command_executor.c")
task = read("src/core/kvs_command_task.c")
fallback_h = read("include/kvs_replication_fallback.h")
fallback = read("src/core/kvs_replication_fallback.c")
makefile = read("Makefile")
dist = read("src/distributed/distributed.c")
rdb = read("src/persistence/kvs_rdb.c")
aof = read("src/persistence/kvs_aof_rewrite.c")

reactor_submit = find_func(reactor, "reactor_submit_async_command")
reactor_fallback = find_func(reactor, "reactor_handle_replication_fallback")
reactor_submit_task = find_func(reactor, "reactor_submit_command_task")
proactor_submit = find_func(proactor, "proactor_submit_async_command")
proactor_fallback = find_func(proactor, "proactor_handle_replication_fallback")
proactor_submit_task = find_func(proactor, "proactor_submit_command_task")
proactor_fail = find_func(proactor, "proactor_fail_async_command")
execute_sync = find_func(task, "kvs_command_task_execute_sync")
post_commit = find_func(task, "kvs_command_task_post_commit")
submit_repl = find_func(executor, "kvs_command_executor_submit_replicated")
build_full_sync = find_func(executor, "kvs_command_executor_build_full_sync_snapshot")

for name, body in [
    ("reactor_submit_async_command", reactor_submit),
    ("reactor_handle_replication_fallback", reactor_fallback),
]:
    require("kvs_filter_protocol" not in body, f"{name} must not call kvs_filter_protocol")
    require("kvs_command_task_execute_sync" not in body, f"{name} must not call execute_sync directly")
    require("net_session_process_resp" not in body, f"{name} must not call net_session_process_resp")
    require("reactor_session_handler" not in body, f"{name} must not call synchronous reactor handler")

for name, body in [
    ("proactor_submit_async_command", proactor_submit),
    ("proactor_handle_replication_fallback", proactor_fallback),
]:
    require("kvs_filter_protocol" not in body, f"{name} must not call kvs_filter_protocol")
    require("kvs_command_task_execute_sync" not in body, f"{name} must not call execute_sync directly")
    require("p_process_received_data" not in body, f"{name} must not call p_process_received_data")
    require("net_session_process_resp" not in body, f"{name} must not call net_session_process_resp")
    require("proactor_session_handler" not in body, f"{name} must not call synchronous proactor handler")

require("reactor_handle_replication_fallback(fd, ext, cmd_len)" in reactor_submit,
        "reactor eBPF fallback must dispatch to explicit classifier")
require("proactor_handle_replication_fallback(ring, ext, cmd_len)" in proactor_submit,
        "proactor eBPF fallback must dispatch to explicit classifier")

for body, label in [
    (reactor_fallback, "reactor"),
    (proactor_fallback, "proactor"),
]:
    require("KVS_REPL_FALLBACK_REPLSYNC" in body, f"{label} fallback must classify REPLSYNC")
    require("KVS_REPL_FALLBACK_REPLICATED_APPLY" in body, f"{label} fallback must classify replicated apply")
    require("KVS_REPL_FALLBACK_NORMAL_COMMAND" in body, f"{label} fallback must classify normal command")
    require("kvs_command_executor_submit_replicated(" in body, f"{label} fallback must submit replicated task")
    require("kvs_command_executor_submit(task)" in (reactor_submit_task if label == "reactor" else proactor_submit_task),
            f"{label} normal fallback must submit command_task")
    require("+OK\\r\\n" in body, f"{label} replicated fallback should inline small OK response")

require("kvs_replicated_command_task_t" in executor_h,
        "replicated command task type missing")
require("kvs_command_executor_submit_replicated(" in submit_repl,
        "replicated command submit implementation missing")
require("kvs_command_executor_submit_replicated(" in dist,
        "TCP slave apply must still use replicated task path")

require("kvs_command_executor_build_full_sync_snapshot" in executor_h,
        "full sync snapshot API missing")
require("pthread_rwlock_rdlock(&g_kv_data_rwlock)" in build_full_sync,
        "full sync snapshot must remain executor-built under retained read lock")
require("kvs_command_executor_build_full_sync_snapshot(&snapshot)" in dist,
        "master full sync must still use executor snapshot")
require("kvs_command_executor_build_rdb_snapshot(&snapshot)" in rdb,
        "RDB snapshot executor path missing")
require("kvs_command_executor_build_aof_rewrite_snapshot(&snapshot)" in aof,
        "AOF snapshot executor path missing")

require("return proactor_fail_async_command(ext, parser, resp, task, \"alloc\")" in proactor_submit_task,
        "proactor alloc failure must fail fast")
require("return proactor_fail_async_command(ext, parser, resp, task, \"response_init\")" in proactor_submit_task,
        "proactor response init failure must fail fast")
require("return proactor_fail_async_command(ext, parser, resp, task, \"task_build\")" in proactor_submit_task,
        "proactor build failure must fail fast")
require("return proactor_fail_async_command(ext, parser, resp, task, \"copy_argv\")" in proactor_submit_task,
        "proactor argv copy failure must fail fast")
require("proactor executor submit failed fast without synchronous fallback" in proactor_submit_task,
        "proactor submit failure fail-fast log missing")
require("p_process_received_data" not in proactor_fail,
        "proactor fail-fast helper must not call sync handler")

require("kvs_replication_fallback_classify" in fallback_h and
        "KVS_REPL_FALLBACK_REPLICATED_APPLY" in fallback_h,
        "replication fallback classifier API missing")
for cmd in ["SET", "DEL", "MOD", "PEXPIREAT", "PERSIST"]:
    require(cmd in fallback, f"classifier missing {cmd}")
require("$(SRC_CORE)/kvs_replication_fallback.c" in makefile,
        "classifier implementation not compiled")

require("set_sync_context(task->sync_context)" in execute_sync,
        "executor command task must apply task sync_context")
require("task->sync_context == 2" in post_commit,
        "post_commit must suppress replication using task sync_context")

require("KVS_ASYNC_NET_OWNER_NTYCO" in ntyco and
        "kvs_command_executor_submit(task)" in ntyco,
        "ntyco source should route normal commands through executor after Stage 10.13")

require("g_kv_data_rwlock" in task and "pthread_rwlock_rdlock(&g_kv_data_rwlock)" in task,
        "g_kv_data_rwlock read lock removed or weakened")
require("pthread_rwlock_wrlock(&g_kv_data_rwlock)" in task and
        "pthread_rwlock_wrlock(&g_kv_data_rwlock)" in executor,
        "g_kv_data_rwlock write lock removed or weakened")

print("Stage 10.12 eBPF replication fallback executorization static audit passed")
PY
