#!/usr/bin/env python3
"""B-scale-7 product RESP benchmark with prepared and public query modes."""

import argparse
import hashlib
import json
import math
import os
from pathlib import Path
import re
import shutil
import signal
import socket
import statistics
import subprocess
import sys
import threading
import time


SCHEMA_VERSION = 1
RECORD_PREFIX = "__kvlite_bench_record__:"
QUERY_PREFIX = "__kvlite_bench_query__:"
PROFILE_TOTAL_FIELDS = [
    "vsearch_total_us",
    "vsearch_backend_search_total_us",
    "vsearch_manager_resolve_total_us",
    "vsearch_response_assembly_total_us",
    "vsearch_raw_hit_count",
    "vsearch_resolved_hit_count",
    "vsearch_filtered_hit_count",
]
PROFILE_COUNT_FIELDS = [
    "vsearch_total_count",
    "vsearch_backend_search_count",
    "vsearch_manager_resolve_count",
    "vsearch_response_assembly_count",
]


def fail(message):
    raise RuntimeError(message)


def sha256_file(path):
    digest = hashlib.sha256()
    with open(path, "rb") as stream:
        while True:
            chunk = stream.read(1024 * 1024)
            if not chunk:
                break
            digest.update(chunk)
    return digest.hexdigest()


def file_info(path):
    return {"path": Path(path).name, "size_bytes": Path(path).stat().st_size,
            "sha256": sha256_file(path)}


def json_load(path):
    with open(path, "r", encoding="utf-8") as stream:
        return json.load(stream)


def json_dump_atomic(path, value):
    path = Path(path)
    temp = path.with_name(path.name + f".tmp.{os.getpid()}")
    with open(temp, "w", encoding="utf-8") as stream:
        json.dump(value, stream, ensure_ascii=False, indent=2, sort_keys=True)
        stream.write("\n")
        stream.flush()
        os.fsync(stream.fileno())
    os.replace(temp, path)


def percentile(values, percent):
    if not values:
        return 0.0
    ordered = sorted(values)
    index = max(0, math.ceil(percent * len(ordered)) - 1)
    return float(ordered[index])


def summarize_latencies(values):
    total = sum(values)
    return {
        "count": len(values),
        "total_us": round(total, 3),
        "avg_us": round(total / len(values), 3) if values else 0.0,
        "p50_us": round(percentile(values, 0.50), 3),
        "p95_us": round(percentile(values, 0.95), 3),
        "p99_us": round(percentile(values, 0.99), 3),
        "min_us": round(min(values), 3) if values else 0.0,
        "max_us": round(max(values), 3) if values else 0.0,
        "qps": round(1000000.0 * len(values) / total, 3) if total else 0.0,
    }


def encode_resp(parts):
    data = [f"*{len(parts)}\r\n".encode()]
    for part in parts:
        if isinstance(part, str):
            part = part.encode("utf-8")
        data.extend([f"${len(part)}\r\n".encode(), part, b"\r\n"])
    return b"".join(data)


class RespConnection:
    def __init__(self, host, port, timeout=180.0):
        self.sock = socket.create_connection((host, port), timeout=timeout)
        self.sock.settimeout(timeout)
        self.stream = self.sock.makefile("rb")

    def close(self):
        try:
            self.stream.close()
        finally:
            self.sock.close()

    def command(self, *parts):
        self.sock.sendall(encode_resp(parts))
        return self._read()

    def _line(self):
        line = self.stream.readline()
        if not line or not line.endswith(b"\r\n"):
            fail("truncated RESP response")
        return line[:-2]

    def _read(self):
        prefix = self.stream.read(1)
        if prefix == b"+":
            return self._line().decode("utf-8", errors="strict")
        if prefix == b"-":
            fail("server error: " + self._line().decode("utf-8", errors="replace"))
        if prefix == b":":
            return int(self._line())
        if prefix == b"$":
            size = int(self._line())
            if size < 0:
                return None
            data = self.stream.read(size)
            if len(data) != size or self.stream.read(2) != b"\r\n":
                fail("truncated RESP bulk string")
            return data.decode("utf-8", errors="strict")
        if prefix == b"*":
            count = int(self._line())
            return [self._read() for _ in range(count)] if count >= 0 else None
        fail(f"unsupported RESP prefix: {prefix!r}")


