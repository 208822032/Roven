# B2-ground-truth-generation: core_flat exact ground truth report

## 1. AGENTS.md 遵守情况

已读取并遵守仓库根目录 `AGENTS.md`，并读取本阶段要求的：

- `docs/vector_benchmark_readiness/benchmark_plan_real_embedding.md`
- `docs/vector_benchmark_readiness/b0_preflight_report.md`
- `docs/vector_benchmark_readiness/b1_fixture_cache_generation_report.md`
- `docs/vector_benchmark_readiness/README.md`
- `docs/vector_benchmark_readiness/index.md`

本阶段是 index-only ground truth 生成。核心边界全部遵守：未修改生产 V 命令代码、未修改 RDB/AOF、未修改 `VSEARCH` response、未修改 `kvs_vector_index_ops_t`、未访问 `g_vector_state`；ground truth 生成器只通过公开的 `kvs_flat_index_*` 函数驱动 core_flat（vector_core 层，只处理 record_id + vector + score）。新增内容仅限 `scripts/benchmark/`、`tests/`（新增独立 smoke test）、`Makefile`（新增 opt-in target）和本目录文档。

本报告只记录本阶段实际实现，不重复已有文档的路线背景与通用边界。

## 2. 实际实现和文件

新增：

```text
scripts/benchmark/generate_real_embedding_groundtruth.c   generate + validate，index-only
tests/vector_real_embedding_groundtruth_smoke_test.sh      smoke test
docs/vector_benchmark_readiness/b2_ground_truth_generation_report.md  本报告
```

修改：

```text
Makefile   新增 gen-embedding-groundtruth / smoke-vector-real-embedding-groundtruth 两个 opt-in target
docs/vector_benchmark_readiness/README.md   标记 B2 完成
docs/vector_benchmark_readiness/index.md    标记 B2 完成，链接本报告
```

生产代码（`src/`、`include/`）未修改。生成器链接 `libkvstore.a` 以复用 core_flat，但不链接 `onnxruntime`（不调用 ONNX），额外链接 `-lcrypto` 做 checksum 校验。

## 3. 使用的 core_flat 调用链

Ground truth 由项目现有 core_flat exact IP 扫描产生，不另写正式实现。调用链（全部来自 `include/vector/kvs_vector_flat.h`）：

```text
kvs_config_init_default(&g_cfg); g_cfg.mem_backend = KVS_MEM_BACKEND_SYSTEM; mem_set_backend(KVS_MEM_BACKEND_SYSTEM)
  -> kvs_flat_index_init(&flat, dim=512, KVS_VECTOR_METRIC_IP)
  -> kvs_flat_index_reserve(&flat, record_count)
  -> kvs_flat_index_add(&flat, record_id=row, base + row*dim, dim)   // record_id == records.jsonl id
  -> per query: kvs_flat_index_search(&flat, qvec, dim, topk, hits, &out_count)
       hits: kvs_vector_hit_t{ uint64_t record_id; float score }, 已按 score 降序返回
  -> kvs_flat_index_destroy(&flat)
```

- core_flat 的 IP kernel 是本地标量点积（`src/vector/core/flat/kvs_flat_search.c`），score 为原始点积，越大越相似；不经过 `kvs_vector_distance.c`，因此与 AVX2/distance backend 无关，完全确定性。
- 内存后端设为 `KVS_MEM_BACKEND_SYSTEM` 后再建索引（与现有 vector core 测试一致），避免使用未初始化的内存后端。
- 生成器写入 groundtruth 的是 core_flat 的 `hits` 原样输出（rank、record_id、score）。独立 double 精度 brute-force 只用于交叉验证，绝不作为写入真值。

## 4. 输入 dataset / checksum

输入只消费 B1 冻结缓存（`/tmp/kvlite_vector_benchmark_artifacts/real_embedding/qa_zh_1k_200_bge_small_zh_v1_5/`）：

```text
manifest.json / embeddings.f32 / query_embeddings.f32 / records.jsonl / queries.jsonl
```

生成器在使用前独立校验（等价于 B1 validate 的核心检查，从而拒绝损坏输入）：

- `manifest.json` 严格 schema：`schema_version==1`、`dataset_id` 一致、`params.{dtype=float32, byte_order=little, metric=ip_normalized, normalized=true}`。
- 四个 artifact 的 sha256 与 `manifest.outputs` 逐一比对。
- 额外校验 `records.jsonl` 的 `id` 与 `queries.jsonl` 的 `query_row` 逐行等于其行号，保证 core_flat 的 `record_id`（embedding 行号）能 1:1 回表到文本（把"行号==id"从假设变成检查）。

