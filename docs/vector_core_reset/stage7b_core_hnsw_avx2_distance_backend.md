# Stage 7B：core_hnsw AVX2 距离后端接入

## 1. 修改 / 新增文件

修改：

- `src/vector/core/hnsw/kvs_hnsw_add.c`
- `src/vector/core/hnsw/kvs_hnsw_search.c`
- `src/vector/kvs_vector_distance_avx2.c`
- `scripts/bench_vector_core_compare.py`
- `tests/vector_core_hnsw_rebuild_profile_test.sh`

新增：

- `docs/vector_core_reset/stage7b_core_hnsw_avx2_distance_backend.md`

## 2. 是否修改生产代码

是。

生产代码修改集中在 core_hnsw 距离打分调用点，以及已有 AVX2 距离 kernel：

- HNSW add 阶段内置 IP 路径现在调用 `kvs_vector_distance_dot()`。
- HNSW add 阶段内置 L2 路径现在调用 `kvs_vector_distance_squared_l2()`，并保留既有 HNSW score 语义：负平方 L2。
- HNSW add 阶段内置 cosine 路径现在调用 `kvs_vector_distance_cosine_score()`。
- HNSW search 阶段内置打分使用同一组 wrapper。
- AVX2 dot / squared L2 / cosine kernel 在 SIMD lane 计算后按标量顺序累加。这样避免水平向量归约顺序改变 HNSW 构图决策，从而保持 recall。

未修改 RDB/AOF 格式、vector index ops 签名、manager 过滤逻辑、manager response 格式、scope、TTL、metadata、question 或 answer 逻辑。

## 3. 当前调用链

文本路径：

```text
VSET / VSEARCH text
  -> kvs_vector_embedding_generate()
     -> hash provider 固定 64d，或 local_c ONNX provider runtime dimension
  -> manager record store
  -> current index ops
  -> core_hnsw add/search
  -> kvs_hnsw_metric_score()
     -> 如设置 custom index->distance，则优先使用
     -> 否则走内置 metric fast path
        -> kvs_vector_distance_dot()
        -> kvs_vector_distance_squared_l2()
        -> kvs_vector_distance_cosine_score()
        -> runtime 选择后端：avx2 或 fallback
```

Direct vector 路径：

```text
kvs_vector_set_prepare_with_embedding()
kvs_vector_search_prepare_with_embedding()
  -> 调用方直接提供 vector，不经过文本 embedding provider
  -> manager record store / search prepare
  -> current index ops
  -> core_hnsw add/search
```

## 4. 修改前复杂度 / 行为

core_hnsw 在 add 和 search 两个文件中各自维护本地标量循环：

- dot / IP
- squared L2，并以 `-distance` 作为 HNSW score
- cosine

项目中已经存在 runtime 选择的距离后端：`src/vector/kvs_vector_distance.c`，以及 AVX2 实现：`src/vector/kvs_vector_distance_avx2.c`。但 core_hnsw 之前没有使用这套后端。

## 5. 修改后设计

core_hnsw 保留自身 metric 语义，但把内置数值 kernel 委托给共享 distance backend。

关键语义细节：

```text
HNSW L2 score 保持：-squared_l2(lhs, rhs)
Public distance helper L2 score 保持：1 / (1 + squared_l2)
```

实现中故意让 HNSW L2 调用 `kvs_vector_distance_squared_l2()`，而不是 `kvs_vector_distance_l2_score()`，以保持分数格式和排序语义不变。

`index->distance != NULL` 仍然优先于内置 metric dispatch。

## 6. 安全不变量

已保持：

- `record_id_to_slot` 语义不变。
- `deleted_flags` 语义不变。
- `visited_tags` 语义不变。
- level0 worst-neighbor cache 语义不变。
- search 输出仍为 `record_id + score`。
- core 不处理 scope / TTL / metadata / question / answer。
- manager 仍负责 response assembly。
- 同一个 index 仍通过既有 `dim != index->dim` 检查拒绝混合维度。
- VSET/VSEARCH 文本路径仍使用 provider 生成的 vector。
- direct vector API 仍绕过 embedding 生成。

AVX2 兼容性：

- HNSW 不直接调用 AVX2 intrinsic。
- AVX2 选择仍集中在 `kvs_vector_distance_init()`。
- `vector_distance_backend=auto` 仅在 `__builtin_cpu_supports("avx2")` 报告支持时选择 AVX2。
- fallback 仍为标量实现，并且始终可用。

## 7. ONNX embedding provider 审计

当前 ONNX/local C 状态：

