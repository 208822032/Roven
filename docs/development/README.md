# Development Docs

维护规则：

- 生产代码边界以 `src/`、`include/`、`config/`、`third_party/` 为主
- 新测试优先放入 `tests/` 子分类目录，而不是继续堆在根目录
- 历史阶段证据优先归档到 `docs/archive/` 或 `tests/archive/`
- 路径调整必须同步更新 `Makefile`、脚本和正式文档入口
