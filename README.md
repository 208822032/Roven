# KVStore 

项目现在支持通过简单的 `key=value` 配置文件集中管理启动参数，并允许命令行覆盖关键项。
## KVStore

一个类 Redis 的轻量级 KV 引擎，支持多种数据结构、RESP 协议 & Pipeline、内存池、RDB/AOF 持久化，以及 TCP / eBPF 两种主从同步模式，并通过统一配置文件完成启动参数管理。

---

## 1. 功能概览

- **数据结构**：数组 / 红黑树 / 哈希表 / 跳表（全部支持**动态长度 value**）
  - 数组：`SET / GET / DEL / MOD / EXIST / RANGE / SORT`
  - 红黑树：`RSET / RGET / RDEL / RMOD / REXIST / RRANGE / RSORT`
  - 哈希表：`HSET / HGET / HDEL / HMOD / HEXIST / HRANGE / HSORT`
  - 跳表：`SSET / SGET / SDEL / SMOD / SEXIST / SRANGE / SSORT`
  - 以上四种结构的 value 均使用统一内存管理层，支持从小 key 到大 value 的**按需动态分配与释放**，不会被固定长度数组/结构体限制。
- **协议**：自研 RESP 解析器，兼容 Redis 风格，多命令 Pipeline
- **持久化**：RDB 快照 + AOF 追加日志，支持重写
- **分布式**：主从复制（TCP / eBPF 两种同步实现）
- **内存管理**：自定义内存池 + jemalloc 封装 + glibc内存管理
- **网络模型**：Reactor（epoll）/ Proactor（io_uring）/ NtyCo 协程

---

## 2. 代码结构

- `src/core/`：核心入口 & 全局配置
  - `kvstore.c`：主函数，加载配置、初始化引擎、启动网络循环
  - `kvs_config.c`：配置解析与命令行参数合并逻辑
  - `kvs_globals.c`：全局配置 `g_cfg`、全局数据结构实例
- `src/storage/`：四种数据结构 + 内存池
- `src/persistence/`：RDB、AOF 以及 AOF 重写
- `src/network/`：RESP 协议解析 + Reactor/Proactor/NtyCo 三种网络实现
- `src/distributed/`：主从同步、eBPF 相关代码
- `include/`：公共头文件（命令表、配置、类型定义等）
- `tests/`：功能测试 & QPS 压测客户端

---

## 3. 编译

项目已经提供 `Makefile`，推荐直接使用：

```bash
make            
```

生成的二进制文件：

- 服务端：`kvstore`
- 测试客户端：
  - `tests/testcase`：协议正确性、主从同步、大pipeline压测
  - `tests/write_qps_test`：单机写入QPS、主从写入QPS、自定义内存池写入用例
  - `tests/kvstore_client`：交互式、管道式客户端
  - `tests/test_paper`：单机写入、获取、修改、删除大value
  - `tests/test_paper_sync`：主从同步写入、获取、修改、删除大value

> 如果使用 eBPF 同步链路，工程会构建 eBPF 相关目标（内核态 `src/eBPF/ebpf_kern.o` 与用户态守护进程 `src/eBPF/ebpf_syncd`），并在编译时生成 `src/eBPF/vmlinux.h`。
> 这要求系统存在 `/sys/kernel/btf/vmlinux`，并安装 `bpftool`、`clang`、`libbpf` 等开发包；运行 `ebpf_syncd` 需要 root/CAP_BPF 权限。

---

## 4. 配置与启动

### 4.1 配置文件位置

- 默认配置：`config/kvstore.conf`
- 已提供示例：
  - `config/master.conf`：主节点
  - `config/slave.conf`：从节点

### 4.2 支持的主要配置项

见 `include/kvs_config.h`，典型字段：

- `port`：服务监听端口
- `role`：`master` / `slave`
- `master_ip`：从节点连接的主节点 IP
- `master_port`：主节点端口
- `sync_port`：主从同步端口
  - 若未配置，master 默认 `port + 1000`
  - slave 默认 `master_port + 1000`
