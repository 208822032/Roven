# Stage 7C：direct-vector 高维 HNSW benchmark harness 清理与验证

## 1. 是否已读取并遵守 AGENTS.md

已读取并遵守项目根目录 `AGENTS.md`。

本阶段只处理 direct-vector high-dimensional benchmark harness / validation。未优化 HNSW 算法，未修改 memory layout，未修改 graph，未修改 manager 语义，未修改 RDB/AOF 格式，未修改 `kvs_vector_index_ops_t` 签名，未修改 VSEARCH response 格式。

## 2. 阶段目标

Stage 7C 目标：

```text
让 KVLite 可以真实测试 384 / 512 / 768 / 1024d synthetic direct vectors 下的 core_hnsw rebuild / VSEARCH / memory / recall。
```

本阶段不把 hash text path 伪装为高维 benchmark。hash text embedding 仍是 64d；synthetic direct-vector harness 显式用命令行 `dim` 初始化测试用派生 index。

## 3. 修改 / 新增文件

修改：

- `tests/vcmd_profile_bench.c`

新增：

- `docs/vector_core_reset/stage7c_direct_vector_highdim_harness.md`

生成 benchmark 输出：

- `results/vector_core_benchmark/stage7c_direct_vector_highdim_1k_384d.json`
- `results/vector_core_benchmark/stage7c_direct_vector_highdim_1k_384d.md`
- `results/vector_core_benchmark/stage7c_direct_vector_highdim_1k_512d.json`
- `results/vector_core_benchmark/stage7c_direct_vector_highdim_1k_512d.md`
- `results/vector_core_benchmark/stage7c_direct_vector_highdim_1k_768d.json`
- `results/vector_core_benchmark/stage7c_direct_vector_highdim_1k_768d.md`
- `results/vector_core_benchmark/stage7c_direct_vector_highdim_1k_1024d.json`
- `results/vector_core_benchmark/stage7c_direct_vector_highdim_1k_1024d.md`

## 4. 是否修改生产代码

否。

只修改测试 benchmark harness：`tests/vcmd_profile_bench.c`。

## 5. Stage 7B direct-vector harness gap 复盘

Stage 7B 发现：

```bash
./build/tests/bin/vcmd_profile_bench ann_core 1000 384 10 50
```

旧 harness 会先按 hash provider 初始化 vector module。hash provider 的 runtime dimension 固定为 `KVS_VECTOR_DEFAULT_DIMENSION = 64`。随后 harness 再提交 384d direct vectors，因此被 64d index 正确拒绝为 `KVS_VECTOR_ERR_INVALID_ARG`。

结论：

```text
这是 benchmark harness 覆盖缺口，不是 vector_core 维度检查失败。
```

## 6. 修复后的 direct-vector high-dim 初始化路径

`ann_core` 现在走 synthetic direct-vector 专用路径：

```text
kvs_config_init_default()
  -> vector_index_type = core_hnsw
  -> vector_metric = ip
  -> vector_distance_backend = auto
  -> vector_hnsw_m = 32
  -> vector_hnsw_ef_construction = 128
  -> vector_hnsw_ef_search = 256
  -> kvs_vector_init()
  -> use_synthetic_direct_vector_dimension(dim)
       destroy / reinit flat baseline
       destroy / reinit core_flat
       destroy / reinit core_hnsw with requested dim
       reserve core_hnsw capacity from manager store capacity
       re-bind current_index
  -> load direct vectors into manager record store with kvs_vector_restore_record_with_embedding()
  -> use_synthetic_direct_vector_dimension(dim)
  -> kvs_vector_index_rebuild_current()
  -> kvs_vector_index_search_current()
  -> exact IP topK against the same synthetic base vectors
  -> recall@K
```

原因：

