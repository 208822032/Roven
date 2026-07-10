#!/usr/bin/env bash
# B4-FAISS-index-only-baseline smoke test.
#
# Uses a disposable /tmp copy of the B1+B2 dataset. It does not call ONNX,
# regenerate ground truth, restore product FAISS backend, or touch official
# B1/B2 artifacts.

set -euo pipefail

ROOT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
cd "$ROOT_DIR"

FAISS_BIN="build/tools/bin/generate_real_embedding_faiss_benchmark"
CACHE_BIN="build/tools/bin/generate_real_embedding_cache"
GT_BIN="build/tools/bin/generate_real_embedding_groundtruth"
REAL_ROOT="/tmp/kvlite_vector_benchmark_artifacts/real_embedding"
DATASET_ID="qa_zh_1k_200_bge_small_zh_v1_5"
SRC_DIR="$REAL_ROOT/$DATASET_ID"
SMOKE_ROOT="/tmp/kvlite_vector_benchmark_artifacts/real_embedding_b4_smoke_$$_$(date +%s)"
SMOKE_DIR="$SMOKE_ROOT/$DATASET_ID"

cleanup() { rm -rf "${SMOKE_ROOT:?}"; }
trap cleanup EXIT

pass() { printf '[PASS] %s\n' "$1"; }
fail() { printf '[FAIL] %s\n' "$1" >&2; exit 1; }

printf '[INFO][build] make gen-embedding-faiss-benchmark\n'
make gen-embedding-faiss-benchmark
[[ -x "$FAISS_BIN" ]] || fail "FAISS benchmark runner not found at $FAISS_BIN"

[[ -f "$SRC_DIR/manifest.json" ]] || fail "B1 cache unavailable: $SRC_DIR/manifest.json"
[[ -f "$SRC_DIR/groundtruth.jsonl" ]] || fail "B2 ground truth unavailable: $SRC_DIR/groundtruth.jsonl"

