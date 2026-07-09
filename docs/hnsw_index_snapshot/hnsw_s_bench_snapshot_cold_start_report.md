# HNSW-S-bench-0 / 1：snapshot cold-start benefit benchmark

## AGENTS.md 遵守情况

已读取并遵守根目录 `AGENTS.md`。

冲突说明：`AGENTS.md` 的历史阶段模板要求报告写入 `docs/vector_core_reset/`；本任务明确属于 HNSW-S snapshot 路线，并要求新增 `docs/hnsw_index_snapshot/hnsw_s_bench_snapshot_cold_start_report.md`。本阶段按任务指定目录输出，同时继续遵守 AGENTS 的核心边界：不修改 RDB/AOF 主格式、不修改 `VSEARCH` response、不修改 `kvs_vector_index_ops_t`、不修改默认 backend、不修改默认 HNSW 参数。

## 是否修改生产代码

否。

本阶段只新增 benchmark harness、补充 Makefile 测试目标和文档。未修改：

- `src/`
- `include/`
- RDB/AOF 主格式
- `VSEARCH` response
- `kvs_vector_index_ops_t`
- 默认 backend
- 默认 HNSW 参数
- HNSW-R / background rebuild / reader safety / GC / mmap / buildperf

## 新增 / 修改文件

新增：

- `tests/vector_index_snapshot_cold_start_bench.c`
- `tests/vector_index_snapshot_cold_start_bench.sh`
- `docs/hnsw_index_snapshot/hnsw_s_bench_snapshot_cold_start_report.md`

修改：

- `Makefile`
- `docs/hnsw_index_snapshot/README.md`
- `docs/hnsw_index_snapshot/index.md`

## Benchmark 模式

新增 benchmark 支持三种模式：

```text
HNSW_S_BENCH_MODE=create-fixture
  生成 synthetic records fixture、构建 core_hnsw_v2、写出 vector_index.snapshot + vector_index.manifest。
  默认 count=100000，可用 HNSW_S_BENCH_COUNT 覆盖。

HNSW_S_BENCH_MODE=load-only
  要求 vector_index.records、vector_index.manifest、vector_index.snapshot 已存在。
  恢复 records fixture 后只执行 manifest validate + snapshot deserialize + bind。
  snapshot missing / invalid / corrupt 时直接失败，不 fallback rebuild。

HNSW_S_BENCH_MODE=quick
  在临时目录中执行 10K / 512d create-fixture + load-only 一轮。
  用于 CI-friendly 验证 harness，不污染仓库根目录 snapshot 文件。
```

额外参数：

```text
HNSW_S_BENCH_COUNT
HNSW_S_BENCH_REPEAT
HNSW_S_BENCH_QUERIES
HNSW_S_BENCH_BASELINE_REBUILD_US
```

## 为什么 load-only 禁止 fallback rebuild

本阶段要测的是 valid snapshot cold-start 的收益。如果 load-only 在 snapshot 缺失或损坏时进入 `kvs_vector_rebuild_after_load()`，就可能触发 100K records-only rebuild，污染压测耗时，并把 fail-fast benchmark 变成生产 fallback 行为测试。

因此 benchmark 内部使用 no-fallback timed loader：

```text
restore records fixture
  -> rebuild manager id maps
  -> validate manifest against restored records + snapshot file
  -> deserialize core_hnsw_v2 snapshot
  -> validate loaded graph vectors against manager records
  -> bind current index
```

失败路径直接返回错误，不调用 `kvs_vector_rebuild_after_load()`，不调用 `kvs_vector_index_rebuild_current()`，不触发 100K rebuild。生产路径的 fallback 语义没有被修改，原有 S3/S4/S5 测试继续覆盖。

## 100K records-only baseline 来源

本报告复用 HNSW-P2 balanced 默认参数报告中的 100K records-only rebuild 数据：

```text
docs/hnsw_productization/hnsw_p2_100k_balanced_default_report.md

count = 100000
dim = 512
M = 48
M0 = 96
efConstruction = 512
efSearch = 1024
build_us = 3,011,810,795
约 50.2 分钟
source = reused_from_HNSW_P2
```

