#!/usr/bin/env bash
set -euo pipefail

ROOT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
cd "${ROOT_DIR}"

if ! command -v gcc >/dev/null 2>&1; then
  echo "Stage 12 TSAN: SKIP gcc not found"
  exit 0
fi

TSAN_TMP="${TSAN_TMP:-/tmp/kvlite-stage12-tsan.$$}"
export KEEP_STAGE12_TMP="${KEEP_STAGE12_TMP:-1}"
mkdir -p "${TSAN_TMP}"
BUILD_LOG="${TSAN_TMP}/build.log"

echo "Stage 12 TSAN: attempting optional build"
if ! make clean >"${BUILD_LOG}" 2>&1; then
  echo "Stage 12 TSAN: SKIP make clean failed; see ${BUILD_LOG}"
  exit 0
fi

if ! make embedding ENABLE_EBPF=0 OPT_CFLAGS="-O1" EXTRA_CFLAGS="-fsanitize=thread -fno-omit-frame-pointer" EXTRA_LIBS="-fsanitize=thread" >>"${BUILD_LOG}" 2>&1; then
  echo "Stage 12 TSAN: SKIP toolchain/dependency does not support TSAN build; see ${BUILD_LOG}"
  exit 0
fi

echo "Stage 12 TSAN: build ok, running minimal weak-lock stress"
TSAN_OPTIONS="${TSAN_OPTIONS:-halt_on_error=1 exitcode=66 second_deadlock_stack=1}" \
KVLITE_ROUTE_C_WEAK_RWLOCK=1 \
RUN_MODES="${RUN_MODES:-reactor}" \
CLIENT_LEVELS="${CLIENT_LEVELS:-4}" \
REQUESTS_PER_CLIENT="${REQUESTS_PER_CLIENT:-20}" \
TMP_DIR="${TSAN_TMP}/stress" \
tests/weak_rwlock_mixed_stress_stage12.sh

echo "Stage 12 TSAN: PASS"
