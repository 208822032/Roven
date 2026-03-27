# 第 7D 步：实现 eBPF 路径的 TTL 主从同步

## 1. 本步目标

本步的目标是在不破坏 TCP 路径已通过验证的 TTL 语义前提下，让 eBPF 路径也具备最小正确的 TTL 主从同步能力，并尽量与 TCP 语义保持一致。

本步坚持三个原则：

1. TCP 路径的 TTL 语义不变。
2. eBPF 路径优先复用 TCP 已验证正确的 TTL 语义。
3. 只做最小改动，先保证语义统一，再谈实现细节差异。

本步需要覆盖的语义是：

1. EXPIRE key seconds
2. PERSIST key
3. DEL key
4. 覆盖写清 TTL
5. key 到期删除后的主从一致性

## 2. 修改前现状

### 2.1 TCP 路径已经有可用的 TTL 方案

TCP 路径已经完成 TTL 主从同步设计与实现，核心原则是：master 是唯一裁判者，slave 不主动过期，TTL 使用绝对时间 expire_at_ms，过期删除最终统一收敛为 DEL。

这套语义已经在 [docs/ttl_mvp/07a_tcp_replication_ttl_design.md](docs/ttl_mvp/07a_tcp_replication_ttl_design.md) 和 [docs/ttl_mvp/07b_tcp_replication_ttl_impl.md](docs/ttl_mvp/07b_tcp_replication_ttl_impl.md) 中明确，并且不应被 eBPF 方案打破。

### 2.2 eBPF 路径当前只是在搬运字节流

当前 eBPF 路径的增量链路是：内核侧从 master 的 tcp_recvmsg 抓取字节流，用户态 `ebpf_syncd` 将字节流原样发送到 slave。它不是一个“复制 master 已确认语义动作”的系统，而是一个“复制 master 收到的请求流”的系统。

这意味着，eBPF 路径本身并不理解 TTL 的最终语义。如果直接把 EXPIRE 原样转发，slave 会按照自己的接收时刻解释 TTL，和 TCP 路径的绝对时间语义不一致。

### 2.3 eBPF 全量同步目前会丢 TTL

当前 eBPF 的 FULLDUMP 只输出普通写命令，已过期 key 会被过滤，但带 TTL 的有效 key 仍然不会带上 expire_at_ms。这会导致新接入的 eBPF slave 只能恢复 value 语义，恢复不了完整 TTL 语义。

### 2.4 eBPF 和 TCP 的差异点不是传输层，而是语义层

从网络上看，eBPF 和 TCP 都是通过 TCP 发给 slave；真正的差异在于：

1. TCP 是 master 执行后的语义广播。
2. eBPF 是 master 接收请求流的字节中继。

因此 7D 的关键不是改网络协议，而是把 eBPF 路径的 TTL 语义对齐到 TCP 路径。

## 3. 修改文件列表

### 3.1 建议优先修改的文件

1. [src/eBPF/ebpf_syncd.c](src/eBPF/ebpf_syncd.c)
2. [src/core/kvs_operation.c](src/core/kvs_operation.c)
3. [src/core/kvstore.c](src/core/kvstore.c)
4. [src/network/ntyco.c](src/network/ntyco.c)
5. [src/network/reactor.c](src/network/reactor.c)
6. [src/network/proactor.c](src/network/proactor.c)
7. [src/eBPF/ebpf_kern.c](src/eBPF/ebpf_kern.c)
8. [Makefile](Makefile)
9. [tests/test_ttl_ebpf_replication.sh](tests/test_ttl_ebpf_replication.sh)

### 3.2 每个文件为什么要改

