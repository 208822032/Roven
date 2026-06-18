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

ntyco = read("src/network/ntyco.c")
main_src = read("src/core/kvstore.c")
executor = read("src/core/kvs_command_executor.c")
task = read("src/core/kvs_command_task.c")

process = find_func(ntyco, "ntyco_process_received_data")
submit_task = find_func(ntyco, "ntyco_submit_command_task")
wait_resp = find_func(ntyco, "ntyco_wait_executor_response")
poll_resp = find_func(ntyco, "ntyco_poll_executor_response")
start = find_func(ntyco, "ntyco_start")

for forbidden in [
    "net_session_process_resp",
    "ntyco_session_handler",
    "kvs_filter_protocol",
    "kvs_handler(",
    "kvs_command_task_execute_sync",
]:
    require(forbidden not in process, f"ntyco process path must not use {forbidden}")
    require(forbidden not in submit_task, f"ntyco submit path must not use {forbidden}")

require("net_session_parse_one_resp" in process,
        "ntyco must parse one RESP command before executor submit")
require("kvs_replication_fallback_classify" in process,
        "ntyco replication fallback must use explicit classifier")
require("kvs_command_executor_submit_replicated(" in process,
        "ntyco replicated apply must submit replicated task")
require("kvs_command_executor_submit(task)" in submit_task,
        "ntyco normal command must submit command_task")
require("kvs_command_task_copy_argv(task)" in submit_task,
        "ntyco command task must own argv before submit")
require("KVS_ASYNC_NET_OWNER_NTYCO" in submit_task,
        "ntyco command task must set NTYCO owner")
require("kvs_command_executor_poll_response_for_owner" in poll_resp,
        "ntyco must poll owner-filtered executor responses")
require("resp->session_id != ctx->session.session_id" in poll_resp and
        "resp->generation != ctx->session.generation" in poll_resp,
        "ntyco response polling must guard fd reuse/session mismatch")
require("ntyco_append_inline_response(session, buf, len)" in wait_resp,
        "ntyco executor response must return through ntyco send buffer")
require("send(" not in wait_resp and "nty_send(" not in wait_resp,
        "ntyco executor wait path must not send socket bytes")
require("(void)handler" in start,
        "ntyco_start must not retain kvs_filter_protocol handler")
require("network_mode=ntyco routes commands through single command executor" in main_src,
        "ntyco startup log must describe executor routing")
require("pthread_rwlock_rdlock(&g_kv_data_rwlock)" in task and
        "pthread_rwlock_wrlock(&g_kv_data_rwlock)" in task,
        "g_kv_data_rwlock must remain retained")
require("kvs_command_executor_submit_replicated" in executor,
        "replicated command executor path missing")

print("Stage 10.13 ntyco executor static audit passed")
PY
