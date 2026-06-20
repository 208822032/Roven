#!/usr/bin/env bash
set -euo pipefail

ROOT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
cd "$ROOT_DIR"

KV_BIN="${KV_BIN:-./kvstore}"
CLIENT_BIN="${CLIENT_BIN:-./tests/kvstore_client}"
BENCH_BIN="${BENCH_BIN:-./tests/ttl_expired_path_bench}"
EBPF_SYNCD_BIN="${EBPF_SYNCD_BIN:-./src/eBPF/ebpf_syncd}"
HOST="${KV_HOST:-127.0.0.1}"

if [[ -n "${EXPIRED_PATH_BACKENDS:-}" ]]; then
  read -r -a BACKENDS <<<"$(printf '%s' "$EXPIRED_PATH_BACKENDS" | tr ',' ' ')"
else
  BACKENDS=(skiptable)
fi

KEYSPACE="${EXPIRED_PATH_KEYSPACE:-100000}"
REQUESTS="${EXPIRED_PATH_REQUESTS:-100000}"
WARMUP="${EXPIRED_PATH_WARMUP:-10000}"
VALUE_SIZE="${EXPIRED_PATH_VALUE_SIZE:-32}"
NON_EXPIRED_TTL_SECS="${EXPIRED_PATH_NON_EXPIRED_TTL_SECS:-60}"
EXPIRED_TTL_SECS="${EXPIRED_PATH_EXPIRED_TTL_SECS:-2}"
WAIT_BUFFER_SECS="${EXPIRED_PATH_WAIT_BUFFER_SECS:-2}"
VERIFY_SLAVE="${EXPIRED_PATH_VERIFY_SLAVE:-1}"
PROFILE_MODE="${EXPIRED_PATH_PERF_MODE:-none}"
PERF_SECONDS="${EXPIRED_PATH_PERF_SECONDS:-20}"
SUDO_PASSWORD="${EXPIRED_PATH_SUDO_PASSWORD:-}"

TMP_DIR="$(mktemp -d)"
MASTER_PID=""
SLAVE_PID=""
SYNCD_PID=""
MASTER_LOG=""
SLAVE_LOG=""
SYNCD_LOG=""
PERF_PID=""
PERF_DATA=""
PERF_LOG=""

cleanup() {
  for pid_var in PERF_PID SYNCD_PID SLAVE_PID MASTER_PID; do
    local pid="${!pid_var:-}"
    if [[ -n "$pid" ]] && kill -0 "$pid" 2>/dev/null; then
      kill -INT "$pid" 2>/dev/null || true
      wait "$pid" 2>/dev/null || true
    fi
  done
  if [[ "${EXPIRED_PATH_KEEP_TMP:-0}" == "1" ]]; then
    printf '[INFO][cleanup] keeping tmp dir: %s\n' "$TMP_DIR"
  else
    rm -rf "$TMP_DIR"
  fi
}
trap cleanup EXIT

normalize_output() {
  python3 -c 'import re,sys; data=sys.stdin.read(); data=re.sub(r"\x1b\[[0-9;]*[A-Za-z]", "", data); data=data.replace("\r", " ").replace("\n", " "); data=re.sub(r"[ \t]+", " ", data).strip(); sys.stdout.write(data)'
}

run_client() {
  "$CLIENT_BIN" "$HOST" "$1" -c "$2" | normalize_output
}

wait_for_port_ready() {
  local port="$1"
  local probe_cmd="$2"
  local i
  for i in $(seq 1 180); do
    if run_client "$port" "$probe_cmd" >/dev/null 2>&1; then
      return 0
    fi
    sleep 0.1
  done
  return 1
}

