# KVLite 最小必要升级：AI-chat 语义检索底座

## 1. 当前 KVLite 现状扫描

KVLite 当前已经具备把 AI-chat 业务的最小语义闭环“装下去”的能力，关键点不是重做数据库，而是把 key/value 合同收敛成稳定命名空间。

已确认的实现能力：

- 四个后端都已经支持字符串 key/value、长度元数据和 TTL 挂载。
- `hash` 适合做 exact lookup，`skiptable` 适合做有序 scan/range。
- `range / sort / fulldump` 已经可以直接复用，不需要重写查询引擎。
- AOF / RDB / TCP replication 都是按普通写命令和绝对 TTL 语义工作的。
- 当前实现仍然是字符串存储模型，payload 不是任意二进制安全 blob，因此 embedding 必须是文本编码或者外部引用。

参考实现：

- [include/kvs_hash.h](../include/kvs_hash.h)
- [include/kvs_skiptable.h](../include/kvs_skiptable.h)
- [include/kvs_types.h](../include/kvs_types.h)
- [src/core/kvs_operation.c](../src/core/kvs_operation.c)
- [src/persistence/kvs_rdb.c](../src/persistence/kvs_rdb.c)
- [src/persistence/kvs_aof.c](../src/persistence/kvs_aof.c)
- [src/distributed/distributed.c](../src/distributed/distributed.c)

## 2. 适合 AI-chat 语义检索闭环的最小能力清单

最小能力只保留这几类：

- QARecord 的完整文本存储与按 record_id 读取。
- normalized question 的候选列表存储与 scope 过滤。
- session-turn 到 record_id 的映射。
- embedding blob / ref 的挂载与状态表示。
- candidate list 的前缀 scan。
- 兼容 TTL / AOF / RDB / replication 的普通键语义。

不在第一版做的内容：

- 原生 ANN / HNSW / IVF。
- 向量分片系统。
- 查询 DSL。
- 多段 rerank。
- 复杂的 metadata 索引引擎。

## 3. Phase 1 结果

### 3.1 哪些结构可以直接复用

- `hash`：最适合做 QARecord、normalized index、session-turn 映射和 embedding 的 exact lookup。
- `skiptable`：最适合做 candidate scan index，尤其是按 scope 前缀收集候选。
- `range / sort`：不需要新增查询语义，直接可以用来扫 `cand:` 命名空间。

### 3.2 哪些现有接口可复用

- `kvs_hash_set / kvs_hash_get`
- `kvs_skiptable_set / kvs_skiptable_get / kvs_skiptable_range`
- `EXPIRE / PEXPIREAT / PERSIST`
- `FULLDUMP`
- AOF / RDB / replication 的现有写路径

### 3.3 哪些地方会影响 AOF/RDB/replication

- 只要新 schema 还是普通写命令，AOF 和 replication 就会自然生效。
- RDB 看到的是普通 key/value，只要 payload 是文本，就不需要额外的格式扩展。
- 真正需要注意的是大 value 和 payload 编码，而不是存储引擎本身。

## 4. Phase 2 结果

### 4.1 QARecord key/value 设计

推荐 key：

- `qa:{record_id}`

推荐 value：

- 完整 QARecord 的 JSON 文本。

建议字段：

- `record_id`
- `session_id`
- `turn_seq`
- `question`
- `normalized_question`
- `question_hash`
- `answer`
- `source_type`
- `provider`
- `model`
- `embedding_model`
- `embedding_status`
- `created_at`
- `updated_at`
- `origin_record_id`
- `metadata`

### 4.2 为什么选 JSON blob

- 当前 KVLite 的 value 处理链仍然是文本字符串，不适合直接塞 raw float bytes。
- QARecord 是业务对象，不是内核对象，JSON blob 最省事，也最利于主项目和调试工具直接消费。
- 细粒度拆字段会把 key 数量和写放大都放大，不符合“最小必要升级”。

### 4.3 结论

QARecord 第一版直接用 JSON blob，是当前实现和最小侵入原则下的最优选择。

## 5. Phase 3 结果

### 5.1 normalized question 索引设计

推荐两层：

- `qnorm:{hash}` -> 全局候选列表 JSON
- `qnorm:{hash}:scope:{scope}` -> scope 过滤后的候选列表 JSON

### 5.2 candidate list 设计

推荐候选对象至少带这些字段：

- `record_id`
- `session_id`
- `turn_seq`
- `provider`
- `model`
- `source_type`
- `embedding_status`
- `origin_record_id`

### 5.3 scope 规则

建议 scope 统一为可读字符串，避免和 key 分隔符冲突，例如：

- `global`
- `provider=openai`
- `model=gpt-4o-mini`
- `session=sess-001`
- `provider=openai|model=gpt-4o-mini|session=sess-001`

这样可以同时表达 provider / model / session 维度，而且可以直接作为 key 前缀的一部分。

### 5.4 如何避免不同 provider/model/session 的答案冲突

- 不要把 normalized question 直接映射成单个 answer。
- normalized question 只指向候选列表。
- scope 决策放在 retrieval adapter 或上层路由层。
- 先 exact hit，再按 scope 过滤，再按记录质量/更新时间/来源选择。

### 5.5 结论

`qnorm:{hash} -> candidate list` 是正确方向，`qnorm:{hash} -> single answer` 不够用。

## 6. Phase 4 结果

### 6.1 embedding 挂载设计

推荐 key：

- `vec:{record_id}`

