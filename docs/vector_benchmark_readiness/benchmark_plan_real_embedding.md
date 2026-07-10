# benchmark-plan: real embedding + core/e2e benchmark design freeze

## 1. AGENTS.md 遵守情况

已读取并遵守仓库根目录 `AGENTS.md`，并读取本阶段要求的 HNSW-S、HNSW-R 和 vector benchmark readiness 文档。

冲突说明：`AGENTS.md` 的历史阶段模板要求阶段报告写入 `docs/vector_core_reset/`；本任务明确要求在 `docs/vector_benchmark_readiness/` 下冻结真实 embedding benchmark plan。本报告按任务指定目录输出，同时继续遵守 AGENTS 的核心边界：不修改生产代码、不修改测试、不修改 Makefile、不修改 RDB/AOF 主格式、不修改 `VSEARCH` response、不修改 `kvs_vector_index_ops_t`，不让 vector core 访问 manager-owned 语义状态。

本阶段是 docs-only benchmark design freeze；没有实现 benchmark harness，也没有验证新增功能。

## 2. 本阶段范围

本阶段只冻结下一阶段真实 embedding benchmark 的设计：

```text
dataset / text fixture schema
offline embedding cache schema
core benchmark input/output schema
e2e benchmark input/output schema
FAISS index-only baseline boundary
core_flat ground truth / recall evaluator
core_hnsw_v2 benchmark metrics
VSET / VSEARCH / VINDEX / VINFO e2e metrics
snapshot load / async rebuild metrics
result JSON / Markdown report schema
benchmark stage roadmap B0-Bfinal
```

本阶段不做：

```text
benchmark harness implementation
production V command changes
test changes
Makefile changes
FAISS product backend restoration
default backend changes
RDB/AOF format changes
VSEARCH response changes
kvs_vector_index_ops_t signature changes
HNSW-R follow-up implementation
HNSW build/search optimization
```

## 3. 上一阶段 readiness 结论摘要

上一阶段 `v_family_implementation_audit_report.md` 的结论：

```text
ready_for_real_embedding_benchmark = partial
recommended_next = benchmark-plan
```

关键事实：

- `core_hnsw_v2` product 主路径、snapshot cold-start、async rebuild、VINFO observability 已具备。
- `models/bge-small-zh-v1.5` 存在，模型维度为 512。
- embedding runtime 已有 ONNX loader、tokenizer、batch encode、L2 normalize 和 runtime cache。
- 当前 product backend 只应对比 `core_flat` 与 `core_hnsw_v2`。
- FAISS product backend 已删除；旧 `faiss*` / `hnswlib*` backend name 不能作为 e2e benchmark 配置。
- FAISS 仍可作为 index-only external baseline，但需要独立 runner 直接读取同一份 embedding fixture。

## 4. benchmark 总体目标

目标是用同一份真实 embedding fixture 同时回答：

```text
core layer:
  core_flat exact ground truth
  core_hnsw_v2 recall / latency / build / memory
  FAISS index-only baseline recall / latency / build / memory

end-to-end product layer:
  core_flat product backend VSET / VSEARCH / VINDEX / VINFO
  core_hnsw_v2 product backend VSET / VSEARCH / VINDEX / VINFO
  snapshot cold-start with real embeddings
  async rebuild with real embeddings
  VSEARCH under async rebuild latency
```

固定口径：

```text
real embedding model = models/bge-small-zh-v1.5
dim = 512
metric = normalized vectors + IP
topK default = 10
core benchmark input = embeddings.f32 + query_embeddings.f32
e2e benchmark input = texts.jsonl + queries.jsonl + config selecting core_flat/core_hnsw_v2
```

## 5. 非目标

本计划明确不要求：

- 恢复 FAISS product backend。
- 用 removed backend name 做 e2e backend 切换。
- 把 ONNX 推理时间混进 core index build/search 指标。
- 用 synthetic hash embedding 代表真实 embedding。
- 变更 snapshot format 或 RDB/AOF 主格式。
- 变更 `VSEARCH` response。
- 变更 `kvs_vector_index_ops_t`。
- 为 benchmark 改默认 HNSW 参数。
- 在 benchmark 阶段做 graph construction、SIMD、layout 或 buildperf 优化。

## 6. dataset / fixture 设计

### 6.1 目录布局

建议每个 benchmark fixture 使用独立目录：

