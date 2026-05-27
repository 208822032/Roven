#!/bin/bash
#
# KVLite shutdown segfault reproduction stress test
# Cycles: start kvstore → concurrent ops → SIGTERM → check for crash
#

set -e

SCRIPT_DIR="$(cd "$(dirname "$0")" && pwd)"
PROJ_DIR="$(dirname "$SCRIPT_DIR")"
cd "$PROJ_DIR"

KVSTORE_BIN="${PROJ_DIR}/kvstore"
PORT=19300
HOST="127.0.0.1"
LOG_DIR="${PROJ_DIR}/tests/shutdown_stress_logs"
CORE_DIR="${PROJ_DIR}/tests/cores"

# Create a temp config file with reactor mode (no persistence to isolate the issue)
TMP_CONFIG="${LOG_DIR}/stress_test.conf"
mkdir -p "$LOG_DIR"
cat > "$TMP_CONFIG" << 'CONFEOF'
role = master
port = PLACEHOLDER_PORT
network_mode = reactor
sync_mode = tcp
sync_port = PLACEHOLDER_SYNC_PORT
enable_aof = no
CONFEOF
SYNC_PORT=$((PORT + 1000))
sed -i "s/PLACEHOLDER_PORT/${PORT}/" "$TMP_CONFIG"
sed -i "s/PLACEHOLDER_SYNC_PORT/${SYNC_PORT}/" "$TMP_CONFIG"

# Number of rounds (tune as needed)
ROUNDS=${1:-500}

# Number of concurrent client worker processes
NWORKERS=${2:-3}

# Seconds to let server run before sending SIGTERM
RUNTIME_MIN=1
RUNTIME_MAX=4

mkdir -p "$LOG_DIR" "$CORE_DIR"

# Check and save core_pattern
ORIG_CORE_PATTERN=$(cat /proc/sys/kernel/core_pattern 2>/dev/null || echo "unknown")
echo "[INFO] Current core_pattern: $ORIG_CORE_PATTERN"
echo "[INFO] If core dumps don't appear, may need: echo 'core.%e.%p' | sudo tee /proc/sys/kernel/core_pattern"

# Set core file location for this session if possible
if [ -w /proc/sys/kernel/core_pattern ]; then
    echo "${CORE_DIR}/core.%e.%p.%t" > /proc/sys/kernel/core_pattern 2>/dev/null || true
    echo "[INFO] Set core_pattern to ${CORE_DIR}/core.%e.%p.%t"
else
    echo "[WARN] Cannot write to /proc/sys/kernel/core_pattern (no sudo)."
    echo "[WARN] Cores may go to: $(cat /proc/sys/kernel/core_pattern 2>/dev/null)"
fi

# Make sure we have binaries built
echo "[INFO] Building kvstore and test clients..."
make kvstore 2>&1 | tail -3
make tests/test_binary_key 2>/dev/null || \
    gcc -I ./include -I ./src -g -O2 -o tests/test_binary_key tests/test_binary_key.c

# Ensure we can generate cores
ulimit -c unlimited 2>/dev/null || true
echo "[INFO] ulimit -c: $(ulimit -c)"

CRASH_COUNT=0
CORE_COUNT=0

echo "================================================"
echo "  KVLite Shutdown Segfault Stress Test"
echo "  Rounds: $ROUNDS  |  Workers: $NWORKERS"
echo "  Port: $PORT"
echo "================================================"

cleanup_round() {
    # Kill any lingering kvstore processes on our port
    pkill -f "kvstore.*${PORT}" 2>/dev/null || true
    pkill -f "kvstore" 2>/dev/null || true
    sleep 0.5
}

# Worker: sends random commands to the kvstore
worker_func() {
    local id=$1
    local round=$2
    local port=$3
    local end_time=$(($(date +%s) + RUNTIME_MAX + 5))

    while [ $(date +%s) -lt $end_time ]; do
        local cmd=$((RANDOM % 10))
        local key="stress_key_${id}_${RANDOM}"
        local val="stress_val_${RANDOM}_${RANDOM}"

        case $cmd in
            0|1)
                # SET (plain)
                printf "SET %s %s\r\n" "$key" "$val" | nc -w 2 $HOST $port > /dev/null 2>&1 || true
                ;;
            2|3)
                # GET
                printf "GET %s\r\n" "$key" | nc -w 2 $HOST $port > /dev/null 2>&1 || true
                ;;
            4)
                # MOD (on rbtree key - RSET then RMOD)
                printf "RSET %s %s\r\n" "$key" "$val" | nc -w 2 $HOST $port > /dev/null 2>&1 || true
                printf "RMOD %s mod_%s\r\n" "$key" "$val" | nc -w 2 $HOST $port > /dev/null 2>&1 || true
                ;;
            5)
                # DEL
                printf "DEL %s\r\n" "$key" | nc -w 2 $HOST $port > /dev/null 2>&1 || true
                ;;
            6)
                # EXPIRE
                printf "EXPIRE %s 60\r\n" "$key" | nc -w 2 $HOST $port > /dev/null 2>&1 || true
                ;;
            7)
                # SET with binary-like key (embedded printable variant)
                printf "SET binkey_%d_%d bingen_val_%d\r\n" "$id" "$RANDOM" "$RANDOM" | nc -w 2 $HOST $port > /dev/null 2>&1 || true
                ;;
            8)
                # HSET/HGET
                printf "HSET hkey_%d_%d hval_%d\r\n" "$id" "$RANDOM" "$RANDOM" | nc -w 2 $HOST $port > /dev/null 2>&1 || true
                printf "HGET hkey_%d_%d\r\n" "$id" "$RANDOM" | nc -w 2 $HOST $port > /dev/null 2>&1 || true
                ;;
            9)
                # SSET
                printf "SSET skey_%d_%d sval_%d\r\n" "$id" "$RANDOM" "$RANDOM" | nc -w 2 $HOST $port > /dev/null 2>&1 || true
                ;;
        esac
    done
}

