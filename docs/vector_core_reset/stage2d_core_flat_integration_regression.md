# Stage 2D-3 core_flat integration regression

## Scope

本轮验证 core_flat 在真实 KVLite V 族命令路径下的基础正确性：

- VSET / VSEARCH / VDEL / VINDEX REBUILD / VINFO
- scope 过滤
- 同 key/scope 更新
- repeated delete
- scope overfetch
- RDB reload 后 rebuild
- 配置文件和命令行选择 core_flat

本轮没有修改 RDB/AOF 格式，没有修改 `kvs_vector_index_ops_t` 签名，没有接入 core_hnsw，没有让 core_flat 处理 scope / TTL / metadata / question / answer。

## Test Script

临时测试脚本：

```bash
/tmp/kvlite_stage2d3_regression.py
```

脚本使用临时目录启动真实 `./kvstore`，通过 RESP socket 执行 V 族命令。测试数据、配置文件、RDB 文件都放在 `/tmp/kvlite_stage2d3_*`，不写入仓库。

执行结果：

```text
stage2d3 core_flat integration regression PASS
cmdline: {'index_type': 'core_flat'}
config/rdb: {
  'update_info_deleted_count': '0',
  'deleted_count_after_first_delete': '1',
  'deleted_count_after_repeated_delete': '1',
  'deleted_count_after_rebuild': '0',
  'reload_deleted_count_after_rebuild': '0',
  'rdb_size': 4589
}
```

## Command Coverage

### Basic VSET / VSEARCH

启动配置：

```text
vector_index_type = core_flat
embedding_provider = hash
embedding_dim = 64
vector_metric = ip
vector_score_threshold = -1.0
```

执行命令：

```text
VSET q1 a1 scopeA
VSET q2 a2 scopeA
VSET q3 a3 scopeB
VSEARCH q1 scopeA 2
VSEARCH q1 scopeB 2
```

结果：

- `scopeA` 查询只返回 `scopeA` 记录，没有泄漏 `q3/scopeB`。
- `scopeB` 查询只返回 `q3/scopeB`，没有泄漏 `q1/q2 scopeA`。
- VSEARCH 返回格式仍是旧格式字符串，例如 `score:...|scope:...|question:...|answer:...`。
- 返回中没有暴露 record_id / slot。

### Same key/scope update

执行命令：

```text
VSET q1 a1_new scopeA
VGET q1 scopeA
VSEARCH q1 scopeA 2
VINFO
```

结果：

- `VGET q1 scopeA` 返回 `a1_new`。
- `VSEARCH q1 scopeA 2` 中 `q1` 只出现一次。
- `core_flat_deleted_count` 保持 `0`。
- 同 record_id 更新后 core_flat 可见结果正确覆盖，没有重复可见结果。

### VDEL and repeated delete

执行命令：

```text
VDEL q1 scopeA
VSEARCH q1 scopeA 3
VINFO
VDEL q1 scopeA
VINFO
```

结果：

- 第一次 `VDEL q1 scopeA` 返回 `OK`。
- 删除后 `VSEARCH q1 scopeA 3` 不返回 `q1`。
- 第一次删除后 `core_flat_deleted_count` 从 `0` 增加到 `1`。
- repeated delete 返回 `NO EXIST`，不崩溃。
- repeated delete 后 `core_flat_deleted_count` 仍为 `1`，没有重复增加。

### Scope overfetch

构造数据：

```text
scopeA: qa1, qa2
scopeB: qb1, qb2, qb3, qb4, qb5, qb6
```

执行命令：

```text
VSEARCH qa1 scopeA 2
```

结果：

- 返回包含 `qa1` 和 `qa2`。
- 不返回 `qb1` 到 `qb6`。
- 说明 adapter 的 overfetch 能支撑 manager 层 scope resolve。
- core_flat 本体仍不处理 scope，scope 过滤发生在 manager resolve 阶段。

### VINDEX REBUILD

执行命令：

```text
VINDEX REBUILD
VINFO
VSEARCH q2 scopeA 3
VSEARCH q3 scopeB 3
VSEARCH q1 scopeA 3
```

结果：

