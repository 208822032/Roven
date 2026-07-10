# V-family implementation audit + real embedding benchmark readiness report

## 1. AGENTS.md 遵守情况

已读取并遵守仓库根目录 `AGENTS.md`。

冲突说明：`AGENTS.md` 的历史阶段模板要求阶段报告写入 `docs/vector_core_reset/`；本任务明确要求新增 `docs/vector_benchmark_readiness/`。本报告按任务指定目录输出，同时继续遵守 AGENTS 的核心边界：不修改生产代码、不修改测试、不修改 Makefile、不修改 RDB/AOF 主格式、不修改 `VSEARCH` response、不修改 `kvs_vector_index_ops_t`、不让 vector core 访问 manager-owned 语义状态。

本轮是 read-only audit + docs-only report；没有运行功能测试，也没有验证新增功能。

## 2. 本轮任务范围

目标是判断当前 V 族实现是否已经可以进入下一阶段压测对比：

```text
引入 models/ 下真实 embedding 模型
对比 core_hnsw_v2 与 FAISS
同时做 core 层对比和端到端 VSET / VSEARCH 对比
```

本轮没有实现 benchmark harness，没有新增压测代码，没有修改生产代码、测试或 Makefile。

## 3. 当前 HNSW-S / HNSW-R 前置状态

HNSW-S 已完成 snapshot cold-start 最小闭环：

```text
records load / replay
  -> validate vector_index.manifest + vector_index.snapshot
  -> valid: load core_hnsw_v2 graph cache
  -> invalid / missing / corrupt: records-only rebuild
  -> rebuild success: best-effort write next snapshot
  -> VINFO exposes load / fallback / write status
```

HNSW-S bench 已记录 100K / 512d balanced load-only repeat=5：

```text
records_only_rebuild_us = 3,011,810,795
snapshot_load_us_avg = 5,923,515.60
snapshot_load_us_p95 = 6,803,620
speedup = 508.45x
loaded_from_snapshot = 1
fallback_rebuild_count = 0
```

HNSW-R 已完成最小 product background rebuild 闭环：

```text
state machine hardening
reader safety rwlock
product active / shadow / retired publish
delta capture generation boundary
shadow full scan + delta replay + final validation
async rebuild worker / command
post-publish best-effort snapshot writer hook
```

这意味着：真实 embedding benchmark 不需要先继续做 HNSW-S/R 功能开发，但需要一个独立 benchmark harness，把 embedding、core index、command path 和 rebuild/snapshot 指标拆开。

## 4. V 族文件清单与逐文件职责分析

### Public API / manager headers

| 文件路径 | 层级 | 主要职责 | V 命令关系 | backend 关系 | benchmark 影响 | 风险 / benchmark 前是否需改 |
| --- | --- | --- | --- | --- | --- | --- |
| `include/kvs_vector.h` | public API | V 族状态码、backend enum、VINFO 结构、prepared VSET/VSEARCH、batch embedding、sync/async rebuild API | 直接定义 VSET/VDEL/VSEARCH/VINDEX/VINFO 对外 C API | enum 保留 removed compatibility；当前产品值是 `core_flat` / `core_hnsw_v2` | core 与 e2e harness 都会使用；VINFO 已有 profile / snapshot / rebuild 字段 | 不需为 audit 修改；benchmark harness 需注意 FAISS enum 不是 product backend |
| `include/vector/kvs_vector_manager.h` | manager | `g_vector_state`、record store、id map、embedding provider、snapshot status、manager resolve / delta capture API | VSET/VDEL/VSEARCH/VINFO 主状态归属 | 持有 `core_flat_index`、`core_hnsw_v2_index`、`current_index` | e2e 与 product API 压测的主入口状态 | 不需修改；报告确认 core 不应引用该 manager 状态 |
| `include/vector/kvs_vector_record.h` | manager record | record、record_id_map、question/scope id_map、store/free-list API | VSET/VDEL/VGET/VSEARCH record source of truth | backend hit 通过 record_id 回到 manager | 影响 resolve latency、slot reuse correctness、e2e memory | 不需修改；benchmark 要记录 record memory |
| `include/vector/kvs_vector_index_internal.h` | manager/index adapter | `kvs_vector_index_ops_t`、current index guard、active rwlock helpers、metric helper | VSEARCH search_current、VSET add_current、VINDEX rebuild_current | 统一 current index adapter，不包含 FAISS ops | core/e2e 主链路关键 | 不得改 ops 签名；当前足够压测 v2/flat |
| `include/vector/kvs_vector_backend_resolve.h` | config compatibility | backend 名称解析、removed compatibility name 判断 | VINFO backend name、init backend select | 旧 FAISS/hnswlib/core_hnsw 名称回退 | 直接影响 benchmark 配置 | benchmark 前必须显式使用 `core_hnsw_v2`；旧 `hnswlib_hnsw` 配置会落到 `core_flat` |
| `include/vector/kvs_vector_index_snapshot.h` | snapshot manager | manifest schema、read/write/validate、default path | VINFO snapshot status、load/rebuild write | 只支持 `core_hnsw_v2` snapshot | snapshot load latency benchmark 可复用 | 不需修改；benchmark 可复用 S-bench harness |
| `include/vector/kvs_vector_rebuild.h` | rebuild state | rebuild state machine、delta log、active/shadow/retired pointer skeleton | VINDEX rebuild/async status | backend-opaque shadow context | async rebuild benchmark 必需 | 不需修改；已有 R1-R7 覆盖 |
| `include/vector/kvs_vector_rebuild_shadow.h` | rebuild product hook | v2 shadow init/build/replay/publish/cleanup/test hooks | VINDEX ASYNC worker 串联 | shadow 只支持 `core_hnsw_v2` | async rebuild latency / under rebuild VSEARCH benchmark 必需 | 不需修改 |
| `include/vector/kvs_vector_core.h` | vector core API | record_id/vector -> hit/score 的 core ops abstraction | 不接触 V response | `core_flat` / `core_hnsw_v2` 共用 hit/info | core index-only benchmark 基础 | 不需修改 |
| `include/vector/kvs_vector_flat.h` | core | exact flat index API | VSEARCH exact baseline、fallback | `core_flat` | ground truth / recall oracle 可用 | 不需修改 |
| `include/vector/kvs_vector_hnsw_v2.h` | core | v2 graph/search/build state、stats、public core API | product ANN backend | `core_hnsw_v2` | 核心对比对象 | 不需修改 |
| `include/vector/kvs_vector_hnsw_v2_snapshot.h` | core snapshot | v2 serialize/deserialize/basic validate | snapshot load/write 间接影响 VINFO | `core_hnsw_v2` only | snapshot load benchmark 可用 | 不需修改 |
| `include/vector/kvs_vector_heap.h` | core util | generic bounded heap | flat search / helper | backend utility | core search topK correctness | 不需修改 |
| `include/vector/kvs_vector_id_slot_map.h` | core util | core-local record_id -> slot map | 不处理 manager semantics | flat/v2 internal map | stale hit prevention in core | 不需修改 |
| `include/vector/kvs_vector_hnsw_v2_heap.h` | core util | v2 candidate/top heap inline helpers | v2 search/build | `core_hnsw_v2` | core latency/recall | 不需修改 |
| `include/vector/kvs_vector_hnsw_v2_score_cache.h` | core util | v2 reverse-prune score cache | v2 build | `core_hnsw_v2` | build-time attribution | 不需修改 |

