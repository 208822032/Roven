# Stage 9F5: active/shadow swap skeleton without VSEARCH behavior change

## AGENTS.md compliance

已读取并遵守根目录 `AGENTS.md`。

本阶段只实现 manager-owned rebuild context 内部 active/shadow/retired pointer lifecycle skeleton。未注册 `core_hnsw_v2` 为产品 backend，未改变 VSEARCH/VINDEX runtime behavior，未修改 VSEARCH response，未修改 RDB/AOF，未修改 `kvs_vector_index_ops_t`，未修改 current `core_hnsw` 算法行为，未实现 background thread，未新增用户命令或配置项。

本 prompt 与 `AGENTS.md` 无硬边界冲突。`AGENTS.md` 中早期 Stage 5 推荐路线是历史基线；本阶段按用户提供的 Stage 9F5 路线执行，符合后续 v2 stage discipline。

## Modified / added files

本阶段修改：

- `Makefile`
- `include/vector/kvs_vector_rebuild.h`
- `src/vector/kvs_vector_rebuild.c`
- `include/vector/kvs_vector_rebuild_shadow.h`
- `src/vector/kvs_vector_rebuild_shadow.c`

本阶段新增：

- `tests/vector_rebuild_shadow_swap_test.c`
- `tests/vector_rebuild_shadow_swap_test.sh`
- `docs/vector_core_reset/stage9f5_active_shadow_swap_skeleton.md`

生产代码有变更，但仅限 manager-owned rebuild lifecycle skeleton 和 test-only shadow swap harness。没有拆出 `kvs_vector_rebuild_swap.{h,c}`，因为现有 `kvs_vector_rebuild.{h,c}` 已经承载状态机和 opaque pointer，占用独立文件会增加接口面但不降低复杂度。

## Runtime behavior guard

确认保持不变：

- 未注册 `core_hnsw_v2` backend。
- VSEARCH runtime behavior 未改变。
- VINDEX runtime behavior 未改变。
- VSEARCH response 未改变。
- RDB/AOF 未修改。
- `kvs_vector_index_ops_t` 未修改。
- current `core_hnsw` 算法代码未修改。
- VSEARCH 不读取 rebuild context `active_index`。
- VSEARCH 不读取 `shadow_index`。
- VINDEX 不消费 rebuild context active/shadow/retired。
- vector_core/v2 未访问 `g_vector_state`、manager `record_id_map`、scope/TTL/metadata/question/answer。

## Current call chain

产品路径仍然是：

```text
VSET / VDEL / VSEARCH / VINDEX
  -> existing manager/current_index path
  -> current product backend
```

9F5 test-only lifecycle path 是：

```text
kvs_vector_rebuild_shadow_v2_init_for_test()
  -> BUILDING
  -> shadow_index = new v2 index

kvs_vector_rebuild_shadow_v2_build_from_active_records_for_test()
  -> REPLAYING_DELTA

kvs_vector_rebuild_shadow_v2_replay_deltas_for_test()
  -> READY_TO_SWAP

kvs_vector_rebuild_swap_ready_shadow_for_test()
  -> kvs_vector_rebuild_promote_shadow()
  -> READY_TO_SWAP -> SWAPPING -> IDLE
  -> shadow_index moves to rebuild.active_index
  -> old rebuild.active_index moves to retired_index
```

## Swap skeleton API

新增/扩展 internal API：

```c
typedef void (*kvs_vector_rebuild_index_destroy_fn)(void *index);

int kvs_vector_rebuild_promote_shadow(kvs_vector_rebuild_context_t *ctx);
int kvs_vector_rebuild_retire_cleanup(kvs_vector_rebuild_context_t *ctx);

int kvs_vector_rebuild_swap_ready_shadow_for_test(void);
int kvs_vector_rebuild_retire_cleanup_for_test(void);
int kvs_vector_rebuild_force_swap_failure_for_test(void);
```

`kvs_vector_rebuild_promote_shadow()` 是 no-fail pointer move section：所有 validation 在 publication 前完成，pointer move 后不执行可能失败的操作。

## Active / shadow / retired ownership

`kvs_vector_rebuild_context_t` 现在为 active/shadow/retired 各自保存 destroy callback：

```text
active_index + active_destroy_fn
shadow_index + shadow_destroy_fn
retired_index + retired_destroy_fn
```

ownership 规则：

- `shadow_index` 是 rebuild-owned v2 index，有 destroy callback。
- swap 后 `shadow_index -> active_index`，destroy callback 同步迁移。
- old `active_index -> retired_index`。
- 如果 old active 是产品 `current_index` placeholder，没有 destroy callback，因此 retired cleanup 只清 rebuild pointer，不释放产品索引。
- 如果 active 是前一次 promote 后的 rebuild-owned v2，下一次 begin/reset/destroy 会通过 callback 释放，避免泄漏。

