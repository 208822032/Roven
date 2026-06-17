#!/usr/bin/env bash
set -euo pipefail

ROOT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
HOST="${HOST:-127.0.0.1}"
REQUESTS="${REQUESTS:-80}"
PREPARE_COUNT="${PREPARE_COUNT:-80}"
RESULT_DIR="${RESULT_DIR:-${ROOT_DIR}/results/stage8_3_embedding_batch}"
TEXT_MODE="${TEXT_MODE:-semantic}"
SERVER_PID=""
SERVER_PORT=""

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
  if [[ -n "${SERVER_PID}" ]] && kill -0 "${SERVER_PID}" >/dev/null 2>&1; then
    if [[ -n "${SERVER_PORT}" ]]; then
      python3 - "${HOST}" "${SERVER_PORT}" >/dev/null 2>&1 <<'PY' || true
import socket
import sys
host = sys.argv[1]
port = int(sys.argv[2])
s = socket.create_connection((host, port), timeout=2)
s.sendall(b"*1\r\n$4\r\nQUIT\r\n")
try:
    s.recv(64)
except OSError:
    pass
s.close()
PY
    fi
    for _ in $(seq 1 50); do
      if ! kill -0 "${SERVER_PID}" >/dev/null 2>&1; then
        break
      fi
      sleep 0.1
    done
    if kill -0 "${SERVER_PID}" >/dev/null 2>&1; then
      kill "${SERVER_PID}" >/dev/null 2>&1 || true
    fi
    wait "${SERVER_PID}" >/dev/null 2>&1 || true
  fi
}
trap cleanup EXIT

resp_call() {
  local port="$1"
  shift
  python3 - "${HOST}" "${port}" "$@" <<'PY'
import socket
import sys
host = sys.argv[1]
port = int(sys.argv[2])
tokens = sys.argv[3:]
payload = [f"*{len(tokens)}\r\n".encode()]
for token in tokens:
    data = str(token).encode()
    payload.append(f"${len(data)}\r\n".encode())
    payload.append(data)
    payload.append(b"\r\n")
s = socket.create_connection((host, port), timeout=5)
s.sendall(b"".join(payload))
print(s.recv(4096).decode(errors="replace").strip())
s.close()
PY
}

wait_ready() {
  local port="$1"
  python3 - "${HOST}" "${port}" <<'PY'
import socket
import sys
import time
host = sys.argv[1]
port = int(sys.argv[2])
payload = b"*3\r\n$3\r\nSET\r\n$14\r\nstage8_3_ready\r\n$1\r\n1\r\n"
deadline = time.time() + 90
while time.time() < deadline:
    try:
        s = socket.create_connection((host, port), timeout=2)
        s.sendall(payload)
        data = s.recv(16)
        s.close()
        if data.startswith(b"+OK"):
            raise SystemExit(0)
    except OSError:
        time.sleep(0.2)
raise SystemExit("server did not become ready")
PY
}

write_conf() {
  local mode="$1"
  local batch_enabled="$2"
  local batch_size="$3"
  local batch_wait_us="$4"
  local graph_opt="$5"
  local inter_threads="$6"
  local execution_mode="$7"
  local port="$8"
  local conf="$9"
  local data_dir="${10}"
  cat >"${conf}" <<EOF_CONF
role = master
port = ${port}
sync_port = $((port + 1000))
network_mode = ${mode}
enable_aof = no
aof_fsync_mode = no
aof_filename = ${data_dir}/kvlite.aof
rdb_filename = ${data_dir}/kvlite.rdb
dump_filename = ${data_dir}/kvlite.dat
mem_backend = pool
sync_mode = tcp
log_level = info
log_async = no
embedding_provider = local_c
embedding_model_path = models/bge-small-zh-v1.5/model.onnx
embedding_dim = 512
embedding_num_threads = 1
embedding_max_text_len = 512
embedding_ort_graph_opt_level = ${graph_opt}
embedding_inter_threads = ${inter_threads}
embedding_ort_execution_mode = ${execution_mode}
embedding_worker_pool_enabled = 1
embedding_worker_threads = 4
embedding_worker_queue_capacity = 1024
embedding_per_thread_runtime_enabled = 1
embedding_per_thread_runtime_fallback_mutex = 1
embedding_shared_ort_env_enabled = 1
embedding_batch_enabled = ${batch_enabled}
embedding_batch_max_size = ${batch_size}
embedding_batch_max_wait_us = ${batch_wait_us}
vector_index_type = hnsw
vector_metric = ip
vector_score_threshold = 0.0
vector_hnsw_m = 32
vector_hnsw_ef_construction = 128
vector_hnsw_ef_search = 128
vector_scope_overfetch_factor = 4
EOF_CONF
}

run_bench() {
  local port="$1"
  local label="$2"
  local bench_mode="$3"
  local op="$4"
  local clients="$5"
  local outfile="$6"
  local prepare_args=()
  if [[ "${op}" == "vsearch" ]]; then
    prepare_args=(--prepare "${PREPARE_COUNT}")
  fi
  "${ROOT_DIR}/tests/vector_benchmark" \
    --mode "${bench_mode}" \
    -h "${HOST}" \
    -p "${port}" \
    -t "${op}" \
    -n "${REQUESTS}" \
    -c "${clients}" \
    -P 1 \
    --scope "stage8_3_${label}_${op}" \
    --prefix "stage8_3_${label}_${op}" \
    --text-mode "${TEXT_MODE}" \
    --topk 5 \
    --threshold 0.0 \
    "${prepare_args[@]}" \
    >"${outfile}"
}

