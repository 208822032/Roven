#!/usr/bin/env bash
# B-scale-3 combined HNSW/FAISS sweep smoke test.
#
# Uses a tiny synthetic external cache + groundtruth fixture. It does not call
# ONNX, regenerate B-scale artifacts, or touch product V command paths.

set -euo pipefail

ROOT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
cd "$ROOT_DIR"

BIN="build/tools/bin/generate_real_embedding_hnsw_faiss_sweep_benchmark"
SMOKE_ROOT="/tmp/kvlite_vector_benchmark_artifacts/hnsw_faiss_sweep_smoke_$$_$(date +%s)"
CACHE_DIR="$SMOKE_ROOT/cache"
GT_DIR="$SMOKE_ROOT/groundtruth"
OUT_DIR="$SMOKE_ROOT/out"
OUT_DIR_B="$SMOKE_ROOT/out_profiling_off_alloc"
OUT_DIR_C="$SMOKE_ROOT/out_profiling_off_reuse"
OUT_DIR_PHASED="$SMOKE_ROOT/out_phased"
DATASET_ID="synthetic_hnsw_faiss_sweep"

cleanup() { rm -rf "${SMOKE_ROOT:?}"; }
trap cleanup EXIT

pass() { printf '[PASS] %s\n' "$1"; }
fail() { printf '[FAIL] %s\n' "$1" >&2; exit 1; }

printf '[INFO][build] make gen-embedding-hnsw-faiss-sweep-benchmark\n'
make gen-embedding-hnsw-faiss-sweep-benchmark
[[ -x "$BIN" ]] || fail "runner not found at $BIN"

