#!/usr/bin/env bash
set -euo pipefail

ROOT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
cd "${ROOT_DIR}"

KV_BIN="${KV_BIN:-./kvstore}"
HOST="${KV_HOST:-127.0.0.1}"
TMP_DIR="${TMP_DIR:-/tmp/kvlite-route-c-final-closeout.$$}"
ARTIFACT_DIR="${ARTIFACT_DIR:-${TMP_DIR}/artifacts}"
KEEP_TMP="${KEEP_TMP:-0}"

SERVER_PID=""
SERVER_PORT=""
LAST_LOG_FILE=""

pick_port() {
  python3 - <<'PY'
import socket
s = socket.socket()
s.bind(("127.0.0.1", 0))
print(s.getsockname()[1])
s.close()
PY
}

cleanup() {
  stop_server
  if [[ "${KEEP_TMP}" != "1" ]]; then
    rm -rf "${TMP_DIR}"
  else
    printf '[INFO] keeping tmp dir: %s\n' "${TMP_DIR}" >&2
  fi
}
trap cleanup EXIT

stop_server() {
  if [[ -n "${SERVER_PID}" ]] && kill -0 "${SERVER_PID}" >/dev/null 2>&1; then
    if [[ -n "${SERVER_PORT}" ]]; then
      python3 - "${HOST}" "${SERVER_PORT}" >/dev/null 2>&1 <<'PY' || true
import socket
import sys
host = sys.argv[1]
port = int(sys.argv[2])
payload = b"*1\r\n$4\r\nQUIT\r\n"
try:
    s = socket.create_connection((host, port), timeout=2)
    s.sendall(payload)
    try:
        s.recv(64)
    except OSError:
        pass
    s.close()
except OSError:
    pass
PY
    fi
    for _ in $(seq 1 100); do
      if ! kill -0 "${SERVER_PID}" >/dev/null 2>&1; then
        break
      fi
      sleep 0.1
    done
    if kill -0 "${SERVER_PID}" >/dev/null 2>&1; then
      kill -TERM "${SERVER_PID}" >/dev/null 2>&1 || true
      wait "${SERVER_PID}" >/dev/null 2>&1 || true
    fi
  fi
  SERVER_PID=""
  SERVER_PORT=""
}

wait_for_ready() {
  local port="$1"
  python3 - "${HOST}" "${port}" <<'PY'
import socket
import sys
import time

host = sys.argv[1]
port = int(sys.argv[2])

def enc(tokens):
    out = [f"*{len(tokens)}\r\n".encode()]
    for token in tokens:
        data = str(token).encode()
        out += [f"${len(data)}\r\n".encode(), data, b"\r\n"]
    return b"".join(out)

deadline = time.time() + 45
while time.time() < deadline:
    try:
        s = socket.create_connection((host, port), timeout=2)
        s.sendall(enc(["SET", "route_c_closeout_probe", "1"]))
        data = s.recv(64)
        s.close()
        if data.startswith(b"+OK") or data.startswith(b"+EXIST"):
            raise SystemExit(0)
    except OSError:
        time.sleep(0.1)
raise SystemExit("server did not become ready")
PY
}

write_conf() {
  local conf="$1"
  local port="$2"
  local network_mode="$3"
  local profile="$4"
  local data_dir="$5"

  cat >"${conf}" <<EOF_CONF
role = master
port = ${port}
sync_port = $((port + 1000))
network_mode = ${network_mode}
enable_aof = yes
aof_fsync_mode = no
aof_filename = ${data_dir}/closeout.aof
rdb_filename = ${data_dir}/closeout.rdb
dump_filename = ${data_dir}/closeout.dat
mem_backend = pool
sync_mode = tcp
log_level = info
log_async = no
vector_index_type = hnsw
vector_metric = ip
vector_score_threshold = 0.0
vector_hnsw_m = 16
vector_hnsw_ef_construction = 64
vector_hnsw_ef_search = 64
EOF_CONF

  if [[ "${profile}" == "embedding" ]]; then
    cat >>"${conf}" <<'EOF_CONF'
embedding_provider = local_c
embedding_model_path = models/bge-small-zh-v1.5/model.onnx
embedding_dim = 512
embedding_num_threads = 1
embedding_max_text_len = 512
embedding_ort_graph_opt_level = basic
embedding_inter_threads = 0
embedding_ort_execution_mode = sequential
embedding_worker_pool_enabled = 0
embedding_worker_threads = 1
embedding_prepare_queue_capacity = 1024
embedding_worker_queue_capacity = 1024
embedding_per_thread_runtime_enabled = 0
embedding_per_thread_runtime_fallback_mutex = 1
embedding_shared_ort_env_enabled = 1
embedding_batch_enabled = 0
embedding_batch_vset_enabled = 0
embedding_batch_vsearch_enabled = 1
embedding_batch_vsearch_max_size = 4
embedding_batch_vsearch_max_wait_us = 500
EOF_CONF
  else
    cat >>"${conf}" <<'EOF_CONF'
embedding_provider = hash
embedding_dim = 64
embedding_num_threads = 1
embedding_max_text_len = 512
embedding_worker_pool_enabled = no
EOF_CONF
  fi
}

