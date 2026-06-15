# 第七章 TTL 子系统：为什么 TTL 不是给 key 加一个 expire 字段这么简单

## 7.1 从普通 KV 到缓存语义：为什么需要 TTL

在前面几章里，KVLite 的核心能力一直围绕普通 KV 展开。

客户端发送 SET，服务端把 key-value 写入内存；客户端发送 GET，服务端从内存中查找并返回结果。

在这个阶段，key 一旦写入，除非显式 DEL，否则会一直存在。

这对早期原型来说是足够的。因为最开始我要验证的是：

```text
网络层能不能收到请求
协议层能不能解析命令
命令层能不能分发
存储层能不能保存数据
响应能不能返回客户端
```

但如果把 KVLite 放到缓存场景里，只支持"永久 key"就不够了。

真实缓存系统里，很多数据本来就应该有生命周期。

比如：

```text
登录 session
验证码
限流计数器
临时 token
AI Chat 的短期上下文缓存
RAG 场景里的临时语义缓存
```

这些数据不应该永远留在内存里。它们应该在一段时间后自动过期。

所以 TTL 是 KVLite 从"普通内存 KV"走向"缓存系统"的一个关键能力。

最开始我对 TTL 的理解其实很简单：给每个 key 加一个过期时间，GET 的时候判断一下是否过期。

但真正接入后，我才发现 TTL 并不是一个局部功能。

它会影响：

```text
写路径
读路径
RANGE / SORT / SCAN
后台清理
AOF
RDB
主从复制
eBPF 同步
测试和压测
```

也就是说，TTL 的入口可能是一条 EXPIRE 命令，但它的影响范围是全链路的。

这也是这一章要复盘的重点：**TTL 不是给 key 加一个 expire 字段这么简单，而是一次跨命令层、存储层、持久化层和复制层的系统改造。**

## 7.2 TTL 最朴素的设计：给 entry 增加 expire_at_ms

TTL 的第一个版本，最核心的设计确实是给每个 key-value entry 增加过期时间字段。

可以抽象成：

```c
typedef struct kv_entry {
    char *key;
    char *value;
    int64_t expire_at_ms;
} kv_entry_t;
```

其中 `expire_at_ms` 表示这个 key 的绝对过期时间。

一般会约定一个特殊值表示永不过期，比如：

```text
expire_at_ms < 0  表示永不过期
expire_at_ms >= 0 表示在这个毫秒时间戳过期
```

当用户执行：

```text
EXPIRE name 10
```

系统会把它转成：

```text
expire_at_ms = now_ms + 10 * 1000
```

之后 GET 的时候，只需要比较：

```text
now_ms >= expire_at_ms
```

如果条件成立，说明 key 已经过期。

这个方案很朴素，但它是合理的起点。

因为它先解决了 TTL 的最小闭环：

```text
SET key value
→ EXPIRE key seconds
→ GET key 时检查 expire_at_ms
→ 如果过期，删除并返回不存在
```

这样就能证明 TTL 的基础语义是通的。

但是，这个最小闭环只覆盖了一个路径：**GET 某个 key 时的惰性过期。**

继续往后做，就会发现问题远不止 GET。

比如：

```text
RANGE 会不会返回过期 key？
SORT 会不会把过期 key 排进去？
AOF rewrite 会不会把过期 key 写回文件？
RDB 快照会不会丢失过期时间？
主从复制时 EXPIRE key 10 应该怎么同步？
后台没人访问的过期 key 什么时候清理？
```

这些问题说明：`expire_at_ms` 字段只是 TTL 的起点，不是 TTL 的全部。

## 7.3 为什么要用绝对过期时间，而不是只保存相对 TTL

TTL 命令输入时，用户通常给的是相对时间。

比如：

```text
EXPIRE key 10
```

意思是 10 秒后过期。

但在存储层，KVLite 更适合保存绝对过期时间：

```text
expire_at_ms = 当前毫秒时间 + 10 秒
```

也就是：

```text
PEXPIREAT key 1748520000000
```

这种设计有几个好处。

**第一，GET 判断简单。**

只需要比较：

```text
now_ms >= expire_at_ms
```

不需要额外保存"这个 key 是什么时候设置的"、"相对 TTL 还剩多少"。

**第二，RDB 快照更自然。**

RDB 保存的是某个时刻的内存状态。如果保存相对 TTL，比如"还剩 10 秒"，那么快照文件过一段时间后再加载，这个"还剩 10 秒"就不准确了。

而保存绝对过期时间后，加载时只要拿当前时间比较：

```text
如果 expire_at_ms 已经过了，就跳过
如果还没过，就恢复
```

