# B-scale-0: T2Ranking subset generation report

## 1. AGENTS.md 遵守情况

已读取并遵守仓库根目录 `AGENTS.md`。本阶段只新增 benchmark 数据子集生成工具、smoke test 和 benchmark readiness 文档；未修改生产向量代码，未修改 RDB/AOF、`VSEARCH` response、`kvs_vector_index_ops_t`、默认 backend 或默认 HNSW 参数。

冲突说明：`AGENTS.md` 的历史阶段模板要求阶段报告写入 `docs/vector_core_reset/`；当前 benchmark readiness 路线和本任务明确要求所有 benchmark 文档集中在 `docs/vector_benchmark_readiness/`。本报告按任务指定目录输出，同时保持 AGENTS 的生产边界。

## 2. 本阶段范围

B-scale-0 只冻结 T2Ranking 10K / 100K real corpus 子集来源和生成策略。

未执行：

- ONNX embedding。
- exact ground truth。
- core_flat / core_hnsw_v2 / FAISS benchmark。
- 产品 V 命令压测。
- 生产代码修改。

本报告不重复源数据完整性审计；只引用已确认结论：`collection.tsv`、`queries.dev.tsv`、`qrels.retrieval.dev.tsv` 均为合法 UTF-8 两列 TSV，qrels 无 relevance 列且每个 `qid-pid` 均为 positive。

## 3. 实现方式

新增生成器：

```text
scripts/benchmark/generate_t2ranking_subsets.py
```

参数：

```text
--dataset-root
--output-root
--source-revision
--query-count          default 200
--collection-sizes     default 10000,100000
```

输入解析：

- 优先读取 `dataset-root/collection.tsv`、`queries.dev.tsv`、`qrels.retrieval.dev.tsv`。
- 若 root 直放文件不存在，则读取 `dataset-root/data/` 下三份 TSV；本次真实数据使用 `data/` 布局。
- 只做运行必需的文件存在、表头、两列解析、duplicate ID / duplicate qid-pid 检查。

collection 处理：

- 第一遍流式读取 `collection.tsv`，只保留 pid set，不载入 230 万条正文。
- 完成 ID 选择后第二遍流式读取，只缓存最终选中的最多 100K 条正文。

原子发布：

- 先写入同父目录临时目录。
- 临时产物完成自验证后，首次生成通过 `os.replace` 发布输出目录。
- 若输出目录已存在且所有目标文件 checksum 完全一致，则返回 `status=unchanged`。
- 若输出目录已存在但内容不一致，fail-fast，不覆盖已有产物。

## 4. 子集选择算法

固定 revision：

```text
2a369a430a70979223f1b9a41b1919774d46b432
```

算法版本：

```text
t2ranking-subset-v1
```

选择规则：

- 候选 query 必须存在于 `queries.dev.tsv`，并至少有一个 positive pid 存在于 collection。
- Query 排序键：

```text
SHA256(source_revision + "\tquery\t" + qid), tie-break qid
```

- 选择前 200 个 query。
- 保留所选 query 的全部有效 positive qid-pid pair，不裁剪。
- positive 文档先进入 collection 子集；positive pid 排序键：

```text
SHA256(source_revision + "\tdocument\t" + pid), tie-break pid
```

- 非 positive 文档使用同一 document hash 继续补齐到 10K / 100K。
- 100K 在同一文档顺序上继续扩展，因此 10K 是 100K 的前缀子集。
- 输出不依赖源文件行顺序、当前时间、Python hash 或随机数。

## 5. 真实生成结果

真实生成命令：

```bash
python3 scripts/benchmark/generate_t2ranking_subsets.py \
  --dataset-root /home/zhengpan/share/datasets/T2Ranking \
  --output-root /home/zhengpan/share/benchmark_artifacts/T2Ranking/b_scale_0 \
  --source-revision 2a369a430a70979223f1b9a41b1919774d46b432 \
  --query-count 200 \
  --collection-sizes 10000,100000
```

输出目录：

```text
/home/zhengpan/share/benchmark_artifacts/T2Ranking/b_scale_0
```

Manifest 摘要：

```text
selected_query_count = 200
positive_pair_count = 1131
unique_positive_pid_count = 1131
collection_rows[10000] = 10000
collection_rows[100000] = 100000
selected_query_ids_sha256 = a50d92d18bf320a063607d8e7eb0f1a642b5133bed8fe70533fa80621cb8ef28
required_positive_ids_sha256 = 45b033b98c0fb3aa2f8cbef006e5dced6422a3ec2a94ff4396dab9d61d92e1f3
input_layout = data
```

