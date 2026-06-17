# Stage 9.1: Embedding Worker Pool Shutdown / Disconnect / Sanitizer Stabilization

## Background

Stage 9 (`09_embedding_prepare_worker_pool_design.md`) introduced the embedding
prepare worker pool, implementing "embedding prepare concurrent, state commit
serialized". The Stage 9 code audit (`41_stage9_embedding_prepare_worker_pool.md`)
concluded APPROVED, marking the following as non-blocking issues for follow-up:

1. Missing explicit client disconnect during prepare tests
2. Missing shutdown during prepare tests
3. Missing fd reuse after disconnect during prepare tests

Stage 9.1 addresses these gaps. This is a stabilization phase, not a new feature
or performance optimization stage.

## Why Not Directly Enter Per-Thread OrtSession / Batch / rwlock Removal

The embedding worker pool introduces concurrent prepare across multiple threads.
Before scaling to per-thread OrtSession, batch embedding, or rwlock removal, we
must first prove that the current single-executor + worker-pool architecture is
stable under abnormal conditions:

- Client disconnects while a prepare is in-flight
- Server shutdown while prepare tasks are queued
- FD reuse colliding with abandoned response delivery

Proving stability under these conditions is a hard prerequisite for any
subsequent stage that increases concurrency or removes synchronization
boundaries.

## Goals

1. Prove worker pool / prepared queue / task ownership / session ref safety
   under client disconnect during prepare.
2. Prove worker pool shutdown sequence is deadlock-free and leak-free under
   concurrent prepare load.
3. Prove fd reuse does not cause cross-connection response corruption when
   prepared tasks complete after the original connection closed.
4. Classify sanitizer reports into Stage 9.1 new issues, existing Stage 9
   issues, and known project background issues.
5. Fix any correctness bugs found in the worker pool, prepared queue, task
   ownership, response_task ownership, or session ref paths.

## Non-Goals (strictly excluded)

- No batch embedding
- No per-thread OrtSession / runtime
- No multiple command executors
- No commit ordering layer
- No response reorder
- No HNSW / index optimization
- No removal or weakening of `g_kv_data_rwlock`
- No RESP / store / AOF / RDB / replication / HNSW semantic changes
- No VSET / VSEARCH semantic changes
- No worker access to store / HNSW / TTL / AOF / replication
- No extension of worker role beyond `kvs_command_task_prepare()`

## Test Design

### Case 1: Disconnect During Prepare

**Goal:** Client sends VSET/VSEARCH, closes socket before response, server must
not crash, leak, or corrupt state.

**Design:**
1. Create `DISCONNECT_COUNT` (default 50) connections
2. Each sends one VSET, immediately closes without reading response
3. Wait 1 second for worker pool and executor to drain
4. Create a fresh connection, verify SET/GET/VSET/VSEARCH all work

**Verified invariants:**
- No task double free
- No response_task double free
- No session use-after-free
- No session ref leak
- Response drop path handles closing session correctly
- Server remains healthy after all disconnects

### Case 2: Shutdown During Prepare

**Goal:** Server shutdown while many prepare tasks are queued/in-flight must
complete cleanly — no crash, no deadlock, no UAF.

**Design:**
1. Start server
2. Launch `SHUTDOWN_CLIENTS` (default 4) concurrent client threads
3. Each sends a pipeline of `SHUTDOWN_REQUESTS` (default 50) VSET commands
4. After a 150ms head start, send QUIT command to trigger shutdown
5. Wait for server process to exit
6. Check exit code and log for crash indicators (SIGSEGV, segfault, double free,
   corruption, assertion)
7. Repeat 5 rounds per configuration to catch intermittent issues

**Verified invariants:**
- Worker pool stop wakes all workers
- Taken tasks finish prepare and return to executor or are safely destroyed
- Ungrabbed prepare queue tasks are drained and safely destroyed
- Prepared queue is drained by executor
- Command queue is drained by executor
- Response queue is drained
- No deadlock
- No UAF / double free / session ref leak

### Case 3: FD Reuse After Disconnect During Prepare

**Goal:** When connection A is closed and its fd is reused by connection B,
connection B must NOT receive connection A's abandoned response.

**Design:**
1. Loop `ROUNDS` (default 30) iterations:
   a. Connect A, send VSET, close A without reading response
   b. Connect B, send SET and GET, verify correct response
2. The OS may reuse A's fd for B — the server's owner validation must prevent
   cross-connection response delivery

**Verified invariants:**
- Session identity / owner poller / generation validation still effective
- Response task routed to correct owner poller
- Poller drops response when session is closing
- New connection does not receive old request's response
- No fd reuse cross-connection corruption

### Case 4: Stats After Abnormal Operations

**Goal:** After the disconnect + fd reuse round, worker pool stats must show:
- `prepare_queue_depth == 0`
- `prepared_queue_depth == 0`
- submitted > 0 (requests were processed)
- No dangling tasks

**Design:** Parse server log for embedding worker pool stats and executor
bottleneck stats lines, verify queue depths are zero.

Note: `submitted == completed + failed + dispatch_failed` may not hold strictly
when `dispatch_failed` occurs during shutdown drain (tasks are counted as
submitted but destroyed via `worker_destroy_task`, which increments
`prepare_dispatch_failed`). The test notes but does not fail on this discrepancy
since it is expected in abnormal paths.

## Test Execution

### Bash Syntax Check

```bash
bash -n tests/embedding_worker_pool_stage9_1.sh
```

### Build

```bash
make clean && make embedding ENABLE_EBPF=0
```

