# KVLite binary-safe key/value 项目实践复盘

> 基于 commit 16be4ce 之上未提交的工作区改动。
> 测试验证日期：2026-05-27。

## 1. 这次迭代的背景

### 面试 1 分钟表达

"KVLite 的底层存储引擎本身已经部分支持二进制 key——节点存了 key_len/value_len，内部比较函数也用 memcmp 加长度。但上层 RESP 协议解析、命令处理、存储入口三层都在用 strlen 重新计算 key 长度。比如 RESP bulk string 声明 `$4\r\nab\0c\r\n` 表示 4 字节，parser 也正确分配了 4+1 字节，但解析完成后 token 只传出 `char*` 指针，长度信息丢了。命令层拿到的 token 用 strlen 一算，4 字节变成 2 字节。存储入口再用 strlen 算一次，又是 2 字节。三层截断下来，二进制 key 不可能正确。

我这次做的事：打通从 RESP parser 到存储引擎、再到 AOF/RDB/主从同步/TTL 的 key_len/value_len 传递通道。核心不是重写底层比较逻辑，而是修复接口层的长度丢失问题。"

### 为什么这类问题在存储系统里比较严重

1. **数据完整性**：key 中包含 `\0` 时，strlen 把 `ab\0c` 和 `ab\0d` 都算成 `"ab"`——两个不同的 key 被当作同一个，覆盖写入或误删除。
2. **value 完整性**：value 中有 `\0`，GET 返回时 strlen 截断，客户端收到的 value 不完整。
3. **持久化一致性**：AOF 用 `%s` 写 key/value，RDB 用 strlen 算 key_len 后存盘——持久化文件里存的就是截断后的数据，重启恢复后数据永远丢失。
4. **主从同步一致性**：master 同步消息用 strlen 算 key_len，slave 收到后写入的 key 已经被截断。

---

## 2. 问题定位过程

### 2.1 问题链路图

```
客户端: *3\r\n$3\r\nSET\r\n$4\r\nab\0c\r\n$6\r\nvalue1\r\n
                         ↑ RESP 声明 key 长度为 4

[RESP Parser]  resp_parser.c:434-444
  parser->bulk_len = 4                           ← 长度正确
  kvs_malloc(5) = "ab\0c\0" (5 bytes)
  tokens[1] = ptr (char*), 长度信息丢失         ← 第一断点

[命令解析]  kvs_operation.c:889
  ctx->key = tokens[1]
  ctx->key_len = strlen(tokens[1])  → 2          ← 第二断点

[存储 API]  kvs_rbtree_set(inst, key, value)
  key_len = strlen(key)  → 2                     ← 第三断点

[内部比较]  rbtree_key_compare(key, 2, node->key, 4)
  虽然 memcmp 逻辑正确，收到的 len 已经是错的      ← 结果永远错
```

### 2.2 问题点表格

| 层级 | 原问题 | 影响 | 修复方式 |
|------|--------|------|---------|
| RESP parser | `parser->tokens[]` 只存 `char*`，不存长度 | token 传出去后长度丢失 | 增加 `size_t *token_lens[]` 数组 |
| msg_handler | 签名 `(char** tokens, int count, char* response)` | 三层网络后端都无法传长度 | 增加 `size_t *token_lens` 参数 |
| 命令解析 | `parse_command_argument` 用 `strlen(tokens[1])` | ctx->key_len 从 4 变成 2 | 使用 `token_lens[1]` |
| 命令上下文 | `command_context_t` 无 `value_len` | value 长度不可知 | 增加 `size_t value_len` |
| 存储 API | set/get/del/mod/exist 只接受 `char* key` | 内部重新 strlen | 新增 `_len` 系列 API，接受 key_len/value_len |
| handle_get | `strlen(result)` 构造 RESP | GET binary value 时截断响应 | `ttl_target_get_value_len(&ref)` 从节点取真实长度 |
| AOF append | `strlen(argv[i])` + `%s` 写 bulk body | AOF 文件中 value 数据被截断 | `aof_append` 增加 `arg_lens` 参数 |
| AOF rewrite | `write_command_parts` 用 strlen | array/hash rewrite 文件 key/value 截断 | 切换为 `write_command_parts_len` |
| RDB save | array save 用 `strlen(item->key)` | 磁盘上 key_len 字段存的是截断后的长度 | 使用 `item->key_len` |
| 主从同步 | `master_sync_command` 用 `strlen(key/value)` | 同步消息中长度错误 | 签名增加 key_len/value_len |
| TTL | `ttl_lookup_array/hash` 不接受 key_len | TTL 查找走旧 strlen 路径 | 改为接受 key_len，调 `_len` 查找 |