```text
benchmark_artifacts/real_embedding/<dataset_id>/
  texts.jsonl
  queries.jsonl
  embeddings.f32
  query_embeddings.f32
  groundtruth.jsonl
  manifest.json
  results/
    core_benchmark_result.json
    e2e_benchmark_result.json
    snapshot_rebuild_result.json
    summary.md
```

`benchmark_artifacts/` 是建议 artifact 根目录，不属于本阶段新增文件；后续实现时应避免把大体积 fixture 加入 git。

### 6.2 `texts.jsonl`

用途：冻结写入记录文本，作为 e2e `VSET` 输入和 `embeddings.f32` 行顺序来源。

每行 JSON object：

```json
{"row":0,"record_id":1,"question":"什么是 KVLite？","answer":"KVLite 是一个轻量级 KV 存储。","scope":"bench/default","source":"sample","lang":"zh"}
```

字段：

| 字段 | 类型 | 必填 | 用途 |
| --- | --- | --- | --- |
| `row` | uint64 | yes | 从 0 开始的行号，必须等于文件行顺序 |
| `record_id` | uint64 | yes | core benchmark 使用的稳定 record id；e2e 用于结果对齐，不暴露给 VSEARCH response |
| `question` | string | yes | VSET question / embedding source |
| `answer` | string | yes | VSET answer，e2e response sanity 使用 |
| `scope` | string | yes | e2e scope filter 分组 |
| `source` | string | no | 数据来源标记 |
| `lang` | string | no | 语言标记 |

Reproducibility：

- `row` 必须连续。
- `record_id` 必须唯一且稳定。
- 文件换行、UTF-8 编码和 JSON key schema 必须固定。
- `manifest.json` 必须记录 `sha256(texts.jsonl)`、`count`、`scope_count`、`text_len_p50/p95/p99`。

### 6.3 `queries.jsonl`

用途：冻结查询文本，作为 core query vectors 和 e2e `VSEARCH` 输入。

每行 JSON object：

```json
{"query_row":0,"query_id":"q000001","text":"KVLite 向量检索怎么工作？","scope":"bench/default","topk":10}
```

字段：

| 字段 | 类型 | 必填 | 用途 |
| --- | --- | --- | --- |
| `query_row` | uint64 | yes | 从 0 开始的查询行号 |
| `query_id` | string | yes | ground truth 和 result join key |
| `text` | string | yes | query embedding source / VSEARCH query |
| `scope` | string | yes | e2e VSEARCH scope |
| `topk` | uint32 | yes | 查询 topK，默认 10 |

Reproducibility：

- `query_id` 必须唯一。
- `query_row` 必须连续。
- `manifest.json` 必须记录 `sha256(queries.jsonl)`、`query_count`、`topk_default`。

## 7. embedding cache 设计

### 7.1 固定模型口径

```text
model_id = bge-small-zh-v1.5
model_path = models/bge-small-zh-v1.5
runtime = onnxruntime
dim = 512
normalization = l2
metric = ip_normalized
dtype = float32
endianness = little
```

核心规则：

- 必须支持 offline cache。
- core benchmark 必须直接读取 `embeddings.f32` / `query_embeddings.f32`。
- core index build/search 指标不得包含重复 ONNX 推理时间。
- e2e benchmark 可以单独报告 embedding prepare/cache hit/cache miss，但不得与 core latency 混淆。

### 7.2 `embeddings.f32`

用途：`texts.jsonl` 对应记录向量缓存，供 core benchmark、ground truth、FAISS baseline 和 e2e preloaded path 复用。

格式：

```text
raw binary, no header
row-major float32
shape = [manifest.count, 512]
row i corresponds to texts.jsonl row i
vectors are L2-normalized
metric is inner product
```

示例读取约束：

```text
expected_size_bytes = count * 512 * 4
row_offset = row * 512 * 4
```

Reproducibility：

- `manifest.json` 必须记录 `sha256(embeddings.f32)`。
- `manifest.json` 必须记录 embedding generator version、normalization、model checksum。
- 实现阶段必须校验文件大小、dim、count 和 checksum 后才能运行 benchmark。

### 7.3 `query_embeddings.f32`

用途：`queries.jsonl` 对应查询向量缓存。

格式同 `embeddings.f32`：

```text
raw binary, no header
row-major float32
shape = [manifest.query_count, 512]
row i corresponds to queries.jsonl query_row i
vectors are L2-normalized
```

Reproducibility：