**第三，AOF rewrite 更稳定。**

AOF rewrite 不是简单保留原始命令，而是根据当前内存状态重新生成一份更紧凑的日志。

如果一个 key 还有 TTL，rewrite 时更适合写成：

```text
SET key value
PEXPIREAT key expire_at_ms
```

这样新 AOF 不依赖"rewrite 发生时还剩多少秒"，而是直接保存最终过期时间。

**第四，主从复制语义更一致。**

如果 master 执行：

```text
EXPIRE key 10
```

然后把这条相对 TTL 命令同步给 slave，slave 收到时可能已经过去了几十毫秒甚至几百毫秒。

这会导致 slave 上的过期时间比 master 晚。

所以更好的做法是 master 把它转换成绝对时间：

```text
PEXPIREAT key expire_at_ms
```

再同步给 slave。

这样 master 和 slave 使用同一个过期时间点。

当然，绝对时间也有自己的边界。

它依赖系统时钟。如果系统时间发生回拨或跳变，过期判断会受到影响。

更严格的系统可能会区分 wall clock 和 monotonic clock。但在 KVLite 当前场景里，使用毫秒级 wall time 是一个实现简单、语义清晰的选择。

## 7.4 读路径惰性删除：GET 为什么要检查过期

TTL 最直接的接入点是 GET。

因为用户读取一个 key 时，系统必须判断：

```text
这个 key 是否还有效？
```

如果 key 已经过期，就应该表现得像不存在一样。

所以 GET 路径大致需要做：

```text
查找 key
→ 判断 expire_at_ms
→ 如果未过期，返回 value
→ 如果已过期，删除 key，并返回不存在
```

这个策略叫惰性删除。

它的好处是简单，只有访问到 key 的时候才检查，不会额外消耗后台资源。

但这里有一个容易忽略的问题：**GET 通常是读操作，但删除过期 key 是写操作。**

如果 KVLite 对 GET 使用读锁，那么在读锁下发现 key 已过期，不能直接删除。因为删除会修改存储结构。

这时就需要一种机制：

```text
第一次 GET 持读锁
→ 发现 key 过期
→ 返回一个特殊状态
→ 上层释放读锁，重新加写锁
→ 再执行一次过期删除
```

这就是 TTL 和第五章里 `lock_mode`、命令层调度之间的联系。

GET 本来是读命令，但在遇到过期 key 时，它可能升级为写路径。

这也是 TTL 的复杂性之一：它会改变某些命令的锁语义。

惰性删除的缺点也很明显。

如果一个 key 过期后再也没有人访问它，那么 GET 永远不会触发删除。

这个 key 会一直占着内存。

所以只靠惰性删除不够，后续必须补后台扫描。

## 7.5 RANGE / SORT / SCAN 为什么也要过滤过期 key

刚接入 TTL 时，很容易只想到 GET。

但如果只在 GET 路径检查过期，就会出现语义不一致。

比如：

```text
EXPIRE session:1 1
等待 1 秒
GET session:1
```

GET 返回不存在。

但如果这时执行：

```text
RANGE session:0 session:z
```

如果 RANGE 没有过滤过期 key，它可能仍然返回 `session:1`。

这就会出现：

```text
GET 看不到
RANGE 却能看到
```

这种结果对用户来说是不一致的。

所以 TTL 必须影响所有读出 key/value 的路径。

包括：

```text
GET
EXIST
RANGE
SORT
SCAN
导出
持久化遍历
```

普通 GET 只处理一个 key，而 RANGE/SORT/SCAN 可能一次读出很多 key。它们在遍历时也必须检查每个 entry 的 `expire_at_ms`。

如果某个 key 已经过期，要么跳过，要么顺手删除。

这在实现上会影响四种数据结构：

```text
array 遍历数组时过滤过期 entry
hash 遍历 bucket 时过滤过期 entry
rbtree 中序遍历时过滤过期 node
skiptable 范围扫描时过滤过期 node
```

这说明 TTL 不是命令层的局部逻辑，而是存储读路径的全局语义。

任何会把 key 暴露给用户的路径，都不能返回过期 key。

## 7.6 只靠惰性删除不够：为什么需要后台扫描

惰性删除只能清理被访问到的过期 key。

但缓存系统里经常会出现这种情况：

```text
一批 session key 被写入
设置 30 分钟过期
30 分钟后用户都离线了
这些 key 再也没人访问
```

如果没有后台清理，它们会一直留在内存里。

所以 TTL 还需要主动删除机制。

KVLite 的做法是增加后台扫描线程，定期遍历存储结构，清理已经过期的 key。