- [src/eBPF/ebpf_syncd.c](src/eBPF/ebpf_syncd.c)：eBPF 增量与全量同步的用户态中继都在这里，TTL 语义要对齐 TCP，最可能的落点就是这里把 FULLDUMP 和增量流做规范化处理。
- [src/core/kvs_operation.c](src/core/kvs_operation.c)：FULLDUMP 由这里生成，当前只输出 value 语义，TTL 要保留就必须在这里补出 `expire_at_ms` 或等价的 TTL 导出信息。
- [src/core/kvstore.c](src/core/kvstore.c)：这里是命令执行后的统一出口，如果 eBPF 路径后续要把 EXPIRE / PERSIST / DEL 归一化，命令层仍然是最合理的判断位置。
- [src/network/ntyco.c](src/network/ntyco.c)、[src/network/reactor.c](src/network/reactor.c)、[src/network/proactor.c](src/network/proactor.c)：这些文件负责 REPLSYNC 和复制连接上下文，eBPF slave 仍然要依赖它们去识别复制连接并执行回放。
- [src/eBPF/ebpf_kern.c](src/eBPF/ebpf_kern.c)：当前 kernel 程序只抓字节流，不理解语义；如果未来需要更精确区分复制流和普通流，这里可能需要配合增强过滤能力，但本步优先不动它。
- [Makefile](Makefile)：eBPF syncd 现在需要链接现有 RESP 解析/编码实现，避免重复造轮子。
- [tests/test_ttl_ebpf_replication.sh](tests/test_ttl_ebpf_replication.sh)：新增 eBPF 主从 TTL 真实链路测试，覆盖四个后端与 FULLDUMP/增量语义。

### 3.3 本步的最小改动边界

如果严格按“最小改动”执行，优先级应为：

1. 先补 FULLDUMP 的 TTL 导出。
2. 再让 eBPF 增量流能够识别并转发规范化 TTL 语义。
3. 最后再考虑内核侧是否需要更精细的过滤或标记。

## 4. TTL 在 eBPF 主从中的实现方式

### 4.1 核心结论

eBPF 路径的 TTL 主从语义应当与 TCP 路径保持一致，差别只放在“传输和抓流的实现方式”，不放在“TTL 的业务语义”上。

换句话说：eBPF 只是传输层不同，同步语义应一致。

### 4.2 推荐实现策略

推荐把 eBPF 路径拆成两类处理：

1. 普通写命令仍然沿用原始 RESP 字节流中继。
2. TTL 相关语义必须走规范化路径，统一转换成与 TCP 一致的绝对时间语义和删除语义。

这意味着 eBPF 不应该继续把 EXPIRE 当成一个“相对秒数命令”原样搬运，而应该把它收敛成：

- `PEXPIREAT key expire_at_ms`，或者
- `DEL key`（当 TTL 已经立即失效时）

### 4.3 TTL 表示方式

继续使用 `expire_at_ms` 作为统一 TTL 表示，不使用相对秒数作为复制层语义。

原因与 TCP 路径一致：

1. 避免网络延迟导致 TTL 漂移。
2. 避免 slave 因本地接收时间不同而出现不同过期点。
3. 让 RDB / AOF / TCP / eBPF 的 TTL 语义尽量统一。

### 4.4 slave 行为仍然保持被动

eBPF slave 不主动过期，不跑独立 TTL 扫描线程，不把本地时钟判断升级成写动作。

slave 只执行 master 发来的规范化 TTL 动作和 DEL，这样才能和 TCP 方案对齐。

## 5. 命令传播 / 状态同步改动

### 5.1 EXPIRE key seconds

推荐做法是：master 在执行 EXPIRE 后，先把 seconds 规范化成绝对过期时间 expire_at_ms，再对 eBPF 复制流传播 `PEXPIREAT key expire_at_ms`。

如果计算结果已经是立即失效，则直接传播 `DEL key`，不要传播一个马上过期的中间态。

### 5.2 PERSIST key

PERSIST 的语义是清除 TTL，应该作为独立动作传播。

slave 收到后把该 key 的 `expire_at_ms` 置为永不过期，不需要额外推导。

### 5.3 DEL key

DEL 直接作为删除动作传播即可。

无论是用户显式 DEL，还是 master 因 TTL 到期而删除，最终都应该落到同一条 DEL 传播路径上。

### 5.4 覆盖写清 TTL

