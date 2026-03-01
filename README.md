## KVStore

一个类 Redis 的轻量级 KV 引擎，支持多数据结构、RESP 协议、AOF/RDB 持久化，以及 TCP/eBPF 两种主从同步链路。

---

## 1. 项目定位

一个可落地运行的 C 语言 KV 数据库，覆盖“协议解析 -> 存储引擎 -> 持久化 -> 主从复制 -> 压测体系”的完整链路，并通过统一脚本对比 Redis 进行性能验证。

**核心职责/成果：**
- 设计并实现四种数据结构引擎：`array / rbtree / hash / skiptable`
- 完成 RESP 协议解析、Pipeline 支持与命令分发
- 实现 AOF/RDB 持久化和重放恢复
- 实现主从同步（`sync_mode=tcp|ebpf`）
- 统一压测框架（同口径对比 KVStore 与 Redis）、自定义读写、自定义内存池虚拟内存占用、自定义eBPF同步链路压测

---

## 2. 技术栈

- **语言与系统**：C、Linux、epoll、io_uring、pthread
- **协议与网络**：RESP、TCP、Pipeline
- **存储与内存**：自定义内存池、glibc malloc、jemalloc 可切换
- **持久化**：AOF（含 fsync 策略）、RDB
- **复制**：Master-Slave（TCP / eBPF）
- **工程化**：统一配置、自动化压测脚本、结果归档

---

## 3. 架构设计

### 3.1 总体架构图

```mermaid
flowchart LR
    C[Client / redis-benchmark] --> N[Network Layer<br/>Reactor / Proactor / NtyCo]
    N --> P[RESP Parser]
    P --> D[Command Dispatcher]
    D --> S1[array]
    D --> S2[rbtree]
    D --> S3[hash]
    D --> S4[skiptable]

    D --> AOF[AOF Append / Replay]
    D --> RDB[RDB Load / Save]

    D --> SYNC[Replication Layer]
    SYNC --> TCP[TCP Sync Backend]
    SYNC --> EBPF[eBPF Sync Daemon]

    M[Memory Backend<br/>pool / system / jemalloc] --> S1
    M --> S2
    M --> S3
    M --> S4
```

### 3.2 请求处理时序图（写命令）

```mermaid
sequenceDiagram
    participant Client
    participant Net as Network
    participant Resp as RESP Parser
    participant Cmd as Command Filter
    participant Store as Data Structure
    participant AOF as AOF
    participant Slave as Slave

    Client->>Net: TCP payload
    Net->>Resp: parse RESP
    Resp->>Cmd: tokens + argc
    Cmd->>Store: execute write command
    Store-->>Cmd: success
    Cmd->>AOF: aof_append(...)
    Cmd->>Slave: master_sync_command(...)
    Cmd-->>Client: RESP reply
```

### 3.3 主从同步架构图（TCP / eBPF）

```mermaid
flowchart TD
    W[Write Traffic] --> M[Master]
    M -->|sync_mode=tcp| ST[TCP Sync Port]
    ST --> S1[Slave1]
    ST --> S2[Slave2]

    W -->|sync_mode=ebpf| K[kprobe/tcp_sendmsg]
    K --> R[ringbuf]
    R --> D[ebpf_syncd]
    D --> S1
    D --> S2
```

---

## 4. 模块划分

| 模块 | 路径 | 说明 |
|---|---|---|
| 核心入口 | `src/core/kvstore.c` | 配置加载、全局初始化、启动网络与优雅退出 |
| 协议解析 | `src/network/resp_protocol.c` | RESP 命令探测、解析、状态机 |
| 存储引擎 | `src/storage/` | array/rbtree/hash/skiptable 实现 |
| 持久化 | `src/persistence/` | AOF/RDB 读写、重放、重写 |
| TCP 复制 | `src/distributed/` | 全量 + 增量同步 |
| eBPF 复制 | `src/eBPF/ebpf_syncd.c` | 抓取 master 流量并广播到 slave |
| 压测体系 | `scripts/bench_all.sh`、`test/*` | 标准对齐压测（KVStore vs Redis）+ 自定义压测（读写负载、内存后端对比、eBPF同步链路） |

---

## 5. 关键代码片段

### 5.1 写命令：执行后落盘 + 同步（`src/core/kvstore.c`）