## READY_TO_SWAP -> SWAPPING -> IDLE

成功路径：

```text
READY_TO_SWAP
  validate shadow_index != NULL
  validate retired_index == NULL
  -> SWAPPING
     old active_index -> retired_index
     shadow_index -> active_index
     shadow_index = NULL
  -> IDLE
     clear delta log
     keep rebuild.active_index for lifecycle tests
     keep retired_index until explicit cleanup
```

非 READY 状态不能 swap。新增测试覆盖 `IDLE`、`BUILDING`、`REPLAYING_DELTA`、`FAILED`、`CANCELLED` 到 swap 均失败且状态不被错误推进。

## Retired delayed destroy

9F5 不实现真实 reader refcount。delayed destroy 用显式 cleanup API 表达：

```text
kvs_vector_rebuild_retire_cleanup()
  -> if retired_destroy_fn exists, destroy retired index
  -> retired_index = NULL
  -> retired_destroy_fn = NULL
```

repeated cleanup 幂等。`reset_for_load`、`destroy`、FAILED/CANCELLED reset 都会清理 active/shadow/retired 和 delta log。

## Rollback / failure design

失败路径分两类：

1. READY_TO_SWAP 前失败：
   - `active_index` 不变；
   - `shadow_index` 被 destroy；
   - `retired_index` 不变；
   - state -> `FAILED`，保留 `error_code/error_message`。

2. SWAPPING 中失败注入：
   - test helper 先进入 `SWAPPING`，但在 pointer publication 前强制 fail；
   - `active_index` 保持旧值；
   - `shadow_index` 被 destroy；
   - 不产生 retired；
   - state -> `FAILED`。

报告结论：真实 product swap 必须把 validation 放在 publication 前，publication 本身保持 no-fail pointer move，避免 publication 后 rollback 窗口。

## Delta log policy

成功 swap 后清理 delta log。

理由：`READY_TO_SWAP` 表示 shadow 已经从 baseline build 并按 generation replay 完当前 delta。promote 成功后这些 delta 已经被吸收进新的 rebuild active，后续 rebuild 应从新的 generation boundary 开始。保留旧 delta 只会增加内存生命周期和重复 replay 风险。

## VSEARCH behavior guard

新增测试使用 shadow-only UPSERT delta：

```text
record_id=9000001 只 append 到 rebuild delta log
不写入 record store
不写入 product current_index
replay 后 shadow/rebuild-active 可以搜索到该 record_id
swap 后产品 VSEARCH 查询同向量仍返回 product current_index 中的真实 record
```

这证明 swap skeleton 后 VSEARCH 仍读取产品 `current_index`，不读取 rebuild context `active_index` 或 `shadow_index`。

VINDEX guard：swap 后执行 `kvs_vector_index_rebuild()` 仍成功，且 rebuild-active v2 中的 shadow-only record 仍可由 test helper 搜索到，说明 VINDEX 没消费或替换 rebuild context active。

## Why not product publication

本阶段不让 VSEARCH 读 rebuild active，因为这会同时引入 reader concurrency、fallback、VINFO observability、RDB/AOF replay、response correctness 和 product config 风险。9F5 只验证 pointer lifecycle，不混入 query semantics。

本阶段不注册 v2 backend，因为 product backend exposure 需要 disabled feature flag、VINFO status、fallback、reader-safety gate 和 command-level integration tests。

本阶段不做 background thread，因为并发 scan/capture/replay/swap 需要锁边界和 generation ordering policy。9F5 只验证 serial lifecycle skeleton。

本阶段不替换 product `current_index`，因为产品读路径仍应稳定使用现有 backend，直到 reader safety 和 observability 完成。

## Reader safety options

未来 product swap 至少需要以下方案之一：

|方案|优点|代价|
|---|---|---|
|RW lock|实现简单，swap 持写锁，reader 持读锁，old index 可在无 reader 时释放|高并发 VSEARCH 下锁竞争明显，swap 可能等待长 reader|
|Refcount|每个 reader acquire/release index，retired index refcount 到 0 后释放|需要所有 reader 严格配对，错误路径必须可靠 release|
|Epoch/RCU-style|读路径开销低，适合 read-heavy index publication|实现复杂，需要 epoch advance、quiescent state 和 retired list 管理|

9F5 skeleton 没发现 reader safety 阻塞 pointer lifecycle，但真正让 VSEARCH 读取 rebuild active 前必须先选定 reader lifetime 方案。

