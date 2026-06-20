# 阶段 1：VSET/VSEARCH embedding 锁外 prepare 审查报告

## 1. 审查结论

CONDITIONALLY_APPROVED

## 2. 核心结论摘要

- `VSET` 和 `VSEARCH` 的 embedding prepare 已经移动到 `g_kv_data_rwlock` 加锁之前。
- `VSET` 的 record/id_map/flat index/HNSW insert 仍在写锁内执行。
- `VSEARCH` 的 HNSW search、record 读取、deleted/inactive 过滤、scope 过滤、topK/threshold 结果组装仍在读锁内执行。
- `VSET` commit 阶段会在写锁内重新检查 `question+scope`，重复写入保持“只更新 answer，不重建 embedding”的原语义。
- prepared 字符串和 embedding owner 清晰；常规错误路径未发现明显泄漏或 double free。
- RESP 协议、RDB/AOF 格式、主从协议未被本阶段修改。
- 构建、hash provider 下现有 vector testcase、手工 V 族功能验证和小规模 latency 冒烟通过。
- 未运行 ASAN/valgrind；ONNX semantic 脚本因当前服务端 hash/64 维配置与脚本期望 ONNX/512 维不匹配而未完成。

## 3. 实际调用链

### VSET

```text
net_session_process_resp
  -> kvs_filter_protocol
  -> parse_command_argument
  -> kvs_vector_set_prepare              # 锁外：复制 question/answer/scope，生成 embedding
  -> pthread_rwlock_wrlock(g_kv_data_rwlock)
  -> kvs_vector_set_commit_prepared      # 写锁内：二次检查 question+scope
       -> kvs_vector_find_record
       -> existing: kvs_vector_record_update_answer
       -> new: kvs_vector_find_free_slot / ensure_capacity
       -> new: kvs_vector_id_map_put
       -> new: kvs_vector_flat_index_add
       -> new: kvs_vector_index_add_current
  -> pthread_rwlock_unlock(g_kv_data_rwlock)
  -> AOF append / TCP replication only when commit status is KVS_VECTOR_OK
```

### VSEARCH

```text
net_session_process_resp
  -> kvs_filter_protocol
  -> parse_command_argument
  -> kvs_vector_search_prepare           # 锁外：复制 query/scope，生成 query embedding
  -> pthread_rwlock_rdlock(g_kv_data_rwlock)
  -> kvs_vector_search_prepared_to_resp
       -> kvs_vector_search_with_prepared
       -> kvs_vector_index_search_current # 读锁内：HNSW/current index search
          -> record active/deleted filtering
          -> scope filtering
          -> topK/threshold export
  -> pthread_rwlock_unlock(g_kv_data_rwlock)
```

## 4. 锁边界审查

- `src/core/kvstore.c` 在 `pthread_rwlock_rdlock/wrlock` 之前对 `VSET/VSEARCH` 特判 prepare。
- `command_table` 未将 `VSET/VSEARCH` 改为 `LOCK_NONE`；`VSET` 仍是写锁命令，`VSEARCH` 仍是读锁命令。
- 其他命令仍走原 `cmd->handler` 路径和原 `lock_mode`。
- read-then-write retry 逻辑仍只适用于原 handler 返回 `KVS_CMD_RETRY_WITH_WRITE_LOCK` 的命令；VSET/VSEARCH 不走该 retry，未发现误影响。
- `VSEARCH` 仍是纯读命令，不触发 AOF/replication。

## 5. VSET 语义审查

- prepare 阶段深拷贝 `question`、`answer`、非空 `scope`，并生成 `embedding`。
- prepare 阶段未访问 vector store/id_map/HNSW；只检查 vector 模块初始化状态。
- embedding 失败会 destroy prepared 数据并直接返回，不进入写锁 commit。
- commit 阶段在写锁内调用 `kvs_vector_find_record` 二次检查 `question+scope`。
- 命中已有 record 时只调用 `kvs_vector_record_update_answer`，随后释放 prepared embedding，保持旧 `kvs_vector_set()` 重复写入语义。
- 新 record 路径把 prepared 字符串和 embedding 转移给 record，随后更新 id_map、flat index 和 current index/HNSW。
- HNSW insert 仍在写锁内。

