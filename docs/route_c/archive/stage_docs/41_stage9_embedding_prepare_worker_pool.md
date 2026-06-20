# Stage 9: Embedding Prepare Worker Pool

## Background

Stage 9 follows `docs/route_c_perf_arch/09_embedding_prepare_worker_pool_design.md`.
The goal is to move VSET/VSEARCH embedding prepare work, including ONNX
`OrtRun`, out of the single command executor while keeping all visible state
mutation and search work serialized in the same single commit executor.

Core rule:

```text
embedding prepare concurrent, state commit serialized.
```

## Scope

Implemented:

- `kvs_embedding_worker_pool_init/start/submit/stop/destroy/get_stats`.
- A bounded, mutex-protected prepare queue.
- A prepared queue in the command executor.
- VSET/VSEARCH prepare dispatch to worker pool when explicitly enabled.
- Worker completion return to the single executor for execute/post/finalize.
- Stage 9 config, stats, correctness script, perf script, and this document.

Not implemented in this stage:

- No worker access to store, HNSW, TTL, AOF, RDB, or replication state.
- No VSET commit or VSEARCH search inside workers.
- No RESP protocol or VSET/VSEARCH result semantic changes.
- No removal or weakening of `g_kv_data_rwlock`.
- No batch embedding and no per-thread OrtSession/runtime use from workers.
- No multiple command executors, commit ordering layer, or response reorder.
- No HNSW/index optimization and no TTL MOD semantic change.

## Architecture

```text
network poller
  -> RESP parse
  -> command_task
  -> single command executor
     -> no prepare -> execute_sync/post_commit/finalize
     -> need prepare -> embedding prepare worker pool
                         -> prepared task queue
                         -> execute_sync/post_commit/finalize
  -> response_task
  -> original owner poller send
```

The command executor remains the only place that calls:

- `kvs_command_task_execute_sync()`
- `kvs_command_task_post_commit()`
- `kvs_response_task_finalize()`

Workers only call:

- `kvs_command_task_prepare()`

## Prepare Queue

The prepare queue lives in `src/core/kvs_embedding_worker_pool.c`.
It is protected by `pthread_mutex_t` and `pthread_cond_t`, uses
`kvs_command_task_t::queue_next`, and enforces
`embedding_prepare_queue_capacity` / legacy
`embedding_worker_queue_capacity`.

`kvs_embedding_worker_pool_submit()` records enqueue time and increments:

- `submitted_prepare_tasks`
- `prepare_queue_depth`
- `prepare_queue_max_depth`

If the pool is stopped or full, submit fails and the executor falls back to
inline prepare/execute for the current task.

## Prepared Queue

The worker calls `kvs_command_executor_submit_prepared()` after prepare
success or failure. The executor keeps a separate prepared queue and consumes
it before the normal command queue to avoid completed prepare work piling up.

Prepared queue stats include:

- `prepared_queue_depth`
- `prepared_queue_max_depth`
- `prepared_tasks`
- `prepared_wait_*`
- `prepared_task_executed`
- `prepared_task_failed`

## Lifetimes

The network owner still creates a heap `command_task`, owned argv,
`response_task`, and session reference. The executor transfers the session
reference into `response_task` only when finalizing and pushing a response.

For async prepare tasks:

1. Executor submits the task to the worker pool and does not destroy it.
2. Worker prepares and returns the same task to the prepared queue.
3. Executor executes commit/search, post-commit side effects, finalizes the
   response, destroys the task exactly once, and pushes the response.
4. Owner poller sends or drops the response and releases the response session
   reference through the existing owner-aware response path.

If a worker cannot return a prepared task to the executor, it destroys the task,
response task, and session references through the same ownership cleanup helper.

## VSET Flow

Prepare worker:

```text
parse-owned task
  -> kvs_command_task_prepare()
  -> kvs_vector_set_prepare()
  -> task owns prepared vector/question/answer/scope
  -> prepared queue
```

Commit executor:

```text
prepared VSET
  -> kvs_command_task_execute_sync()
  -> kvs_vector_set_commit_prepared()
  -> post_commit AOF/replication
  -> finalize response
```

Existing update semantics are preserved: commit re-checks current vector state
and releases prepared data during task destruction.

## VSEARCH Flow

Prepare worker:

```text
parse-owned task
  -> kvs_command_task_prepare()
  -> kvs_vector_search_prepare()
  -> task owns prepared query vector/scope
  -> prepared queue
```

Commit executor:

```text
prepared VSEARCH
  -> kvs_command_task_execute_sync()
  -> kvs_vector_search_with_prepared()
  -> scope/threshold filtering
  -> finalize response
```

## Failure Path

Prepare failure is stored in `task->prepare_status`, counted in
`failed_prepare_tasks`, and returned to the executor. The executor skips
commit/post-commit and finalizes the error response through the normal response
path.

Prepared queue submit failure increments:

- `prepare_dispatch_failed`
- `prepared_queue_submit_failed`

The worker then safely destroys task, response task, and session references.

## Shutdown