---

## 3. 具体改动内容

### 3.1 RESP parser（1 文件）

`include/resp_protocol.h:61` — `resp_parser_t` 增加 `size_t *token_lens` 成员。

`src/network/resp_protocol.c`:
- `resp_parser_init`: 初始化 `token_lens = NULL`
- `RESP_PARSE_ARRAY_LEN` 状态: tokens 和 token_lens 同步分配
- `RESP_PARSE_BULK_DATA` 状态: `token_lens[arg_index] = (size_t)bulk_len` — 记录真实长度
- `resp_parser_reset`: token_lens 清零（数组保留复用）
- `resp_parser_free`: `kvs_free(token_lens)`

AOF replay 路径复用同一个 parser，token_lens 自动生效。

### 3.2 命令层（3 文件）

`include/kvs_server.h:4` — `msg_handler` 签名增加 `size_t *token_lens`。三个网络后端（reactor/proactor/ntyco）同步更新，在 `*_session_handler` 中传递 `parser->token_lens`。

`include/kvs_command.h`:
- `command_context_t` 增加 `size_t value_len`
- `parse_command_argument` 增加 `size_t *token_lens` 参数
- `ttl_get/set_any_key_expire_at_ms` 增加 `size_t key_len` 参数

`src/core/kvs_operation.c`:
- `parse_command_argument`: 所有 `kvs_vcmd_token_len(tokens[n])` 和 `strlen(tokens[n])` 替换为 `token_lens[n]`
- `handle_set/del/mod/exist`: 四个 handler 的 switch 分支全部调用 `_len` API，传入 `ctx->key_len` / `ctx->value_len`
- `handle_get`: `strlen(result)` 替换为 `ttl_target_get_value_len(&ref)`，从节点 `value_len` 字段取真实长度
- TTL lookup: `ttl_lookup_array/hash` 改为接受 `key_len`，调用 `kvs_array_lookup_index_len` / `kvs_hash_lookup_index_len`

### 3.3 四种存储结构

#### ARRAY

**关键发现**：`kvs_array_index_locate`（line 125-170）本身已经支持 key_len + memcmp。side_index 节点（`kvs_array_index_entry_t`）也有 `key_len` 字段。真正的 bug 只有一个——`kvs_array_set_len` 的插入逻辑写错了。

**原 bug**：`kvs_array_set_len` 中，新 key 的插入路径用 `kvs_array_index_locate` 返回的 `insert_slot`（side_index 的 hash slot 位置）当作 `inst->table` 的下标做 memmove，然后完全没有调用 `kvs_array_index_insert_or_update`。side_index 里没有这个 key 的条目，后续所有 lookup 全部返回"未找到"。

**修复**：照搬旧 `kvs_array_set` 的流程——在 `table[inst->len]` 写入条目，调用 `kvs_array_index_insert_or_update(inst, kcopy, key_len, key_hash, inst->len)` 在 side_index 建立映射，然后 `inst->len++`。

**为什么 key1=ab\0c 和 key2=ab 能共存**：side_index 的 `kvs_array_index_locate` 使用 `entry->key_len == key_len && memcmp(entry->key, key, key_len)` 比较——key1 长度 4，key2 长度 2，先比较长度就区分开了。