- `config/kvstore-embedding.conf` 设置 `embedding_provider = local_c`。
- `config/kvstore-embedding.conf` 设置 `embedding_model_path = models/bge-small-zh-v1.5/model.onnx`。
- 模型文件本地存在于 `models/bge-small-zh-v1.5/`。
- `models/bge-small-zh-v1.5/config.json` 中 `hidden_size = 512`。
- `config/kvstore-embedding.conf` 设置 `embedding_dim = 512`。
- `kvs_vector_embedding_create_runtime_handle()` 调用 `embedding_create()`，随后调用 `embedding_dimension(handle)`。
- local_c provider 保存 `provider->dimension = embedding_dimension(handle)`。
- 当 provider 为 local_c 时，`kvs_vector_embedding_generate()` 用 local_c 处理 VSET/VSEARCH 文本。
- `kvs_vector_embedding_generate()` 会对生成的 embedding 做 L2 normalize。

路径结论：

```text
ONNX code path exists: yes
configured ONNX model path: models/bge-small-zh-v1.5/model.onnx
configured ONNX embedding dim: 512
model config hidden_size: 512
runtime dimension source: embedding_dimension(handle)
VSET text path uses ONNX when embedding_provider=local_c: yes
VSEARCH text path uses ONNX when embedding_provider=local_c: yes
direct vector path bypasses embedding: yes
```

当前 VINFO 暴露：

- `dimension`
- `backend_dim`
- `distance_backend`

VINFO 当前没有单独暴露 `embedding_dim` 或 `embedding_model_version` 字段。Stage 7B 未新增这些字段，因为本阶段范围是 HNSW 距离后端接入与验证，不是扩展 VINFO schema。

## 8. 维度验证

三类维度：

```text
hash/smoke dim:
  64

ONNX real embedding dim:
  512，来自 config 和 model hidden_size；runtime provider 通过 embedding_dimension(handle) 确认

direct vector synthetic dim:
  调用方通过 *_with_embedding API 传入，绕过文本 embedding
```

Benchmark 注意事项：

`scripts/bench_vector_core_compare.py` 仍使用 `VSET` / `VSEARCH` 文本命令，并写入 `embedding_provider = hash`。在该路径中，生成配置里的 `embedding_dim = <args.dim>` 不会让 hash embedding 变成高维，因为 hash provider 初始化时使用 `provider->dimension = KVS_VECTOR_DEFAULT_DIMENSION`，当前为 64。

因此：

```text
DIM=384 with bench_vector_core_compare.py + hash text path is not a real 384d HNSW benchmark.
Actual HNSW backend_dim remains 64 for that script unless provider changes.
Direct vector synthetic high-dim validation should use tests/vcmd_profile_bench.c or another *_with_embedding harness.
ONNX real-dim validation should use config/kvstore-embedding.conf with core_hnsw and local_c.
```

## 9. 正确性测试

已通过：

```bash
make clean
make ENABLE_HNSWLIB=1 ENABLE_FAISS=1 -j"$(nproc)"

tests/vector_core_hnsw_correctness_test.sh
tests/vector_core_hnsw_rebuild_test.sh
tests/vector_core_hnsw_rdb_reload_test.sh
tests/vector_core_hnsw_aof_replay_test.sh
tests/vector_core_hnsw_rebuild_profile_test.sh

tests/vector_manager_resolve_fastpath_test.sh
tests/vector_vsearch_profile_breakdown_test.sh
tests/vector_vinfo_backend_stats_test.sh
```

额外检查：

```bash
python3 -m py_compile scripts/bench_vector_core_compare.py
git diff --check -- src/vector/core/hnsw/kvs_hnsw_add.c src/vector/core/hnsw/kvs_hnsw_search.c scripts/bench_vector_core_compare.py tests/vector_core_hnsw_rebuild_profile_test.sh docs/vector_core_reset/stage7b_core_hnsw_avx2_distance_backend.md
```

## 10. Benchmark 结果

比较规则：

- 如果 Stage 7A event counters 仍保留，则 10K profile 数值与 Stage 7A 对比。
- 如果 event counters 被删除或减少，则与 Stage 6E 对比，并说明 instrumentation 口径差异。

当前实现保留 Stage 7A event counters，因此匹配基线为 Stage 7A：

```text
Stage 7A 10K with event counters:
  rebuild_core_add_total_us: 1,013,818
  hnsw_add_layer_search_us: 755,854
  add_distance_compute_count: 9,106,016
  recall@10: 0.9870
```

Stage 7B final 10K / 64d hash text-path 结果：