- `hash` text provider 仍固定为 64d，不能作为高维 benchmark 依据。
- direct-vector benchmark 关注 record store source of truth -> rebuild -> core_hnsw search。
- 装载阶段使用 manager restore API 写入 record store 和 manager-owned maps，但不让 core 处理 scope / TTL / metadata / question / answer。

## 7. text embedding path / ONNX path / direct vector path 区分

路径区分：

```text
hash text path:
  VSET / VSEARCH text
  -> hash provider
  -> 64d

ONNX text path:
  VSET / VSEARCH text
  -> local_c provider
  -> models/bge-small-zh-v1.5/model.onnx
  -> 512d

direct vector path:
  *_with_embedding / restore-with-embedding style direct vector input
  -> caller-provided vector
  -> dim comes from caller-provided vector length

synthetic direct vector path:
  tests/vcmd_profile_bench ann_core <N> <DIM> <TOPK> <QUERIES>
  -> random direct vectors
  -> test harness initializes derived indexes at DIM
```

## 8. 384d benchmark 结果

Command:

```bash
./build/tests/bin/vcmd_profile_bench ann_core 1000 384 10 50
```

Result file:

```text
results/vector_core_benchmark/stage7c_direct_vector_highdim_1k_384d.json
```

Summary:

```text
backend_dim: 384
distance_backend: avx2
rebuild_core_add_total_us: 700,931
hnsw_add_layer_search_us: 185,871
hnsw_add_neighbor_select_us: 497,909
hnsw_add_link_us: 2,689
vsearch_backend_search_avg_us: 403
backend_memory_bytes: 6,188,240
backend_memory_bytes_source: measured
recall@10: 1.0000
```

## 9. 512d benchmark 结果

Command:

```bash
./build/tests/bin/vcmd_profile_bench ann_core 1000 512 10 50
```

Result file:

```text
results/vector_core_benchmark/stage7c_direct_vector_highdim_1k_512d.json
```

Summary:

```text
backend_dim: 512
distance_backend: avx2
rebuild_core_add_total_us: 952,357
hnsw_add_layer_search_us: 239,645
hnsw_add_neighbor_select_us: 690,969
hnsw_add_link_us: 2,746
vsearch_backend_search_avg_us: 516
backend_memory_bytes: 6,712,528
backend_memory_bytes_source: measured
recall@10: 1.0000
```

## 10. 768d / 1024d benchmark 结果

768d:

```text
result file:
  results/vector_core_benchmark/stage7c_direct_vector_highdim_1k_768d.json

backend_dim: 768
distance_backend: avx2
rebuild_core_add_total_us: 1,448,042
hnsw_add_layer_search_us: 346,079
hnsw_add_neighbor_select_us: 1,077,146
hnsw_add_link_us: 2,927
vsearch_backend_search_avg_us: 737
backend_memory_bytes: 7,761,104
backend_memory_bytes_source: measured
recall@10: 1.0000
```

1024d:

```text
result file:
  results/vector_core_benchmark/stage7c_direct_vector_highdim_1k_1024d.json

backend_dim: 1024
distance_backend: avx2
rebuild_core_add_total_us: 1,956,233
hnsw_add_layer_search_us: 451,981
hnsw_add_neighbor_select_us: 1,467,678
hnsw_add_link_us: 3,146
vsearch_backend_search_avg_us: 970
backend_memory_bytes: 8,809,680
backend_memory_bytes_source: measured
recall@10: 1.0000
```

## 11. backend_dim 是否真实生效

是。

`ann_core` 在 benchmark 结束前调用 `kvs_vector_info()` 并断言：

```text
info.dimension == requested dim
info.backend_dim == requested dim
```

已验证：

```text
384d: dimension=384, backend_dim=384
512d: dimension=512, backend_dim=512
768d: dimension=768, backend_dim=768
1024d: dimension=1024, backend_dim=1024
```

## 12. distance_backend 是否为 avx2

是。

本机运行结果：