后台扫描需要考虑几个问题。

**第一，扫描频率不能太高。** 如果频繁遍历所有数据，会影响正常请求。

**第二，每次扫描不能太多。** 如果一次扫描大量 key，可能长时间持有锁，影响 SET/GET 延迟。

**第三，四种数据结构都要支持扫描。** array、hash、rbtree、skiptable 的遍历方式不同，所以后台扫描必须分别适配。

**第四，要考虑主从角色。** 如果 slave 自己主动扫描删除过期 key，可能和 master 的同步语义不一致。因此更稳的策略是让 master 驱动 TTL 删除和同步，slave 尽量不独立做破坏数据一致性的清理。

后台扫描的价值在于清理冷过期 key，避免内存长期被无效数据占用。

但它也有不足。

比如固定频率、固定扫描数量的策略比较简单。如果 key 数量很大，扫描可能跟不上；如果大量 key 同时过期，可能造成瞬时抖动。

更复杂的系统可以引入随机采样、时间预算、自适应扫描频率等策略。

KVLite 当前的后台扫描更偏轻量级实现，适合当前项目规模，但还不是工业级过期回收策略。

## 7.7 TTL 与 AOF：为什么 rewrite 不能丢过期语义

TTL 接入持久化后，问题会变得更复杂。

AOF 记录的是写命令。

如果用户执行：

```text
SET name zhengpan
EXPIRE name 10
```

AOF 不能只记录 SET。

否则重放后，`name` 会变成永不过期。

所以 EXPIRE、PEXPIRE、PEXPIREAT、PERSIST 这些 TTL 相关命令也必须进入 AOF。

但这里还有一个更细的问题：**AOF 中不应该简单保存相对 TTL。**

如果 AOF 里记录的是：

```text
EXPIRE name 10
```

然后一小时后重启重放 AOF，这条命令会让 `name` 从当前时间开始再活 10 秒。

这显然不符合原来的语义。

所以更合适的方式是把 EXPIRE 转换为绝对时间命令：

```text
PEXPIREAT name expire_at_ms
```

这样无论什么时候重放，过期时间都是原来的那个时间点。

AOF rewrite 也必须处理 TTL。

rewrite 的目标是根据当前内存状态生成新的 AOF 文件。

如果某个 key 已经过期，就不应该写入新 AOF。

如果某个 key 还没过期，就需要写入：

```text
SET key value
PEXPIREAT key expire_at_ms
```

这样新 AOF 才能完整保留 key 的值和过期语义。

所以 TTL 对 AOF 的影响不只是"多记录一条 EXPIRE"。

它要求 AOF append、AOF rewrite、AOF load 都理解过期语义。

## 7.8 TTL 与 RDB：快照里必须保存 expire_at_ms

RDB 是内存数据在某个时刻的快照。

如果 RDB 只保存 key/value，不保存过期时间，那么加载后所有 key 都会变成永不过期。

这会直接破坏 TTL 语义。

所以 RDB 记录里必须包含 `expire_at_ms`。

保存时，每条记录不仅要写：

```text
key
value
```

还要写：

```text
expire_at_ms
```

加载时，也不能无脑恢复所有 key。

应该判断：

```text
如果 expire_at_ms 已经过期
→ 跳过，不恢复

如果未过期
→ 恢复 key/value，同时恢复 expire_at_ms
```

这说明一个重要原则：

> 持久化保存的不只是数据本身，也要保存影响数据语义的元数据。

TTL 的过期时间就是这样的元数据。

如果丢掉它，数据虽然还在，但语义已经错了。

## 7.9 TTL 与主从复制：为什么要同步绝对过期语义

主从复制也必须考虑 TTL。

假设 master 执行：

```text
EXPIRE token 10
```

如果 master 原样把这条命令同步给 slave，slave 收到时可能已经过去了 1 秒。

那么 master 上 token 的过期时间是：

```text
master 执行时刻 + 10 秒
```

slave 上 token 的过期时间会变成：

```text
slave 收到时刻 + 10 秒
```

两边会不一致。

所以 master 更适合把相对 TTL 转成绝对过期时间：

```text
PEXPIREAT token expire_at_ms
```

再同步给 slave。

这样 master 和 slave 共享同一个过期时间点。

如果 EXPIRE 设置的是 0 或负数，语义上等价于立即删除，那么同步给 slave 的也可以直接是 DEL，而不是继续传 EXPIRE。

另外，slave 是否应该自己后台扫描过期 key，也需要谨慎。

如果 slave 自己扫描，可能因为时钟差异或扫描时机不同，提前删除 master 还没同步删除的 key。

