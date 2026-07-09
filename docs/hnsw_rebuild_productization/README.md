# HNSW-R rebuild 产品化路线

本目录用于存放 HNSW-R product background rebuild 路线文档。HNSW-S 已完成 `core_hnsw_v2` snapshot 最小闭环，并通过 100K load-only benchmark 证明 valid snapshot cold-start 可以绕过约 50.2 分钟的 records-only rebuild；下一阶段的核心问题不再是 snapshot load，而是 product-safe background rebuild。

## 与 AGENTS.md 的关系

已读取并遵守仓库根目录 `AGENTS.md`。

冲突说明：`AGENTS.md` 的历史阶段模板要求阶段报告写入 `docs/vector_core_reset/`；本任务明确要求为 HNSW-R 新建独立目录 `docs/hnsw_rebuild_productization/`。本目录按任务要求创建，同时继续遵守 AGENTS 的核心边界：不修改 RDB/AOF 主格式、不修改 `VSEARCH` response、不修改 `kvs_vector_index_ops_t`、不改变默认 backend / 默认 HNSW 参数、不让 vector_core 访问 manager-owned 语义状态。

## HNSW-S / HNSW-R 边界

HNSW-S 负责：

```text
records restore 后加载 valid graph cache
snapshot invalid/missing/corrupt 时 fallback records-only rebuild
rebuild / VINDEX REBUILD / shadow publish skeleton 成功后 best-effort 写 snapshot
VINFO 暴露 snapshot load / write / fallback 状态
```

HNSW-S 不负责：

```text
降低 anncore build CPU 成本
background rebuild worker
reader safety
active/shadow/retired 产品级发布
delta capture/replay 的产品级并发语义
snapshot GC
mmap serving
```

HNSW-R 负责：

```text
snapshot invalid/missing 时避免启动同步卡死在 full rebuild
online rebuild 不阻塞 VSEARCH
current active 持续服务
shadow index 后台构建
VSET/VDEL append delta
shadow replay delta
final consistency validation
reader-safe publish
retired index 延迟回收
publish 成功后复用 HNSW-S snapshot writer
```

## 当前结论

valid snapshot 正常启动路径已经可接受：100K / 512d / balanced default 的 snapshot load-only 平均约 5.92 秒，p95 约 6.80 秒，相比 records-only rebuild 约 50.2 分钟有约 508.45x 加速。

因此：

```text
background snapshot load is not HNSW-R first priority
HNSW-R primary goal is product-safe background rebuild
```

## HNSW-R final 状态

HNSW-R1 到 HNSW-R7 已完成最小 product background rebuild 闭环：

```text
state machine hardening
  -> reader safety rwlock
  -> product active/shadow/retired publish
  -> delta capture generation boundary
  -> shadow full scan + delta replay + final validation
  -> async rebuild worker / command
  -> post-publish best-effort snapshot writer hook
```

最终收口报告：

- [hnsw_r_final_product_rebuild_consolidation_report.md](hnsw_r_final_product_rebuild_consolidation_report.md)

## 入口文档

- [hnsw_r_execution_plan.md](hnsw_r_execution_plan.md): HNSW-R0 到 HNSW-R7 的完整执行路线。
- [index.md](index.md): 本目录索引和后续阶段报告命名约定。
