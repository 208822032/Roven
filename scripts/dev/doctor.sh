#!/usr/bin/env bash
# ============================================================================
# doctor.sh — kvstore 环境全面诊断
# ============================================================================
# 检查内容:
#   1. 操作系统与内核版本
#   2. 内核 BTF (CONFIG_DEBUG_INFO_BTF)
#   3. 编译器 (GCC, Clang)
#   4. 必须/推荐库
#   5. eBPF 支持状态
#   6. 内核版本 → eBPF 功能兼容矩阵
#   7. 子模块状态
#   8. 构建产物状态
#
# 用法:
#   ./scripts/dev/doctor.sh
#   make doctor
# ============================================================================

set -euo pipefail

RED='\033[0;31m'; GREEN='\033[0;32m'; YELLOW='\033[1;33m'
CYAN='\033[0;36m'; BOLD='\033[1m'; NC='\033[0m'

ok()   { echo -e "  ${GREEN}✓${NC} $1"; }
warn() { echo -e "  ${YELLOW}⚠${NC} $1"; }
err()  { echo -e "  ${RED}✗${NC} $1"; }
info() { echo -e "  ${CYAN}→${NC} $1"; }
hdr()  { echo ""; echo -e "${BOLD}── $1 ──${NC}"; }

PASS=0; WARN=0; FAIL=0

# ── 1. 系统信息 ────────────────────────────────────────────────────────
hdr "1. 操作系统与内核"

if command -v lsb_release &>/dev/null; then
    OS="$(lsb_release -ds 2>/dev/null)" && ok "$OS"
else
    OS="$(cat /etc/os-release 2>/dev/null | grep PRETTY_NAME | cut -d= -f2 | tr -d '"')" && ok "$OS"
fi

KVER="$(uname -r)"
ARCH="$(uname -m)"
KVER_MAJ="$(echo "$KVER" | cut -d. -f1)"
KVER_MIN="$(echo "$KVER" | cut -d. -f2)"

ok "内核: $KVER 架构: $ARCH"

# 内核版本兼容矩阵
KVER_NUM=$(( KVER_MAJ * 100 + KVER_MIN ))
if [ "$KVER_NUM" -ge 508 ]; then
    ok "内核 >= 5.8 — 完整 eBPF 支持 (BPF_MAP_TYPE_RINGBUF 等)"
elif [ "$KVER_NUM" -ge 504 ]; then
    warn "内核 5.4 — eBPF 可编译，但 BPF_MAP_TYPE_RINGBUF 需 5.8+ 内核加载"
else
    err "内核 < 5.4 — eBPF 支持不可靠"
fi

# ── 2. BTF 检查 ────────────────────────────────────────────────────────
hdr "2. 内核 BTF"

if [ -f /sys/kernel/btf/vmlinux ]; then
    BTF_SIZE=$(stat -c%s /sys/kernel/btf/vmlinux 2>/dev/null || echo 0)
    if [ "$BTF_SIZE" -gt 1000 ]; then
        ok "/sys/kernel/btf/vmlinux 存在 ($(echo "scale=1; $BTF_SIZE/1048576" | bc)MB)"
    else
        err "/sys/kernel/btf/vmlinux 过小 ($BTF_SIZE bytes)"
    fi
else
    err "/sys/kernel/btf/vmlinux 不存在"
fi

if grep -q 'CONFIG_DEBUG_INFO_BTF=y' "/boot/config-${KVER}" 2>/dev/null; then
    ok "CONFIG_DEBUG_INFO_BTF=y"
else
    warn "CONFIG_DEBUG_INFO_BTF 状态未知 (config 文件未找到)"
fi

# ── 3. 编译器 ──────────────────────────────────────────────────────────
hdr "3. 编译器"

# GCC
if command -v gcc &>/dev/null; then
    GCCVER=$(gcc -dumpversion 2>/dev/null || echo "?")
    GCC_MAJ=$(echo "$GCCVER" | cut -d. -f1)
    if [ "$GCC_MAJ" -ge 8 ]; then ok "GCC $GCCVER"; else warn "GCC $GCCVER (建议 >= 8)"; fi
else
    err "GCC 未找到"
fi

if command -v g++ &>/dev/null; then
    GXXVER=$(g++ -dumpversion 2>/dev/null || echo "?")
    ok "G++ $GXXVER"
else
    err "G++ 未找到"
fi

# Clang
CLANG_FOUND=""
for c in clang-11 clang-12 clang-13 clang-14 clang-15 clang; do
    if command -v "$c" &>/dev/null; then
        CLANGVER=$("$c" --version 2>/dev/null | head -1 | grep -oP '\d+\.\d+\.\d+' | head -1)
        if [ -n "$CLANGVER" ]; then
            CLANG_MAJ=$(echo "$CLANGVER" | cut -d. -f1)
            if [ "$CLANG_MAJ" -ge 11 ]; then
                ok "Clang: $c ($CLANGVER)"
            else
                warn "Clang: $c ($CLANGVER) — eBPF 编译建议 >= 11"
            fi
            CLANG_FOUND="$c"
            break
        fi
    fi
done
[ -z "$CLANG_FOUND" ] && warn "未找到 Clang >= 11 (eBPF 编译需要)"

# ── 4. 必须系统库 ──────────────────────────────────────────────────────
hdr "4. 系统库"