- `enable_aof`：`yes/no/true/false/0/1`
- `aof_fsync_mode`：`no / everysec / always`：系统决定同步/每秒一次同步/每次写命令都同步
- `aof_filename`：AOF 文件名
- `rdb_filename`：RDB 文件名
- `mem_backend`：`pool / system / jemalloc`：自定义内存池/glibc/jemalloc
- `network_mode`：`reactor / proactor / ntyco`
- `sync_mode`：`tcp / ebpf`
  - `tcp`：使用内置 `distributed` 模块走独立同步端口（默认 `master_port + 1000`）
  - `ebpf`：由 `ebpf_syncd` 抓取并转发 RESP 同步流量（见第 7 章）
- `ebpf_conf`：eBPF 守护进程配置文件路径（默认 `config/ebpf_sync.conf`）
- `max_value_size`：value 最大长度（字节，`0` 表示不限制；当前更多用于预留/实验项）

### 4.3 Master 示例（`config/master.conf`）

```ini
port=8080
role=master

# 持久化
enable_aof=yes
aof_fsync_mode=everysec
aof_filename=kvstore-master.aof
rdb_filename=kvstore-master.rdb
dump_filename=kvstore-master.dat

# 网络模型
network_mode=ntyco
```

### 4.4 Slave 示例（`config/slave.conf`）

```ini
port=8081
role=slave

master_ip=127.0.0.1
master_port=8080

enable_aof=yes
aof_fsync_mode=everysec
aof_filename=kvstore-slave.aof
rdb_filename=kvstore-slave.rdb
dump_filename=kvstore-slave.dat

network_mode=ntyco
```

### 4.5 启动方式

**Master：**

```bash
./kvstore --config config/master.conf
```

**Slave：**

```bash
./kvstore --config config/slave.conf
```

命令行参数可以覆盖配置文件中的部分字段，例如：

```bash
./kvstore --config config/master.conf --aof_fsync always
./kvstore --config config/master.conf --aof_filename data/kvstore.aof
```

> Slave 节点会自动连接 master 的同步端口，并做全量 + 增量同步；在 Slave 上执行写命令会返回只读错误。

---

## 5. 协议与命令

### 5.1 基本命令

- 数组

  ```text
  SET key value
  GET key
  DEL key
  MOD key value
  EXIST key
  RANGE startKey endKey
  SORT [high|low] [limit]
  ```

- 红黑树

  ```text
  RSET key value
  RGET key
  RDEL key
  RMOD key value
  REXIST key
  RRANGE startKey endKey
  RSORT [high|low] [limit]
  ```

- 哈希表

  ```text
  HSET key value
  HGET key
  HDEL key
  HMOD key value
  HEXIST key
  HRANGE startKey endKey
  HSORT [high|low] [limit]
  ```

- 跳表

  ```text
  SSET key value
  SGET key
  SDEL key
  SMOD key value
  SEXIST key
  SRANGE startKey endKey
  SSORT [high|low] [limit]
  ```

- 大key大value

  ```text
  PAPERSET key value
  PAPERGET key
  PAPERDEL key
  PAPERMOD key value
  ```

- 其他

  ```text
  QUIT  # 关闭连接 / 触发服务退出流程
  ```

### 5.2 RESP 与 Pipeline

项目内部使用 RESP 编解码，与 Redis 类似，例如：

```text
*3\r
$3\r
SET\r
$3\r
k1\r
$3\r
v1\r

*2\r
$3\r
GET\r
$3\r
k1\r
```

支持在同一 TCP 连接上连续发送多条命令，服务端按顺序解析并返回结果，可用来做 pipeline QPS 压测（见 `tests/testcase.c` 中的 pipeline 测试）。

---

## 6. 持久化

### 6.1 RDB

- 代码：`src/persistence/kvs_rdb.c`
- 开关宏：`ENABLE_RDB`
- 主流程：启动时加载 RDB，退出时自动保存当前内存数据到 `g_cfg.rdb_filename`。

### 6.2 AOF

- 代码：`src/persistence/kvs_aof.c`, `kvs_aof_rewrite.c`
- 启动时若 `enable_aof=yes`：
  - 调用 `aof_init(g_cfg.aof_filename, g_cfg.aof_fsync_mode)`
  - 读取 AOF 回放数据
