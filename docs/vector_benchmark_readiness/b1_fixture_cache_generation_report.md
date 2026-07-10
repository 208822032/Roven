# B1-fixture-cache-generation: real embedding cache generation report

## 1. AGENTS.md 遵守情况

已读取并遵守仓库根目录 `AGENTS.md`，并读取本阶段要求的：

- `docs/vector_benchmark_readiness/README.md`
- `docs/vector_benchmark_readiness/index.md`
- `docs/vector_benchmark_readiness/benchmark_plan_real_embedding.md`
- `docs/vector_benchmark_readiness/b0_preflight_report.md`
- `docs/vector_benchmark_readiness/v_family_implementation_audit_report.md`

冲突说明：`AGENTS.md` 的历史阶段模板要求阶段报告写入 `docs/vector_core_reset/`；本任务明确要求写入 `docs/vector_benchmark_readiness/`。本报告按任务指定目录输出，同时继续遵守 AGENTS 的核心边界：不修改生产 V 命令代码、不修改 RDB/AOF、不修改 `VSEARCH` response、不修改 `kvs_vector_index_ops_t`、不让 vector core 访问 manager-owned 状态。本阶段新增的内容仅限于 `scripts/benchmark/`、`tests/`（新增一个独立 smoke test，不改动既有测试）、`Makefile`（新增显式 opt-in target）与本目录文档。

本报告只记录本阶段实际实现，不重复既有文档已经写过的路线背景、B0 依赖结论和通用边界。

## 2. 实际实现

新增文件：

```text
scripts/benchmark/generate_real_embedding_cache.c   生成器 + validate-only，单一 C 程序
tests/vector_real_embedding_cache_smoke_test.sh      smoke test
docs/vector_benchmark_readiness/b1_fixture_cache_generation_report.md  本报告
```

修改文件：

```text
Makefile   新增 gen-embedding-cache / smoke-vector-real-embedding-cache 两个显式 opt-in target
docs/vector_benchmark_readiness/README.md   标记 B1 完成，更新后续建议
docs/vector_benchmark_readiness/index.md    标记 B1 完成，链接本报告
```

生产代码（`src/`、`include/`）未修改；`kvs_config.c`、`embedding_runtime_onnx.c` 按只读方式编译进新增独立二进制，源码零改动。

## 3. TSV 实际 schema（不假定，逐字节核对）

`tests/fixtures/qa_zh_1000.tsv`（1000 行）与 `tests/fixtures/qa_zh_queries.tsv`（200 行）均为：

```text
2 列，tab 分隔，无表头
records: question<TAB>answer
queries : query<TAB>expected_answer
UTF-8 编码
```

生成器对每行做了强校验：必须恰好一个 tab（拒绝额外 tab）、两列均非空，否则打印 `文件:行号` 定位信息并 fail-fast，不产出任何 artifact。

## 4. runtime / tokenizer 复用方式

生成器直接编译并链接两个未修改的生产源文件，不经过 `src/vector/kvs_vector_embedding.c` 的 provider dispatch：

```text
src/embedding/embedding_runtime_onnx.c   真实 ONNX 推理 + BERT WordPiece tokenizer（同一份生产代码）
src/core/kvs_config.c                    kvs_config_init_default()，仅用于填充 ONNX 会话线程/执行模式配置
```

链接方式（Makefile 新增 target，见第 8 节）：

```text
不链接 libkvstore.a，不链接 src/embedding/embedding_runtime.c（hash provider）
generate_real_embedding_cache.c 自身定义全局 kvs_config_t g_cfg（embedding_runtime_onnx.c 通过 extern 引用）
```

因为 hash provider 的目标文件根本不会出现在这个二进制的链接命令里，"不使用 hash fallback" 是链接期不可能违反的约束，而不是仅靠调用约定去回避。

