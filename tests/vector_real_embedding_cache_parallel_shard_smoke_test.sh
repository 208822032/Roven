#!/usr/bin/env bash

set -euo pipefail

ROOT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
cd "$ROOT_DIR"

GEN="build/tools/bin/generate_real_embedding_cache"
RUNNER="scripts/benchmark/run_real_embedding_cache_shards.py"
TMP="/tmp/kvlite_vector_benchmark_artifacts/embedding_shard_smoke_$$_$(date +%s)"

cleanup() {
	rm -rf "$TMP"
}
trap cleanup EXIT

fail() { printf '[FAIL] %s\n' "$1" >&2; exit 1; }
pass() { printf '[PASS] %s\n' "$1"; }

make gen-embedding-cache
mkdir -p "$TMP/fixture"

cat >"$TMP/fixture/records.tsv" <<'EOF'
pid	text
p0	Redis 使用内存保存常用数据。
p1	向量索引支持近似最近邻检索。
p2	HNSW 通过多层图加速搜索。
p3	快照可以缩短冷启动时间。
p4	AOF 记录写命令用于恢复。
p5	内积可比较归一化向量。
p6	候选堆限制搜索工作集合。
p7	确定性分片保持全局顺序。
p8	断点续跑需要先校验产物。
p9	原子发布避免暴露半成品。
EOF
head -n 5 "$TMP/fixture/records.tsv" >"$TMP/fixture/records.base.tsv"
cat >"$TMP/fixture/queries.tsv" <<'EOF'
qid	text
q0	什么是向量索引
q1	如何缩短冷启动
q2	归一化向量如何比较
EOF

"$GEN" generate \
	--dataset-id shard_smoke_base \
	--records-tsv "$TMP/fixture/records.base.tsv" \
	--queries-tsv "$TMP/fixture/queries.tsv" \
	--input-format id_text \
	--expected-record-count 4 \
	--expected-query-count 3 \
	--batch-size 2 \
	--num-threads 1 \
	--max-text-len 512 \
	--output-dir "$TMP/base"

"$GEN" generate \
	--dataset-id shard_smoke_reference \
	--records-tsv "$TMP/fixture/records.tsv" \
	--queries-tsv "$TMP/fixture/queries.tsv" \
	--input-format id_text \
	--expected-record-count 10 \
	--expected-query-count 3 \
	--batch-size 2 \
	--num-threads 1 \
	--max-text-len 512 \
	--output-dir "$TMP/reference"

python3 "$RUNNER" run \
	--dataset-id shard_smoke_merged \
	--records-tsv "$TMP/fixture/records.tsv" \
	--queries-tsv "$TMP/fixture/queries.tsv" \
	--base-cache-dir "$TMP/base" \
	--output-root "$TMP/run" \
	--start-row 4 \
	--end-row 10 \
	--workers 2 \
	--threads-per-worker 1 \
	--batch-size 2 \
	--max-text-len 512 \
	--reference-cache-dir "$TMP/reference"

python3 "$RUNNER" validate --output-root "$TMP/run"

for file in embeddings.f32 query_embeddings.f32 records.jsonl queries.jsonl; do
	cmp "$TMP/reference/$file" "$TMP/run/merged_cache/$file" || fail "$file differs from monolithic reference"
done
pass "parallel shard merge is byte-identical to monolithic output"

python3 - "$TMP/run/merged_cache/records.jsonl" <<'PY'
import json
import sys

with open(sys.argv[1], encoding="utf-8") as stream:
    rows = [json.loads(line) for line in stream]
assert [row["row"] for row in rows] == list(range(10))
assert [row["id"] for row in rows] == list(range(10))
assert [row["pid"] for row in rows] == [f"p{i}" for i in range(10)]
PY
pass "global numeric row/id and pid order are preserved"

rm -rf "$TMP/run/merged_cache"
rm -f "$TMP/run/run_result.json"
python3 "$RUNNER" run \
	--dataset-id shard_smoke_merged \
	--records-tsv "$TMP/fixture/records.tsv" \
	--queries-tsv "$TMP/fixture/queries.tsv" \
	--base-cache-dir "$TMP/base" \
	--output-root "$TMP/run" \
	--start-row 4 \
	--end-row 10 \
	--workers 2 \
	--threads-per-worker 1 \
	--batch-size 2 \
	--max-text-len 512 \
	--reference-cache-dir "$TMP/reference" \
	--resume
python3 - "$TMP/run/run_result.json" <<'PY'
import json
import sys
with open(sys.argv[1]) as stream:
    result = json.load(stream)
assert result["generated_record_count"] == 0
assert all(worker["status"] == "reused" for worker in result["worker_results"])
PY
pass "validated shards support deterministic resume without ONNX rerun"

SHARD="$(find "$TMP/run/shards" -mindepth 1 -maxdepth 1 -type d | sort | head -1)"
printf '\0' >>"$SHARD/embeddings.f32"
if python3 "$RUNNER" validate --output-root "$TMP/run" >"$TMP/corrupt.log" 2>&1; then
	fail "corrupt shard unexpectedly passed validation"
fi
grep -Eq 'size mismatch|sha256 mismatch' "$TMP/corrupt.log" || fail "corrupt shard failure lacked checksum/size evidence"
pass "corrupt shard is rejected"

printf '\n=== parallel embedding shard smoke: ALL CHECKS PASSED ===\n'
