# Stage 2E core_flat regression report

## Stage 2D Status

Stage 2D 已完成，当前 core_flat 已通过最小 adapter 接入真实 V 族命令路径：

- `vector_index_type=core_flat`
- `--vector_index_type core_flat`
- `KVS_VECTOR_INDEX_CORE_FLAT`
- `core_flat_index` manager state
- core_flat old-ops adapter
- VSET 写入 core_flat
- VSEARCH 走 core_flat，并由 manager resolve record_id / score
- VDEL 调用 core_flat mark_delete
- VINDEX REBUILD 调用 core_flat rebuild
- VINFO 显示 `index_type:core_flat`
- RDB reload 后 `kvs_vector_rebuild_after_load()` 已补 core_flat rebuild 分支

## Stage 2E Test Scripts

本阶段新增正式测试脚本：

```text
tests/vector_core_flat_correctness_test.sh
tests/vector_core_flat_rebuild_test.sh
tests/vector_core_flat_rdb_reload_test.sh
```

三个脚本均使用独立 `/tmp` 临时目录、临时配置、临时 RDB 文件和动态端口。脚本通过 RESP socket 直接校验返回结构，不依赖交互式 client 的显示格式。

## Correctness Test

脚本：

```bash
tests/vector_core_flat_correctness_test.sh
```

覆盖：

- 使用配置文件 `vector_index_type = core_flat` 启动 kvstore
- 使用命令行 `--vector_index_type core_flat` 覆盖配置文件中的 `flat`
- VSET 多条记录
- VSEARCH 返回 scope 内结果
- scopeA / scopeB 隔离
- 同 key/scope 更新后 answer 正确更新
- 同 key/scope 更新后不产生重复可见结果
- VDEL 后不返回删除记录
- repeated delete 不崩溃，不重复增加 `core_flat_deleted_count`
- VINFO 显示 `index_type:core_flat`
- VSEARCH 返回中不暴露 `record_id` / `slot`

结果：

```text
vector_core_flat_correctness_test PASS
```

## Rebuild Test

脚本：

```bash
tests/vector_core_flat_rebuild_test.sh
```

覆盖：

- 使用 `vector_index_type=core_flat` 启动 kvstore
- VSET 多条 scopeA / scopeB 记录
- VDEL 删除部分记录
- VINDEX REBUILD
- rebuild 后 active records 仍可查
- deleted records 不可见
- rebuild 后 `core_flat_deleted_count` 回到 `0`
- VINFO 仍显示 `index_type:core_flat`
- scope 过滤仍正确

结果：

```text
vector_core_flat_rebuild_test PASS
```

## RDB Reload Test

脚本：

```bash
tests/vector_core_flat_rdb_reload_test.sh
```

覆盖：

- 使用 `vector_index_type=core_flat` 启动 kvstore
- VSET 多条记录
- 更新一条记录
- 删除一条记录
- SIGTERM 优雅 shutdown 生成 RDB
- 使用同一 RDB 重新启动 kvstore
- reload 后 VGET active record 正常
- reload 后 VSEARCH active records 正常
- reload 后 deleted records 不可见
- reload 后 updated answer 正确
- reload 后 VINDEX REBUILD 成功
- reload + rebuild 后 `core_flat_deleted_count` 为 `0`
- core_flat 不作为持久化事实来源，仍从 record store rebuild

结果：

```text
vector_core_flat_rdb_reload_test PASS
```

## RDB Reload Bug Confirmation

Stage 2D-3 曾发现并修复 RDB reload 后 core_flat 未自动 rebuild 的问题：

- reload 后 record store 和 id_map 已恢复。
- 但 core_flat 没有在 `kvs_vector_rebuild_after_load()` 中从 active records rebuild。
- reload 后 VSEARCH 曾返回 `ERR invalid vector argument`。

当前确认：

- `tests/vector_core_flat_rdb_reload_test.sh` 已覆盖 reload 后直接 VSEARCH active records。
- 测试通过，说明该问题已修复。
- 修复没有修改 RDB/AOF 格式。
- 修复没有把 core_flat index 当作持久化事实来源。