- `manifest.json` 必须记录 `sha256(query_embeddings.f32)`。
- `query_count * 512 * 4` 必须匹配文件大小。

## 8. core benchmark 设计

Core benchmark 只测 index 行为：

```text
inputs:
  manifest.json
  embeddings.f32
  query_embeddings.f32
  groundtruth.jsonl

backends:
  core_flat
  core_hnsw_v2
  faiss_flat
  faiss_hnsw

excluded:
  RESP parser
  manager record store
  scope / TTL / metadata filtering
  response assembly
  embedding runtime
  RDB/AOF
```

### 8.1 `core_benchmark_result.json`

示例：

```json
{
  "schema_version": 1,
  "dataset_id": "bge-small-zh-v1.5-100k-v1",
  "backend": "core_hnsw_v2",
  "dim": 512,
  "metric": "ip_normalized",
  "count": 100000,
  "query_count": 1000,
  "topk": 10,
  "params": {"M":48,"M0":96,"efConstruction":512,"efSearch":1024},
  "build_us": 0,
  "add_avg_us": 0,
  "search_avg_us": 0,
  "search_p50_us": 0,
  "search_p95_us": 0,
  "search_p99_us": 0,
  "recall_at_10": 0.0,
  "topk_exact_match_rate": 0.0,
  "memory_bytes": 0,
  "memory_bytes_source": "measured",
  "checksum": {"manifest_sha256":"...","embeddings_sha256":"...","groundtruth_sha256":"..."}
}
```

Required fields：

- dataset identity：`schema_version`、`dataset_id`、`count`、`query_count`、`dim`、`metric`、`topk`
- backend identity：`backend`、`params`
- timing：`build_us`、`add_*_us`、`search_*_us`
- correctness：`recall_at_10`、`topk_exact_match_rate`
- memory：`memory_bytes`、`memory_bytes_source`
- reproducibility：fixture checksums and binary git/build metadata

Acceptance：

- `core_flat` must produce ground truth compatible topK.
- `core_hnsw_v2` and FAISS must use identical vectors, queries, metric, dim and topK.
- latency percentiles must be computed from per-query samples, not only aggregate averages.
- memory source must be `measured`, `estimated`, or `unsupported`; unsupported must not be treated as zero.

## 9. FAISS baseline 设计

FAISS boundary：

```text
FAISS is index-only external baseline.
FAISS is not a product backend in this plan.
FAISS is not selected through vector_index_type.
Removed backend names such as faiss_hnsw or hnswlib_hnsw must not be used for e2e comparison.
```

Recommended FAISS index-only variants：

```text
faiss_flat:
  exact IP baseline, can cross-check core_flat ground truth.

faiss_hnsw:
  ANN baseline using same dim=512, metric=IP, topK=10.
  Parameters must be recorded explicitly.
```

Required FAISS result metadata：

```text
faiss_version_or_git
compile_flags
thread_count
index_type
metric
M / efConstruction / efSearch where applicable
memory source
```

Risk handling：

- If FAISS library or runner is missing, B4 must fail as `blocked`, not silently reuse product `core_flat`.
- If FAISS memory is unsupported, result must say `memory_bytes_source=unsupported`.
- FAISS topK must be evaluated against `groundtruth.jsonl`; it must not define ground truth for the entire benchmark unless explicitly selected as `faiss_flat` exact cross-check.

## 10. e2e benchmark 设计

E2E benchmark uses product paths:

```text
VSET
VSEARCH
VINDEX
VINFO
```

Product backends:

```text
core_flat
core_hnsw_v2
```

Explicitly excluded:

```text
product FAISS backend switching
removed backend names in config
VSEARCH response format changes
RDB/AOF format changes
```

### 10.1 VSET metrics

Required:

```text
vset_total_qps
vset_total_avg_us / p50 / p95 / p99
embedding_prepare_avg_us / p95 / p99
embedding_cache_hit_rate
record_commit_avg_us if observable
index_add_avg_us if observable
write_error_count
```

Interpretation:

- If e2e run uses real ONNX during VSET, report it as product ingestion latency, not core build latency.
- If e2e run preloads embeddings through a prepared path in future harness, report that mode separately.

### 10.2 VSEARCH metrics

Required:

```text
vsearch_qps
vsearch_total_avg_us / p50 / p95 / p99
vsearch_backend_search_avg_us / p95 / max
vsearch_manager_resolve_avg_us / p95 / max
vsearch_response_assembly_avg_us / p95 / max
vsearch_raw_hit_count
vsearch_resolved_hit_count
vsearch_filtered_hit_count
embedding_prepare_avg_us / p95 / p99
embedding_cache_hit_rate
recall_at_10
topk_exact_match_rate
error_count
```

Timing label:

`vsearch_backend_search_us` must be labeled as:

```text
estimated_from_ops_search_wall_us_minus_manager_resolve_us
```

It is not pure core kernel time.

### 10.3 VINDEX / VINFO metrics

Required:

```text
vindex_rebuild_sync_us
vindex_rebuild_async_total_us
vindex_async_state_timeline
vinfo_backend_name
vinfo_backend_size
vinfo_backend_capacity
vinfo_backend_dim
vinfo_backend_metric
vinfo_backend_memory_bytes
vinfo_backend_memory_bytes_source
vinfo_snapshot_load_status
vinfo_snapshot_write_status
vinfo_rebuild_state
vinfo_rebuild_last_error
```

## 11. snapshot / async rebuild benchmark 设计

### 11.1 Snapshot load with real embeddings

Goal:

```text
valid snapshot cold-start with real embedding records
```

Inputs:

```text
texts.jsonl
embeddings.f32
manifest.json
vector_index.manifest
vector_index.snapshot
```

Metrics:

```text
records_restore_us
snapshot_load_us_avg / p50 / p95
snapshot_validate_us
snapshot_deserialize_us
bind_us
snapshot_file_size_bytes
manifest_file_size_bytes
post_load_search_ok
loaded_from_snapshot
fallback_rebuild_count
```

Rule:

- load-only benchmark must fail fast on missing / invalid / corrupt snapshot.
- It must not fallback into records-only rebuild.
- Production fallback semantics must remain unchanged.

### 11.2 Async rebuild with real embeddings

Goal:

```text
measure HNSW-R worker latency and safety using real embeddings
```

Metrics:

```text
async_rebuild_total_us
shadow_init_us
full_scan_us
delta_capture_count
delta_replay_us
final_validation_us
publish_us
retired_cleanup_us
post_publish_snapshot_write_us
post_publish_snapshot_write_status
active_memory_bytes
shadow_memory_bytes
retired_memory_bytes
peak_rss_bytes
cancel_status if tested
retry_status if tested
```

### 11.3 VSEARCH under async rebuild

Goal:

```text
measure product reader safety and latency while background rebuild is active
```

Workload:

```text
start async rebuild
run steady VSEARCH load
optionally run controlled VSET / VDEL mix if stage explicitly allows write-load mode
sample VINFO status timeline
stop when rebuild published or failed
```

Metrics:

```text
vsearch_during_rebuild_qps
vsearch_during_rebuild_avg_us / p50 / p95 / p99 / max
vsearch_error_count
rebuild_state_timeline
publish_seen
old_active_kept_on_failure
post_publish_search_ok
```

## 12. result schema

### 12.1 `groundtruth.jsonl`

用途：core/e2e recall evaluator 的统一真值。

示例：

```json
{"query_id":"q000001","query_row":0,"topk":10,"metric":"ip_normalized","neighbors":[{"rank":1,"record_id":42,"score":0.9123},{"rank":2,"record_id":7,"score":0.9011}]}
```

字段：

| 字段 | 类型 | 用途 |
| --- | --- | --- |
| `query_id` | string | join key |
| `query_row` | uint64 | query_embeddings 行号 |
| `topk` | uint32 | truth topK |
| `metric` | string | must be `ip_normalized` |
| `neighbors` | array | ordered exact neighbors |
| `neighbors[].rank` | uint32 | 1-based rank |
| `neighbors[].record_id` | uint64 | expected record id |
| `neighbors[].score` | float | exact score |

Ground truth source:

```text
primary = core_flat exact scan
optional cross-check = faiss_flat IP
```

Recall evaluator:

```text
recall_at_k = |actual_topk_record_ids intersect expected_topk_record_ids| / k
topk_exact_match = actual ordered record_id list equals expected ordered record_id list
```

For e2e VSEARCH response, evaluator must map returned `question` / `answer` / `scope` back to `record_id` through `texts.jsonl`; it must not require record_id in user-facing response.

### 12.2 `manifest.json`

示例：

