#!/usr/bin/env bash
set -euo pipefail

ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/../.." && pwd)"
cd "$ROOT"

DATASET_ID="${DATASET_ID:-smoke_N1000_nq100_d64_seed42_ip_norm_top10_scope-empty}"
MANIFEST="${MANIFEST:-results/vector_native_faiss/datasets/${DATASET_ID}/manifest.json}"
OUT_DIR="${OUT_DIR:-results/vector_native_faiss/index_only_smoke}"
RAW_CSV="$OUT_DIR/raw.csv"
SUMMARY_MD="$OUT_DIR/summary.md"

mkdir -p "$OUT_DIR"

if [ ! -f "$MANIFEST" ]; then
  echo "manifest not found: $MANIFEST" >&2
  exit 1
fi

read_manifest_field() {
  python3 - "$MANIFEST" "$1" <<'PY'
import json
import sys
with open(sys.argv[1], "r", encoding="utf-8") as f:
    data = json.load(f)
value = data.get(sys.argv[2], "")
if isinstance(value, bool):
    print("true" if value else "false")
else:
    print(value)
PY
}

N="$(read_manifest_field N)"
NQ="$(read_manifest_field nq)"
DIM="$(read_manifest_field dim)"
METRIC="$(read_manifest_field metric)"
NORMALIZE="$(read_manifest_field normalize)"
TOPK="$(read_manifest_field topK)"
DATASET_PATH="$(read_manifest_field dataset_path)"
QUERIES_PATH="$(read_manifest_field queries_path)"

HNSW_M="${HNSW_M:-32}"
HNSW_EF_CONSTRUCTION="${HNSW_EF_CONSTRUCTION:-256}"
HNSW_EF_SEARCH="${HNSW_EF_SEARCH:-128}"
RUN_ID="index_only_smoke_$(date -u +%Y%m%dT%H%M%SZ)"

hnswlib_status="unavailable"
hnswlib_reason="third_party/hnswlib is empty or missing hnswlib/hnswlib.h"
if [ -f "third_party/hnswlib/hnswlib/hnswlib.h" ]; then
  hnswlib_status="available"
  hnswlib_reason="third_party/hnswlib/hnswlib/hnswlib.h exists"
fi

faiss_status="unavailable"
faiss_reason="third_party/faiss is empty or missing faiss headers/build/faiss/libfaiss.a"
if [ -f "third_party/faiss/faiss/IndexFlat.h" ] && [ -f "third_party/faiss/build/faiss/libfaiss.a" ]; then
  faiss_status="available"
  faiss_reason="faiss headers and static library exist"
elif [ -f "third_party/faiss/faiss/IndexFlat.h" ]; then
  faiss_reason="faiss headers exist but third_party/faiss/build/faiss/libfaiss.a is missing"
fi

manifest_tool_status="unavailable"
manifest_tool_reason="no KVLite index-only binary currently consumes dataset.bin/queries.bin/manifest.json directly"
if [ -x "build/tests/bin/vector_index_only_benchmark" ]; then
  manifest_tool_status="available"
  manifest_tool_reason="build/tests/bin/vector_index_only_benchmark exists"
fi

