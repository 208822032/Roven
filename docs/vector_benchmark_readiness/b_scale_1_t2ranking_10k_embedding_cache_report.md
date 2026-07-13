# B-scale-1: T2Ranking 10K real embedding cache report

## 1. AGENTS.md 遵守情况

已读取并遵守仓库根目录 `AGENTS.md`。本阶段只扩展 benchmark 工具、测试和 `docs/vector_benchmark_readiness/` 文档；未修改生产 V 命令代码，未修改 RDB/AOF，未修改 `VSEARCH` response，未修改 `kvs_vector_index_ops_t`，未修改默认 backend 或默认 HNSW 参数。

冲突说明：`AGENTS.md` 的历史阶段模板要求阶段报告写入 `docs/vector_core_reset/`；当前 benchmark readiness 路线要求所有 benchmark 文档集中在 `docs/vector_benchmark_readiness/`。本报告按任务指定目录输出，同时保持 AGENTS 的生产边界。

## 2. 阶段目标和边界

目标：复用现有 B1 `generate_real_embedding_cache.c`，让它支持外部 TSV 输入和外部输出目录，并基于 B-scale-0 冻结的 T2Ranking 10K corpus 生成真实 bge-small-zh-v1.5 embedding cache。

本阶段未执行：

- B-scale-0 子集重新生成。
- T2Ranking 源数据完整性审计。
- synthetic / random / hash / mock embedding。
- exact ground truth。
- core_flat / core_hnsw_v2 / FAISS benchmark。
- 产品 V 命令压测。

## 3. 输入、模型和输出

输入：

```text
records_tsv = /home/zhengpan/share/benchmark_artifacts/T2Ranking/b_scale_0/collection.10k.tsv
queries_tsv = /home/zhengpan/share/benchmark_artifacts/T2Ranking/b_scale_0/queries.dev.200.tsv
```

输入 checksum：

```text
collection.10k.tsv sha256 = 7d3155be4ad6c5a1c74c3d132e4318e16665533f378cbe04e4259579aea4b190
queries.dev.200.tsv sha256 = 00b7641ac19a3b731243f4a62251757b1e7520efd28946e10d226525fff15ab2
```

模型：

```text
model_dir = /home/zhengpan/share/KVLite/9.1-kvstore/models/bge-small-zh-v1.5
runtime = ONNX Runtime 1.24.4
dim = 512
dtype = float32
normalized = true
metric = ip_normalized
```

模型 checksum：

```text
model.onnx = 69b353bb2aa2d09ab606ddbbc35437b03c843615a6bff28216a37fee7309c2aa
model.onnx_data = e72da961b03613124aa11317470c995ca197651a9d7f6be2b0e90aad92f71df0
vocab.txt = 45bbac6b341c319adc98a532532882e91a9cefc0329aa57bac9ae761c27b291c
```

输出目录：

```text
/home/zhengpan/share/benchmark_artifacts/T2Ranking/b_scale_1_10k_embedding
```

## 4. 对现有 B1 工具的改造

审计结论：

- 现有 `scripts/benchmark/generate_real_embedding_cache.c` 已直接链接 `src/embedding/embedding_runtime_onnx.c` 和 `src/core/kvs_config.c`，不会链接 hash fallback。
- 现有 schema 已被 B2/B3/B4 使用：`manifest.json`、`embeddings.f32`、`query_embeddings.f32`、`records.jsonl`、`queries.jsonl`。
- 下游 ground truth / HNSW / FAISS 工具依赖 numeric row id：`record_id == records.jsonl row`，`query_row == queries.jsonl row`。
- 因此本阶段复用并最小扩展现有工具，没有新增 T2Ranking 专用 embedding 生成器。

实际改造：

- 新增 `--model-dir`，等价于 `${model_dir}/model.onnx`。
- 新增 `--output-dir`，支持直接发布到指定目录；旧 `--out-root + --dataset-id` 保留。
- 新增 `--input-format id_text`，要求 `pid<TAB>text` / `qid<TAB>text` 表头，按第一个 TAB 分割 ID 和 text，保留原始 pid/qid。
- 新增 `--expected-record-count` / `--expected-query-count`，在 ONNX 初始化前 fail-fast。
- 新增 `--max-text-len` 参数；T2Ranking 使用较大的 raw text guard。
- `records.jsonl` / `queries.jsonl` 保留旧字段，并额外记录 `external_id`、`pid` / `qid`。numeric `id` 仍为 row number，保持下游 record_id 兼容。
- `--output-dir` 已存在且核心文件一致时返回 `status=unchanged core_outputs_identical=true`；旧 B1 默认路径仍保留已存在 fail-fast 语义。
- manifest 增加 tokenizer、validation、num_threads、input_format、expected counts、command_line 和 peak RSS 信息。

