#!/usr/bin/env bash
set -euo pipefail

ROOT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
TMP_DIR="$(mktemp -d /tmp/kvlite-unified-config-stage3-disabled-XXXXXX)"
PORT="${KVLITE_STAGE3_DISABLED_PORT:-28674}"
SERVER_PID=""

cleanup() {
  if [[ -n "$SERVER_PID" ]] && kill -0 "$SERVER_PID" 2>/dev/null; then
    kill -TERM "$SERVER_PID" 2>/dev/null || true
    wait "$SERVER_PID" 2>/dev/null || true
  fi
  rm -rf "$TMP_DIR"
}
trap cleanup EXIT

mkdir -p "$TMP_DIR/data"
cat >"$TMP_DIR/disabled.conf" <<EOF
[server]
role=master
config_root=$ROOT_DIR
data_dir=$TMP_DIR/data

[network]
listen_address=127.0.0.1
port=$PORT
mode=reactor

[persistence]
rdb_enabled=false
aof_enabled=false

[vector]
enabled=false

[embedding]
provider=local_c
model_path=missing.onnx
worker_pool_enabled=true

[vector_snapshot]
enabled=false

[replication]
transport=tcp
sync_listen_port=$((PORT + 1000))
EOF

cd "$ROOT_DIR"
make -s -j"$(nproc)"
./kvstore --config "$TMP_DIR/disabled.conf" >"$TMP_DIR/server.log" 2>&1 &
SERVER_PID=$!

python3 - "$PORT" <<'PY'
import socket
import sys
import time

port = int(sys.argv[1])
deadline = time.time() + 20
sock = None
while time.time() < deadline:
    try:
        sock = socket.create_connection(("127.0.0.1", port), timeout=1)
        break
    except OSError:
        time.sleep(0.1)
if sock is None:
    raise SystemExit("server did not become ready")

def command(*parts):
    request = [f"*{len(parts)}\r\n".encode()]
    for part in parts:
        data = part.encode()
        request.extend([f"${len(data)}\r\n".encode(), data, b"\r\n"])
    sock.sendall(b"".join(request))
    result = b""
    while not result.endswith(b"\r\n"):
        result += sock.recv(4096)
    return result

for parts in [("VINFO",), ("VSET", "q", "a")]:
    response = command(*parts)
    if b"ERR vector" not in response or b"initialized" not in response:
        raise SystemExit(f"unexpected response for {parts[0]}: {response!r}")
sock.close()
PY

if grep -q "initializing vector module" "$TMP_DIR/server.log"; then
  echo "FAIL: vector module initialized while vector.enabled=false" >&2
  exit 1
fi
if compgen -G "$TMP_DIR/data/vector_index.*" >/dev/null; then
  echo "FAIL: vector snapshot created while vector.enabled=false" >&2
  exit 1
fi

echo "unified config Stage 3 vector-disabled test: PASS"
