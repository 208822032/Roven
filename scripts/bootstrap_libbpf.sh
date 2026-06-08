#!/usr/bin/env bash
# ============================================================================
# bootstrap_libbpf.sh — 下载并构建 libbpf 到指定目录
# ============================================================================
# 用法:
#   ./scripts/bootstrap_libbpf.sh <安装目录> [版本] [git_url]
#
# 参数:
#   INSTALL_DIR   安装目标目录 (必需)，如 /path/to/third_party/libbpf
#   VERSION       libbpf tag/分支 (默认 v1.0.3)
#   GIT_URL       git 仓库地址 (默认 https://github.com/libbpf/libbpf.git)
#
# 输出:
#   INSTALL_DIR/
#     include/bpf/  — 头文件
#     lib/          — libbpf.a, libbpf.so
#
# 幂等: 已存在且版本匹配则跳过
# ============================================================================

set -euo pipefail

INSTALL_DIR="${1:?用法: $0 <安装目录> [版本] [git_url]}"
VERSION="${2:-v1.0.3}"
GIT_URL="${3:-https://github.com/libbpf/libbpf.git}"
WORK_DIR="${INSTALL_DIR}.build"

# 版本 stamp 文件 — 用于幂等检查
STAMP="${INSTALL_DIR}/.bpf_bootstrap_stamp"

# 已构建且版本一致 → 跳过
if [ -f "${STAMP}" ] && [ "$(cat "${STAMP}")" = "${VERSION}" ] && \
   [ -f "${INSTALL_DIR}/lib/libbpf.a" ] && \
   [ -d "${INSTALL_DIR}/include/bpf" ]; then
    echo "  [bootstrap] libbpf ${VERSION} 已就绪，跳过"
    exit 0
fi

echo "  [bootstrap] 构建 libbpf ${VERSION}..."

# ── 清理旧的构建临时目录 ──────────────────────────────────────────────
rm -rf "${WORK_DIR}"

# ── clone ───────────────────────────────────────────────────────────────
git clone --depth 1 --branch "${VERSION}" "${GIT_URL}" "${WORK_DIR}" 2>&1 | \
    sed 's/^/  [git] /'

# ── 编译 ────────────────────────────────────────────────────────────────
NPROC=$(nproc 2>/dev/null || echo 4)
make -C "${WORK_DIR}/src" -j"${NPROC}" \
    BUILD_STATIC_ONLY=1 2>&1 | tail -3

# ── 安装到目标目录 ─────────────────────────────────────────────────────
rm -rf "${INSTALL_DIR}"
mkdir -p "${INSTALL_DIR}/lib" "${INSTALL_DIR}/include/bpf"

# 复制库文件
cp "${WORK_DIR}/src/libbpf.a"  "${INSTALL_DIR}/lib/"
cp "${WORK_DIR}/src/libbpf.so."* "${INSTALL_DIR}/lib/" 2>/dev/null || true
ln -sf "$(basename "$(ls "${INSTALL_DIR}/lib/libbpf.so."* 2>/dev/null | head -1)")" \
    "${INSTALL_DIR}/lib/libbpf.so" 2>/dev/null || true

# 复制头文件 — 以 <bpf/xxx.h> 方式可用
BPF_HEADERS=(
    bpf.h            bpf_core_read.h  bpf_endian.h
    bpf_helpers.h    bpf_helper_defs.h bpf_tracing.h
    libbpf.h         btf.h
    libbpf_common.h  libbpf_legacy.h
    libbpf_version.h
)
for h in "${BPF_HEADERS[@]}"; do
    if [ -f "${WORK_DIR}/src/${h}" ]; then
        cp "${WORK_DIR}/src/${h}" "${INSTALL_DIR}/include/bpf/"
    fi
done

# 复制 UAPI linux/ 头 (较新的 BPF_* 常量)
mkdir -p "${INSTALL_DIR}/include/linux"
if [ -f "${WORK_DIR}/include/uapi/linux/bpf.h" ]; then
    cp "${WORK_DIR}/include/uapi/linux/bpf.h" "${INSTALL_DIR}/include/linux/"
fi

# ── 写入版本 stamp ─────────────────────────────────────────────────────
echo "${VERSION}" > "${STAMP}"

# ── 清理构建临时目录 ──────────────────────────────────────────────────
rm -rf "${WORK_DIR}"

echo "  [bootstrap] libbpf ${VERSION} → $(ls -lh "${INSTALL_DIR}/lib/libbpf.a" | awk '{print $5}')"
echo "  [bootstrap] 安装完成: ${INSTALL_DIR}"