实际输入 checksum（记入 ground truth manifest 的 `source` 段）：

```text
manifest.json            0176cdcf05070fe96db39ac40897704a284af7c2f7a040a901539517b3ca6f2b
embeddings.f32           57dc5611ee5d30a6da11a7efe2e0393a19da106bad5dae4f63d5cd7a5a4506ce
query_embeddings.f32     fc3117ef25dfd8e2137b0df836d776e697c39c3934e8b4018b0b9f2c5c5058c6
records.jsonl            720bf45ae6b543106bacd26e728697e5ab4a28c5d30b06a997dff575baa80df2
queries.jsonl            e8c8dbbd7fb4a9c82f720f67d99f6893d97f15b601bc08444eea81639eea1a9c
```

## 5. groundtruth 实际 schema

### 5.1 `groundtruth.jsonl`（每 query 一行，共 200 行）

```json
{"query_id":"q000000","query_row":0,"topk":10,"metric":"ip_normalized","neighbors":[{"rank":1,"record_id":9,"score":0.908512533},{"rank":2,"record_id":0,"score":0.87285...}, ...]}
```

字段与 `benchmark_plan_real_embedding.md` 第 12.1 节冻结 schema 一致：`query_id`(string)、`query_row`(uint)、`topk`、`metric="ip_normalized"`、`neighbors[]{rank(1-based), record_id, score}`。

映射与 join 约定（B3 必须遵守）：

```text
query_id   = "q%06d" of query_row，确定性派生，仅作字符串 join key
query_row  -> queries.jsonl.query_row（整型 join，本阶段已校验 query_row==行号）
record_id  -> records.jsonl.id（本阶段已校验 id==行号；也等于 embeddings.f32 行号）
score      = core_flat 原始 IP 点积（float，%.9g 序列化，可精确 round-trip）
```

### 5.2 `groundtruth.manifest.json`（本阶段新增的结果 manifest）

`benchmark_plan_real_embedding.md` 未单独冻结 B2 结果 manifest（其 §12.2 的 `manifest.json` 是 B1 dataset manifest，本阶段不改写它以保持 B1 冻结产物不可变）。因此本阶段新增并冻结一个独立的 `groundtruth.manifest.json`，供 B3 validate 使用：

```text
schema_version, dataset_id, created_at_utc
generator{name, version, source_sha256}
source{manifest_path, manifest_sha256, embeddings_sha256, query_embeddings_sha256, records_jsonl_sha256, queries_jsonl_sha256}
params{record_count, query_count, dim, topk, metric, ground_truth_source="core_flat_exact_ip", tie_break}
outputs.groundtruth_jsonl{path, sha256, size_bytes, rows}
crosscheck{method, queries_checked, score_tolerance, tie_eps, set_match_all, strict_order_checked, strict_order_match_all, max_score_abs_diff}
timing_us{cache_load_us, index_build_us, search_total_us, search_avg_us, crosscheck_us, total_us}
```

## 6. topK 和 tie-break 规则（冻结）

```text
topK = 10
主排序    = score 降序（core_flat 保证输出已按 score 降序）
tie 归属  = 相等 score 时 membership 由最低 slot 决定 = 插入顺序 = record_id 升序
```

冻结依据（读源码确认，非猜测）：core_flat 用 score-only 的大小根堆（比较器 `kvs_vector_heap_min_score_less` 是严格 `<`），扫描按 slot 升序，替换用严格 `item.score > top->score`；相等 score 的后来者无法挤掉更早的候选，因此 membership 恒等于"最低 slot / 最早插入"。本阶段 `record_id == slot == 行号`，故 tie membership 等价于 record_id 升序。

补充说明（诚实标注）：core_flat 在同一 score tier 内的**输出位置顺序**是堆数组 artifact（确定但非 slot/record_id 的简单排序键）。真实 embedding 下 512 维 float 点积不产生精确 tie（本数据集 `max_score_abs_diff=1.07e-06`，且 200 条 query 全部通过严格顺序核对），因此该 artifact 在本数据集上不触发。交叉验证对"相邻分数差 ≤ 1e-6"的 tier 自动退化为集合比较，保证即使出现 tie 也不误报。

## 7. 实际生成结果

Dataset：`qa_zh_1k_200_bge_small_zh_v1_5`。

```text
record_count = 1000, query_count = 200, dim = 512, topk = 10, metric = ip_normalized
groundtruth.jsonl        rows=200, size≈109KB
groundtruth.manifest.json
groundtruth.jsonl sha256 = b53ddf15773402072c2e27deeba00af5dc837648bddd4e64091cd4426603f3c9
generator source_sha256  = c9d8284620209bdbfbac2ab704c42d7e7bb6e528e2cd137b01fa48d4c93e039c
```