start_server() {
  local mode_name="$1"
  local network_mode="$2"
  local profile="$3"
  local mode_dir="${TMP_DIR}/${mode_name}/${network_mode}-${profile}"
  local conf="${mode_dir}/kvstore.conf"
  local log="${mode_dir}/kvstore.log"
  local port

  mkdir -p "${mode_dir}"
  port="$(pick_port)"
  write_conf "${conf}" "${port}" "${network_mode}" "${profile}" "${mode_dir}"

  if [[ "${mode_name}" == "weak" ]]; then
    KVLITE_ROUTE_C_WEAK_RWLOCK=1 KVLITE_EMBEDDING_PROFILE=1 "${KV_BIN}" --config "${conf}" >"${log}" 2>&1 &
  else
    KVLITE_ROUTE_C_WEAK_RWLOCK=0 KVLITE_EMBEDDING_PROFILE=1 "${KV_BIN}" --config "${conf}" >"${log}" 2>&1 &
  fi

  SERVER_PID=$!
  SERVER_PORT="${port}"
  wait_for_ready "${port}"
  LAST_LOG_FILE="${log}"
}

parse_owner_stats() {
  local log_file="$1"
  local out_json="$2"
  python3 - "${log_file}" "${out_json}" <<'PY'
import json
import re
import sys

log_file = sys.argv[1]
out_json = sys.argv[2]
line = ""
with open(log_file, encoding="utf-8", errors="replace") as f:
    for current in f:
        if "owner rwlock stats:" in current:
            line = current.strip()
if not line:
    raise SystemExit(f"missing owner rwlock stats in {log_file}")
stats = {k: int(v) for k, v in re.findall(r"([a-z_]+)=([0-9]+)", line)}
stats["_line"] = line
with open(out_json, "w", encoding="utf-8") as f:
    json.dump(stats, f, indent=2, sort_keys=True)
    f.write("\n")
print(json.dumps(stats, sort_keys=True))
PY
}

run_resp_bench() {
  local port="$1"
  local command_tpl="$2"
  local expect_type="$3"
  local out_json="$4"
  local requests="$5"
  local clients="$6"
  local pipeline="$7"
  python3 scripts/kvstore_resp_bench.py \
    --host "${HOST}" \
    --port "${port}" \
    --clients "${clients}" \
    --requests "${requests}" \
    --pipeline "${pipeline}" \
    --command "${command_tpl}" \
    --value-size 64 \
    --keyspace 5000 \
    --timeout 30 \
    --validate-ratio 0.01 \
    --expect-type "${expect_type}" \
    --system kvlite \
    --ds route_c \
    --op closeout >"${out_json}"
}

run_vector_bench() {
  local port="$1"
  local op="$2"
  local out_file="$3"
  shift 3
  tests/vector_benchmark --mode throughput -h "${HOST}" -p "${port}" -t "${op}" "$@" >"${out_file}"
}

run_ttl_bench() {
  local port="$1"
  local scenario="$2"
  local out_file="$3"
  tests/ttl_expired_path_bench "${HOST}" "${port}" 5 prepare "${scenario}" 5000 5000 1000 2 2 32 >"${out_file}.prepare"
  tests/ttl_expired_path_bench "${HOST}" "${port}" 5 measure "${scenario}" 5000 5000 1000 2 2 32 >"${out_file}.measure"
}

run_timed_script() {
  local mode_name="$1"
  local label="$2"
  local script_path="$3"
  local out_json="$4"
  local start_ns end_ns elapsed_ms

  start_ns="$(date +%s%N)"
  if [[ "${mode_name}" == "weak" ]]; then
    KVLITE_ROUTE_C_WEAK_RWLOCK=1 bash "${script_path}" >"${out_json}.log" 2>&1
  else
    KVLITE_ROUTE_C_WEAK_RWLOCK=0 bash "${script_path}" >"${out_json}.log" 2>&1
  fi
  end_ns="$(date +%s%N)"
  elapsed_ms="$(( (end_ns - start_ns) / 1000000 ))"
  python3 - "${label}" "${script_path}" "${elapsed_ms}" "${out_json}" <<'PY'
import json
import sys
data = {
    "label": sys.argv[1],
    "script": sys.argv[2],
    "elapsed_ms": int(sys.argv[3]),
}
with open(sys.argv[4], "w", encoding="utf-8") as f:
    json.dump(data, f, indent=2, sort_keys=True)
    f.write("\n")
print(json.dumps(data, sort_keys=True))
PY
}

