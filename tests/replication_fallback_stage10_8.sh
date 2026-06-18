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
    start = -1
    pattern = re.compile(rf"(^|\n)[^\n;]*\b{re.escape(name)}\s*\(")
    for match in pattern.finditer(src):
        line_end = src.find("\n", match.start() + 1)
        if line_end < 0:
            line_end = len(src)
        line = src[match.start():line_end]
        stripped = line.strip()
        if ";" not in line and stripped.startswith(("static ", "int ", "void ")):
            start = match.start()
            break
    require(start >= 0, f"missing function {name}")
    brace = src.find("{", start)
    require(brace >= 0, f"missing function body for {name}")
    depth = 0
    for idx in range(brace, len(src)):
        ch = src[idx]
        if ch == "{":
            depth += 1
        elif ch == "}":
            depth -= 1
            if depth == 0:
                return src[start:idx + 1]
    raise AssertionError(f"unterminated function {name}")


reactor = read("src/network/reactor.c")
proactor = read("src/network/proactor.c")
distributed = read("src/distributed/distributed.c")
kvstore = read("src/core/kvstore.c")
task = read("src/core/kvs_command_task.c")

reactor_handler = find_func(reactor, "reactor_session_handler")
reactor_submit = find_func(reactor, "reactor_submit_async_command")
reactor_fallback = find_func(reactor, "reactor_handle_replication_fallback")
reactor_submit_task = find_func(reactor, "reactor_submit_command_task")
proactor_handler = find_func(proactor, "proactor_session_handler")
proactor_process = find_func(proactor, "p_process_received_data")
proactor_fail = find_func(proactor, "proactor_fail_async_command")
proactor_submit = find_func(proactor, "proactor_submit_async_command")
proactor_fallback = find_func(proactor, "proactor_handle_replication_fallback")
proactor_submit_task = find_func(proactor, "proactor_submit_command_task")
kvs_filter = find_func(kvstore, "kvs_filter_protocol")
execute_sync = find_func(task, "kvs_command_task_execute_sync")

require("strcasecmp(cmd_name, \"REPLSYNC\")" in reactor_handler,
        "reactor handler no longer handles REPLSYNC")
require("ext->is_replication = 1" in reactor_handler,
        "reactor REPLSYNC no longer marks replication connection")
require("set_sync_context(2)" in reactor_handler,
        "reactor replication handler no longer sets sync_context=2")
require("kvs_handler(parser->tokens" in reactor_handler,
        "reactor replication handler no longer calls kvs_handler")
require("ext->is_replication || reactor_command_is_replsync(session)" in reactor_submit,
        "reactor submit no longer has replication/REPLSYNC branch")
require("reactor_handle_replication_fallback(fd, ext, cmd_len)" in reactor_submit,
        "reactor replication branch no longer uses explicit fallback classifier")
require("net_session_process_resp" not in reactor_submit and "reactor_session_handler" not in reactor_submit,
        "reactor replication branch still uses synchronous handler")
require("kvs_command_executor_submit_replicated(" in reactor_fallback,
        "reactor replication fallback no longer submits replicated command task")
require("kvs_command_executor_submit(task)" in reactor_submit_task,
        "reactor normal path no longer submits to executor")

require("strcasecmp(cmd_name, \"REPLSYNC\")" in proactor_handler,
        "proactor handler no longer handles REPLSYNC")
require("ext->is_replication = 1" in proactor_handler,
        "proactor REPLSYNC no longer marks replication connection")
require("set_sync_context(2)" in proactor_handler,
        "proactor replication handler no longer sets sync_context=2")
require("kvs_handler(parser->tokens" in proactor_handler,
        "proactor replication handler no longer calls kvs_handler")
require("net_session_process_resp(ext->fd" in proactor_process,
        "proactor p_process_received_data no longer calls net_session_process_resp")
require("proactor_session_handler" in proactor_process,
        "proactor p_process_received_data no longer uses proactor_session_handler")
require("ext->is_replication || proactor_command_is_replsync(session)" in proactor_submit,
        "proactor submit no longer has replication/REPLSYNC branch")
require("proactor_handle_replication_fallback(ring, ext, cmd_len)" in proactor_submit,
        "proactor replication branch no longer uses explicit fallback classifier")
require("p_process_received_data" not in proactor_submit,
        "proactor replication branch still uses p_process_received_data")
require("kvs_command_executor_submit_replicated(" in proactor_fallback,
        "proactor replication fallback no longer submits replicated command task")
require("return p_process_received_data(ext) < 0 ? -1 : 1;" not in proactor_submit,
        "proactor async failure fallback still synchronously executes p_process_received_data")
require("return proactor_fail_async_command(ext, parser, resp, task, \"alloc\")" in proactor_submit_task,
        "proactor alloc failure no longer fail-fast exits")
require("return proactor_fail_async_command(ext, parser, resp, task, \"response_init\")" in proactor_submit_task,
        "proactor response init failure no longer fail-fast exits")
require("return proactor_fail_async_command(ext, parser, resp, task, \"task_build\")" in proactor_submit_task,
        "proactor task build failure no longer fail-fast exits")
require("return proactor_fail_async_command(ext, parser, resp, task, \"copy_argv\")" in proactor_submit_task,
        "proactor argv copy failure no longer fail-fast exits")
require("proactor executor submit failed fast without synchronous fallback" in proactor_submit_task,
        "proactor executor submit failure no longer logs fail-fast path")
require("async_fallback_count" in proactor_submit_task,
        "proactor async fallback stats missing from audited fallback path")
require("p_process_received_data" not in proactor_fail,
        "proactor fail-fast helper must not call synchronous handler path")
require("resp_parser_reset(parser)" in proactor_fail,
        "proactor fail-fast helper should reset parser before closing")
require("kvs_command_executor_submit(task)" in proactor_submit_task,
        "proactor normal path no longer submits to executor")

require("kvs_command_task_execute_sync(&task)" in kvs_filter,
        "kvs_filter_protocol no longer synchronously executes command task")
require("kvs_command_task_post_commit(&task" in kvs_filter,
        "kvs_filter_protocol no longer runs post_commit on synchronous path")
require("KVS_CMD_RETRY_WITH_WRITE_LOCK" in execute_sync,
        "execute_sync no longer contains read-to-write retry path")
require("kvs_owner_rwlock_wrlock(&g_kv_data_rwlock" in execute_sync,
        "execute_sync no longer uses g_kv_data_rwlock write lock wrapper")

require("kvs_command_executor_submit_replicated(" in distributed,
        "TCP slave apply no longer submits replicated command task to executor")

print("Stage 10.8A replication/proactor fallback static audit passed")
PY