- 运行过程中所有写命令都会通过 `aof_append` 记录；
- 支持 AOF 重写（前台/后台），减小文件体积。

典型命令行：

```bash
./kvstore --config config/master.conf --aof_fsync everysec
./kvstore --config config/master.conf --aof_fsync always
```

---

## 7. 主从同步

### 7.1 模式选择

通过配置文件的 `sync_mode=tcp|ebpf` 选择：

- `sync_mode=tcp`：内置 TCP 主从同步（`src/distributed/`），同步端口默认 `master_port + 1000`。
- `sync_mode=ebpf`：eBPF 同步链路（`src/eBPF/ebpf_syncd` + `src/eBPF/ebpf_kern.c`），通过抓取发往 master 的 RESP 请求并转发到 slave 来实现增量同步；全量同步通过 `FULLDUMP` 完成。

核心文件：

- TCP 同步：`src/distributed/distributed.c`、`src/distributed/sync_backend_tcp.c`
- eBPF 同步：`src/eBPF/ebpf_kern.c`、`src/eBPF/ebpf_syncd.c`（批处理发送见 `src/utils/tx_batcher.c`）

### 7.2 行为说明

- TCP 同步（`sync_mode=tcp`）：
  - Master：监听 `sync_port` 接收 Slave 连接；写命令执行成功后通过 `master_sync_command` 广播增量；Slave 首次连接时会做全量同步。
  - Slave：连接 Master 的 `sync_port` 接收全量 + 增量；在本地重放写命令，并可记录 AOF 以便独立重启恢复。

- eBPF 同步（`sync_mode=ebpf`）：
  - 由 `ebpf_syncd` 负责：
    - 全量：连接 master 服务端口发送 `FULLDUMP`，把返回的数据流广播到各 slave。
    - 增量：加载 eBPF 程序（`kprobe/tcp_sendmsg`）抓取“发往 master_ip:master_port 的 TCP payload（RESP 请求字节流）”，通过 ringbuf 取出后广播到各 slave。
  - slave 侧通过 `REPLSYNC` 握手把某条连接标记为“复制连接”，该连接上的写命令会在复制上下文执行（跳过 slave 只读限制）。

eBPF 同步启动示例（需要 root/CAP_BPF 运行 `ebpf_syncd`）：

```bash
# 1) 启动 master/slave
./kvstore --config config/master.conf
./kvstore --config config/slave.conf

# 2) 启动 eBPF 守护进程（配置见 config/ebpf_sync.conf）
sudo ./src/eBPF/ebpf_syncd config/ebpf_sync.conf
```

**eBPF 链路的端口/流量方向）**

- `master_port`：kvstore master 的服务端口（接收客户端 RESP 命令）。
- `slave_port`：kvstore slave 的服务端口（`ebpf_syncd` 会主动连接它，并先发送 `REPLSYNC` 握手，把该 TCP 连接变成“复制连接”。）

链路分两段：

1）全量同步（FULLDUMP）：
- `ebpf_syncd` -> `master_ip:master_port` 发送 `FULLDUMP`（RESP：`*1\r\n$8\r\nFULLDUMP\r\n`）。
- master 把全量数据以字节流形式写回；`ebpf_syncd` recv 后原样广播给各 slave。
- `max_payload` 仅用于这一阶段的 `recv` 缓冲区大小。

2）增量同步（抓包转发）：
- eBPF 程序挂在 `kprobe/tcp_sendmsg`，过滤 **目的地址=配置中的 `master_ip:master_port`** 的发送调用。
- 这意味着：产生写流量的客户端进程需要运行在同一台加载 eBPF 程序的机器上（否则该机器上看不到“发往 master 的 sendmsg”）。
- `ebpf_syncd` 从 ringbuf 取出 payload 后，不做解析，直接原样广播到每个 slave 的“复制连接”。

**config/ebpf_sync.conf 字段说明**

该文件由 `ebpf_syncd` 读取。示例见 `config/ebpf_sync.conf`，字段含义如下：

