#!/usr/bin/env bash
# bench_vector_ef_compare.sh — Compare VSEARCH result consistency across ef_search values.
#
# Writes deterministic data once, then queries with both baseline and test ef_search.
# Computes topK overlap rates to validate whether lower ef_search is acceptable.
#
# Usage:
#   COUNT=1000 QUERY_COUNT=100 ./scripts/bench_vector_ef_compare.sh          # smoke
#   COUNT=100000 QUERY_COUNT=1000 ./scripts/bench_vector_ef_compare.sh        # formal
#   COUNT=100000 QUERY_COUNT=1000 TEST_EF=128 ./scripts/bench_vector_ef_compare.sh  # compare 128
#
# Output:
#   results/ef_compare/ef_<N>_results.txt
#   results/ef_compare/ef_compare_raw.csv
#   results/ef_compare/ef_compare_summary.md
#   logs/ef_compare/

set -euo pipefail

ROOT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
cd "$ROOT_DIR"

# ---- configurable env vars ----
KV_BIN="${KV_BIN:-./kvstore}"
HOST="${HOST:-127.0.0.1}"
PORT="${PORT:-9300}"
CONFIG="${CONFIG:-config/master.conf}"

COUNT="${COUNT:-100000}"
QUERY_COUNT="${QUERY_COUNT:-1000}"
TOPK="${TOPK:-10}"
SCOPE="${SCOPE:-efcmp_scope}"
PREFIX="${PREFIX:-efcmp}"
BASELINE_EF="${BASELINE_EF:-256}"
TEST_EF="${TEST_EF:-64}"

# Fixed server config
INDEX_TYPE="hnsw"
METRIC="ip"
DISTANCE_BACKEND="auto"

RESULT_DIR="results/ef_compare"
LOG_DIR="logs/ef_compare"

BENCH_BIN="${BENCH_BIN:-tests/vector_benchmark}"

RUN_ID="$(date +%Y%m%d_%H%M%S)"

# ---- helpers ----
log()  { echo "[$(date +%H:%M:%S)] $*"; }
warn() { echo "[$(date +%H:%M:%S)] WARN: $*" >&2; }

need_cmd() {
  if ! command -v "$1" >/dev/null 2>&1; then
    echo "[ERROR] Missing dependency: $1" >&2
    return 1
  fi
}

mkdir -p "$LOG_DIR" "$RESULT_DIR" data

# ---- kvstore lifecycle ----
kvstore_pid=""

start_kvstore() {
  local ef_search="$1"

  log "Starting kvstore: ef_search=$ef_search idx=$INDEX_TYPE metric=$METRIC"

  $KV_BIN --config "$CONFIG" \
    --vector_index_type "$INDEX_TYPE" \
    --vector_metric "$METRIC" \
    --vector_distance_backend "$DISTANCE_BACKEND" \
    --vector_hnsw_ef_search "$ef_search" \
    --vector_scope_overfetch_factor 1 \
    > "$LOG_DIR/${RUN_ID}_kvstore_ef${ef_search}.log" 2>&1 &

  kvstore_pid=$!
  wait_for_port "$HOST" "$PORT" 30
}

stop_kvstore() {
  if [ -n "${kvstore_pid:-}" ] && kill -0 "$kvstore_pid" 2>/dev/null; then
    log "Stopping kvstore pid=$kvstore_pid"
    kill "$kvstore_pid" 2>/dev/null || true
    wait "$kvstore_pid" 2>/dev/null || true
  fi
  kvstore_pid=""
  sleep 0.5
}

cleanup() {
  stop_kvstore
}
trap cleanup EXIT

wait_for_port() {
  local host="$1" port="$2" timeout="${3:-30}"
  local deadline=$(($(date +%s) + timeout))
  while ! nc -z "$host" "$port" 2>/dev/null; do
    if [ "$(date +%s)" -gt "$deadline" ]; then
      log "ERROR: timeout waiting for $host:$port"
      return 1
    fi
    sleep 0.5
  done
}

# ---- prepare data using vector_benchmark ----
prepare_data() {
  log "Writing $COUNT records (prefix=$PREFIX scope=$SCOPE)..."
  $BENCH_BIN --mode latency -t vset -n 1 \
    -h "$HOST" -p "$PORT" \
    --scope "$SCOPE" --prefix "$PREFIX" \
    --prepare "$COUNT" \
    > "$LOG_DIR/${RUN_ID}_prepare.log" 2>&1

  local su
  su=$(grep "^prepare_success=" "$LOG_DIR/${RUN_ID}_prepare.log" | cut -d= -f2)
  if [ "${su:-0}" -ne "$COUNT" ]; then
    log "ERROR: prepare failed, success=$su expected=$COUNT"
    cat "$LOG_DIR/${RUN_ID}_prepare.log"
    return 1
  fi
  log "Prepare done: $su records written"
}