```text
result file:
  results/vector_core_benchmark/stage7b_final/stage7b_hnsw_avx2_distance_backend_benchmark_10k_64d.json

core_hnsw:
  distance_backend: avx2
  dimension: 64
  backend_dim: 64
  rebuild_core_add_total_us: 853,896
  hnsw_add_layer_search_us: 653,971
  hnsw_add_neighbor_select_us: 143,566
  add_distance_compute_count: 9,106,016
  VSEARCH backend_search_avg_us: 69
  recall@10: 0.9870
```

相对 Stage 7A profiled baseline 的改善：

```text
rebuild_core_add_total_us:
  1,013,818 -> 853,896
  improvement: 15.77%

hnsw_add_layer_search_us:
  755,854 -> 653,971
  improvement: 13.48%

recall@10:
  0.9870 -> 0.9870
```

达到 Stage 7B 64d acceptable pass 目标：

```text
hnsw_add_layer_search_us下降 >= 10%: yes
rebuild_core_add_total_us下降 >= 6%: yes
recall@10不低于0.9870: yes
```

重要迭代记录：

早期的 AVX2 水平向量归约实现 rebuild 更快，但 recall@10 为 0.9860。该版本未被接受。最终实现让 AVX2 kernel 内部保持标量顺序累加，避免改变 HNSW 构图决策。

ONNX 真实维度验证：

```text
temporary config:
  based on config/kvstore-embedding.conf
  vector_index_type changed to core_hnsw
  embedding_provider local_c
  embedding_model_path models/bge-small-zh-v1.5/model.onnx

VINFO at startup:
  index_type: core_hnsw
  dimension: 512
  backend_dim: 512
  distance_backend: avx2
  metric: ip

After 3 text VSET + 1 text VSEARCH:
  active_records: 3
  index_size: 3
  backend_size: 3
  dimension: 512
  backend_dim: 512
  vsearch_backend_search_count: 1
```

这确认 ONNX 文本 embedding 路径可以把 512d vector 写入 core_hnsw，并且 VSEARCH 会进入 core_hnsw backend。该小样本语义结果未作为 recall 质量 benchmark。

Direct vector synthetic dim 验证：

```text
attempted:
  ./build/tests/bin/vcmd_profile_bench ann_core 1000 384 10 50

result:
  failed at commit[0] with KVS_VECTOR_ERR_INVALID_ARG

reason:
  existing vcmd_profile_bench initializes the vector module with hash provider dimension 64 before using caller-provided 384d vectors.
  The index correctly rejects 384d vectors against a 64d index.
```

这是当前 benchmark harness 覆盖缺口，不是 vector_core 维度检查失败。后续 direct-vector 高维 harness 应在 commit vector 前把 index/provider 维度初始化为请求的 synthetic dim，或新增一个绕过 manager 文本 provider 维度的专用 core_hnsw direct-index benchmark。

## 11. 静态边界检查结果

已运行命令：

```bash
grep -RIn "g_vector_state" src/vector/core include/vector 2>/dev/null
grep -RIn "scope\|ttl\|metadata\|question\|answer" src/vector/core 2>/dev/null
grep -RIn "record_index" src/vector include 2>/dev/null
grep -RIn "kvs_vector_index_ops_t" include src/vector 2>/dev/null
```

结果：

```text
g_vector_state:
  no src/vector/core hits
  include/vector hits are existing manager/internal declarations and comments

scope|ttl|metadata|question|answer in src/vector/core:
  no hits

record_index:
  existing manager / legacy adapter / old internal API hits
  no new core_hnsw record_index dependency introduced by Stage 7B

kvs_vector_index_ops_t:
  existing declarations and ops tables only
  signature unchanged
```

## 12. 构建结果

已通过：

```bash
make clean
make ENABLE_HNSWLIB=1 ENABLE_FAISS=1 -j"$(nproc)"
```

## 13. git status --short

报告生成时，工作区已经存在许多 Stage 5A-7A 的既有修改和未跟踪报告/测试。Stage 7B 只触及第 1 节列出的文件，以及被忽略的 benchmark 输出目录 `results/vector_core_benchmark/stage7b_final/`。

Stage 7B 相关主要状态：

```text
 M scripts/bench_vector_core_compare.py
 M src/vector/core/hnsw/kvs_hnsw_add.c
 M src/vector/core/hnsw/kvs_hnsw_search.c
 M src/vector/kvs_vector_distance_avx2.c
?? docs/vector_core_reset/stage7b_core_hnsw_avx2_distance_backend.md
```

## 14. 下一步建议

Stage 7B 验证完成后：

```text
Stage 7C: direct-vector high-dimensional HNSW benchmark harness cleanup / validation
```

只有在 Stage 7B 已经证明 distance compute 变快后，后续阶段才应考虑非 distance 的 memory layout 工作。不要从本阶段开始 graph persistence、shard/partition、scope-aware search 或 upper-neighbor dynamic allocation。
