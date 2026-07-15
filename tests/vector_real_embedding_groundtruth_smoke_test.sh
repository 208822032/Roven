#!/usr/bin/env bash
# B2-ground-truth-generation smoke test.
#
# Builds the index-only ground truth generator (links libkvstore.a, uses the
# core_flat exact IP path) and verifies, on a DISPOSABLE COPY of the B1 dataset:
#   - B1 validate-only passes on the input cache (required precondition)
#   - core_flat exact ground truth is produced for all queries
#   - groundtruth.jsonl schema: 200 lines, topk=10, contiguous ranks, monotonic
#     scores, unique in-range record_ids, back-joinable to records/queries.jsonl
#   - B2 validate-only passes without core_flat/ONNX
#   - >=10 queries independently brute-force cross-check (row/order/score) match
#   - regenerating over existing ground truth fails fast
#   - output is deterministic across regenerations (identical sha256)
#   - a corrupted input cache is rejected by the generator
#   - a corrupted ground truth copy fails validate
#   - artifacts live under /tmp
#
# Operates on a copy under a unique temp out-root (trap-cleaned), so it never
# touches the official dataset. The frozen ground truth B3 consumes is produced
# separately by an explicit `generate` on the real dataset (see the B2 report).

set -euo pipefail

ROOT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
cd "$ROOT_DIR"

GT_BIN="build/tools/bin/generate_real_embedding_groundtruth"
CACHE_BIN="build/tools/bin/generate_real_embedding_cache"
REAL_ROOT="/tmp/kvlite_vector_benchmark_artifacts/real_embedding"
DATASET_ID="qa_zh_1k_200_bge_small_zh_v1_5"
SRC_DIR="$REAL_ROOT/$DATASET_ID"
SMOKE_ROOT="/tmp/kvlite_vector_benchmark_artifacts/real_embedding_b2_smoke_$$_$(date +%s)"
SMOKE_DIR="$SMOKE_ROOT/$DATASET_ID"

cleanup() { rm -rf "${SMOKE_ROOT:?}"; }
trap cleanup EXIT

pass() { printf '[PASS] %s\n' "$1"; }
fail() { printf '[FAIL] %s\n' "$1" >&2; exit 1; }

printf '[INFO][build] make gen-embedding-groundtruth\n'
make gen-embedding-groundtruth

[[ -x "$GT_BIN" ]] || fail "ground truth generator not found at $GT_BIN"

# The B2 smoke needs the B1 cache but must not run ONNX implicitly. B1 fixture
# generation is a separate stage; this test only consumes an existing cache.
if [[ ! -d "$SRC_DIR" ]]; then
	fail "B1 source dataset unavailable: $SRC_DIR (run B1 explicitly; this smoke must not call ONNX)"
fi
[[ -d "$SRC_DIR" ]] || fail "B1 source dataset unavailable: $SRC_DIR"