## Tests

Build：

```text
make ENABLE_HNSWLIB=1 ENABLE_FAISS=1 -j"$(nproc)" PASS
```

Stage 9F tests：

```text
tests/vector_rebuild_state_machine_test.sh PASS
tests/vector_rebuild_delta_capture_test.sh PASS
tests/vector_rebuild_shadow_replay_test.sh PASS
tests/vector_rebuild_shadow_swap_test.sh PASS
```

v2 tests：

```text
tests/vector_core_hnsw_v2_tombstone_test.sh PASS
tests/vector_core_hnsw_v2_skeleton_test.sh PASS
tests/vector_core_hnsw_v2_graph_construction_test.sh PASS
tests/vector_core_hnsw_v2_search_recall_test.sh PASS
tests/vector_core_hnsw_v2_performance_profile_test.sh PASS
tests/vector_core_hnsw_v2_distance_profile_test.sh PASS
tests/vector_core_hnsw_v2_reverse_prune_profile_test.sh PASS
```

Observed v2 recall gate during this run:

```text
1K/512d ef=256  recall@10=1.0000
5K/512d ef=256  recall@10=1.0000
10K/512d ef=64  recall@10=0.6800
10K/512d ef=128 recall@10=0.8840
10K/512d ef=256 recall@10=0.9860
10K/512d ef=512 recall@10=1.0000
10K/512d ef=1024 recall@10=1.0000
```

current core_hnsw regression：

```text
tests/vector_core_hnsw_correctness_test.sh PASS
tests/vector_core_hnsw_rebuild_test.sh PASS
tests/vector_core_hnsw_rdb_reload_test.sh PASS
tests/vector_core_hnsw_aof_replay_test.sh PASS
```

## Boundary checks

All produced no output:

```bash
grep -RIn "g_vector_state" src/vector/core/hnsw_v2 include/vector/kvs_vector_hnsw_v2.h || true
grep -RIn "record_id_map" src/vector/core/hnsw_v2 include/vector/kvs_vector_hnsw_v2.h || true
grep -RIn "scope\|ttl\|metadata\|question\|answer" src/vector/core/hnsw_v2 include/vector/kvs_vector_hnsw_v2.h || true
```

`git diff --check`: PASS.

## Blockers found

No 9F5 lifecycle blocker found.

Remaining blockers before product publication:

- no reader lifetime protection for real concurrent VSEARCH;
- rebuild state is still not user/admin observable;
- no disabled feature flag or v2 backend registration;
- no background worker locking/generation policy;
- no command-level product swap integration.

## Stage 9F6 recommendation

Recommended next:

```text
Stage 9F6: VINFO rebuild status exposure
```

Reason:

- 9F1-9F5 now cover state machine, delta capture, shadow build/replay, and swap lifecycle skeleton.
- The rebuild lifecycle remains invisible to operators/tests outside internal harness.
- Before background worker or backend registration, expose manager-owned rebuild status through VINFO or equivalent internal info without changing VSEARCH response and without letting VSEARCH use v2.
- Reader safety remains mandatory before product publication, but it did not block the 9F5 skeleton itself.

Do not proceed directly to v2 backend registration or product current_index replacement.

## git status --short

Captured after implementation and validation:

```text
 M AGENTS.md
 M Makefile
 M README.md
 M include/kvs_vector.h
 M include/vector/kvs_vector_hnsw.h
 M include/vector/kvs_vector_manager.h
 M src/core/kvs_operation.c
 M src/vector/core/hnsw/kvs_hnsw_add.c
 M src/vector/core/hnsw/kvs_hnsw_index.c
 M src/vector/core/hnsw/kvs_hnsw_info.c
 M src/vector/kvs_vector.c
 M src/vector/kvs_vector_distance_avx2.c
 M tests/vcmd_profile_bench.c
 M tests/vector_core_hnsw_rebuild_profile_test.sh
?? docs/vector_core_reset/stage9f5_active_shadow_swap_skeleton.md
?? include/vector/kvs_vector_hnsw_v2.h
?? include/vector/kvs_vector_rebuild.h
?? include/vector/kvs_vector_rebuild_shadow.h
?? src/vector/core/hnsw_v2/
?? src/vector/kvs_vector_rebuild.c
?? src/vector/kvs_vector_rebuild_shadow.c
?? tests/vector_rebuild_shadow_swap_test.c
?? tests/vector_rebuild_shadow_swap_test.sh
```

The repository also contains prior untracked Stage 7/8/9 reports and v2/rebuild tests from earlier stages; they were not reverted.