```c
if(ret >= 0 && cmd->write_command){
#if ENABLE_AOF
    if(!aof_is_loading()){
        aof_append(count, tokens);
    }
#endif

    if(get_sync_context() != 2){
        if (global_dist_config.role == ROLE_MASTER && g_cfg.sync_mode == KVS_SYNC_TCP) {
            master_sync_command(tokens[0], tokens[1], (count > 2) ? tokens[2] : NULL);
        }
    }
}
```

### 5.2 RESP 完整命令探测（`src/network/resp_protocol.c`）

```c
int resp_peek_command(const char* buf, size_t len, size_t offset){
    if (!buf || offset >= len) return 0;
    ...
    if (*p != '*') return -1;
    ...
    for (long long i = 0; i < array_len; ++i) {
        if (*p != '$') return -1;
        ...
    }
    return (int)(p - (buf + offset));
}
```

### 5.3 主节点全量同步（`src/distributed/distributed.c`）

```c
#if ENABLE_ARRAY
for (int i = 0; i < global_array.total; i++) {
    if (global_array.table[i].key && global_array.table[i].value) {
        master_sync_command("SET", global_array.table[i].key, global_array.table[i].value);
    }
}
#endif
```

---

## 6. 性能报告

### 6.1 标准对齐压测（KVStore vs Redis）

- 压测工具：`redis-benchmark`
- 二进制路径：`/usr/local/bin/redis-benchmark`
- 版本：`redis-benchmark 6.2.21 (git:578ac274)`
- 对齐范围：`populate / set / get`（KVStore 与 Redis 使用同一工具与同口径参数，仅目标地址不同）
- 参数口径：`-c 50 -n 100000 -P 1 -d 32 -r 100000`

| ds | op | kvstore_qps | redis_qps | kv/redis |
|---|---:|---:|---:|---:|
| array | set | 1226.890000 | 108577.630000 | 0.011 |
| array | get | 1191.280000 | 129032.270000 | 0.009 |
| rbtree | set | 95785.440000 | 121802.680000 | 0.786 |
| rbtree | get | 94786.730000 | 112359.550000 | 0.844 |
| hash | set | 88967.980000 | 123001.230000 | 0.723 |
| hash | get | 90579.710000 | 124378.110000 | 0.728 |
| skiptable | set | 88495.580000 | 122699.390000 | 0.721 |
| skiptable | get | 93632.960000 | 121802.680000 | 0.769 |

- 当前瓶颈集中在 `array` 路径（最低 `array/get = 1191.28 qps`）
- `rbtree/hash/skiptable` 的 `set/get` 已进入高吞吐区间，与 Redis 差距显著缩小
- TCP 主从模式下吞吐损耗较小；eBPF 模式在启用批处理后，吞吐显著优于无批处理路径
- 后续优化优先级：`array` 路径、eBPF 大包与分片处理、内存后端按场景调优

> 原始数据：`results/raw.csv`  
> 汇总报告：`results/summary.md`

### 6.2 自定义压测数据：单机 vs 主从同步（TCP）

注：该组数据均为 `100000` 次写入，且测试时关闭 AOF。

| Mode         | Structure | count  | QPS      |
| ------------ | --------- | ------ | -------- |
| Standalone   | array     | 100000 | 17668.08 |
| Standalone   | rbtree    | 100000 | 16593.61 |
| Standalone   | hash      | 100000 | 16897.22 |
| Standalone   | skiptable | 100000 | 17324.24 |
| Master-Slave | array     | 100000 | 17156.03 |
| Master-Slave | rbtree    | 100000 | 16055.72 |
| Master-Slave | hash      | 100000 | 16452.01 |
| Master-Slave | skiptable | 100000 | 16559.29 |

### 6.3 自定义压测数据：单机 vs 主从同步（eBPF）

| Mode         | Structure | count  | QPS |
| ------------ | --------- | ------ | --- |
| Standalone   | array     | 100000 | 17668.08 |
| Standalone   | rbtree    | 100000 | 16593.61 |
| Standalone   | hash      | 100000 | 16897.22 |
| Standalone   | skiptable | 100000 | 17324.24 |
| Master-Slave | array     | 100000 | 11761.18（无批处理） |
| Master-Slave | rbtree    | 100000 | 11496.70（无批处理） |
| Master-Slave | hash      | 100000 | 11455.85（无批处理） |
| Master-Slave | skiptable | 100000 | 11270.69（无批处理） |
| Master-Slave | array     | 100000 | 15063.18（批处理） |
| Master-Slave | rbtree    | 100000 | 15041.32（批处理） |
| Master-Slave | hash      | 100000 | 15231.43（批处理） |
| Master-Slave | skiptable | 100000 | 15179.61（批处理） |

