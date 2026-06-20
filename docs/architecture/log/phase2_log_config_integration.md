# KVLite 阶段 2：日志级别和日志文件路径接入配置

## 1. 本阶段目标

阶段 1 已完成统一日志出口，本阶段只做“配置接入”，不扩展异步队列、滚动、采样或模块级动态配置。

目标是把日志初始化从代码写死改成配置驱动，至少支持：

- 日志级别
- 日志输出文件路径

## 2. 配置链路分析

### 2.1 默认值定义位置

默认配置集中在 [src/core/kvs_config.c](../../src/core/kvs_config.c) 的 `kvs_config_init_default`：

- `port = 8080`
- `role = master`
- `enable_aof = ENABLE_AOF`
- `sync_mode = KVS_SYNC_TCP`
- `log_level = info`
- `log_file = ""`，表示继续输出到 `stderr`

### 2.2 配置结构体定义位置

配置结构体定义在 [include/kvs_config.h](../../include/kvs_config.h)，本阶段新增了日志相关字段：

- `log_level`
- `log_file`

### 2.3 解析入口位置

现有配置解析入口保持不变：

- `kvs_config_load_file(...)` 负责解析 `key=value`
- `kvs_config_apply_cmdline(...)` 负责命令行覆盖

本阶段只是在这两个入口中增加日志相关 key / 参数，不改现有配置优先级。

### 2.4 启动时日志初始化时机

`main` 里现在采用以下顺序：

1. `kvs_config_init_default`
2. 解析命令行，拿到 `--config`
3. 加载配置文件
4. 再次应用命令行，保证 CLI 优先级最高
5. 根据最终配置调用 `kvs_log_init(...)`
6. 继续后续启动流程

这个顺序保证了日志初始化由配置驱动，同时不改变原有业务控制流。

## 3. 新增配置项

### 3.1 `log_level`

支持值：

- `error`
- `warn`
- `info`
- `debug`

默认值是 `info`。

### 3.2 `log_file`

默认为空字符串，表示继续输出到 `stderr`。

当配置为非空路径时，日志会尝试写入该文件；如果文件打开失败，会回退到 `stderr`，并打印可观察的错误提示。

## 4. 默认行为和降级策略

- 未配置 `log_level` 时，使用 `info`
- 未配置 `log_file` 时，继续输出到 `stderr`
- `log_level` 非法时，降级为 `info`，并输出错误提示
- `log_file` 打开失败时，降级为 `stderr`，并输出错误提示

这里的降级策略是“不中断启动，保留可观测性”。

## 5. 关键实现说明

### 5.1 最小侵入接入

实现没有拆出新的后台线程，也没有改动现有命令执行链路，只是在启动阶段把最终配置喂给日志初始化接口。

### 5.2 配置项解析

`log_level` 和 `log_file` 都沿用现有 `key=value` 文本配置风格；命令行也支持对应覆盖参数，保持和项目现有配置系统一致。

### 5.3 日志初始化策略

为了兼容配置加载阶段的非致命错误，`main` 采用“先完成配置，再初始化最终日志”的方式。

这样可以确保：

- 最终日志级别和输出文件都来自配置
- 配置错误仍然可以被打印出来
- 业务控制流不发生变化

## 6. 验证步骤与结果

### 6.1 不配日志配置时仍可启动

使用默认配置或不填写日志项，启动流程仍然正常，日志继续输出到 `stderr`。

### 6.2 `log_level=error`

配置为 `error` 后，`INFO/WARN` 类启动信息会被抑制，只保留错误级别日志。

### 6.3 `log_file=...`

配置为有效文件路径后，日志会写入目标文件，`stderr` 不再承载常规日志输出。

### 6.4 非法 `log_level`

非法值不会导致进程退出；启动时会回退到 `info`，并打印错误提示。

## 7. 本阶段改动文件

- [include/kvs_config.h](../../include/kvs_config.h)
- [src/core/kvs_config.c](../../src/core/kvs_config.c)
- [src/core/kvstore.c](../../src/core/kvstore.c)
- [config/kvstore.conf](../../config/kvstore.conf)
- [docs/log/phase2_log_config_integration.md](phase2_log_config_integration.md)

## 8. 当前遗留问题

- 目前只接入了全局日志级别和全局日志文件路径，没有做模块级别配置
- 仍然没有异步日志队列
- 仍然没有日志滚动
- 仍然没有采样 / 限流

## 9. 下一步建议

1. 继续小步迁移剩余 legacy 输出，但不要碰热路径和业务控制流。
2. 如需进一步优化性能，再考虑异步队列和日志滚动。