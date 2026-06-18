#!/usr/bin/env bash
set -euo pipefail

ROOT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
cd "${ROOT_DIR}"

python3 - <<'PY'
from pathlib import Path
import re

root = Path(".")
files = list((root / "src").rglob("*.c")) + list((root / "include").rglob("*.h"))
matches = []
for path in files:
    text = path.read_text(encoding="utf-8", errors="replace")
    for lineno, line in enumerate(text.splitlines(), 1):
        if "g_shutdown_flag" in line:
            matches.append((path.as_posix(), lineno, line.strip()))

allowed = {
    ("src/core/kvstore.c", "static int g_shutdown_flag = 0;"),
}
for path, lineno, line in matches:
    if path == "src/core/kvstore.c" and (
        line == "static int g_shutdown_flag = 0;" or
        "__atomic_load_n(&g_shutdown_flag" in line or
        "__atomic_store_n(&g_shutdown_flag" in line
    ):
        continue
    raise SystemExit(f"unexpected g_shutdown_flag reference: {path}:{lineno}: {line}")

required = {
    "src/core/kvs_operation.c": "kvs_shutdown_request();",
    "src/distributed/distributed.c": "kvs_shutdown_requested()",
    "src/network/reactor.c": "kvs_shutdown_requested()",
    "src/network/proactor.c": "kvs_shutdown_requested()",
    "src/network/ntyco.c": "kvs_shutdown_requested()",
    "include/kvs_shutdown.h": "kvs_shutdown_requested",
}
for path, needle in required.items():
    text = Path(path).read_text(encoding="utf-8", errors="replace")
    if needle not in text:
        raise SystemExit(f"missing shutdown helper usage: {path}: {needle}")

print("stage12 shutdown flag atomic audit: ok")
PY
