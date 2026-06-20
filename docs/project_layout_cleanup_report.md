# Project Layout Cleanup Report

## 目标

本次整理的目标是把 KVLite 的目录边界收敛到更接近正式产品的结构，同时保持 Route C 完成态、构建流程和历史证据可追溯。

## 生产代码变更

未修改核心业务逻辑；本次主要变更目录布局、入口文档、脚本路径和兼容 wrapper。

## 新目录边界

- `src/` / `include/`：生产源码与头文件，未做功能性移动
- `config/`：正式配置
- `third_party/`：vendored 依赖，`third_party/NtyCo` 保持不动
- `tests/route_c/`：Route C 产品级 gate 与活跃阶段回归
- `tests/ttl/`：TTL 主矩阵与后端语义测试
- `tests/vector/`：向量/embedding 语义测试入口
- `tests/benchmark/`：测试侧 benchmark 辅助脚本
- `tests/archive/route_c/`：历史阶段脚本归档
- `scripts/benchmark/`：压测与结果处理脚本
- `scripts/build/`：构建辅助脚本
- `scripts/dev/`：开发诊断脚本
- `docs/route_c/`：Route C 正式文档入口
- `docs/architecture/`：仍然有效的架构文档
- `docs/archive/`：历史阶段和内部计划归档

## 文件移动映射

关键移动：

- `docs/route_c_perf_arch/*` → `docs/route_c/archive/stage_docs/*`
- `tests/*stage*.sh` 中 Route C 活跃脚本 → `tests/route_c/`
- `tests/test_ttl_*.sh` 与 `tests/test_ttl_all_backends.sh` → `tests/ttl/`
- `tests/semantic_onnx_test.sh` → `tests/vector/semantic_onnx_test.sh`
- `scripts/bench_*` 与 benchmark helper → `scripts/benchmark/`
- `scripts/bootstrap_libbpf.sh`, `scripts/install_deps.sh` → `scripts/build/`
- `scripts/doctor.sh` → `scripts/dev/doctor.sh`

## 兼容层

旧路径下保留了兼容 wrapper：

- `tests/*.sh` 包装到 `tests/route_c/`、`tests/ttl/`、`tests/vector/`、`tests/benchmark/`
- `scripts/*` 包装到 `scripts/benchmark/`、`scripts/build/`、`scripts/dev/`
- `docs/route_c_perf_arch/README.md` 用于提示新入口

## 归档与删除

- 归档：Route C 历史阶段设计/验证文档、Stage 7-9 embedding 历史测试、TTL MVP 文档、内部 superpowers 计划
- 删除：本次不直接删除不确定文件

## 引用更新

已同步更新：

- `Makefile`
- `README.md`
- `docs/route_c/*.md`
- Route C 活跃脚本内部互调路径

## 验证项

需要确认：

1. `git diff --check`
2. `make clean`
3. `make embedding ENABLE_EBPF=0`

注：Route C 测试脚本已于 2026-06-19 最终清理中删除（详见 `docs/route_c/route_c_test_script_removal_report.md`）。

## 剩余风险

- 历史归档文档中的旧路径引用仍然存在，但兼容 wrapper 允许继续访问
- `tests/` 根目录仍保留编译型测试二进制与源码，后续若要进一步收口，需要配套调整 Makefile 目标命名
- `data/`、`logs/`、`results/`、`workspace/` 仍是运行期目录，未纳入本次结构重命名范围