推荐 value：

- 文本 envelope，第一版可以是 JSON，里面放：
  - `status`：`pending / ready / failed`
  - `mode`：`blob / ref / blob+ref`
  - `blob_b64` 或 `ref`
  - `model`
  - `dim`
  - `updated_at`
  - `error`

### 6.2 为什么不存 raw float array

- 当前 value 处理链不是 binary-safe。
- AOF / RDB / replication 里如果混入原始二进制，排障和兼容成本都会明显上升。
- base64 或 ref 让值仍然是文本，最符合当前代码路径。

### 6.3 状态建议

- `pending`：embedding 任务已发出，但结果还没回来。
- `ready`：embedding 已可用于检索。
- `failed`：生成失败或被拒绝，保留错误信息。

### 6.4 结论

第一版推荐“embedding status + ref 优先，blob 作为可选”，这样不会阻塞主链路，也不会把 KVLite 强行变成向量数据库。

## 7. Phase 5 结果

### 7.1 retrieval-facing API

这批接口最小且够用：

- `LoadQARecord(record_id)`
- `FindByNormalizedQuestion(hash)`
- `FindByNormalizedQuestion(hash, scope)`
- `FindBySessionTurn(session_id, turn_seq)`
- `LoadEmbedding(record_id)`
- `ScanCandidateRecords(scope, limit)`

### 7.2 当前仓库里对应的实现方式

这批接口不需要改内核，直接映射到 key builders + 现有 hash/skiptable exact lookup 即可。

建议把它们做成一个轻量 helper 层，供未来的 retrieval adapter 或 client 直接复用。

### 7.3 本次新增的 helper

已新增：

- [include/kvs_ai_chat_schema.h](../include/kvs_ai_chat_schema.h)

它提供：

- key builder
- QARecord exact load/store
- qnorm exact load/store
- session-turn exact load/store
- embedding exact load/store
- candidate scan 前缀 helper

## 8. Phase 6 结果

### 8.1 AOF

- 只要走普通写命令，AOF 会自然记录。
- 由于 payload 是文本 blob，AOF 重放时仍然是可读的 RESP 命令序列。
- 大 value 需要注意缓冲区和 fsync 开销，但这是已有问题，不是新 schema 独有的问题。

### 8.2 RDB

- RDB 按 key/value 快照保存。
- 只要 value 是文本，现有快照格式就能直接兼容。
- embedding 建议尽量用 ref 或较小的 base64 blob。

### 8.3 replication

- 普通写命令照常广播。
- candidate list、session-turn 映射、embedding 等都只是普通 key，复制语义不需要特殊扩展。
- 只要不引入内核级 binary blob，就不会把复制链路复杂化。

### 8.4 TTL

- TTL 不建议塞进 payload。
- 记录生命周期仍然应该依赖现有 `EXPIRE / PEXPIREAT / PERSIST`。
- 这样 TTL 语义和当前 KVLite 的绝对时间模型完全一致。

### 8.5 range/sort/full dump

- `cand:` 命名空间可以被 `range / sort` 直接利用做候选扫描。
- `FULLDUMP` 会把这些键当普通 key 输出，不需要特殊处理。
- 如果后续要清理业务数据，namespace 前缀也方便做批量定位和迁移。

### 8.6 namespace 建议

推荐固定前缀：

- `qa:`
- `qnorm:`
- `sess:`
- `vec:`
- `cand:`

这样既利于排障，也利于未来按 namespace 做回收和迁移。

## 9. 测试方案

本次补了一个独立的 schema 回归测试：

- [tests/ai_chat_schema_test.c](../tests/ai_chat_schema_test.c)

测试覆盖：

- key format 校验
- QARecord 写读
- qnorm 写读
- session-turn 写读
- embedding 写读
- candidate prefix scan
- record_id 反查

推荐再补两类测试：

- 大 value / base64 embedding 的边界测试
- AOF / RDB 启动后回放一致性测试

## 10. 文档路径

本次新增的文档和 helper 路径：

- [KVLite/docs/ai_chat_minimal_upgrade.md](ai_chat_minimal_upgrade.md)
- [KVLite/include/kvs_ai_chat_schema.h](../include/kvs_ai_chat_schema.h)
- [KVLite/tests/ai_chat_schema_test.c](../tests/ai_chat_schema_test.c)

## 11. 当前不做原生 ANN 的理由

结论：不做。

原因：

- 当前 KVLite 的核心优势是简单、可解释、已有 TTL/AOF/RDB/replication 闭环。
- 原生 ANN 会直接把内核复杂度、内存布局、复制语义和快照格式都推高。
- 现在的目标只是为 AI-chat 提供一个稳定的业务存储底座，不是要把 KVLite 变成完整向量数据库。

最小折中：

- exact lookup + candidate list + scope 过滤 + 小规模 brute-force。
- 向量检索先放到上层 adapter 或外部索引。

## 12. 后续理想演进路线

建议后续按这个顺序走：

1. 先把 `qa / qnorm / sess / vec / cand` 的 schema 在主项目和 KVLite 之间稳定下来。
2. 再把 retrieval adapter 做成可切换的 local/cache_hit/hybrid 路由。
3. 然后补 embedding 生成和候选拉取的批量路径。
4. 最后如果数据量真的上来，再考虑专门的向量索引，而不是先改 KVLite 内核。

这条路线的核心原则一直不变：先把业务闭环做对，再决定要不要引入更重的检索引擎。