def wait_for_server(port, process, timeout=120.0):
    deadline = time.monotonic() + timeout
    while time.monotonic() < deadline:
        if process.poll() is not None:
            fail(f"server exited during startup with code {process.returncode}")
        try:
            conn = RespConnection("127.0.0.1", port, timeout=1.0)
            conn.close()
            return
        except (OSError, RuntimeError):
            time.sleep(0.1)
    fail("server startup timeout")


def read_proc_rss_kb(pid):
    try:
        text = Path(f"/proc/{pid}/status").read_text(encoding="utf-8")
    except OSError:
        return 0
    match = re.search(r"^VmRSS:\s+(\d+)\s+kB$", text, re.MULTILINE)
    return int(match.group(1)) if match else 0


class RssMonitor:
    def __init__(self, pid):
        self.pid = pid
        self.peak_kb = 0
        self.stop_event = threading.Event()
        self.thread = threading.Thread(target=self._run, daemon=True)

    def _run(self):
        while not self.stop_event.wait(0.1):
            self.peak_kb = max(self.peak_kb, read_proc_rss_kb(self.pid))

    def start(self):
        self.thread.start()

    def stop(self):
        self.stop_event.set()
        self.thread.join()
        self.peak_kb = max(self.peak_kb, read_proc_rss_kb(self.pid))


def wait_for_child_pid(parent_pid, timeout=10.0):
    path = Path(f"/proc/{parent_pid}/task/{parent_pid}/children")
    deadline = time.monotonic() + timeout
    while time.monotonic() < deadline:
        try:
            children = path.read_text().split()
        except OSError:
            children = []
        if children:
            return int(children[0])
        time.sleep(0.01)
    fail("could not identify benchmark server child process")


def find_free_port():
    with socket.socket() as sock:
        sock.bind(("127.0.0.1", 0))
        return sock.getsockname()[1]


def parse_vinfo(response):
    if not isinstance(response, list):
        fail("VINFO did not return an array")
    result = {}
    for item in response:
        if isinstance(item, str) and ":" in item:
            key, value = item.split(":", 1)
            result[key] = value
    return result


def int_field(values, name):
    try:
        return int(values[name])
    except (KeyError, ValueError):
        fail(f"missing or invalid VINFO field: {name}")


def vinfo_delta(before, after):
    delta = {}
    for name in PROFILE_TOTAL_FIELDS + PROFILE_COUNT_FIELDS:
        value = int_field(after, name) - int_field(before, name)
        if value < 0:
            fail(f"negative VINFO delta: {name}")
        delta[name] = value
    for label, total_name, count_name in [
        ("server_vsearch_avg_us", "vsearch_total_us", "vsearch_total_count"),
        ("backend_search_avg_us", "vsearch_backend_search_total_us", "vsearch_backend_search_count"),
        ("manager_resolve_avg_us", "vsearch_manager_resolve_total_us", "vsearch_manager_resolve_count"),
        ("response_assembly_avg_us", "vsearch_response_assembly_total_us", "vsearch_response_assembly_count"),
    ]:
        count = delta[count_name]
        delta[label] = round(delta[total_name] / count, 3) if count else 0.0
    return delta