### Manager / product implementation

| 文件路径 | 层级 | 主要职责 | V 命令关系 | backend 关系 | benchmark 影响 | 风险 / benchmark 前是否需改 |
| --- | --- | --- | --- | --- | --- | --- |
| `src/vector/kvs_vector.c` | manager/product | global vector lifecycle、embedding prepare/commit、current index adapter、VSET/VDEL/VSEARCH/VINDEX/VINFO、snapshot load/write、async rebuild worker | VSET/VDEL/VSEARCH/VINDEX/VINFO 主链路 | binds `core_flat` 或 `core_hnsw_v2`; removed backend fallback | e2e benchmark 主测对象 | 不需为 audit 修改；benchmark 必须控制 `vector_index_type` 并拆分 embedding/index/response |
| `src/vector/kvs_vector_index.c` | manager record/store | record/string/id_map/free-list/metric helpers | VSET/VDEL/VSEARCH resolve/VINDEX compact | backend add/rebuild 从 manager active records 取 embedding | resolve latency、slot reuse correctness | 不需修改 |
| `src/vector/kvs_vector_backend_resolve.c` | manager config | backend name resolver | init/VINFO | `core_hnsw_v2` default；FAISS/hnswlib names removed -> `core_flat` | 对比压测配置关键 | benchmark 前需修正或覆盖旧 config，不能用 removed name 表示 FAISS |
| `src/vector/kvs_vector_embedding.c` | manager embedding | hash/local_c provider、embedding cache、batch generate、normalization、runtime stats | VSET/VSEARCH prepare | 输出统一进入 current index | 真实 embedding benchmark 关键 | 可用；需要 fixture/cache 设计避免重复模型调用污染 index 耗时 |
| `src/vector/kvs_vector_distance.c` | core/manager shared | scalar/auto distance backend dispatch | core score calculation | flat/v2 metric scorer | core latency attribution | 不需修改 |
| `src/vector/kvs_vector_distance_avx2.c` | core/manager shared | AVX2 dot/L2/cosine kernels | core score calculation | flat/v2 metric scorer | 影响 core search/build latency | 不需修改 |
| `src/vector/kvs_vector_index_snapshot.c` | snapshot manager | manifest parser/writer/validator/checksum/default paths | load/rebuild/snapshot VINFO | `core_hnsw_v2` snapshot metadata | snapshot load benchmark | 不需修改 |
| `src/vector/kvs_vector_rebuild.c` | rebuild | state transition, delta append, failure/cancel/reset/publish pointer state | VINDEX sync/async rebuild state | backend-opaque state machine | async rebuild correctness / status | 不需修改 |
| `src/vector/kvs_vector_rebuild_shadow.c` | rebuild/product | v2 shadow full scan, delta replay, final validation, publish, retired cleanup, snapshot hook | VINDEX ASYNC worker | `core_hnsw_v2` shadow/product active | async rebuild latency and under-rebuild VSEARCH | 不需修改 |

### Vector core implementation