样例（query 0，`"什么是Redis AOF？"`）：top1 `record_id=9 score=0.908512533`，top2 `record_id=0 score=0.872857`，score 严格单调递减、rank 1..10 连续、record_id 唯一且在 `[0,1000)`。

## 8. 时间指标（不含 B1 ONNX 时间）

来自 `groundtruth.manifest.json.timing_us`：

```text
cache_load_us     = 1,726     （读取两个 .f32）
index_build_us    = 1,124     （core_flat init/reserve/add 1000 条）
search_total_us   = 79,473    （200 条 query 的 core_flat exact 检索）
search_avg_us     = 397       （每 query）
crosscheck_us     = 97,157    （200 条独立 double brute-force 核对，非 ground truth 生成时间）
total_us          = 184,485
```

B1 的 ONNX 推理时间不计入 B2：B2 完全不调用 ONNX，只读离线 `embeddings.f32` / `query_embeddings.f32`。

## 9. 抽样 brute-force 交叉验证结果

生成器对**全部 200 条** query 用独立 double 精度点积 brute-force 复算（远超"至少 10 条"要求），并按 `(score 降序, record_id 升序)` 排序取 topK：

```text
queries_checked        = 200
set_match_all          = true    （每 query topK 的 record_id 集合与 core_flat 完全一致）
strict_order_checked   = 200     （全部 query 无相邻近似 tie，做了严格顺序核对）
strict_order_match_all = true    （逐 rank record_id 完全一致）
max_score_abs_diff     = 1.07e-06（core_flat float 点积 vs double 点积的最大差，容差 1e-4）
```

Smoke test 另用 Python `struct` 对 20 条 query 独立复算（row/顺序/score 容差内一致），并做 records/queries.jsonl 实际回表。两条独立路径（生成器内 C double、smoke Python）结论一致。

## 10. deterministic / corrupt-input 测试

Smoke test（`tests/vector_real_embedding_groundtruth_smoke_test.sh`，独立一次性 out-root，trap 清理，不触碰正式 dataset）覆盖并全部通过：

```text
[PASS] B1 validate-only passed on input cache
[PASS] ground truth generated via core_flat exact IP (no ONNX)
[PASS] ground truth structural checks + back-join + brute-force cross-check passed
[PASS] B2 validate-only passed
[PASS] regenerating over existing ground truth fails fast
[PASS] ground truth output is deterministic across regenerations   （移除后重生成 groundtruth.jsonl sha256 完全一致）
[PASS] corrupted input cache is rejected (sha256 mismatch)          （翻转 embeddings.f32 一个字节 -> 生成器拒绝）
[PASS] corrupted ground truth fails validate (sha256 mismatch)      （翻转 groundtruth.jsonl 一个字节 -> validate 失败）
```

`generate` 默认 fail-fast：目标已存在 `groundtruth.jsonl` 或 `groundtruth.manifest.json` 时拒绝覆盖。`validate` 全程不调用 core_flat / ONNX，只做 manifest schema、checksum、rank 连续性、score 单调、id/row 范围、行数一致等纯文件检查，并核对 `source.manifest_sha256` 与当前 B1 `manifest.json` 一致（provenance 绑定）。

## 11. 自审与修复（adversarial review）

对本阶段 diff 做了多维 find→verify 对抗审计，确认并修复 3 项：

```text
[high] Makefile GEN_GT_BIN 丢失 libkvstore.a 依赖（$(LIBKVSTORE) 在其定义前被引用，展开为空）
       -> 改用早定义的 $(BUILD_LIB)/libkvstore.a 作为前置依赖；clean 构建可先建库再链接。
[low]  ground truth manifest 的 fclose 返回值未检查、写入前无 fflush
       -> 与 gt 数据文件一致：fflush + ferror + fclose 三者合并检查后再 rename。
[low]  validate 在 B1 manifest 不可读时静默跳过 provenance 校验
       -> 读取失败视为 validate 失败。
```

额外硬化（非缺陷，主动加强）：`records.jsonl id / queries.jsonl query_row` 逐行 == 行号校验（支撑回表要求）、`stat` 失败硬失败、score 由 `%.8g` 提升为 `%.9g`（float 精确 round-trip）、smoke test 临时文件移入 trap 清理目录。其余审计项为 false positive 或超出范围（见第 13 节限制）。

## 12. B3 输入契约