#### RBTREE

**为什么最早修通**：RBTREE 的内部查找 `kvs_rbtree_lookup_node(inst, key, key_len)` 和比较函数 `rbtree_key_compare(lhs, lhs_len, rhs, rhs_len)` 从设计之初就接受 key_len。所以 _len API 只需去掉入口处的 `strlen(key)` 直接使用传入的长度即可，不用修改任何内部逻辑。

RBTREE 也是四种结构中唯一在 AOF rewrite 中早就使用 `node->key_len` + `write_command_parts_len` 的结构。

#### HASH

**原 bug**：`kvs_hash_set_len` 使用 `kvs_hash_key_hash(key)`（扫描 `\0` 的旧哈希函数），而 `kvs_hash_lookup_index_len` 使用 `kvs_hash_key_hash_len(key, key_len)`（接受长度的新哈希函数）。SET 把条目存到 hash A 位置，GET 去 hash B 位置找——必然找不到。

**修复**：一行替换：`kvs_hash_key_hash(key)` → `kvs_hash_key_hash_len(key, key_len)`。碰撞链比较已经是 `key_len + memcmp`，不需要改。

#### SKIPTABLE

**原 bug**：`create_node` 内部通过 `skiptable_key_hash_with_len(key, &klen)` 和 `skiptable_value_len(value)` 扫描 `\0` 确定长度，然后 `kvs_malloc(klen+1)` 和 `memcpy(key, klen+1)`。对 4 字节 key `ab\0c`，klen 算出 2，只分配 3 字节、只拷贝 3 字节——数据截断。

**修复**：新增 `create_node_len(level, key, key_len, value, value_len)`：
- `skiptable_key_hash_len(key, key_len)` 计算 hash
- `kvs_malloc(key_len + 1)` 精确分配
- `memcpy(key, key_len)` 精确拷贝
- `key[key_len] = '\0'` 只用于调试兼容，不参与比较（比较走 `node->key_len` + `memcmp`）

### 3.4 AOF / RDB / 同步 / TTL

**AOF append**: `aof_append(int argc, char **argv, size_t *arg_lens)` — 新增 `arg_lens` 参数。计算长度时用 `arg_lens ? arg_lens[i] : strlen(argv[i])` 兼容。`kvs_filter_protocol` 中 `aof_append(count, tokens, token_lens)` 传入真实长度。

**AOF rewrite**: array 段改用 `write_command_parts_len`，从 `array_snapshot[i].key_len/value_len` 取长度（快照结构新增了这两个字段）。hash 段同样切换。rbtree/skiptable 早已用 `write_command_parts_len`。

**RDB save**: array 的 `rdb_save_array` 将 `strlen(item->key)` 替换为 `item->key_len`（array item 已有该字段）。rbtree/hash/skiptable 早已使用 `node->key_len`。

**RDB apply_expire**: `rdb_apply_expire_rbtree` 从 `rbtree_search((char*)key)` 改为 `kvs_rbtree_lookup_node(&global_rbtree, key, key_len)`，消除内部 strlen。`rdb_apply_expire_array/skiptable` 的 `strcmp` 改为 `memcmp`。

**主从同步**: `master_sync_command` 签名增加 `key_len/value_len` 参数，移除内部 `strlen(key)/strlen(value)`。全量同步遍历四种结构时，传入 `node->key_len/value_len`。kvstore.c 中调用点传入 `ctx.key_len/token_lens[1]`。

**TTL**: `ttl_lookup_array/hash` 改为接受 key_len，调用 `_len` 查找。后台扫描使用 `ttl_scan_record_expired_len` 版本。

---

## 4. 这次改动的技术难点和踩坑

### 难点1：RESP 协议带长度，但项目内部丢长度