```json
{
  "schema_version": 1,
  "dataset_id": "bge-small-zh-v1.5-100k-v1",
  "created_at_utc": "2026-07-09T00:00:00Z",
  "model": {
    "id": "bge-small-zh-v1.5",
    "path": "models/bge-small-zh-v1.5",
    "dim": 512,
    "runtime": "onnxruntime",
    "normalization": "l2",
    "metric": "ip_normalized"
  },
  "files": {
    "texts_jsonl": {"path":"texts.jsonl","sha256":"...","rows":100000},
    "queries_jsonl": {"path":"queries.jsonl","sha256":"...","rows":1000},
    "embeddings_f32": {"path":"embeddings.f32","sha256":"...","shape":[100000,512],"dtype":"float32"},
    "query_embeddings_f32": {"path":"query_embeddings.f32","sha256":"...","shape":[1000,512],"dtype":"float32"},
    "groundtruth_jsonl": {"path":"groundtruth.jsonl","sha256":"...","rows":1000}
  },
  "benchmark_defaults": {"topk":10,"efSearch":1024,"M":48,"M0":96,"efConstruction":512}
}
```

Required:

- file checksums for every input.
- model identity and model file checksum when feasible.
- dim / metric / normalization.
- record count / query count / topK.
- benchmark parameter defaults.

### 12.3 `e2e_benchmark_result.json`

示例:

```json
{
  "schema_version": 1,
  "dataset_id": "bge-small-zh-v1.5-100k-v1",
  "backend": "core_hnsw_v2",
  "config": {"vector_index_type":"core_hnsw_v2","dim":512,"metric":"ip"},
  "vset": {"qps":0,"avg_us":0,"p50_us":0,"p95_us":0,"p99_us":0,"error_count":0},
  "vsearch": {"qps":0,"avg_us":0,"p50_us":0,"p95_us":0,"p99_us":0,"recall_at_10":0.0,"error_count":0},
  "vinfo": {"backend_name":"core_hnsw_v2","backend_memory_bytes":0,"backend_memory_bytes_source":"measured"},
  "checksums": {"manifest_sha256":"...","texts_sha256":"...","queries_sha256":"...","groundtruth_sha256":"..."}
}
```

### 12.4 `snapshot_rebuild_result.json`

示例:

```json
{
  "schema_version": 1,
  "dataset_id": "bge-small-zh-v1.5-100k-v1",
  "backend": "core_hnsw_v2",
  "snapshot": {
    "snapshot_load_us_avg": 0,
    "snapshot_load_us_p50": 0,
    "snapshot_load_us_p95": 0,
    "loaded_from_snapshot": 1,
    "fallback_rebuild_count": 0,
    "snapshot_file_size_bytes": 0
  },
  "async_rebuild": {
    "async_rebuild_total_us": 0,
    "delta_capture_count": 0,
    "final_validation_ok": 1,
    "publish_ok": 1,
    "snapshot_write_status": "ok"
  },
  "vsearch_under_rebuild": {
    "qps": 0,
    "p50_us": 0,
    "p95_us": 0,
    "p99_us": 0,
    "error_count": 0
  }
}
```

### 12.5 `summary.md`

Required sections:

```text
dataset and model
environment and build flags
core comparison table
e2e comparison table
snapshot cold-start table
async rebuild table
VSEARCH under rebuild table
correctness / recall summary
known caveats
decision block for the benchmark run
```

## 13. benchmark stage roadmap B0-Bfinal

