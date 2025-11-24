# KVStore （类 Redis KV 引擎）

项目现在支持通过简单的 `key=value` 配置文件集中管理启动参数，并允许命令行覆盖关键项。
## KVStore（类 Redis KV 引擎）

一个类 Redis 的轻量级 KV 引擎，支持多种数据结构、RESP 协议 & Pipeline、内存池、RDB/AOF 持久化，以及 TCP / eBPF 两种主从同步模式，并通过统一配置文件完成启动参数管理。

---

## 1. 功能概览

- **数据结构**：数组 / 红黑树 / 哈希表 / 跳表
  - 数组：`SET / GET / DEL / MOD / EXIST / RANGE / SORT`
  - 红黑树：`RSET / RGET / RDEL / RMOD / REXIST / RRANGE / RSORT`
  - 哈希表：`HSET / HGET / HDEL / HMOD / HEXIST / HRANGE / HSORT`
  - 跳表：`SSET / SGET / SDEL / SMOD / SEXIST / SRANGE / SSORT`
- **协议**：自研 RESP 解析器，兼容 Redis 风格，多命令 Pipeline
- **持久化**：RDB 快照 + AOF 追加日志，支持重写
- **分布式**：主从复制（TCP / eBPF 两种同步实现）
- **内存管理**：自定义内存池 + jemalloc 封装，可按配置选择后端
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
make            # 默认 SYNC_MODE=0，使用 TCP 同步
make SYNC_MODE=1  # 启用 eBPF 同步，需要 libbpf/clang 等环境和 root 运行权限
```

生成的二进制：

- 服务端：`kvstore`
- 测试客户端：
  - `tests/testcase`
  - `tests/write_qps_test`
  - `tests/kvstore_client`

> 如果使用 eBPF 模式，`Makefile` 会在 `src/distributed/` 下自动生成并编译 `ebpf_sync.bpf.o`，要求内核导出 `/sys/kernel/btf/vmlinux` 且已安装 `bpftool`、`clang`、`libbpf` 相关开发包。

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
- `aof_fsync_mode`：`no / everysec / always`
- `aof_filename`：AOF 文件名
- `rdb_filename`：RDB 文件名
- `dump_filename`：旧文本快照文件名（关闭 RDB 时使用）
- `mem_backend`：`pool / system / jemalloc`
- `network_mode`：`reactor / proactor / ntyco`（当前主要做记录，真正选择由编译阶段 `NETWORK_SELECT` 控制）

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

通过编译参数 `SYNC_MODE` 选择：

- `SYNC_MODE=0`：TCP 同步（默认）
- `SYNC_MODE=1`：eBPF 同步（涉及 BPF 程序加载，**需要在 root/有 CAP_BPF 能力的用户下运行 kvstore**）

核心文件：

- `src/distributed/distributed.c`
- `src/distributed/sync_backend_tcp.c`
- `src/distributed/sync_backend_ebpf.c`
- `src/distributed/ebpf_sync.c`, `ebpf_sync.bpf.c`

### 7.2 行为说明

- Master：
  - 监听 `g_cfg.sync_port`，接收 Slave 连接
  - 写命令执行成功后调用 `master_sync_command`，通过 sync backend 广播到所有 Slave
- Slave：
  - 启动后连接 Master，同步全量数据，然后持续接收增量指令
  - 在本地重放写命令，并记录 AOF，以便 Slave 独立重启恢复
- eBPF 模式：
  - Master 端通过回环口发送特定同步流量，由 eBPF 程序在内核态抓取并转发

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

---

# 9 `kvstore_client` 交互客户端

源码：`tests/kvstore_client.c`，编译时 `make` 会自动生成二进制：`tests/kvstore_client`。

### 启动方式

```bash
# 交互模式
./tests/kvstore_client <ip> <port>

# 单条命令模式
./tests/kvstore_client <ip> <port> -c "CMD ..."

# 管道模式（从 stdin 读取多行命令）
echo "SET k v" | ./tests/kvstore_client <ip> <port>
```

### 内置命令与功能

- 基本 KV 操作：`SET / GET / DEL` 以及 `RSET/RGET/RDEL`、`HSET/HGET/HDEL`、`SSET/SGET/SDEL`
- Paper 存取：
  - `PAPERSET key /path/to/paper`：从本地文件读取内容，写入服务器（底层用 RESP bulk 传输）
  - `PAPERGET key /path/to/save`：从服务器拉取内容，直接写入本地文件，只在终端打印长度
- 大 Value 存储：
  - `SAVEHUGE key /path/to/file`：把大文件作为二进制 value 存到 `SET key` 中
- 连接/退出：
  - `EXIT`：退出客户端
  - `SQUIT`：向服务器发送 `QUIT` 命令（不会退出客户端本身）
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
kv> SAVEHUGE big /tmp/big.bin
kv> PAPERSET paper1 /home/user/paper.pdf
kv> PAPERGET paper1 /tmp/paper-copy.pdf
kv> EXIT

# 2）非交互调用一条命令
./tests/kvstore_client 127.0.0.1 8080 -c "GET foo"

# 3）脚本或管道模式
printf "SET a 1\nGET a\n" | ./tests/kvstore_client 127.0.0.1 8080
```

借助该客户端，可以方便地做手工调试、大 value/论文文件读写验证以及简单脚本化测试。

---

## 10. 快速入门脚本示例

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
