# Stage 9B: core_hnsw_v2 index-only skeleton

## 1. AGENTS.md compliance

已读取并遵守项目根目录 `AGENTS.md`。

本阶段只新增独立 `core_hnsw_v2` index-only skeleton 和对应离线测试。未实现 add/search，未接入 VINDEX/VSEARCH，未修改 current `core_hnsw` 算法，未修改 RDB/AOF，未修改 `kvs_vector_index_ops_t`，未修改 VSEARCH response，未让 vector_core 访问 `g_vector_state` 或 manager 语义字段，未处理既有无关 `README.md` 修改。

本 prompt 要求新增 v2 skeleton；这是 Stage 9A/9A1 后的明确路线，与 `AGENTS.md` 的 manager/core 边界、RDB/AOF derived-state 规则和 stage discipline 不冲突。

## 2. Modified / added files

本阶段修改：

- `Makefile`

本阶段新增：

- `include/vector/kvs_vector_hnsw_v2.h`
- `src/vector/core/hnsw_v2/kvs_hnsw_v2_index.c`
- `src/vector/core/hnsw_v2/kvs_hnsw_v2_storage.c`
- `src/vector/core/hnsw_v2/kvs_hnsw_v2_context.c`
- `src/vector/core/hnsw_v2/kvs_hnsw_v2_info.c`
- `tests/vector_core_hnsw_v2_skeleton_test.c`
- `tests/vector_core_hnsw_v2_skeleton_test.sh`
- `docs/vector_core_reset/stage9b_core_hnsw_v2_index_only_skeleton.md`

工作树中仍有此前 Stage 7/8/9 报告和代码改动，以及既有无关 `README.md` 修改；本阶段未回退或处理这些无关项。

## 3. Whether production code changed

新增了 v2 production-source skeleton 文件，但未接入生产 backend：

- 未新增 `KVS_VECTOR_INDEX_TYPE_CORE_HNSW_V2`。
- 未注册 `kvs_vector_index_ops_t`。
- 未修改 VINDEX/VSEARCH/rebuild 生产路径。
- `kvstore` 主二进制不选择、不调用 v2。
- `Makefile` 只把 v2 skeleton 源直接链接进 `vector_core_hnsw_v2_skeleton_test` 测试目标。

## 4. Current call chain

Stage 9B v2 只存在 index-only harness：

```text
tests/vector_core_hnsw_v2_skeleton_test.sh
  -> build/tests/bin/vector_core_hnsw_v2_skeleton_test
  -> kvs_hnsw_v2_index_init()
  -> kvs_hnsw_v2_index_reserve()
  -> kvs_hnsw_v2_index_info()
  -> kvs_hnsw_v2_index_reset()
  -> kvs_hnsw_v2_index_destroy()
```

current `core_hnsw` call chain 未变：

```text
VINDEX / VSEARCH / rebuild
  -> current core_hnsw ops
  -> src/vector/core/hnsw/*
```

## 5. Before behavior

Stage 9A/9A1 后没有独立 `core_hnsw_v2` implementation directory。下一阶段如果直接修改 current `core_hnsw`，会继续污染 legacy backend / regression reference。

## 6. After design

新增独立目录：

```text
include/vector/kvs_vector_hnsw_v2.h
src/vector/core/hnsw_v2/
  kvs_hnsw_v2_index.c
  kvs_hnsw_v2_storage.c
  kvs_hnsw_v2_context.c
  kvs_hnsw_v2_info.c
```

v2 index object 当前字段：

- params: `dim`, `metric`, `M`, `M0`, `ef_construction`, `ef_search`
- lifecycle: `capacity`, `size`, `ready`, `needs_rebuild`, `deleted_count`
- vector storage: `vector_stride`, `vectors`
- id/slot: `slot_to_record_id`, v2-local `record_id_to_slot`
- slot state: `levels`, `deleted_flags`, `visited_tags`, `current_visit_tag`
- entry state: `entry_point`, `has_entry`, `max_level`
- graph placeholder: `level0_neighbors`, `level0_counts`, `upper_offsets`, `upper_neighbors`, `upper_counts`
- context placeholder: `context_pool`
- memory accounting: `kvs_hnsw_v2_memory_t memory`

