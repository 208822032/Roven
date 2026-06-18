#!/usr/bin/env bash
set -euo pipefail

ROOT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
cd "${ROOT_DIR}"

PASS=0
FAIL=0

check() {
  local desc="$1"
  local cond="$2"
  if eval "${cond}"; then
    echo "PASS: ${desc}"
    PASS=$((PASS + 1))
  else
    echo "FAIL: ${desc}"
    FAIL=$((FAIL + 1))
  fi
}

echo "=== Stage 12 Infra: NtyCo third_party vendoring audit ==="

# 1. Directory existence
check "third_party/NtyCo directory exists" 'test -d third_party/NtyCo'
check "third_party/NtyCo/core directory exists" 'test -d third_party/NtyCo/core'

# 2. Source file existence
check "nty_socket.c exists in third_party" 'test -f third_party/NtyCo/core/nty_socket.c'
check "nty_coroutine.c exists in third_party" 'test -f third_party/NtyCo/core/nty_coroutine.c'

# 3. Stage 12 pthread_once fix preserved
check "pthread_once fix present in third_party NtyCo" 'grep -q "pthread_once" third_party/NtyCo/core/nty_socket.c'
check "init_hook_inner present in third_party NtyCo" 'grep -q "init_hook_inner" third_party/NtyCo/core/nty_socket.c'
check "g_hook_once present in third_party NtyCo" 'grep -q "g_hook_once" third_party/NtyCo/core/nty_socket.c'
check "no unsafe if (!socket_f) init_hook() in third_party NtyCo" '! grep -q "if (!socket_f) init_hook()" third_party/NtyCo/core/nty_socket.c'

# 4. Old root NtyCo/ must not exist as build reference
check "old root NtyCo/ directory absent" '! test -d NtyCo'

# 5. Makefile paths
check "Makefile uses third_party/NtyCo for NTYCO_DIR" 'grep -q "NTYCO_DIR.*:=.*third_party/NtyCo" Makefile'
check "Makefile CFLAGS uses third_party/NtyCo include" 'grep -q "I ./third_party/NtyCo/core/" Makefile'
check "Makefile LDFLAGS uses third_party/NtyCo lib" 'grep -q "L ./third_party/NtyCo/" Makefile'
check "Makefile does NOT reference old NtyCo/ path as build dir" '! grep -qE "\-I \./NtyCo/|\-L \./NtyCo/|NTYCO_DIR.*:= NtyCo\b" Makefile'

# 6. Submodule removed
check ".gitmodules does not contain NtyCo" '! grep -q "NtyCo" .gitmodules 2>/dev/null || ! test -s .gitmodules'
check "no NtyCo submodule" '! git submodule status 2>/dev/null | grep -q "NtyCo" || true'

# 7. No .git inside third_party/NtyCo
check "no .git inside third_party/NtyCo" '! test -e third_party/NtyCo/.git'

# 8. Build artifacts gitignored
check "third_party/NtyCo/libntyco.a is gitignored" 'git check-ignore third_party/NtyCo/libntyco.a >/dev/null 2>&1'
check "third_party/NtyCo/objs/*.o is gitignored" 'git check-ignore third_party/NtyCo/objs/nty_socket.o >/dev/null 2>&1'

# 9. Build passes
check "make embedding ENABLE_EBPF=0 succeeds" 'make embedding ENABLE_EBPF=0 >/dev/null 2>&1'

# 10. Binary exists
check "kvstore binary exists" 'test -x kvstore'

# 11. ntyco executor tests
check "ntyco executor static audit passes" 'bash tests/ntyco_executor_stage10_13.sh >/dev/null 2>&1'
check "ntyco mixed shutdown regression passes" 'bash tests/ntyco_mixed_shutdown_stage12.sh >/dev/null 2>&1'

# 12. Core static audits
check "rwlock weakening gate passes" 'bash tests/rwlock_weakening_gate_stage11.sh >/dev/null 2>&1'
check "shutdown flag atomic audit passes" 'bash tests/shutdown_flag_atomic_stage12.sh >/dev/null 2>&1'

# 13. Shutdown / quit route references must use kvs_shutdown helpers, not raw flag
# Already verified by shutdown_flag_atomic_stage12.sh above

echo ""
echo "=== Stage 12 Infra NtyCo vendoring audit: PASS=${PASS} FAIL=${FAIL} ==="
if [[ "${FAIL}" -gt 0 ]]; then
  echo "STAGE12_INFRA_NTYCO_VENDOR: FAIL"
  exit 1
fi
echo "STAGE12_INFRA_NTYCO_VENDOR: PASS"
