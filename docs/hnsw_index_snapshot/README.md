# HNSW index snapshot 路线

本目录记录 KVLite `core_hnsw_v2` 的 index snapshot / manifest 冷启动加速方案。

## 目录定位

`docs/hnsw_productization/` 主要记录 HNSW 默认 backend、100K recall gate、参数硬化、rebuild / cold-start 风险判断。`HNSW-R` rebuild 产品化路线解决 online rebuild 不阻塞和 active / shadow / retired 安全发布问题。

本目录单独切出 `HNSW-S` 路线，原因是 cold-start 还有一个不同问题：即使 background shadow rebuild 可以避免在线维护阻塞，进程启动后如果没有可用 HNSW graph，仍需要等待从 records 重新构图。100K balanced gate 的 build 时间约 50.2 分钟，这不能作为生产级冷启动体验。

`HNSW-S` 的目标是把 HNSW graph 作为可丢弃 cache 保存到独立 snapshot 文件中，使启动流程可以在 records 恢复后快速校验并加载 index cache。snapshot 失败时必须回退到 records-only rebuild 路径。

## 本路线解决的问题

- cold-start 快速恢复可搜索 HNSW graph。
- 避免每次启动都执行约 50 分钟的 100K rebuild。
- snapshot 失效、缺失或损坏时，可以 fallback 到 flat / degraded mode 并触发 rebuild。
- 给后续实现阶段定义 manifest、校验、加载、写入、失败处理和观测边界。

HNSW-S-final 收口后，本路线已经形成第一版最小闭环：

```text
records load / replay
  -> validate vector_index.manifest + vector_index.snapshot
  -> valid: load core_hnsw_v2 graph cache
  -> invalid / missing / corrupt: records-only rebuild
  -> rebuild / VINDEX REBUILD / shadow publish skeleton success: best-effort write next snapshot
  -> VINFO exposes load / fallback / write status
```

这个闭环只在 valid cache 存在时缩短 cold-start；它不降低 anncore build CPU 成本，也不等于完整 background rebuild 产品化。

## 本路线不解决的问题

- HNSW build CPU 本身慢。
- online rebuild safe swap。
- 真实 embedding recall / QPS SLA。
- graph construction refactor。
- SIMD / layout / reverse_prune cache 微优化。
- RDB/AOF 主格式改造。
- snapshot GC / 多版本 snapshot。
- mmap / view serving。
- 完整 crash-consistency / fsync 保证。
- admin command。

## 基本原则

- records 是 source of truth。
- `vector_index.snapshot` 是 cache，不是 source of truth。
- snapshot 可以删除、过期、损坏或不兼容；这不影响 records load。
- snapshot load 失败不得导致 records load 失败。
- KVManager 管生命周期、manifest、路径、fallback、状态观测和 rebuild 触发。
- anncore 只提供 graph serialize / deserialize / validate 等 index-local 能力。
- `core_hnsw_v2` 不访问 `g_vector_state`、manager `record_id_map`、scope、TTL、metadata、question、answer、RDB/AOF 或 VSEARCH response。

## 路线关系

```text
HNSW-R:
  background shadow rebuild / online maintenance

HNSW-S:
  index snapshot / cold-start acceleration

HNSW-buildperf:
  anncore build CPU optimization
```

`HNSW-S` 解决启动后快速可搜索；`HNSW-R` 解决在线 rebuild 不阻塞；`HNSW-buildperf` 解决单次构图 CPU 成本太高。三条线不要混在同一个阶段。

## 当前入口

- [snapshot_design.md](snapshot_design.md)
- [implementation_plan.md](implementation_plan.md)
- [hnsw_s0_project_structure_analysis_report.md](hnsw_s0_project_structure_analysis_report.md)
- [codebase_hygiene_before_s1_report.md](codebase_hygiene_before_s1_report.md)
- [hnsw_s1_minimal_manifest_report.md](hnsw_s1_minimal_manifest_report.md)
- [hnsw_s2_core_hnsw_v2_snapshot_roundtrip_report.md](hnsw_s2_core_hnsw_v2_snapshot_roundtrip_report.md)
- [hnsw_s3_snapshot_load_fallback_report.md](hnsw_s3_snapshot_load_fallback_report.md)
- [hnsw_s4_snapshot_write_after_rebuild_report.md](hnsw_s4_snapshot_write_after_rebuild_report.md)
- [hnsw_s5_vinfo_snapshot_status_report.md](hnsw_s5_vinfo_snapshot_status_report.md)
- [hnsw_s6_shadow_publish_snapshot_report.md](hnsw_s6_shadow_publish_snapshot_report.md)
- [hnsw_s_final_consolidation_report.md](hnsw_s_final_consolidation_report.md)

## 当前实现状态

HNSW-S1 到 HNSW-S6 已完成第一版 cold-start snapshot cache 最小闭环：

```text
records load / replay
  -> manager rebuild id map
  -> validate vector_index.manifest + vector_index.snapshot
  -> valid: load core_hnsw_v2 graph cache
  -> invalid / missing / corrupt: records-only rebuild
  -> rebuild success: best-effort write next snapshot
  -> VINFO exposes load / fallback / write status
```

`VINFO` 只暴露 machine-checkable snapshot status，不暴露完整 snapshot 文件路径，不改变 `VSEARCH` response。snapshot 仍是 derived cache；records / RDB / AOF 仍是 source of truth。

HNSW-S6 已把 snapshot writer 接到当前已有的 shadow rebuild publish skeleton：当 test/internal shadow promote 成功后，如果当前 backend 是 `core_hnsw_v2`，会 best-effort 写出新的 `vector_index.snapshot` 和 `vector_index.manifest`，并更新 S5 已暴露的 write status。当前 shadow publish 仍是 harness / skeleton，不是完整 background rebuild worker，也没有让 `VSEARCH` 读取 rebuild context active pointer。

## HNSW-S-final 结论

当前可以把 HNSW-S 视为收口，不建议继续在 S 线增加功能。后续工作应分流：

- `HNSW-R`：真正 background shadow rebuild、reader safety、active / shadow / retired 产品化、delta replay / safe swap。
- `HNSW-buildperf`：bulk-build API、parallel build、reverse prune / candidate selection、allocator / memory layout。
- real/proxy embedding benchmark：recall@10、QPS、p50 / p95 / p99、snapshot load time、cold-start no-snapshot vs with-snapshot 对比。
- future `HNSW-S`：snapshot GC、多版本 snapshot、mmap/view、更强 fsync/crash consistency。