start_master() {
  local backend="$1"
  local port="$2"
  local sync_mode="$3"

  MASTER_LOG="$TMP_DIR/${backend}_master.log"
  local config_file="$TMP_DIR/${backend}_master.conf"

  rm -f "$TMP_DIR/${backend}_master.aof" "$TMP_DIR/${backend}_master.rdb" "$TMP_DIR/${backend}_master.dat"

  cat > "$config_file" <<EOF
role = master
port = $port
sync_port = $((port + 1000))
enable_aof = no
aof_fsync_mode = no
aof_filename = $TMP_DIR/${backend}_master.aof
rdb_filename = $TMP_DIR/${backend}_master.rdb
dump_filename = $TMP_DIR/${backend}_master.dat
network_mode = reactor
mem_backend = pool
sync_mode = $sync_mode
max_value_size = 1048576
EOF

  "$KV_BIN" --config "$config_file" > "$MASTER_LOG" 2>&1 &
  MASTER_PID=$!
}

start_slave() {
  local backend="$1"
  local port="$2"
  local master_port="$3"
  local sync_mode="$4"

  SLAVE_LOG="$TMP_DIR/${backend}_slave.log"
  local config_file="$TMP_DIR/${backend}_slave.conf"

  rm -f "$TMP_DIR/${backend}_slave.aof" "$TMP_DIR/${backend}_slave.rdb" "$TMP_DIR/${backend}_slave.dat"

  cat > "$config_file" <<EOF
role = slave
port = $port
master_ip = 127.0.0.1
master_port = $master_port
sync_port = $((master_port + 1000))
enable_aof = no
aof_fsync_mode = no
aof_filename = $TMP_DIR/${backend}_slave.aof
rdb_filename = $TMP_DIR/${backend}_slave.rdb
dump_filename = $TMP_DIR/${backend}_slave.dat
network_mode = reactor
mem_backend = pool
sync_mode = $sync_mode
max_value_size = 1048576
EOF

  "$KV_BIN" --config "$config_file" > "$SLAVE_LOG" 2>&1 &
  SLAVE_PID=$!
}

stop_master() {
  if [[ -n "${MASTER_PID:-}" ]] && kill -0 "$MASTER_PID" 2>/dev/null; then
    kill -TERM "$MASTER_PID" 2>/dev/null || true
    wait "$MASTER_PID" 2>/dev/null || true
  fi
  MASTER_PID=""
}

stop_slave() {
  if [[ -n "${SLAVE_PID:-}" ]] && kill -0 "$SLAVE_PID" 2>/dev/null; then
    kill -TERM "$SLAVE_PID" 2>/dev/null || true
    wait "$SLAVE_PID" 2>/dev/null || true
  fi
  SLAVE_PID=""
}

stop_syncd() {
  if [[ -n "${SYNCD_PID:-}" ]] && kill -0 "$SYNCD_PID" 2>/dev/null; then
    kill -TERM "$SYNCD_PID" 2>/dev/null || true
    wait "$SYNCD_PID" 2>/dev/null || true
  fi
  SYNCD_PID=""
}

start_syncd() {
  local backend="$1"
  local master_port="$2"
  local slave_port="$3"

  SYNCD_LOG="$TMP_DIR/${backend}_syncd.log"
  local syncd_config="$TMP_DIR/${backend}_ebpf_sync.conf"

  cat > "$syncd_config" <<EOF
master_ip=127.0.0.1
master_port=$master_port
slave_count=1
slave_ip=127.0.0.1
slave_port=$slave_port
max_payload=1048576
EOF

  "$EBPF_SYNCD_BIN" "$syncd_config" > "$SYNCD_LOG" 2>&1 &
  SYNCD_PID=$!
}

run_phase() {
  local port="$1"
  local mode="$2"
  local phase="$3"
  local scenario="$4"
  local ttl_secs="$5"

  "$BENCH_BIN" "$HOST" "$port" "$mode" "$phase" "$scenario" "$KEYSPACE" "$REQUESTS" "$WARMUP" "$ttl_secs" "$WAIT_BUFFER_SECS" "$VALUE_SIZE"
}