本阶段没有重跑 100K records-only rebuild，避免重复触发约 50 分钟构图。

## 10K quick benchmark 结果

S-bench-0 必跑 quick：

```bash
HNSW_S_BENCH_MODE=quick tests/vector_index_snapshot_cold_start_bench.sh
```

输出摘要：

```text
dataset:
  count = 10000
  dim = 512
  metric = ip_normalized
  M / M0 / efConstruction / efSearch = 48 / 96 / 512 / 1024
  topK = 10
  queries = 10

fixture:
  fixture_create_build_us = 126,327,129
  snapshot_file_size_bytes = 84,580,136
  manifest_file_size_bytes = 402
  records_fixture_size_bytes = 20,640,096
  post_build_search_ok = 1
  topk_match_checked = 10
  topk_match_count = 10
  duplicate_edge = 0
  degree_overflow = 0

snapshot load-only:
  records_restore_us = 52,909
  snapshot_load_us = 374,815
  snapshot_core_load_us = 321,906
  snapshot_validate_us = 134,344
  snapshot_deserialize_us = 187,561
  bind_us = 1
  post_load_search_ok = 1
  VINFO_loaded_from_snapshot = 1
  fallback_rebuild_count = 0
```

`snapshot_load_us` 包含 records fixture restore、manifest validate、snapshot deserialize、loaded-record validation 和 bind。`snapshot_core_load_us` 只包含 manifest validate、deserialize / validation 和 bind。

## 10K quick benefit 计算

使用复用的 100K records-only rebuild baseline：

```text
records_only_rebuild_us = 3,011,810,795
snapshot_load_us_avg = 374,815

speedup = records_only_rebuild_us / snapshot_load_us_avg
        = 8,035.46x

cold_start_saved_time_us = 3,011,435,980
cold_start_saved_time_percent = 99.99%
```

说明：10K quick 用于验证 benchmark harness、snapshot load-only 机制和 fail-fast no-fallback 语义。这里的 `8,035.46x` 使用 100K records-only rebuild baseline 与 10K quick snapshot load 实测计算，只能作为机制验证和数量级参考，不是 100K apples-to-apples 实测 speedup。最终是否进入 HNSW-R 应以后续 100K load-only 实测为主。

## 100K load-only 补测结果

S-bench-1 先检查仓库根目录 fixture：

```bash
ls -lh vector_index.records vector_index.manifest vector_index.snapshot || true
```

检查时没有现成 fixture，因此按任务要求只运行一次 `create-fixture`：

```bash
HNSW_S_BENCH_MODE=create-fixture \
HNSW_S_BENCH_COUNT=100000 \
HNSW_S_BENCH_QUERIES=50 \
tests/vector_index_snapshot_cold_start_bench.sh
```

create-fixture 输出：

```text
count = 100000
dim = 512
metric = ip_normalized
M / M0 / efConstruction / efSearch = 48 / 96 / 512 / 1024
topK = 10
queries = 50
fixture_create_build_us = 2,933,596,834
snapshot_file_size_bytes = 845,800,136
manifest_file_size_bytes = 403
records_fixture_size_bytes = 206,400,096
post_build_search_ok = 1
topk_match_checked = 50
topk_match_count = 50
memory_bytes = 851,194,400
duplicate_edge = 0
degree_overflow = 0
```

`fixture_create_build_us` 只记录为 fixture 生成成本，不作为本次 old-path 对照重复实测，不用于包装新路径收益。

随后运行 100K load-only：

```bash
HNSW_S_BENCH_MODE=load-only \
HNSW_S_BENCH_COUNT=100000 \
HNSW_S_BENCH_REPEAT=5 \
HNSW_S_BENCH_QUERIES=50 \
tests/vector_index_snapshot_cold_start_bench.sh
```

old-path baseline 继续复用 HNSW-P2：

