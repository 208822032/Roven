#!/usr/bin/env bash
# B1-fixture-cache-generation smoke test.
#
# Builds scripts/benchmark/generate_real_embedding_cache.c against the real
# ONNX embedding runtime, generates a throwaway real-embedding cache from the
# repo's qa_zh_1000.tsv / qa_zh_queries.tsv fixtures, and verifies:
#   - real ONNX inference produced the cache (no hash fallback)
#   - record_count=1000, query_count=200, dim=512
#   - f32 artifact sizes and manifest checksums are correct
#   - validate-only passes without calling ONNX
#   - >=10 sampled record/query vectors are finite and L2-normalized
#   - regenerating the same dataset id fails fast (no silent overwrite)
#   - validate-only fails on a corrupted copy
#   - artifacts live under /tmp
#
# Uses a unique, disposable dataset id per run (trap-cleaned on exit) so it
# never touches a "production" dataset id. The frozen cache B2+ will read is
# produced separately via an explicit `generate` invocation (see
# docs/vector_benchmark_readiness/b1_fixture_cache_generation_report.md).

set -euo pipefail

ROOT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
cd "$ROOT_DIR"

GEN_BIN="build/tools/bin/generate_real_embedding_cache"
OUT_ROOT="/tmp/kvlite_vector_benchmark_artifacts/real_embedding"
DATASET_ID="b1_smoke_$$_$(date +%s)"
CORRUPT_ID="${DATASET_ID}_corrupt"
EXT_ROOT="/tmp/kvlite_vector_benchmark_artifacts/real_embedding_external_tsv_smoke_$$_$(date +%s)"
EXT_ID="external_tsv_smoke"
EXT_DIR="$EXT_ROOT/$EXT_ID"

cleanup() {
	rm -rf "${OUT_ROOT:?}/${DATASET_ID}" "${OUT_ROOT:?}/${CORRUPT_ID}"
	rm -rf "${EXT_ROOT:?}"
}
trap cleanup EXIT

pass() { printf '[PASS] %s\n' "$1"; }
fail() { printf '[FAIL] %s\n' "$1" >&2; exit 1; }

printf '[INFO][build] make gen-embedding-cache\n'
make gen-embedding-cache

[[ -x "$GEN_BIN" ]] || fail "generator binary not found at $GEN_BIN"