def validate_inputs(cache_dir, groundtruth_dir, baseline_dir, insertion_order):
    cache_dir = Path(cache_dir)
    groundtruth_dir = Path(groundtruth_dir)
    baseline_dir = Path(baseline_dir)
    cache_manifest = json_load(cache_dir / "manifest.json")
    gt_manifest = json_load(groundtruth_dir / "groundtruth.manifest.json")
    record_count = int(cache_manifest["params"]["record_count"])
    query_count = int(cache_manifest["params"]["query_count"])
    dim = int(cache_manifest["params"]["dim"])
    if Path(cache_dir / "embeddings.f32").stat().st_size != record_count * dim * 4:
        fail("record embedding shape mismatch")
    if Path(cache_dir / "query_embeddings.f32").stat().st_size != query_count * dim * 4:
        fail("query embedding shape mismatch")
    outputs = cache_manifest["outputs"]
    for key, filename in [("embeddings_f32", "embeddings.f32"),
                          ("query_embeddings_f32", "query_embeddings.f32"),
                          ("records_jsonl", "records.jsonl"),
                          ("queries_jsonl", "queries.jsonl")]:
        if sha256_file(cache_dir / filename) != outputs[key]["sha256"]:
            fail(f"cache checksum mismatch: {filename}")
    gt_path = groundtruth_dir / "groundtruth.jsonl"
    if sha256_file(gt_path) != gt_manifest["outputs"]["groundtruth_jsonl"]["sha256"]:
        fail("ground truth checksum mismatch")
    order = [int(line) for line in Path(insertion_order).read_text().splitlines() if line]
    if len(order) != record_count or sorted(order) != list(range(record_count)):
        fail("insertion order is not a full record-row permutation")
    baseline = json_load(baseline_dir / "core_hnsw_v2_results.json")
    if baseline["params"]["record_count"] != record_count:
        fail("index-only baseline record count mismatch")
    groundtruth = []
    with open(gt_path, "r", encoding="utf-8") as stream:
        for line in stream:
            row = json.loads(line)
            groundtruth.append([int(item["record_id"]) for item in row["neighbors"]])
    if len(groundtruth) != query_count or any(len(row) != 10 for row in groundtruth):
        fail("ground truth query/topK mismatch")
    return cache_manifest, gt_manifest, baseline, order, groundtruth


def build_jsonl_offsets(path, expected_rows):
    offsets = []
    offset = 0
    with open(path, "rb") as stream:
        for line in stream:
            offsets.append(offset)
            offset += len(line)
    if len(offsets) != expected_rows:
        fail(f"JSONL row count mismatch for {path}: {len(offsets)}")
    return offsets


def read_jsonl_at(stream, offsets, row):
    stream.seek(offsets[row])
    return json.loads(stream.readline())


def parse_hit(item):
    if not isinstance(item, str):
        fail("VSEARCH hit is not a bulk string")
    score_match = re.match(r"score:([^|]+)\|", item)
    row_match = re.search(r"\|question:__kvlite_bench_record__:(\d+):", item)
    if not score_match or not row_match:
        fail("VSEARCH response does not preserve benchmark record identity")
    score = float(score_match.group(1))
    if not math.isfinite(score):
        fail("nonfinite VSEARCH score")
    return {"record_id": int(row_match.group(1)), "score": score}


def run_query(conn, query_text, scope, topk):
    start = time.perf_counter_ns()
    response = conn.command("VSEARCH", query_text, scope, str(topk), "-1")
    elapsed_us = (time.perf_counter_ns() - start) / 1000.0
    if not isinstance(response, list):
        fail("VSEARCH did not return an array")
    return elapsed_us, [parse_hit(item) for item in response]


def evaluate_hits(hits, exact):
    ids = [item["record_id"] for item in hits]
    duplicate_count = len(ids) - len(set(ids))
    order_violations = sum(1 for left, right in zip(hits, hits[1:])
                           if left["score"] < right["score"])
    recall = len(set(ids) & set(exact)) / len(exact)
    return ids, duplicate_count, order_violations, recall