case "$SMOKE_ROOT" in
	/tmp/*) ;;
	*) fail "SMOKE_ROOT is not under /tmp: $SMOKE_ROOT" ;;
esac

mkdir -p "$SMOKE_ROOT"
cp -r "$SRC_DIR" "$SMOKE_DIR"
rm -f "$SMOKE_DIR/faiss_benchmark_result.json" \
	"$SMOKE_DIR/faiss_hnsw_per_query.jsonl" \
	"$SMOKE_DIR/faiss_flat_per_query.jsonl"

[[ -x "$CACHE_BIN" ]] || make gen-embedding-cache
[[ -x "$GT_BIN" ]] || make gen-embedding-groundtruth
"$CACHE_BIN" validate --dataset-id "$DATASET_ID" --out-root "$REAL_ROOT"
"$GT_BIN" validate --dataset-id "$DATASET_ID" --out-root "$REAL_ROOT"
"$CACHE_BIN" validate --dataset-id "$DATASET_ID" --out-root "$SMOKE_ROOT"
"$GT_BIN" validate --dataset-id "$DATASET_ID" --out-root "$SMOKE_ROOT"
pass "B1 cache + B2 ground truth validate passed"

"$FAISS_BIN" generate --dataset-id "$DATASET_ID" --out-root "$SMOKE_ROOT"
pass "FAISS index-only benchmark generated"

[[ -f "$SMOKE_DIR/faiss_benchmark_result.json" ]] || fail "result manifest not published"
[[ -f "$SMOKE_DIR/faiss_hnsw_per_query.jsonl" ]] || fail "hnsw per-query file not published"
[[ -f "$SMOKE_DIR/faiss_flat_per_query.jsonl" ]] || fail "flat per-query file not published"

python3 - "$SMOKE_DIR" <<'PY'
import json
import math
import sys

d = sys.argv[1]
topk = 10

with open(f"{d}/faiss_benchmark_result.json") as f:
    res = json.load(f)

assert res["schema_version"] == 1
assert res["backend"] == "faiss_index_only"
p = res["params"]
assert p["dim"] == 512
assert p["count"] == 1000
assert p["query_count"] == 200
assert p["topk"] == topk
assert p["thread_count"] == 1
assert p["metric"] == "ip_normalized"
assert p["faiss_metric"] == "METRIC_INNER_PRODUCT"
assert p["flat"]["index"] == "IndexFlatIP"
assert p["hnsw"]["index"] == "IndexHNSWFlat"
assert p["hnsw"]["requested_M"] == 48
assert p["hnsw"]["efConstruction"] == 512
assert p["hnsw"]["efSearch"] == 1024
assert p["hnsw"]["actual_level0_neighbor_capacity"] >= 48

fq = res["flat_quality"]
assert fq["record_id_set_match_all"] is True
assert fq["strict_rank_match_all"] is True
assert fq["max_score_abs_diff"] <= fq["score_tolerance"] <= 1e-4
assert fq["queries_checked"] == 200

hq = res["hnsw_quality"]
for k in ("recall_at_1", "recall_at_10", "top1_match_rate",
          "topk_exact_match_rate", "min_per_query_recall_at_10"):
    assert 0.0 <= hq[k] <= 1.0, (k, hq[k])
assert hq["queries_checked"] == 200
assert hq["recall_at_10"] >= 0.90

perf = res["performance"]
for k in ("cache_load_us", "flat_build_us", "flat_search_avg_us",
          "flat_search_p50_us", "flat_search_p95_us", "flat_search_p99_us",
          "flat_qps", "hnsw_build_us", "hnsw_search_avg_us",
          "hnsw_search_p50_us", "hnsw_search_p95_us", "hnsw_search_p99_us",
          "hnsw_qps"):
    assert k in perf, k
assert perf["flat_qps"] > 0 and perf["hnsw_qps"] > 0
assert perf["flat_search_p50_us"] <= perf["flat_search_p95_us"] <= perf["flat_search_p99_us"]
assert perf["hnsw_search_p50_us"] <= perf["hnsw_search_p95_us"] <= perf["hnsw_search_p99_us"]

st = res["index_stats"]
assert st["flat_ntotal"] == 1000 and st["hnsw_ntotal"] == 1000
assert st["requested_M"] == 48
assert st["actual_level0_neighbor_capacity"] == p["hnsw"]["actual_level0_neighbor_capacity"]
assert st["efConstruction"] == 512 and st["efSearch"] == 1024
assert st["thread_count"] == 1
assert st["flat_serialized_index_bytes"] > 0
assert st["hnsw_serialized_index_bytes"] > 0
assert st["memory_bytes"] is None
assert st["memory_bytes_source"] == "unsupported"
assert st["storage_metric_kind"] == "storage_estimate"

record_ids = set()
with open(f"{d}/records.jsonl") as f:
    for line in f:
        record_ids.add(json.loads(line)["id"])

gt = {}
gt_scores = {}
with open(f"{d}/groundtruth.jsonl") as f:
    for line in f:
        o = json.loads(line)
        gt[o["query_row"]] = [n["record_id"] for n in o["neighbors"]]
        gt_scores[o["query_row"]] = [n["score"] for n in o["neighbors"]]

flat_rows = 0
max_diff = 0.0
with open(f"{d}/faiss_flat_per_query.jsonl") as f:
    for line in f:
        o = json.loads(line)
        qr = o["query_row"]
        ann = o["ann"]
        hits = o["hits"]
        assert qr in gt
        assert ann == gt[qr], (qr, ann, gt[qr])
        assert o["record_id_set_match"] is True
        assert o["strict_rank_match"] is True
        assert len(ann) == len(hits) == topk
        prev = None
        for i, hit in enumerate(hits):
            assert hit["rank"] == i + 1
            assert hit["record_id"] == ann[i]
            assert hit["record_id"] in record_ids
            assert math.isfinite(hit["score"])
            if prev is not None:
                assert hit["score"] <= prev + 1e-7
            prev = hit["score"]
            max_diff = max(max_diff, abs(hit["score"] - gt_scores[qr][i]))
        flat_rows += 1
assert flat_rows == 200
assert max_diff <= 1e-4

sum_r1 = sum_r10 = sum_top1 = sum_exact = 0.0
min_r10 = 1.0
hnsw_rows = 0
with open(f"{d}/faiss_hnsw_per_query.jsonl") as f:
    for line in f:
        o = json.loads(line)
        qr = o["query_row"]
        ann = o["ann"]
        hits = o["hits"]
        assert qr in gt
        assert len(ann) == len(hits) == topk
        assert len(set(ann)) == topk
        prev = None
        for i, hit in enumerate(hits):
            assert hit["rank"] == i + 1
            assert hit["record_id"] == ann[i]
            assert hit["record_id"] in record_ids
            assert math.isfinite(hit["score"])
            if prev is not None:
                assert hit["score"] <= prev + 1e-7
            prev = hit["score"]
        g = gt[qr]
        r10 = len(set(ann) & set(g[:topk])) / topk
        r1 = 1.0 if g[0] in ann else 0.0
        t1 = 1.0 if ann[0] == g[0] else 0.0
        ex = 1.0 if ann == g[:topk] else 0.0
        assert abs(o["recall_at_10"] - r10) < 1e-9
        assert float(o["recall_at_1"]) == r1
        assert float(o["top1_match"]) == t1
        assert float(o["exact_match"]) == ex
        sum_r1 += r1
        sum_r10 += r10
        sum_top1 += t1
        sum_exact += ex
        min_r10 = min(min_r10, r10)
        hnsw_rows += 1
assert hnsw_rows == 200
assert abs(hq["recall_at_1"] - sum_r1 / hnsw_rows) < 1e-9
assert abs(hq["recall_at_10"] - sum_r10 / hnsw_rows) < 1e-9
assert abs(hq["top1_match_rate"] - sum_top1 / hnsw_rows) < 1e-9
assert abs(hq["topk_exact_match_rate"] - sum_exact / hnsw_rows) < 1e-9
assert abs(hq["min_per_query_recall_at_10"] - min_r10) < 1e-9

print(f"[python] flat_exact=yes max_score_diff={max_diff:.9g} "
      f"hnsw_recall@10={hq['recall_at_10']:.4f} "
      f"hnsw_avg_us={perf['hnsw_search_avg_us']:.3f} "
      f"level0_capacity={p['hnsw']['actual_level0_neighbor_capacity']}")
PY
pass "result schema + flat parity + hnsw recall independently verified"

"$FAISS_BIN" validate --dataset-id "$DATASET_ID" --out-root "$SMOKE_ROOT"
pass "B4 validate-only passed"

HNSW_SHA_BEFORE="$(sha256sum "$SMOKE_DIR/faiss_hnsw_per_query.jsonl" | awk '{print $1}')"
FLAT_SHA_BEFORE="$(sha256sum "$SMOKE_DIR/faiss_flat_per_query.jsonl" | awk '{print $1}')"
Q_BEFORE="$(python3 -c "import json; r=json.load(open('$SMOKE_DIR/faiss_benchmark_result.json')); print(json.dumps({'flat_quality':r['flat_quality'],'hnsw_quality':r['hnsw_quality']},sort_keys=True))")"
rm -f "$SMOKE_DIR/faiss_benchmark_result.json" \
	"$SMOKE_DIR/faiss_hnsw_per_query.jsonl" \
	"$SMOKE_DIR/faiss_flat_per_query.jsonl"
"$FAISS_BIN" generate --dataset-id "$DATASET_ID" --out-root "$SMOKE_ROOT" >/dev/null
HNSW_SHA_AFTER="$(sha256sum "$SMOKE_DIR/faiss_hnsw_per_query.jsonl" | awk '{print $1}')"
FLAT_SHA_AFTER="$(sha256sum "$SMOKE_DIR/faiss_flat_per_query.jsonl" | awk '{print $1}')"
Q_AFTER="$(python3 -c "import json; r=json.load(open('$SMOKE_DIR/faiss_benchmark_result.json')); print(json.dumps({'flat_quality':r['flat_quality'],'hnsw_quality':r['hnsw_quality']},sort_keys=True))")"
[[ "$HNSW_SHA_BEFORE" == "$HNSW_SHA_AFTER" ]] || fail "FAISS HNSW per-query output not deterministic"
[[ "$FLAT_SHA_BEFORE" == "$FLAT_SHA_AFTER" ]] || fail "FAISS flat per-query output not deterministic"
[[ "$Q_BEFORE" == "$Q_AFTER" ]] || fail "quality metrics not deterministic"
pass "quality metrics + per-query files deterministic across regenerations"

if "$FAISS_BIN" generate --dataset-id "$DATASET_ID" --out-root "$SMOKE_ROOT" 2>"$SMOKE_ROOT/regen.err"; then
	fail "regenerating an existing FAISS result must fail"
fi
grep -qi "already exists" "$SMOKE_ROOT/regen.err" || fail "regenerate failure did not mention existing result"
pass "regenerating over an existing result fails fast"

CORRUPT_ROOT="$SMOKE_ROOT/corrupt"
CORRUPT_DIR="$CORRUPT_ROOT/$DATASET_ID"
mkdir -p "$CORRUPT_ROOT"
cp -r "$SMOKE_DIR" "$CORRUPT_DIR"
rm -f "$CORRUPT_DIR/faiss_benchmark_result.json" \
	"$CORRUPT_DIR/faiss_hnsw_per_query.jsonl" \
	"$CORRUPT_DIR/faiss_flat_per_query.jsonl"
python3 -c "
p='$CORRUPT_DIR/embeddings.f32'
with open(p,'r+b') as f:
    f.seek(300); b=f.read(1); f.seek(300); f.write(bytes([b[0]^0xFF]))
"
if "$FAISS_BIN" generate --dataset-id "$DATASET_ID" --out-root "$CORRUPT_ROOT" 2>"$SMOKE_ROOT/corrupt.err"; then
	fail "runner must reject corrupted cache"
fi
grep -qi "sha256 mismatch" "$SMOKE_ROOT/corrupt.err" || fail "corrupt cache rejection did not report sha256 mismatch"
pass "corrupted input cache is rejected"

GT_CORRUPT_ROOT="$SMOKE_ROOT/gtcorrupt"
GT_CORRUPT_DIR="$GT_CORRUPT_ROOT/$DATASET_ID"
mkdir -p "$GT_CORRUPT_ROOT"
cp -r "$SMOKE_DIR" "$GT_CORRUPT_DIR"
rm -f "$GT_CORRUPT_DIR/faiss_benchmark_result.json" \
	"$GT_CORRUPT_DIR/faiss_hnsw_per_query.jsonl" \
	"$GT_CORRUPT_DIR/faiss_flat_per_query.jsonl"
python3 -c "
p='$GT_CORRUPT_DIR/groundtruth.jsonl'
with open(p,'r+b') as f:
    f.seek(20); b=f.read(1); f.seek(20); f.write(bytes([b[0]^0xFF]))
"
if "$FAISS_BIN" generate --dataset-id "$DATASET_ID" --out-root "$GT_CORRUPT_ROOT" 2>"$SMOKE_ROOT/gtcorrupt.err"; then
	fail "runner must reject corrupted ground truth"
fi
grep -qi "sha256 mismatch" "$SMOKE_ROOT/gtcorrupt.err" || fail "corrupt ground truth rejection did not report sha256 mismatch"
pass "corrupted ground truth is rejected"

RES_CORRUPT_ROOT="$SMOKE_ROOT/rescorrupt"
RES_CORRUPT_DIR="$RES_CORRUPT_ROOT/$DATASET_ID"
mkdir -p "$RES_CORRUPT_ROOT"
cp -r "$SMOKE_DIR" "$RES_CORRUPT_DIR"
python3 -c "
p='$RES_CORRUPT_DIR/faiss_hnsw_per_query.jsonl'
with open(p,'r+b') as f:
    f.seek(20); b=f.read(1); f.seek(20); f.write(bytes([b[0]^0xFF]))
"
if "$FAISS_BIN" validate --dataset-id "$DATASET_ID" --out-root "$RES_CORRUPT_ROOT" >"$SMOKE_ROOT/resval.out" 2>&1; then
	fail "validate must fail on corrupted B4 result"
fi
grep -qi "sha256 mismatch\\|verification failed" "$SMOKE_ROOT/resval.out" || fail "corrupt result validate did not report checksum failure"
pass "corrupted B4 result fails validate"

echo ""
echo "=== B4 FAISS index-only benchmark smoke test: ALL CHECKS PASSED ==="
