#!/usr/bin/env bash
set -euo pipefail

ROOT="$(cd "$(dirname "$0")/.." && pwd)"
cd "$ROOT"

BIN="./build/tests/bin/vector_core_hnsw_v2_tombstone_policy_test"
if [ ! -x "$BIN" ]; then
	make ENABLE_HNSWLIB=1 ENABLE_FAISS=1 "$BIN" -j"$(nproc)"
fi

TMP="$(mktemp)"
trap 'rm -f "$TMP"' EXIT

"$BIN" | tee "$TMP"

python3 - "$TMP" <<'PY'
import re
import sys

text = open(sys.argv[1], "r", encoding="utf-8").read()
if "vector_core_hnsw_v2_tombstone_policy_test PASS" not in text:
    raise SystemExit("missing PASS marker")

stages = {}
for line in text.splitlines():
    if line.startswith("hnsw_v2_tombstone_policy "):
        fields = dict(re.findall(r"([A-Za-z0-9_]+)=([^ ]+)", line))
        stages[fields["stage"]] = fields

for stage in ("recommend", "strong", "after_rebuild"):
    if stage not in stages:
        raise SystemExit(f"missing stage {stage}")

if stages["recommend"]["recommend"] != "1" or stages["recommend"]["strong"] != "0":
    raise SystemExit("30 percent policy expectation failed")
if stages["strong"]["recommend"] != "1" or stages["strong"]["strong"] != "1":
    raise SystemExit("50 percent policy expectation failed")
if stages["after_rebuild"]["deleted"] != "0":
    raise SystemExit("rebuild did not clear tombstones")
if stages["after_rebuild"]["recommend"] != "0" or stages["after_rebuild"]["strong"] != "0":
    raise SystemExit("policy did not clear after rebuild")
PY

echo "vector_core_hnsw_v2_tombstone_policy_test.sh PASS"