for round in $(seq 1 $ROUNDS); do
    round_log="${LOG_DIR}/round_${round}.log"

    echo -n "[Round $round/$ROUNDS] "

    # Clean up from previous round
    cleanup_round
    rm -f "${PROJ_DIR}/data/kvstore.rdb" "${PROJ_DIR}/data/kvstore.aof" 2>/dev/null || true

    # Start kvstore in background
    ${KVSTORE_BIN} --config ${TMP_CONFIG} \
        > "$round_log" 2>&1 &
    KVS_PID=$!

    # Wait for server to be ready
    for i in $(seq 1 20); do
        if nc -z $HOST $PORT 2>/dev/null; then
            break
        fi
        sleep 0.3
    done

    if ! kill -0 $KVS_PID 2>/dev/null; then
        echo "FAIL (kvstore failed to start)"
        cat "$round_log" | tail -20
        continue
    fi

    # Launch concurrent workers
    worker_pids=()
    for w in $(seq 1 $NWORKERS); do
        worker_func $w $round $PORT &
        worker_pids+=($!)
    done

    # Let it run for a random duration then SIGTERM
    sleep_time=$(awk "BEGIN { srand(); print ${RUNTIME_MIN} + rand() * (${RUNTIME_MAX} - ${RUNTIME_MIN}) }")
    sleep "$sleep_time"

    # Send SIGTERM
    kill -TERM $KVS_PID 2>/dev/null || true

    # Wait for workers to finish
    for wp in "${worker_pids[@]}"; do
        wait $wp 2>/dev/null || true
    done

    # Wait for kvstore to exit (with timeout)
    wait_sec=0
    while kill -0 $KVS_PID 2>/dev/null && [ $wait_sec -lt 10 ]; do
        sleep 0.5
        wait_sec=$((wait_sec + 1))
    done

    # If still running, force kill
    if kill -0 $KVS_PID 2>/dev/null; then
        echo -n "(force kill) "
        kill -KILL $KVS_PID 2>/dev/null || true
        sleep 0.5
    fi

    # Check exit status / crash
    wait $KVS_PID 2>/dev/null
    exit_code=$?

    # Check for crash indicators in log
    if grep -qi "segmentation fault\|SIGSEGV\|core dump\|signal 11\|Assertion.*failed\|double free\|corruption\|use-after-free\|AddressSanitizer\|heap-buffer-overflow\|heap-use-after-free" "$round_log" 2>/dev/null; then
        echo "CRASH DETECTED (log evidence)"
        CRASH_COUNT=$((CRASH_COUNT + 1))
        cp "$round_log" "${LOG_DIR}/CRASH_round_${round}.log"
    elif [ $exit_code -gt 128 ] && [ $exit_code -ne 137 ] && [ $exit_code -ne 143 ]; then
        # Exit > 128 minus signal number = killed by signal
        sig=$((exit_code - 128))
        if [ $sig -eq 11 ] || [ $sig -eq 6 ]; then
            echo "CRASH DETECTED (signal $sig)"
            CRASH_COUNT=$((CRASH_COUNT + 1))
            cp "$round_log" "${LOG_DIR}/CRASH_round_${round}.log"
        else
            echo "OK (signal $sig, exit=$exit_code)"
        fi
    else
        echo "OK (exit=$exit_code)"
    fi

    # Check for new core files
    new_cores=$(find "$CORE_DIR" -name "core*" -newer "$round_log" -type f 2>/dev/null | wc -l)
    if [ "$new_cores" -gt 0 ]; then
        CORE_COUNT=$((CORE_COUNT + new_cores))
        echo "  [CORE] $new_cores core file(s) found"
        # Try to get backtrace if gdb available
        for corefile in $(find "$CORE_DIR" -name "core*" -newer "$round_log" -type f 2>/dev/null); do
            gdb -batch -ex "bt full" -ex "quit" "$KVSTORE_BIN" "$corefile" 2>&1 | head -60 > "${LOG_DIR}/backtrace_round_${round}.txt"
        done
    fi

    # Progress indicator
    if [ $((round % 50)) -eq 0 ]; then
        echo "  [Progress] $round/$ROUNDS rounds done, crashes so far: $CRASH_COUNT"
    fi
done

cleanup_round

echo ""
echo "================================================"
echo "  Stress Test Complete"
echo "================================================"
echo "  Total rounds:    $ROUNDS"
echo "  Crashes:         $CRASH_COUNT"
echo "  Core files:      $CORE_COUNT"
echo "  Logs:            $LOG_DIR"
echo "  Cores:           $CORE_DIR"
echo "================================================"

if [ "$CRASH_COUNT" -gt 0 ]; then
    echo ""
    echo "FAIL: $CRASH_COUNT crashes detected!"
    echo "Crash logs:"
    ls -la "${LOG_DIR}"/CRASH_round_*.log 2>/dev/null || echo "  (none saved)"
    exit 1
else
    echo ""
    echo "PASS: No crashes in $ROUNDS rounds"
    exit 0
fi
