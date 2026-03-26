# 第 5 步：RDB 接入 TTL

## 1. 本步目标

本步要把 TTL 变成 RDB 可保存、可恢复、可过滤的状态，保证重启后基本语义正确：

- 保存时，过期 key 不进入快照
- 恢复时，未过期 key 恢复 `expire_at_ms`
- 恢复时，已过期 key 不再回灌内存
- 整体采用绝对毫秒时间戳，不把相对 TTL 写进 RDB

本步仍然坚持最小侵入式改法，不重做整个 RDB 框架。

## 2. 当前 RDB 链路分析

### 2.1 保存入口

当前保存入口在 [src/persistence/kvs_rdb.c](src/persistence/kvs_rdb.c) 的 `kvs_rdb_save`，启动和退出路径分别由 [src/core/kvstore.c](src/core/kvstore.c) 的 `main` 触发。

当前 RDB 文件头部顺序是：

1. magic：`KVSRDB`
2. version：`RDB_VERSION`
3. snapshot 时间戳：`time_t timestamp`
4. 若干数据块：array / rbtree / hash / skiptable
5. 结束标记：`0xFF`

### 2.2 当前 key 编码方式

当前每条记录的编码方式是长度前缀形式：

- `key_len` + `key`
- `value_len` + `value`

也就是说，RDB 里 key/value 是原样二进制保存，只是带长度，不依赖字符串终止符。

### 2.3 当前加载路径

加载入口在 `kvs_rdb_load`，流程是：

1. 校验 magic
2. 读取版本号
3. 读取 snapshot 时间戳
4. 清空当前内存态数据结构
5. 按 type 逐块加载各后端数据

当前实现里，加载时并没有 TTL 语义：读到什么就恢复什么。

## 3. TTL 在 RDB 中的表示方式

### 3.1 选择的表示

本步采用“每条记录追加一个 `int64_t expire_at_ms`”的方式。

单条记录的新布局变成：

- `key_len`
- `key`
- `value_len`
- `value`
- `expire_at_ms`

其中：

- `-1` 表示永不过期
- `>= 0` 表示绝对过期时间，单位毫秒

### 3.2 为什么选这种方式

原因是当前仓库没有统一 KV 元对象，TTL 已经下沉到四个节点结构上，所以 RDB 最自然的做法就是把同一个字段按记录写回去，而不是额外引入独立 expires 字典。

这种表示方式的优点：

- 和当前内存结构一致，保存/恢复逻辑对齐
- 不需要额外索引或复杂二级结构
- 复制和 AOF 之外，RDB 自身就能保留完整过期点
- 可以直接用当前时间判断是否已经过期，不需要重算相对 TTL

### 3.3 版本选择

为了避免旧文件误解析，本步把 `RDB_VERSION` 升级为 2。

兼容策略是：

- version 1：按旧格式读取，没有 TTL 字段，默认 `expire_at_ms = -1`
- version 2：按新格式读取，恢复 `expire_at_ms`

这是最小兼容改法：文件格式只在每条记录后多一个字段，不引入新分块，也不重排头部结构。

## 4. 保存路径改动

### 4.1 统一保存原则

保存时要按“当前有效状态”落盘：

- 已过期记录不写入快照
- 未过期记录写入 `expire_at_ms`

这能避免快照本身携带已经失效的数据。

### 4.2 各块保存方式

- array：遍历 `global_array.table`，跳过过期项，写入 key/value/expire_at_ms
- rbtree：中序遍历时跳过过期节点，只保存有效节点
- hash：扫描槽位时跳过过期项，只保存 `used == 1` 且未过期的节点
- skiptable：沿 `forward[0]` 遍历时跳过过期节点，只保存有效节点

### 4.3 count 的处理

由于会跳过过期项，各数据块的记录数不能直接沿用内存态总数，所以保存时需要写占位 count，再回填实际写入数量。

这仍然是最小侵入方式，不需要修改块边界设计。

## 5. 加载路径改动

### 5.1 读取策略

加载时先读出记录本身，再读 `expire_at_ms`：