## 5. tokenizer / batch / thread 配置

```text
batch_size = 32
num_threads = 1
max_text_len = 1048576
effective max_tokens = 512
truncation = runtime_token_cap
```

说明：

- ONNX runtime 的 `max_text_len` 是 raw byte guard。
- `embedding_runtime_onnx.c` 内部把 token 数 capped at `EMBEDDING_ONNX_MAX_TOKENS = 512`。
- 首次使用 `max_text_len=512` 和 `65536` 均因 T2Ranking 长文档超过 raw guard 失败；最终使用 `1048576` 允许长文档进入 tokenizer，同时仍按 512 token 上限截断。

## 6. 真实生成命令

```bash
mkdir -p /home/zhengpan/share/benchmark_artifacts/T2Ranking/b_scale_1_10k_embedding

build/tools/bin/generate_real_embedding_cache generate \
  --dataset-id b_scale_1_10k_embedding \
  --records-tsv /home/zhengpan/share/benchmark_artifacts/T2Ranking/b_scale_0/collection.10k.tsv \
  --queries-tsv /home/zhengpan/share/benchmark_artifacts/T2Ranking/b_scale_0/queries.dev.200.tsv \
  --input-format id_text \
  --model-dir /home/zhengpan/share/KVLite/9.1-kvstore/models/bge-small-zh-v1.5 \
  --output-dir /home/zhengpan/share/benchmark_artifacts/T2Ranking/b_scale_1_10k_embedding \
  --expected-record-count 10000 \
  --expected-query-count 200 \
  --dim 512 \
  --batch-size 32 \
  --num-threads 1 \
  --max-text-len 1048576
```

真实推理证据：

```text
runtime = onnxruntime
model = /home/zhengpan/share/KVLite/9.1-kvstore/models/bge-small-zh-v1.5/model.onnx
embedding_runtime_confirmed = onnxruntime
synthetic/mock/hash embedding = not used
```

## 7. 生成结果

```text
record_count = 10000
query_count = 200
dim = 512
embeddings.f32 bytes = 20480000
query_embeddings.f32 bytes = 409600
records.jsonl rows = 10000
queries.jsonl rows = 200
```

耗时：

```text
model_load_us = 176642
record_embedding_total_us = 1707950545
record_embedding_avg_us = 170795
query_embedding_total_us = 1096029
query_embedding_avg_us = 5480
artifact_write_us = 182041
validation_us = 152760
total_us = 1710704368
peak_rss_kb = 1958848
```

吞吐：

```text
record throughput = 5.855 records/s
query throughput = 182.477 queries/s
overall throughput = 5.962 items/s
```

解释：records 是长文档并按 512 token cap 进入 ONNX；queries 是短文本，因此 query throughput 显著更高。本阶段记录的是 artifact 生成成本，不是 core index-only 或 product VSEARCH 性能。

## 8. 输出文件

| file | rows / shape | bytes | sha256 |
|---|---:|---:|---|
| `/home/zhengpan/share/benchmark_artifacts/T2Ranking/b_scale_1_10k_embedding/manifest.json` | - | 4010 | `ff349b42dc6ed07fc492b0163df536c89783c0998cbc61be0ec974702953ac9b` |
| `/home/zhengpan/share/benchmark_artifacts/T2Ranking/b_scale_1_10k_embedding/embeddings.f32` | `[10000,512]` | 20480000 | `aeead23e186bc73da2d92ce52885bf5e1763fbed5cb430059ba65abf21c77fc1` |
| `/home/zhengpan/share/benchmark_artifacts/T2Ranking/b_scale_1_10k_embedding/query_embeddings.f32` | `[200,512]` | 409600 | `2b1d3e5952051dcf4eee938b6edf3454fef1660f16aa0e319d9e3d53e8dca318` |
| `/home/zhengpan/share/benchmark_artifacts/T2Ranking/b_scale_1_10k_embedding/records.jsonl` | 10000 | 34409206 | `7372ebe3fd264dd2bf5276cc07952c68d1bc2b9500f4d19a8350b7adfbfdf981` |
| `/home/zhengpan/share/benchmark_artifacts/T2Ranking/b_scale_1_10k_embedding/queries.jsonl` | 200 | 28094 | `d4ced627ace77b2f806a7206944be98c0b7d9b170c84038dc9a4ca1cfaad45b4` |

## 9. 验证结果

Validate-only：

```text
$ build/tools/bin/generate_real_embedding_cache validate \
  --dataset-id b_scale_1_10k_embedding \
  --output-dir /home/zhengpan/share/benchmark_artifacts/T2Ranking/b_scale_1_10k_embedding
[validate] PASS dataset_id=b_scale_1_10k_embedding record_count=10000 query_count=200 dim=512
```