### 6.4 eBPF 批处理佐证（send_all 统计）

- 无批处理：每次发送通常对应一条小同步指令，`avg` 约 30B，系统调用频繁。
- 批处理：多条同步指令聚合后再发，`avg` 提升到 128KB（`131072`），系统调用次数显著下降。

| 场景 | send_all calls | send_all bytes | send_all avg (B) |
| ---- | -------------: | -------------: | ---------------: |
| 无批处理 | 8281 ~ 13553 | 256711 ~ 420143 | 31.0 ~ 31.8 |
| 批处理 | 3 ~ 5 | 393216 ~ 655360 | 131072.0 |

### 6.5 自定义压测数据：不同内存后端对比

**基础功能（pool / system / jemalloc）**

| BACKEND | PATTERN        | maxrss (KB) | QPS / OPS |
| ------- | -------------- | ----------- | --------- |
| POOL    | small-fixed    | 210576      | qps-small = 27027027.03 |
| POOL    | mid-fixed      | 210576      | read-heavy-small = 24005840 |
| POOL    | big-after-frag | 1029412     | time = 0 ms |
| SYSTEM  | small-fixed    | 10176       | qps-small = 26315789.47 |
| SYSTEM  | mid-fixed      | 52416       | read-heavy-small = 46167076.92 |
| SYSTEM  | big-after-frag | 1068024     | time = 0 ms |
| JEMALLOC| small-fixed    | 10188       | qps-small = 25641025.64 |
| JEMALLOC| mid-fixed      | 52428       | read-heavy-small = 49996000 |
| JEMALLOC| big-after-frag | 1068036     | time = 0 ms |

**混合写删负载（2次写入 → 1次删除 → 间隔后再写入1次 → 最后删除2次）的 QPS 对比**

| Backend  | Structure | n      | total_ops | QPS (ops/s) | 虚拟内存占用 (GB) |
|----------|-----------|--------|-----------|-------------|----------------|
| pool     | array     | 100000 | 600000    | 18085.46    | 1.88 |
| pool     | rbtree    | 100000 | 600000    | 16750.64    | 1.88 |
| pool     | hash      | 100000 | 600000    | 17039.22    | 1.88 |
| pool     | skiptable | 100000 | 600000    | 17490.27    | 1.88 |
| system   | array     | 100000 | 600000    | 17234.81    | 1.96 |
| system   | rbtree    | 100000 | 600000    | 17415.63    | 1.96 |
| system   | hash      | 100000 | 600000    | 17502.00    | 1.96 |
| system   | skiptable | 100000 | 600000    | 17021.75    | 1.96 |
| jemalloc | array     | 100000 | 600000    | 17750.15    | 1.95 |
| jemalloc | rbtree    | 100000 | 600000    | 17342.31    | 1.95 |
| jemalloc | hash      | 100000 | 600000    | 17684.56    | 1.95 |
| jemalloc | skiptable | 100000 | 600000    | 17790.69    | 1.95 |

---

## 7. 压测复现步骤

### 7.1 依赖准备

```bash
# 必需工具
which redis-benchmark
redis-benchmark --version
python3 --version
```

### 7.2 编译项目

```bash
git submodule init
git submodule update 
make 
```

### 7.3 启动待测服务

```bash
# KVStore
./kvstore --config config/master.conf

# Redis（示例）
redis-server --port 6379
```

### 7.4 一键启动

```bash
KV_HOST=127.0.0.1 KV_PORT=9300 \
REDIS_HOST=127.0.0.1 REDIS_PORT=6379 \
C=50 N=100000 POP_N=100000 P=1 KEYSPACE=100000 VALUE_SIZE=32 \
scripts/bench_all.sh
```

### 7.5 结果产物

- 原始明细：`results/raw.csv`
- 汇总报告：`results/summary.md`
- 执行日志：`logs/`

### 7.6 对齐说明

- `set/get/populate`：同工具（`redis-benchmark`）同参数口径对齐
- `range/sort`：KVStore 与 Redis 语义不完全一致，报告中标记为 **not aligned**，用于 KVStore 内部分析

### 7.7 自定义压测复现命令

以下命令用于复现本 README 中“自定义压测数据”部分。