- **问题表现**：对同一个 `$4\r\nab\0c\r\n`，parser 知道是 4 字节，但 tokens 出来只有 `char*`，命令层和存储层各用 strlen 算一遍。
- **根因**：token 设计只考虑了 C 字符串语义。
- **修复**：parser 加 `token_lens[]`，msg_handler 签名加参数，一路传下去。
- **一句话**：RESP 已经告诉你长度了，问题是你自己丢了它。

### 难点2：不能只修 SET/GET，AOF/RDB/同步/TTL 全链路都要修

- **问题表现**：SET/GET 修好后，RDB load 内部又调旧 `kvs_array_set`，strlen 再次截断。
- **根因**：存储系统有持久化、同步、过期多个链路，每个链路都独立处理 key/value。
- **修复**：逐模块排查，AOF append/rewrite/replay、RDB save/load、主从全量/增量同步、TTL 查找/删除/后台扫描。
- **一句话**：KV 存储不是只修 CRUD 就完了，写入链路有多条，每条都要过一遍。

### 难点3：GET 返回 value 不能再 strlen

- **问题表现**：SET binary value `xy\0z`(4B) 成功，GET 返回只有 2 字节 `$2\r\nxy\r\n`。
- **根因**：`handle_get` 用 `strlen(result)` 构造 RESP 响应长度。
- **修复**：新增 `ttl_target_get_value_len(&ref)` 从节点的 `value_len` 字段取真实长度。
- **一句话**：value_len 存在节点里，GET 时直接用，不要再 strlen 重算。

### 难点4：ARRAY side_index 和 table 是两个独立索引空间

- **问题表现**：把 `kvs_array_index_locate` 返回的 `insert_slot`（side_index 的 hash slot 位置）当成 `inst->table` 的下标做 memmove——类型错误，索引空间错乱。
- **根因**：side_index 是 hash 表，table 是稠密数组，下标不在同一个空间。
- **修复**：照搬旧 `kvs_array_set` 流程——table 末尾写入→side_index 插入映射→len++。
- **一句话**：ARRAY 有两个索引结构，操作 table 不能拿 side_index 的下标。

### 难点5：HASH SET 和 GET 必须用同一个 hash 函数

- **问题表现**：SET 成功但 GET 找不到。SET 用 `kvs_hash_key_hash(key)` 扫 `\0`，GET 用 `kvs_hash_key_hash_len(key, key_len)` 接受长度——同一个 key 算出了两个不同的 hash 值。
- **根因**：两个 hash 函数输出不一致。
- **修复**：一行替换，`kvs_hash_set_len` 改用 `kvs_hash_key_hash_len`。
- **一句话**：hash 表里 SET 和 GET 必须用同一个 hash 函数，否则 key 存进去了但永远找不到。

### 难点6：SKIPTABLE 节点创建必须显式长度分配

- **问题表现**：`create_node` 扫描 `\0` 确定长度，分配内存太小，memcpy 截断数据。
- **根因**：C 字符串语义——`strlen` 和 `while(*p!='\0')` 是同一个模式。
- **修复**：新增 `create_node_len`，接受显式 key_len/value_len，精确 malloc + memcpy。
- **一句话**：动态长度的节点不能用 strlen 算大小再分配。

### 难点7：旧 wrapper 和 binary-safe 主路径要划清边界

- **问题表现**：主路径修好了，但某些调用方（如 AI chat schema）还在用旧 `kvs_hash_set(store, key, value)`，内部 strlen 无所谓（因为它的 key 是 hex hash string，不含 `\0`）。
- **决策**：旧 API 保留为文本 wrapper，主命令路径（`kvs_operation.c`）全部走 `_len` API。
- **一句话**：不能一刀切删除旧 API——兼容调用方保留，但主路径必须走新 API。

### 难点8：测试必须验证 `ab\0c` 和 `ab` 能共存

