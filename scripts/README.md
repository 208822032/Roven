# Scripts Layout

`scripts/` 目录按工具职责分层：

- `benchmark/`：压测、结果解析、性能辅助工具
- `build/`：依赖安装、libbpf bootstrap
- `dev/`：环境诊断

根目录保留兼容 wrapper，正式入口优先使用子目录路径。
