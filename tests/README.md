# Tests Layout

`tests/` 目录只保留测试源码（`.c` / `.sh` / `.py` / `.md`）。所有构建产物统一进入 `build/tests/bin/`。

## 目录结构

- `ttl/`：TTL 后端矩阵与链路验证（7 个 shell 脚本）
- `vector/`：向量/embedding 语义测试
- `benchmark/`：测试侧 benchmark shell

根目录保留 C 测试源码，由 Makefile 编译，二进制输出到 `build/tests/bin/`：

| 源码 | 类别 |
|------|------|
| `testcase.c` | 通用 pipeline 集成测试 |
| `mempool_test.c` | 内存池单元测试 |
| `ai_chat_schema_test.c` | AI Chat schema 集成测试 |
| `test_bigvalue.c` / `test_paper.c` / `test_paper_sync.c` | 集成测试 |
| `kvstore_client.c` | RESP CLI 客户端工具 |
| `vector_testcase.c` | 向量 CRUD 集成测试 |
| `write_qps_test.c` / `write_ttl_qps_test.c` | KV 写入 QPS benchmark |
| `vector_benchmark.c` / `vector_import_bench.c` | 向量 benchmark |
| `ttl_expired_path_bench.c` / `ttl_lookup_split_bench.c` | TTL 路径 benchmark |
| `skiptable_targeted_bench.c` | Skiptable 专项 benchmark |
| `test_binary_key.c` / `vector_search_bench.c` / `vector_store_bench.c` | 手动/补充测试 |

## Vector Gate 分组

- core_flat 是 exact correctness baseline / fallback，保留 correctness、rebuild、RDB reload 回归。
- current core_hnsw 是 legacy regression/reference backend，保留 correctness、rebuild、RDB reload、AOF replay 回归，不再作为新优化主线。
- core_hnsw_v2 是默认 ANN backend，保留 tombstone、skeleton、graph construction、search recall、product integration、default backend gate。
- `vector_rebuild_*` 是 manager-owned rebuild harness，保留为 v2 default backend 的内部安全门禁。
- v2 performance / distance / reverse-prune profile tests 是 optional profile，不属于默认 `make tests` 构建集合。

## 运行测试

```bash
# 构建所有测试
make tests

# 构建可选 profiling / attribution 二进制
make optional-profile-tests

# 运行示例
./build/tests/bin/testcase 127.0.0.1 9300 5
./build/tests/bin/vector_testcase 127.0.0.1 9300
bash tests/ttl/test_ttl_all_backends.sh 127.0.0.1 9300
bash tests/vector/semantic_onnx_test.sh 127.0.0.1 28080
```

## Route C 说明

- Route C 已完工，阶段性测试脚本已删除，历史证据保留在 `docs/route_c/` 中。
- 日常回归通过产品级通用功能测试覆盖。
- 回滚命令：`KVLITE_ROUTE_C_WEAK_RWLOCK=0 ./kvstore --config config/kvstore.conf`
