# B-scale-4A: 100K embedding parallel throughput preflight

## 1. 范围与结论

本阶段只扩展 real embedding cache benchmark 工具的连续行分片、断点续跑和确定性合并能力，并在 B-scale-0/B-scale-1 冻结输入上做 2K records 吞吐预检。未修改模型、512 token cap、float32、L2 normalization、输入顺序或生产向量代码；未执行完整 90K、ground truth、HNSW 或 FAISS。

当前 VM 未通过 `aggregate throughput >= 28 records/s` 门禁。最佳完整配置为 `2p x 2t / batch32 = 18.058 records/s`，预计新增 90K 约需 83.1 分钟。完整 90K 生成不得在该结果上启动。

## 2. 环境与输入

```text
host: VMware / AMD EPYC 9654
vCPU: 4
RAM: 3.8 GiB
swap: 3.8 GiB
model: models/bge-small-zh-v1.5/model.onnx
dim/dtype/normalization: 512 / float32 / L2
token cap: 512 (unchanged)
source records: B-scale-0 collection.100k.tsv
measured range: [10000, 12000), 2000 real records
base cache: B-scale-1 first 10000 records and all 200 queries
```

base cache 的 numeric row、pid 与 `collection.100k.tsv` 前 10K 逐行核对通过。query 文件直接复用，不做 query ONNX inference：

```text
query_embeddings.f32 = 2b1d3e5952051dcf4eee938b6edf3454fef1660f16aa0e319d9e3d53e8dca318
queries.jsonl         = d4ced627ace77b2f806a7206944be98c0b7d9b170c84038dc9a4ca1cfaad45b4
```

## 3. 实现

`generate_real_embedding_cache` 新增显式 `--start-row/--end-row` 和 `--query-cache-dir`：

- row range 使用 `[start,end)`，TSV 只缓存 range 内文本。
- shard JSONL 保留全局 numeric `row/id`、原始 pid 和源文件行号。
- 每个 worker 是独立进程、独立 ONNX session；`num_threads` 由配置显式指定。
- shard 仍输出独立 f32/JSONL/manifest，并由原 validate 路径检查 checksum、shape、finite 和 norm。
- 未传 range/query cache 参数时，原 B1 `generate/validate` 行为保持不变。

新增 `run_real_embedding_cache_shards.py`：

- 启动固定数量独立 worker，并记录 wall time、child CPU 和 worker peak RSS。
- resume 前先 validate shard 的 checksum、range、参数、source checksum 和全局 row/id。
- merge 拒绝 overlap/gap/provenance mismatch，按全局 row 顺序拼接 base + shards。
- merge 复用 B-scale-1 query cache，在临时目录完成后用 `os.replace` 原子发布。
- 可选 reference gate 对四个核心输出逐字节比较。
- worker 失败时不发布 shard/merged cache，并清理对应临时目录。

## 4. 2K 吞吐结果

所有成功配置使用相同 `[10000,12000)`、相同 base、相同输入顺序。RSS 为 worker 自报 peak；多进程行的 sum peak 是各 worker peak 之和。

| config | batch | wall_s | records/s | CPU | peak RSS KiB | byte-identical |
|---|---:|---:|---:|---:|---:|---|
| 1p x 1t | 32 | 346.300 | 5.775 | 99.5% | 1,944,368 | reference |
| 1p x 4t | 32 | 111.364 | 17.959 | 362.6% | 1,946,916 | yes |
| 1p x 4t | 64 | 124.166 | 16.107 | 334.9% | 2,930,880 | yes |
| 2p x 2t | 32 | 110.755 | 18.058 | 348.3% | 3,388,328 sum | yes |
| 2p x 2t | 64 | stopped | n/a | swap-bound | >3,009,980 observed | no artifact published |
| 4p x 1t | 32/64 | not started | n/a | resource gate | projected unsafe | n/a |
| 5p x 1t | 32/64 | not started | n/a | host has 4 vCPU | projected unsafe | n/a |

`2p x 2t / batch64` 在约 32 秒时只剩 55 MiB available RAM、已占用 2.84 GiB swap，两个 worker CPU 分别降至约 51%/44%。继续执行会把换页/OOM 当成推理吞吐，因此按资源门禁终止。单进程 batch32 已使用约 1.9 GiB，4/5 个独立 session 明显超过当前物理 RAM；batch64 单进程约 2.86 GiB，风险更高。

## 5. 确定性与 checksum

四个完整配置的 merged core outputs 一致：

```text
embeddings.f32       size=24,576,000  sha256=177d34484a4bde00d3fa9d98bac48a1e3fef441af44c3e27e48106e9648fe46b
records.jsonl        size=40,890,248  sha256=5c0349666ba01a8382ae9fd899fe60e6ec14f023d43ff308efd316f571b89803
query_embeddings.f32 size=409,600     sha256=2b1d3e5952051dcf4eee938b6edf3454fef1660f16aa0e319d9e3d53e8dca318
queries.jsonl        size=28,094      sha256=d4ced627ace77b2f806a7206944be98c0b7d9b170c84038dc9a4ca1cfaad45b4
```

这证明 process/thread/batch 变化没有改变 float32 bytes、finite/norm、pid/global row 顺序或 query cache。artifact 位于：

```text
/tmp/kvlite_vector_benchmark_artifacts/b_scale_4a_embedding_parallel_preflight/
```

## 6. 测试与验证

```text
make gen-embedding-cache
PASS

tests/vector_real_embedding_cache_smoke_test.sh
PASS

tests/vector_real_embedding_cache_parallel_shard_smoke_test.sh
PASS: monolithic/sharded byte-identical, global row/id, resume, corrupt rejection

run_real_embedding_cache_shards.py validate (4 successful configurations)
PASS

git diff --check
PASS
```

smoke 只使用 disposable synthetic fixture；真实 preflight 未修改 B-scale-0/B-scale-1 正式 artifact。未调用 ground truth/HNSW/FAISS。

## 7. 推荐配置与下一步

当前 4 vCPU VM 的最佳可运行配置是 `2p x 2t / batch32`，但它和 `1p x 4t / batch32` 基本持平，说明 CPU 已饱和；batch64 增加内存且降低吞吐。该 VM 不满足约一小时生成目标。

建议提高到至少 `6 vCPU / 12-14 GiB RAM`，在同一 2K range 上优先复测 `5p x 1t / batch32`。按单进程基线线性上限，5 worker 才接近 `28.88 records/s`；必须实测达到 28 且保持 byte-identical 后，才能进入完整 B-scale-4 90K generation。batch64 不推荐。

```text
B-scale-4A decision:
  production_code_changed = no
  benchmark_tool_changed = yes
  tests_changed = yes
  docs_changed = yes
  first_10k_cache_reused = yes
  query_cache_reused = yes
  deterministic_row_sharding_added = yes
  resume_and_checksum_validation_added = yes
  deterministic_merge_added = yes
  model_or_token_cap_changed = no
  normalization_changed = no
  successful_outputs_byte_identical = yes
  aggregate_throughput_gate_28_rps = no
  full_90k_started = no
  current_vm_sufficient = no
  vm_upgrade_recommended = yes
  recommended_config = 5p-x-1t-batch32-on-6vcpu-12to14gb
  ready_for_B_scale_4_full_generation = no
  recommended_next = B-scale-4A-resource-followup
```