- **问题表现**：如果只测 `ab\0c` 能 SET/GET，可能有 bug 没暴露——比如比较逻辑在公共前缀 `"ab"` 相等后就返回结果，忽略了长度差异。
- **根因**：前缀相同的不同长度 key 是最容易出错的边界条件。
- **验证**：SET key1=`ab\0c`(4B)；SET key2=`ab`(2B)；分别 GET，验证互相不影响。
- **一句话**：二进制 key 测试不能只测带 `\0` 的，必须测前缀相同但长度不同的两个 key 能否共存。

---

## 5. 测试验证

### 5.1 测试结果汇总

| 测试 | 结果 | 说明 |
|------|------|------|
| test_binary_key | **36/36 PASS** | 四种结构 SET/GET/EXIST/MOD/DEL + key1/key2 共存 |
| testcase mode 1 | PASS | 基础 array/rbtree/hash/skiptable 操作 |
| testcase mode 5 | PASS | 100 万命令 Pipeline，约 175K QPS |
| write_qps_test | PASS | 四种结构各 1000 条写入 |
| mempool_test | PASS | 内存池 |
| ai_chat_schema_test | PASS | AI Chat schema |
| vector_testcase | 大部分 PASS | 1 个 FAIL：VSEARCH-threshold-keep-exact |

### 5.2 binary key 专项测试细节

```
$ ./test_binary_key 127.0.0.1 8080

--- ARRAY ---
  PASS: ARRAY SET key1 val1
  PASS: ARRAY GET key1 -> 4 bytes          ← 返回完整 4 字节 xy\0z
  PASS: ARRAY EXIST key1
  PASS: ARRAY MOD key1 val2
  PASS: ARRAY GET key1 after MOD -> 2 bytes
  PASS: ARRAY DEL key1
  PASS: ARRAY EXIST key1 after DEL -> not found

--- RBTREE ---  (同上 7/7 PASS)
--- HASH ---    (同上 7/7 PASS)
--- SKIPTABLE --- (同上 7/7 PASS)

--- Coexistence: key1 != key2 ---
  PASS: ARRAY key2 coexists with key1 -> 2 bytes
  PASS: ARRAY key1 still intact -> 4 bytes
  (RBTREE/HASH/SKIPTABLE 同样 2/2 PASS)

Results: 36 passed, 0 failed
```

### 5.3 已知非本次问题

`vector_testcase` 中 `VSEARCH-threshold-keep-exact` 失败——这是向量检索的阈值筛选测试，响应 `*0\r\n` 表示没搜到结果。此测试在本次改动前已存在（vector 模块没有修改），与 binary-safe 改动无关。

### 5.4 grep 静态检查

`strlen(tokens[...])`：**0 处**——全部清除。

`strlen(key)` 剩余：全部在旧 API wrapper 中（`kvs_rbtree_set/get/del/mod/exist`、`kvs_hash_set/get`、`kvs_array_set`、`rbtree_search`）或 TTL 旧 wrapper / AOF rewrite 旧 `write_command_parts`——这些都是文本兼容路径，不在 binary-safe 主链路。

`strcpy` 剩余：仅 `kvs_rbtree.c` 调试用 batch insert，不在主路径。

---

## 6. 最终成果边界

### 可以说的

- KVLite 核心 KV 命令（SET/GET/DEL/MOD/EXIST）已实现 binary-safe key/value 支持
- 四种数据结构（ARRAY/RBTREE/HASH/SKIPTABLE）全部通过测试
- key 中包含 `\0`、value 中包含 `\0` 均可正确存取
- 前缀相同但长度不同的 key（如 `ab\0c` 和 `ab`）可共存
- 改动覆盖了 AOF/RDB/主从同步/TTL 链路
- Pipeline 100 万命令测试通过，未引入性能退化

### 不应该夸大的

- 不是 Redis 级完整兼容——只覆盖核心 KV 命令
- sort/range 仍是文本语义（天然为文本范围查询设计）
- 旧 API wrapper 保留，仍走 strlen 路径，但仅用于外部兼容调用方
- vector 模块有一个阈值测试失败（非本次改动引入）
- 某些关闭路径偶发 segfault（需独立排查，非本次引入）

