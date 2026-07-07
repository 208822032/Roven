#!/usr/bin/env bash
set -euo pipefail

ROOT="$(cd "$(dirname "$0")/.." && pwd)"
cd "$ROOT"

BIN="./build/tests/bin/vector_core_hnsw_v2_scale_stability_test"
if [ ! -x "$BIN" ]; then
	make "$BIN" -j"$(nproc)"
fi

TMP="$(mktemp)"
trap 'rm -f "$TMP"' EXIT

"$BIN" | tee "$TMP"

python3 - "$TMP" <<'PY'
import re
import sys

path = sys.argv[1]
text = open(path, "r", encoding="utf-8").read()
if "vector_core_hnsw_v2_scale_stability_test PASS" not in text:
    raise SystemExit("missing PASS marker")

builds = {}
for line in text.splitlines():
    if line.startswith("hnsw_v2_scale_build "):
        fields = dict(re.findall(r"([A-Za-z0-9_]+)=([^ ]+)", line))
        count = int(fields["count"])
        builds[count] = fields
        if fields.get("duplicate_edge") != "0":
            raise SystemExit(f"duplicate_edge not zero for {count}")
        if fields.get("degree_overflow") != "0":
            raise SystemExit(f"degree_overflow not zero for {count}")
        if fields.get("deleted_count") != "0":
            raise SystemExit(f"deleted_count not zero for {count}")

required_counts = {10000, 50000, 100000}
missing = required_counts - builds.keys()
if missing:
    raise SystemExit(f"missing scale build counts: {sorted(missing)}")

results = {}
for line in text.splitlines():
    if line.startswith("hnsw_v2_scale_result "):
        fields = dict(re.findall(r"([A-Za-z0-9_@]+)=([^ ]+)", line))
        count = int(fields["count"])
        ef = int(fields["ef"])
        recall = float(fields["recall@10"])
        results[(count, ef)] = recall

for count in required_counts:
    for ef in (128, 256, 512):
        if (count, ef) not in results:
            raise SystemExit(f"missing result count={count} ef={ef}")

if results[(10000, 256)] < 0.98:
    raise SystemExit("10K ef=256 recall gate failed")
if results[(10000, 512)] < 0.99:
    raise SystemExit("10K ef=512 recall gate failed")
if not (
    results[(10000, 128)] <= results[(10000, 256)] + 1e-6
    and results[(10000, 256)] <= results[(10000, 512)] + 1e-6
):
    raise SystemExit("10K ef sensitivity is not monotonic")
PY

echo "vector_core_hnsw_v2_scale_stability_test.sh PASS"