# ---- run VSEARCH queries and save results ----
run_queries() {
  local ef="$1" outfile="$2"

  log "Running $QUERY_COUNT VSEARCH queries (ef=$ef topk=$TOPK)..."

  python3 - "$HOST" "$PORT" "$QUERY_COUNT" "$TOPK" "$SCOPE" "$PREFIX" "$outfile" <<'PYEOF'
import socket
import sys

host = sys.argv[1]
port = int(sys.argv[2])
query_count = int(sys.argv[3])
topk = int(sys.argv[4])
scope = sys.argv[5]
prefix = sys.argv[6]
outfile = sys.argv[7]

class RespClient:
    def __init__(self, host, port):
        self.sock = socket.create_connection((host, port), timeout=30.0)
        self.sock.setsockopt(socket.IPPROTO_TCP, socket.TCP_NODELAY, 1)
        self.buf = b""

    def close(self):
        try:
            self.sock.shutdown(socket.SHUT_RDWR)
        except OSError:
            pass
        self.sock.close()

    def _read_line(self):
        while b"\r\n" not in self.buf:
            chunk = self.sock.recv(8192)
            if not chunk:
                raise ConnectionError("connection closed")
            self.buf += chunk
        idx = self.buf.index(b"\r\n")
        line = self.buf[:idx]
        self.buf = self.buf[idx + 2:]
        return line

    def _read_exact(self, n):
        while len(self.buf) < n:
            chunk = self.sock.recv(8192)
            if not chunk:
                raise ConnectionError("connection closed")
            self.buf += chunk
        data = self.buf[:n]
        self.buf = self.buf[n:]
        return data

    @staticmethod
    def encode_cmd(tokens):
        parts = [f"*{len(tokens)}\r\n".encode()]
        for t in tokens:
            b = t.encode()
            parts.append(f"${len(b)}\r\n".encode())
            parts.append(b)
            parts.append(b"\r\n")
        return b"".join(parts)

    def send_cmd(self, tokens):
        self.sock.sendall(self.encode_cmd(tokens))

    def read_resp(self):
        prefix_byte = self._read_exact(1)
        t = prefix_byte.decode(errors="ignore")

        if t == "+":
            return "simple", self._read_line().decode(errors="ignore")
        if t == "-":
            return "error", self._read_line().decode(errors="ignore")
        if t == ":":
            return "integer", int(self._read_line())
        if t == "$":
            nbytes = int(self._read_line())
            if nbytes == -1:
                return "null", None
            data = self._read_exact(nbytes + 2)
            return "bulk", data[:nbytes].decode(errors="ignore")
        if t == "*":
            nitems = int(self._read_line())
            if nitems == -1:
                return "null", None
            return "array", [self.read_resp() for _ in range(nitems)]
        raise ValueError(f"Unknown RESP type: {prefix_byte!r}")

    def vsearch(self, query, scope, topk):
        tokens = ["VSEARCH", query]
        if scope:
            tokens.append(scope)
        tokens.append(str(topk))
        self.send_cmd(tokens)
        return self.read_resp()

    def vinfo(self):
        self.send_cmd(["VINFO"])
        return self.read_resp()

def extract_question(item):
    """Extract question value from a VSEARCH result item.
    Each result is a single bulk string: score:X|scope:Y|question:Z|answer:W
    """
    if item[0] != "bulk" or not item[1]:
        return None
    for part in item[1].split("|"):
        if part.startswith("question:"):
            return part[len("question:"):]
    return None

client = RespClient(host, port)

# Verify server is ready
rtype, rval = client.vinfo()
if rtype == "array":
    for item in rval:
        if item[0] == "bulk" and item[1]:
            if "index_ready:1" in item[1]:
                break
else:
    print(f"VINFO unexpected: {rtype}", file=sys.stderr)

with open(outfile, "w") as f:
    for i in range(query_count):
        query = f"{prefix}_question_{i:08d}"
        rtype, rval = client.vsearch(query, scope, topk)

        if rtype != "array":
            print(f"query {i}: unexpected response type {rtype}", file=sys.stderr)
            f.write(f"{i}\t{query}\tERROR\n")
            continue

        results = rval  # list of (type, value) tuples
        items = []
        for item in results:
            q = extract_question(item)
            if q:
                items.append(q)

        f.write(f"{i}\t{query}\t" + "\t".join(items) + "\n")

        if (i + 1) % 100 == 0 or i + 1 == query_count:
            print(f"  progress: {i+1}/{query_count}", file=sys.stderr)

client.close()
print(f"  Results saved to {outfile}", file=sys.stderr)
PYEOF

  log "Queries done (ef=$ef): $outfile"
}