| Stage | 目标 | 输入 | 输出 | 允许修改范围 | 禁止事项 | 验收标准 |
| --- | --- | --- | --- | --- | --- | --- |
| B0 preflight | 验证环境、依赖和边界 | repo, model files, FAISS lib, ONNX Runtime | preflight report | docs / benchmark scripts only | 不改生产代码、测试主路径、Makefile，除非单独任务允许 | model exists, dim=512, ONNX lib path confirmed, FAISS index-only availability confirmed, removed backend names rejected |
| B1 fixture/cache generation | 生成 texts/queries embeddings cache | dataset source, model | `texts.jsonl`, `queries.jsonl`, `embeddings.f32`, `query_embeddings.f32`, `manifest.json` | benchmark harness / artifact only | 不把大文件加入 git，不把 ONNX time 混入 core metrics | checksums valid, vector norm sanity passes, dim/count match |
| B2 core_flat ground truth | 生成 exact truth | embeddings, query_embeddings | `groundtruth.jsonl` | index-only runner | 不经过 RESP / manager response | core_flat exact topK written, recall evaluator smoke passes |
| B3 core_hnsw_v2 index-only | 测 v2 build/search/recall/memory | fixture + groundtruth | `core_benchmark_result.json` for v2 | index-only runner | 不调低 recall 参数偷换性能 | recall@10, p50/p95/p99, build_us, memory recorded |
| B4 FAISS index-only baseline | 测 FAISS flat/hnsw baseline | same fixture + groundtruth | FAISS result JSON | index-only runner | 不恢复 product FAISS backend，不用 removed backend name | FAISS uses same dim/metric/topK, flags/threading recorded |
| B5 core comparison report | 汇总 core_flat/v2/FAISS | B2-B4 results | core comparison section | docs/report generator | 不混入 e2e latency | apples-to-apples table with checksums |
| B6 e2e core_flat | 测 product core_flat | texts/queries/groundtruth | e2e result for core_flat | benchmark harness/config artifact | 不改 VSEARCH response | VSET/VSEARCH/VINFO metrics and recall recorded |
| B7 e2e core_hnsw_v2 | 测 product core_hnsw_v2 | texts/queries/groundtruth | e2e result for v2 | benchmark harness/config artifact | 不使用 old `hnswlib_hnsw` config name | v2 product backend confirmed, metrics recorded |
| B8 snapshot load real embeddings | 测 real embedding snapshot cold-start | real records + snapshot | snapshot load result | benchmark harness/artifact | load-only 不 fallback rebuild | loaded_from_snapshot=1, fallback_rebuild_count=0, p50/p95 recorded |
| B9 async rebuild real embeddings | 测 HNSW-R worker | loaded records, v2 config | async rebuild result | benchmark harness/artifact | 不改 R6/R7 semantics | final validation ok, publish ok, status observable |
| B10 VSEARCH under async rebuild | 测 rebuild 期间读延迟 | B9 workload + search load | under-rebuild result | benchmark harness/artifact | 不提前实现新 reader safety | p50/p95/p99/error_count and rebuild timeline recorded |
| B-final consolidation | 汇总并给结论 | B0-B10 outputs | `summary.md` and final benchmark report | docs/report generator | 不补功能实现 | readiness decision includes caveats and next action |

## 14. 当前阻塞项

进入 implementation 前需要显式处理：

1. FAISS product backend 已删除，因此 e2e 不做 FAISS backend 切换。
2. 旧 config 中 removed backend name 不可作为 benchmark 配置；特别是 `hnswlib_hnsw` 当前会 fallback 到 `core_flat`。
3. 需要确认 ONNX Runtime `libonnxruntime.so` symlink / loader path。
4. 需要新增或修复 FAISS index-only runner。
5. 需要新增 embedding cache generator。
6. 需要新增 recall evaluator。
7. 需要新增 VSEARCH under async rebuild 编排脚本。
8. 需要明确 benchmark artifact 目录和大文件清理策略，避免把 fixture 加入 git。

## 15. 推荐下一阶段

推荐下一阶段：

```text
B0-preflight
```

B0 只应验证环境和依赖，不应直接实现完整 benchmark：

```text
confirm models/bge-small-zh-v1.5 files
confirm ONNX Runtime load path
confirm FAISS library / headers / link flags
confirm current product backend names
confirm removed backend names are not used for e2e
confirm benchmark artifact root and cleanup policy
confirm planned harness files and result schema
```

如果 B0 发现 FAISS runner 或 ONNX runtime path 缺失，应进入 `followup-plan` 或 `B0-fix-readiness`，不要在同一阶段顺手恢复 product FAISS backend。

## 16. 检查结果

本轮未运行功能测试。理由：任务明确为 docs-only benchmark design freeze，且本轮不修改生产代码、测试或 Makefile。

```text
git diff --check
PASS, no output

git status --short
?? docs/vector_benchmark_readiness/
```

## 17. Decision block

```text
benchmark-plan decision:
  production_code_changed = no
  tests_changed = no
  makefile_changed = no
  docs_changed = yes
  benchmark_plan_added = yes
  fixture_schema_defined = yes
  embedding_cache_schema_defined = yes
  core_benchmark_schema_defined = yes
  e2e_benchmark_schema_defined = yes
  faiss_index_only_boundary_defined = yes
  product_faiss_backend_required = no
  recall_evaluator_defined = yes
  result_schema_defined = yes
  ready_for_B0_preflight = yes
  recommended_next = B0-preflight
```