1) **单机写入 QPS（四种结构）**

```bash
./tests/write_qps_test 127.0.0.1 9300 5 100000
```

2) **主从一致性功能验证（TCP/eBPF 均可复用）**

```bash
# mode 6~9: array / rbtree / hash / skiptable 主从一致性
./tests/testcase 127.0.0.1 9300 6 127.0.0.1 9301
./tests/testcase 127.0.0.1 9300 7 127.0.0.1 9301
./tests/testcase 127.0.0.1 9300 8 127.0.0.1 9301
./tests/testcase 127.0.0.1 9300 9 127.0.0.1 9301
```

3) **自定义混合写删负载（对应 6.5 第二张表）**

```bash
# mode=7: 2次写入 -> 1次删除 -> 间隔后再写入1次 -> 最后删除2次
./tests/write_qps_test 127.0.0.1 9300 7 100000
```

4) **eBPF 同步链路复现（先启动 master/slave，再启动 ebpf_syncd）**

```bash
./kvstore --config config/master.conf
./kvstore --config config/slave.conf
sudo ./src/eBPF/ebpf_syncd config/ebpf_sync.conf

# 在 master 侧打压
./tests/write_qps_test 127.0.0.1 9300 5 100000
```

5) **不同内存后端对比（pool/system/jemalloc）**

```bash
# 分别替换 mem_backend=pool|system|jemalloc 后重启服务，再执行同一压测命令
./kvstore --config config/master.conf
./tests/write_qps_test 127.0.0.1 9300 7 100000
```

---

## 8. 功能与命令覆盖

### 8.1 基础命令族

- array：`SET/GET/DEL/MOD/EXIST/RANGE/SORT`
- rbtree：`RSET/RGET/RDEL/RMOD/REXIST/RRANGE/RSORT`
- hash：`HSET/HGET/HDEL/HMOD/HEXIST/HRANGE/HSORT`
- skiptable：`SSET/SGET/SDEL/SMOD/SEXIST/SRANGE/SSORT`

### 8.2 其他命令

- 大 value：`PAPERSET/PAPERGET/PAPERMOD/PAPERDEL`
- 连接控制：`QUIT`

### 8.3 RANGE / SORT 参数支持

#### RANGE 家族

- `RANGE startKey endKey`（array）
- `HRANGE startKey endKey`（hash）
- `RRANGE startKey eney [LIMIT k]]`（rbtree）
- `SRANGE startKey endKey [LIMIT k]`（skiptable）

参数规则：
- `RANGE/HRANGE` 固定 3 个参数（含命令名），不支持 `LIMIT`。
- `RRANGE/SRANGE` 仅支持两种参数个数：`3` 或 `5`。
- 当使用 `LIMIT` 时，格式必须是 `... LIMIT k`，其中 `k` 必须为正整数；否则返回参数错误。
- `RRANGE/SRANGE` 不带 `LIMIT` 时返回整个命中区间；带 `LIMIT` 时返回前 `k` 条（按升序区间遍历顺序）。

#### SORT 家族

- `SORT [high|low] [limit]`
- `RSORT [high|low] [limit]`
- `HSORT [high|low] [limit]`
- `SSORT [high|low] [limit]`

参数规则：
- 默认排序方向：`low`（升序）。
- `high` 表示降序，`low` 表示升序。
- `limit` 为可选正整数；`<=0` 或无法解析时按“不限制条数”处理。
- 不带任何参数时等价于 `low` 且不限制条数。

#### 比较语义

- 四种结构的 `RANGE/SORT` 均基于 `strcmp` 的字典序比较 key。
- 若希望获得数值顺序，建议使用 zero-pad key（如 `key_000001`）。

---

## 9. 项目亮点

- 具备数据库内核端到端能力：协议、存储、持久化、复制、压测全链路
- 实现多后端内存抽象（pool/system/jemalloc），可用于性能与碎片对比
- 复制链路覆盖用户态 TCP 与内核态 eBPF 两种方案
- 具备可复现的 benchmark 基建，与自定义压测脚本，支持结果归档与版本化对比

---

## 10. 快速索引

- 默认配置：`config/kvstore.conf`
- 主从示例：`config/master.conf`、`config/slave.conf`
- eBPF 配置：`config/ebpf_sync.conf`
- 测试客户端：`tests/kvstore_client`
- 压测脚本：`scripts/bench_all.sh`

---