```text
baseline_source = reused_from_HNSW_P2
records_only_rebuild_us = 3,011,810,795
old_path_rebuild_rerun = no
```

每轮结果：

| iter | records_restore_us | snapshot_load_us | snapshot_core_load_us | snapshot_validate_us | snapshot_deserialize_us | bind_us | post_load_search_ok | VINFO_loaded_from_snapshot | fallback_rebuild_count |
| ---: | ---: | ---: | ---: | ---: | ---: | ---: | ---: | ---: | ---: |
| 1 | 1,013,932 | 6,803,620 | 5,789,688 | 2,169,278 | 3,620,409 | 1 | 1 | 1 | 0 |
| 2 | 1,027,044 | 6,423,254 | 5,396,210 | 1,924,969 | 3,471,240 | 1 | 1 | 1 | 0 |
| 3 | 916,666 | 6,050,485 | 5,133,819 | 1,815,667 | 3,318,150 | 1 | 1 | 1 | 0 |
| 4 | 721,419 | 4,314,362 | 3,592,943 | 1,655,367 | 1,937,574 | 0 | 1 | 1 | 0 |
| 5 | 797,208 | 6,025,857 | 5,228,649 | 1,400,357 | 3,828,291 | 1 | 1 | 1 | 0 |

100K load-only summary：

```text
records_restore_us_avg = 895,253.80
snapshot_load_us_avg = 5,923,515.60
snapshot_load_us_p50 = 6,050,485
snapshot_load_us_p95 = 6,803,620
snapshot_core_load_us_avg = 5,028,261.80
snapshot_validate_us_avg = 1,793,127.60
snapshot_deserialize_us_avg = 3,235,132.80
bind_us_avg = 0.80
snapshot_file_size_bytes = 845,800,136
manifest_file_size_bytes = 403
records_only_rebuild_us = 3,011,810,795
baseline_source = reused_from_HNSW_P2
speedup = 508.45x
cold_start_saved_time_us = 3,005,887,279.40
cold_start_saved_time_percent = 99.80%
post_load_search_ok = 1
VINFO_loaded_from_snapshot = 1
fallback_rebuild_count = 0
```

100K load-only 使用 benchmark-local no-fallback loader；valid snapshot 被加载并绑定，5 轮均未触发 records-only fallback rebuild。

## Correctness / VINFO 状态

quick 和 100K load-only 结果：

```text
10K quick:
  post_load_search_ok = 1
  topK match sanity = 10 / 10
  VINFO loaded_from_snapshot = 1
  fallback_rebuild_count = 0

100K load-only:
  post_load_search_ok = 1
  topK match sanity = 50 / 50
  VINFO loaded_from_snapshot = 1
  fallback_rebuild_count = 0
```

这说明 load-only 走的是 snapshot bind 路径，没有进入 records-only fallback rebuild。

## 当前结论

- benchmark harness 已新增，并能生成 fixture、执行 load-only、输出 avg / p50 / p95 结构。
- load-only 模式已经具备 fail-fast 行为，不会在 snapshot missing / invalid / corrupt 时触发 100K rebuild。
- quick 实测中，10K / 512d balanced snapshot load 为 375ms 量级；该数据只用于机制验证。
- 100K / 512d balanced load-only repeat=5 实测中，snapshot load 平均约 5.92s，p95 约 6.80s，远小于复用的 100K balanced records-only rebuild 50.2 分钟 baseline。
- 当前数据证明 valid snapshot cache 对 100K cold-start 有明确收益，也支持进入 HNSW-R。

## 当前风险

- quick 使用 synthetic normalized IP 数据，不代表真实 embedding 全量 SLA。
- 100K load-only 使用 synthetic normalized IP 数据，不代表真实 embedding 全量 SLA。
- benchmark 的 records fixture 是测试文件格式，不是 RDB/AOF 主格式。
- hash embedding provider 当前默认 64d；benchmark 进程内为了 512d 测试会重建 manager 内 core index 维度，不改变生产默认。
- snapshot 仍是 cache，不是 source of truth。
- 仍未实现 snapshot GC、mmap、admin command、reader safety、background rebuild worker 或更强 fsync/crash consistency。
- 100K fixture 文件 `vector_index.records`、`vector_index.manifest`、`vector_index.snapshot` 是本地 benchmark artifact；补测完成后已按任务清理命令删除，未加入 git。