- `VINDEX REBUILD` 返回 `OK`。
- `VINFO` 仍显示 `index_type:core_flat`。
- rebuild 后 `core_flat_deleted_count:0`。
- active records `q2`、`q3` 仍可查。
- deleted record `q1` 不可见。

### RDB reload + rebuild

流程：

1. 以 `vector_index_type = core_flat` 启动。
2. 写入 `rdb_alive1`、`rdb_alive2`、`rdb_update`、`rdb_delete`。
3. 更新 `rdb_update` 为新 answer。
4. 删除 `rdb_delete`。
5. 发送 SIGTERM，让 kvstore 优雅退出并保存 RDB。
6. 使用同一配置和同一 RDB 文件重启。
7. 执行 VGET / VSEARCH / VINDEX REBUILD / VINFO。

结果：

- RDB 文件生成成功，测试中大小为 `4589` bytes。
- reload 后 `VGET rdb_alive1 scopeR` 正常。
- reload 后 active records 可通过 VSEARCH 查到。
- reload 后 `rdb_update` 的 answer 是 `new`。
- reload 后 deleted record `rdb_delete` 不可见。
- reload 后执行 `VINDEX REBUILD` 返回 `OK`。
- reload + rebuild 后 `core_flat_deleted_count:0`。
- core_flat 不依赖自身持久化 index，仍从 record store active records rebuild。

## Config Selection

配置文件方式：

```text
vector_index_type = core_flat
```

结果：

- kvstore 启动成功。
- `VINFO` 显示 `index_type:core_flat`。

命令行方式：

测试脚本使用配置文件提供端口、RDB、网络模式等运行环境，并在配置文件中设置：

```text
vector_index_type = flat
```

启动时使用命令行覆盖：

```bash
./kvstore --config /tmp/.../cmdline_base.conf --vector_index_type core_flat
```

结果：

- kvstore 启动成功。
- `VINFO` 显示 `index_type:core_flat`。
- 说明命令行 `--vector_index_type core_flat` 覆盖配置文件生效。

默认 index type 检查：

- `kvs_config_init_default()` 中默认值仍为 `hnsw`，本轮未修改默认 index type。

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
grep -RIn "question|answer|scope|TTL|metadata" \
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

这些只出现在 header 注释中，core_flat 逻辑代码没有处理 question / answer / scope / TTL / metadata。

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

## Bug Found And Fixed

发现的问题：

- RDB reload 后，record store 和 id_map 已恢复，`VGET rdb_alive1 scopeR` 正常。
- 但 reload 后首次 `VSEARCH rdb_alive1 scopeR 5` 返回 `ERR invalid vector argument`。
- 手工执行 `VINDEX REBUILD` 后 VSEARCH 恢复正常。

原因：

- `kvs_vector_rebuild_after_load()` 原本只重建旧 flat / ANN / SQ8 / hnswlib 路径。
- 当前 index type 为 `KVS_VECTOR_INDEX_CORE_FLAT` 时，load 后没有调用 core_flat adapter rebuild。
- 因此 core_flat 没有从 active records 重建，搜索路径进入空或不完整 index 状态。

修复点：

- 文件：`src/vector/kvs_vector.c`
- 函数：`kvs_vector_rebuild_after_load()`
- 位置：`kvs_vector_index_bind_current()` 成功之后、hnswlib rebuild 分支之前。
- 修复：当 `g_vector_state.index_type == KVS_VECTOR_INDEX_CORE_FLAT` 时，调用当前 adapter 的 `rebuild` 回调，从 active records 重建 core_flat。

该修复没有修改 RDB/AOF 格式，没有修改 `kvs_vector_index_ops_t` 签名，没有改旧 flat / ANN / SQ8 / Faiss / hnswlib backend。

## Production Code Changes

本轮修改了生产代码一处，用于修复验证中暴露的必要 bug：

- `src/vector/kvs_vector.c`
  - `kvs_vector_rebuild_after_load()` 增加 `KVS_VECTOR_INDEX_CORE_FLAT` 专属 rebuild 分支。

本轮新增文档：

- `docs/vector_core_reset/stage2d_core_flat_integration_regression.md`

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
?? include/vector/kvs_vector_flat.h
?? include/vector/kvs_vector_heap.h
?? include/vector/kvs_vector_id_slot_map.h
?? src/vector/core/
```
