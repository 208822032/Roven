# 第 8 步：轻量后台扫描 + 全量测试 + 面试总结

## 1. 本步目标

本步完成两件事：

1. 落地一个“惰性删除为主、后台扫描为辅”的轻量 TTL 兜底线程。
2. 把 TTL MVP 的测试矩阵整理成可直接执行的命令序列，并补齐面试总结文档。

本步仍然不引入最小堆、时间轮或独立 expires 索引，后台扫描只做有限预算的周期清理。

## 2. 后台扫描接入点分析

当前仓库里最适合复用的机制是主进程生命周期 + 周期后台线程：

- [src/core/kvstore.c](src/core/kvstore.c) 负责启动和退出，是线程生命周期最自然的挂载点。
- [src/persistence/kvs_aof.c](src/persistence/kvs_aof.c) 已经有周期唤醒的后台 fsync 线程，可以作为轻量后台任务的实现参考。
- [src/core/kvs_ttl_background.c](src/core/kvs_ttl_background.c) 已经承载了 TTL 后台扫描逻辑，直接复用即可。

当前实现把扫描线程挂在 [src/core/kvstore.c](src/core/kvstore.c) 的启动流程中：

- 服务启动后创建 TTL 后台线程。
- 服务退出前停止后台线程。
- 扫描和写命令共用 [include/kvs_globals.h](include/kvs_globals.h) 暴露的全局数据锁。

## 3. 扫描策略设计

### 3.1 总体原则

后台扫描只做兜底，不做主裁判。真正的过期判定仍然以读路径惰性删除为主，后台线程只负责补充清理冷 key。

### 3.2 扫描节奏

当前实现采用固定节拍：

- 周期为 1 秒。
- 每轮只处理 32 个候选节点。

这意味着扫描线程不会长时间占用 CPU，也不会把前台请求锁住太久。

### 3.3 扫描对象

扫描线程直接遍历四个后端节点里的 `expire_at_ms`：

- [include/kvs_array.h](include/kvs_array.h)
- [include/kvs_rbtree.h](include/kvs_rbtree.h)
- [include/kvs_hash.h](include/kvs_hash.h)
- [include/kvs_skiptable.h](include/kvs_skiptable.h)

扫描时只判断：`expire_at_ms >= 0 && expire_at_ms <= now_ms`。

### 3.4 删除策略

当前实现对扫描命中的过期 key 采取两步：

1. 先删除本地 key。
2. 如果节点是 master 且同步模式是 TCP，再广播 `DEL` 给 slave。

这样能避免后台扫描只删 master 本地、却不通知从节点的问题。

## 4. 扫描成本控制

成本控制点很明确：

1. 固定周期唤醒，不忙等。
2. 固定预算 32，不做全量扫。
3. 只持有一把全局数据写锁。
4. slave 不主动扫描，只跟随 master 的删除结果。

这个设计的目标不是“尽快扫空全部过期 key”，而是“用可控成本不断推进回收”。

## 5. 修改文件列表

### 5.1 代码文件

- [src/core/kvs_ttl_background.c](src/core/kvs_ttl_background.c)
- [src/core/kvstore.c](src/core/kvstore.c)
- [include/kvs_globals.h](include/kvs_globals.h)
- [Makefile](Makefile)

### 5.2 文档文件

- [docs/ttl_mvp/08_background_scan_and_test.md](docs/ttl_mvp/08_background_scan_and_test.md)
- [docs/ttl_mvp/09_ttl_mvp_interview_summary.md](docs/ttl_mvp/09_ttl_mvp_interview_summary.md)

### 5.3 本步实际改动点

- [src/core/kvs_ttl_background.c](src/core/kvs_ttl_background.c)：后台扫描线程、有限预算遍历、过期删除和 TCP 主从广播。
- [src/core/kvstore.c](src/core/kvstore.c)：启动/停止后台线程。
- [include/kvs_globals.h](include/kvs_globals.h)：导出后台线程接口和共享锁。
- [Makefile](Makefile)：把后台扫描源文件纳入构建。

## 6. 全量测试清单

本步整理的测试命令可以直接执行，按链路分层跑：

### 6.1 构建

```bash
make -j2
```

### 6.2 内存态 TTL 主矩阵

```bash
bash tests/ttl/test_ttl_all_backends.sh
```

验证内容：

- `EXPIRE`
- `TTL`
- `PERSIST`
- `GET` / `EXIST` 的惰性删除
- `RANGE` / `SORT` / `FULLDUMP` 的过期过滤
- 四个后端的差异化删除语义

### 6.3 RDB 恢复

```bash
bash tests/test_ttl_rdb.sh
```

验证内容：

- 已过期 key 不入库。
- 未过期 key 恢复 `expire_at_ms`。
- 重启后 TTL 语义不丢。

### 6.4 AOF 回放与重写

```bash
bash tests/test_ttl_aof.sh
```

验证内容：

- `EXPIRE` 规范化成绝对过期语义。
- `PERSIST` 回放后 TTL 清除。
- `AOFREWRITE` 后 TTL 仍保留。

### 6.5 TCP 主从复制

```bash
bash tests/test_ttl_tcp_replication.sh
```

验证内容：

- master 广播 `PEXPIREAT` / `PERSIST` / `DEL`。
- slave 只回放，不主动过期。
- 全量同步带上 TTL。

### 6.6 eBPF 主从复制

```bash
sudo bash tests/test_ttl_ebpf_replication.sh
```

验证内容：

- eBPF 同步链路下 TTL 仍保持一致。
- master / slave / syncd 三方联动可跑通。

### 6.7 兼容入口

```bash
bash tests/test_ttl_mvp.sh
```

这个脚本只是薄包装，真实逻辑仍然回到 [tests/ttl/test_ttl_all_backends.sh](tests/ttl/test_ttl_all_backends.sh)。

## 7. 风险点

1. 后台扫描是 best-effort，不保证精确到秒级删除。
2. 扫描需要共享数据写锁，极端情况下会和前台写命令争锁。
3. slave 不主动扫描，所以主从一致性仍然依赖 master 广播删除。
4. TTL 测试依赖时间窗口，脚本必须用轮询断言，不能只靠一次性 sleep。
5. 后台扫描广播 DEL 属于额外同步开销，但这是为了维持复制一致性，代价可接受。

## 8. 验证方式

本步验证分三层：

1. 编译验证：`make -j2`
2. 功能验证：`bash tests/ttl/test_ttl_all_backends.sh`
3. 链路验证：`bash tests/test_ttl_rdb.sh`、`bash tests/test_ttl_aof.sh`、`bash tests/test_ttl_tcp_replication.sh`、`sudo bash tests/test_ttl_ebpf_replication.sh`

如果只想快速看 TTL 主矩阵，先跑 `bash tests/ttl/test_ttl_all_backends.sh` 就够。

## 9. 本步结论

KVLite 的 TTL MVP 最终落成的是一套轻量闭环：

- 读路径先做惰性删除。
- 后台线程按固定周期和固定预算补充清理。
- 后台删除会在 master TCP 路径上继续广播 `DEL`。
- 测试覆盖内存、RDB、AOF、TCP 复制和 eBPF 复制。

这套实现没有引入复杂索引结构，但已经把 TTL 语义、恢复语义和复制语义收敛到同一条链路上。