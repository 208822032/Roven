# Tools

`tools/` 存放项目自研或项目维护的开发辅助工具。

## 当前工具

| 目录 | 说明 |
|------|------|
| `btf2header/` | 从内核 BTF 生成 vmlinux.h（eBPF 编译依赖）。源码 `btf2header.c` 由 `make bootstrap` 编译；预编译二进制 `btf2header` 已提交以便无 Clang/BPF 环境使用。 |

## 职责边界

- `tools/` — 项目自研/维护的辅助工具和代码生成器
- `third_party/` — 外部依赖源码、SDK、runtime（jemalloc, NtyCo, libbpf, onnxruntime）
- `scripts/build/` — 依赖下载、安装、bootstrap、setup 脚本（如 `setup_onnx_embedding.sh`）