run_kv_smoke() {
  local port="$1"
  local outfile="$2"
  local set_resp
  local get_resp
  set_resp="$(resp_call "${port}" SET stage8_3_kv_smoke ok)"
  get_resp="$(resp_call "${port}" GET stage8_3_kv_smoke)"
  {
    echo "mode=smoke"
    echo "op=kv_setget"
    echo "set_response=${set_resp}"
    echo "get_response=${get_resp}"
  } >"${outfile}"
  [[ "${set_resp}" == "+OK" ]]
  [[ "${get_resp}" == *"ok"* ]]
}

mkdir -p "${RESULT_DIR}"

COMBOS=(
  "no_batch_basic 0 1 0 basic 0 sequential"
  "batch2_wait0_basic 1 2 0 basic 0 sequential"
  "batch2_wait500_basic 1 2 500 basic 0 sequential"
  "batch4_wait0_basic 1 4 0 basic 0 sequential"
  "batch4_wait500_basic 1 4 500 basic 0 sequential"
  "batch4_wait1000_basic 1 4 1000 basic 0 sequential"
  "batch8_wait500_basic 1 8 500 basic 0 sequential"
  "batch4_wait500_graphall 1 4 500 all 0 sequential"
  "batch4_wait500_inter1 1 4 500 basic 1 sequential"
  "batch4_wait500_parallel 1 4 500 all 1 parallel"
)

for mode in reactor proactor; do
  for combo in "${COMBOS[@]}"; do
    cleanup
    SERVER_PID=""
    SERVER_PORT=""
    read -r label batch_enabled batch_size batch_wait_us graph_opt inter_threads execution_mode <<<"${combo}"

    port="$(pick_port)"
    data_dir="${RESULT_DIR}/${mode}_${label}_data"
    rm -rf "${data_dir}"
    mkdir -p "${data_dir}"
    conf="${RESULT_DIR}/${mode}_${label}.conf"
    log="${RESULT_DIR}/${mode}_${label}.log"
    write_conf \
      "${mode}" \
      "${batch_enabled}" \
      "${batch_size}" \
      "${batch_wait_us}" \
      "${graph_opt}" \
      "${inter_threads}" \
      "${execution_mode}" \
      "${port}" \
      "${conf}" \
      "${data_dir}"

    "${ROOT_DIR}/kvstore" -c "${conf}" >"${log}" 2>&1 &
    SERVER_PID="$!"
    SERVER_PORT="${port}"
    wait_ready "${port}"

    run_kv_smoke "${port}" "${RESULT_DIR}/${mode}_${label}_kv_smoke.txt"
    run_bench "${port}" "${mode}_${label}" latency vset 1 "${RESULT_DIR}/${mode}_${label}_latency_vset.txt"
    run_bench "${port}" "${mode}_${label}" latency vsearch 1 "${RESULT_DIR}/${mode}_${label}_latency_vsearch.txt"
    for clients in 4 8 16; do
      run_bench "${port}" "${mode}_${label}_c${clients}" throughput vset "${clients}" "${RESULT_DIR}/${mode}_${label}_throughput_vset_c${clients}.txt"
      run_bench "${port}" "${mode}_${label}_c${clients}" throughput vsearch "${clients}" "${RESULT_DIR}/${mode}_${label}_throughput_vsearch_c${clients}.txt"
    done

    cleanup
    SERVER_PID=""
    SERVER_PORT=""
    stats_file="${RESULT_DIR}/${mode}_${label}_runtime_stats.txt"
    {
      echo "mode=${mode}"
      echo "label=${label}"
      echo "worker_pool_enabled=1"
      echo "worker_threads=4"
      echo "embedding_num_threads=1"
      echo "resource_product=4"
      echo "per_thread_runtime=1"
      echo "shared_ort_env=1"
      echo "batch_enabled=${batch_enabled}"
      echo "batch_size=${batch_size}"
      echo "batch_wait_us=${batch_wait_us}"
      echo "graph_opt=${graph_opt}"
      echo "inter_threads=${inter_threads}"
      echo "execution_mode=${execution_mode}"
      sed -n 's/.*embedding worker pool stats: //p' "${log}" | tail -n 1
      sed -n 's/.*embedding batch stats: //p' "${log}" | tail -n 1
      sed -n 's/.*embedding executor bottleneck stats: //p' "${log}" | tail -n 1
      sed -n 's/.*embedding runtime final stats: //p' "${log}" | tail -n 1
    } >"${stats_file}"
    echo "${mode} ${label}: wrote ${RESULT_DIR}"
  done
done

python3 - "${RESULT_DIR}" <<'PY'
import pathlib
import sys

root = pathlib.Path(sys.argv[1])
print("file,mode,op,clients,pipeline,qps,avg_ms,p50_ms,p95_ms,p99_ms,success,failed,errors")
for path in sorted(root.glob("*.txt")):
    data = {}
    for line in path.read_text(errors="replace").splitlines():
        if "=" in line:
            k, v = line.split("=", 1)
            data[k.strip()] = v.strip()
    if data.get("mode") in {"latency", "throughput", "smoke"}:
        print(",".join([
            path.name,
            data.get("mode", ""),
            data.get("op", ""),
            data.get("clients", ""),
            data.get("pipeline", ""),
            data.get("qps", ""),
            data.get("avg_ms", ""),
            data.get("p50_ms", ""),
            data.get("p95_ms", ""),
            data.get("p99_ms", ""),
            data.get("success", ""),
            data.get("failed", ""),
            data.get("errors", ""),
        ]))
PY