case "$SMOKE_ROOT" in
	/tmp/*) ;;
	*) fail "SMOKE_ROOT is not under /tmp: $SMOKE_ROOT" ;;
esac

make_fixture() {
	local cache="$1"
	local gt="$2"
	local variant="${3:-ok}"
	python3 - "$cache" "$gt" "$variant" <<'PY'
import hashlib
import json
import os
import struct
import sys

cache, gt, variant = sys.argv[1:4]
os.makedirs(cache, exist_ok=True)
os.makedirs(gt, exist_ok=True)

records = [
    (0, "p0", (1.0, 0.0)),
    (1, "p1", (0.0, 1.0)),
    (2, "p2", (-1.0, 0.0)),
    (3, "p3", (0.0, -1.0)),
]
queries = [
    (0, "q0", (1.0, 0.0)),
    (1, "q1", (0.0, 1.0)),
]
topk = 2
dim = 2

with open(os.path.join(cache, "embeddings.f32"), "wb") as f:
    for _, _, vec in records:
        f.write(struct.pack("<2f", *vec))
with open(os.path.join(cache, "query_embeddings.f32"), "wb") as f:
    for _, _, vec in queries:
        f.write(struct.pack("<2f", *vec))
with open(os.path.join(cache, "records.jsonl"), "w", encoding="utf-8") as f:
    for rid, pid, _ in records:
        f.write(json.dumps({"id": rid, "external_id": pid, "pid": pid, "text": f"record {rid}"}) + "\n")
with open(os.path.join(cache, "queries.jsonl"), "w", encoding="utf-8") as f:
    for qrow, qid, _ in queries:
        f.write(json.dumps({"query_row": qrow, "query_id": qid, "qid": qid, "text": f"query {qrow}"}) + "\n")

def sha(path):
    with open(path, "rb") as f:
        return hashlib.sha256(f.read()).hexdigest()

manifest = {
    "schema_version": 1,
    "dataset_id": "synthetic_hnsw_faiss_sweep",
    "params": {
        "record_count": 4 if variant != "bad_cache_count" else 5,
        "query_count": 2,
        "dim": 2,
        "dtype": "float32",
        "byte_order": "little",
        "normalized": True,
        "metric": "ip_normalized",
    },
    "outputs": {
        "embeddings_f32": {"path": "embeddings.f32", "sha256": sha(os.path.join(cache, "embeddings.f32"))},
        "query_embeddings_f32": {"path": "query_embeddings.f32", "sha256": sha(os.path.join(cache, "query_embeddings.f32"))},
        "records_jsonl": {"path": "records.jsonl", "sha256": sha(os.path.join(cache, "records.jsonl"))},
        "queries_jsonl": {"path": "queries.jsonl", "sha256": sha(os.path.join(cache, "queries.jsonl"))},
    },
}
with open(os.path.join(cache, "manifest.json"), "w", encoding="utf-8") as f:
    json.dump(manifest, f, indent=2)
    f.write("\n")
manifest_sha = sha(os.path.join(cache, "manifest.json"))

gt_rows = [
    {"query_id": "q000000", "query_row": 0, "topk": topk, "metric": "ip_normalized",
     "neighbors": [{"rank": 1, "record_id": 0, "score": 1.0}, {"rank": 2, "record_id": 1, "score": 0.0}]},
    {"query_id": "q000001", "query_row": 1, "topk": topk, "metric": "ip_normalized",
     "neighbors": [{"rank": 1, "record_id": 1, "score": 1.0}, {"rank": 2, "record_id": 0, "score": 0.0}]},
]
with open(os.path.join(gt, "groundtruth.jsonl"), "w", encoding="utf-8") as f:
    for row in gt_rows:
        if variant == "bad_gt_count" and row["query_row"] == 1:
            row = dict(row)
            row["query_row"] = 99
        f.write(json.dumps(row, separators=(",", ":")) + "\n")

gtm = {
    "schema_version": 1,
    "dataset_id": "synthetic_hnsw_faiss_sweep",
    "source": {"manifest_sha256": manifest_sha},
    "params": {
        "record_count": 4 if variant != "bad_gt_count" else 5,
        "query_count": 2,
        "dim": 2,
        "topk": topk,
        "metric": "ip_normalized",
        "ground_truth_source": "core_flat_exact_ip",
    },
    "outputs": {
        "groundtruth_jsonl": {
            "path": "groundtruth.jsonl",
            "sha256": sha(os.path.join(gt, "groundtruth.jsonl")),
            "size_bytes": os.path.getsize(os.path.join(gt, "groundtruth.jsonl")),
            "rows": 2,
        }
    },
}
with open(os.path.join(gt, "groundtruth.manifest.json"), "w", encoding="utf-8") as f:
    json.dump(gtm, f, indent=2)
    f.write("\n")
PY
}

make_fixture "$CACHE_DIR" "$GT_DIR" ok

"$BIN" generate \
	--cache-dir "$CACHE_DIR" \
	--groundtruth-dir "$GT_DIR" \
	--output-dir "$OUT_DIR" \
	--dataset-id "$DATASET_ID" \
	--source-revision smoke-revision \
	--top-k 2 \
	--ef-search-list 2,4 \
	--core-build-profiling on \
	--core-search-profiling on
pass "external cache/groundtruth/output benchmark generated"

"$BIN" validate \
	--cache-dir "$CACHE_DIR" \
	--groundtruth-dir "$GT_DIR" \
	--output-dir "$OUT_DIR" \
	--dataset-id "$DATASET_ID" \
	--top-k 2
pass "validate-only passed"

python3 - "$OUT_DIR" <<'PY'
import json
import os
import sys

out = sys.argv[1]
with open(f"{out}/benchmark_manifest.json") as f:
    m = json.load(f)
assert m["params"]["efSearch_list"] == [2, 4]
assert m["insertion_order"]["qrels_used"] is False if "qrels_used" in m["insertion_order"] else True
assert m["validation"]["qrels_used_for_ann_recall"] is False

ids = [int(x.strip()) for x in open(f"{out}/insertion_order.ids") if x.strip()]
assert sorted(ids) == [0, 1, 2, 3]
assert len(ids) == len(set(ids)) == 4

for name in ("core_hnsw_v2_results.json", "faiss_hnsw_results.json"):
    with open(f"{out}/{name}") as f:
        r = json.load(f)
    assert [x["ef_search"] for x in r["sweep"]] == [2, 4]
    for row in r["sweep"]:
        assert 0 <= row["recall_at_1"] <= 1
        assert 0 <= row["recall_at_10"] <= 1
        assert row["query_count"] == 2
        assert row["failed_query_count"] == 0
        assert row["search_avg_us"] >= 0

with open(f"{out}/faiss_flat_results.json") as f:
    flat = json.load(f)
assert flat["sweep"][0]["recall_at_10"] == 1.0

for name, expected in [
    ("core_hnsw_v2_per_query.jsonl", 4),
    ("faiss_hnsw_per_query.jsonl", 4),
    ("faiss_flat_per_query.jsonl", 2),
]:
    rows = [json.loads(line) for line in open(f"{out}/{name}")]
    assert len(rows) == expected, (name, len(rows))
    for row in rows:
        assert len(row["ann"]) == 2
        assert len(set(row["ann"])) == 2
PY
pass "schema, insertion permutation, efSearch sweep, and flat parity checked"

"$BIN" generate \
	--cache-dir "$CACHE_DIR" \
	--groundtruth-dir "$GT_DIR" \
	--output-dir "$OUT_DIR_PHASED" \
	--backend core \
	--dataset-id "$DATASET_ID" \
	--source-revision smoke-revision \
	--top-k 2 \
	--ef-search-list 2,4 \
	--core-build-profiling off \
	--core-search-profiling off >/dev/null
[[ ! -e "$OUT_DIR_PHASED" ]] || fail "formal output published before both backends completed"
[[ -f "$OUT_DIR_PHASED.work/.core.checkpoint.json" ]] || fail "core checkpoint marker missing"
[[ ! -e "$OUT_DIR_PHASED.work/.faiss.checkpoint.json" ]] || fail "FAISS checkpoint appeared during core phase"
PHASED_CORE_SHA="$(sha256sum "$OUT_DIR_PHASED.work/core_hnsw_v2_per_query.jsonl" | awk '{print $1}')"
if "$BIN" generate \
	--cache-dir "$CACHE_DIR" \
	--groundtruth-dir "$GT_DIR" \
	--output-dir "$OUT_DIR_PHASED" \
	--backend finalize \
	--dataset-id "$DATASET_ID" \
	--source-revision smoke-revision \
	--top-k 2 \
	--ef-search-list 2,4 \
	--core-build-profiling off \
	--core-search-profiling off >"$SMOKE_ROOT/premature_finalize.out" 2>&1; then
	fail "finalize must reject a missing FAISS checkpoint"
fi
[[ "$PHASED_CORE_SHA" == "$(sha256sum "$OUT_DIR_PHASED.work/core_hnsw_v2_per_query.jsonl" | awk '{print $1}')" ]] || fail "failed finalize changed core checkpoint"

"$BIN" generate \
	--cache-dir "$CACHE_DIR" \
	--groundtruth-dir "$GT_DIR" \
	--output-dir "$OUT_DIR_PHASED" \
	--backend core \
	--dataset-id "$DATASET_ID" \
	--source-revision smoke-revision \
	--top-k 2 \
	--ef-search-list 2,4 \
	--core-build-profiling off \
	--core-search-profiling off >"$SMOKE_ROOT/core_resume.out"
grep -q 'backend=core status=reused' "$SMOKE_ROOT/core_resume.out" || fail "valid core checkpoint was not reused"

"$BIN" generate \
	--cache-dir "$CACHE_DIR" \
	--groundtruth-dir "$GT_DIR" \
	--output-dir "$OUT_DIR_PHASED" \
	--backend faiss \
	--dataset-id "$DATASET_ID" \
	--source-revision smoke-revision \
	--top-k 2 \
	--ef-search-list 2,4 >/dev/null
[[ ! -e "$OUT_DIR_PHASED" ]] || fail "formal output published before finalize"
"$BIN" generate \
	--cache-dir "$CACHE_DIR" \
	--groundtruth-dir "$GT_DIR" \
	--output-dir "$OUT_DIR_PHASED" \
	--backend finalize \
	--dataset-id "$DATASET_ID" \
	--source-revision smoke-revision \
	--top-k 2 \
	--ef-search-list 2,4 \
	--core-build-profiling off \
	--core-search-profiling off >/dev/null
"$BIN" validate \
	--cache-dir "$CACHE_DIR" \
	--groundtruth-dir "$GT_DIR" \
	--output-dir "$OUT_DIR_PHASED" \
	--dataset-id "$DATASET_ID" \
	--top-k 2 >/dev/null
[[ ! -e "$OUT_DIR_PHASED.work" ]] || fail "work directory remained after atomic publish"
pass "backend checkpoints survive partial execution, resume, and finalize atomically"

"$BIN" generate \
	--cache-dir "$CACHE_DIR" \
	--groundtruth-dir "$GT_DIR" \
	--output-dir "$OUT_DIR_B" \
	--dataset-id "$DATASET_ID" \
	--source-revision smoke-revision \
	--top-k 2 \
	--ef-search-list 2,4 \
	--core-build-profiling off \
	--core-search-profiling off \
	--core-hit-buffer-reuse off >/dev/null

"$BIN" generate \
	--cache-dir "$CACHE_DIR" \
	--groundtruth-dir "$GT_DIR" \
	--output-dir "$OUT_DIR_C" \
	--dataset-id "$DATASET_ID" \
	--source-revision smoke-revision \
	--top-k 2 \
	--ef-search-list 2,4 \
	--core-build-profiling off \
	--core-search-profiling off \
	--core-hit-buffer-reuse on >/dev/null

python3 - "$OUT_DIR" "$OUT_DIR_B" "$OUT_DIR_C" <<'PY'
import json
import sys

roots = sys.argv[1:]
results = []
rows = []
for root in roots:
    with open(f"{root}/core_hnsw_v2_results.json") as f:
        results.append(json.load(f))
    with open(f"{root}/core_hnsw_v2_per_query.jsonl") as f:
        rows.append([json.loads(line) for line in f])

assert results[0]["search_measurement"] == {
    "hot_loop_profiling_enabled": True,
    "wrapper_hit_buffer_reused": False,
}
assert results[1]["search_measurement"] == {
    "hot_loop_profiling_enabled": False,
    "wrapper_hit_buffer_reused": False,
}
assert results[2]["search_measurement"] == {
    "hot_loop_profiling_enabled": False,
    "wrapper_hit_buffer_reused": True,
}
assert results[0]["build_measurement"]["fine_grained_profiling_enabled"] is True
assert results[1]["build_measurement"]["fine_grained_profiling_enabled"] is False
assert results[2]["build_measurement"]["fine_grained_profiling_enabled"] is False
assert len({r["build_measurement"]["graph_sha256"] for r in results}) == 1
for key in (
    "distance_compute_count",
    "construction_distance_compute_count",
    "neighbor_select_distance_compute_count",
    "reverse_prune_distance_compute_count",
    "temporary_allocation_count",
    "temporary_free_count",
    "temporary_allocation_bytes",
    "known_score_reuse_count",
    "pair_cache_lookup_count",
    "pair_cache_hit_count",
):
    assert len({r["build_measurement"][key] for r in results}) == 1, key

work_keys = (
    "distance_compute_count",
    "visited_node_count",
    "neighbor_scan_count",
    "final_top_count",
)
for triplet in zip(*rows):
    baseline = triplet[0]
    for row in triplet[1:]:
        assert row["ef_search"] == baseline["ef_search"]
        assert row["query_row"] == baseline["query_row"]
        assert row["ann"] == baseline["ann"]
        assert row["hits"] == baseline["hits"]
        assert row["recall_at_1"] == baseline["recall_at_1"]
        assert row["recall_at_10"] == baseline["recall_at_10"]
        assert [row[k] for k in work_keys] == [baseline[k] for k in work_keys]

for result in results:
    for row in result["sweep"]:
        assert row["avg_distance_compute_count"] > 0
        assert row["avg_visited_node_count"] > 0
        assert row["avg_neighbor_scan_count"] > 0
        assert row["avg_final_top_count"] >= 2
assert all(row["avg_final_sort_us"] == 0 for row in results[1]["sweep"])
assert all(row["avg_final_sort_us"] == 0 for row in results[2]["sweep"])
PY
pass "profiling toggle and wrapper buffer reuse preserve ordered results and work counters"

SHA_INS_BEFORE="$(sha256sum "$OUT_DIR/insertion_order.ids" | awk '{print $1}')"
SHA_CORE_BEFORE="$(sha256sum "$OUT_DIR/core_hnsw_v2_per_query.jsonl" | awk '{print $1}')"
SHA_FLAT_BEFORE="$(sha256sum "$OUT_DIR/faiss_flat_per_query.jsonl" | awk '{print $1}')"
SHA_FAISS_BEFORE="$(sha256sum "$OUT_DIR/faiss_hnsw_per_query.jsonl" | awk '{print $1}')"
"$BIN" generate \
	--cache-dir "$CACHE_DIR" \
	--groundtruth-dir "$GT_DIR" \
	--output-dir "$OUT_DIR" \
	--dataset-id "$DATASET_ID" \
	--source-revision smoke-revision \
	--top-k 2 \
	--ef-search-list 2,4 \
	--core-build-profiling on \
	--core-search-profiling on >"$SMOKE_ROOT/repeat.out"
grep -q "status=unchanged core_outputs_identical=true" "$SMOKE_ROOT/repeat.out" || fail "repeat did not return unchanged"
[[ "$SHA_INS_BEFORE" == "$(sha256sum "$OUT_DIR/insertion_order.ids" | awk '{print $1}')" ]] || fail "insertion order changed"
[[ "$SHA_CORE_BEFORE" == "$(sha256sum "$OUT_DIR/core_hnsw_v2_per_query.jsonl" | awk '{print $1}')" ]] || fail "core per-query changed"
[[ "$SHA_FLAT_BEFORE" == "$(sha256sum "$OUT_DIR/faiss_flat_per_query.jsonl" | awk '{print $1}')" ]] || fail "flat per-query changed"
[[ "$SHA_FAISS_BEFORE" == "$(sha256sum "$OUT_DIR/faiss_hnsw_per_query.jsonl" | awk '{print $1}')" ]] || fail "faiss hnsw per-query changed"
pass "repeated generation deterministic and unchanged"

cp "$OUT_DIR/insertion_order.ids" "$OUT_DIR/insertion_order.ids.bak"
printf '0\n0\n2\n3\n' > "$OUT_DIR/insertion_order.ids"
if "$BIN" validate --cache-dir "$CACHE_DIR" --groundtruth-dir "$GT_DIR" --output-dir "$OUT_DIR" --dataset-id "$DATASET_ID" --top-k 2 >"$SMOKE_ROOT/bad_insert.out" 2>&1; then
	fail "validate must fail on duplicate insertion order"
fi
mv "$OUT_DIR/insertion_order.ids.bak" "$OUT_DIR/insertion_order.ids"
pass "validate rejects duplicate insertion order"

cp "$OUT_DIR/core_hnsw_v2_per_query.jsonl" "$OUT_DIR/core_hnsw_v2_per_query.jsonl.bak"
printf '\n' >> "$OUT_DIR/core_hnsw_v2_per_query.jsonl"
if "$BIN" validate --cache-dir "$CACHE_DIR" --groundtruth-dir "$GT_DIR" --output-dir "$OUT_DIR" --dataset-id "$DATASET_ID" --top-k 2 >"$SMOKE_ROOT/bad_per_query.out" 2>&1; then
	fail "validate must fail on corrupt per-query output"
fi
mv "$OUT_DIR/core_hnsw_v2_per_query.jsonl.bak" "$OUT_DIR/core_hnsw_v2_per_query.jsonl"
pass "validate rejects corrupt per-query checksum"

BAD_CACHE="$SMOKE_ROOT/bad_cache"
BAD_GT="$SMOKE_ROOT/bad_gt"
make_fixture "$BAD_CACHE" "$BAD_GT" ok
python3 - "$BAD_CACHE/embeddings.f32" <<'PY'
import sys
p = sys.argv[1]
with open(p, "r+b") as f:
    f.seek(3)
    b = f.read(1)
    f.seek(3)
    f.write(bytes([b[0] ^ 0xff]))
PY
if "$BIN" generate --cache-dir "$BAD_CACHE" --groundtruth-dir "$BAD_GT" --output-dir "$SMOKE_ROOT/bad_out" --dataset-id "$DATASET_ID" --top-k 2 --ef-search-list 2 >"$SMOKE_ROOT/bad_cache.out" 2>&1; then
	fail "generate must reject bad cache checksum"
fi
pass "generate rejects bad cache checksum"

BAD_CACHE2="$SMOKE_ROOT/bad_cache2"
BAD_GT2="$SMOKE_ROOT/bad_gt2"
make_fixture "$BAD_CACHE2" "$BAD_GT2" bad_gt_count
if "$BIN" generate --cache-dir "$BAD_CACHE2" --groundtruth-dir "$BAD_GT2" --output-dir "$SMOKE_ROOT/bad_gt_out" --dataset-id "$DATASET_ID" --top-k 2 --ef-search-list 2 >"$SMOKE_ROOT/bad_gt.out" 2>&1; then
	fail "generate must reject bad ground truth params"
fi
pass "generate rejects bad ground truth schema/count"

echo ""
echo "=== B-scale-3 HNSW/FAISS sweep smoke test: ALL CHECKS PASSED ==="