def run_sweep(conn, mode, queries, groundtruth, ef_values, warmup_rounds,
              measured_rounds, scope, control_file, per_query_stream):
    summaries = []
    for ef in ef_values:
        Path(control_file).write_text(f"{ef}\n", encoding="ascii")
        query_texts = []
        for row, query in enumerate(queries):
            if mode == "prepared":
                query_texts.append(f"{QUERY_PREFIX}{row}:{query['qid']}\n{query['text']}")
            else:
                query_texts.append(query["text"])
        for _ in range(warmup_rounds):
            for query_text in query_texts:
                run_query(conn, query_text, scope, 10)
        before = parse_vinfo(conn.command("VINFO"))
        latencies = []
        recalls = []
        top1_matches = 0
        duplicate_count = 0
        missing_query_count = 0
        nonfinite_count = 0
        order_violation_count = 0
        returned_id_digest = hashlib.sha256()
        for round_index in range(measured_rounds):
            for query_row, query_text in enumerate(query_texts):
                latency_us, hits = run_query(conn, query_text, scope, 10)
                ids, duplicates, violations, recall = evaluate_hits(
                    hits, groundtruth[query_row])
                latencies.append(latency_us)
                recalls.append(recall)
                top1_matches += int(bool(ids) and ids[0] == groundtruth[query_row][0])
                duplicate_count += duplicates
                order_violation_count += violations
                missing_query_count += int(len(hits) != 10)
                nonfinite_count += sum(not math.isfinite(item["score"]) for item in hits)
                returned_id_digest.update((",".join(str(value) for value in ids) + "\n").encode())
                row = {
                    "mode": mode, "ef_search": ef, "round": round_index,
                    "query_row": query_row, "latency_us": round(latency_us, 3),
                    "recall_at_10": recall, "hits": hits,
                }
                per_query_stream.write(json.dumps(row, ensure_ascii=False, separators=(",", ":")) + "\n")
        after = parse_vinfo(conn.command("VINFO"))
        latency = summarize_latencies(latencies)
        latency.update({
            "mode": mode,
            "ef_search": ef,
            "recall_at_1": round(top1_matches / len(latencies), 6),
            "recall_at_10": round(statistics.mean(recalls), 6),
            "minimum_per_query_recall": round(min(recalls), 6),
            "query_count": len(queries),
            "measured_rounds": measured_rounds,
            "hits_per_query": 10,
            "duplicate_id_count": duplicate_count,
            "missing_query_count": missing_query_count,
            "nonfinite_score_count": nonfinite_count,
            "score_order_violation_count": order_violation_count,
            "returned_ids_sha256": returned_id_digest.hexdigest(),
            "vinfo_delta": vinfo_delta(before, after),
        })
        summaries.append(latency)
    return summaries


def parse_time_v(path):
    result = {}
    if not Path(path).exists():
        return result
    text = Path(path).read_text(encoding="utf-8", errors="replace")
    patterns = {
        "user_seconds": r"User time \(seconds\):\s+([0-9.]+)",
        "system_seconds": r"System time \(seconds\):\s+([0-9.]+)",
        "elapsed": r"Elapsed \(wall clock\) time[^:]*:\s+(.+)",
        "maximum_resident_set_kb": r"Maximum resident set size \(kbytes\):\s+(\d+)",
        "exit_status": r"Exit status:\s+(\d+)",
    }
    for key, pattern in patterns.items():
        match = re.search(pattern, text)
        if match:
            value = match.group(1).strip()
            result[key] = float(value) if key.endswith("seconds") else int(value) if value.isdigit() else value
    return result


def make_config(path, port, data_dir, args):
    text = f"""port={port}
sync_port={port + 1000}
role=master
enable_aof=no
aof_fsync_mode=no
aof_filename={data_dir}/kvlite.aof
rdb_filename={data_dir}/kvlite.rdb
dump_filename={data_dir}/kvlite.dat
network_mode=reactor
sync_mode=ebpf
mem_backend=jemalloc
max_value_size=10485760
log_level=info
log_async=no
embedding_provider=local_c
embedding_model_path={Path(args.model_path).resolve()}
embedding_dim=512
embedding_num_threads=1
embedding_max_text_len=1048576
embedding_ort_graph_opt_level=all
embedding_inter_threads=1
embedding_ort_execution_mode=sequential
embedding_worker_pool_enabled=no
embedding_cache_enabled=no
vector_index_type=core_hnsw_v2
vector_metric=ip
vector_distance_backend=auto
vector_hnsw_m=48
vector_hnsw_ef_construction=512
vector_hnsw_ef_search=32
vector_hnsw_initial_capacity={args.record_count}
vector_score_threshold=-1
vector_scope_overfetch_factor=1
"""
    Path(path).write_text(text, encoding="utf-8")


def read_audit(path):
    result = {}
    if not Path(path).exists():
        return result
    for line in Path(path).read_text().splitlines():
        if "=" in line:
            key, value = line.split("=", 1)
            result[key] = int(value)
    return result