Embedding 调用与归一化流程与 `kvs_vector_embedding.c` 保持一致：`embedding_encode` / `embedding_encode_batch` 输出未归一化的原始向量（ONNX runtime 本身不做归一化），由调用方在 double 精度下做 L2 normalize；这与生产路径 `kvs_vector_embedding_l2_normalize_batch()` 的算法一致，只是在生成器里独立实现了一份（不复用生产函数本身，因为该函数不在 `embedding_c_api.h` 暴露的 ABI 里）。

字节序：写入前显式探测宿主机字节序（`host_is_little_endian()`），非小端主机会在写入/读回时逐 float 做字节交换，保证 `embeddings.f32` / `query_embeddings.f32` 始终是 little-endian，不依赖宿主机实际字节序假设。

## 5. sanity 结果

生成器在批量推理前会跑一次小规模 sanity gate（对前 3 条中文记录文本），失败则在产出任何 artifact 前直接 abort：

```text
zh_samples_checked = 3
determinism_max_abs_diff = 0.000000000   (阈值 <= 1e-6)
determinism_cosine       = 1.000000000   (阈值 >= 0.999999)
distinct_cosine          = 0.857237405   (阈值 <= 0.999999，越低越好)
embedding_runtime_confirmed = onnxruntime
```

determinism 检查采用 max_abs_diff + cosine 双重阈值，而不是要求 byte-identical——ONNX Runtime 在固定线程数、`sequential` 执行模式下对同一输入是数值稳定的，但不强制要求逐 bit 相等。

## 6. artifact / manifest schema

Artifact 目录（每个 dataset_id 一份）：

```text
manifest.json
embeddings.f32          row-major little-endian float32, [record_count, dim], 无 header
query_embeddings.f32    同上, [query_count, dim]
records.jsonl           {"row","source_line","id","question","answer","text"}，text = question（embedding 源文本）
queries.jsonl           {"query_row","source_line","id","text","expected_answer"}
```

`manifest.json` 顶层字段：`schema_version`、`dataset_id`、`created_at_utc`、`generator`（含 `source_sha256`）、`model`（id/path/dim/runtime/onnxruntime_version/normalization/metric/checksums）、`inputs`（两个 TSV 的 path/sha256/size_bytes/rows）、`outputs`（四个 artifact 的 path/sha256/size_bytes-or-rows/shape）、`params`（record_count/query_count/dim/dtype/byte_order/normalized/metric/batch_size）、`sanity`、`timing_us`。完整字段清单和实际生成值见第 7 节。

这与 `benchmark_plan_real_embedding.md` 冻结的 fixture/cache 设计一致（row-major/no-header/little-endian/L2-normalized/IP metric），但 B1 的 schema 更精简：只覆盖 B1 明确要求的字段，不提前实现 `groundtruth.jsonl`、`scope`、`topk` 等属于 B2 及以后阶段的内容。

## 7. 实际生成结果和耗时

Dataset：`qa_zh_1k_200_bge_small_zh_v1_5`，位于 `/tmp/kvlite_vector_benchmark_artifacts/real_embedding/qa_zh_1k_200_bge_small_zh_v1_5/`。

```text
record_count = 1000
query_count  = 200
dim          = 512
batch_size   = 32
num_threads  = 1
onnxruntime_version = 1.24.4
embeddings.f32       2,048,000 bytes  (1000*512*4)
query_embeddings.f32   409,600 bytes  (200*512*4)
records.jsonl           228,013 bytes
queries.jsonl            36,270 bytes
```

耗时（`timing_us`，来自 manifest）：

```text
model_load_us               = 70,471
record_embedding_total_us   = 5,823,638   avg_us = 5,823
query_embedding_total_us    = 1,153,006   avg_us = 5,765
artifact_write_us           = 4,743
validation_us               = 2,202
total_us                    = 7,141,324   (~7.14s wall clock)
```

Checksums（sha256，摘录）：