所以更简单稳定的方式是：TTL 主控权在 master，slave 通过同步命令跟随 master。

这也说明，TTL 在复制场景中不仅是数据问题，也是语义一致性问题。

## 7.10 TTL 的测试与压测：怎么证明它真的全链路生效

TTL 的测试不能只测：

```text
EXPIRE key 1
sleep 1
GET key
```

这个只能证明最简单的 GET 惰性删除。

要证明 TTL 真正全链路生效，需要覆盖更多路径。

至少应该包括：

```text
基础命令测试：
EXPIRE / TTL / PERSIST / PEXPIREAT

读路径测试：
GET 过期后返回不存在
EXIST 不返回过期 key

范围测试：
RANGE / SORT / SCAN 不返回过期 key

后台扫描测试：
过期后不访问 key，后台线程能清理内存

AOF 测试：
EXPIRE 是否转换为 PEXPIREAT
AOF rewrite 后 TTL 是否保留
AOF 加载时过期 key 是否跳过

RDB 测试：
RDB 是否保存 expire_at_ms
加载时已过期 key 是否跳过

主从复制测试：
master 的 TTL 是否同步到 slave
EXPIRE 是否转换为绝对时间
slave 上 key 是否按相同语义过期

压测：
大量 key 设置 TTL
TTL 与普通 SET/GET 混合
大量 key 同时过期
后台扫描是否影响 QPS
```

这也是 TTL 复盘中很重要的一点。

一个功能不能只证明"单条命令能跑"。

它必须证明在所有相关路径中语义一致。

否则就会出现：

```text
GET 看不到过期 key
RANGE 还能看到

重启前 key 会过期
重启后 key 永不过期

master 上过期了
slave 上还没过期
```

这种问题在面试里非常容易被追问。

## 7.11 当前 TTL 设计解决了什么，又留下了什么问题

当前 TTL 设计解决了几个核心问题。

**第一，key 有了生命周期。** 普通 SET 写入的 key 可以通过 EXPIRE、PEXPIRE、PEXPIREAT 设置过期时间，也可以通过 PERSIST 移除过期时间。

**第二，读路径支持惰性删除。** GET 访问 key 时，会判断是否过期。过期则删除，并返回不存在。

**第三，批量读路径尽量保持一致。** RANGE、SORT、SCAN 这类读出多个 key 的路径，也需要过滤过期 key，避免出现 GET 和 RANGE 语义不一致。

**第四，后台扫描清理冷过期 key。** 即使过期 key 不再被访问，也能由后台线程逐步清理，避免内存长期占用。

**第五，AOF/RDB 保存过期语义。** AOF 通过 PEXPIREAT 保存绝对过期时间，RDB 记录 `expire_at_ms`，保证重启恢复后 TTL 不丢失。

**第六，主从复制同步绝对过期时间。** master 不直接同步相对 TTL，而是同步绝对过期时间，减少 master 和 slave 之间因为延迟导致的过期时间偏差。

但当前设计也有一些边界。

**第一，后台扫描策略还比较简单。** 固定周期、固定扫描预算，适合当前项目规模，但面对更大数据量时可能需要自适应策略。

**第二，大量 key 同时过期可能造成抖动。** 如果大量 key 在同一时间过期，惰性删除和后台清理可能在短时间内触发大量删除操作。

**第三，系统时钟变化会影响 TTL。** 因为使用的是绝对 wall clock 时间，如果系统时间回拨，key 的过期判断可能延后。

**第四，TTL 相关副作用仍然有一些特殊逻辑。** 比如 AOF 和复制路径中，EXPIRE 转 PEXPIREAT 这类逻辑可能仍然需要特殊判断。更理想的方式是通过 command flags 或统一的 TTL command metadata 表达，而不是散落在字符串判断中。

**第五，四种数据结构的 TTL 行为需要持续保持一致。** 只要有一条路径漏掉过期过滤，就可能出现语义不一致。

所以 TTL 当前已经从功能上打通了主链路，但如果要继续生产化，还可以在扫描策略、时钟处理、过期抖动、命令元信息设计和一致性测试上继续优化。

## 7.12 面试时我怎么讲这个阶段

### 30 秒版

TTL 最开始看起来像是给 key 加一个 `expire_at_ms` 字段，但真正落地后会发现它是全链路功能。

因为 GET 要做惰性删除，RANGE/SORT/SCAN 也不能返回过期 key；只靠访问时删除不够，所以还要有后台扫描清理冷 key；AOF rewrite 和 RDB snapshot 必须保存过期时间，否则重启后 TTL 会丢；主从复制也不能直接同步 `EXPIRE key 10`，而要转成 `PEXPIREAT key expire_at_ms`，避免 slave 收到命令时延迟导致过期时间不一致。