| 文件路径 | 层级 | 主要职责 | V 命令关系 | backend 关系 | benchmark 影响 | 风险 / benchmark 前是否需改 |
| --- | --- | --- | --- | --- | --- | --- |
| `src/vector/core/flat/kvs_flat_index.c` | core | flat lifecycle/reset/info | VINDEX rebuild exact baseline | `core_flat` | ground truth / fallback | 不需修改 |
| `src/vector/core/flat/kvs_flat_storage.c` | core | flat add / tombstone | VSET/VDEL via adapter | `core_flat` | exact build/add latency | 不需修改 |
| `src/vector/core/flat/kvs_flat_search.c` | core | exact scan / topK | VSEARCH exact search | `core_flat` | brute-force oracle and recall baseline | 不需修改 |
| `src/vector/core/flat/kvs_flat_info.c` | core | flat info placeholder | VINFO via adapter | `core_flat` | VINFO backend stats | 不需修改 |
| `src/vector/core/kvs_vector_heap.c` | core util | generic heap implementation | flat/v2 topK helper | shared | search correctness | 不需修改 |
| `src/vector/core/kvs_vector_id_slot_map.c` | core util | core-local id/slot hash map | backend delete/search map | flat/v2 | stale hit safety | 不需修改 |
| `src/vector/core/hnsw_v2/kvs_hnsw_v2_index.c` | core | v2 allocation/reset/reserve/delete/destroy | VSET/VDEL/VINDEX through adapter | `core_hnsw_v2` | build memory/latency | 不需修改 |
| `src/vector/core/hnsw_v2/kvs_hnsw_v2_storage.c` | core | storage unit placeholder/organization | indirect | `core_hnsw_v2` | memory layout context | 不需修改 |
| `src/vector/core/hnsw_v2/kvs_hnsw_v2_context.c` | core | search/build context pool | VSEARCH/v2 add | `core_hnsw_v2` | query allocation overhead | 不需修改 |
| `src/vector/core/hnsw_v2/kvs_hnsw_v2_info.c` | core | info/memory refresh | VINFO | `core_hnsw_v2` | memory stats | 不需修改 |
| `src/vector/core/hnsw_v2/kvs_hnsw_v2_random.c` | core | random level generator/seed | v2 add | `core_hnsw_v2` | reproducibility | 不需修改 |
| `src/vector/core/hnsw_v2/kvs_hnsw_v2_graph.c` | core | graph edge helpers, metric score, graph stats | v2 add/search | `core_hnsw_v2` | recall/build/search | 不需修改 |
| `src/vector/core/hnsw_v2/kvs_hnsw_v2_neighbor_select.c` | core | neighbor selection and score cache use | v2 add | `core_hnsw_v2` | build latency/recall | 不需修改 |
| `src/vector/core/hnsw_v2/kvs_hnsw_v2_add.c` | core | v2 add-time layer search, reverse edge, diagnostics | VSET/VINDEX build | `core_hnsw_v2` | build latency primary path | 不需修改 |
| `src/vector/core/hnsw_v2/kvs_hnsw_v2_search.c` | core | greedy / level0 search, query stats | VSEARCH | `core_hnsw_v2` | search latency/recall | 不需修改 |
| `src/vector/core/hnsw_v2_snapshot/kvs_hnsw_v2_snapshot_format.h` | core snapshot | on-disk snapshot structs/helpers declarations | snapshot cache | `core_hnsw_v2` | load/write size/time | 不需修改 |
| `src/vector/core/hnsw_v2_snapshot/kvs_hnsw_v2_snapshot_io.c` | core snapshot | checksum, file IO, payload sizing | snapshot cache | `core_hnsw_v2` | snapshot IO metrics | 不需修改 |
| `src/vector/core/hnsw_v2_snapshot/kvs_hnsw_v2_snapshot_validate.c` | core snapshot | graph basic validation | final validation/snapshot write | `core_hnsw_v2` | correctness before publish/write | 不需修改 |
| `src/vector/core/hnsw_v2_snapshot/kvs_hnsw_v2_snapshot_serialize.c` | core snapshot | graph serialize | post-rebuild snapshot write | `core_hnsw_v2` | snapshot write benchmark | 不需修改 |
| `src/vector/core/hnsw_v2_snapshot/kvs_hnsw_v2_snapshot_deserialize.c` | core snapshot | graph deserialize + id map rebuild | cold-start load | `core_hnsw_v2` | snapshot load benchmark | 不需修改 |

### Command / executor path

| 文件路径 | 层级 | 主要职责 | V 命令关系 | backend 关系 | benchmark 影响 | 风险 / benchmark 前是否需改 |
| --- | --- | --- | --- | --- | --- | --- |
| `src/core/kvs_operation.c` | command | command table, V command parser, synchronous handlers, VINFO response assembly | parses VSET/VGET/VSEARCH/VDEL/VINDEX/VINFO; emits RESP | calls public vector API | e2e response/latency path | 不改；VSEARCH response 格式必须保持 |
| `src/core/kvs_command_task.c` | command task | async executor task prepare/execute/post_commit; prepared VSET/VSEARCH response path | VSET/VSEARCH prepare outside commit; VINDEX/VINFO via handlers | calls vector API | embedding worker and e2e latency split | 不改；benchmark 要区分 prepare vs commit/search |
| `src/core/kvs_command_executor.c` | executor | command queue, prepared task queue, RDB/AOF snapshot builders, embedding worker dispatch | VSET/VSEARCH may be prepared by worker pool | no direct backend logic | e2e concurrent latency / worker pool stats | 不改 |
| `include/kvs_command_task.h` | command API | task prepared state fields | VSET/VSEARCH prepared state | no backend | e2e profiling context | 不改 |
| `include/kvs_embedding_worker_pool.h` / `src/core/kvs_embedding_worker_pool.c` | embedding worker | prepare queue, worker threads, stats | VSET/VSEARCH embedding prepare offload | no backend | real embedding e2e benchmark critical | 不改；benchmark should report queue wait/prepare stats |

