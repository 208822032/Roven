#!/usr/bin/env bash
# ============================================================================
# install_deps.sh — 检查 / 安装 kvstore 系统构建依赖
# ============================================================================
# 模式：
#   普通模式（默认）: 检查缺失包，输出安装命令，退出码 0/1
#   AUTO_INSTALL_DEPS=1: 自动执行 apt-get install (需 sudo)
#
# 用法：
#   ./scripts/install_deps.sh            # 检查
#   AUTO_INSTALL_DEPS=1 ./scripts/install_deps.sh   # 自动安装
#   make deps                            # 通过 Makefile 调用
#   AUTO_INSTALL_DEPS=1 make deps        # 自动安装
# ============================================================================

set -euo pipefail

# ── 样式 ────────────────────────────────────────────────────────────────
RED='\033[0;31m'; GREEN='\033[0;32m'; YELLOW='\033[1;33m'; NC='\033[0m'
ok()   { echo -e "  ${GREEN}✓${NC} $1"; }
warn() { echo -e "  ${YELLOW}⚠${NC} $1"; }
err()  { echo -e "  ${RED}✗${NC} $1"; }

# ── 平台检测 ────────────────────────────────────────────────────────────
if ! command -v apt-get &>/dev/null; then
    echo "注意: 当前仅支持 apt-get (Debian/Ubuntu)。请手动安装依赖。"
    exit 0
fi

AUTO=${AUTO_INSTALL_DEPS:-0}

# ── 必需的系统包 ────────────────────────────────────────────────────────
# 格式: "包名:用途:级别(must/recommend)"
REQUIRED=(
    "build-essential:GCC/G++/make 工具链:must"
    "gcc:GCC C 编译器:must"
    "g++:GCC C++ 编译器:must"
    "clang-11:Clang 编译器 (用于 eBPF 内核态编译):recommend"
    "llvm-11:LLVM 工具链 (用于 eBPF):recommend"
    "libelf-dev:ELF 文件处理库:must"
    "zlib1g-dev:zlib 压缩库:must"
    "libreadline-dev:GNU readline (kvstore_client):recommend"
    "liburing-dev:io_uring 异步 IO:must"
    "libssl-dev:OpenSSL 加密库:must"
    "libhiredis-dev:hiredis 客户端库:recommend"
    "liburcu-dev:Userspace RCU (无锁数据结构):recommend"
    "liblz4-dev:LZ4 压缩库:recommend"
    "linux-tools-common:内核工具 (bpftool):recommend"
    "linux-tools-$(uname -r):当前内核专用工具:recommend"
    "git:版本控制:must"
    "pkg-config:包配置工具:must"
    "curl:HTTP 客户端 (可选):recommend"
)

# ── 检查包是否安装 ──────────────────────────────────────────────────────
is_installed() {
    dpkg -l "$1" 2>/dev/null | grep -q '^ii'
}

# ── 收集缺失包 ──────────────────────────────────────────────────────────
MISSING_MUST=()
MISSING_REC=()
PRESENT=()

for entry in "${REQUIRED[@]}"; do
    pkg="${entry%%:*}"
    rest="${entry#*:}"
    desc="${rest%%:*}"
    level="${rest##*:}"

    if is_installed "$pkg"; then
        PRESENT+=("$pkg: $desc")
    elif [ "$level" = "must" ]; then
        MISSING_MUST+=("$pkg: $desc")
    else
        MISSING_REC+=("$pkg: $desc")
    fi
done

# ── 输出检查结果 ────────────────────────────────────────────────────────
echo ""
echo "═══════════════════════════════════════════════"
echo "  kvstore 系统依赖检查"
echo "═══════════════════════════════════════════════"
echo ""

# 已安装
echo "已安装 (${#PRESENT[@]}/${#REQUIRED[@]}):"
for p in "${PRESENT[@]}"; do ok "$p"; done
echo ""

# 缺失 (必须)
HAS_MISSING=0
if [ ${#MISSING_MUST[@]} -gt 0 ]; then
    HAS_MISSING=1
    echo "缺失 — 必须 (${#MISSING_MUST[@]}):"
    for p in "${MISSING_MUST[@]}"; do err "$p"; done
    echo ""
fi

# 缺失 (推荐)
if [ ${#MISSING_REC[@]} -gt 0 ]; then
    echo "缺失 — 推荐 (${#MISSING_REC[@]}):"
    for p in "${MISSING_REC[@]}"; do warn "$p"; done
    echo ""
fi

# ── 安装操作 ────────────────────────────────────────────────────────────
TOTAL_MISSING=$(( ${#MISSING_MUST[@]} + ${#MISSING_REC[@]} ))

if [ $TOTAL_MISSING -eq 0 ]; then
    echo "所有依赖已就绪。"
    exit 0
fi

# 提取包名列表
ALL_MISSING_PKGS=()
for p in "${MISSING_MUST[@]}"; do ALL_MISSING_PKGS+=("${p%%:*}"); done
for p in "${MISSING_REC[@]}"; do ALL_MISSING_PKGS+=("${p%%:*}"); done

MUST_PKGS=""
for p in "${MISSING_MUST[@]}"; do MUST_PKGS="$MUST_PKGS ${p%%:*}"; done

# 检查实际可安装的包 (排除内核版本特定的 linux-tools)
INSTALLABLE=()
for pkg in "${ALL_MISSING_PKGS[@]}"; do
    if echo "$pkg" | grep -q 'linux-tools-5\.'; then
        # 内核版本工具可能无法安装，跳过
        warn "跳过内核版本特定包: $pkg (如需要请手动安装)"
    elif apt-cache show "$pkg" &>/dev/null; then
        INSTALLABLE+=("$pkg")
    else
        warn "包 $pkg 不在 apt 源中，跳过"
    fi
done

if [ "$AUTO" = "1" ]; then
    if [ ${#INSTALLABLE[@]} -gt 0 ]; then
        echo ""
        echo "→ 自动安装 ${#INSTALLABLE[@]} 个包: ${INSTALLABLE[*]}"
        sudo apt-get update -qq 2>/dev/null || true
        sudo apt-get install -y "${INSTALLABLE[@]}"
        echo ""
        echo "安装完成。"
    fi
else
    if [ $HAS_MISSING -eq 1 ]; then
        echo "───────────────────────────────────────────────"
        echo "  默认不自动安装系统包 (安全策略)。"
        echo ""
        echo "  选择一 (推荐): 手动执行"
        echo "    sudo apt-get update"
        echo "    sudo apt-get install -y$MUST_PKGS"
        echo ""
        echo "  选择二: 让 make 自动安装"
        echo "    AUTO_INSTALL_DEPS=1 make deps"
        echo ""
        if [ ${#INSTALLABLE[@]} -gt 0 ]; then
            echo "  可安装的包 (${#INSTALLABLE[@]}):"
            for p in "${INSTALLABLE[@]}"; do echo "    $p"; done
        fi
        echo "───────────────────────────────────────────────"
        exit 1
    elif [ ${#MISSING_REC[@]} -gt 0 ]; then
        echo "仅缺失推荐包，不影响核心构建。"
        if [ ${#INSTALLABLE[@]} -gt 0 ]; then
            echo "如需安装: sudo apt-get install -y ${INSTALLABLE[*]}"
        fi
        exit 0
    fi
fi