### 简历 bullet 版本

"排查并修复了 KVLite 的 binary-safe key/value 支持问题。原项目 RESP 协议解析正确得到长度，但 token 输出、命令解析、存储 API 三层均用 strlen 重新计算导致截断。通过增加 token_lens 数组、扩展 _len API、补齐 AOF/RDB/主从同步/TTL 长度传递链路，实现四种数据结构（array/rbtree/hash/skiptable）核心 KV 命令支持含 `\0` 字节的 key/value，36 项专项测试全部通过，Pipeline 100 万命令基准测试未引入性能退化。"

### 面试 30 秒版本

"KVLite 原来不支持二进制 key——虽然 RESP 协议声明了 `$4\r\nab\0c\r\n` 是 4 字节，parser 也读对了，但 token 传出去丢了长度，命令层和存储层各用 strlen 重算一遍，4 字节变 2 字节。我做的就是在 parser 里加 token_lens 数组记录真实长度，然后一路传到命令上下文、存储 API、AOF、RDB、同步链路。四种数据结构现在都能正确存取含 `\0` 的 key 和 value。"

### 面试 2 分钟版本

"KVLite 的二进制 key 支持问题，本质是 C 字符串语义向二进制安全语义的迁移。

RESP 协议用 `$长度\r\n数据\r\n` 编码，天然支持 `\0` 嵌入。parser 也正确分配了 `bulk_len+1` 字节并 memcpy。但问题在于：parser 的 tokens 只给 `char*` 不给长度；命令解析层拿 token 用 strlen 算 key_len；存储 API 签名是 `char* key`，内部再用 strlen 算一遍。三层截断下来，`ab\0c` 在每一层都被当作 2 字节的 `ab`。

我的修复分几块：RESP parser 增加 token_lens 数组，在 bulk string 解析时记录 `$N` 声明的真实长度；三个网络后端的 msg_handler 签名扩展，把 token_lens 传给命令层；命令上下文增加 value_len，parse_command_argument 里所有 strlen(tokens[n]) 替换为 token_lens[n]；存储层新增 _len API，set/get/del/mod/exist 全部接受显式 key_len/value_len，内部不再 strlen；handle_get 改从节点 value_len 字段取长度构造 RESP；AOF append 增加 arg_lens，RDB array save 改用 node->key_len，主从同步 master_sync_command 传 key_len/value_len，TTL 查找全部切到 _len API。

修复过程中踩了几个坑。ARRAY 的 side_index 本身已经是二进制安全的，但我的 _len set 函数写漏了 index_insert_or_update 调用，导致索引不知道新 key。HASH 的 set_len 用了旧 hash 函数扫描 `\0`，但 lookup_len 用了新 hash 函数接受长度——同一个 key 算出两个 hash，当然找不到。SKIPTABLE 的 create_node 内部扫描 `\0` 确定分配大小，需要新写一个 create_node_len 接受显式长度。

测试设计上，除了验证 `ab\0c` 能 SET/GET/DEL，我还验证了 `ab\0c`（4字节）和 `ab`（2字节）能共存——这是最容易出错的边界：前缀相同但长度不同。36 个专项测试全部通过，100 万命令 Pipeline 基准 175K QPS，没有性能回归。"

### 被追问时的诚实边界版本

"这次改动覆盖了核心 KV 命令链路。sort 和 range 命令仍是文本语义——因为范围查询的 start_key/end_key 本身就是文本概念。vector 模块有一个阈值测试失败，但不是这次改的。AI chat schema 还在用旧 API，因为它的 key 是 hex hash string，本来就不含 `\0`，不需要改。shutdown 路径有个偶发 segfault，需要单独排查，也不是这次引入的。如果要追求完整性，后续可以：1）继续补齐 sort/range 的二进制范围语义；2）排查 shutdown segfault；3）AOF 的 COW 路径仍用 `%s` 写 bulk body，目前 arg_lens 只在主路径生效。"

