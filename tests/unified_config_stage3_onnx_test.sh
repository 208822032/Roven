#!/usr/bin/env bash
set -euo pipefail

ROOT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
TMP_DIR="$(mktemp -d /tmp/kvlite-unified-config-stage3-onnx-XXXXXX)"
PORT="${KVLITE_STAGE3_ONNX_PORT:-28673}"
SERVER_PID=""

cleanup() {
  if [[ -n "$SERVER_PID" ]] && kill -0 "$SERVER_PID" 2>/dev/null; then
    kill -TERM "$SERVER_PID" 2>/dev/null || true
    wait "$SERVER_PID" 2>/dev/null || true
  fi
  rm -rf "$TMP_DIR"
}
trap cleanup EXIT

cd "$ROOT_DIR"
make -s embedding -j"$(nproc)"
mkdir -p "$TMP_DIR/data"

write_config() {
  local path="$1"
  local dimension="$2"
  local model_version="$3"
  local rdb_enabled="$4"
  cat >"$path" <<EOF
[server]
role=master
config_root=$ROOT_DIR
data_dir=$TMP_DIR/data

[network]
listen_address=127.0.0.1
port=$PORT
mode=reactor

[persistence]
rdb_enabled=$rdb_enabled
rdb_path=vector-contract.rdb
aof_enabled=false

[vector]
enabled=true
index_type=core_flat
dimension=$dimension
metric=ip
normalize=true
hnsw_m=48
hnsw_m0=96
hnsw_ef_construction=512
hnsw_ef_search=1024
initial_capacity=64
default_topk=1

[embedding]
provider=local_c
model_path=models/bge-small-zh-v1.5/model.onnx
vocab_path=models/bge-small-zh-v1.5/vocab.txt
output_dimension=$dimension
model_version=$model_version
intra_op_threads=1
inter_op_threads=0
ort_graph_optimization=basic
ort_execution_mode=sequential
max_text_bytes=4096
max_tokens=512
worker_pool_enabled=false
cache_enabled=false

[vector_snapshot]
enabled=false

[vector_rebuild]
async_enabled=false

[replication]
transport=tcp
sync_listen_port=$((PORT + 1000))

[observability]
embedding_profile_enabled=false
hnsw_build_profile_enabled=false
hnsw_search_profile_enabled=false
EOF
}

write_config "$TMP_DIR/good.conf" 512 stage3-semantic true
./kvstore --config "$TMP_DIR/good.conf" >"$TMP_DIR/server.log" 2>&1 &
SERVER_PID=$!

python3 - "$PORT" <<'PY'
import socket
import sys
import time

port = int(sys.argv[1])
deadline = time.time() + 30
while time.time() < deadline:
    try:
        with socket.create_connection(("127.0.0.1", port), timeout=1):
            raise SystemExit(0)
    except OSError:
        time.sleep(0.1)
raise SystemExit("server did not become ready")
PY

tests/vector/semantic_onnx_test.sh 127.0.0.1 "$PORT"
kill -TERM "$SERVER_PID"
wait "$SERVER_PID" || true
SERVER_PID=""

grep -q "model_identity=" "$TMP_DIR/server.log"
grep -q "vocab loaded path=$ROOT_DIR/models/bge-small-zh-v1.5/vocab.txt" "$TMP_DIR/server.log"

write_config "$TMP_DIR/bad-dimension.conf" 384 stage3-semantic false
if ./kvstore --config "$TMP_DIR/bad-dimension.conf" >"$TMP_DIR/bad.log" 2>&1; then
  echo "FAIL: ONNX output dimension mismatch unexpectedly started" >&2
  exit 1
fi
grep -q "output dimension" "$TMP_DIR/bad.log"

write_config "$TMP_DIR/bad-model-version.conf" 512 stage3-other-version true
if ./kvstore --config "$TMP_DIR/bad-model-version.conf" >"$TMP_DIR/bad-model.log" 2>&1; then
  echo "FAIL: recovered record model mismatch unexpectedly started" >&2
  exit 1
fi
grep -q "record model mismatch" "$TMP_DIR/bad-model.log"

echo "unified config Stage 3 ONNX tests: PASS"