def compare_baseline(summaries, baseline):
    index_by_ef = {int(item["ef_search"]): item for item in baseline["sweep"]}
    comparisons = []
    for item in summaries:
        index = index_by_ef[item["ef_search"]]
        internal = item["vinfo_delta"]["server_vsearch_avg_us"]
        comparisons.append({
            "mode": item["mode"], "ef_search": item["ef_search"],
            "index_only_avg_us": index["search_avg_us"],
            "client_e2e_avg_us": item["avg_us"],
            "server_vsearch_avg_us": internal,
            "server_overhead_us": round(internal - index["search_avg_us"], 3),
            "total_product_overhead_us": round(item["avg_us"] - index["search_avg_us"], 3),
            "product_index_only_ratio": round(item["avg_us"] / index["search_avg_us"], 6),
            "recall_delta": round(item["recall_at_10"] - index["recall_at_10"], 6),
        })
    return comparisons


def independent_audit(per_query_path, summaries, groundtruth):
    aggregate = {}
    rows = 0
    with open(per_query_path, "r", encoding="utf-8") as stream:
        for line in stream:
            item = json.loads(line)
            key = (item["mode"], int(item["ef_search"]))
            state = aggregate.setdefault(key, {"recalls": [], "top1": 0, "count": 0})
            ids = [int(hit["record_id"]) for hit in item["hits"]]
            exact = groundtruth[int(item["query_row"])]
            state["recalls"].append(len(set(ids) & set(exact)) / len(exact))
            state["top1"] += int(bool(ids) and ids[0] == exact[0])
            state["count"] += 1
            rows += 1
    for summary in summaries:
        state = aggregate[(summary["mode"], summary["ef_search"])]
        computed = round(statistics.mean(state["recalls"]), 6)
        if computed != summary["recall_at_10"]:
            fail("summary/per-query recall mismatch")
        summary["recall_at_1"] = round(state["top1"] / state["count"], 6)
    return {"per_query_rows": rows, "summary_per_query_recall_match": True,
            "recall_at_1_recomputed": True}