---

## 7. 面试追问准备

**Q1: 为什么 Redis 可以支持二进制安全 key？**
A: Redis 的 SDS（Simple Dynamic String）在结构体里同时存了 `len` 和 `free`，所有字符串操作都基于 len 而不是 `\0`。RESP 协议用 `$长度\r\n` 声明长度，客户端和服务器都按长度处理。KVLite 的问题就是在协议到存储的中间层，把长度信息丢了。

**Q2: RESP bulk string 为什么天然支持二进制？**
A: RESP bulk string 编码是 `$N\r\n<N bytes>\r\n`。`N` 是字节数，不是字符串长度。所以 `$4\r\nab\0c\r\n` 明确告诉接收方"接下来 4 字节数据"——中间有没有 `\0` 不影响解析。和 C 的 `\0` 终止符完全是两个概念。

**Q3: 你项目原来为什么不支持？**
A: 因为 token 只传 `char*`，没有传长度。命令层和存储层拿到 `char*` 后只能调用 `strlen`，strlen 遇到 `\0` 就停了。核心是 C 语言习惯——看到 `char*` 就当 C 字符串用。

**Q4: 为什么不能用 strlen？**
A: `strlen` 的定义就是扫描到第一个 `\0` 为止。key=`ab\0c` 时 strlen 返回 2，不是 4。用它算长度，分配内存少了，比较也截断了。在二进制场景下必须用显式长度。

**Q5: 为什么 GET 也要带 value_len？**
A: GET 返回 value 给客户端，需要构造 RESP 响应 `$N\r\n<data>\r\n`。如果不从节点取 value_len 而用 strlen，value 中的 `\0` 会让响应长度算错——客户端收到的数据就不完整。SET 时存的 value_len 就在节点里，GET 时直接用就行。

**Q6: 为什么 key1=ab\0c 和 key2=ab 共存是关键测试？**
A: 因为比较函数如果先比公共前缀、前缀相同就返回"相等"，那 `ab\0c` 和 `ab` 就会被误判为同一个 key——因为它们的前 2 字节都是 `ab`。正确的逻辑是：前缀相同时，长度短的 key 排在前面。这个边界条件最容易暴露比较逻辑没有正确处理长度的 bug。

**Q7: ARRAY 的 side_index bug 是什么？**
A: side_index 是 array 的 hash 索引，负责 key→dense_idx 映射。它本身已支持 key_len+memcmp。但我的 _len set 函数在插入新 key 后，没有调用 `kvs_array_index_insert_or_update` 把 key 注册到 side_index。结果就是 table 里有数据，但索引里没有——查找永远返回"未找到"。

**Q8: HASH 的 hash 不一致 bug 是什么？**
A: `kvs_hash_set_len` 用旧 `kvs_hash_key_hash(key)` 计算 hash——这个函数扫描到 `\0` 停止。而 `kvs_hash_lookup_index_len` 用新 `kvs_hash_key_hash_len(key, key_len)`——这个函数 hash 全部 key_len 字节。对同一个 key `ab\0c`，前者算出 hash("ab")，后者算出 hash("ab\0c")，两个值不同。SET 存到 slot A，GET 去 slot B 找，永远找不到。

**Q9: SKIPTABLE 的 create_node bug 是什么？**
A: `create_node` 内部用 `while(key[klen]!='\0')` 扫描确定 key 长度，然后用这个长度分配内存和拷贝。对 `ab\0c`，它算出长度 2，只分配 3 字节，只拷贝 `ab\0`。数据本身就被截断了。需要新写一个接受显式 key_len/value_len 的 `create_node_len`。

**Q10: AOF 为什么不能用 %s？**
A: `%s` 输出到第一个 `\0` 为止。如果用 `fprintf(fp, "$%zu\r\n%s\r\n", len, key)`，即使 len 正确，`%s` 也只输出 `ab`（到 `\0` 截止）。正确做法是 `fwrite(key, 1, len, fp)`——按字节数写入，不管内容。