本阶段没有 add/search/random-level/neighbor-select/bidirectional-link 语义。

## 7. Memory ownership

v2 owns:

- aligned vector rows
- `slot_to_record_id`
- v2-local `record_id_to_slot`
- `levels`
- `deleted_flags`
- `visited_tags`
- level0 graph placeholder
- upper graph placeholder metadata
- context placeholder

Manager still owns:

- record store
- manager-owned `record_id_map`
- scope / TTL / metadata / question / answer
- AOF/RDB/replication/rebuild orchestration
- response formatting

RDB/AOF still save records, not v2 graph.

## 8. Aligned vector storage strategy

v2 uses 64-byte vector base alignment via `posix_memalign`.

`vector_stride` pads rows to a 64-byte float boundary:

```text
vector_stride = align_up(dim, 64 / sizeof(float))
vectors_bytes = capacity * vector_stride * sizeof(float)
```

Logical `dim` remains unchanged and does not leak to manager. The skeleton test verifies `vectors % 64 == 0`.

## 9. Graph placeholder strategy

Level0 placeholder:

```text
level0_neighbors: capacity * M0 uint32_t
level0_counts: capacity uint16_t
```

Upper placeholder:

```text
upper_offsets: capacity size_t
upper_counts: capacity uint16_t
upper_neighbors: NULL in Stage 9B
```

Reason: Stage 9B must establish lifecycle and accounting only. Upper graph allocation policy should be decided in Stage 9C when random level / add / graph-construction requirements are implemented.

## 10. Search context placeholder strategy

`kvs_hnsw_v2_context_pool_t` currently reserves a minimal placeholder buffer:

```text
count = KVS_HNSW_V2_DEFAULT_CONTEXT_COUNT
bytes = count * sizeof(uint64_t)
```

No candidate heap, top heap, visited list, or result buffer is implemented in Stage 9B.

## 11. Lifecycle behavior

`init`:

- validates non-null index and non-zero dim.
- sets defaults: `M=32`, `M0=64`, `ef_construction=128`, `ef_search=256`.
- initializes entry/search state and memory accounting.

`reserve`:

- rejects zero capacity.
- allocates all capacity-dependent skeleton arrays.
- preserves custom `M/M0/ef_construction/ef_search` set after `init`.
- uses 64-byte aligned vector rows.
- repeated reserve with smaller/equal capacity is a no-op.
- larger reserve replaces storage and resets runtime state.

`reset`:

- preserves capacity and allocated storage.
- clears vectors, id-slot map, levels, deleted flags, visited tags, level0 placeholders, upper counts, context placeholder.
- resets size / deleted_count / entry_point / has_entry / max_level / current_visit_tag.

`destroy`:

- releases all v2-owned memory.
- clears the index object to zero.

`info`:

- fills `kvs_vector_core_info_t` with capacity, size, dim, metric, ready, needs_rebuild, deleted_count, total memory bytes.

## 12. Memory breakdown

Stage 9B tracks:

| field | formula |
| --- | --- |
| vectors bytes | `capacity * vector_stride * sizeof(float)` |
| slot_to_record_id bytes | `capacity * sizeof(uint64_t)` |
| record_id_to_slot bytes | `record_id_to_slot.capacity * sizeof(kvs_vector_id_slot_entry_t)` |
| levels bytes | `capacity * sizeof(uint8_t)` |
| deleted_flags bytes | `capacity * sizeof(uint8_t)` |
| visited_tags bytes | `capacity * sizeof(uint32_t)` |
| level0 graph bytes | `capacity * M0 * sizeof(uint32_t) + capacity * sizeof(uint16_t)` |
| upper graph bytes | `capacity * sizeof(size_t) + capacity * sizeof(uint16_t)` |
| context bytes | `KVS_HNSW_V2_DEFAULT_CONTEXT_COUNT * sizeof(uint64_t)` |
| total memory bytes | sum of all fields above |

Example covered by skeleton test:

```text
dim=17
M=24
M0=48
capacity=128
vector_stride=32
vectors pointer 64-byte aligned
```

## 13. Safety invariants