## 是否建议进入 HNSW-R

建议进入 HNSW-R。

理由：

- HNSW-S 最小闭环已完成。
- S-bench-0 已用 10K quick 证明 benchmark harness 和 fail-fast load-only 机制。
- S-bench-1 已用 100K load-only repeat=5 证明 valid snapshot load-only 能绕过 records-only rebuild，平均 load time 相对复用 P2 baseline 加速约 508.45x。
- 当前主要产品化缺口已经转向 background shadow rebuild、reader safety、active / shadow / retired 生命周期和 delta replay / safe swap。

## 测试结果

累计已执行，其中 quick 来自 S-bench-0，本轮 S-bench-1 重新执行构建、snapshot 测试和 100K create-fixture / load-only：

```text
make ENABLE_HNSWLIB=1 ENABLE_FAISS=1 -j"$(nproc)"
  PASS

tests/vector_index_snapshot_manifest_test.sh
  PASS

tests/vector_core_hnsw_v2_snapshot_roundtrip_test.sh
  PASS

tests/vector_index_snapshot_load_after_records_test.sh
  PASS

tests/vector_index_snapshot_write_after_rebuild_test.sh
  PASS

tests/vector_index_snapshot_vinfo_status_test.sh
  PASS

HNSW_S_BENCH_MODE=quick tests/vector_index_snapshot_cold_start_bench.sh
  PASS

HNSW_S_BENCH_MODE=create-fixture HNSW_S_BENCH_COUNT=100000 HNSW_S_BENCH_QUERIES=50 tests/vector_index_snapshot_cold_start_bench.sh
  PASS

HNSW_S_BENCH_MODE=load-only HNSW_S_BENCH_COUNT=100000 HNSW_S_BENCH_REPEAT=5 HNSW_S_BENCH_QUERIES=50 tests/vector_index_snapshot_cold_start_bench.sh
  PASS
```

AGENTS 边界 grep：

```text
grep -RIn "g_vector_state" src/vector/core/hnsw_v2 src/vector/core/hnsw_v2_snapshot include/vector/kvs_vector_hnsw_v2.h include/vector/kvs_vector_hnsw_v2_snapshot.h || true
  no output

grep -RIn "record_id_map" src/vector/core/hnsw_v2 src/vector/core/hnsw_v2_snapshot include/vector/kvs_vector_hnsw_v2.h include/vector/kvs_vector_hnsw_v2_snapshot.h || true
  no output

grep -RIn "scope\|ttl\|metadata\|question\|answer" src/vector/core/hnsw_v2 src/vector/core/hnsw_v2_snapshot include/vector/kvs_vector_hnsw_v2.h include/vector/kvs_vector_hnsw_v2_snapshot.h || true
  no output
```

## git diff --check

```text
PASS
```

## git status --short

```text
 M docs/hnsw_index_snapshot/hnsw_s_bench_snapshot_cold_start_report.md
```

## HNSW-S-bench-0 decision

```text
HNSW-S-bench-0 decision:
  production_code_changed = no
  benchmark_added = yes
  load_only_skips_fallback_rebuild = yes
  snapshot_benefit_proven = yes
  ready_for_HNSW_R = yes
  next_stage = HNSW-R
```

## HNSW-S-bench-1 decision

```text
HNSW-S-bench-1 decision:
  production_code_changed = no
  hundred_k_load_only_measured = yes
  old_path_rebuild_rerun = no
  old_path_baseline_source = reused_from_HNSW_P2
  load_only_skips_fallback_rebuild = yes
  VINFO_loaded_from_snapshot = yes
  post_load_search_ok = yes
  snapshot_benefit_proven = yes
  ready_for_HNSW_R = yes
  next_stage = HNSW-R
```
