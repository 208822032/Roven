# B0-preflight: real embedding benchmark environment and dependency check

## 1. AGENTS.md 遵守情况

已读取并遵守仓库根目录 `AGENTS.md`，并读取：

- `docs/vector_benchmark_readiness/README.md`
- `docs/vector_benchmark_readiness/index.md`
- `docs/vector_benchmark_readiness/v_family_implementation_audit_report.md`
- `docs/vector_benchmark_readiness/benchmark_plan_real_embedding.md`

冲突说明：`AGENTS.md` 的历史阶段模板要求阶段报告写入 `docs/vector_core_reset/`；当前 benchmark readiness 路线要求报告写入 `docs/vector_benchmark_readiness/`。本报告按当前路线指定目录输出，同时继续遵守 AGENTS 的核心边界：不修改生产代码、不修改测试、不修改 Makefile、不修改 RDB/AOF 主格式、不修改 `VSEARCH` response、不修改 `kvs_vector_index_ops_t`、不让 vector core 访问 manager-owned 语义状态。

本轮只做轻量 preflight 和 docs report。没有实现 benchmark harness，没有生成 `embeddings.f32` / `query_embeddings.f32`，没有运行大规模 ONNX 推理或压测。

## 2. B0 范围说明

B0 目标是验证真实 embedding benchmark 的环境和依赖是否满足 `benchmark_plan_real_embedding.md` 中的 preflight 要求：

```text
model files
model dim
ONNX Runtime header / library / loader path
FAISS headers / static library / index-only link flags
product backend names
removed backend name risk
artifact root / cleanup policy
B1-B4 prerequisite status
```

本阶段不进入：

```text
B1 fixture/cache generation
B2 ground truth generation
B3 core_hnsw_v2 benchmark
B4 FAISS benchmark
e2e benchmark harness
product FAISS backend restoration
V command behavior changes
```

## 3. 模型文件检查

检查目录：

```text
models/bge-small-zh-v1.5
```

关键文件存在：

| 文件 | 大小 | 结论 |
| --- | ---: | --- |
| `config.json` | 776 bytes | ready |
| `model.onnx` | 41,689 bytes | ready |
| `model.onnx_data` | 94,765,056 bytes | ready |
| `onnx/model.onnx` | 41,689 bytes | ready |
| `onnx/model.onnx_data` | 94,765,056 bytes | ready |
| `vocab.txt` | 109,540 bytes | ready |
| `tokenizer.json` | 362,603 bytes | ready |
| `tokenizer_config.json` | 367 bytes | ready |
| `special_tokens_map.json` | 125 bytes | ready |

`config.json` 关键字段：

```text
hidden_size = 512
model_type = bert
num_attention_heads = 8
num_hidden_layers = 4
vocab_size = 21128
```

结论：

```text
model_files_ready = yes
model_dim_confirmed = yes
dim = 512
metric for benchmark = normalized vectors + IP
```

## 4. ONNX Runtime 检查

检查结果：

```text
third_party/onnxruntime/include/onnxruntime_c_api.h exists
third_party/onnxruntime/lib/libonnxruntime.so exists
third_party/onnxruntime/lib/libonnxruntime.so.1 exists
third_party/onnxruntime/lib/libonnxruntime.so.1.24.4 exists
```

symlink 链：

```text
libonnxruntime.so -> libonnxruntime.so.1
libonnxruntime.so.1 -> libonnxruntime.so.1.24.4
```

`readlink -f third_party/onnxruntime/lib/libonnxruntime.so`：

```text
/home/zhengpan/share/KVLite/9.1-kvstore/third_party/onnxruntime/lib/libonnxruntime.so.1.24.4
```

`ldd third_party/onnxruntime/lib/libonnxruntime.so.1.24.4` 依赖均可解析：

```text
libdl.so.2
librt.so.1
libpthread.so.0
libstdc++.so.6
libm.so.6
libgcc_s.so.1
libc.so.6
```

轻量 C API link/run probe：

```text
compile: cc -Ithird_party/onnxruntime/include
link:    cc <probe.o> -Lthird_party/onnxruntime/lib \
         -Wl,-rpath,$PWD/third_party/onnxruntime/lib -lonnxruntime
run:     prints 1.24.4
```