# ---- compare results ----
compare_results() {
  local baseline_file="$1" test_file="$2" ef_test="$3"

  log "Comparing $baseline_file vs $test_file..."

  python3 - "$baseline_file" "$test_file" "$QUERY_COUNT" "$TOPK" "$ef_test" \
    "$RESULT_DIR/ef_compare_raw.csv" "$RESULT_DIR/ef_compare_summary.md" <<'PYEOF'
import sys
import os

baseline_file = sys.argv[1]
test_file = sys.argv[2]
query_count = int(sys.argv[3])
topk = int(sys.argv[4])
ef_test = sys.argv[5]
raw_csv = sys.argv[6]
summary_md = sys.argv[7]

def load_results(filepath):
    """Load tab-separated results: idx<TAB>query<TAB>item1<TAB>item2<...>"""
    results = {}
    if not os.path.exists(filepath):
        print(f"ERROR: {filepath} not found", file=sys.stderr)
        return results
    with open(filepath) as f:
        for line in f:
            line = line.strip()
            if not line:
                continue
            parts = line.split("\t")
            if len(parts) < 3:
                continue
            idx = int(parts[0])
            query = parts[1]
            items = parts[2:]
            results[idx] = (query, items)
    return results

baseline = load_results(baseline_file)
test = load_results(test_file)

if not baseline or not test:
    print("ERROR: empty results", file=sys.stderr)
    sys.exit(1)

total = 0
top1_same = 0
top3_overlap_sum = 0.0
top10_overlap_sum = 0.0
rank_shift_sum = 0.0
rank_shift_count = 0
missing_sum = 0.0

csv_lines = ["query_index,query,top1_same,top3_overlap,top10_overlap,missing_count,rank_shift_sum"]

for idx in sorted(baseline.keys()):
    if idx not in test:
        continue
    total += 1
    b_query, b_items = baseline[idx]
    t_query, t_items = test[idx]

    b_set = {}
    for rank, item in enumerate(b_items):
        if item not in b_set:
            b_set[item] = rank

    # top1 same
    t1 = 0
    if len(b_items) > 0 and len(t_items) > 0 and b_items[0] == t_items[0]:
        t1 = 1
        top1_same += 1

    # topK overlap
    k3 = min(3, len(b_items), len(t_items))
    k10 = min(topk, len(b_items), len(t_items))

    b_top3 = set(b_items[:3])
    t_top3 = set(t_items[:3])
    if k3 > 0:
        top3_overlap_sum += len(b_top3 & t_top3) / 3.0

    b_topk = set(b_items[:topk])
    t_topk = set(t_items[:topk])
    if k10 > 0:
        top10_overlap_sum += len(b_topk & t_topk) / topk

    # missing: baseline items not in test results
    missing = len(b_topk - t_topk)
    missing_sum += missing

    # rank shift: for items in both, compare positions
    rshift = 0
    rcount = 0
    for item in b_topk & t_topk:
        b_rank = b_set.get(item, -1)
        t_rank = -1
        for r, it in enumerate(t_items):
            if it == item:
                t_rank = r
                break
        if b_rank >= 0 and t_rank >= 0:
            rshift += abs(b_rank - t_rank)
            rcount += 1
    rank_shift_sum += rshift
    rank_shift_count += rcount

    csv_lines.append(f"{idx},{b_query},{t1},{b_top3 & t_top3 and len(b_top3 & t_top3)/3.0 or 0:.2f},{len(b_topk & t_topk)/topk:.2f},{missing},{rshift}")

if total == 0:
    print("ERROR: no overlapping query indices", file=sys.stderr)
    sys.exit(1)

top1_rate = top1_same / total * 100
top3_rate = top3_overlap_sum / total * 100
top10_rate = top10_overlap_sum / total * 100
avg_missing = missing_sum / total
avg_rank_shift = rank_shift_sum / max(rank_shift_count, 1)

# Write CSV
os.makedirs(os.path.dirname(raw_csv), exist_ok=True)
with open(raw_csv, "w") as f:
    f.write("\n".join(csv_lines) + "\n")

# Write summary
md = []
md.append("# ef_search 一致率对比报告")
md.append("")
md.append(f"> 基准 ef_search=256 vs 测试 ef_search={ef_test}")
md.append(f"> 数据量: 100000 (实际查询 {query_count} 条)")
md.append(f"> topK: {topk}")
md.append("")
md.append("## 结果指标")
md.append("")
md.append("| metric | value |")
md.append("|---|---:|")
md.append(f"| query_count | {total} |")
md.append(f"| top1_same_rate | {top1_rate:.1f}% |")
md.append(f"| top3_overlap_rate | {top3_rate:.1f}% |")
md.append(f"| top10_overlap_rate | {top10_rate:.1f}% |")
md.append(f"| avg_missing_rate | {avg_missing / topk * 100:.1f}% |")
md.append(f"| avg_rank_shift | {avg_rank_shift:.2f} |")
md.append("")

# Recommendation
if top1_rate >= 90 and top3_rate >= 90 and top10_rate >= 95:
    md.append("## 结论")
    md.append("")
    md.append(f"ef_search={ef_test} 与 ef_search=256 的结果一致性较高，**可以作为轻量默认配置**。")
else:
    md.append("## 结论")
    md.append("")
    md.append(f"ef_search={ef_test} 存在明显结果差异。")
    if int(ef_test) < 128:
        md.append(f"建议测试 ef_search=128 作为折中配置：")
        md.append("```bash")
        md.append(f"TEST_EF=128 ./scripts/bench_vector_ef_compare.sh")
        md.append("```")

md.append("")
md.append("> 注意：本测试比较的是 ef_search 参数变化导致的结果一致率。")
md.append("> embedding_provider=hash 时不代表真实语义召回质量。")
md.append("> 真正的 recall@K 需要 flat 精确搜索作为 ground truth。")

os.makedirs(os.path.dirname(summary_md), exist_ok=True)
with open(summary_md, "w") as f:
    f.write("\n".join(md) + "\n")

print(f"top1_same_rate={top1_rate:.1f}%", file=sys.stderr)
print(f"top3_overlap_rate={top3_rate:.1f}%", file=sys.stderr)
print(f"top10_overlap_rate={top10_rate:.1f}%", file=sys.stderr)
print(f"avg_missing_rate={avg_missing / topk * 100:.1f}%", file=sys.stderr)
print(f"avg_rank_shift={avg_rank_shift:.2f}", file=sys.stderr)
PYEOF
}