### Embedding / model / config

| 文件路径 | 层级 | 主要职责 | V 命令关系 | backend 关系 | benchmark 影响 | 风险 / benchmark 前是否需改 |
| --- | --- | --- | --- | --- | --- | --- |
| `src/embedding/embedding_runtime.c` | model/hash runtime | deterministic C hash embedding fallback | VSET/VSEARCH when ONNX disabled | feeds current backend | synthetic tests/benchmarks | 不改；不代表 real embedding |
| `src/embedding/embedding_runtime_onnx.c` | model runtime | ONNX Runtime C API loader, vocab, tokenizer, single/batch `sentence_embedding` encode | VSET/VSEARCH local_c provider | feeds current backend | real embedding path | 可用性较好；benchmark 前需确认 build preflight 和 tokenizer semantic quality |
| `include/embedding_c_api.h` | model API | C ABI for embedding runtime | called by vector embedding provider | no backend | harness can reuse batch encode | 不改 |
| `models/bge-small-zh-v1.5/config.json` | model | BGE small zh config, `hidden_size=512` | dimension source | recommends 512d | real embedding dimension | 不改 |
| `models/bge-small-zh-v1.5/model.onnx` / `model.onnx_data` | model | ONNX graph and tensor data | local_c provider | 512d vectors | real embedding available | 不改 |
| `models/bge-small-zh-v1.5/onnx/model.onnx` / `model.onnx_data` | model | duplicate ONNX location | optional | 512d vectors | path consistency risk | 不改 |
| `models/bge-small-zh-v1.5/vocab.txt` / tokenizer JSON files | model tokenizer | vocab/tokenizer metadata | ONNX tokenizer uses `vocab.txt`; JSON files not consumed by C runtime | no backend | semantic quality | 不改 |
| `config/kvstore-embedding.conf` | config | ONNX embedding sample config | VSET/VSEARCH real embedding | currently `vector_index_type=hnswlib_hnsw` | risky for benchmark | benchmark 前必须改为/覆盖 `core_hnsw_v2`;旧名当前会 fallback 到 `core_flat` |
| `third_party/onnxruntime/` | dependency | ONNX Runtime headers/libs | embedding runtime link | no backend | build/runtime prerequisite | 发现 `libonnxruntime.so.1.24.4`，Makefile check expects `libonnxruntime.so`;需要确认 symlink or path |

### Build / benchmark / FAISS artifacts

| 文件路径 | 层级 | 主要职责 | V 命令关系 | backend 关系 | benchmark 影响 | 风险 / benchmark 前是否需改 |
| --- | --- | --- | --- | --- | --- | --- |
| `Makefile` | build | builds kvstore, v2 tests, snapshot/rebuild tests, optional ONNX embedding target | compiles V-family product path | product build excludes FAISS product backend | benchmark build source of truth | 本轮不改；FAISS comparison needs separate harness because product adapter removed |
| `third_party/faiss/` | dependency | vendored FAISS source/build | no product V command binding | baseline library exists | FAISS core comparison possible only via separate runner | `third_party/faiss/build/faiss/libfaiss.a` exists; no current product backend |
| `third_party/hnswlib/` | dependency | vendored hnswlib source | product backend removed | historical baseline only | not target of v2 vs FAISS benchmark except historical context | 不作为新 core design 模板 |
| `scripts/benchmark/gen_vector_benchmark_dataset.py` | benchmark | synthetic float32 dataset/query manifest generator | no V command | index-only fixture | useful for synthetic core baseline | 不是真实 embedding fixture |
| `scripts/bench_vector_core_compare.py` | benchmark | old server-path backend comparison with VINFO/profile parsing | VSET/VSEARCH/VINFO | still lists removed `core_hnsw`/FAISS/hnswlib names | not directly valid for current product backend state | benchmark 前需更新 or replace |
| `scripts/benchmark/run_vector_native_faiss_formal.py` | benchmark | intended native/hnswlib/FAISS formal comparison | server path uses RESP, index-only uses binary vectors | expects missing `tests/vector_index_only_formal.cpp` | currently incomplete | benchmark harness needed |
| `scripts/benchmark/run_hnswlib_vs_faiss_benchmark.py` | benchmark | hnswlib vs FAISS formal runner | server path uses hash embedding | expects missing index runner | historical/partial | not sufficient for core_hnsw_v2 real embedding comparison |
| `scripts/benchmark/run_hnswlib_vs_faiss_full_e2e.py` | benchmark | full e2e external comparison script | VSET/VSEARCH server path | old external backends | partial historical tool | needs audit before reuse |
| `scripts/benchmark/bench_vector_rw.sh` / `vector_rw_client.py` / `kvstore_resp_bench.py` | benchmark | RESP latency/throughput clients, p50/p95/p99 | VSET/VSEARCH/VDEL | backend via server config | useful for e2e latency | needs fixture/cache integration |
| `tests/vector_benchmark.c` | benchmark | C RESP latency/throughput tool | VSET/VGET/VSEARCH/VDEL | backend via server config | useful for e2e smoke | no recall/real embedding fixture support |
| `tests/vector_store_bench.c` / `tests/vector_import_bench.c` | benchmark | store/import microbench | VSET-like storage path | not backend comparison | limited utility | not enough for target benchmark |
| `tests/vector_core_benchmark_compare.sh` | benchmark | wrapper for old Python core compare | VSET/VSEARCH server path | old backend names | likely stale | needs replacement or update |
| `tests/vector_index_snapshot_cold_start_bench.c` / `.sh` | benchmark | snapshot create-fixture/load-only benchmark | snapshot/VINFO | `core_hnsw_v2` | reusable for snapshot load latency | not real embedding benchmark |

