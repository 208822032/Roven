#!/usr/bin/env bash
set -e

SCRIPT_DIR="$(cd "$(dirname "$0")" && pwd)"

python3 -m grpc_tools.protoc \
  -I "${SCRIPT_DIR}" \
  --python_out="${SCRIPT_DIR}" \
  --grpc_python_out="${SCRIPT_DIR}" \
  "${SCRIPT_DIR}/embedding.proto"

echo "generated embedding_pb2.py and embedding_pb2_grpc.py"