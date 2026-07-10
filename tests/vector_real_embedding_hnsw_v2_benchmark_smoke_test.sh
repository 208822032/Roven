#!/usr/bin/env bash
# B3-core-hnsw-v2-index-only-benchmark smoke test.
#
# Builds the index-only core_hnsw_v2 benchmark runner and verifies, on a
# DISPOSABLE COPY of the B1+B2 dataset:
#   - B1 cache validate + B2 ground truth validate pass (required preconditions)
#   - core_hnsw_v2 benchmark runs over all 200 queries with frozen defaults
#   - result manifest schema, provenance, params (M/M0/efC/efS), index stats
#   - recall recomputed INDEPENDENTLY by python from per-query ann + groundtruth
#   - every ANN record_id back-joins to records.jsonl; ranks/scores/dedup valid
#   - B3 validate-only passes without building v2 / ONNX
#   - deterministic quality: per-query file sha256 identical across regenerations
#   - regenerating over an existing result fails fast
#   - a corrupted input cache is rejected by the runner
#   - a corrupted B3 result fails validate
#   - artifacts live under /tmp
#
# Operates on a copy under a unique temp out-root (trap-cleaned); never touches
# the official dataset. This is a 1K smoke/correctness run, NOT a large-scale SLA.

set -euo pipefail

ROOT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
cd "$ROOT_DIR"

HV2_BIN="build/tools/bin/generate_real_embedding_hnsw_v2_benchmark"
CACHE_BIN="build/tools/bin/generate_real_embedding_cache"
GT_BIN="build/tools/bin/generate_real_embedding_groundtruth"
REAL_ROOT="/tmp/kvlite_vector_benchmark_artifacts/real_embedding"
DATASET_ID="qa_zh_1k_200_bge_small_zh_v1_5"
SRC_DIR="$REAL_ROOT/$DATASET_ID"
SMOKE_ROOT="/tmp/kvlite_vector_benchmark_artifacts/real_embedding_b3_smoke_$$_$(date +%s)"
SMOKE_DIR="$SMOKE_ROOT/$DATASET_ID"

cleanup() { rm -rf "${SMOKE_ROOT:?}"; }
trap cleanup EXIT

pass() { printf '[PASS] %s\n' "$1"; }
fail() { printf '[FAIL] %s\n' "$1" >&2; exit 1; }

printf '[INFO][build] make gen-embedding-hnsw-v2-benchmark\n'
make gen-embedding-hnsw-v2-benchmark
[[ -x "$HV2_BIN" ]] || fail "benchmark runner not found at $HV2_BIN"

# B3 must consume already-frozen B1/B2 artifacts. Missing/corrupt input fails;
# this smoke test must not regenerate embeddings or ground truth.
[[ -f "$SRC_DIR/manifest.json" ]] || fail "B1 cache unavailable: $SRC_DIR/manifest.json"
[[ -f "$SRC_DIR/groundtruth.jsonl" ]] || fail "B2 ground truth unavailable: $SRC_DIR/groundtruth.jsonl"