- `master_ip` / `master_port`
  - 用途 1：eBPF 过滤条件（只抓取目的地址为该 master 的 `tcp_sendmsg`）。
  - 用途 2：FULLDUMP 阶段 `ebpf_syncd` 主动连接该地址。
- `slave_count`
  - 从节点数量（上限 10）。
- `slave_ip` / `slave_port`
  - 当 `slave_count=1` 时的兼容写法（等价于 `slave1_ip/slave1_port`）。
- `slaveN_ip` / `slaveN_port`
  - 多从节点写法：`slave1_ip/slave1_port`、`slave2_ip/slave2_port` ...
  - `ebpf_syncd` 会对每个 slave：`connect(slave_ip:slave_port)` -> 发送 `REPLSYNC`（RESP：`*1\r\n$8\r\nREPLSYNC\r\n`）-> 接收 `+OK` 后进入转发。
- `max_payload`
  - FULLDUMP 阶段的单次 `recv` 缓冲区大小（字节）。

**当前的实现约束**

- 仅抓取 `tcp_sendmsg` 中 `size <= 2048` 的发送（超出会直接跳过）。
- 仅处理 `iov_offset == 0` 且只读取第一个 `iovec` 的内容。

因此，eBPF 增量同步目前更适合用“小命令/小包”场景来验证（例如 README 的 mode6-9 一致性用例）。

`tests/testcase.c` 中提供了 master-slave 一致性验证用例：

```bash
./tests/testcase <master_ip> <master_port> 6 <slave_ip> <slave_port>   # 数组
./tests/testcase <master_ip> <master_port> 7 <slave_ip> <slave_port>   # 红黑树
./tests/testcase <master_ip> <master_port> 8 <slave_ip> <slave_port>   # 哈希表
./tests/testcase <master_ip> <master_port> 9 <slave_ip> <slave_port>   # 跳表
```

---

## 8. 自带测试与压测

### 8.1 功能测试

`tests/testcase` 支持多种模式：

```bash
./tests/testcase <ip> <port> <mode>

# 单节点功能
# 1 : 数组 RESP 功能
# 2 : 红黑树 RESP 功能
# 3 : 哈希 RESP 功能
# 4 : 跳表 RESP 功能
# 5 : 大包（huge packet）压力测试

# 主从一致性
# 6 : 数组 master-slave 一致性（需要额外传 slave_ip slave_port）
# 7 : 红黑树 master-slave 一致性
# 8 : 哈希 master-slave 一致性
# 9 : 跳表 master-slave 一致性
```

### 8.2 Pipeline QPS 测试

`tests/write_qps_test.c`、`tests/testcase.c` 中提供了针对四种数据结构的 pipeline QPS 测试脚本，用于对比：

- 无从节点时的最大吞吐
- 挂载从节点后主从同步带来的性能影响（TCP / eBPF 模式对比）

### 8.3 PAPER 测试

`tests/test_paper.c`、`tests/test_paper_sync.c` 中提供了针对单机、主从同步的测试脚本，用于验证：

- 单机对大key大value的增删改查
- 主从同步对大key大value的增删改查
---

# 9 `kvstore_client` 交互客户端

源码：`tests/kvstore_client.c`，编译时 `make` 会自动生成二进制：`tests/kvstore_client`。

### 启动方式

```bash
# 交互模式
./tests/kvstore_client <ip> <port>

# 单条命令模式
./tests/kvstore_client <ip> <port> -c "CMD ..."
```

### 内置命令与功能

- 基本 KV 操作：`SET / GET / DEL` 以及 `RSET/RGET/RDEL`、`HSET/HGET/HDEL`、`SSET/SGET/SDEL`
- Paper 存取：
  - `PAPERSET key /path/to/paper`：从本地文件读取内容，写入服务器（底层用 RESP bulk 传输，使用 RBTree 存储）。
  - `PAPERMOD key /path/to/paper`：从本地文件读取内容，修改已有论文内容（语义类似 `MOD`）。
  - `PAPERGET key /path/to/save`：从服务器拉取内容，如果存在则按 bulk 方式写入本地文件并打印长度；如果不存在则打印 `NO EXIST`。
  - `PAPERDEL key`：删除论文，成功返回 `OK`，不存在返回 `NO EXIST`。