B3（core_hnsw_v2 index-only benchmark）只能使用以下已校验文件，且必须先跑 `validate`，不得重新调用 ONNX，也不得重新生成 ground truth：

```text
manifest.json
embeddings.f32
query_embeddings.f32
records.jsonl
queries.jsonl
groundtruth.jsonl
（并可读取 groundtruth.manifest.json 做 provenance / schema 校验）
```

推荐 B3 先运行：

```bash
build/tools/bin/generate_real_embedding_cache        validate --dataset-id qa_zh_1k_200_bge_small_zh_v1_5
build/tools/bin/generate_real_embedding_groundtruth  validate --dataset-id qa_zh_1k_200_bge_small_zh_v1_5
```

## 13. 当前限制

- 仅覆盖 B1 冻结的单一 smoke 规模（1000 records / 200 queries / dim=512 / topk=10）；大规模真实语料未引入。
- ground truth 真值来源固定为 core_flat exact IP；计划中提到的 `faiss_flat` 交叉核对属于 B4，本阶段未做（B2 用独立 double brute-force 作为交叉验证，等价于 exact IP 的第二实现）。
- `manifest.json` 是未签名的信任根：checksum 校验能捕获意外损坏，但无法防御"同时一致地篡改 manifest 与 artifact"的构造式攻击（需签名，超出本阶段范围）。
- `validate` 的每行去重缓冲固定为 4096，仅在 `topk>4096` 时不完整；本阶段 `topk=10`，不触发。
- 生成器要求以仓库根目录为工作目录（相对 fixture 路径与 `generator.source_sha256` 依赖）。

## 14. 测试及 git 状态

```text
$ make ENABLE_HNSWLIB=1 ENABLE_FAISS=1 -j"$(nproc)"
  ✓ 构建完成（生产主构建不受影响）

$ build/tools/bin/generate_real_embedding_cache validate --dataset-id qa_zh_1k_200_bge_small_zh_v1_5
[validate] PASS ...

$ tests/vector_real_embedding_groundtruth_smoke_test.sh
=== B2 real embedding ground truth smoke test: ALL CHECKS PASSED ===

$ build/tools/bin/generate_real_embedding_groundtruth validate --dataset-id qa_zh_1k_200_bge_small_zh_v1_5
[validate] PASS dataset_id=qa_zh_1k_200_bge_small_zh_v1_5 query_count=200 topk=10 record_count=1000 dim=512

$ git diff --check
（无输出，PASS）

$ git status --short
 M Makefile
?? docs/vector_benchmark_readiness/
?? scripts/benchmark/generate_real_embedding_cache.c
?? scripts/benchmark/generate_real_embedding_groundtruth.c
?? tests/vector_real_embedding_cache_smoke_test.sh
?? tests/vector_real_embedding_groundtruth_smoke_test.sh

$ find /tmp/kvlite_vector_benchmark_artifacts/real_embedding/qa_zh_1k_200_bge_small_zh_v1_5 -maxdepth 2 -type f -printf '%p %s\n'
.../records.jsonl 228013
.../groundtruth.jsonl 109222
.../manifest.json 2618
.../groundtruth.manifest.json ~1.3KB
.../queries.jsonl 36270
.../embeddings.f32 2048000
.../query_embeddings.f32 409600

$ find . -type f -size +10M -not -path './third_party/*' -not -path './models/*' -not -path './.git/*'
（无输出 — 仓库内无新增大文件）
```

Cleanup 命令：

```bash
rm -f /tmp/kvlite_vector_benchmark_artifacts/real_embedding/qa_zh_1k_200_bge_small_zh_v1_5/groundtruth.jsonl \
      /tmp/kvlite_vector_benchmark_artifacts/real_embedding/qa_zh_1k_200_bge_small_zh_v1_5/groundtruth.manifest.json
# 或整套 artifact：
rm -rf /tmp/kvlite_vector_benchmark_artifacts/real_embedding
```

## 15. Decision block

```text
B2-ground-truth-generation decision:
  production_code_changed = no
  product_behavior_changed = no
  tests_changed = yes (new smoke test added, no existing test modified)
  makefile_changed = yes
  docs_changed = yes
  b1_cache_validation_passed = yes
  onnx_inference_rerun = no
  core_flat_exact_path_used = yes
  record_count = 1000
  query_count = 200
  dim = 512
  topk = 10
  groundtruth_schema_frozen = yes
  brute_force_crosscheck_passed = yes
  deterministic_output_passed = yes
  artifacts_outside_repo = yes
  ready_for_B3_core_hnsw_v2_benchmark = yes
  recommended_next = B3-core-hnsw-v2-index-only-benchmark
```
