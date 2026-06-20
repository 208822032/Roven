#!/bin/bash
#
# bench_vcmd_profile.sh — Comprehensive V-family profiling experiment runner.
#
# Runs the experiment matrix:
#   - VSET_VEC flat / hnsw
#   - VSET_TEXT hnsw
#   - VSEARCH_VEC flat / hnsw
#   - VSEARCH_TEXT hnsw
#   - Faiss IndexFlatIP / IndexHNSWFlat (if available)
#
# Configurable parameters via environment variables:
#   NB=10000         Number of base vectors
#   DIM=512          Vector dimension
#   TOPK=10          Number of results
#   ITER=1000        Number of operations to benchmark
#   KVLITE_BIN=./build/tests/bin/vcmd_profile_bench
#   FAISS_BIN=./build/tests/bin/faiss_baseline
#   OUTPUT_DIR=results/vcmd_profile
#
# Usage:
#   NB=10000 DIM=512 TOPK=10 ITER=1000 bash scripts/bench_vcmd_profile.sh
#

set -euo pipefail

# ── Configuration ───────────────────────────────────────────────────────
NB="${NB:-10000}"
DIM="${DIM:-512}"
TOPK="${TOPK:-10}"
ITER="${ITER:-1000}"
KVLITE_BIN="${KVLITE_BIN:-./build/tests/bin/vcmd_profile_bench}"
FAISS_BIN="${FAISS_BIN:-./build/tests/bin/faiss_baseline}"
OUTPUT_DIR="${OUTPUT_DIR:-results/vcmd_profile}"
TIMESTAMP=$(date +%Y%m%d_%H%M%S)

mkdir -p "$OUTPUT_DIR"

SUMMARY="$OUTPUT_DIR/summary_${TIMESTAMP}.md"
RAW_CSV="$OUTPUT_DIR/raw_${TIMESTAMP}.csv"

echo "# KVLite V-Cmd Profile Experiment" | tee "$SUMMARY"
echo "## Configuration" | tee -a "$SUMMARY"
echo "" | tee -a "$SUMMARY"
echo "- NB=$NB" | tee -a "$SUMMARY"
echo "- DIM=$DIM" | tee -a "$SUMMARY"
echo "- TOPK=$TOPK" | tee -a "$SUMMARY"
echo "- ITER=$ITER" | tee -a "$SUMMARY"
echo "- Timestamp: $TIMESTAMP" | tee -a "$SUMMARY"
echo "" | tee -a "$SUMMARY"

# ── CSV header ──────────────────────────────────────────────────────────
echo "experiment,index_type,mode,nb,dim,topk,iterations,total_us,avg_us,min_us,max_us,p50_us,p95_us,p99_us,qps" > "$RAW_CSV"

# ── Helper ──────────────────────────────────────────────────────────────
run_kvlite_bench() {
    local mode="$1"
    local index_type="$2"
    local label="$3"

    echo "" | tee -a "$SUMMARY"
    echo "### $label (index=$index_type)" | tee -a "$SUMMARY"
    echo '```' | tee -a "$SUMMARY"

    # Set index type via config (need to rebuild for different index types)
    # For now, run with current config
    if "$KVLITE_BIN" "$mode" "$NB" "$DIM" "$TOPK" "$ITER" 2>&1 | tee "$OUTPUT_DIR/${label// /_}.log"; then
        echo "  PASS" | tee -a "$SUMMARY"
    else
        echo "  FAIL (exit code: $?)" | tee -a "$SUMMARY"
    fi
    echo '```' | tee -a "$SUMMARY"
}

run_faiss_bench() {
    local index_type="$1"
    local label="$2"
    local extra_args="${3:-}"

    echo "" | tee -a "$SUMMARY"
    echo "### $label" | tee -a "$SUMMARY"
    echo '```' | tee -a "$SUMMARY"

    if [ -x "$FAISS_BIN" ]; then
        "$FAISS_BIN" "$index_type" "$NB" "$DIM" "$ITER" "$TOPK" $extra_args 2>&1 | tee "$OUTPUT_DIR/${label// /_}.log"
        echo "  PASS" | tee -a "$SUMMARY"
    else
        echo "  SKIP: Faiss binary not found at $FAISS_BIN" | tee -a "$SUMMARY"
    fi
    echo '```' | tee -a "$SUMMARY"
}

# ── Build ───────────────────────────────────────────────────────────────
echo "## Build" | tee -a "$SUMMARY"
echo '```' | tee -a "$SUMMARY"
echo "Building vcmd_profile_bench..."
make vcmd_profile_bench 2>&1 | tail -5 | tee -a "$SUMMARY"
echo '```' | tee -a "$SUMMARY"

# ── Experiment Matrix ───────────────────────────────────────────────────
echo "" | tee -a "$SUMMARY"
echo "## Experiment Results" | tee -a "$SUMMARY"

# Check if KVLite binary exists
if [ ! -x "$KVLITE_BIN" ]; then
    echo "ERROR: $KVLITE_BIN not found or not executable" | tee -a "$SUMMARY"
    echo "Build with: make vcmd_profile_bench" | tee -a "$SUMMARY"
    exit 1
fi

# KVLite flat index tests
echo "### KVLite Flat Index Tests" | tee -a "$SUMMARY"

# ANN-core (flat)
run_kvlite_bench "ann_core" "flat" "KVLite ANN-core (flat)"

# VSET_VEC flat
run_kvlite_bench "vset_vec" "flat" "VSET_VEC (flat)"

# VSEARCH_VEC flat
run_kvlite_bench "vsearch_vec" "flat" "VSEARCH_VEC (flat)"

# KVLite HNSW index tests
echo "### KVLite HNSW Index Tests" | tee -a "$SUMMARY"

# ANN-core (hnsw)
run_kvlite_bench "ann_core" "hnsw" "KVLite ANN-core (HNSW)"

# VSET_VEC hnsw
run_kvlite_bench "vset_vec" "hnsw" "VSET_VEC (HNSW)"

# VSET_TEXT hnsw
run_kvlite_bench "vset_text" "hnsw" "VSET_TEXT (HNSW)"

# VSEARCH_VEC hnsw
run_kvlite_bench "vsearch_vec" "hnsw" "VSEARCH_VEC (HNSW)"

# VSEARCH_TEXT hnsw
run_kvlite_bench "vsearch_text" "hnsw" "VSEARCH_TEXT (HNSW)"

# ── Faiss Baseline ──────────────────────────────────────────────────────
echo "### Faiss Baseline" | tee -a "$SUMMARY"

run_faiss_bench "flat_ip" "Faiss IndexFlatIP"
run_faiss_bench "hnsw" "Faiss IndexHNSWFlat" "16 200 128"

# ── Summary ─────────────────────────────────────────────────────────────
echo "" | tee -a "$SUMMARY"
echo "## Output Files" | tee -a "$SUMMARY"
echo "- Summary: $SUMMARY" | tee -a "$SUMMARY"
echo "- Raw CSV: $RAW_CSV" | tee -a "$SUMMARY"
echo "- Logs: $OUTPUT_DIR/*.log" | tee -a "$SUMMARY"
echo "" | tee -a "$SUMMARY"
echo "Done at $(date)" | tee -a "$SUMMARY"