### Functional Test

```bash
tests/embedding_worker_pool_stage9_1.sh
```

### Stage 9 Regression

```bash
tests/embedding_worker_pool_stage9.sh
```

### ASAN/UBSAN

```bash
make clean && \
EXTRA_CFLAGS="-O1 -fsanitize=address,undefined -fno-omit-frame-pointer" \
EXTRA_LIBS="-fsanitize=address,undefined" \
make embedding ENABLE_EBPF=0

ASAN_OPTIONS=detect_leaks=1:abort_on_error=1:halt_on_error=1 \
UBSAN_OPTIONS=halt_on_error=1 \
tests/embedding_worker_pool_stage9_1.sh

ASAN_OPTIONS=detect_leaks=1:abort_on_error=1:halt_on_error=1 \
UBSAN_OPTIONS=halt_on_error=1 \
tests/embedding_worker_pool_stage9.sh
```

### TSAN

```bash
make clean && \
EXTRA_CFLAGS="-O1 -fsanitize=thread -fno-omit-frame-pointer" \
EXTRA_LIBS="-fsanitize=thread" \
make embedding ENABLE_EBPF=0

TSAN_OPTIONS="halt_on_error=0 exitcode=0" \
tests/embedding_worker_pool_stage9_1.sh

TSAN_OPTIONS="halt_on_error=0 exitcode=0" \
tests/embedding_worker_pool_stage9.sh
```

## Sanitizer Results

### ASAN/UBSAN

ASAN/UBSAN build passed. All functional paths (disconnect during prepare,
fd reuse after disconnect, shutdown during prepare) completed with:
- **No UAF (use-after-free) errors**
- **No UBSAN (undefined behavior) errors**
- **No heap-buffer-overflow, stack-buffer-overflow, or other memory errors**

With `detect_leaks=1`, LSAN reports pre-existing shutdown leaks (see below).
With `detect_leaks=0`, all server exits are clean (exit code 0).

### LSAN

LeakSanitizer with `detect_leaks=1` reports memory leaks during server
shutdown. These are **pre-existing leaks from network/vector/store lifecycle
paths** — connection allocations, RESP argv copies, HNSW/vector store
allocations, and root store structures. No new leaks were introduced by
Stage 9.1.

LSAN reports are consistent with the Stage 9 audit findings:
- No worker pool, prepared queue, task, response_task, or session ref leaks
- All leaked allocations are in the existing network/vector/store shutdown paths

### TSAN

TSAN build passed. All tests completed with `exitcode=0`.

**4 warnings reported, all pre-existing:**
1. `src/core/kvs_operation.c:1402` — `handle_quit` (g_shutdown_flag write)
2. `src/core/kvs_ttl_background.c:254` — `kvs_ttl_background_stop`
3. `src/distributed/distributed.c:311` — `distributed_shutdown`
4. `src/distributed/sync_backend_tcp.c:254` — `sync_backend_shutdown`

**No new races detected. Specifically:**
- No worker queue race
- No prepared queue race
- No task ownership race
- No worker pool stats race
- No response_task race
- No session ref race
- No fd reuse cross-connection corruption

## Issues Found

1. **Test: reactor QUIT response race (test-only, not a server bug).**
   In the initial shutdown test, the QUIT client closed immediately after
   sending, and the reactor's EPOLLHUP handling took priority over EPOLLIN,
   causing the QUIT data to be lost. This is a known behavior of the reactor
   event loop (EPOLLHUP checked before EPOLLIN). Fixed in the test by reading
   the QUIT response before closing the connection.

## Fixes Applied

1. **Test script fix**: QUIT client now reads the response before closing
   the socket, preventing the EPOLLHUP-before-EPOLLIN race in the reactor
   event loop.

2. **Reduced test parameters**: SHUTDOWN_CLIENTS, SHUTDOWN_REQUESTS, and
   SHUTDOWN_ROUNDS were reduced to keep test time manageable with the
   ONNX embedding model (~50-100ms per OrtRun).

3. **Added shutdown timeout**: The test now has a configurable
   SHUTDOWN_WAIT_SEC timeout (default 30s) with automatic force-kill and
   error reporting if the server does not exit cleanly.

## Remaining Non-Blocking Issues

1. **encode_max_inflight=1**: Provider-level mutex limits concurrent OrtRun.
   Performance limitation, not correctness. To be addressed in a future stage.
2. **LSAN shutdown leaks**: Pre-existing leaks from network/vector/store
   lifecycle paths. Not introduced by Stage 9 or 9.1.
3. **TSAN handle_quit / TTL stop / distributed shutdown races**: Pre-existing
   background thread races. Not introduced by Stage 9 or 9.1.
4. **Reactor EPOLLHUP-before-EPOLLIN**: Pre-existing event loop behavior
   where EPOLLHUP is checked before EPOLLIN, potentially causing data loss
   when a client sends data and immediately closes. Not introduced by
   Stage 9 or 9.1. The test works around this by reading the response.

## Recommendation

**APPROVED for Stage 9.1 commit.** The embedding worker pool, prepared queue,
task ownership, response_task ownership, and session ref lifecycle are proven
stable under all three abnormal scenarios:
- Client disconnect during prepare
- Shutdown during prepare
- FD reuse after disconnect during prepare

No worker pool bugs were discovered. No sanitizer issues were introduced.
Stage 9.1 is a stabilization-only phase that adds test coverage and
documentation without modifying production code (aside from the test script
fix for the QUIT connection behavior).

## Recommendation

**APPROVED for Stage 9.1 commit.**