run_perf_record() {
  local label="$1"

  PERF_DATA="$TMP_DIR/${label}.perf.data"
  PERF_LOG="$TMP_DIR/${label}.perf.log"

  if [[ -n "$SUDO_PASSWORD" ]]; then
    printf '%s\n' "$SUDO_PASSWORD" | sudo -S -p '' perf record -g -F 99 -p "$MASTER_PID" -o "$PERF_DATA" -- sleep "$PERF_SECONDS" > "$PERF_LOG" 2>&1 &
  else
    sudo perf record -g -F 99 -p "$MASTER_PID" -o "$PERF_DATA" -- sleep "$PERF_SECONDS" > "$PERF_LOG" 2>&1 &
  fi
  PERF_PID=$!
}

run_perf_stat() {
  local label="$1"

  PERF_LOG="$TMP_DIR/${label}.perf.stat.log"
  if [[ -n "$SUDO_PASSWORD" ]]; then
    printf '%s\n' "$SUDO_PASSWORD" | sudo -S -p '' perf stat -e cpu-clock,task-clock,context-switches,cpu-migrations,cycles,instructions -p "$MASTER_PID" > "$PERF_LOG" 2>&1 &
  else
    sudo perf stat -e cpu-clock,task-clock,context-switches,cpu-migrations,cycles,instructions -p "$MASTER_PID" > "$PERF_LOG" 2>&1 &
  fi
  PERF_PID=$!
}

stop_perf_and_report() {
  local mode="$1"
  if [[ "$mode" != "record" && -n "$PERF_PID" ]] && kill -0 "$PERF_PID" 2>/dev/null; then
    kill -INT "$PERF_PID" 2>/dev/null || true
    wait "$PERF_PID" 2>/dev/null || true
  elif [[ "$mode" == "record" && -n "$PERF_PID" ]]; then
    wait "$PERF_PID" 2>/dev/null || true
  fi
  PERF_PID=""

  if [[ "$mode" == "record" && -f "$PERF_DATA" ]]; then
    if [[ -n "$SUDO_PASSWORD" ]]; then
      printf '%s\n' "$SUDO_PASSWORD" | sudo -S -p '' perf report --stdio -i "$PERF_DATA" --sort symbol,dso > "$PERF_LOG.report" 2>&1
    else
      sudo perf report --stdio -i "$PERF_DATA" --sort symbol,dso > "$PERF_LOG.report" 2>&1
    fi
    tail -n 40 "$PERF_LOG.report"
  elif [[ "$mode" == "stat" && -f "$PERF_LOG" ]]; then
    cat "$PERF_LOG"
  fi
}

pick_ports() {
  python3 - <<'PY'
import socket

ports = []
while len(ports) < 3:
    sock = socket.socket()
    sock.bind(("127.0.0.1", 0))
    port = sock.getsockname()[1]
    sock.close()
    if port not in ports:
        ports.append(port)
print(" ".join(str(port) for port in ports))
PY
}

run_non_expired() {
  local backend="$1"
  local port="$2"
  local mode="$3"

  printf '[INFO][%s][%s] prepare\n' "$backend" "non-expired-read"
  run_phase "$port" "$mode" "prepare" 1 "$NON_EXPIRED_TTL_SECS"

  if [[ "$PROFILE_MODE" == "record" ]]; then
    run_perf_record "${backend}_non_expired"
  elif [[ "$PROFILE_MODE" == "stat" ]]; then
    run_perf_stat "${backend}_non_expired"
  fi

  printf '[INFO][%s][%s] measure\n' "$backend" "non-expired-read"
  run_phase "$port" "$mode" "measure" 1 "$NON_EXPIRED_TTL_SECS"
  stop_perf_and_report "$PROFILE_MODE"
}

run_expired_delete_only() {
  local backend="$1"
  local port="$2"
  local mode="$3"

  printf '[INFO][%s][%s] prepare\n' "$backend" "expired-delete-only"
  run_phase "$port" "$mode" "prepare" 2 "$EXPIRED_TTL_SECS"
  sleep $((EXPIRED_TTL_SECS + WAIT_BUFFER_SECS))

  if [[ "$PROFILE_MODE" == "record" ]]; then
    run_perf_record "${backend}_expired_delete_only"
  elif [[ "$PROFILE_MODE" == "stat" ]]; then
    run_perf_stat "${backend}_expired_delete_only"
  fi

  printf '[INFO][%s][%s] measure\n' "$backend" "expired-delete-only"
  run_phase "$port" "$mode" "measure" 2 "$EXPIRED_TTL_SECS"
  stop_perf_and_report "$PROFILE_MODE"
}

