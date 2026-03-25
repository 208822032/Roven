# TTL 测试用例说明

## 1. 主入口与兼容入口

当前 TTL 主测试入口只有一个：

- 主入口： [tests/test_ttl_all_backends.sh](../../tests/test_ttl_all_backends.sh)
- 兼容入口： [tests/test_ttl_mvp.sh](../../tests/test_ttl_mvp.sh)

其中 [tests/test_ttl_mvp.sh](../../tests/test_ttl_mvp.sh) 只是薄包装，真实逻辑全部在 [tests/test_ttl_all_backends.sh](../../tests/test_ttl_all_backends.sh) 中。

## 2. 测试目标

验证 KVLite 的 TTL 语义是否通过真实服务链路生效，而不是函数级单测。

脚本会：

- 自己启动服务端进程
- 等待服务可连接
- 通过 `tests/kvstore_client -c` 发送 RESP 命令
- 对服务端响应做自动断言
- 在退出时清理服务进程和临时文件

## 3. 后端矩阵

脚本覆盖四个后端：

| 后端 | 写命令 | 读命令 | EXIST 命令 | 删除命令 | 覆盖写清 TTL | RANGE/SORT TTL 过滤 |
|---|---|---|---|---|---|---|
| array | SET | GET | EXIST | DEL | 支持并严格验证 | 未接入，仅 smoke |
| rbtree | RSET | RGET | REXIST | RDEL | 通过 MOD 路径验证；SET 覆盖语义不作为通过项 | 未接入，仅 smoke |
| hash | HSET | HGET | HEXIST | HDEL | 通过 MOD 路径验证；SET 覆盖语义不作为通过项 | 未接入，仅 smoke |
| skiptable | SSET | SGET | SEXIST | SDEL | 支持并严格验证 | 未接入，仅 smoke |

说明：

- `array`、`skiptable` 的 `SET/SSET` 覆盖写清 TTL 会被直接验证。
- `rbtree`、`hash` 当前只把 `MOD/RMOD/HMOD` 作为稳定的 TTL 清理验证路径，`SET` 覆盖语义会在输出里明确标记为 `[SKIP]`，不会伪装成通过。
- `RANGE/SORT` 当前还没有接入 TTL 过滤，所以只能做命令路径 smoke，不算已完成该语义。

## 4. 已严格验证的 TTL 语义

脚本当前对以下语义做了真实链路断言：

1. 基础 TTL 闭环
- 写入 key
- `EXPIRE`
- `TTL`
- 到期前可读
- 到期后不可读

2. 惰性删除
- 通过读命令触发
- 通过 `EXIST/REXIST/HEXIST/SEXIST` 触发

3. `PERSIST`
- 设置 TTL 后取消过期
- 跨过原窗口后仍可读

4. 覆盖写清 TTL
- `SET / RSET / HSET / SSET`
- `MOD / RMOD / HMOD / SMOD`
- 对不稳定的 backend，会在输出里清晰说明为什么只验证 MOD 路径

5. 删除语义
- 删除后 key 不存在
- `TTL` 返回不存在语义 `-2`
- 不残留过期元数据

6. 不存在 key 的语义
- `TTL no_such_key`
- `EXPIRE no_such_key`
- `PERSIST no_such_key`

## 5. 仅 smoke 的部分

当前 `RANGE/SORT` 仍是 smoke，只验证命令链路通，不验证 TTL 过滤：

- 会执行 `RANGE / SORT` 的命令路径
- 会确保不会把它们误判为日志级别的“看起来成功”
- 会在输出里明确标记 `[SKIP][backend][range-ttl] reason=TTL filtering is not wired into RANGE/SORT yet`

如果后续服务端把 TTL 过滤接入这两个接口，再把脚本里的 `range-ttl` 从 skip 升级成真正断言即可。

## 6. readiness 和轮询

脚本已经把 readiness 和 TTL 语义解耦：

- readiness 使用的是非 TTL 的基础连通性探测
- TTL 验证使用轮询 helper，而不是固定 `sleep 2`

当前脚本里的轮询 helper 主要有：

- `expect_eventually_exact`
- `expect_eventually_ttl_exact`

它们都带超时、轮询间隔和最小等待时间，避免时序脆弱性。

## 7. 输出约定

脚本输出会尽量保持统一格式：

- `[PASS][array][basic-expire] cmd=EXPIRE ...`
- `[SKIP][hash][set-overwrite] reason=...`
- `[FAIL][skiptable][persist] reason=... expected=... actual=... cmd=...`

这样可以一眼看出：

- 哪个 backend 出问题
- 哪个 case 出问题
- 是通过、跳过，还是失败

## 8. 执行方式

先构建：

```bash
make
```

执行主入口：

```bash
bash tests/test_ttl_all_backends.sh
```

兼容入口也可以执行：

```bash
bash tests/test_ttl_mvp.sh
```

可选环境变量：

- `KV_BIN`：kvstore 可执行文件路径，默认 `./kvstore`
- `CLIENT_BIN`：客户端可执行文件路径，默认 `./tests/kvstore_client`
- `KV_HOST`：服务地址，默认 `127.0.0.1`
- `TTL_TEST_PORT`：测试端口，默认 `19331`

## 9. 预期结果

主测试成功结束时，应该看到类似输出：

- 每个 backend 都有自己的 `[INFO]`、`[PASS]`、`[SKIP]`
- 最终有：`[PASS][ttl][all-backends] TTL matrix suite completed successfully`

其中 `[SKIP]` 不代表失败，而是明确声明“当前 backend / 当前能力尚未覆盖”或“当前只做 smoke”。

## 10. 已知限制

1. 这是内存态 TTL 测试，不覆盖 RDB / AOF / 主从复制恢复。
2. `RANGE / SORT` 当前仅 smoke，不代表 TTL 过滤已完成。
3. 脚本通过 `tests/kvstore_client -c` 走真实 socket/RESP 链路，不是函数级单测。
4. 脚本默认只启动单机 master，不启动 slave。

## 11. 失败时优先排查

1. 服务是否成功启动，查看临时目录里的 `kvstore.log`
2. `tests/kvstore_client` 是否能连接测试端口
3. `EXPIRE / TTL / PERSIST` 是否接入了命令表和参数解析
4. `MOD/RMOD/HMOD/SMOD` 是否能正确清 TTL
5. `GET / EXIST` 是否在过期后触发了惰性删除
6. `RANGE / SORT` 是否至少能跑通 smoke 路径