The current shutdown path stops the worker pool before the command executor
drains. Stopping the pool rejects new prepare submissions, wakes workers, lets
taken tasks finish prepare, and returns them to the executor prepared queue.
Remaining queued prepare tasks, if any, are destroyed safely.

Then the executor drains command/prepared queues, flushes response queue, and
the network owner path finishes response cleanup.

## Config

Defaults:

```conf
embedding_worker_pool_enabled = 0
embedding_worker_threads = 1
embedding_prepare_queue_capacity = 1024
```

The legacy name `embedding_worker_queue_capacity` remains accepted and is kept
in sync with `embedding_prepare_queue_capacity`.

CLI supports:

```text
--embedding_worker_pool_enabled <yes|no>
--embedding_worker_threads <n>
--embedding_prepare_queue_capacity <n>
```

## Stats

Worker pool stats include:

- `enabled`
- `submitted_prepare_tasks`
- `completed_prepare_tasks`
- `failed_prepare_tasks`
- `prepare_dispatch_failed`
- `prepare_queue_depth`
- `prepare_queue_max_depth`
- `prepare_queue_wait_count`
- `prepare_queue_wait_total_us`
- `prepare_queue_wait_max_us`
- `prepare_count`
- `prepare_total_us`
- `prepare_max_us`
- `prepared_queue_submit_count`
- `prepared_queue_submit_failed`
- `encode_max_inflight`

Executor stats include:

- `normal_task_executed`
- `prepare_task_submitted`
- `prepared_task_executed`
- `prepared_task_failed`
- `prepared_queue_depth`
- `prepared_queue_max_depth`
- `commit_latency_count`
- `commit_latency_total_us`
- `commit_latency_max_us`

## Tests

Correctness:

```bash
tests/embedding_worker_pool_stage9.sh
```

The script covers disabled worker pool, enabled threads=1/2, reactor,
proactor, VSET/VGET/VSEARCH/VDEL, repeat VSET overwrite, scoped search,
VDEL/search deletion visibility, invalid command error path, pipeline 10/50,
multi-session concurrent VSET, client disconnect before response, fd reuse,
and final stats accounting.

Performance:

```bash
RESULT_DIR=results/stage9_embedding_worker_pool \
tests/embedding_worker_pool_perf_stage9.sh
```

The perf script compares disabled, threads=1, threads=2, and threads=4 for
VSET/VSEARCH latency and throughput c=4/8/16, plus KV SET/GET smoke. It also
extracts max inflight, prepare queue depth, prepared queue depth, and commit
latency stats from server logs.

## Validation Result

Implementation validation:

```text
git diff --check: passed
bash -n tests/embedding_worker_pool_stage9.sh: passed
bash -n tests/embedding_worker_pool_perf_stage9.sh: passed
make embedding ENABLE_EBPF=0: passed
make clean && make embedding ENABLE_EBPF=0: passed
tests/embedding_worker_pool_stage9.sh: passed
RESULT_DIR=results/stage9_embedding_worker_pool tests/embedding_worker_pool_perf_stage9.sh: passed
```

Correctness covered reactor/proactor with worker pool disabled, threads=1,
and threads=2. Worker stats satisfied:

```text
submitted == completed + failed + dispatch_failed
prepared_task_executed > 0
prepare_queue_depth == 0
prepared_queue_depth == 0
```

The performance run wrote results under
`results/stage9_embedding_worker_pool`. With the current shared provider path,
`encode_max_inflight` stayed at 1 and VSET QPS did not materially improve.
VSEARCH throughput showed a small improvement in threads=1/2 runs, but
provider serialization remained the dominant limit.

ASAN/UBSAN and TSAN status should be recorded after sanitizer runs in the
target environment.

Sanitizer notes from this run:

- ASAN/UBSAN build passed.
- `ASAN_OPTIONS=detect_leaks=0 ... tests/embedding_worker_pool_stage9.sh`
  passed.
- `detect_leaks=1` completed functional checks but LeakSanitizer reported
  shutdown leaks from existing network/vector/store lifetime paths, including
  reactor/proactor connection allocations, RESP argv copies, HNSW/vector store
  allocations, and root store structures. No UAF or UBSAN error was observed.
- TSAN build passed. `TSAN_OPTIONS="halt_on_error=0 exitcode=0"
  tests/embedding_worker_pool_stage9.sh` completed, with the existing
  shutdown/background races in `handle_quit`, `kvs_ttl_background_stop`,
  `distributed_shutdown`, and `sync_backend_shutdown`. No worker queue,
  prepared queue, task ownership, or worker pool stats race was reported.

## Known Limits

- Worker pool is disabled by default.
- Workers share the existing embedding runtime path; provider-level mutex can
  still limit QPS even when `encode_max_inflight > 1`.
- No per-thread OrtSession/runtime is introduced in this stage.
- No batch embedding is used in this stage.
- Commit executor can still be blocked by HNSW search or VSET commit work.

## Next Stage

Recommended next step after correctness and sanitizer clean runs: review
provider-level encode contention and only then decide whether a later stage
should revisit per-thread OrtSession or batching.