def generate(args):
    output = Path(args.output_dir)
    if output.exists():
        fail(f"output already exists: {output}")
    work = output.with_name(f".{output.name}.tmp.{os.getpid()}")
    if work.exists():
        shutil.rmtree(work)
    work.mkdir(parents=True)
    data_dir = work / "server_data"
    data_dir.mkdir()
    cache_manifest, gt_manifest, baseline, insertion_order, groundtruth = validate_inputs(
        args.cache_dir, args.groundtruth_dir, args.index_baseline_dir, args.insertion_order)
    args.record_count = int(cache_manifest["params"]["record_count"])
    args.query_count = int(cache_manifest["params"]["query_count"])
    if args.expected_record_count and args.record_count != args.expected_record_count:
        fail("unexpected record count")
    if args.expected_query_count and args.query_count != args.expected_query_count:
        fail("unexpected query count")

    records_path = Path(args.cache_dir) / "records.jsonl"
    queries_path = Path(args.cache_dir) / "queries.jsonl"
    offsets = build_jsonl_offsets(records_path, args.record_count)
    queries = [json.loads(line) for line in queries_path.read_text(encoding="utf-8").splitlines()]
    port = args.port or find_free_port()
    config_path = work / "benchmark.conf"
    control_path = work / "ef_search.control"
    audit_path = work / "prepare_path_audit.txt"
    server_log = work / "server.log"
    time_log = work / "server.time.txt"
    make_config(config_path, port, data_dir, args)
    control_path.write_text("32\n", encoding="ascii")
    env = os.environ.copy()
    env.update({
        "KVLITE_BENCHMARK_CACHE_DIR": str(Path(args.cache_dir).resolve()),
        "KVLITE_BENCHMARK_EF_CONTROL_FILE": str(control_path.resolve()),
        "KVLITE_BENCHMARK_AUDIT_FILE": str(audit_path.resolve()),
        "KVLITE_BENCHMARK_HNSW_SEED": args.hnsw_seed,
    })
    server_bin = str(Path(args.server_bin).resolve())
    command = ["/usr/bin/time", "-v", "-o", str(time_log), server_bin,
               "--config", str(config_path)]
    log_stream = open(server_log, "wb")
    process = subprocess.Popen(command, stdout=log_stream, stderr=subprocess.STDOUT,
                               env=env, cwd=data_dir, start_new_session=True)
    server_pid = wait_for_child_pid(process.pid)
    monitor = RssMonitor(server_pid)
    monitor.start()
    startup_start = time.perf_counter()
    per_query_path = work / "product_e2e_per_query.jsonl"
    completed = False
    try:
        wait_for_server(port, process)
        startup_us = int((time.perf_counter() - startup_start) * 1000000)
        conn = RespConnection("127.0.0.1", port)
        load_start = time.perf_counter_ns()
        failures = 0
        with open(records_path, "rb") as records_stream:
            for position, row in enumerate(insertion_order):
                record = read_jsonl_at(records_stream, offsets, row)
                question = f"{RECORD_PREFIX}{row}:{record['pid']}\n{record['text']}"
                response = conn.command("VSET", question, "", args.scope)
                if response != "OK":
                    failures += 1
                if args.progress_every and (position + 1) % args.progress_every == 0:
                    print(f"loaded {position + 1}/{args.record_count}", flush=True)
        load_us = (time.perf_counter_ns() - load_start) // 1000
        ready_start = time.perf_counter_ns()
        loaded_info = parse_vinfo(conn.command("VINFO"))
        index_ready_us = (time.perf_counter_ns() - ready_start) // 1000
        if failures or int_field(loaded_info, "active_records") != args.record_count or \
                int_field(loaded_info, "core_hnsw_v2_size") != args.record_count or \
                int_field(loaded_info, "core_hnsw_v2_active_count") != args.record_count or \
                loaded_info.get("vector_backend") != "core_hnsw_v2":
            fail("product load/index-ready validation failed")
        audit_after_load = read_audit(audit_path)
        if audit_after_load.get("prepared_vset_count") != args.record_count:
            fail("prepared VSET shim count mismatch")

        with open(per_query_path, "w", encoding="utf-8") as per_query_stream:
            prepared = run_sweep(conn, "prepared", queries, groundtruth, args.ef_search_list,
                                 args.warmup_rounds, args.measured_rounds, args.scope,
                                 control_path, per_query_stream)
            per_query_stream.flush()
            os.fsync(per_query_stream.fileno())
            audit_after_7a = read_audit(audit_path)
            expected_per_mode = len(args.ef_search_list) * \
                (args.warmup_rounds + args.measured_rounds) * args.query_count
            if audit_after_7a.get("prepared_vsearch_count") != expected_per_mode or \
                    audit_after_7a.get("public_vsearch_count", 0) != 0:
                fail("B-scale-7A unexpectedly entered public embedding path")
            public = run_sweep(conn, "public_onnx", queries, groundtruth, args.ef_search_list,
                               args.warmup_rounds, args.measured_rounds, args.scope,
                               control_path, per_query_stream)
        audit_after_7b = read_audit(audit_path)
        if audit_after_7b.get("public_vsearch_count") != expected_per_mode:
            fail("B-scale-7B public ONNX path count mismatch")
        summaries = prepared + public
        independent = independent_audit(per_query_path, summaries, groundtruth)
        for item in summaries:
            if item["duplicate_id_count"] or item["missing_query_count"] or \
                    item["nonfinite_score_count"] or item["score_order_violation_count"]:
                fail("product search correctness gate failed")
        final_info = parse_vinfo(conn.command("VINFO"))
        conn.close()
        os.kill(server_pid, signal.SIGTERM)
        try:
            process.wait(timeout=args.shutdown_timeout)
        except subprocess.TimeoutExpired:
            fail("graceful server shutdown timeout")
        if process.returncode != 0:
            fail(f"server exited with code {process.returncode}")
        completed = True
    finally:
        if process.poll() is None:
            os.killpg(process.pid, signal.SIGTERM)
            try:
                process.wait(timeout=10)
            except subprocess.TimeoutExpired:
                os.killpg(process.pid, signal.SIGKILL)
                process.wait()
        else:
            try:
                os.killpg(process.pid, signal.SIGKILL)
            except ProcessLookupError:
                pass
        monitor.stop()
        log_stream.close()
    if not completed:
        fail("benchmark did not complete")

    time_stats = parse_time_v(time_log)
    log_text = server_log.read_text(encoding="utf-8", errors="replace")
    runtime_matches = re.findall(r"runtime_encode_count=(\d+)", log_text)
    runtime_encode_count = int(runtime_matches[-1]) if runtime_matches else -1
    if runtime_encode_count != expected_per_mode:
        fail(f"ONNX runtime encode count mismatch: {runtime_encode_count} != {expected_per_mode}")
    comparisons = compare_baseline(summaries, baseline)
    rdb_path = data_dir / "kvlite.rdb"
    shutdown_rdb = file_info(rdb_path) if rdb_path.exists() else None
    if shutdown_rdb:
        shutdown_rdb["path"] = "server_data/kvlite.rdb"
    result = {
        "schema_version": SCHEMA_VERSION,
        "dataset_id": cache_manifest["dataset_id"],
        "created_at_utc": time.strftime("%Y-%m-%dT%H:%M:%SZ", time.gmtime()),
        "inputs": {
            "cache_dir": str(Path(args.cache_dir).resolve()),
            "cache_manifest_sha256": sha256_file(Path(args.cache_dir) / "manifest.json"),
            "groundtruth_dir": str(Path(args.groundtruth_dir).resolve()),
            "groundtruth_manifest_sha256": sha256_file(Path(args.groundtruth_dir) / "groundtruth.manifest.json"),
            "index_baseline_dir": str(Path(args.index_baseline_dir).resolve()),
            "index_baseline_result_sha256": sha256_file(Path(args.index_baseline_dir) / "core_hnsw_v2_results.json"),
            "insertion_order_sha256": sha256_file(args.insertion_order),
        },
        "params": {
            "record_count": args.record_count, "query_count": args.query_count,
            "dim": 512, "topk": 10, "metric": "ip_normalized",
            "M": 48, "M0": 96, "efConstruction": 512,
            "efSearch_list": args.ef_search_list,
            "warmup_rounds": args.warmup_rounds,
            "measured_rounds": args.measured_rounds,
            "thread_count": 1, "scope": args.scope,
            "hnsw_seed": args.hnsw_seed,
            "product_raw_topk": 42,
        },
        "server": {
            "binary": server_bin, "port": port,
            "startup_us": startup_us, "peak_rss_kb_sampled": monitor.peak_kb,
            "time_v": time_stats, "aof_enabled": False,
            "aof_fsync_mode": "no", "rdb_enabled": True,
            "rdb_path_at_runtime": str(rdb_path),
            "shutdown_rdb": shutdown_rdb,
            "disposable_data_dir_retained": False,
            "replication_connected": False,
        },
        "load": {
            "wall_us": load_us,
            "records_per_second": round(args.record_count * 1000000.0 / load_us, 3),
            "index_ready_check_us": index_ready_us, "failure_count": failures,
            "vinfo": loaded_info,
        },
        "b_scale_7a_prepared": prepared,
        "b_scale_7b_public_onnx": public,
        "prepare_path_audit": audit_after_7b,
        "onnx_runtime_encode_count": runtime_encode_count,
        "index_only_comparison": comparisons,
        "correctness": independent,
        "final_vinfo": final_info,
    }
    result_path = work / "product_e2e_result.json"
    json_dump_atomic(result_path, result)
    shutil.rmtree(data_dir)
    outputs = {}
    for path in [result_path, per_query_path, config_path, audit_path, server_log, time_log]:
        outputs[path.name] = file_info(path)
    manifest = {
        "schema_version": SCHEMA_VERSION,
        "dataset_id": cache_manifest["dataset_id"],
        "inputs": result["inputs"], "params": result["params"], "outputs": outputs,
    }
    json_dump_atomic(work / "product_e2e_manifest.json", manifest)
    validate_directory(work, expected_output=False)
    os.replace(work, output)
    print(json.dumps({"status": "generated", "output_dir": str(output),
                      "load_wall_us": load_us, "server_peak_rss_kb": monitor.peak_kb,
                      "onnx_runtime_encode_count": runtime_encode_count}, sort_keys=True))