### Test coverage files

| 文件路径 | 层级 | 主要职责 | 关系与 benchmark readiness |
| --- | --- | --- | --- |
| `tests/vector_core_flat_correctness_test.sh` | test | core_flat correctness via server | exact baseline smoke; useful but not enough for large recall |
| `tests/vector_core_flat_rebuild_test.sh` | test | core_flat VINDEX rebuild/tombstone cleanup | e2e rebuild baseline |
| `tests/vector_core_flat_rdb_reload_test.sh` | test | core_flat RDB reload | persistence regression only |
| `tests/vector_core_hnsw_v2_skeleton_test.c` / `.sh` | test | v2 lifecycle skeleton | core v2 basic gate |
| `tests/vector_core_hnsw_v2_graph_construction_test.c` / `.sh` | test | v2 graph structure/degree | core build gate |
| `tests/vector_core_hnsw_v2_search_recall_test.c` / `.sh` | test | v2 synthetic exact recall@K | core recall baseline exists |
| `tests/vector_core_hnsw_v2_performance_profile_test.c` / `.sh` | test | v2 performance attribution | optional profile, not product benchmark |
| `tests/vector_core_hnsw_v2_distance_profile_test.c` / `.sh` | test | distance backend attribution, p50/p95/p99, recall | useful for core latency method |
| `tests/vector_core_hnsw_v2_reverse_prune_profile_test.c` / `.sh` | test | reverse prune build attribution | buildperf context |
| `tests/vector_core_hnsw_v2_tombstone_test.c` / `.sh` | test | v2 tombstone correctness | delete/rebuild safety |
| `tests/vector_core_hnsw_v2_tombstone_policy_test.c` / `.sh` | test | tombstone policy gate | rebuild trigger context |
| `tests/vector_core_hnsw_v2_snapshot_roundtrip_test.c` / `.sh` | test | serialize/deserialize roundtrip | snapshot correctness |
| `tests/vector_core_hnsw_v2_product_integration_test.c` / `.sh` | test | product v2 default/integration | e2e v2 smoke |
| `tests/vector_core_hnsw_v2_default_backend_test.c` / `.sh` | test | default backend is v2 | benchmark config guard |
| `tests/vector_core_hnsw_v2_scale_stability_test.c` / `.sh` | test | scale stability | pre-benchmark confidence |
| `tests/vector_core_hnsw_v2_100k_balanced_gate_test.c` / `.sh` | test | 100K balanced gate | heavy baseline, not repeated casually |
| `tests/vector_core_hnsw_v2_parity_baseline_test.c` / `.sh` | test | v2 vs flat synthetic parity profiles | closest current core comparison, but no FAISS |
| `tests/vector_index_snapshot_manifest_test.c` / `.sh` | test | manifest read/write/validate | snapshot fixture safety |
| `tests/vector_index_snapshot_load_after_records_test.c` / `.sh` | test | records load then snapshot load/fallback | cold-start path |
| `tests/vector_index_snapshot_write_after_rebuild_test.c` / `.sh` | test | rebuild writes snapshot | snapshot write path |
| `tests/vector_index_snapshot_vinfo_status_test.c` / `.sh` | test | VINFO snapshot status | observability |
| `tests/vector_index_snapshot_shadow_publish_test.c` / `.sh` | test | shadow publish + snapshot hook | R/S integration |
| `tests/vector_rebuild_state_machine_test.c` / `.sh` | test | R1 state machine | async rebuild precondition |
| `tests/vector_rebuild_delta_capture_test.c` / `.sh` | test | R4 delta capture | under-rebuild write correctness |
| `tests/vector_rebuild_shadow_replay_test.c` / `.sh` | test | R5 replay/final validation | async rebuild correctness |
| `tests/vector_rebuild_shadow_swap_test.c` / `.sh` | test | R3 publish/retired cleanup | reader-safe publish |
| `tests/vector_rebuild_async_worker_test.c` / `.sh` | test | R6 async worker | async rebuild benchmark prerequisite |
| `tests/vector_manager_resolve_fastpath_test.sh` | test | manager record_id fast resolve | e2e VSEARCH resolve overhead |
| `tests/vector_vinfo_backend_stats_test.sh` | test | VINFO backend stats | benchmark stats source |
| `tests/vector_vsearch_profile_breakdown_test.sh` | test | VSEARCH profile fields | endpoint latency decomposition |
| `tests/vector_testcase.c` | test/client | basic vector client | smoke utility |
| `tests/vector/semantic_onnx_test.sh` | test | ONNX semantic VSET/VSEARCH check | real embedding smoke, requires service |