run_mode_bench() {
  local mode_name="$1"
  local mode_dir="${ARTIFACT_DIR}/${mode_name}"

  mkdir -p "${mode_dir}"

  start_server "${mode_name}" reactor hash
  run_resp_bench "${SERVER_PORT}" "SET key:__rand_int__ __value__" "simple" "${mode_dir}/getset_set.json" 20000 32 8
  run_resp_bench "${SERVER_PORT}" "GET key:__rand_int__" "bulk" "${mode_dir}/getset_get.json" 20000 32 8
  run_ttl_bench "${SERVER_PORT}" 2 "${mode_dir}/ttl"
  stop_server
  parse_owner_stats "${LAST_LOG_FILE}" "${mode_dir}/owner_hash_reactor.json"

  start_server "${mode_name}" reactor embedding
  run_vector_bench "${SERVER_PORT}" vset "${mode_dir}/vector_vset.txt" -n 200 -c 4 -P 1 --text-mode semantic --scope closeout_scope --prefix "${mode_name}_vset"
  run_vector_bench "${SERVER_PORT}" vsearch "${mode_dir}/vector_vsearch.txt" -n 200 -c 4 -P 1 --text-mode semantic --scope closeout_scope --prefix "${mode_name}_vsearch" --prepare 64 --topk 5 --threshold 0.0
  stop_server
  parse_owner_stats "${LAST_LOG_FILE}" "${mode_dir}/owner_embedding_reactor.json"

  run_timed_script "${mode_name}" "rdb_snapshot" "tests/rdb_snapshot_stage10_6.sh" "${mode_dir}/rdb_snapshot.json"
  run_timed_script "${mode_name}" "aof_rewrite_snapshot" "tests/aof_rewrite_snapshot_stage10_7.sh" "${mode_dir}/aof_rewrite_snapshot.json"
  run_timed_script "${mode_name}" "master_full_sync" "tests/master_full_sync_stage10_5.sh" "${mode_dir}/master_full_sync.json"
  run_timed_script "${mode_name}" "replicated_command_task" "tests/replicated_command_task_stage10_3.sh" "${mode_dir}/replicated_command_task.json"
}

run_network_matrix() {
  local out_log="${ARTIFACT_DIR}/weak_rwlock_mixed_stress.log"
  local tmp_keep="${TMP_DIR}/mixed-stress"
  KEEP_STAGE12_TMP=1 TMP_DIR="${tmp_keep}" CLIENT_LEVELS="4 8" REQUESTS_PER_CLIENT="10" RUN_MODES="reactor proactor ntyco" \
    bash tests/weak_rwlock_mixed_stress_stage12.sh >"${out_log}" 2>&1
  printf '%s\n' "${tmp_keep}"
}

run_correctness() {
  local log_file="${ARTIFACT_DIR}/weak_rwlock_correctness.log"
  local status_file="${ARTIFACT_DIR}/weak_rwlock_correctness.status"
  local rc=0
  if ! bash tests/weak_rwlock_correctness_stage12.sh >"${log_file}" 2>&1; then
    rc=$?
  fi
  python3 - "${status_file}" "${rc}" <<'PY'
import json
import sys
data = {"exit_code": int(sys.argv[2])}
with open(sys.argv[1], "w", encoding="utf-8") as f:
    json.dump(data, f, indent=2, sort_keys=True)
    f.write("\n")
print(json.dumps(data, sort_keys=True))
PY
}

write_summary() {
  local summary_file="${ARTIFACT_DIR}/summary.txt"
  {
    echo "git_status_short_begin"
    git status --short
    echo "git_status_short_end"
    echo "artifact_dir=${ARTIFACT_DIR}"
  } >"${summary_file}"
}

mkdir -p "${ARTIFACT_DIR}"
write_summary
run_mode_bench weak
run_mode_bench real
MIXED_TMP="$(run_network_matrix)"
run_correctness

printf 'ARTIFACT_DIR=%s\n' "${ARTIFACT_DIR}"
printf 'MIXED_TMP=%s\n' "${MIXED_TMP}"