- v2 directory contains no `g_vector_state`.
- v2 directory contains no manager-owned `record_id_map`.
- v2 directory contains no scope / TTL / metadata / question / answer handling.
- v2 does not register as a production backend.
- v2 does not implement add/search/mark_delete graph semantics in Stage 9B.
- current `core_hnsw`, `core_flat`, FAISS, and hnswlib remain available.
- RDB/AOF compatibility is unchanged.
- VSEARCH response format is unchanged.

## 14. Correctness tests

Passed:

```bash
tests/vector_core_hnsw_v2_skeleton_test.sh
tests/vector_core_hnsw_correctness_test.sh
tests/vector_core_hnsw_rebuild_test.sh
tests/vector_core_hnsw_rdb_reload_test.sh
tests/vector_core_hnsw_aof_replay_test.sh
```

Results:

```text
vector_core_hnsw_v2_skeleton_test PASS
vector_core_hnsw_correctness_test PASS
vector_core_hnsw_rebuild_test PASS
vector_core_hnsw_rdb_reload_test PASS
vector_core_hnsw_aof_replay_test PASS
```

## 15. Build result

Passed:

```bash
make ENABLE_HNSWLIB=1 ENABLE_FAISS=1 -j"$(nproc)"
```

Result:

```text
✓ 构建完成
二进制: kvstore src/eBPF/ebpf_syncd
eBPF:   src/eBPF/ebpf_kern.o
```

## 16. Static boundary check result

Passed:

```bash
grep -RIn "g_vector_state" src/vector/core/hnsw_v2 include/vector/kvs_vector_hnsw_v2.h || true
grep -RIn "record_id_map" src/vector/core/hnsw_v2 include/vector/kvs_vector_hnsw_v2.h || true
grep -RIn "scope\\|ttl\\|metadata\\|question\\|answer" src/vector/core/hnsw_v2 include/vector/kvs_vector_hnsw_v2.h || true
git diff --check
```

Results:

- `g_vector_state`: no output
- `record_id_map`: no output
- manager semantic grep: no output
- `git diff --check`: PASS

## 17. git status --short

```text
 M Makefile
 M README.md
 M include/kvs_vector.h
 M include/vector/kvs_vector_hnsw.h
 M src/core/kvs_operation.c
 M src/vector/core/hnsw/kvs_hnsw_add.c
 M src/vector/core/hnsw/kvs_hnsw_index.c
 M src/vector/core/hnsw/kvs_hnsw_info.c
 M src/vector/kvs_vector.c
 M src/vector/kvs_vector_distance_avx2.c
 M tests/vcmd_profile_bench.c
 M tests/vector_core_hnsw_rebuild_profile_test.sh
?? "docs/vector_core_reset/hnsw_v2\345\256\236\346\226\275\346\226\271\346\241\210.md"
?? docs/vector_core_reset/stage7e_neighbor_select_score_reuse_optimization.md
?? docs/vector_core_reset/stage7f_highdim_layer_search_audit.md
?? docs/vector_core_reset/stage7g_layer_search_distance_alignment_optimization.md
?? docs/vector_core_reset/stage7h_level0_neighbor_visited_layout_audit.md
?? docs/vector_core_reset/stage7i_level0_visited_slot_locality_prototype.md
?? docs/vector_core_reset/stage7j_add_search_slot_state_locality_prototype.md
?? docs/vector_core_reset/stage7k_remove_failed_layout_prototypes.md
?? docs/vector_core_reset/stage7l_post_layout_layer_search_hotspot_audit.md
?? docs/vector_core_reset/stage8a_core_flat_hnsw_faiss_parity_architecture_audit.md
?? docs/vector_core_reset/stage9a1_core_hnsw_bidirectional_link_acceptance_audit.md
?? docs/vector_core_reset/stage9a_core_hnsw_v2_design_boundary.md
?? include/vector/kvs_vector_hnsw_v2.h
?? src/vector/core/hnsw_v2/
?? tests/vector_core_hnsw_v2_skeleton_test.c
?? tests/vector_core_hnsw_v2_skeleton_test.sh
```

## 18. Next suggested step

Stage 9C should only implement:

- random level generation;
- add path;
- graph construction;
- graph quality stats.

Stage 9C should still not implement search and should not connect v2 to VINDEX/VSEARCH until index-only add/graph quality gates are validated.
