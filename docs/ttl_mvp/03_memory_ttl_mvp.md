# 第 3 步：内存态 TTL 基础能力

## 目标

只实现 TTL 的内存态核心能力，不接入 RDB / AOF / 主从复制 / 后台扫描。

## 本步范围

- 在节点结构上增加 `expire_at_ms`
- 提供统一时间与过期判断辅助函数
- 实现 `EXPIRE key seconds`
- 实现 `TTL key`
- 实现 `PERSIST key`
- 给 `GET / EXISTS` 加惰性删除
- 保持 `SET / MOD` 覆盖时清空 TTL
- 保持 `DEL` 删除时一并移除 TTL 元数据

## 实现策略

- 采用绝对毫秒时间戳 `expire_at_ms`
- `-1` 表示永不过期
- 读路径优先做惰性删除，避免引入额外后台线程
- TTL 命令先做最小可运行闭环，不扩展到持久化与复制链路

## 代码改动点

- `include/kvs_types.h`
- `include/kvs_array.h`
- `include/kvs_rbtree.h`
- `include/kvs_hash.h`
- `include/kvs_skiptable.h`
- `include/kvs_command.h`
- `src/storage/kvs_array.c`
- `src/storage/kvs_rbtree.c`
- `src/storage/kvs_hash.c`
- `src/storage/kvs_skiptable.c`
- `src/core/kvs_operation.c`

## 语义约定

- `EXPIRE` 成功返回 `OK`
- `EXPIRE` 目标不存在返回 `NO EXIST`
- `TTL` 返回剩余秒数
- `TTL` 目标不存在或已过期返回 `-2`
- `TTL` 永不过期返回 `-1`
- `PERSIST` 成功返回 `OK`
- `PERSIST` 目标不存在返回 `NO EXIST`

## 验证方式

- 执行 `make`
- 手工验证如下命令序列：
  - `SET a 1`
  - `EXPIRE a 1`
  - `TTL a`
  - `GET a`
  - `PERSIST a`
  - `DEL a`
- 再验证一个过期场景：
  - `SET b 2`
  - `EXPIRE b 1`
  - 等待 1 秒后执行 `GET b`
  - 期望返回 `NO EXIST`

## 不做的事情

- 不修改 RDB/AOF
- 不修改复制协议
- 不添加后台 TTL 扫描线程
- 不新增独立的 TTL 持久化格式
