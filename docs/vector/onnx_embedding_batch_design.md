# ONNX Embedding Micro-Batch Design

## 1. Current Single-Request Path

Current `VSET` / `VSEARCH` embedding flow is single-item:

1. Normalize request text at command layer.
2. Tokenize one text.
3. Build one ONNX input tensor set.
4. Run `embedding_encode()` for one sample.
5. Read one `sentence_embedding`.
6. L2-normalize one vector.
7. Return embedding to vector prepare path.

This keeps semantics simple but forces one ONNX invocation per request. The previous benchmark stage showed this is the dominant cost.

## 2. Goal

Micro-batch embedding should improve throughput without changing `VSET` / `VSEARCH` protocol semantics:

- Keep request/response semantics unchanged.
- Preserve per-request failure reporting.
- Bound added queueing latency.
- Prefer `VSEARCH` latency over `VSET` throughput.

## 3. Batch Queue Structure

Recommended first implementation:

```c
typedef struct kvs_embedding_batch_item {
    kvs_command_task_t *task;
    uint64_t enqueue_us;
    int is_vsearch;
    int canceled;
    struct kvs_embedding_batch_item *next;
} kvs_embedding_batch_item_t;

typedef struct kvs_embedding_batch_queue {
    pthread_mutex_t mu;
    pthread_cond_t cv;
    kvs_embedding_batch_item_t *search_head;
    kvs_embedding_batch_item_t *search_tail;
    kvs_embedding_batch_item_t *vset_head;
    kvs_embedding_batch_item_t *vset_tail;
    size_t size;
    int stopping;
} kvs_embedding_batch_queue_t;
```

Two queues are simpler than a priority heap and are enough for `VSEARCH` priority.

## 4. Request Completion Mechanism

Each queued request should keep its existing prepared-task completion flow:

- Worker thread or batch thread owns the embedding step.
- On batch completion, each item writes its own embedding into `prepared_set` or `prepared_search`.
- The same task-level callback / prepared-submit path is reused.

No protocol-level promise object is required if the existing `kvs_command_task_t` lifecycle is preserved.

## 5. Batch Formation

Recommended initial policy:

- `embedding_batch_enabled = yes|no`
- `embedding_batch_max_size = 8`
- `embedding_batch_max_wait_us = 2000`
- `embedding_batch_search_priority = yes|no`

Batch assembly rules:

1. Prefer draining `VSEARCH` queue first.
2. If first queued item is `VSEARCH`, fill from `VSEARCH` queue up to `max_batch_size`.
3. If no `VSEARCH` exists, drain `VSET`.
4. If oldest item waits longer than `max_wait_us`, dispatch partial batch.

## 6. Why `max_batch_size = 8`

The current dataset and latency target suggest a conservative batch:

- Small enough to keep tail latency bounded.
- Large enough to amortize tokenizer / tensor-build / ONNX launch overhead.
- Easier to roll back if p95 regresses.

Recommended starting values:

- `batch_size = 4` for latency-sensitive environments
- `batch_size = 8` for throughput-biased experiments

## 7. VSEARCH Priority

Priority rule:

- `VSEARCH` should bypass queued `VSET` work whenever possible.
- `VSET` can absorb queueing delay better because it is write-path work.

Implementation rule:

- Always check `search_head` first when forming a batch.
- Allow optional mixed batches later only if measurements show no regression.

## 8. Shutdown and Drain

Shutdown behavior should be explicit:

1. Mark queue as stopping.
2. Wake batch thread.
3. Drain remaining queued items.
4. Complete queued tasks with either:
   - normal embedding result, or
   - deterministic shutdown error if runtime is already unavailable.

Do not silently drop queued embedding tasks.

## 9. Timeout and Cancellation

Required handling:

- If task is canceled before batch dispatch, mark item canceled and skip it.
- If client disconnects after dispatch, finish compute but skip response handoff when existing command lifecycle already supports that.
- If wait time exceeds configured upper bound because runtime is stalled, emit timeout metrics and fail requests deterministically.

## 10. Relation to Existing Worker Pool

Recommended layering:

- Keep existing embedding worker pool as the outer concurrency boundary.
- Add micro-batching inside embedding workers, not above command executor.

Reasoning:

- Avoid a second unbounded queue at executor level.
- Preserve current `prepare -> submit prepared` contract.
- Make rollback easier: disable batching but keep worker pool.

## 11. Avoiding CPU Oversubscription

The key control variable is:

```text
active_embedding_workers * onnx_intra_threads
```

Guidelines:

- Do not scale `embedding_workers` and `onnx_intra_threads` independently.
- Prefer `workers=1..2` before increasing `intra_threads`.
- Keep batch thread count small and fixed.
- Use sequential ORT execution mode first.

Recommended rollout order:

1. `workers=1, intra=1, batch=4`
2. `workers=1, intra=2, batch=4`
3. `workers=2, intra=1, batch=4`

## 12. Metrics

New metrics should include:

- `embedding_batch_dispatch_count`
- `embedding_batch_item_count`
- `embedding_batch_size_p50/p95/max`
- `embedding_batch_wait_us_total/max`
- `embedding_batch_run_us_total/max`
- `embedding_batch_end_to_end_us_total/max`

Per-request stats should still report:

- `embedding_total_us`
- `onnx_run_us`
- `batch_wait_us`

## 13. Rollback Strategy

Rollback must be one config flip:

- `embedding_batch_enabled = no`

No data migration should be required. All batch-only state is in-memory and transient.

## 14. Recommendation

Based on the current benchmark result, micro-batching is worth implementing after exact cache because:

- cache removes repeated-query cost;
- batching can still reduce cold-query ONNX cost;
- it does not require HNSW or protocol changes.

Recommended first rollout:

- `embedding_batch_enabled = no` by default
- `embedding_batch_max_size = 4`
- `embedding_batch_max_wait_us = 1000`
- `embedding_batch_search_priority = yes`