case "$OUT_ROOT" in
	/tmp/*) ;;
	*) fail "OUT_ROOT is not under /tmp: $OUT_ROOT" ;;
esac

printf '[INFO][generate] dataset_id=%s\n' "$DATASET_ID"
"$GEN_BIN" generate --dataset-id "$DATASET_ID" --out-root "$OUT_ROOT"
pass "generate exited 0 with real ONNX inference"

DATASET_DIR="$OUT_ROOT/$DATASET_ID"
[[ -d "$DATASET_DIR" ]] || fail "dataset directory not published: $DATASET_DIR"

python3 - "$DATASET_DIR" "$DATASET_ID" <<'PY'
import json
import re
import sys

dataset_dir, dataset_id = sys.argv[1], sys.argv[2]

with open(f"{dataset_dir}/manifest.json") as f:
    manifest = json.load(f)

assert manifest["schema_version"] == 1, "schema_version must be 1"
assert manifest["dataset_id"] == dataset_id, "dataset_id mismatch"

params = manifest["params"]
assert params["record_count"] == 1000, f'record_count={params["record_count"]}'
assert params["query_count"] == 200, f'query_count={params["query_count"]}'
assert params["dim"] == 512, f'dim={params["dim"]}'
assert params["dtype"] == "float32"
assert params["byte_order"] == "little"
assert params["normalized"] is True
assert params["metric"] == "ip_normalized"

sanity = manifest["sanity"]
assert sanity["embedding_runtime_confirmed"] == "onnxruntime", "must confirm real onnxruntime, not hash fallback"
assert sanity["deterministic_check_passed"] is True
assert sanity["distinct_check_passed"] is True

hex64 = re.compile(r"^[0-9a-f]{64}$")
outputs = manifest["outputs"]
for key in ("embeddings_f32", "query_embeddings_f32", "records_jsonl", "queries_jsonl"):
    sha = outputs[key]["sha256"]
    assert hex64.match(sha), f"{key} sha256 not 64 hex chars: {sha}"

for key in ("model_onnx_sha256", "model_onnx_data_sha256", "vocab_txt_sha256"):
    sha = manifest["model"]["checksums"][key]
    assert hex64.match(sha), f"model.{key} not 64 hex chars: {sha}"

emb_shape = outputs["embeddings_f32"]["shape"]
qemb_shape = outputs["query_embeddings_f32"]["shape"]
assert emb_shape == [1000, 512], emb_shape
assert qemb_shape == [200, 512], qemb_shape

import os
emb_size = os.path.getsize(f"{dataset_dir}/embeddings.f32")
qemb_size = os.path.getsize(f"{dataset_dir}/query_embeddings.f32")
assert emb_size == 1000 * 512 * 4, emb_size
assert qemb_size == 200 * 512 * 4, qemb_size
assert emb_size == outputs["embeddings_f32"]["size_bytes"]
assert qemb_size == outputs["query_embeddings_f32"]["size_bytes"]

timing = manifest["timing_us"]
for key in (
    "model_load_us", "record_embedding_total_us", "record_embedding_avg_us",
    "query_embedding_total_us", "query_embedding_avg_us", "artifact_write_us",
    "validation_us", "total_us",
):
    assert key in timing, f"missing timing field {key}"

print("[python] manifest schema/shape/checksum-format checks passed")
PY
pass "manifest fields: record_count=1000 query_count=200 dim=512, checksums well-formed"

"$GEN_BIN" validate --dataset-id "$DATASET_ID" --out-root "$OUT_ROOT"
pass "validate-only passed on freshly generated dataset"

python3 - "$DATASET_DIR" <<'PY'
import math
import struct
import sys

dataset_dir = sys.argv[1]
dim = 512


def check(path, count, sample_n):
    with open(path, "rb") as f:
        data = f.read()
    assert len(data) == count * dim * 4
    step = max(1, count // sample_n)
    checked = 0
    for row in range(0, count, step):
        offset = row * dim * 4
        vec = struct.unpack_from(f"<{dim}f", data, offset)
        assert all(math.isfinite(v) for v in vec), f"{path} row {row} has non-finite value"
        norm = math.sqrt(sum(v * v for v in vec))
        assert abs(norm - 1.0) < 1e-3, f"{path} row {row} norm={norm}"
        checked += 1
        if checked >= sample_n:
            break
    assert checked >= sample_n, f"{path} only sampled {checked} rows, need >= {sample_n}"


check(f"{dataset_dir}/embeddings.f32", 1000, 10)
check(f"{dataset_dir}/query_embeddings.f32", 200, 10)
print("[python] sampled >=10 record and >=10 query vectors: finite, norm~=1")
PY
pass "sampled >=10 record/query vectors are finite and L2-normalized"

BEFORE_MANIFEST_SHA="$(sha256sum "$DATASET_DIR/manifest.json" | awk '{print $1}')"
if "$GEN_BIN" generate --dataset-id "$DATASET_ID" --out-root "$OUT_ROOT" 2>/tmp/b1_smoke_regen.err; then
	fail "regenerating an existing dataset id must fail, but it succeeded"
fi
grep -qi "already exists" /tmp/b1_smoke_regen.err || fail "regenerate failure message did not mention 'already exists'"
AFTER_MANIFEST_SHA="$(sha256sum "$DATASET_DIR/manifest.json" | awk '{print $1}')"
[[ "$BEFORE_MANIFEST_SHA" == "$AFTER_MANIFEST_SHA" ]] || fail "manifest.json changed after a rejected regenerate (silent overwrite)"
rm -f /tmp/b1_smoke_regen.err
pass "regenerating the same dataset id fails fast without touching the existing cache"

CORRUPT_DIR="$OUT_ROOT/$CORRUPT_ID"
rm -rf "$CORRUPT_DIR"
cp -r "$DATASET_DIR" "$CORRUPT_DIR"
python3 - "$CORRUPT_DIR" "$CORRUPT_ID" <<'PY'
import json
import sys

corrupt_dir, corrupt_id = sys.argv[1], sys.argv[2]
manifest_path = f"{corrupt_dir}/manifest.json"
with open(manifest_path) as f:
    manifest = json.load(f)
manifest["dataset_id"] = corrupt_id
with open(manifest_path, "w") as f:
    json.dump(manifest, f)

f32_path = f"{corrupt_dir}/embeddings.f32"
with open(f32_path, "r+b") as f:
    f.seek(100)
    b = f.read(1)
    f.seek(100)
    f.write(bytes([b[0] ^ 0xFF]))
print(f"[python] corrupted 1 byte in {f32_path}, patched dataset_id in copied manifest")
PY

if "$GEN_BIN" validate --dataset-id "$CORRUPT_ID" --out-root "$OUT_ROOT" >/tmp/b1_smoke_validate_corrupt.out 2>&1; then
	fail "validate-only must fail on a corrupted cache copy, but it passed"
fi
grep -qi "sha256 mismatch" /tmp/b1_smoke_validate_corrupt.out || fail "corrupt-copy validate failure did not report a sha256 mismatch"
rm -f /tmp/b1_smoke_validate_corrupt.out
pass "validate-only fails on a corrupted cache copy (sha256 mismatch detected)"

mkdir -p "$EXT_ROOT/fixture"
cat >"$EXT_ROOT/fixture/records.tsv" <<'EOF'
pid	text
pA	外部文档 A：Redis AOF 会追加写入命令。
pB	外部文档 B：HNSW 使用图结构做近似最近邻搜索。
pC	外部文档 C：向量归一化后可以用内积表示 cosine。
pD	外部文档 D：snapshot 可以减少 cold start rebuild 成本。
EOF
cat >"$EXT_ROOT/fixture/queries.tsv" <<'EOF'
qid	text
qA	Redis AOF 是什么
qB	HNSW 检索结构
qC	向量为什么要归一化
EOF

"$GEN_BIN" generate \
	--dataset-id "$EXT_ID" \
	--records-tsv "$EXT_ROOT/fixture/records.tsv" \
	--queries-tsv "$EXT_ROOT/fixture/queries.tsv" \
	--input-format id_text \
	--expected-record-count 4 \
	--expected-query-count 3 \
	--output-dir "$EXT_DIR"
pass "external TSV id_text generate passed"

"$GEN_BIN" validate --dataset-id "$EXT_ID" --output-dir "$EXT_DIR"
pass "external TSV output-dir validate passed"

python3 - "$EXT_DIR" <<'PY'
import json
import math
import os
import struct
import sys

d = sys.argv[1]
dim = 512

with open(f"{d}/manifest.json", encoding="utf-8") as f:
    manifest = json.load(f)

assert manifest["params"]["record_count"] == 4
assert manifest["params"]["query_count"] == 3
assert manifest["params"]["input_format"] == "id_text"
assert manifest["params"]["dim"] == dim
assert manifest["params"]["expected_record_count"] == 4
assert manifest["params"]["expected_query_count"] == 3
assert manifest["validation"]["id_order_alignment"] is True
assert manifest["tokenizer"]["max_text_len"] == 512
assert manifest["sanity"]["embedding_runtime_confirmed"] == "onnxruntime"

assert os.path.getsize(f"{d}/embeddings.f32") == 4 * dim * 4
assert os.path.getsize(f"{d}/query_embeddings.f32") == 3 * dim * 4

with open(f"{d}/records.jsonl", encoding="utf-8") as f:
    records = [json.loads(line) for line in f]
with open(f"{d}/queries.jsonl", encoding="utf-8") as f:
    queries = [json.loads(line) for line in f]

assert [r["pid"] for r in records] == ["pA", "pB", "pC", "pD"]
assert [r["external_id"] for r in records] == ["pA", "pB", "pC", "pD"]
assert [r["row"] for r in records] == [0, 1, 2, 3]
assert [r["id"] for r in records] == [0, 1, 2, 3]
assert [q["qid"] for q in queries] == ["qA", "qB", "qC"]
assert [q["external_id"] for q in queries] == ["qA", "qB", "qC"]
assert [q["query_row"] for q in queries] == [0, 1, 2]

def check(path, count):
    with open(path, "rb") as f:
        data = f.read()
    assert len(data) == count * dim * 4
    for row in range(count):
        vec = struct.unpack_from(f"<{dim}f", data, row * dim * 4)
        assert all(math.isfinite(v) for v in vec)
        norm = math.sqrt(sum(v * v for v in vec))
        assert abs(norm - 1.0) <= 1e-3, (path, row, norm)

check(f"{d}/embeddings.f32", 4)
check(f"{d}/query_embeddings.f32", 3)
print("[python] external TSV id/order, f32 size, finite, normalized checks passed")
PY
pass "external TSV id/order alignment and vector checks passed"

EXT_CORE_BEFORE="$(sha256sum "$EXT_DIR/embeddings.f32" "$EXT_DIR/query_embeddings.f32" "$EXT_DIR/records.jsonl" "$EXT_DIR/queries.jsonl")"
"$GEN_BIN" generate \
	--dataset-id "$EXT_ID" \
	--records-tsv "$EXT_ROOT/fixture/records.tsv" \
	--queries-tsv "$EXT_ROOT/fixture/queries.tsv" \
	--input-format id_text \
	--expected-record-count 4 \
	--expected-query-count 3 \
	--output-dir "$EXT_DIR" >"$EXT_ROOT/repeat.out"
grep -q "status=unchanged" "$EXT_ROOT/repeat.out" || fail "external TSV repeat did not report unchanged"
EXT_CORE_AFTER="$(sha256sum "$EXT_DIR/embeddings.f32" "$EXT_DIR/query_embeddings.f32" "$EXT_DIR/records.jsonl" "$EXT_DIR/queries.jsonl")"
[[ "$EXT_CORE_BEFORE" == "$EXT_CORE_AFTER" ]] || fail "external TSV core outputs changed after repeated generate"
pass "external TSV repeated generate keeps core outputs byte-identical"

BAD_HEADER="$EXT_ROOT/bad_header.tsv"
cp "$EXT_ROOT/fixture/records.tsv" "$BAD_HEADER"
sed -i '1s/.*/bad\theader/' "$BAD_HEADER"
if "$GEN_BIN" generate \
	--dataset-id bad_header \
	--records-tsv "$BAD_HEADER" \
	--queries-tsv "$EXT_ROOT/fixture/queries.tsv" \
	--input-format id_text \
	--expected-record-count 4 \
	--expected-query-count 3 \
	--output-dir "$EXT_ROOT/bad_header_out" >/dev/null 2>&1; then
	fail "external TSV bad header must fail"
fi
pass "external TSV bad header fails"

if "$GEN_BIN" generate \
	--dataset-id bad_count \
	--records-tsv "$EXT_ROOT/fixture/records.tsv" \
	--queries-tsv "$EXT_ROOT/fixture/queries.tsv" \
	--input-format id_text \
	--expected-record-count 5 \
	--expected-query-count 3 \
	--output-dir "$EXT_ROOT/bad_count_out" >/dev/null 2>&1; then
	fail "external TSV expected-record-count mismatch must fail"
fi
pass "external TSV expected-count mismatch fails"

echo ""
echo "=== B1 real embedding cache smoke test: ALL CHECKS PASSED ==="