说明：第一次 probe 若把 `-lonnxruntime` 放在对象文件前，会因静态链接顺序失败；对象文件在前、library 在后后通过。这是 link flag order 问题，不是库缺失。

结论：

```text
onnxruntime_headers_ready = yes
onnxruntime_library_ready = yes
onnxruntime_loader_path_ready = yes
```

## 5. FAISS index-only 依赖检查

检查到 FAISS headers：

```text
third_party/faiss/faiss/Index.h
third_party/faiss/faiss/IndexFlat.h
third_party/faiss/faiss/IndexHNSW.h
```

检查到 FAISS static library：

```text
third_party/faiss/build/faiss/libfaiss.a
size = 15,641,312 bytes
file = current ar archive
```

系统 link 依赖可见：

```text
libopenblas.so
libblas.so
liblapack.so
libgomp.so.1
libgfortran.so.5
```

轻量 C++ index-only link/run probe：

```text
compile:
  g++ -std=c++17 -Ithird_party/faiss -Ithird_party/faiss/build

probe:
  faiss::IndexFlatIP flat(4)
  faiss::IndexHNSWFlat hnsw(4, 16, faiss::METRIC_INNER_PRODUCT)

link:
  g++ <probe.o> third_party/faiss/build/faiss/libfaiss.a \
      -fopenmp -lblas -llapack

result:
  PASS
```

结论：

```text
faiss_headers_ready = yes
faiss_library_ready = yes
faiss_index_only_possible = yes
```

B4 仍需要新增或修复 index-only runner；B0 只证明依赖和 link flags 可用。

## 6. product backend name 检查

当前产品 e2e benchmark 只应显式使用：

```text
core_flat
core_hnsw_v2
```

代码依据：

```text
src/vector/kvs_vector_backend_resolve.c
include/vector/kvs_vector_backend_resolve.h
include/kvs_vector.h
```

`kvs_vector_backend_resolve_from_any_backend()` 行为：

- empty / missing backend -> `KVS_VECTOR_INDEX_CORE_HNSW_V2`
- `flat` / `core_flat` -> `KVS_VECTOR_INDEX_CORE_FLAT`
- `core_hnsw_v2` -> `KVS_VECTOR_INDEX_CORE_HNSW_V2`
- unknown backend -> logs error and falls back to `core_hnsw_v2`
- removed compatibility names -> logs error and falls back to `core_flat`

结论：

```text
product_backend_names_confirmed = yes
e2e product benchmark backends = core_flat, core_hnsw_v2
```

## 7. removed backend name 风险确认

Removed compatibility names detected in resolver:

```text
native_hnsw
core_hnsw
hnsw
hnswlib
hnswlib_hnsw
sq8_flat
faiss
faiss_hnsw
faiss_flat
```

这些名字当前不是产品 backend；如果用于 e2e benchmark config，会 fallback 到 `core_flat`，从而污染 benchmark 结论。

当前风险实例：

```text
config/kvstore-embedding.conf:
  vector_index_type = hnswlib_hnsw
```

该配置不适合作为真实 embedding `core_hnsw_v2` benchmark 默认配置。B1 及后续 e2e benchmark 必须生成或使用显式配置：

```text
vector_index_type = core_flat
vector_index_type = core_hnsw_v2
```

结论：

```text
removed_backend_names_blocked_for_benchmark = yes
```

说明：这里的 blocked 是 benchmark policy 和 B0 报告结论。当前产品 resolver 仍保留 compatibility fallback；后续 benchmark harness 必须在读取配置时 fail-fast 拒绝 removed names，不能依赖产品 fallback。

## 8. benchmark artifact / cleanup 策略

B0 未创建 benchmark artifact，也未生成大体积 fixture。

建议 B1 默认 artifact root：

```text
/tmp/kvlite_vector_benchmark_artifacts/real_embedding/<dataset_id>/
```

如果后续阶段必须把 artifact 放在仓库内，应使用：

```text
benchmark_artifacts/real_embedding/<dataset_id>/
```

并满足：

- 大体积文件不得加入 git。
- B1/B-final 必须输出 cleanup command。
- 每轮结果必须记录 artifact root。
- `embeddings.f32`、`query_embeddings.f32`、snapshot、records fixture 等文件只作为 local artifacts。
- 如果使用 repo-local `benchmark_artifacts/`，后续阶段应显式确认 `.gitignore` 或清理策略；B0 不修改 `.gitignore`。