所以 TTL 不是一个字段，而是跨命令层、存储层、持久化层和复制层的语义一致性问题。

### 2 分钟版

KVLite 最早是普通内存 KV，SET 进去的 key 除非 DEL，否则会一直存在。后来为了支持缓存场景，比如 session、验证码、AI Chat 短期上下文缓存，我加入了 TTL。

最朴素的设计是在每个 entry 里加 `expire_at_ms` 字段，用负数表示永不过期，用绝对毫秒时间戳表示过期时间。EXPIRE 命令会把相对秒数转换成绝对过期时间。

最直接的接入点是 GET。GET 查到 key 后会判断是否过期，过期则删除并返回不存在。这里有一个细节：GET 通常是读命令，但删除过期 key 是写操作，所以如果读锁下发现过期，需要返回特殊状态，让上层升级到写锁后重试删除。

但只做 GET 不够。RANGE、SORT、SCAN 这些批量读路径也必须过滤过期 key，否则会出现 GET 看不到但 RANGE 能看到的语义不一致。只靠惰性删除也不够，因为冷 key 过期后可能永远不再被访问，所以还要后台扫描线程定期清理。

然后是持久化和复制。AOF 不能只记录 SET，否则 TTL 会丢；EXPIRE 最好转换成 PEXPIREAT 这种绝对时间命令写入 AOF。AOF rewrite 和 RDB snapshot 也必须保存 `expire_at_ms`，并跳过已经过期的 key。主从复制里也不能直接同步相对 TTL，因为 slave 收到命令时已经过了一段时间，所以 master 要同步绝对过期时间，保证主从语义一致。

所以 TTL 这件事让我意识到，存储系统里的功能不是局部改一处代码就完了。一个看似简单的过期时间，会影响读路径、扫描、持久化、复制和测试验证。

当前方案已经覆盖了主链路，但仍有不足，比如后台扫描策略比较简单，大量 key 同时过期可能有抖动，系统时钟回拨会影响绝对过期时间，TTL 命令在 AOF/复制路径里的特殊处理还可以通过更系统的 command flags 优化。

## 7.13 本章小结

这一章复盘了 KVLite 的 TTL 子系统。

TTL 的最小实现，是给 entry 增加 `expire_at_ms`，再在 GET 路径做惰性删除。

但完整 TTL 远不止这些。

它会影响：

```text
GET 惰性删除
RANGE / SORT / SCAN 过滤
后台扫描
AOF append / rewrite
RDB save / load
主从复制
eBPF 同步
测试和压测
```

`expire_at_ms` 使用绝对过期时间，是为了让持久化和复制语义更稳定。

AOF rewrite 可以写入 PEXPIREAT，RDB 可以直接保存 `expire_at_ms`，主从同步也可以用同一个绝对过期时间点。

TTL 让 KVLite 从普通内存 KV 往缓存系统迈了一步。

它也让我意识到：存储系统里的一个小功能，往往会变成一次全链路改造。

下一章将复盘 AOF 与 RDB 持久化：为什么内存 KV 需要重启恢复，以及 AOF 和 RDB 如何分别承担日志和快照的角色。

## 7.14 本章对应的面试追问清单

1. 为什么 KV 系统需要 TTL？
2. TTL 最简单怎么实现？
3. 为什么 TTL 不是给 key 加字段就完了？
4. `expire_at_ms` 为什么适合用绝对时间？
5. 为什么不用只保存相对 TTL？
6. 系统时间回拨会影响 TTL 吗？
7. GET 惰性删除怎么做？
8. GET 发现 key 过期时为什么可能需要写锁？
9. 只靠惰性删除有什么问题？
10. 后台扫描线程怎么设计？
11. 后台扫描如何避免影响正常请求？
12. RANGE/SORT/SCAN 为什么要过滤过期 key？
13. AOF 为什么必须记录 TTL？
14. AOF rewrite 为什么推荐写 PEXPIREAT？
15. RDB 为什么要保存 `expire_at_ms`？
16. RDB 加载时发现 key 已经过期怎么办？
17. 主从复制为什么不能直接同步 `EXPIRE key 10`？
18. 为什么同步绝对过期时间更好？
19. slave 是否应该主动扫描过期 key？
20. 大量 key 同时过期会有什么问题？
21. TTL 怎么测试才算全链路正确？
22. 如果重新设计 TTL，你会怎么优化？

---

*第七章完。下一章主题：AOF 与 RDB 持久化——内存 KV 的重启恢复与数据安全。*
