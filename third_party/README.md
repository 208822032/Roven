# Third Party Dependencies

`third_party/` 存放外部依赖源码、SDK、runtime。

| 目录 | 类型 | 说明 |
|------|------|------|
| `jemalloc/` | Vendored 源码 + 本地构建 | 内存分配器，`make setup` 自动构建。`.a`/`.so` 以 prebuilt 形式提交，也支持从源码重建。 |
| `NtyCo/` | Vendored 源码 | 轻量协程库，`core/` 为源码，构建产物 `objs/` 和 `libntyco.a` 由 Makefile 管理 |
| `libbpf/` | Bootstrap 构建 | eBPF 用户态库，由 `make bootstrap` 构建，产物在 `lib/` |
| `onnxruntime/` | 本地 SDK（gitignored） | ONNX Runtime C API，由 `make setup-embedding` 下载安装 |

## 职责边界

- `third_party/` — 外部依赖源码、SDK、runtime
- `tools/` — 项目自研/维护的辅助工具
- `scripts/build/` — 依赖下载、安装、bootstrap 脚本