### Docs

| 文件路径 | 层级 | 主要职责 | benchmark 影响 |
| --- | --- | --- | --- |
| `docs/hnsw_index_snapshot/README.md` / `index.md` / reports | docs | HNSW-S snapshot route, cold-start benchmark | confirms snapshot load benchmark exists and S line is closed |
| `docs/hnsw_rebuild_productization/README.md` / `index.md` / reports | docs | HNSW-R product rebuild route | confirms async rebuild product loop exists |
| `docs/vector_core_reset/` | docs | historical core split, v2 architecture, stats/profile stages | provides core_flat/v2 architecture and synthetic benchmark history |
| `docs/hnsw_productization/` | docs | HNSW-P productization history and 100K baseline | source of reused 100K records-only baseline |

## 5. VSET / VDEL / VSEARCH / VINDEX / VINFO 链路分析

### VSET

Current chain:

```text
RESP parser
  -> command task prepare or direct handler
  -> kvs_vector_set_prepare()
       -> kvs_vector_embedding_generate()
       -> optional embedding cache
       -> ONNX/hash runtime
       -> L2 normalize
  -> kvs_vector_set_commit_prepared()
       -> rebuild_worker write guard
       -> record store / id_map / record_id_map
       -> kvs_vector_index_add_current()
       -> active index write guard
       -> core_flat or core_hnsw_v2 add
       -> generation++
       -> capture upsert delta if rebuild capture active
```

结论：VSET ingestion throughput 可以压测，但必须区分：

- embedding generate / cache hit / cache miss time
- record store + id_map commit time
- core index add time
- response finalization time

### VDEL

Current chain:

```text
RESP parser
  -> kvs_vector_del()
       -> rebuild_worker write guard
       -> find active record by question/scope id_map
       -> active index write guard
       -> record mark deleted
       -> core_flat/core_hnsw_v2 mark_delete
       -> remove manager id maps
       -> free-list push
       -> generation++
       -> capture delete delta if rebuild capture active
```

结论：VDEL tombstone / rebuild cleanup path 可测。需要 benchmark 中单独记录 delete mix 与 tombstone ratio，否则 search latency / recall 会混入 tombstone effects。

### VSEARCH

Current chain:

```text
RESP parser
  -> kvs_vector_search_prepare()
       -> embedding generate/cache/batch worker
  -> kvs_vector_search_with_prepared()
       -> rebuild_worker read guard
       -> kvs_vector_index_search_current()
       -> active index read guard
       -> core search raw hits
       -> kvs_vector_manager_resolve_hits()
       -> manager scope/active/deleted/threshold filtering
  -> command response assembly
```

结论：端到端 VSEARCH latency 可以压测，且 VINFO 已暴露：

```text
vsearch_total_*
vsearch_backend_search_*
vsearch_manager_resolve_*
vsearch_response_assembly_*
vsearch_raw_hit_count
vsearch_resolved_hit_count
vsearch_filtered_hit_count
```

注意：`vsearch_backend_search_us` 是 `ops_search_wall_us - manager_resolve_us`，不是纯 core kernel time。

### VINDEX

Current chain:

```text
VINDEX REBUILD
  -> compact store
  -> rebuild id maps
  -> bind current index
  -> rebuild current index
  -> best-effort snapshot write

VINDEX REBUILD ASYNC|START
  -> one-shot worker
  -> shadow init
  -> full scan active records
  -> delta replay
  -> final validation
  -> reader-safe publish
  -> retired cleanup
  -> best-effort snapshot write

VINDEX REBUILD STATUS|CANCEL|RETRY
  -> async status/control response
```

结论：sync rebuild latency、async rebuild latency、VSEARCH under rebuild latency 都有产品入口；benchmark harness 仍需补压测编排和状态采样。

### VINFO

VINFO 当前可观测：

```text
backend stats
core_hnsw_v2 params / size / memory / tombstone
snapshot load/write/fallback status
rebuild state / delta count / error
embedding worker stats
embedding runtime/cache stats
VSEARCH profile breakdown
```

结论：VINFO 足够作为 e2e benchmark stats source；报告生成器仍需对字段做 schema freeze。

## 6. core_flat / core_hnsw_v2 / FAISS 对比可行性

当前状态：

```text
core_flat:
  product backend exists
  exact recall baseline exists
  measured memory stats exists

core_hnsw_v2:
  product default backend exists
  index-only tests and product integration tests exist
  snapshot and async rebuild product loop exist

FAISS:
  third_party/faiss source exists
  third_party/faiss/build/faiss/libfaiss.a exists
  product backend adapter is removed
  backend resolver maps faiss/faiss_hnsw/faiss_flat to core_flat compatibility fallback
```

Answer to required fairness questions:

| 问题 | 当前判断 |
| --- | --- |
| 是否已有 FAISS backend | 作为 third-party library / historical script dependency 存在；当前 product backend 已删除 |
| FAISS 是否是 product backend | 否 |
| FAISS 与 core_hnsw_v2 是否可使用同一批 vectors | 可以通过新 index-only harness / embedding cache 实现；当前产品路径不能直接切 FAISS |
| FAISS 与 core_hnsw_v2 是否可使用同一 metric | 可以；建议 512d normalized + IP，并记录 FAISS IndexFlatIP / HNSW metric |
| FAISS 与 core_hnsw_v2 是否可使用同一 dim | 可以；BGE 模型 dim=512 |
| 是否可以输出相同 topK 对照 | v2 tests 已有 exact topK/recall；FAISS 对照需新 harness |
| 是否已有 brute-force ground truth | core_flat 和 synthetic exact helpers 已有；真实 embedding fixture 需要冻结 ground truth |
| 是否已有 recall@K 计算 | v2 tests 有；FAISS real embedding harness 仍缺 |
| 是否已有 search latency 统计 | v2 tests / VINFO / vector_benchmark 有；FAISS unified latency harness 仍缺 |

结论：core benchmark readiness = partial。

可以先做：

- `core_flat` vs `core_hnsw_v2` index-only real embedding fixture benchmark。
- `core_hnsw_v2` synthetic / real embedding recall@10 with `core_flat` oracle。
- snapshot load / async rebuild benchmark using existing v2 paths。

不能直接做：

- product backend `core_hnsw_v2` vs product backend `FAISS` e2e 对比，因为 FAISS product backend 已删除。
- 旧 `scripts/benchmark/run_vector_native_faiss_formal.py` 直接跑通，因为它引用的 `tests/vector_index_only_formal.cpp` 当前不存在。

## 7. 端到端 benchmark 可行性

当前可测：

```text
VSET ingestion throughput:
  yes, but must split embedding/cache and commit/index add.

VSEARCH latency:
  yes, VINFO has backend/resolve/response profile.

VSEARCH recall:
  partial, product response gives question/answer/score; needs query expected topK fixture.

VINDEX REBUILD latency:
  yes, sync rebuild has status/timing.

snapshot load latency:
  yes, S-bench exists.

async rebuild latency:
  yes, R6 worker exists, but benchmark harness needed.

VSEARCH under rebuild latency:
  partial, product reader safety exists; load generator and timeline sampler needed.
```

Core benchmark 与 e2e benchmark 必须分开：

```text
core benchmark:
  input = cached float32 vectors / queries
  measure = index add/build/search/recall/memory
  no command parser, no RESP, no response assembly, no embedding calls

end-to-end benchmark:
  input = text dataset or preloaded records
  path = VSET / VSEARCH / VINDEX / VINFO
  measure = embedding prepare/cache, command scheduling, manager resolve,
            response assembly, VSEARCH profile, snapshot/rebuild status
```

结论：e2e benchmark readiness = partial。

当前产品路径足够支持 `core_hnsw_v2` e2e 压测；FAISS e2e 对比不能通过 product backend 切换完成，只能作为 external baseline 或新增 test-only harness。

## 8. models/ 真实 embedding 可用性分析

发现：

```text
models/bge-small-zh-v1.5/config.json
models/bge-small-zh-v1.5/model.onnx
models/bge-small-zh-v1.5/model.onnx_data
models/bge-small-zh-v1.5/vocab.txt
models/bge-small-zh-v1.5/tokenizer.json
models/bge-small-zh-v1.5/tokenizer_config.json
models/bge-small-zh-v1.5/special_tokens_map.json
```

`config.json` 显示：

```text
model_type = bert
hidden_size = 512
num_hidden_layers = 4
num_attention_heads = 8
vocab_size = 21128
```

ONNX runtime 状态：

```text
third_party/onnxruntime/include/onnxruntime_c_api.h exists
third_party/onnxruntime/lib/libonnxruntime.so.1.24.4 exists
Makefile check expects third_party/onnxruntime/lib/libonnxruntime.so
```

embedding runtime 能力：

```text
local_c provider exists
ONNX model load exists
vocab.txt loader exists
WordPiece-like tokenizer exists
single encode exists
batch encode exists
sentence_embedding output exists
L2 normalize exists
embedding cache exists
embedding worker pool exists
```

判断：

- 模型维度：512。
- 输出归一化：`kvs_vector_embedding_generate()` 和 batch path 会做 L2 normalize。
- 推荐 metric：normalized vectors + IP。
- 是否需要 batch embedding：需要。否则 VSET/VSEARCH e2e 指标会被重复单条 ONNX 调用支配。
- 是否能离线生成 fixture：可以，但当前没有专用 real embedding fixture/cache harness。
- 是否需要新增 embedding cache：产品已有 runtime cache；benchmark 仍需要离线 embedding cache 文件，避免每轮重复 ONNX 推理污染 index build/search。

结论：real embedding model available = yes；但 benchmark harness / fixture cache 仍缺。

## 9. benchmark 指标建议

Dataset:

```text
dataset_id
source
language/domain
count
query_count
dim = 512
metric = ip_normalized
topK = 10
scope distribution
text_len p50/p95/p99
embedding_model = bge-small-zh-v1.5
embedding_runtime = onnxruntime
embedding_cache_key/version
```