```text
model.onnx       = 69b353bb2aa2d09ab606ddbbc35437b03c843615a6bff28216a37fee7309c2aa
model.onnx_data  = e72da961b03613124aa11317470c995ca197651a9d7f6be2b0e90aad92f71df0
vocab.txt        = 45bbac6b341c319adc98a532532882e91a9cefc0329aa57bac9ae761c27b291c
embeddings.f32   = 57dc5611ee5d30a6da11a7efe2e0393a19da106bad5dae4f63d5cd7a5a4506ce
query_embeddings.f32 = fc3117ef25dfd8e2137b0df836d776e697c39c3934e8b4018b0b9f2c5c5058c6
records.jsonl    = 720bf45ae6b543106bacd26e728697e5ab4a28c5d30b06a997dff575baa80df2
queries.jsonl    = e8c8dbbd7fb4a9c82f720f67d99f6893d97f15b601bc08444eea81639eea1a9c
```

样例 `records.jsonl` 行：

```json
{"row":0,"source_line":1,"id":0,"question":"什么是Redis AOF？","answer":"Redis AOF 是追加日志持久化机制，通过顺序写记录写命令。","text":"什么是Redis AOF？"}
```

样例 `queries.jsonl` 行：

```json
{"query_row":0,"source_line":1,"id":0,"text":"请解释一下Redis AOF","expected_answer":"Redis AOF 是追加日志持久化机制，通过顺序写记录写命令。"}
```

## 8. 校验结果

`validate` 子命令不调用 `embedding_create`/ONNX（代码路径上完全不可达，只有 `generate` 子命令会走到 ONNX 初始化）：

```text
$ ./build/tools/bin/generate_real_embedding_cache validate \
    --dataset-id qa_zh_1k_200_bge_small_zh_v1_5
[validate] PASS dataset_id=qa_zh_1k_200_bge_small_zh_v1_5 record_count=1000 query_count=200 dim=512
```

validate 内部做的检查（严格 schema 校验，缺字段/类型不符/取值不符均 fail）：

```text
schema_version == 1
dataset_id 与请求一致
params.{record_count,query_count,dim} 存在且为正
params.dtype == "float32"，byte_order == "little"，metric == "ip_normalized"，normalized == true
outputs.{embeddings_f32,query_embeddings_f32,records_jsonl,queries_jsonl} 各自的 path/sha256(64 hex)/size_bytes-or-rows 存在
四个 artifact 文件存在，大小与 manifest 一致，sha256 与 manifest 一致
两个 .f32 文件 shape 与 record_count*dim*4 / query_count*dim*4 匹配，逐行 finite + norm≈1（容差 1e-3）
两个 .jsonl 行数与 record_count/query_count 一致
```

Smoke test（`tests/vector_real_embedding_cache_smoke_test.sh`）使用独立的一次性 dataset id（`b1_smoke_<pid>_<epoch>`，trap 清理，不触碰上面这份正式 dataset），完整跑通并全部通过：

```text
[PASS] generate exited 0 with real ONNX inference
[PASS] manifest fields: record_count=1000 query_count=200 dim=512, checksums well-formed
[PASS] validate-only passed on freshly generated dataset
[PASS] sampled >=10 record/query vectors are finite and L2-normalized
[PASS] regenerating the same dataset id fails fast without touching the existing cache
[PASS] validate-only fails on a corrupted cache copy (sha256 mismatch detected)

=== B1 real embedding cache smoke test: ALL CHECKS PASSED ===
```

抽样点说明：`records.jsonl`/`queries.jsonl` 抽样均 >=10 行（脚本按 `count/10` 步长抽样，覆盖 1000/200 行的分布，而不是只测前 10 行）。

## 9. 当前限制

