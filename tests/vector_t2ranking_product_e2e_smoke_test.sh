#!/usr/bin/env bash
set -euo pipefail

ROOT="$(cd "$(dirname "$0")/.." && pwd)"
TMP="$(mktemp -d /tmp/kvlite-product-e2e-smoke.XXXXXX)"
trap 'rm -rf "$TMP"' EXIT

cd "$ROOT"
make product-e2e-benchmark-server >/dev/null

python3 - "$TMP" <<'PY'
import hashlib
import json
import math
from pathlib import Path
import struct
import sys

root = Path(sys.argv[1])
cache = root / "cache"
gt = root / "groundtruth"
baseline = root / "baseline"
cache.mkdir(); gt.mkdir(); baseline.mkdir()
records = 32
queries = 2
dim = 512

vectors = []
for row in range(records):
    vector = [((row + 3) * (column + 5) % 97) / 97.0 for column in range(dim)]
    norm = math.sqrt(sum(value * value for value in vector))
    vectors.append([value / norm for value in vector])
query_vectors = [vectors[3], vectors[19]]

def write_f32(path, rows):
    with open(path, "wb") as stream:
        for row in rows:
            stream.write(struct.pack("<" + "f" * len(row), *row))

def sha(path):
    return hashlib.sha256(path.read_bytes()).hexdigest()

write_f32(cache / "embeddings.f32", vectors)
write_f32(cache / "query_embeddings.f32", query_vectors)
with open(cache / "records.jsonl", "w", encoding="utf-8") as stream:
    for row in range(records):
        stream.write(json.dumps({"row": row, "id": row, "pid": f"p{row}",
                                 "text": f"synthetic document {row}"}) + "\n")
with open(cache / "queries.jsonl", "w", encoding="utf-8") as stream:
    for row in range(queries):
        stream.write(json.dumps({"query_row": row, "id": row, "qid": f"q{row}",
                                 "text": f"synthetic query {row}"}) + "\n")
outputs = {}
for key, name, shape in [
    ("embeddings_f32", "embeddings.f32", [records, dim]),
    ("query_embeddings_f32", "query_embeddings.f32", [queries, dim]),
    ("records_jsonl", "records.jsonl", None),
    ("queries_jsonl", "queries.jsonl", None),
]:
    item = {"path": name, "sha256": sha(cache / name),
            "size_bytes": (cache / name).stat().st_size}
    if shape:
        item["shape"] = shape
    outputs[key] = item
manifest = {"dataset_id": "product_e2e_smoke", "params": {
    "record_count": records, "query_count": queries, "dim": dim,
    "metric": "ip_normalized", "normalized": True}, "outputs": outputs}
(cache / "manifest.json").write_text(json.dumps(manifest), encoding="utf-8")

truth_rows = []
for query_row, query in enumerate(query_vectors):
    ranked = sorted(((sum(a * b for a, b in zip(query, vector)), row)
                     for row, vector in enumerate(vectors)), key=lambda item: (-item[0], item[1]))[:10]
    truth_rows.append({"query_row": query_row, "neighbors": [
        {"rank": rank + 1, "record_id": row, "score": score}
        for rank, (score, row) in enumerate(ranked)]})
with open(gt / "groundtruth.jsonl", "w", encoding="utf-8") as stream:
    for row in truth_rows:
        stream.write(json.dumps(row) + "\n")
gt_manifest = {"outputs": {"groundtruth_jsonl": {
    "path": "groundtruth.jsonl", "sha256": sha(gt / "groundtruth.jsonl"),
    "size_bytes": (gt / "groundtruth.jsonl").stat().st_size}}}
(gt / "groundtruth.manifest.json").write_text(json.dumps(gt_manifest), encoding="utf-8")

(baseline / "insertion_order.ids").write_text("".join(f"{row}\n" for row in range(records)))
baseline_result = {"params": {"record_count": records}, "sweep": [{
    "ef_search": 32, "search_avg_us": 1.0, "recall_at_10": 1.0}]}
(baseline / "core_hnsw_v2_results.json").write_text(json.dumps(baseline_result))
PY

python3 scripts/benchmark/run_t2ranking_product_e2e.py generate \
  --cache-dir "$TMP/cache" \
  --groundtruth-dir "$TMP/groundtruth" \
  --index-baseline-dir "$TMP/baseline" \
  --output-dir "$TMP/result" \
  --ef-search-list 32 \
  --warmup-rounds 1 \
  --measured-rounds 1 \
  --expected-record-count 32 \
  --expected-query-count 2 \
  --progress-every 0

python3 scripts/benchmark/run_t2ranking_product_e2e.py validate --output-dir "$TMP/result"

python3 - "$TMP/result/product_e2e_result.json" <<'PY'
import json
import sys

result = json.load(open(sys.argv[1], encoding="utf-8"))
assert result["prepare_path_audit"] == {
    "prepared_vset_count": 32,
    "prepared_vsearch_count": 4,
    "public_vsearch_count": 4,
}
assert result["onnx_runtime_encode_count"] == 4
assert result["load"]["failure_count"] == 0
assert result["load"]["vinfo"]["core_hnsw_v2_size"] == "32"
assert result["load"]["vinfo"]["core_hnsw_v2_capacity"] == "32"
assert result["b_scale_7a_prepared"][0]["recall_at_10"] == 1.0
assert result["final_vinfo"]["vector_backend"] == "core_hnsw_v2"
PY

cp -a "$TMP/result" "$TMP/corrupt"
printf '\ncorrupt\n' >> "$TMP/corrupt/product_e2e_per_query.jsonl"
if python3 scripts/benchmark/run_t2ranking_product_e2e.py validate \
    --output-dir "$TMP/corrupt" >/dev/null 2>&1; then
  echo "validator accepted corrupt artifact" >&2
  exit 1
fi

echo "vector_t2ranking_product_e2e_smoke_test: PASS"