case "$SMOKE_ROOT" in
	/tmp/*) ;;
	*) fail "SMOKE_ROOT is not under /tmp: $SMOKE_ROOT" ;;
esac

# Disposable copy under the SAME dataset id; strip any pre-existing B3 output.
mkdir -p "$SMOKE_ROOT"
cp -r "$SRC_DIR" "$SMOKE_DIR"
rm -f "$SMOKE_DIR/hnsw_v2_benchmark_result.json" "$SMOKE_DIR/hnsw_v2_per_query.jsonl"

# 1. Required preconditions: B1 + B2 validate.
[[ -x "$CACHE_BIN" ]] || make gen-embedding-cache
[[ -x "$GT_BIN" ]] || make gen-embedding-groundtruth
"$CACHE_BIN" validate --dataset-id "$DATASET_ID" --out-root "$REAL_ROOT"
"$GT_BIN" validate --dataset-id "$DATASET_ID" --out-root "$REAL_ROOT"
"$CACHE_BIN" validate --dataset-id "$DATASET_ID" --out-root "$SMOKE_ROOT"
"$GT_BIN" validate --dataset-id "$DATASET_ID" --out-root "$SMOKE_ROOT"
pass "B1 cache + B2 ground truth validate passed"

# 2. Run the benchmark.
"$HV2_BIN" generate --dataset-id "$DATASET_ID" --out-root "$SMOKE_ROOT"
pass "core_hnsw_v2 benchmark generated (no ONNX, no ground-truth regeneration)"

[[ -f "$SMOKE_DIR/hnsw_v2_benchmark_result.json" ]] || fail "result manifest not published"
[[ -f "$SMOKE_DIR/hnsw_v2_per_query.jsonl" ]] || fail "per-query file not published"

# 3. Independent structural + recall recomputation.
python3 - "$SMOKE_DIR" <<'PY'
import json
import sys

d = sys.argv[1]
topk = 10

with open(f"{d}/hnsw_v2_benchmark_result.json") as f:
    res = json.load(f)

assert res["schema_version"] == 1
assert res["backend"] == "core_hnsw_v2"
p = res["params"]
assert (p["M"], p["M0"], p["efConstruction"], p["efSearch"]) == (48, 96, 512, 1024), p
assert p["params_source"] == "core_hnsw_v2_frozen_defaults"
assert p["metric"] == "ip_normalized"
assert p["topk"] == topk
count = p["count"]
query_count = p["query_count"]
assert count == 1000 and query_count == 200

st = res["index_stats"]
assert st["size"] == count and st["active_count"] == count and st["deleted_count"] == 0
assert st["memory_bytes_source"] == "estimated" and st["memory_bytes"] > 0
# healthy build: no rejected edges
assert st["duplicate_edge_count"] == 0, st
assert st["degree_overflow_count"] == 0, st

perf = res["performance"]
for k in ("cache_load_us", "index_build_us", "build_avg_us_per_record",
          "search_total_us", "search_avg_us", "search_p50_us", "search_p95_us",
          "search_p99_us", "qps"):
    assert k in perf, f"missing perf {k}"
assert perf["qps"] > 0 and perf["search_avg_us"] > 0
assert perf["search_p50_us"] <= perf["search_p95_us"] <= perf["search_p99_us"]

# record ids present in records.jsonl for back-join
record_ids = set()
with open(f"{d}/records.jsonl") as f:
    for line in f:
        record_ids.add(json.loads(line)["id"])

# ground truth record_id lists per query_row
gt = {}
with open(f"{d}/groundtruth.jsonl") as f:
    for line in f:
        o = json.loads(line)
        gt[o["query_row"]] = [n["record_id"] for n in o["neighbors"]]

# recompute recall from per-query ann INDEPENDENTLY
n = 0
sum_r1 = sum_r10 = sum_top1 = sum_exact = 0.0
min_r10 = 1.0
with open(f"{d}/hnsw_v2_per_query.jsonl") as f:
    for line in f:
        o = json.loads(line)
        qr = o["query_row"]
        ann = o["ann"]
        hits = o["hits"]
        assert qr in gt, f"query_row {qr} not in ground truth"
        assert len(ann) == o["hit_count"]
        assert len(ann) == topk
        assert len(hits) == topk
        assert len(set(ann)) == len(ann), "duplicate record_id in ann"
        prev_score = None
        for idx, hit in enumerate(hits, start=1):
            assert hit["rank"] == idx, f"bad rank {hit}"
            assert hit["record_id"] == ann[idx - 1], f"hit/ann mismatch {hit} vs {ann[idx - 1]}"
            assert isinstance(hit["score"], (int, float)), f"non-numeric score {hit}"
            assert hit["score"] == hit["score"] and abs(hit["score"]) != float("inf"), f"bad score {hit}"
            if prev_score is not None:
                assert hit["score"] <= prev_score + 1e-7, f"scores not descending at query {qr}"
            prev_score = hit["score"]
        for rid in ann:
            assert rid in record_ids, f"ann record_id {rid} not back-joinable"
        g = gt[qr]
        inter = len(set(ann) & set(g[:topk]))
        r10 = inter / topk
        r1 = 1.0 if g[0] in ann else 0.0
        t1 = 1.0 if (ann and ann[0] == g[0]) else 0.0
        ex = 1.0 if ann == g[:topk] else 0.0
        # cross-check the runner's own per-query numbers
        assert abs(o["recall_at_10"] - r10) < 1e-9, (qr, o["recall_at_10"], r10)
        assert float(o["recall_at_1"]) == r1
        assert float(o["top1_match"]) == t1
        assert float(o["exact_match"]) == ex
        sum_r1 += r1; sum_r10 += r10; sum_top1 += t1; sum_exact += ex
        min_r10 = min(min_r10, r10)
        n += 1

assert n == query_count, f"{n} per-query rows, expected {query_count}"

q = res["quality"]
assert abs(q["recall_at_1"] - sum_r1 / n) < 1e-9, (q["recall_at_1"], sum_r1 / n)
assert abs(q["recall_at_10"] - sum_r10 / n) < 1e-9, (q["recall_at_10"], sum_r10 / n)
assert abs(q["top1_match_rate"] - sum_top1 / n) < 1e-9
assert abs(q["topk_exact_match_rate"] - sum_exact / n) < 1e-9
assert abs(q["min_per_query_recall_at_10"] - min_r10) < 1e-9
assert q["queries_checked"] == query_count
# sanity floor: on this 1K set with frozen defaults recall must be high
assert q["recall_at_10"] >= 0.90, f"recall@10 unexpectedly low: {q['recall_at_10']}"

print(f"[python] recall@1={q['recall_at_1']:.4f} recall@10={q['recall_at_10']:.4f} "
      f"top1={q['top1_match_rate']:.4f} exact={q['topk_exact_match_rate']:.4f} "
      f"min_recall@10={q['min_per_query_recall_at_10']:.4f} (independently recomputed, matches)")
PY
pass "result schema + index stats + INDEPENDENT recall recomputation all consistent"

# 4. B3 validate-only.
"$HV2_BIN" validate --dataset-id "$DATASET_ID" --out-root "$SMOKE_ROOT"
pass "B3 validate-only passed"

# 5. Determinism of QUALITY: per-query file (no timing) must be byte-identical.
PQ_SHA_BEFORE="$(sha256sum "$SMOKE_DIR/hnsw_v2_per_query.jsonl" | awk '{print $1}')"
Q_BEFORE="$(python3 -c "import json;print(json.dumps(json.load(open('$SMOKE_DIR/hnsw_v2_benchmark_result.json'))['quality'],sort_keys=True))")"
rm -f "$SMOKE_DIR/hnsw_v2_benchmark_result.json" "$SMOKE_DIR/hnsw_v2_per_query.jsonl"
"$HV2_BIN" generate --dataset-id "$DATASET_ID" --out-root "$SMOKE_ROOT" >/dev/null
PQ_SHA_AFTER="$(sha256sum "$SMOKE_DIR/hnsw_v2_per_query.jsonl" | awk '{print $1}')"
Q_AFTER="$(python3 -c "import json;print(json.dumps(json.load(open('$SMOKE_DIR/hnsw_v2_benchmark_result.json'))['quality'],sort_keys=True))")"
[[ "$PQ_SHA_BEFORE" == "$PQ_SHA_AFTER" ]] || fail "per-query file not deterministic: $PQ_SHA_BEFORE != $PQ_SHA_AFTER"
[[ "$Q_BEFORE" == "$Q_AFTER" ]] || fail "quality metrics not deterministic across regenerations"
pass "quality metrics + per-query file deterministic across regenerations"

# 6. Regenerating over existing result fails fast.
if "$HV2_BIN" generate --dataset-id "$DATASET_ID" --out-root "$SMOKE_ROOT" 2>"$SMOKE_ROOT/regen.err"; then
	fail "regenerating an existing result must fail, but it succeeded"
fi
grep -qi "already exists" "$SMOKE_ROOT/regen.err" || fail "regenerate failure did not mention 'already exists'"
pass "regenerating over an existing result fails fast"

# 7. Corrupted input cache must be rejected by the runner.
CORRUPT_ROOT="$SMOKE_ROOT/corrupt"
CORRUPT_DIR="$CORRUPT_ROOT/$DATASET_ID"
mkdir -p "$CORRUPT_ROOT"
cp -r "$SMOKE_DIR" "$CORRUPT_DIR"
rm -f "$CORRUPT_DIR/hnsw_v2_benchmark_result.json" "$CORRUPT_DIR/hnsw_v2_per_query.jsonl"
python3 -c "
p='$CORRUPT_DIR/embeddings.f32'
with open(p,'r+b') as f:
    f.seek(300); b=f.read(1); f.seek(300); f.write(bytes([b[0]^0xFF]))
"
if "$HV2_BIN" generate --dataset-id "$DATASET_ID" --out-root "$CORRUPT_ROOT" 2>"$SMOKE_ROOT/corrupt.err"; then
	fail "runner must reject a corrupted input cache, but it succeeded"
fi
grep -qi "sha256 mismatch" "$SMOKE_ROOT/corrupt.err" || fail "corrupt-input rejection did not report a sha256 mismatch"
pass "corrupted input cache is rejected (sha256 mismatch)"

# 8. Corrupted B2 ground truth must be rejected by the runner.
GT_CORRUPT_ROOT="$SMOKE_ROOT/gtcorrupt"
GT_CORRUPT_DIR="$GT_CORRUPT_ROOT/$DATASET_ID"
mkdir -p "$GT_CORRUPT_ROOT"
cp -r "$SMOKE_DIR" "$GT_CORRUPT_DIR"
rm -f "$GT_CORRUPT_DIR/hnsw_v2_benchmark_result.json" "$GT_CORRUPT_DIR/hnsw_v2_per_query.jsonl"
python3 -c "
p='$GT_CORRUPT_DIR/groundtruth.jsonl'
with open(p,'r+b') as f:
    f.seek(20); b=f.read(1); f.seek(20); f.write(bytes([b[0]^0xFF]))
"
if "$HV2_BIN" generate --dataset-id "$DATASET_ID" --out-root "$GT_CORRUPT_ROOT" 2>"$SMOKE_ROOT/gtcorrupt.err"; then
	fail "runner must reject a corrupted ground truth, but it succeeded"
fi
grep -qi "sha256 mismatch" "$SMOKE_ROOT/gtcorrupt.err" || fail "corrupt-groundtruth rejection did not report a sha256 mismatch"
pass "corrupted ground truth is rejected (sha256 mismatch)"

# 9. Corrupted B3 result must fail validate.
GTC_ROOT="$SMOKE_ROOT/rescorrupt"
GTC_DIR="$GTC_ROOT/$DATASET_ID"
mkdir -p "$GTC_ROOT"
cp -r "$SMOKE_DIR" "$GTC_DIR"
python3 -c "
p='$GTC_DIR/hnsw_v2_per_query.jsonl'
with open(p,'r+b') as f:
    f.seek(20); b=f.read(1); f.seek(20); f.write(bytes([b[0]^0xFF]))
"
if "$HV2_BIN" validate --dataset-id "$DATASET_ID" --out-root "$GTC_ROOT" >"$SMOKE_ROOT/resval.out" 2>&1; then
	fail "validate must fail on a corrupted result, but it passed"
fi
grep -qi "sha256 mismatch" "$SMOKE_ROOT/resval.out" || fail "corrupt-result validate did not report a sha256 mismatch"
pass "corrupted B3 result fails validate (sha256 mismatch)"

echo ""
echo "=== B3 core_hnsw_v2 index-only benchmark smoke test: ALL CHECKS PASSED ==="
