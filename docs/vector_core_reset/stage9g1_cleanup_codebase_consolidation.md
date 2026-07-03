# Stage 9G1-cleanup: codebase consolidation and obsolete profiling cleanup

## AGENTS.md compliance

已读取并遵守根目录 `AGENTS.md`。本阶段与 `AGENTS.md` 无冲突。

本阶段只做 cleanup / consolidation：未把 `core_hnsw_v2` 切为默认 backend，未修改 RDB/AOF 格式，未修改 `kvs_vector_index_ops_t`，未修改 VSEARCH response，未修改 current `core_hnsw` 算法，未让 v2 core 访问 manager state。

## Modified / added files

Stage 9G1 本阶段修改：

- `Makefile`
- `include/kvs_config.h`
- `include/kvs_vector.h`
- `src/vector/kvs_vector.c`
- `tests/README.md`
- `docs/vector_core_reset/优化步骤.md`
- `docs/vector_core_reset/执行步骤书.md`
- `docs/vector_core_reset/stage9g1_cleanup_codebase_consolidation.md`

删除：

- `tests/vcmd_profile_bench.c`
- `tests/vector_core_hnsw_rebuild_profile_test.sh`

工作树中仍包含 Stage 9G0 未提交改动：

- `include/vector/kvs_vector_manager.h`
- `src/core/kvs_config.c`
- `src/core/kvs_operation.c`
- `docs/vector_core_reset/stage9g0_experimental_core_hnsw_v2_product_integration.md`
- `tests/vector_core_hnsw_v2_product_integration_test.c`
- `tests/vector_core_hnsw_v2_product_integration_test.sh`

## Production code changed

生产代码只有注释 / 文档定位变更：

- `include/kvs_config.h`：注明 current `core_hnsw` 是 legacy regression/reference/fallback，`core_hnsw_v2` 仍需 experimental flag。
- `include/kvs_vector.h`：把 current `core_hnsw` VINFO 字段标记为 legacy。
- `src/vector/kvs_vector.c`：把 current `core_hnsw` wrapper 注释标记为 legacy；把 flat 注释收敛为 exact baseline / unit-test oracle。

没有改 current `core_hnsw` 算法、v2 graph/search 算法、manager write/read behavior、RDB/AOF、ops 签名或 VSEARCH response。

## Current call chain

默认路径不变：

```text
vector_hnsw_v2_experimental_enabled=0
  -> existing configured product backend
  -> current core_hnsw / hnswlib / existing default behavior
```

experimental v2 路径不变：

```text
vector_index_type=core_hnsw_v2
vector_hnsw_v2_experimental_enabled=yes
  -> manager-owned product wrapper
  -> core_hnsw_v2 record_id/vector-only API
  -> manager resolve/filter/response assembly
```

## Before behavior

默认 `make` / `make tests` 会构建旧 Stage 7/8/9 profiling artifacts：

- `tests/vcmd_profile_bench.c`
- v2 performance / distance / reverse-prune profile binaries

同时，早期路线文档仍容易被误读为 current `core_hnsw` 是后续优化主线。

## After design

Makefile 分组收敛为：

- default tests: correctness / product integration / persistence regression gates
- optional profile: expensive v2 profiling / attribution binaries
- legacy regression: current `core_hnsw` correctness/rebuild/RDB/AOF gates

新增显式目标：

```bash
make optional-profile-tests
make hnsw_v2_performance_profile
make hnsw_v2_distance_profile
make hnsw_v2_reverse_prune_profile
```

默认 `make` / `make tests` 不再构建 v2 三个 profile binaries，也不再构建旧 `vcmd_profile_bench`。

## Deleted profiling

删除：

- `tests/vcmd_profile_bench.c`
  - 历史 Stage 7/8/current-core HNSW direct-vector profiling harness。
  - 当前 required gates、Makefile 和 `AGENTS.md` 已无引用。
- `tests/vector_core_hnsw_rebuild_profile_test.sh`
  - 历史 current `core_hnsw` rebuild profiling machine-check 脚本。
  - 当前 9G1 required tests 改为保留 correctness/rebuild/RDB/AOF legacy regression，不再保留 current-core profile-only gate。

确认：

```text
rg "vcmd_profile_bench|vector_core_hnsw_rebuild_profile_test" Makefile tests AGENTS.md
no output
```

## Retained profiling

保留为 optional profile：

- `tests/vector_core_hnsw_v2_performance_profile_test.{c,sh}`
- `tests/vector_core_hnsw_v2_distance_profile_test.{c,sh}`
- `tests/vector_core_hnsw_v2_reverse_prune_profile_test.{c,sh}`

理由：它们仍是 v2 product hardening 前可复用的 attribution tools，但不应污染默认 build/test path。

保留 VSEARCH/VINFO 低开销观测字段。current `core_hnsw` 的 VINFO profiling counters 未在本阶段移除，因为它们已经进入 public VINFO schema；删除会扩大兼容风险。本阶段只删除明显一次性 harness，并把 current `core_hnsw` 定位降级为 legacy。

## Gate lists

core_flat baseline:

- `tests/vector_core_flat_correctness_test.sh`
- `tests/vector_core_flat_rebuild_test.sh`
- `tests/vector_core_flat_rdb_reload_test.sh`

legacy current `core_hnsw`:

- `tests/vector_core_hnsw_correctness_test.sh`
- `tests/vector_core_hnsw_rebuild_test.sh`
- `tests/vector_core_hnsw_rdb_reload_test.sh`
- `tests/vector_core_hnsw_aof_replay_test.sh`

core_hnsw_v2 required gates:

- `tests/vector_core_hnsw_v2_tombstone_test.sh`
- `tests/vector_core_hnsw_v2_skeleton_test.sh`
- `tests/vector_core_hnsw_v2_graph_construction_test.sh`
- `tests/vector_core_hnsw_v2_search_recall_test.sh`
- `tests/vector_core_hnsw_v2_product_integration_test.sh`

manager-owned rebuild harness:

- `tests/vector_rebuild_state_machine_test.sh`
- `tests/vector_rebuild_delta_capture_test.sh`
- `tests/vector_rebuild_shadow_replay_test.sh`
- `tests/vector_rebuild_shadow_swap_test.sh`

## Historical docs

Added a status note to:

- `docs/vector_core_reset/优化步骤.md`
- `docs/vector_core_reset/执行步骤书.md`

The historical content was not rewritten. The new note says their current `core_hnsw` production/optimization route is superseded: `core_flat` is exact baseline, current `core_hnsw` is legacy regression/reference/fallback, and `core_hnsw_v2` is the experimental future ANN backend until a later default-switch stage.

## Safety invariants

Confirmed unchanged:

- default product behavior
- VSEARCH response format
- RDB/AOF format
- `kvs_vector_index_ops_t`
- current `core_hnsw` algorithm
- `core_flat` baseline
- `core_hnsw_v2` required code and tests
- v2 manager/core boundary

## Build result

```text
make clean
make ENABLE_HNSWLIB=1 ENABLE_FAISS=1 -j"$(nproc)"
PASS
```

Additional compile check:

```text
make ENABLE_HNSWLIB=1 ENABLE_FAISS=1 optional-profile-tests -j"$(nproc)"
PASS
```

## Correctness tests

```text
tests/vector_core_flat_correctness_test.sh PASS
tests/vector_core_flat_rebuild_test.sh PASS
tests/vector_core_flat_rdb_reload_test.sh PASS

tests/vector_core_hnsw_correctness_test.sh PASS
tests/vector_core_hnsw_rebuild_test.sh PASS
tests/vector_core_hnsw_rdb_reload_test.sh PASS
tests/vector_core_hnsw_aof_replay_test.sh PASS

tests/vector_core_hnsw_v2_tombstone_test.sh PASS
tests/vector_core_hnsw_v2_skeleton_test.sh PASS
tests/vector_core_hnsw_v2_graph_construction_test.sh PASS
tests/vector_core_hnsw_v2_search_recall_test.sh PASS
tests/vector_core_hnsw_v2_product_integration_test.sh PASS

tests/vector_rebuild_state_machine_test.sh PASS
tests/vector_rebuild_delta_capture_test.sh PASS
tests/vector_rebuild_shadow_replay_test.sh PASS
tests/vector_rebuild_shadow_swap_test.sh PASS
```

Key v2 recall gate from this run:

```text
10K/512d ef=256 recall@10 = 0.9860
10K/512d ef=512 recall@10 = 1.0000
```

## Static boundary checks

```text
grep -RIn "g_vector_state" src/vector/core/hnsw_v2 include/vector/kvs_vector_hnsw_v2.h || true
no output

grep -RIn "record_id_map" src/vector/core/hnsw_v2 include/vector/kvs_vector_hnsw_v2.h || true
no output

grep -RIn "scope\|ttl\|metadata\|question\|answer" src/vector/core/hnsw_v2 include/vector/kvs_vector_hnsw_v2.h || true
no output
```

Temporary debug grep:

```text
grep -RIn "DBG\|debug after\|after_update\|TODO.*temporary\|temporary profile" src include tests || true
src/eBPF/vmlinux.h:2965: FIX_DBGP_BASE = 512,
src/eBPF/vmlinux.h:46205: SOCK_DBG = 10,
```

These are generated kernel symbol names in `src/eBPF/vmlinux.h`, not temporary project debug/probe code.

`git diff --check`:

```text
PASS
```

## git status --short

```text
 M Makefile
 M docs/vector_core_reset/优化步骤.md
 M docs/vector_core_reset/执行步骤书.md
 M include/kvs_config.h
 M include/kvs_vector.h
 M include/vector/kvs_vector_manager.h
 M src/core/kvs_config.c
 M src/core/kvs_operation.c
 M src/vector/kvs_vector.c
 M tests/README.md
 D tests/vcmd_profile_bench.c
 D tests/vector_core_hnsw_rebuild_profile_test.sh
?? docs/vector_core_reset/stage9g0_experimental_core_hnsw_v2_product_integration.md
?? docs/vector_core_reset/stage9g1_cleanup_codebase_consolidation.md
?? tests/vector_core_hnsw_v2_product_integration_test.c
?? tests/vector_core_hnsw_v2_product_integration_test.sh
```

## Next suggested step

Stage 9G1 cleanup succeeded. Next stage may enter:

```text
Stage 9G2: default backend switch hardening
```

Do not directly do SIMD/layout/reverse-prune optimization here. Do not switch v2 to default outside a dedicated default-switch hardening stage.