- version 1：没有 `expire_at_ms`，默认 `-1`
- version 2：读取 `expire_at_ms`

### 5.2 恢复策略

恢复时使用当前时钟 `kv_now_ms()` 判断：

- 如果 `expire_at_ms` 已经过期，则跳过，不恢复到内存
- 如果 `expire_at_ms` 仍有效，则恢复 key/value，并把 `expire_at_ms` 写回节点

### 5.3 为什么选择“跳过”而不是“恢复后立即删除”

两种策略都能达到“加载后内存里不该存在过期 key”的目标，但本步选择跳过，原因是：

- 更简单，恢复路径不需要再走一次删除链路
- 不会在加载阶段触发额外的写放大
- 不会在启动阶段生成多余的 AOF / 复制副作用
- 语义更直接：快照里已失效的数据不应进入有效内存态

## 6. 已过期 key 的恢复策略

本步明确采用：加载时直接跳过。

理由如下：

1. 避免启动后短暂暴露已过期数据
2. 避免加载后立刻再删，减少恢复路径复杂度
3. 保持 RDB 快照的语义更接近“当前有效状态”而不是“历史记录 + 过期标签”

补充说明：

- 保存前如果 key 已经过期，也会被过滤掉
- 加载时如果 key 在保存时未过期、但在恢复时已经过期，也会被跳过

这样能同时覆盖“保存时过期”和“恢复时过期”两类情况。

## 7. 修改文件列表

- [include/kvs_rdb.h](include/kvs_rdb.h)
- [include/kvstore.h](include/kvstore.h)
- [src/persistence/kvs_rdb.c](src/persistence/kvs_rdb.c)
- [tests/test_ttl_rdb.sh](tests/test_ttl_rdb.sh)

## 8. 风险点

1. 版本升级会让旧代码无法直接识别新文件。
   - 处理方式：加载端同时兼容 version 1 和 version 2

2. `time_t` 在文件头里仍然保留原样。
   - 影响：它只用于打印快照时间，不参与 TTL 计算

3. 保存 / 加载过程中对 `expire_at_ms` 的回填依赖当前节点查找。
   - 影响：加载阶段有额外扫描成本，但这是启动路径，成本可接受

4. 不同后端的删除语义不完全一致。
   - 影响：测试脚本需要按后端真实返回值写断言

## 9. 验证方式

### 9.1 编译验证

- 执行 `make`

### 9.2 save/load 回归脚本

运行：

```text
bash tests/test_ttl_rdb.sh
```

建议验证的场景：

1. 永不过期 key 保存后重启仍存在
2. 长 TTL key 保存后重启仍存在，且 TTL 变短但仍为正
3. 保存前已过期的 key 不会出现在 RDB 中
4. 保存时未过期、但重启前已经过期的 key，加载时会被跳过

### 9.3 手工命令序列

可以按以下思路手工验证：

```text
SET persist v1
SET survive v2
EXPIRE survive 60
SET skip_before_save v3
EXPIRE skip_before_save 1
SET expire_before_load v4
EXPIRE expire_before_load 3
sleep 2
退出服务，触发 RDB 保存
sleep 4
重启服务
GET persist
GET survive
GET skip_before_save
GET expire_before_load
```

预期：

- `persist` 仍存在
- `survive` 仍存在，TTL 仍大于 0
- `skip_before_save` 不存在
- `expire_before_load` 不存在

## 10. 本步结论

本步用最小兼容改法把 TTL 接入 RDB：

- 记录级别追加 `expire_at_ms`
- 保存时过滤已过期 key
- 加载时恢复 TTL 或跳过过期 key
- 版本 1 / 2 同时兼容

这样可以保证 RDB 不再丢失 TTL 语义，也不会把过期数据重新灌回内存。

## 11. 下一步建议

下一步建议进入 AOF 与主从复制对齐：

1. 让 TTL 变更命令进入 AOF
2. 让主从同步携带 TTL 状态
3. 统一恢复、回放和导出路径的 TTL 语义

这样 TTL 才能真正覆盖“重启、复制、恢复”三条主链路。