# ==================== Main ====================
main() {
  need_cmd nc
  need_cmd python3

  if [ ! -x "$BENCH_BIN" ]; then
    log "ERROR: $BENCH_BIN not found. Run 'make' first."
    return 1
  fi

  # Clean old data files to ensure fresh start
  rm -f data/kvstore-master.* data/kvstore-master.dat 2>/dev/null

  log "========== ef_search Compare: $BASELINE_EF vs $TEST_EF =========="
  log "Run ID: $RUN_ID"
  log "COUNT=$COUNT QUERY_COUNT=$QUERY_COUNT TOPK=$TOPK"

  BASELINE_FILE="$RESULT_DIR/ef_${BASELINE_EF}_results.txt"
  TEST_FILE="$RESULT_DIR/ef_${TEST_EF}_results.txt"

  # ---- Phase 1: Baseline (ef=256) ----
  log ""
  log "===== Phase 1: Baseline ef_search=$BASELINE_EF ====="
  start_kvstore "$BASELINE_EF"
  prepare_data
  run_queries "$BASELINE_EF" "$BASELINE_FILE"
  stop_kvstore

  # ---- Phase 2: Test (ef=64) ----
  log ""
  log "===== Phase 2: Test ef_search=$TEST_EF ====="
  # Re-clean to ensure fresh start for test ef
  rm -f data/kvstore-master.* data/kvstore-master.dat 2>/dev/null
  start_kvstore "$TEST_EF"
  prepare_data
  run_queries "$TEST_EF" "$TEST_FILE"
  stop_kvstore

  # ---- Phase 3: Compare ----
  log ""
  log "===== Phase 3: Compare Results ====="
  compare_results "$BASELINE_FILE" "$TEST_FILE" "$TEST_EF"

  log ""
  log "========== DONE =========="
  log "Baseline results: $BASELINE_FILE"
  log "Test results:     $TEST_FILE"
  log "Raw CSV:          $RESULT_DIR/ef_compare_raw.csv"
  log "Summary:          $RESULT_DIR/ef_compare_summary.md"
  log "Logs:             $LOG_DIR"

  # Show summary to stdout
  if [ -f "$RESULT_DIR/ef_compare_summary.md" ]; then
    echo ""
    cat "$RESULT_DIR/ef_compare_summary.md"
  fi
}

main