SET / RSET / HSET / SSET / MOD / RMOD / HMOD / SMOD 这类覆盖写成功后，存储层本身已经会把 TTL 重置为永不过期。

因此 eBPF 路径不需要单独传播“清 TTL 消息”，只要继续传播原写命令即可。

### 5.5 全量同步中的 TTL

全量同步需要把有效 key 的 TTL 一并恢复。

建议做法是：

1. FULLDUMP 仍然输出 key/value。
2. 对于带 TTL 的 key，额外补出 `expire_at_ms`。
3. 已过期 key 直接跳过，不进入全量结果。

这和 TCP 路径对 TTL 的全量恢复语义保持一致。

## 6. 过期删除同步方案

### 6.1 统一删除裁判

过期删除必须由 master 负责裁判，slave 只跟随回放。

推荐链路是：

1. master 在读路径或后台回收中发现 key 已过期。
2. master 本地先删除。
3. master 再让 eBPF 路径传播 DEL。
4. slave 收到 DEL 后删除副本。

### 6.2 为什么不能让 slave 自己删

如果 slave 自己按本地时间删 key，会出现时钟偏差、延迟抖动和执行时序差异，最终导致 master/slave 的删除时刻不一致。

这会直接破坏 7B 已经建立好的最小正确语义，因此 eBPF 路径也应该和 TCP 一样，保持 slave 不主动过期。

### 6.3 立即失效场景

如果 EXPIRE / PEXPIREAT 计算后已经是过去时间，直接转成 DEL 更稳。

这样 slave 看到的是最终删除结果，而不是一个马上失效的中间状态。

## 7. slave 行为策略

eBPF slave 的行为策略与 TCP slave 一致：不主动过期，只回放 master 语义。

具体表现为：

1. 不启动 TTL 定时扫描线程。
2. 不根据本地时钟自行删除 key。
3. 只接受 master 下发的 PEXPIREAT / PERSIST / DEL / 覆盖写等动作。
4. 如果后续需要读路径兜底，也只做可见性过滤，不做独立删除。

这套策略的目标不是复杂，而是稳定：让 eBPF 和 TCP 在 TTL 上尽量同语义。

## 8. 风险点

### 8.1 FULLDUMP 语义风险

当前 FULLDUMP 只输出 value 语义，TTL 必须额外补出，否则 eBPF 新 slave 会丢失 expire_at_ms。

### 8.2 请求流与语义流不一致

eBPF 当前抓的是 master 收到的请求流，不是 master 处理后的最终语义流。如果不做规范化，EXPIRE 和 PERSIST 会被按错误时机解释。

### 8.3 全量与增量交错

FULLDUMP 和增量流之间可能出现交错，TTL 相关动作如果不明确顺序，可能会有短暂不一致。

### 8.4 兼容性风险

如果后续为了 TTL 规范化引入新的语义动作，slave 必须能识别这些动作，否则会出现旧节点和新节点行为不一致的问题。

### 8.5 过度扩展风险

如果为了 eBPF TTL 去重做一套和 TCP 完全不同的语义，反而会增加维护成本。本步应优先追求“语义统一，传输不同”。

## 9. 验证方式

### 9.1 编译与基础连通性

先确认 eBPF 路径仍能正常启动，REPLSYNC、FULLDUMP、增量复制都不被破坏。

### 9.2 TTL 复制验证

建议按以下序列验证：

1. master 执行 `SET a 1`。
2. master 执行 `EXPIRE a 3`。
3. 确认 slave 收到的是绝对过期时间，而不是 3 秒这个相对值。
4. 等待过期后，确认 master 删除时会同步 DEL，slave 最终删除。

### 9.3 PERSIST 验证

1. master 执行 `SET b 2`。
2. master 执行 `EXPIRE b 60`。
3. master 执行 `PERSIST b`。
4. 确认 slave 跟随清 TTL，TTL 变为永不过期。

### 9.4 覆盖写清 TTL 验证