- 连接/退出：
  - `EXIT`：退出客户端
  - `SQUIT`：向服务器发送 `QUIT` 命令（客户端仍保持运行，服务器收到后会进入优雅关闭流程）
- 帮助与补全：
  - `HELP`：列出所有支持的客户端命令
  - `HELP <command>`：查看某个命令的说明与用法
  - 交互模式下，支持基于 readline 的命令自动补全和历史记录

### 使用示例

```bash
# 1）连接到本地 8080 端口，进入交互模式
./tests/kvstore_client 127.0.0.1 8080

kv> SET foo bar
kv> GET foo
kv> PAPERSET paper1 /home/user/paper.pdf
kv> PAPERGET paper1 /tmp/paper-copy.pdf
kv> PAPERMOD paper1 /home/user/paper-v2.pdf
kv> PAPERDEL paper1
kv> EXIT

# 2）非交互调用一条命令
./tests/kvstore_client 127.0.0.1 8080 -c "GET foo"
```

借助该客户端，可以方便地做手工调试、大 value/论文文件读写验证以及简单脚本化测试。

---

# 10. 快速入门脚本示例

1. 编译：

   ```bash
   make
   ```

2. 启动 master：

   ```bash
   ./kvstore --config config/master.conf
   ```

3. 另开终端，运行功能测试（以数组为例）：

   ```bash
   ./tests/testcase 127.0.0.1 8080 1
   ```

4. 启动 slave 并做一致性测试（可选）：

   ```bash
   ./kvstore --config config/slave.conf
   ./tests/testcase 127.0.0.1 8080 6 127.0.0.1 8081
   ```

5. 启动 客户端进行交互：

   ```bash
   ./kvstore --config config/master.conf
   ./tests/kvstore_client <masterip> <masterport> 交互模式
   ./tests/kvstore_client <masterip> <masterport> -c "单条命令"
   ```

6. 以eBPF作为主从同步路径：

   ```bash
   ./kvstore --config config/master.conf
   ./kvstore --config config/slave.conf
   sudo ./src/eBPF/ebpf_syncd config/ebpf_sync.conf
   ./tests/kvstore_client <masterip> <masterport> 交互模式
   ./tests/kvstore_client <masterip> <masterport> -c "单条命令"
   ```