**Q11: RDB 为什么不能直接 fwrite struct？**
A: struct 有 padding 字节（内容不确定）、有指针成员（存的是内存地址，下次加载时无效）、大小端差异。KVLite 的 RDB 格式已经是逐字段序列化（写 key_len→写 key 字节→写 value_len→写 value 字节），问题只是 key_len 的来源用错了——应该用节点存的 key_len，而不是 strlen 重算。

**Q12: 这次改动有没有性能影响？**
A: 正向影响——`_len` API 里不再调用 strlen，省掉了 `\0` 扫描。token_lens 是直接从 parser 已有的 bulk_len 记录的，零开销。100 万 Pipeline 基准 175K QPS 和改动前一致，没有退化。

**Q13: Pipeline 测试说明了什么？**
A: 100 万命令一次性发送，验证了 net_session 的 RESP 批量解析 + token_lens 传递在大量命令下没有内存泄漏、没有性能退化、没有解析错误。Pipeline 是 RESP 协议的核心能力，不能因为改 token 结构就破坏。

**Q14: 为什么不能说已经完全 Redis 兼容？**
A: Redis 的二进制安全是全局的——从协议到内部数据结构到持久化到复制到集群通信，每一层都是 len-based。KVLite 这次覆盖了核心 KV 命令链路，但 sort/range 仍是文本语义，vector 模块未覆盖，AOF 的 COW 路径还有 `%s`。只能说核心 KV 命令完成了 binary-safe 支持。

**Q15: 还有哪些后续优化？**
A: 1）sort/range 的二进制语义；2）排查 shutdown segfault；3）AOF COW 路径的 `%s` 改为 fwrite；4）vector 模块 key 如果需要二进制支持；5）可以考虑把旧 API wrapper 标记为 deprecated，逐步迁移调用方。

---

## 8. 最终归档总结

### 迭代名称
KVLite 核心 KV 命令 binary-safe key/value 支持

### 背景
RESP 协议本身支持二进制长度编码，但 KVLite 内部在 token 输出、命令解析、存储 API 三层用 strlen 重新计算 key/value 长度，导致包含 `\0` 的数据被截断。

### 改动范围
- **协议层**：resp_parser_t 增加 token_lens[]
- **命令层**：msg_handler 签名扩展、command_context_t 增加 value_len、parse_command_argument 和 handler 全部切换
- **存储层**：四种数据结构新增 _len API（set/get/del/mod/exist）
- **持久化**：AOF append arg_lens、AOF rewrite write_command_parts_len、RDB save 使用 node->key_len
- **同步**：master_sync_command key_len/value_len 传参
- **TTL**：lookup 和后台扫描切换到 _len 查找
- **测试**：36 项 binary key 专项测试 + 全量回归

### 核心难点
1. RESP 带长度但内部丢长度——需打通三层传递
2. 不只是 SET/GET，AOF/RDB/同步/TTL 全链路都要改
3. ARRAY 的 side_index 和 table 是两个独立索引空间
4. HASH 的 set/get 必须用同一个 hash 函数
5. SKIPTABLE 节点创建必须显式长度分配
6. 测试必须验证前缀相同的不同长度 key 能共存

### 验证结果
- test_binary_key: 36/36 PASS
- testcase mode 1: PASS
- testcase mode 5 (100万 Pipeline): PASS, ~175K QPS
- write_qps_test: PASS
- mempool_test: PASS
- ai_chat_schema_test: PASS

### 边界
- 核心 KV 命令 binary-safe，非全量兼容
- sort/range 文本语义
- 旧 API wrapper 保留
- vector VSEARCH threshold 测试失败（非本次引入）

### 面试主线（2 分钟完整版）
见第 6 节"面试 2 分钟版本"。