Core benchmark:

```text
backend = core_flat / core_hnsw_v2 / faiss_flat / faiss_hnsw
build_us
add_avg_us / add_p50_us / add_p95_us / add_p99_us
search_avg_us / search_p50_us / search_p95_us / search_p99_us
recall_at_10 against faiss_flat or core_flat
topK match sanity
memory_bytes + memory_source
index_size
deleted_count
M / M0 / efConstruction / efSearch
```

End-to-end benchmark:

```text
VSET qps / avg / p50 / p95 / p99
VSEARCH qps / avg / p50 / p95 / p99
embedding_prepare_avg/p95/p99
embedding_cache_hit_rate
vsearch_backend_search_avg/p95/max
vsearch_manager_resolve_avg/p95/max
vsearch_response_assembly_avg/p95/max
VINDEX REBUILD sync latency
VINDEX REBUILD ASYNC total latency
VSEARCH under async rebuild p50/p95/p99
snapshot_file_size_bytes
snapshot_load_us_avg/p50/p95
snapshot_write_us if measured
active + shadow + retired memory peak
```

Benchmark fixture/cache:

```text
texts.jsonl:
  id, question, answer, scope

embeddings.f32:
  row-major float32 normalized vectors

queries.jsonl:
  query_id, text, scope, expected_topk ids

manifest.json:
  model id/version, dim, metric, normalize, checksum,
  topK, count, query_count, source, created_at
```

## 10. 当前缺口

1. FAISS product backend 已删除，不能直接通过 `vector_index_type=faiss_hnsw` 做 e2e backend 切换。
2. `config/kvstore-embedding.conf` 使用旧 `vector_index_type=hnswlib_hnsw`，当前会 fallback 到 `core_flat`，不适合作为 v2 benchmark 配置。
3. 旧 formal benchmark 脚本引用 `tests/vector_index_only_formal.cpp`，当前文件不存在。
4. 没有冻结的真实 embedding fixture/cache 格式。
5. 没有同一批 real embeddings 下 `core_hnsw_v2` vs FAISS 的统一 core runner。
6. 没有 e2e recall fixture，把 VSEARCH response 与 expected topK 自动对齐。
7. 没有 VSEARCH under async rebuild 的 benchmark 编排脚本。
8. ONNX Runtime Makefile preflight 期望 `libonnxruntime.so`，当前实际发现的是 `libonnxruntime.so.1.24.4`，需要确认 symlink / lib path。
9. FAISS memory/threading/build flags 需要在 benchmark manifest 中固化，否则 core_hnsw_v2 vs FAISS 对比不可复现。
10. benchmark 应避免把 embedding generation time 混入 index build time；需要离线 embedding cache 或 explicit measurement split。

## 11. 是否可以进入压测对比

```text
ready_for_real_embedding_benchmark = partial
```

可以先做：

```text
1. benchmark-plan：冻结真实 embedding fixture/cache schema 和指标。
2. core_flat vs core_hnsw_v2 real embedding index-only benchmark。
3. core_hnsw_v2 e2e VSET/VSEARCH/VINDEX/VINFO benchmark。
4. snapshot load / async rebuild benchmark with real embedding records。
```

暂不能直接做：

```text
1. product backend core_hnsw_v2 vs product backend FAISS e2e 对比。
2. 复用旧 formal FAISS runner 作为最终结果。
3. 不带 embedding cache 的 VSET build-time 对比。
```

最低补齐项：

```text
1. 新增或修复 index-only FAISS runner，直接吃同一份 embeddings.f32 / queries.f32。
2. 新增 real embedding fixture/cache generator，包含 manifest/checksum/model id。
3. 新增 e2e benchmark config，显式使用 core_hnsw_v2，不使用 removed backend name。
4. 新增 e2e recall evaluator，从 VSEARCH response 对齐 expected topK。
5. 在 report schema 中强制记录 embedding cache hit/miss、metric、dim、normalization、FAISS build flags。
```

## 12. 推荐下一阶段

推荐下一阶段：

```text
benchmark-plan
```

目标应是冻结真实 embedding benchmark 设计，不先写大规模 harness：

```text
dataset source
embedding cache format
core runner input/output schema
FAISS baseline choice
e2e runner config
recall evaluator
snapshot / async rebuild benchmark schedule
result JSON / Markdown schema
```

如果直接进入实现，建议只实现 harness，不改生产 V command 行为，不恢复 FAISS product backend，不改变 default backend。

## 13. 检查结果

本轮未运行功能测试。理由：任务明确为 read-only audit + docs-only report，且本轮不修改生产代码、测试或 Makefile。

执行检查：

```text
git diff --check
PASS, no output

git status --short
?? docs/vector_benchmark_readiness/
```

## 14. Decision block

```text
V-family benchmark readiness decision:
  production_code_changed = no
  tests_changed = no
  makefile_changed = no
  docs_changed = yes
  v_family_files_audited = yes
  core_benchmark_ready = partial
  e2e_benchmark_ready = partial
  faiss_comparison_ready = partial
  real_embedding_model_available = yes
  embedding_cache_needed = yes
  benchmark_harness_needed = yes
  ready_for_real_embedding_benchmark = partial
  recommended_next = benchmark-plan
```