- 仅覆盖仓库现有 smoke 规模（1000 条 records / 200 条 queries），未冻结/引入大规模 real benchmark corpus——`benchmark_plan_real_embedding.md` 中提到的"大规模真实语料源"仍未确定，留给需要 100K+/1M 规模真实压测的阶段单独解决。
- `records.jsonl` 目前不含 `scope`；B2/B6/B7 若需要 scope 分组语义，需要在读取本阶段产出的 `records.jsonl` 后自行补充或在后续阶段扩展 schema（不属于 B1 范围）。
- `manifest.json` 里没有单独记录生成器二进制的编译 flags/CPU 型号；只记录了源码 sha256 和 ONNX Runtime 版本，复现性依赖使用者用同一份仓库/同一个 `make gen-embedding-cache` 重新编译。
- 生成器要求以仓库根目录为当前工作目录运行（同现有 `tests/*.sh` 约定），否则 `records-tsv`/`queries-tsv`/`model-path` 等相对路径和 `generator.source_sha256` 会解析失败。

## 10. B2 输入契约

B2（core_flat ground truth）只允许读取以下 5 个文件，且必须先调用 `validate` 子命令确认通过后才能使用，不得重新调用 ONNX：

```text
manifest.json
embeddings.f32
query_embeddings.f32
records.jsonl
queries.jsonl
```

对应目录：

```text
/tmp/kvlite_vector_benchmark_artifacts/real_embedding/qa_zh_1k_200_bge_small_zh_v1_5/
```

## 11. cleanup 命令

```bash
rm -rf /tmp/kvlite_vector_benchmark_artifacts/real_embedding/qa_zh_1k_200_bge_small_zh_v1_5
# 或清理该阶段全部 artifact：
rm -rf /tmp/kvlite_vector_benchmark_artifacts/real_embedding
```

## 12. 测试与 git 状态

必跑命令与结果：

```text
$ make ENABLE_HNSWLIB=1 ENABLE_FAISS=1 -j"$(nproc)"
  ✓ 构建完成（kvstore / src/eBPF/ebpf_syncd / eBPF 对象，均使用默认 hash embedding provider，未受本阶段改动影响）

$ tests/vector_real_embedding_cache_smoke_test.sh
=== B1 real embedding cache smoke test: ALL CHECKS PASSED ===

$ ./build/tools/bin/generate_real_embedding_cache validate --dataset-id qa_zh_1k_200_bge_small_zh_v1_5
[validate] PASS dataset_id=qa_zh_1k_200_bge_small_zh_v1_5 record_count=1000 query_count=200 dim=512

$ git diff --check
(no output, PASS)

$ git status --short
 M Makefile
?? docs/vector_benchmark_readiness/
?? scripts/benchmark/generate_real_embedding_cache.c
?? tests/vector_real_embedding_cache_smoke_test.sh

$ find /tmp/kvlite_vector_benchmark_artifacts/real_embedding -maxdepth 3 -type f -printf '%p %s\n'
.../qa_zh_1k_200_bge_small_zh_v1_5/records.jsonl 228013
.../qa_zh_1k_200_bge_small_zh_v1_5/manifest.json 2618
.../qa_zh_1k_200_bge_small_zh_v1_5/queries.jsonl 36270
.../qa_zh_1k_200_bge_small_zh_v1_5/embeddings.f32 2048000
.../qa_zh_1k_200_bge_small_zh_v1_5/query_embeddings.f32 409600

$ find . -type f -size +10M -not -path './third_party/*' -not -path './models/*' -not -path './.git/*'
(no output — no new large files in the repo)
```

## 13. Decision block

```text
B1-fixture-cache-generation decision:
  production_code_changed = no
  product_behavior_changed = no
  tests_changed = yes (new smoke test added, no existing test modified)
  makefile_changed = yes
  docs_changed = yes
  real_onnx_inference_confirmed = yes
  hash_fallback_used = no
  record_count = 1000
  query_count = 200
  dim = 512
  vectors_normalized = yes
  cache_schema_frozen = yes
  manifest_checksums_valid = yes
  validate_only_passed = yes
  artifacts_outside_repo = yes
  ready_for_B2_ground_truth_generation = yes
  recommended_next = B2-ground-truth-generation
```
