#!/usr/bin/env bash
set -euo pipefail

ROOT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
cd "${ROOT_DIR}"

TSAN_TMP="${TSAN_TMP:-/tmp/kvlite-stage12-tsan.$$}"
export KEEP_STAGE12_TMP="${KEEP_STAGE12_TMP:-1}"
mkdir -p "${TSAN_TMP}"
BUILD_LOG="${TSAN_TMP}/build.log"

build_with_toolchain() {
  local cc="$1"
  local cxx="$2"

  echo "Stage 12 TSAN: trying CC=${cc} CXX=${cxx}" >>"${BUILD_LOG}"
  if ! make clean >>"${BUILD_LOG}" 2>&1; then
    return 1
  fi
  if ! make CC="${cc}" CXX="${cxx}" \
      embedding ENABLE_EBPF=0 \
      OPT_CFLAGS="-O1" \
      EXTRA_CFLAGS="-fsanitize=thread -fno-omit-frame-pointer" \
      EXTRA_LIBS="-fsanitize=thread" >>"${BUILD_LOG}" 2>&1; then
    return 1
  fi
  return 0
}

echo "Stage 12 TSAN: attempting optional build"
if build_with_toolchain gcc g++; then
  TSAN_CC="gcc"
elif command -v clang >/dev/null 2>&1 && command -v clang++ >/dev/null 2>&1 && \
     build_with_toolchain clang clang++; then
  TSAN_CC="clang"
else
  echo "Stage 12 TSAN: SKIP toolchain/dependency does not support TSAN build; see ${BUILD_LOG}"
  exit 0
fi

echo "Stage 12 TSAN: build ok with ${TSAN_CC}, running reactor stress"
TSAN_OPTIONS="${TSAN_OPTIONS:-halt_on_error=1 exitcode=66 second_deadlock_stack=1}" \
RUN_MODES="${RUN_MODES:-reactor}" \
CLIENT_LEVELS="${CLIENT_LEVELS:-4}" \
REQUESTS_PER_CLIENT="${REQUESTS_PER_CLIENT:-20}" \
TMP_DIR="${TSAN_TMP}/stress" \
tests/weak_rwlock_mixed_stress_stage12.sh

echo "Stage 12 TSAN: PASS (${TSAN_CC})"