```text
384d: distance_backend=avx2
512d: distance_backend=avx2
768d: distance_backend=avx2
1024d: distance_backend=avx2
```

## 13. recall@10 是否回退

未观察到 Stage 7C harness 引入 recall 回退。

Synthetic direct-vector 1K 结果：

```text
384d: recall@10=1.0000
512d: recall@10=1.0000
768d: recall@10=1.0000
1024d: recall@10=1.0000
```

说明：

```text
Stage 7C 没有修改 HNSW 算法，因此 recall 变化不应来自本阶段生产逻辑。
```

## 14. ONNX 512d 验证

临时配置基于 `config/kvstore-embedding.conf` 语义：

```text
vector_index_type = core_hnsw
embedding_provider = local_c
embedding_model_path = models/bge-small-zh-v1.5/model.onnx
embedding_dim = 512
vector_distance_backend = auto
vector_metric = ip
```

验证结果：

```text
startup_dimension: 512
startup_backend_dim: 512
startup_distance_backend: avx2

After 3 text VSET + 1 text VSEARCH:
  active_records: 3
  backend_size: 3
  backend_dim: 512
  distance_backend: avx2
  vsearch_backend_search_count: 1
```

结论：

```text
ONNX local_c text path 仍可把 512d embedding 输入 core_hnsw，且 text VSEARCH 会进入 backend search。
```

## 15. 不变量

已保持：

- same index 不混用不同 dim。
- direct vector dim 必须等于 index dim。
- text embedding dim 必须等于 index dim。
- `record_id_to_slot` 语义保持。
- `deleted_flags` 语义保持。
- `visited_tags` 语义保持。
- level0 worst-neighbor cache 语义保持。
- search 输出仍为 `record_id + score`。
- core 不处理 scope / TTL / metadata / question / answer。
- manager 仍负责 response。

## 16. tests 结果

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

## 17. benchmark 结果

已运行：

```bash
./build/tests/bin/vcmd_profile_bench ann_core 1000 384 10 50
./build/tests/bin/vcmd_profile_bench ann_core 1000 512 10 50
./build/tests/bin/vcmd_profile_bench ann_core 1000 768 10 50
./build/tests/bin/vcmd_profile_bench ann_core 1000 1024 10 50
```

结果文件见第 3 节和第 8-10 节。

## 18. 静态边界检查结果

已运行：

```bash
grep -RIn "g_vector_state" src/vector/core include/vector 2>/dev/null
grep -RIn "scope\|ttl\|metadata\|question\|answer" src/vector/core 2>/dev/null
grep -RIn "kvs_vector_index_ops_t" include src/vector 2>/dev/null
grep -RIn "record_id_map" src/vector/core include/vector 2>/dev/null
git diff -- src/persistence
```

结果：

```text
g_vector_state:
  no src/vector/core hits
  include/vector hits are existing manager/internal declarations and comments

scope|ttl|metadata|question|answer in src/vector/core:
  no hits

kvs_vector_index_ops_t:
  existing declarations and ops tables only
  signature unchanged

record_id_map:
  no src/vector/core hits
  include/vector hits are manager record-store declarations

src/persistence:
  no diff
```

## 19. git status --short

Stage 7C 相关状态：

```text
 M tests/vcmd_profile_bench.c
?? docs/vector_core_reset/stage7c_direct_vector_highdim_harness.md
```

工作区还存在前序 Stage 5A-7B 的既有修改 / 未跟踪文件；本阶段未处理 unrelated diff。

## 20. 下一步建议

建议下一阶段：

```text
Stage 7D: core_hnsw high-dimensional memory-layout / cache locality optimization planning
```

Stage 7D 开始前应使用 Stage 7C 的真实高维 direct-vector baseline，不再用 hash 64d text path 推断 384/512/768/1024d 行为。不要在 Stage 7D 引入 graph persistence、shard / partition、scope-aware search、RDB/AOF 格式变化或 response 格式变化。