独立 Python 检查：

```text
records.jsonl rows = 10000
queries.jsonl rows = 200
embeddings.f32 bytes = 20480000
query_embeddings.f32 bytes = 409600
record_norm_min = 0.9999999699725455
record_norm_max = 1.0000000310473591
record_norm_max_abs_delta = 3.104735912806689e-08
query_norm_min = 0.9999999708152267
query_norm_max = 1.0000000289819126
query_norm_max_abs_delta = 2.9184773264745445e-08
id_order_alignment = true
```

ID/order 对齐：

```text
collection.10k.tsv 第 N 条 pid
  == records.jsonl 第 N 行 pid/external_id
  == embeddings.f32 第 N 个 512d 向量

queries.dev.200.tsv 第 N 条 qid
  == queries.jsonl 第 N 行 qid/external_id
  == query_embeddings.f32 第 N 个 512d 向量
```

## 10. 确定性结果

重复运行同一 generate 命令，重新执行真实 ONNX 推理到临时目录后比对核心文件：

```text
[publish] dataset_id=b_scale_1_10k_embedding dir=/home/zhengpan/share/benchmark_artifacts/T2Ranking/b_scale_1_10k_embedding status=unchanged core_outputs_identical=true
```

重复运行的核心文件 byte-level SHA-256 与首次发布一致：

```text
embeddings.f32 = aeead23e186bc73da2d92ce52885bf5e1763fbed5cb430059ba65abf21c77fc1
query_embeddings.f32 = 2b1d3e5952051dcf4eee938b6edf3454fef1660f16aa0e319d9e3d53e8dca318
records.jsonl = 7372ebe3fd264dd2bf5276cc07952c68d1bc2b9500f4d19a8350b7adfbfdf981
queries.jsonl = d4ced627ace77b2f806a7206944be98c0b7d9b170c84038dc9a4ca1cfaad45b4
```

## 11. 测试结果

```text
$ make gen-embedding-cache
PASS

$ tests/vector_real_embedding_cache_smoke_test.sh
=== B1 real embedding cache smoke test: ALL CHECKS PASSED ===
```

Smoke test 现在同时覆盖：

- 旧 B1 1K fixture 调用方式。
- 旧 B1 已存在 dataset id fail-fast 语义。
- validate-only 不调用 ONNX。
- 外部 `id_text` TSV 表头解析。
- pid/qid 保留。
- 行数与向量数对齐。
- f32 尺寸、finite、L2 normalized。
- 外部 `--output-dir` 重复生成 core outputs byte-identical。
- 错误表头失败。
- expected count mismatch 失败。

## 12. 当前限制和风险

- `records.jsonl` 保留 numeric `id=row` 以兼容 B2/B3/B4；T2Ranking 原始 pid 记录在 `external_id` / `pid` 字段。
- `max_text_len=1048576` 是 raw byte guard；实际语义截断仍是 512 tokens。后续报告必须区分 raw text guard 与 token cap。
- 生成过程 peak RSS 约 1.9GB；这是 ONNX artifact 生成成本，不应混入 ANN index benchmark。
- 本阶段只生成 10K cache；100K embedding cache 尚未生成。

## 13. 下一阶段

建议进入：

```text
B-scale-2 — T2Ranking 10K Exact Ground Truth
```

B-scale-2 应读取本阶段输出的 `manifest.json`、`embeddings.f32`、`query_embeddings.f32`、`records.jsonl`、`queries.jsonl`，先执行 validate-only，再生成 exact ground truth；不得重新调用 ONNX，不得重新选择 T2Ranking 子集。

## 14. git 检查

```text
$ git diff --check
PASS (no output)

$ git status --short
 M docs/vector_benchmark_readiness/README.md
 M docs/vector_benchmark_readiness/index.md
 M scripts/benchmark/generate_real_embedding_cache.c
 M tests/vector_real_embedding_cache_smoke_test.sh
?? docs/interview-prep/KVLite项目复盘（十四）：真实Embedding接入与ONNX本地推理.md
?? docs/vector_benchmark_readiness/b_scale_0_t2ranking_subset_generation_report.md
?? docs/vector_benchmark_readiness/b_scale_1_t2ranking_10k_embedding_cache_report.md
?? scripts/benchmark/generate_t2ranking_subsets.py
?? tests/vector_t2ranking_subset_generator_smoke_test.sh
```

说明：`docs/interview-prep/...` 是本阶段外的未跟踪文件，未处理。B-scale-0 未提交改动保留；本阶段未提交 commit。