## Config Verification

配置文件方式：

```text
vector_index_type = core_flat
```

验证结果：

- kvstore 正常启动。
- VINFO 显示 `index_type:core_flat`。
- 三个正式测试均覆盖该路径。

命令行方式：

```bash
./kvstore --config /tmp/.../cli.conf --vector_index_type core_flat
```

其中临时配置文件故意设置：

```text
vector_index_type = flat
```

验证结果：

- 命令行覆盖生效。
- VINFO 显示 `index_type:core_flat`。
- 覆盖路径由 `tests/vector_core_flat_correctness_test.sh` 验证。

默认 index type：

- `kvs_config_init_default()` 默认值仍为 `hnsw`。
- 本阶段未修改默认 index type。

## Static Boundary Checks

执行：

```bash
grep -RIn "g_vector_state" \
  include/vector/kvs_vector_flat.h \
  src/vector/core/flat \
  src/vector/core/kvs_vector_id_slot_map.c \
  src/vector/core/kvs_vector_heap.c 2>/dev/null
```

结果：无输出。

执行：

```bash
grep -RIn "question\\|answer\\|scope\\|TTL\\|metadata" \
  include/vector/kvs_vector_flat.h \
  src/vector/core/flat \
  src/vector/core/kvs_vector_id_slot_map.c \
  src/vector/core/kvs_vector_heap.c 2>/dev/null
```

结果：

```text
include/vector/kvs_vector_flat.h:19: *   question / answer / scope / TTL / metadata / manager store
include/vector/kvs_vector_flat.h:23: *   scope filtering
include/vector/kvs_vector_flat.h:24: *   TTL filtering
```

结论：

- 业务语义字段只出现在 core_flat 头文件边界注释中。
- core_flat 逻辑代码没有处理 question / answer / scope / TTL / metadata。
- scope 过滤仍由 manager resolve 负责。

## Full Build

执行：

```bash
make clean
make ENABLE_HNSWLIB=1 ENABLE_FAISS=1 -j"$(nproc)"
```

结果：

```text
✓ 构建完成
二进制: kvstore src/eBPF/ebpf_syncd
eBPF:   src/eBPF/ebpf_kern.o
```

clean build 后再次执行三个正式测试，全部通过：

```text
vector_core_flat_correctness_test PASS
vector_core_flat_rebuild_test PASS
vector_core_flat_rdb_reload_test PASS
```

## Production Code Changes

本阶段原则上只做测试沉淀和报告整理。

本阶段未继续修改生产代码。当前工作区已有生产代码改动来自 Stage 2D 已完成内容，包括 core_flat adapter、config/bind/rebuild/VINFO/VDEL 接入，以及 Stage 2D-3 的 RDB reload 后 core_flat rebuild 修复。

本阶段新增：

```text
tests/vector_core_flat_correctness_test.sh
tests/vector_core_flat_rebuild_test.sh
tests/vector_core_flat_rdb_reload_test.sh
docs/vector_core_reset/stage2e_core_flat_regression_report.md
```

## Git Status

执行：

```bash
git status --short
```

当前输出：

```text
 M Makefile
 M include/kvs_vector.h
 M include/vector/kvs_vector_manager.h
 M src/core/kvs_command_executor.c
 M src/core/kvs_config.c
 M src/core/kvs_operation.c
 M src/vector/kvs_vector.c
?? docs/vector_core_reset/stage2d_core_flat_integration_audit.md
?? docs/vector_core_reset/stage2d_core_flat_integration_regression.md
?? docs/vector_core_reset/stage2e_core_flat_regression_report.md
?? include/vector/kvs_vector_flat.h
?? include/vector/kvs_vector_heap.h
?? include/vector/kvs_vector_id_slot_map.h
?? src/vector/core/
?? tests/vector_core_flat_correctness_test.sh
?? tests/vector_core_flat_rdb_reload_test.sh
?? tests/vector_core_flat_rebuild_test.sh
```