验证结果：

```text
selected_queries_have_positive = true
ids_match_tsv_order = true
nested_prefix = true
positive_pids_present_in_all_collections = true
```

重复运行同一命令结果：

```text
status = unchanged
```

## 6. 输出文件

| file | data rows | bytes | size | sha256 |
|---|---:|---:|---:|---|
| `/home/zhengpan/share/benchmark_artifacts/T2Ranking/b_scale_0/manifest.json` | - | 2498 | 4.0K | `9eb31fdbc37eed14984de5eb373b0047993646161efab4bcbe2c944c1305bddb` |
| `/home/zhengpan/share/benchmark_artifacts/T2Ranking/b_scale_0/queries.dev.200.tsv` | 200 | 7504 | 8.0K | `00b7641ac19a3b731243f4a62251757b1e7520efd28946e10d226525fff15ab2` |
| `/home/zhengpan/share/benchmark_artifacts/T2Ranking/b_scale_0/qrels.retrieval.dev.200.tsv` | 1131 | 14059 | 16K | `97221f7ca305c817e41da2e1cee3e136b38e3fb563586b2ed5e06cba7563af1f` |
| `/home/zhengpan/share/benchmark_artifacts/T2Ranking/b_scale_0/collection.10k.tsv` | 10000 | 16698103 | 16M | `7d3155be4ad6c5a1c74c3d132e4318e16665533f378cbe04e4259579aea4b190` |
| `/home/zhengpan/share/benchmark_artifacts/T2Ranking/b_scale_0/collection.100k.tsv` | 100000 | 160070563 | 153M | `738f03d1a1b063a1957fb627669c9520b75b4745e1475d2740eabcc1b630736c` |
| `/home/zhengpan/share/benchmark_artifacts/T2Ranking/b_scale_0/collection.10k.ids` | 10000 | 74403 | 76K | `e634a5052aa8d106feb61e8b6d56a89c811a89aebc807f9ddc35e1390fe55440` |
| `/home/zhengpan/share/benchmark_artifacts/T2Ranking/b_scale_0/collection.100k.ids` | 100000 | 751284 | 736K | `361a6aa41fe23788ccf362494a9387e8d862568502fa112eaa0b305170adc1fb` |

TSV 文件保留表头；`.ids` 文件无表头，且 ID 顺序与对应 collection TSV 数据行一致。

## 7. 测试结果

```text
$ python3 -m py_compile scripts/benchmark/generate_t2ranking_subsets.py
PASS

$ tests/vector_t2ranking_subset_generator_smoke_test.sh
[PASS] source row order does not affect output
[PASS] repeated run is deterministic
[python] manifest and output invariants passed
[PASS] positive retention, nesting, and id order validated
[PASS] duplicate_pid failed as expected
[PASS] duplicate_qid failed as expected
[PASS] bad_header failed as expected
[PASS] insufficient_data failed as expected
[PASS] vector_t2ranking_subset_generator_smoke_test completed
```

真实生成后重复运行同一命令：

```text
status = unchanged
```

## 8. git 检查

```text
$ git diff --check
PASS (no output)

$ git status --short
 M docs/vector_benchmark_readiness/README.md
 M docs/vector_benchmark_readiness/index.md
?? docs/vector_benchmark_readiness/b_scale_0_t2ranking_subset_generation_report.md
?? scripts/benchmark/generate_t2ranking_subsets.py
?? tests/vector_t2ranking_subset_generator_smoke_test.sh
```

未提交 commit。

## 9. 当前限制与风险

- 本阶段只生成 text corpus 子集，不包含 embedding cache。
- 输出位于仓库外 `/home/zhengpan/share/benchmark_artifacts/T2Ranking/b_scale_0`，不应加入 git。
- 生成器对 duplicate pid、duplicate qid、duplicate qid-pid 采取 fail-fast；若未来源数据策略允许重复行，需要显式调整。
- 当前 10K / 100K 只冻结 corpus 选择，不代表 ANN recall、build/search latency 或 product e2e SLA。

## 10. 下一阶段

建议进入：

```text
B-scale-1 — T2Ranking 10K Real Embedding Cache
```

B-scale-1 应复用本阶段 `queries.dev.200.tsv`、`qrels.retrieval.dev.200.tsv`、`collection.10k.tsv` 和 manifest checksum，生成一次可复用 10K real embedding cache；不得重新选择 query/document 子集。