{
  echo "run_id,dataset_id,layer,backend,phase,N,nq,dim,metric,normalize,topK,M,efConstruction,efSearch,count,total_us,avg_us,p50_us,p95_us,p99_us,max_us,qps,recall_at_1,recall_at_10,rss_kb,index_memory_bytes,timeout,error_count,status,notes"
  echo "$RUN_ID,$DATASET_ID,index_only,native_flat,add_search,$N,$NQ,$DIM,$METRIC,$NORMALIZE,$TOPK,$HNSW_M,$HNSW_EF_CONSTRUCTION,$HNSW_EF_SEARCH,0,,,,,,,,,,,,false,0,unavailable,\"$manifest_tool_reason\""
  if [ "$hnswlib_status" = "available" ] && [ "$manifest_tool_status" = "available" ]; then
    echo "$RUN_ID,$DATASET_ID,index_only,hnswlib_hnsw,add_search,$N,$NQ,$DIM,$METRIC,$NORMALIZE,$TOPK,$HNSW_M,$HNSW_EF_CONSTRUCTION,$HNSW_EF_SEARCH,0,,,,,,,,,,,,false,0,not_run,\"runner hook not implemented in this smoke script\""
  else
    echo "$RUN_ID,$DATASET_ID,index_only,hnswlib_hnsw,add_search,$N,$NQ,$DIM,$METRIC,$NORMALIZE,$TOPK,$HNSW_M,$HNSW_EF_CONSTRUCTION,$HNSW_EF_SEARCH,0,,,,,,,,,,,,false,0,unavailable,\"$hnswlib_reason; $manifest_tool_reason\""
  fi
  if [ "$faiss_status" = "available" ] && [ "$manifest_tool_status" = "available" ]; then
    echo "$RUN_ID,$DATASET_ID,index_only,faiss_flat,add_search,$N,$NQ,$DIM,$METRIC,$NORMALIZE,$TOPK,$HNSW_M,$HNSW_EF_CONSTRUCTION,$HNSW_EF_SEARCH,0,,,,,,,,,,,,false,0,not_run,\"runner hook not implemented in this smoke script\""
    echo "$RUN_ID,$DATASET_ID,index_only,faiss_hnsw,add_search,$N,$NQ,$DIM,$METRIC,$NORMALIZE,$TOPK,$HNSW_M,$HNSW_EF_CONSTRUCTION,$HNSW_EF_SEARCH,0,,,,,,,,,,,,false,0,not_run,\"runner hook not implemented in this smoke script\""
  else
    echo "$RUN_ID,$DATASET_ID,index_only,faiss_flat,add_search,$N,$NQ,$DIM,$METRIC,$NORMALIZE,$TOPK,$HNSW_M,$HNSW_EF_CONSTRUCTION,$HNSW_EF_SEARCH,0,,,,,,,,,,,,false,0,unavailable,\"$faiss_reason; $manifest_tool_reason\""
    echo "$RUN_ID,$DATASET_ID,index_only,faiss_hnsw,add_search,$N,$NQ,$DIM,$METRIC,$NORMALIZE,$TOPK,$HNSW_M,$HNSW_EF_CONSTRUCTION,$HNSW_EF_SEARCH,0,,,,,,,,,,,,false,0,unavailable,\"$faiss_reason; $manifest_tool_reason\""
  fi
} > "$RAW_CSV"

cat > "$SUMMARY_MD" <<EOF
# KVLite Native vs Faiss Index-only Smoke

This is a 1K/64d readiness smoke only. It does not run 10K/100K and does not provide formal performance evidence.

## Dataset

| Field | Value |
|---|---|
| dataset_id | \`$DATASET_ID\` |
| manifest | \`$MANIFEST\` |
| dataset_path | \`$DATASET_PATH\` |
| queries_path | \`$QUERIES_PATH\` |
| N | $N |
| nq | $NQ |
| dim | $DIM |
| metric | $METRIC |
| normalize | $NORMALIZE |
| topK | $TOPK |

## Dependency Readiness

| Dependency | Status | Reason |
|---|---|---|
| hnswlib | $hnswlib_status | $hnswlib_reason |
| Faiss | $faiss_status | $faiss_reason |
| manifest-compatible index-only runner | $manifest_tool_status | $manifest_tool_reason |

## Backend Results

| Backend | Status | Reason |
|---|---|---|
| native_flat | unavailable | $manifest_tool_reason |
| hnswlib_hnsw | unavailable | $hnswlib_reason; $manifest_tool_reason |
| faiss_flat | unavailable | $faiss_reason; $manifest_tool_reason |
| faiss_hnsw | unavailable | $faiss_reason; $manifest_tool_reason |

## Output

| File | Path |
|---|---|
| raw.csv | \`$RAW_CSV\` |
| summary.md | \`$SUMMARY_MD\` |

## Notes

Existing tools such as \`tests/vector_search_bench.c\` generate their own mock vectors and do not consume the fixed \`dataset.bin\` and \`queries.bin\` files. This smoke therefore records index-only backend execution as unavailable instead of mixing non-equivalent data.
EOF

echo "index-only smoke readiness written:"
echo "  $RAW_CSV"
echo "  $SUMMARY_MD"