def validate_directory(output_dir, expected_output=True):
    output = Path(output_dir)
    manifest = json_load(output / "product_e2e_manifest.json")
    if manifest.get("schema_version") != SCHEMA_VERSION:
        fail("unsupported product E2E schema")
    for name, info in manifest["outputs"].items():
        path = output / name
        if path.stat().st_size != info["size_bytes"] or sha256_file(path) != info["sha256"]:
            fail(f"output checksum mismatch: {name}")
    result = json_load(output / "product_e2e_result.json")
    expected = len(result["params"]["efSearch_list"])
    if len(result["b_scale_7a_prepared"]) != expected or \
            len(result["b_scale_7b_public_onnx"]) != expected:
        fail("incomplete product E2E sweep")
    per_query_expected = expected * result["params"]["measured_rounds"] * \
        result["params"]["query_count"] * 2
    if result["correctness"]["per_query_rows"] != per_query_expected:
        fail("per-query row count mismatch")
    for item in result["b_scale_7a_prepared"] + result["b_scale_7b_public_onnx"]:
        for field in ("recall_at_1", "recall_at_10", "minimum_per_query_recall"):
            if item[field] is None or not 0.0 <= item[field] <= 1.0:
                fail(f"invalid quality metric: {field}")
        if item["count"] != result["params"]["query_count"] * result["params"]["measured_rounds"]:
            fail("latency count mismatch")
        if any(item[name] != 0 for name in ("duplicate_id_count", "missing_query_count",
                                             "nonfinite_score_count", "score_order_violation_count")):
            fail("stored correctness gate failed")
    if expected_output:
        print(json.dumps({"status": "valid", "output_dir": str(output)}, sort_keys=True))