run_expired_delete_tcp_sync() {
  local backend="$1"
  local master_port="$2"
  local slave_port="$3"
  local mode="$4"

  printf '[INFO][%s][%s] prepare\n' "$backend" "expired-delete-tcp-sync"
  run_phase "$master_port" "$mode" "prepare" 3 "$EXPIRED_TTL_SECS"
  sleep $((EXPIRED_TTL_SECS + WAIT_BUFFER_SECS))

  if [[ "$PROFILE_MODE" == "record" ]]; then
    run_perf_record "${backend}_expired_delete_tcp"
  elif [[ "$PROFILE_MODE" == "stat" ]]; then
    run_perf_stat "${backend}_expired_delete_tcp"
  fi

  printf '[INFO][%s][%s] measure\n' "$backend" "expired-delete-tcp-sync"
  run_phase "$master_port" "$mode" "measure" 3 "$EXPIRED_TTL_SECS"
  stop_perf_and_report "$PROFILE_MODE"

  if [[ "$VERIFY_SLAVE" == "1" ]]; then
    local probe_key="exp_s3_${backend}_000000"
    local out
    out="$(run_client "$slave_port" "GET $probe_key")"
    out="$(printf '%s' "$out" | normalize_output)"
    if [[ "$out" != "NO EXIST" ]]; then
      printf '[FAIL][%s][tcp-sync-verify] expected NO EXIST actual=%s\n' "$backend" "$out" >&2
      return 1
    fi
    printf '[PASS][%s][tcp-sync-verify] GET %s -> NO EXIST\n' "$backend" "$probe_key"
  fi
}

run_expired_delete_ebpf_sync() {
  local backend="$1"
  local master_port="$2"
  local slave_port="$3"
  local mode="$4"

  printf '[INFO][%s][%s] prepare\n' "$backend" "expired-delete-ebpf-sync"
  run_phase "$master_port" "$mode" "prepare" 4 "$EXPIRED_TTL_SECS"
  sleep $((EXPIRED_TTL_SECS + WAIT_BUFFER_SECS))

  if [[ "$PROFILE_MODE" == "record" ]]; then
    run_perf_record "${backend}_expired_delete_ebpf"
  elif [[ "$PROFILE_MODE" == "stat" ]]; then
    run_perf_stat "${backend}_expired_delete_ebpf"
  fi

  printf '[INFO][%s][%s] measure\n' "$backend" "expired-delete-ebpf-sync"
  run_phase "$master_port" "$mode" "measure" 4 "$EXPIRED_TTL_SECS"
  stop_perf_and_report "$PROFILE_MODE"

  if [[ "$VERIFY_SLAVE" == "1" ]]; then
    local probe_key="exp_s4_${backend}_000000"
    local out
    out="$(run_client "$slave_port" "GET $probe_key")"
    out="$(printf '%s' "$out" | normalize_output)"
    if [[ "$out" != "NO EXIST" ]]; then
      printf '[FAIL][%s][ebpf-sync-verify] expected NO EXIST actual=%s\n' "$backend" "$out" >&2
      return 1
    fi
    printf '[PASS][%s][ebpf-sync-verify] GET %s -> NO EXIST\n' "$backend" "$probe_key"
  fi
}