## 6. VSEARCH 语义审查

- prepare 阶段深拷贝 `query` 和非空 `scope`，并生成 query embedding。
- query embedding 失败直接返回错误，不进入读锁。
- search 阶段在读锁内调用 `kvs_vector_index_search_current`。
- HNSW search、record active/deleted 过滤、scope 过滤和 topK/threshold 逻辑仍由原 index/search 路径完成。
- response 格式保持旧单条 VSEARCH 的 array of bulk string 格式：`score:...|scope:...|question:...|answer:...`。
- VSEARCH 看到的是进入读锁并 search 时刻的数据，不是 prepare 时刻快照。

## 7. 内存所有权审查

- `kvs_vector_set_prepared_destroy` 统一释放 `question/answer/scope/embedding` 并 reset。
- `kvs_vector_search_prepared_destroy` 统一释放 `query/scope/embedding` 并 reset。
- `kvs_vector_record_take_prepared_fields` 成功后将 prepared 字段置空，再 reset，避免 record 接管后 double free。
- repeated VSET 命中已有 record 时释放未使用 prepared embedding。
- VSEARCH 完成后由 `kvs_filter_protocol` cleanup 释放 query embedding。
- 未发现常规错误路径中的明显泄漏或 double free；建议后续补 ASAN/valgrind 验证。

## 8. 持久化与复制影响审查

- 本阶段未修改 RDB save/load 格式。
- 本阶段未修改 AOF append/rewrite 格式。
- 本阶段未修改主从复制协议或 slave apply 行为。
- VSET 的 AOF append / TCP replication 仍位于锁释放后的后置逻辑，并且只在 `kvs_vector_set_commit_prepared()` 返回 `KVS_VECTOR_OK` 后执行。
- prepare 阶段不会产生 AOF 或 replication。

## 9. 性能影响审查

- 锁内不再执行 embedding provider 的 `kvs_vector_embedding_generate`。
- 阶段 1 的直接收益是缩短 `g_kv_data_rwlock` 持有时间。
- reactor 下 handler 仍同步串行，`max_inflight` 可能仍为 1；这不是阶段 1 阻塞问题。
- 真正并发 embedding 仍需后续 worker pool。
- 新增深拷贝和 prepared 结构 cleanup 是阶段边界所需开销；未发现重复 embedding 或明显超范围 malloc/free。

## 10. 测试结果

- `make clean && make`：通过。
- `make clean && make embedding`：通过。
- `./tests/vector_testcase 127.0.0.1 8080`：通过。
- 手工 RESP 验证：VSET、VGET、VSEARCH、重复 VSET、VDEL 后过滤、scope 过滤均通过。
- `bash tests/vector/semantic_onnx_test.sh 127.0.0.1 8080`：未完成；当前服务端使用 hash provider、dimension=64，脚本期望 ONNX、dimension=512。
- latency 冒烟：
  - VSET c1 n=100：success=100 failed=0 errors=0。
  - VSEARCH c1 n=100：success=100 failed=0 errors=0。

## 11. 阻塞问题

无。

## 12. 非阻塞建议

- 后续用 ONNX/512 维配置重新运行 `tests/vector/semantic_onnx_test.sh`。
- 后续补 ASAN/valgrind 或等价泄漏检测，覆盖 prepare 失败、commit 失败、HNSW insert 失败和 response buffer 错误路径。
- 若需要性能结论，应使用固定配置记录 provider、network_mode、c4/c8/c16、p95/p99 和 `KVLITE_EMBEDDING_PROFILE=1` 的 max_inflight；当前 latency 冒烟只证明功能路径可用。

## 13. 是否建议提交

是。建议以 `CONDITIONALLY_APPROVED` 提交阶段 1 实现，并把 ONNX 语义测试和泄漏检测作为后续补充验证。

## 14. 建议 commit message

```bash
git add docs/route_c_perf_arch/03_vector_embedding_prepare_outside_lock.md \
        docs/route_c_perf_arch/15_stage1_vector_prepare_review.md \
        include/kvs_vector.h \
        src/core/kvstore.c \
        src/vector/kvs_vector.c
git commit -m "将V族embedding生成移出全局数据锁"
```