B0 检查：

```text
benchmark_artifacts directory missing
```

结论：

```text
artifact_cleanup_policy_defined = yes
```

## 9. B1-B4 前置条件判断

### B1 fixture/cache generation

Ready:

- model files ready。
- model dim confirmed as 512。
- ONNX Runtime header/library/loader path ready。
- existing smoke text fixtures exist:

```text
tests/fixtures/qa_zh_1000.tsv     1000 rows
tests/fixtures/qa_zh_queries.tsv   200 rows
```

Limit:

- 仓库内未发现专用大规模真实 benchmark corpus。
- B1 应先支持 smoke fixture/cache generation；full-scale run 需要用户提供或明确 dataset source。

### B2 core_flat ground truth

Ready after B1:

- once `embeddings.f32` and `query_embeddings.f32` exist, `core_flat` can generate exact ground truth.
- `groundtruth.jsonl` schema 已在 benchmark plan 中冻结。

### B3 core_hnsw_v2 index-only benchmark

Ready after B1/B2:

- `core_hnsw_v2` product/core implementation exists.
- core benchmark schema 已冻结。
- B3 仍需新增 index-only runner；B0 不实现。

### B4 FAISS index-only baseline

Dependencies ready:

- headers ready。
- static library ready。
- link flags confirmed: `-fopenmp -lblas -llapack` with `libfaiss.a`。

Implementation gap:

```text
tests/vector_index_only_formal.cpp missing
```

旧脚本引用该缺失文件：

```text
scripts/benchmark/run_vector_native_faiss_formal.py
scripts/benchmark/run_hnswlib_vs_faiss_benchmark.py
```

B4 必须新增或修复 index-only runner，直接读取同一份 `embeddings.f32` / `query_embeddings.f32`，不得使用 product backend fallback。

## 10. 当前阻塞项

Blocking for B1 smoke:

```text
none found
```

Risks / required work in B1-B4:

1. B1 full-scale dataset source 尚未冻结；仓库内只有 1K/200 rows smoke TSV。
2. B1 embedding cache generator 尚未实现。
3. B2 groundtruth generator 尚未实现。
4. B4 FAISS index-only runner 尚未实现；旧 runner 依赖的 `tests/vector_index_only_formal.cpp` 缺失。
5. 后续 e2e benchmark config 必须拒绝 removed backend names，不能复用 `config/kvstore-embedding.conf` 的 `hnswlib_hnsw`。
6. 如果 artifact 放入 repo-local `benchmark_artifacts/`，后续阶段需要显式 `.gitignore` 或 cleanup 策略执行；B0 只定义策略，不创建目录。

## 11. 推荐下一阶段

推荐进入：

```text
B1-fixture-cache-generation
```

B1 最小目标应是：

```text
read texts / queries source
load models/bge-small-zh-v1.5 through ONNX Runtime
generate embeddings.f32 / query_embeddings.f32
write manifest.json with checksums
support smoke fixture first
write local artifacts under /tmp by default
avoid mixing ONNX inference time into later core benchmark metrics
```

不要在 B1 中实现 FAISS benchmark、e2e benchmark、async rebuild benchmark 或 product backend changes。

## 12. 检查结果

本轮未运行功能测试。理由：任务明确为 preflight + docs-only report，且本轮不修改生产代码、测试或 Makefile。

```text
git diff --check
PASS, no output

git status --short
?? docs/vector_benchmark_readiness/
```

## 13. Decision block

```text
B0-preflight decision:
  production_code_changed = no
  tests_changed = no
  makefile_changed = no
  docs_changed = yes
  model_files_ready = yes
  model_dim_confirmed = yes
  onnxruntime_headers_ready = yes
  onnxruntime_library_ready = yes
  onnxruntime_loader_path_ready = yes
  faiss_headers_ready = yes
  faiss_library_ready = yes
  faiss_index_only_possible = yes
  product_backend_names_confirmed = yes
  removed_backend_names_blocked_for_benchmark = yes
  artifact_cleanup_policy_defined = yes
  ready_for_B1_fixture_cache_generation = yes
  recommended_next = B1-fixture-cache-generation
```
