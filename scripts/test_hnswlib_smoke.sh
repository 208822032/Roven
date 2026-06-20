#!/bin/bash
set -e
SCRIPT_DIR="$(cd "$(dirname "$0")" && pwd)"
ROOT="$(dirname "$SCRIPT_DIR")"

echo "[*] hnswlib smoke test"

mkdir -p "$ROOT/build"

g++ -std=c++11 \
    -I "$ROOT/third_party/hnswlib" \
    "$ROOT/tests/hnswlib_smoke/hnswlib_smoke.cpp" \
    -o "$ROOT/build/hnswlib_smoke"

echo "[*] compiled OK"

OUTPUT=$("$ROOT/build/hnswlib_smoke")
echo "$OUTPUT"

if echo "$OUTPUT" | grep -q "hnswlib smoke test passed"; then
    echo "[PASS] hnswlib smoke test"
else
    echo "[FAIL] hnswlib smoke test"
    exit 1
fi