# 11. 测试附录

  1. 单机与主从同步 QPS 差异

  **单次写（100000 条），TCP，QPS 对比**
  
  注 所有测试均不开启aof

  | Mode         | Structure  | count    | QPS       |
  | ------------ | ---------  | -------- | --------- |
  | Standalone   | array      | 100000   | 17668.08  |
  | Standalone   | rbtree     | 100000   | 16593.61  |
  | Standalone   | hash       | 100000   | 16897.22  |
  | Standalone   | skiptable  | 100000   | 17324.24  |
  | Master-Slave | array      | 100000   | 17156.03  |
  | Master-Slave | rbtree     | 100000   | 16055.72  |
  | Master-Slave | hash       | 100000   | 16452.01  |
  | Master-Slave | skiptable  | 100000   | 16559.29  |

  **单次写（100000 条），eBPF，QPS 对比**

  | Mode         | Structure  | count    |    QPS    |
  | ------------ | ---------  | -------- | --------- |
  | Standalone   | array      | 100000   | 17668.08  |
  | Standalone   | rbtree     | 100000   | 16593.61  |
  | Standalone   | hash       | 100000   | 16897.22  |
  | Standalone   | skiptable  | 100000   | 17324.24  |
  | Master-Slave | array      | 100000   | 11761.18(无批处理)  |
  | Master-Slave | rbtree     | 100000   | 11496.70(无批处理)  |
  | Master-Slave | hash       | 100000   | 11455.85(无批处理)  |
  | Master-Slave | skiptable  | 100000   | 11270.69(无批处理)  |
  | Master-Slave | array      | 100000   | 15063.18(批处理)  |
  | Master-Slave | rbtree     | 100000   | 15041.32(批处理)  |
  | Master-Slave | hash       | 100000   | 15231.43(批处理)  |
  | Master-Slave | skiptable  | 100000   | 15179.61(批处理)  |

  **eBPF 批处理佐证数据（send_all 统计）**

  上表中 eBPF “无批处理/批处理”差异，除 QPS 结果外，还可以用同步通道的发送统计作为佐证：

  - 无批处理：每次发送通常对应一条小的同步指令，`avg` 大约是 30B 级别（频繁系统调用）。
  - 批处理：把多条同步指令聚合后再发送，`avg` 提升到 128KiB 量级（`131072`），系统调用次数显著下降。

  采样输出关键数据：

  | 场景 | send_all calls | send_all bytes | send_all avg (B) |
  | ---- | -------------: | -------------: | ---------------: |
  | 无批处理 | 8281 ~ 13553 | 256711 ~ 420143 | 31.0 ~ 31.8 |
  | 批处理 | 3 ~ 5 | 393216 ~ 655360 | 131072.0 |

  2. 不同内存池对比

  **内存池 QPS/内存占用测试**

  - `maxrss(KB)`：进程峰值常驻内存。
  - `qps-small`：执行 `ops` 次“分配 + 写入 + 释放”。
  - `read-heavy-small`：预分配 `pool_size` 个对象，循环 `rounds` 轮，每次都做读/轻写；按 `write_ratio`随机执行一次“释放并重新分配”。统计总操作数并计算 `ops/s`。
  - `big-after-frag`：在制造碎片场景后，探测一次大块分配的耗时与 RSS。
  - `big-after-frag time = 0 ms`：表示测得耗时小于 1ms。

  **基础功能（pool / system / jemalloc 对比）**

  | BACKEND | PATTERN           | maxrss (KB) | QPS / OPS                    |
  | ------- | ----------------- | ----------- | ---------------------------- |
  | POOL    | small-fixed       | 210576      | qps-small = 27027027.03      |
  | POOL    | mid-fixed         | 210576      | read-heavy-small = 24005840  |
  | POOL    | big-after-frag    | 1029412     | time = 0 ms                  |
  | SYSTEM  | small-fixed       | 10176       | qps-small = 26315789.47      |
  | SYSTEM  | mid-fixed         | 52416       | read-heavy-small = 46167076.92 |
  | SYSTEM  | big-after-frag    | 1068024     | time = 0 ms                  |
  | JEMALLOC| small-fixed       | 10188       | qps-small = 25641025.64      |
  | JEMALLOC| mid-fixed         | 52428       | read-heavy-small = 49996000  |
  | JEMALLOC| big-after-frag    | 1068036     | time = 0 ms                  |

  **写两次 -> 删一次 -> 隔一段时间 -> 再写一次 -> 最后删两次，不同内存后端 QPS 对比**
| Backend  | Structure | n       | total_ops | QPS (ops/s) | 虚拟内存 (GiB)  |
|----------|-----------|---------|-----------|-------------|----------------|
| pool     | array     | 100000  | 600000    | 18085.46    | 1.88           |
| pool     | rbtree    | 100000  | 600000    | 16750.64    | 1.88           |
| pool     | hash      | 100000  | 600000    | 17039.22    | 1.88           |
| pool     | skiptable | 100000  | 600000    | 17490.27    | 1.88           |
| system   | array     | 100000  | 600000    | 17234.81    | 1.96           |
| system   | rbtree    | 100000  | 600000    | 17415.63    | 1.96           |
| system   | hash      | 100000  | 600000    | 17502.00    | 1.96           |
| system   | skiptable | 100000  | 600000    | 17021.75    | 1.96           |
| jemalloc | array     | 100000  | 600000    | 17750.15    | 1.95           |
| jemalloc | rbtree    | 100000  | 600000    | 17342.31    | 1.95           |
| jemalloc | hash      | 100000  | 600000    | 17684.56    | 1.95           |
| jemalloc | skiptable | 100000  | 600000    | 17790.69    | 1.95           |

