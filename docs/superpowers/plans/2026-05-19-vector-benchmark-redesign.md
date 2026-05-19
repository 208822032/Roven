# Vector Benchmark Redesign Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Replace multiple vector benchmark tools with a single unified `tests/vector_benchmark.c` supporting latency and throughput modes.

**Architecture:** Single C binary with two execution modes. Latency mode uses 1 connection, serial send/recv, measures per-request timing. Throughput mode uses N non-blocking connections with epoll, pipeline depth P, measures server saturation QPS. Both modes share RESP parsing, key generation, and statistics computation. The shell script `bench_vector_rw.sh` becomes a thin wrapper that invokes the C binary for each op.

**Tech Stack:** C (gcc), POSIX sockets, epoll, RESP protocol parsing

---

### Task 1: Create tests/vector_benchmark.c

**Files:**
- Create: `tests/vector_benchmark.c`

This is the core deliverable. The file is ~800 lines. Key components:

1. Command-line parsing (getopt_long)
2. RESP command builder (reuse pattern from vector_import_bench.c `send_resp_command`)
3. RESP parser (reuse pattern from vector_testcase.c `parse_resp_object`)
4. Latency mode: single connection, serial loop
5. Throughput mode: N connections, epoll event loop, pipeline ring buffers
6. Prepare phase: single-connection serial VSET
7. Statistics: percentile calculation, key=value output
8. Key generation: `<prefix>_question_%08d` / `<prefix>_answer_%08d`

### Task 2: Modify Makefile

**Files:**
- Modify: `Makefile`

Add `TEST_VECTOR_BENCHMARK` variable and build rule. Add to `TEST_BINARIES` and `clean`.

### Task 3: Rewrite scripts/bench_vector_rw.sh

**Files:**
- Modify: `scripts/bench_vector_rw.sh`

Replace Python client calls with `tests/vector_benchmark` invocations. Support `BENCH_MODE` env var. Update CSV header and summary format.

### Task 4: Build and verify

**Files:** None (verification only)

Run `make clean && make`, then small-scale latency and throughput tests.