1. master 执行 `SET c 3`。
2. master 执行 `EXPIRE c 60`。
3. master 再执行同类覆盖写命令。
4. 确认 slave 通过相同覆盖写把 TTL 清掉。

### 9.5 全量同步验证

1. master 准备一个永久 key 和一个带 TTL 的 key。
2. 新 slave 使用 eBPF 路径连接 master。
3. 确认全量同步后永久 key 仍存在，TTL key 仍保留绝对过期时间。
4. 确认已过期 key 不会进入全量结果。

### 9.6 对比 TCP 与 eBPF

同一批 TTL 操作在 TCP 和 eBPF 两条路径下都跑一遍，确认最终可见状态一致。这是最关键的语义回归验证。

## 10. 本步结论

eBPF 路径如果要支持 TTL 主从同步，最小正确方案不是重建一套新的 TTL 体系，而是把当前字节流中继提升为“TTL 语义对齐后的中继”。

核心结论是：

1. eBPF 只是传输层不同，同步语义应一致。
2. TTL 必须统一成绝对时间 expire_at_ms。
3. master 仍然是唯一裁判者。
4. slave 仍然只回放，不主动过期。
5. 过期删除最终统一收敛为 DEL。

这样才能在不破坏 TCP 路径的前提下，让 eBPF 路径也形成最小正确的 TTL 主从同步闭环。

## 11. 下一步建议

如果后续进入实现阶段，建议按以下顺序推进：

1. 先补 FULLDUMP 的 TTL 导出，避免新 slave 丢失 expire_at_ms。
2. 再把 eBPF 增量路径中的 EXPIRE / PERSIST / DEL 语义收敛到与 TCP 一致。
3. 最后再补全测试脚本，验证两条路径的 TTL 最终一致性。

这条顺序的好处是先保证全量正确，再保证增量一致，最后再做边界校验，风险最小。

## 12. 代码改动建议

本步先不改代码，但如果后续开始落地，建议按下面的最小 patch 思路推进：

### 12.1 [src/core/kvs_operation.c](src/core/kvs_operation.c)

1. 扩展 `handle_fulldump`，让有效 key 的全量导出同时携带 `expire_at_ms`。
2. 继续跳过已过期 key，避免把失效数据带入全量同步。
3. 保持现有命令表和普通写命令行为不变，不把 TCP 方案的 TTL 语义改坏。

### 12.2 [src/eBPF/ebpf_syncd.c](src/eBPF/ebpf_syncd.c)

1. 在 FULLDUMP 阶段接收带 TTL 的导出结果，并将其原样或按规范化格式发给 slave。
2. 对增量流增加 TTL 语义识别入口，优先把 EXPIRE 规范化成 `PEXPIREAT`，把立即失效场景规范化成 `DEL`。
3. 保持现有字节流中继能力不变，普通写命令仍然按原样转发。

### 12.3 [src/core/kvstore.c](src/core/kvstore.c)

1. 保持 TCP 分支的 TTL 逻辑不动。
2. eBPF 分支只负责让复制入口保持一致，不把 TTL 语义分散到多个地方。

### 12.4 [src/network/ntyco.c](src/network/ntyco.c)、[src/network/reactor.c](src/network/reactor.c)、[src/network/proactor.c](src/network/proactor.c)

1. 保持 REPLSYNC 的复制连接标记逻辑不变。
2. 让 slave 继续只执行复制上下文里的规范化命令，不额外引入 TTL 扫描线程。

### 12.5 [src/eBPF/ebpf_kern.c](src/eBPF/ebpf_kern.c)

1. 默认不改内核侧抓流逻辑，先把语义问题留在用户态解决。
2. 只有在需要更严格过滤复制流时，再考虑增加更精细的目标连接识别。

### 12.6 推荐的最小 patch 顺序

1. 先改 FULLDUMP 导出 TTL。
2. 再改 eBPF 用户态转发逻辑，让 TTL 动作和普通写动作分流。
3. 最后补回归测试，验证 TCP 与 eBPF 的 TTL 结果一致。