check_dev() {
    local pkg="$1" header="$2" desc="$3" level="${4:-must}"
    if [ -f "$header" ] || pkg-config --exists "$pkg" 2>/dev/null; then
        ok "$desc"
    else
        if [ "$level" = "must" ]; then err "$desc"; else warn "$desc"; fi
    fi
}

check_dev libelf   /usr/include/libelf.h       "libelf (ELF 解析)"    must
check_dev zlib     /usr/include/zlib.h          "zlib (压缩)"          must
check_dev openssl  /usr/include/openssl/ssl.h   "OpenSSL (加密)"       must
check_dev liburing /usr/include/liburing.h      "liburing (io_uring)"  must
check_dev readline /usr/include/readline/readline.h "readline (CLI)"   recommend
check_dev hiredis  /usr/include/hiredis/hiredis.h  "hiredis (Redis)"   recommend
check_dev lz4      /usr/include/lz4.h           "LZ4 (压缩)"           recommend

# 检查 libbpf (系统级别)
if dpkg -l libbpf-dev &>/dev/null; then
    ok "libbpf-dev (系统包)"
    warn "系统 libbpf-dev 已安装，但项目使用 third_party/libbpf (版本可控)"
else
    info "libbpf-dev 未安装 (项目将使用 third_party/libbpf)"
fi

# ── 5. eBPF 工具链 ────────────────────────────────────────────────────
hdr "5. eBPF 工具链"

if command -v bpftool &>/dev/null; then
    BPFTOOL_VER=$(bpftool version 2>/dev/null | head -1 | grep -oP 'v?\d+\.\d+' | head -1)
    ok "bpftool: $(which bpftool) ($BPFTOOL_VER)"
else
    warn "bpftool 未安装 (不影响项目构建)"
fi

# 项目内置工具
BTF2H="$PWD/tools/btf2header/btf2header"
if [ -x "$BTF2H" ]; then
    ok "btf2header 已构建"
else
    warn "btf2header 未构建 — 执行 make bootstrap"
fi

# 项目内置 libbpf
BPFA="$PWD/third_party/libbpf/lib/libbpf.a"
if [ -f "$BPFA" ]; then
    ok "third_party/libbpf 已就绪"
else
    warn "third_party/libbpf 未构建 — 执行 make bootstrap"
fi

# ── 6. eBPF 能力矩阵 ──────────────────────────────────────────────────
hdr "6. eBPF 能力兼容矩阵"

echo ""
echo "  功能                    需要内核   当前状态"
echo "  ───────────────────────  ─────────  ────────"
for entry in \
    "kprobe/kretprobe:4.4:${KVER_NUM}" \
    "BPF ring buffer:5.8:${KVER_NUM}" \
    "BTF (CO-RE):5.4:${KVER_NUM}" \
    "BPF_MAP_TYPE_RINGBUF:5.8:${KVER_NUM}" \
    "bpf_probe_read_user:5.5:${KVER_NUM}" \
    "BPF link (perf):5.7:${KVER_NUM}"
do
    feat="${entry%%:*}"
    rest="${entry#*:}"
    need="${rest%%:*}"
    have="${rest##*:}"
    need_num=$(echo "$need" | awk -F. '{print $1*100+$2}')
    if [ "$have" -ge "$need_num" ] 2>/dev/null; then
        printf "  %-25s  ≥ %-8s  ${GREEN}支持${NC}\n" "$feat" "$need"
    else
        printf "  %-25s  ≥ %-8s  ${RED}不支持${NC}\n" "$feat" "$need"
    fi
done

echo ""
echo "  说明: 5.4 内核上 ebpf_kern.o 可编译，但 ringbuf 功能"
echo "        需 5.8+ 内核加载。可在同架构 5.8+ 机器上编译后拷贝。"

# ── 7. Git 子模块 ──────────────────────────────────────────────────────
hdr "7. Git 子模块"

if git submodule status 2>/dev/null | grep -q '^ '; then
    warn "子模块未初始化 — 执行: git submodule update --init --recursive"
else
    ok "子模块已初始化"
fi

# ── 8. 构建产物 ────────────────────────────────────────────────────────
hdr "8. 构建产物状态"

TARGETS=("kvstore" "src/eBPF/ebpf_kern.o" "src/eBPF/ebpf_syncd" "src/eBPF/vmlinux.h")
for t in "${TARGETS[@]}"; do
    if [ -f "$t" ]; then
        ok "$t ($(ls -lh "$t" | awk '{print $5}'))"
    else
        warn "$t — 未构建"
    fi
done

# ── 总结 ────────────────────────────────────────────────────────────────
echo ""
echo "═══════════════════════════════════════════════"
echo "  诊断完成"
echo ""
echo "  如果你的环境报告有缺失项，执行:"
echo "    AUTO_INSTALL_DEPS=1 make deps    # 安装系统包"
echo "    make bootstrap                  # 构建项目内工具链"
echo "    make setup                      # 一键初始化"
echo "═══════════════════════════════════════════════"

# 返回码
if [ "$FAIL" -gt 0 ]; then
    echo -e "\n${RED}${FAIL} 项错误${NC}"
    exit 1
elif [ "$WARN" -gt 0 ]; then
    echo -e "\n${YELLOW}${WARN} 项警告 (不影响核心构建)${NC}"
    exit 0
else
    echo -e "\n${GREEN}所有检查通过${NC}"
    exit 0
fi