main() {
  if [[ ! -x "$BENCH_BIN" ]]; then
    make -j2 tests/ttl_expired_path_bench
  fi

  local backend mode MASTER_PORT SLAVE_PORT SYNCD_PORT
  for backend in "${BACKENDS[@]}"; do
    case "$backend" in
      array) mode=1 ;;
      rbtree) mode=2 ;;
      hash) mode=3 ;;
      skiptable) mode=4 ;;
      all) mode=5 ;;
      *)
        printf '[FAIL] unknown backend=%s\n' "$backend" >&2
        exit 1
        ;;
    esac

    read -r MASTER_PORT SLAVE_PORT SYNCD_PORT < <(pick_ports)
    printf '[INFO][%s] master=%s slave=%s syncd=%s\n' "$backend" "$MASTER_PORT" "$SLAVE_PORT" "$SYNCD_PORT"

    start_master "$backend" "$MASTER_PORT" ebpf
    if ! wait_for_port_ready "$MASTER_PORT" "EXIST __expired_path_master_probe__"; then
      printf '[FAIL][%s] master startup timeout\n' "$backend" >&2
      [[ -f "$MASTER_LOG" ]] && tail -n 120 "$MASTER_LOG" >&2 || true
      exit 1
    fi

    run_non_expired "$backend" "$MASTER_PORT" "$mode"
    stop_master

    start_master "$backend" "$MASTER_PORT" ebpf
    if ! wait_for_port_ready "$MASTER_PORT" "EXIST __expired_path_master_probe__"; then
      printf '[FAIL][%s] expired-delete-only master startup timeout\n' "$backend" >&2
      [[ -f "$MASTER_LOG" ]] && tail -n 120 "$MASTER_LOG" >&2 || true
      exit 1
    fi

    run_expired_delete_only "$backend" "$MASTER_PORT" "$mode"

    stop_master
    start_master "$backend" "$MASTER_PORT" tcp
    if ! wait_for_port_ready "$MASTER_PORT" "EXIST __expired_path_master_probe__"; then
      printf '[FAIL][%s] tcp master startup timeout\n' "$backend" >&2
      [[ -f "$MASTER_LOG" ]] && tail -n 120 "$MASTER_LOG" >&2 || true
      exit 1
    fi
    start_slave "$backend" "$SLAVE_PORT" "$MASTER_PORT" tcp
    if ! wait_for_port_ready "$SLAVE_PORT" "EXIST __expired_path_slave_probe__"; then
      printf '[FAIL][%s] tcp slave startup timeout\n' "$backend" >&2
      [[ -f "$SLAVE_LOG" ]] && tail -n 120 "$SLAVE_LOG" >&2 || true
      exit 1
    fi

    run_expired_delete_tcp_sync "$backend" "$MASTER_PORT" "$SLAVE_PORT" "$mode"

    stop_slave
    stop_master

    if [[ "${EXPIRED_PATH_RUN_EBPF:-0}" == "1" ]]; then
      start_master "$backend" "$MASTER_PORT" ebpf
      if ! wait_for_port_ready "$MASTER_PORT" "EXIST __expired_path_master_probe__"; then
        printf '[FAIL][%s] ebpf master startup timeout\n' "$backend" >&2
        [[ -f "$MASTER_LOG" ]] && tail -n 120 "$MASTER_LOG" >&2 || true
        exit 1
      fi
      start_slave "$backend" "$SLAVE_PORT" "$MASTER_PORT" ebpf
      if ! wait_for_port_ready "$SLAVE_PORT" "EXIST __expired_path_slave_probe__"; then
        printf '[FAIL][%s] ebpf slave startup timeout\n' "$backend" >&2
        [[ -f "$SLAVE_LOG" ]] && tail -n 120 "$SLAVE_LOG" >&2 || true
        exit 1
      fi
      start_syncd "$backend" "$MASTER_PORT" "$SLAVE_PORT"
      sleep 1
      run_expired_delete_ebpf_sync "$backend" "$MASTER_PORT" "$SLAVE_PORT" "$mode"
      if [[ -n "$SYNCD_PID" ]] && kill -0 "$SYNCD_PID" 2>/dev/null; then
        kill -TERM "$SYNCD_PID" 2>/dev/null || true
        wait "$SYNCD_PID" 2>/dev/null || true
        SYNCD_PID=""
      fi
      stop_slave
      stop_master
    fi
  done
}

main "$@"