def parse_args():
    parser = argparse.ArgumentParser()
    sub = parser.add_subparsers(dest="command", required=True)
    generate_parser = sub.add_parser("generate")
    generate_parser.add_argument("--cache-dir", required=True)
    generate_parser.add_argument("--groundtruth-dir", required=True)
    generate_parser.add_argument("--index-baseline-dir", required=True)
    generate_parser.add_argument("--insertion-order")
    generate_parser.add_argument("--output-dir", required=True)
    generate_parser.add_argument("--server-bin", default="build/tools/bin/kvstore_vector_product_e2e_benchmark")
    generate_parser.add_argument("--model-path", default="models/bge-small-zh-v1.5/model.onnx")
    generate_parser.add_argument("--ef-search-list", default="32,64,128,256,512,1024")
    generate_parser.add_argument("--warmup-rounds", type=int, default=1)
    generate_parser.add_argument("--measured-rounds", type=int, default=5)
    generate_parser.add_argument("--expected-record-count", type=int, default=100000)
    generate_parser.add_argument("--expected-query-count", type=int, default=200)
    generate_parser.add_argument("--scope", default="t2ranking-b-scale-7")
    generate_parser.add_argument("--hnsw-seed", default=str(0x9D000000 + 100000))
    generate_parser.add_argument("--port", type=int, default=0)
    generate_parser.add_argument("--progress-every", type=int, default=1000)
    generate_parser.add_argument("--shutdown-timeout", type=int, default=600)
    validate_parser = sub.add_parser("validate")
    validate_parser.add_argument("--output-dir", required=True)
    args = parser.parse_args()
    if args.command == "generate":
        args.ef_search_list = [int(value) for value in args.ef_search_list.split(",")]
        if not args.insertion_order:
            args.insertion_order = str(Path(args.index_baseline_dir) / "insertion_order.ids")
        if args.ef_search_list != sorted(set(args.ef_search_list)) or \
                args.warmup_rounds < 0 or args.measured_rounds <= 0:
            parser.error("invalid sweep parameters")
    return args


def main():
    args = parse_args()
    try:
        if args.command == "generate":
            generate(args)
        else:
            validate_directory(args.output_dir)
    except (OSError, ValueError, KeyError, RuntimeError, json.JSONDecodeError) as exc:
        print(f"error: {exc}", file=sys.stderr)
        return 1
    return 0


if __name__ == "__main__":
    sys.exit(main())