case "$SMOKE_ROOT" in
	/tmp/*) ;;
	*) fail "SMOKE_ROOT is not under /tmp: $SMOKE_ROOT" ;;
esac

# Disposable copy under the SAME dataset id (so B1 validate's dataset_id check passes).
mkdir -p "$SMOKE_ROOT"
cp -r "$SRC_DIR" "$SMOKE_DIR"
# The copy must not inherit any pre-existing ground truth from the source.
rm -f "$SMOKE_DIR/groundtruth.jsonl" "$SMOKE_DIR/groundtruth.manifest.json"

# 1. Required precondition: B1 validate-only on the input cache.
[[ -x "$CACHE_BIN" ]] || make gen-embedding-cache
"$CACHE_BIN" validate --dataset-id "$DATASET_ID" --out-root "$SMOKE_ROOT"
pass "B1 validate-only passed on input cache"

# 2. Generate ground truth via core_flat exact IP.
"$GT_BIN" generate --dataset-id "$DATASET_ID" --out-root "$SMOKE_ROOT"
pass "ground truth generated via core_flat exact IP (no ONNX)"

[[ -f "$SMOKE_DIR/groundtruth.jsonl" ]] || fail "groundtruth.jsonl not published"
[[ -f "$SMOKE_DIR/groundtruth.manifest.json" ]] || fail "groundtruth.manifest.json not published"

# 3. Structural + back-join checks and an independent brute-force cross-check.
python3 - "$SMOKE_DIR" <<'PY'
import json
import math
import struct
import sys

d = sys.argv[1]
dim = 512
topk = 10

with open(f"{d}/manifest.json") as f:
    cache_manifest = json.load(f)
record_count = cache_manifest["params"]["record_count"]
query_count = cache_manifest["params"]["query_count"]
assert record_count == 1000 and query_count == 200 and cache_manifest["params"]["dim"] == dim

with open(f"{d}/groundtruth.manifest.json") as f:
    gm = json.load(f)
assert gm["schema_version"] == 1
assert gm["params"]["topk"] == topk
assert gm["params"]["metric"] == "ip_normalized"
assert gm["params"]["ground_truth_source"] == "core_flat_exact_ip"
assert gm["crosscheck"]["set_match_all"] is True
assert gm["crosscheck"]["topk_id_match_all"] is True
assert gm["crosscheck"]["strict_order_match_all"] is True
assert gm["crosscheck"]["nonfinite_score_count"] == 0
assert gm["crosscheck"]["max_score_abs_diff"] >= 0.0
assert gm["crosscheck"]["max_score_rel_diff"] >= 0.0
for key in ("max_abs_diff_location", "max_rel_diff_location"):
    assert gm["crosscheck"][key]["query_row"] >= 0
    assert gm["crosscheck"][key]["record_id"] >= 0
    assert gm["crosscheck"][key]["rank"] >= 1
for key in ("cache_load_us", "index_build_us", "search_total_us", "search_avg_us"):
    assert key in gm["timing_us"], f"missing timing {key}"

# Load records.jsonl / queries.jsonl id sets for back-join.
record_ids = set()
with open(f"{d}/records.jsonl") as f:
    for line in f:
        record_ids.add(json.loads(line)["id"])
query_rows = set()
with open(f"{d}/queries.jsonl") as f:
    for line in f:
        query_rows.add(json.loads(line)["query_row"])

# Load base + query vectors for independent brute-force.
with open(f"{d}/embeddings.f32", "rb") as f:
    base = f.read()
with open(f"{d}/query_embeddings.f32", "rb") as f:
    qraw = f.read()
assert len(base) == record_count * dim * 4
assert len(qraw) == query_count * dim * 4


def rec(i):
    return struct.unpack_from(f"<{dim}f", base, i * dim * 4)


def qry(i):
    return struct.unpack_from(f"<{dim}f", qraw, i * dim * 4)


gt = {}
with open(f"{d}/groundtruth.jsonl") as f:
    for lineno, line in enumerate(f):
        obj = json.loads(line)
        qr = obj["query_row"]
        assert obj["topk"] == topk
        assert obj["metric"] == "ip_normalized"
        assert qr in query_rows, f"query_row {qr} not in queries.jsonl"
        nb = obj["neighbors"]
        assert len(nb) == topk, f"line {lineno}: {len(nb)} neighbors"
        prev = None
        seen = set()
        for r, n in enumerate(nb):
            assert n["rank"] == r + 1, "ranks must be contiguous from 1"
            rid = n["record_id"]
            assert rid in record_ids, f"record_id {rid} not in records.jsonl"
            assert rid not in seen, "duplicate record_id in a query"
            seen.add(rid)
            sc = n["score"]
            assert math.isfinite(sc)
            if prev is not None:
                assert sc <= prev + 1e-9, "score not monotonic non-increasing"
            prev = sc
        gt[qr] = obj

assert len(gt) == query_count, f"{len(gt)} ground truth lines, expected {query_count}"

# Independent brute-force cross-check on >=10 evenly spaced queries.
sample = list(range(0, query_count, max(1, query_count // 20)))[:20]
assert len(sample) >= 10
for qr in sample:
    qv = qry(qr)
    scored = []
    for i in range(record_count):
        rv = rec(i)
        s = sum(a * b for a, b in zip(qv, rv))
        scored.append((s, i))
    # tie-break: score desc, record_id asc (matches core_flat membership rule)
    scored.sort(key=lambda t: (-t[0], t[1]))
    truth = scored[:topk]
    got = gt[qr]["neighbors"]
    got_ids = [n["record_id"] for n in got]
    truth_ids = [rid for _, rid in truth]
    assert set(got_ids) == set(truth_ids), f"q{qr}: set mismatch {got_ids} vs {truth_ids}"
    # strict order (no ties in real embeddings) + score tolerance
    for r in range(topk):
        assert got_ids[r] == truth_ids[r], f"q{qr} rank {r}: {got_ids[r]} vs {truth_ids[r]}"
        assert abs(got[r]["score"] - truth[r][0]) < 1e-4, f"q{qr} rank {r} score diff too large"

print(f"[python] structural + back-join OK; brute-force cross-check passed on {len(sample)} queries")
PY
pass "ground truth structural checks + back-join + brute-force cross-check passed"

# 4. B2 validate-only (no core_flat, no ONNX).
"$GT_BIN" validate --dataset-id "$DATASET_ID" --out-root "$SMOKE_ROOT"
pass "B2 validate-only passed"

# 5. Regenerating over existing ground truth must fail fast.
if "$GT_BIN" generate --dataset-id "$DATASET_ID" --out-root "$SMOKE_ROOT" 2>"$SMOKE_ROOT/regen.err"; then
	fail "regenerating existing ground truth must fail, but it succeeded"
fi
grep -qi "already exists" "$SMOKE_ROOT/regen.err" || fail "regenerate failure did not mention 'already exists'"
rm -f "$SMOKE_ROOT/regen.err"
pass "regenerating over existing ground truth fails fast"

# 6. Determinism: remove + regenerate, compare groundtruth.jsonl sha256.
SHA_BEFORE="$(sha256sum "$SMOKE_DIR/groundtruth.jsonl" | awk '{print $1}')"
rm -f "$SMOKE_DIR/groundtruth.jsonl" "$SMOKE_DIR/groundtruth.manifest.json"
"$GT_BIN" generate --dataset-id "$DATASET_ID" --out-root "$SMOKE_ROOT" >/dev/null
SHA_AFTER="$(sha256sum "$SMOKE_DIR/groundtruth.jsonl" | awk '{print $1}')"
[[ "$SHA_BEFORE" == "$SHA_AFTER" ]] || fail "ground truth not deterministic: $SHA_BEFORE != $SHA_AFTER"
pass "ground truth output is deterministic across regenerations"

# 7. Corrupted input cache must be rejected by the generator.
CORRUPT_ROOT="$SMOKE_ROOT/corrupt"
CORRUPT_DIR="$CORRUPT_ROOT/$DATASET_ID"
mkdir -p "$CORRUPT_ROOT"
cp -r "$SMOKE_DIR" "$CORRUPT_DIR"
rm -f "$CORRUPT_DIR/groundtruth.jsonl" "$CORRUPT_DIR/groundtruth.manifest.json"
python3 -c "
p='$CORRUPT_DIR/embeddings.f32'
with open(p,'r+b') as f:
    f.seek(200); b=f.read(1); f.seek(200); f.write(bytes([b[0]^0xFF]))
"
if "$GT_BIN" generate --dataset-id "$DATASET_ID" --out-root "$CORRUPT_ROOT" 2>"$SMOKE_ROOT/corrupt.err"; then
	fail "generator must reject a corrupted input cache, but it succeeded"
fi
grep -qi "sha256 mismatch" "$SMOKE_ROOT/corrupt.err" || fail "corrupt-input rejection did not report a sha256 mismatch"
rm -f "$SMOKE_ROOT/corrupt.err"
pass "corrupted input cache is rejected (sha256 mismatch)"

# 8. Corrupted ground truth copy must fail validate.
GTCORRUPT_ROOT="$SMOKE_ROOT/gtcorrupt"
GTCORRUPT_DIR="$GTCORRUPT_ROOT/$DATASET_ID"
mkdir -p "$GTCORRUPT_ROOT"
cp -r "$SMOKE_DIR" "$GTCORRUPT_DIR"
python3 -c "
p='$GTCORRUPT_DIR/groundtruth.jsonl'
with open(p,'r+b') as f:
    f.seek(50); b=f.read(1); f.seek(50); f.write(bytes([b[0]^0xFF]))
"
if "$GT_BIN" validate --dataset-id "$DATASET_ID" --out-root "$GTCORRUPT_ROOT" >"$SMOKE_ROOT/gtval.out" 2>&1; then
	fail "validate must fail on a corrupted ground truth, but it passed"
fi
grep -qi "sha256 mismatch" "$SMOKE_ROOT/gtval.out" || fail "corrupt ground truth validate did not report sha256 mismatch"
rm -f "$SMOKE_ROOT/gtval.out"
pass "corrupted ground truth fails validate (sha256 mismatch)"

# 9. External cache/output mode on a tiny synthetic fixture with exact score ties.
SYN_ROOT="$SMOKE_ROOT/synthetic"
SYN_CACHE="$SYN_ROOT/cache"
SYN_OUT="$SYN_ROOT/out"
mkdir -p "$SYN_ROOT"

make_synthetic_cache() {
	local dir="$1"
	local variant="${2:-ok}"
	python3 - "$dir" "$variant" <<'PY'
import hashlib
import json
import math
import os
import struct
import sys

out = sys.argv[1]
variant = sys.argv[2]
os.makedirs(out, exist_ok=True)

records = [
    (0, (1.0, 0.0)),
    (1, (1.0, 0.0)),
    (2, (0.0, 1.0)),
    (3, (-1.0, 0.0)),
]
queries = [
    (0, (1.0, 0.0)),
    (1, (0.0, 1.0)),
]
record_count = len(records)
query_count = len(queries)
dim = 2

if variant == "wrong_dim":
    dim_for_manifest = 3
else:
    dim_for_manifest = dim
if variant == "count_mismatch":
    record_count_for_manifest = record_count + 1
else:
    record_count_for_manifest = record_count

emb = bytearray()
for rid, vec in records:
    vals = list(vec)
    if variant == "nonfinite" and rid == 2:
        vals[0] = float("nan")
    emb.extend(struct.pack("<2f", *vals))
qemb = b"".join(struct.pack("<2f", *vec) for _, vec in queries)

with open(os.path.join(out, "embeddings.f32"), "wb") as f:
    f.write(emb)
with open(os.path.join(out, "query_embeddings.f32"), "wb") as f:
    f.write(qemb)

with open(os.path.join(out, "records.jsonl"), "w", encoding="utf-8") as f:
    for rid, _ in records:
        json_id = 1 if variant == "duplicate_id" and rid == 2 else rid
        f.write(json.dumps({"id": json_id, "external_id": f"p{rid}", "pid": f"p{rid}", "text": f"record {rid}"}, ensure_ascii=False) + "\n")
with open(os.path.join(out, "queries.jsonl"), "w", encoding="utf-8") as f:
    for qid, _ in queries:
        f.write(json.dumps({"query_row": qid, "query_id": f"q{qid}", "qid": f"q{qid}", "text": f"query {qid}"}, ensure_ascii=False) + "\n")

def sha(name):
    with open(os.path.join(out, name), "rb") as f:
        return hashlib.sha256(f.read()).hexdigest()

manifest = {
    "schema_version": 1,
    "dataset_id": "synthetic_tie_cache",
    "params": {
        "record_count": record_count_for_manifest,
        "query_count": query_count,
        "dim": dim_for_manifest,
        "dtype": "float32",
        "byte_order": "little",
        "metric": "ip_normalized",
        "normalized": True,
    },
    "outputs": {
        "embeddings_f32": {"path": "embeddings.f32", "sha256": sha("embeddings.f32")},
        "query_embeddings_f32": {"path": "query_embeddings.f32", "sha256": sha("query_embeddings.f32")},
        "records_jsonl": {"path": "records.jsonl", "sha256": sha("records.jsonl")},
        "queries_jsonl": {"path": "queries.jsonl", "sha256": sha("queries.jsonl")},
    },
}
with open(os.path.join(out, "manifest.json"), "w", encoding="utf-8") as f:
    json.dump(manifest, f, ensure_ascii=False, indent=2)
    f.write("\n")
PY
}

make_synthetic_cache "$SYN_CACHE" ok
"$GT_BIN" generate --cache-dir "$SYN_CACHE" --output-dir "$SYN_OUT" --top-k 3
"$GT_BIN" validate --output-dir "$SYN_OUT"
"$GT_BIN" validate --cache-dir "$SYN_CACHE" --output-dir "$SYN_OUT"

python3 - "$SYN_OUT" <<'PY'
import json
import sys

out = sys.argv[1]
with open(f"{out}/groundtruth.manifest.json") as f:
    m = json.load(f)
assert m["params"]["record_count"] == 4
assert m["params"]["query_count"] == 2
assert m["params"]["dim"] == 2
assert m["params"]["topk"] == 3
assert m["params"]["tie_break"] == "score_desc_record_id_asc"
assert m["crosscheck"]["set_match_all"] is True
assert m["crosscheck"]["record_id_match_all"] is True
assert m["crosscheck"]["strict_order_match_all"] is True
assert m["crosscheck"]["tie_query_count"] >= 1
assert m["crosscheck"]["tie_result_count"] >= 2

with open(f"{out}/groundtruth.jsonl") as f:
    rows = [json.loads(line) for line in f]
assert [n["record_id"] for n in rows[0]["neighbors"]] == [0, 1, 2]
assert [n["record_id"] for n in rows[1]["neighbors"]] == [2, 0, 1]
PY
pass "external cache/output mode preserves deterministic score-desc record_id-asc tie-break"

SYN_SHA_BEFORE="$(sha256sum "$SYN_OUT/groundtruth.jsonl" | awk '{print $1}')"
"$GT_BIN" generate --cache-dir "$SYN_CACHE" --output-dir "$SYN_OUT" --top-k 3 >"$SYN_ROOT/repeat.out"
grep -q "status=unchanged core_outputs_identical=true" "$SYN_ROOT/repeat.out" || fail "external repeat did not return unchanged"
SYN_SHA_AFTER="$(sha256sum "$SYN_OUT/groundtruth.jsonl" | awk '{print $1}')"
[[ "$SYN_SHA_BEFORE" == "$SYN_SHA_AFTER" ]] || fail "external repeat changed groundtruth checksum"
pass "external cache/output repeated generation is deterministic and unchanged"

for variant in duplicate_id wrong_dim count_mismatch nonfinite; do
	BAD_CACHE="$SYN_ROOT/bad_$variant/cache"
	BAD_OUT="$SYN_ROOT/bad_$variant/out"
	make_synthetic_cache "$BAD_CACHE" "$variant"
	if "$GT_BIN" generate --cache-dir "$BAD_CACHE" --output-dir "$BAD_OUT" --top-k 3 >"$SYN_ROOT/bad_$variant.out" 2>&1; then
		fail "synthetic bad fixture '$variant' unexpectedly generated ground truth"
	fi
	[[ ! -f "$BAD_OUT/groundtruth.jsonl" ]] || fail "bad fixture '$variant' left groundtruth.jsonl behind"
	[[ ! -f "$BAD_OUT/groundtruth.manifest.json" ]] || fail "bad fixture '$variant' left groundtruth.manifest.json behind"
done
pass "external mode rejects duplicate id, wrong dim, count mismatch, and non-finite vectors without half-published output"

SMALL_OUT="$SYN_ROOT/too_large_topk/out"
if "$GT_BIN" generate --cache-dir "$SYN_CACHE" --output-dir "$SMALL_OUT" --top-k 5 >"$SYN_ROOT/too_large_topk.out" 2>&1; then
	fail "top-k larger than record_count must fail"
fi
[[ ! -f "$SMALL_OUT/groundtruth.jsonl" ]] || fail "top-k failure left groundtruth.jsonl behind"
pass "external mode rejects insufficient records for requested topK"

echo ""
echo "=== B2 real embedding ground truth smoke test: ALL CHECKS PASSED ==="
