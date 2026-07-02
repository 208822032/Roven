# ============================================================================
#  kvstore build system
# ============================================================================
#  Quick start:
#    make doctor      # 检查环境
#    make setup       # 一键初始化 (vendored deps + libbpf + btf2header)
#    make             # 完整构建
#
#  Targets:
#    all             默认，构建全部 (含 eBPF，按 ENABLE_EBPF 控制)
#    kvstore         仅构建主服务二进制
#    ebpf            仅构建 eBPF 内核对象 + 用户态守护进程
#    tests           仅构建测试二进制
#    test            构建默认测试二进制
#    test-embedding  运行 ONNX embedding 语义召回检查（需服务已启动）
#    setup           初始化：vendored 依赖检查 + bootstrap
#    embedding       构建 ONNX embedding 版本
#    setup-embedding 显式下载 ONNX Runtime 和 bge-small-zh-v1.5 模型
#    deps            检查/提示缺失的系统包 (AUTO_INSTALL_DEPS=1 自动安装)
#    bootstrap       构建 libbpf + btf2header (项目内安装，无需 sudo)
#    doctor          全面环境诊断 (内核、Clang、库、BTF)
#    clean           清理编译产物
#    distclean       清理编译产物 + third_party
#
#  Options:
#    ENABLE_EBPF=0   跳过 eBPF 对象构建
#    ENABLE_ONNX_EMBEDDING=1  使用 ONNX Runtime C API embedding 空壳
#    ONNXRUNTIME_ROOT=/path/to/onnxruntime  ONNX Runtime 安装目录
#    ONNX_MODEL_DIR=models/bge-small-zh-v1.5  ONNX embedding 模型目录
#    AUTO_INSTALL_DEPS=1  配合 deps 目标自动 apt-get install (需 sudo)
#    SYNC_MODE=1     编译同步模式
#    OPT_CFLAGS=-O0  覆盖优化级别
# ============================================================================

# ── 编译工具链 ───────────────────────────────────────────────────────────
ifeq ($(origin CC), default)
CC = gcc
endif
ifeq ($(origin CXX), default)
CXX = g++
endif
CLANG   ?= $(shell which clang-11 2>/dev/null || which clang 2>/dev/null || echo clang)

# ── 项目根 & 目录 ────────────────────────────────────────────────────────
ROOT        := $(shell pwd)
SRC_CORE    := src/core
SRC_STORAGE := src/storage
SRC_PERSIST := src/persistence
SRC_NETWORK := src/network
SRC_DIST    := src/distributed
SRC_UTILS   := src/utils
SRC_VECTOR  := src/vector
SRC_EMBED   := src/embedding
TEST_DIR    := tests
BUILD_DIR   := build
BUILD_TESTS_BIN := $(BUILD_DIR)/tests/bin
BUILD_LIB   := $(BUILD_DIR)/lib
KVS_LOG_OBJS := $(SRC_UTILS)/kvs_log.o $(SRC_UTILS)/kvs_log_writer.o

# ── embedding runtime 选择 ─────────────────────────────────────────────
ENABLE_ONNX_EMBEDDING ?= 0
ONNXRUNTIME_ROOT ?= third_party/onnxruntime
ONNXRUNTIME_HEADER := $(ONNXRUNTIME_ROOT)/include/onnxruntime_c_api.h
ONNXRUNTIME_LIBDIR := $(ONNXRUNTIME_ROOT)/lib
ONNX_MODEL_DIR ?= models/bge-small-zh-v1.5
ONNX_MODEL_PATH ?= $(ONNX_MODEL_DIR)/model.onnx
ONNX_EMBEDDING_DIM ?= 512
ENABLE_FAISS ?= 0
FAISS_ROOT ?= third_party/faiss
FAISS_INCLUDEDIR := $(FAISS_ROOT)
FAISS_LIB := $(FAISS_ROOT)/build/faiss/libfaiss.a

ENABLE_HNSWLIB ?= 1
HNSWLIB_ROOT ?= third_party/hnswlib

ifeq ($(ENABLE_ONNX_EMBEDDING),1)
  EMBED_RUNTIME_SRC := $(SRC_EMBED)/embedding_runtime_onnx.c
  ONNX_CFLAGS := -I$(ONNXRUNTIME_ROOT)/include
  ONNX_LDFLAGS := -L$(ONNXRUNTIME_LIBDIR) -lonnxruntime -Wl,-rpath,$(ONNXRUNTIME_LIBDIR)
  ONNX_PREFLIGHT := check-onnx-runtime
else
  EMBED_RUNTIME_SRC := $(SRC_EMBED)/embedding_runtime.c
  ONNX_CFLAGS :=
  ONNX_LDFLAGS :=
  ONNX_PREFLIGHT :=
endif

# ── 第三方库 (项目内安装，端口无关) ─────────────────────────────────────
THIRD_PARTY := $(ROOT)/third_party
LIBBPF_DIR  := $(THIRD_PARTY)/libbpf
LIBBPF_INC  := $(LIBBPF_DIR)/include
LIBBPF_LIB  := $(LIBBPF_DIR)/lib

BOOTSTRAP_LIBBPF := $(LIBBPF_LIB)/libbpf.a

# btf2header 工具
TOOLS_DIR    := tools
BTF2HEADER_DIR  := $(TOOLS_DIR)/btf2header
BTF2HEADER_BIN  := $(BTF2HEADER_DIR)/btf2header

# ── jemalloc ─────────────────────────────────────────────────────────────
JEMALLOC_DIR     := third_party/jemalloc
JEMALLOC_INCLUDE := $(JEMALLOC_DIR)/include
JEMALLOC_LIB     := $(JEMALLOC_DIR)/lib/libjemalloc.a

# ── NtyCo 协程库 ─────────────────────────────────────────────────────────
NTYCO_DIR      := third_party/NtyCo
NTYCO_CORE_DIR := $(NTYCO_DIR)/core
NTYCO_LIB      := $(NTYCO_DIR)/libntyco.a
NTYCO_OBJS     := $(NTYCO_DIR)/objs/nty_socket.o \
                  $(NTYCO_DIR)/objs/nty_coroutine.o \
                  $(NTYCO_DIR)/objs/nty_epoll.o \
                  $(NTYCO_DIR)/objs/nty_schedule.o

# ── 源文件 ───────────────────────────────────────────────────────────────
HNSW_V2_SRCS = \
	$(SRC_VECTOR)/core/hnsw_v2/kvs_hnsw_v2_index.c \
	$(SRC_VECTOR)/core/hnsw_v2/kvs_hnsw_v2_storage.c \
	$(SRC_VECTOR)/core/hnsw_v2/kvs_hnsw_v2_context.c \
	$(SRC_VECTOR)/core/hnsw_v2/kvs_hnsw_v2_info.c \
	$(SRC_VECTOR)/core/hnsw_v2/kvs_hnsw_v2_random.c \
	$(SRC_VECTOR)/core/hnsw_v2/kvs_hnsw_v2_graph.c \
	$(SRC_VECTOR)/core/hnsw_v2/kvs_hnsw_v2_neighbor_select.c \
	$(SRC_VECTOR)/core/hnsw_v2/kvs_hnsw_v2_add.c \
	$(SRC_VECTOR)/core/hnsw_v2/kvs_hnsw_v2_search.c

BASE_SRCS = \
	$(SRC_CORE)/kvstore.c        $(SRC_CORE)/kvs_config.c        \
	$(SRC_CORE)/kvs_globals.c    $(SRC_CORE)/kvs_operation.c     \
	$(SRC_CORE)/kvs_command_task.c $(SRC_CORE)/kvs_response_task.c \
	$(SRC_CORE)/kvs_command_executor.c \
	$(SRC_CORE)/kvs_replication_fallback.c \
	$(SRC_CORE)/kvs_owner_lock.c \
	$(SRC_CORE)/kvs_embedding_worker_pool.c \
	$(SRC_CORE)/kvs_vcmd_profile.c \
	$(SRC_CORE)/kvs_ttl_background.c \
	$(SRC_STORAGE)/kvs_array.c   $(SRC_STORAGE)/kvs_rbtree.c     \
	$(SRC_STORAGE)/kvs_hash.c    $(SRC_STORAGE)/kvs_skiptable.c  \
	$(SRC_STORAGE)/kvs_mempool.c $(SRC_STORAGE)/jemalloc_wrapper.c \
	$(SRC_DIST)/distributed.c                                    \
	$(SRC_NETWORK)/resp_protocol.c $(SRC_NETWORK)/reactor.c       \
	$(SRC_NETWORK)/kvs_async_net_adapter.c                       \
	$(SRC_NETWORK)/proactor.c    $(SRC_NETWORK)/ntyco.c           \
	$(SRC_NETWORK)/net_session.c                                  \
	$(SRC_PERSIST)/kvs_rdb.c     $(SRC_PERSIST)/kvs_aof.c        \
	$(SRC_PERSIST)/kvs_aof_rewrite.c                              \
	$(SRC_UTILS)/msg_queue.c     $(SRC_UTILS)/master_slave_proto.c \
	$(SRC_UTILS)/kvs_log.c       $(SRC_UTILS)/kvs_log_writer.c   \
	$(SRC_UTILS)/netbuf.c        $(SRC_UTILS)/tx_batcher.c        \
	$(SRC_VECTOR)/kvs_vector.c   $(SRC_VECTOR)/kvs_vector_index.c \
	$(SRC_VECTOR)/kvs_vector_rebuild.c                            \
	$(SRC_VECTOR)/kvs_vector_rebuild_shadow.c                     \
	$(SRC_VECTOR)/kvs_vector_embedding.c                          \
	$(SRC_VECTOR)/kvs_vector_distance.c                           \
	$(SRC_VECTOR)/kvs_vector_distance_avx2.c                      \
	$(SRC_VECTOR)/core/kvs_vector_id_slot_map.c                   \
	$(SRC_VECTOR)/core/kvs_vector_heap.c                          \
	$(SRC_VECTOR)/core/flat/kvs_flat_index.c                      \
	$(SRC_VECTOR)/core/flat/kvs_flat_storage.c                    \
	$(SRC_VECTOR)/core/flat/kvs_flat_search.c                     \
	$(SRC_VECTOR)/core/flat/kvs_flat_info.c                       \
	$(SRC_VECTOR)/core/hnsw/kvs_hnsw_index.c                      \
	$(SRC_VECTOR)/core/hnsw/kvs_hnsw_graph.c                      \
	$(SRC_VECTOR)/core/hnsw/kvs_hnsw_context.c                    \
	$(SRC_VECTOR)/core/hnsw/kvs_hnsw_info.c                       \
	$(SRC_VECTOR)/core/hnsw/kvs_hnsw_random.c                     \
	$(SRC_VECTOR)/core/hnsw/kvs_hnsw_add.c                        \
	$(SRC_VECTOR)/core/hnsw/kvs_hnsw_search.c                     \
	$(SRC_VECTOR)/core/hnsw/kvs_hnsw_delete.c                     \
	$(HNSW_V2_SRCS)                                               \
	$(EMBED_RUNTIME_SRC)

SRCS = $(BASE_SRCS) $(SRC_DIST)/sync_backend_tcp.c
OBJS = $(SRCS:.c=.o)

# ── 编译标志 ─────────────────────────────────────────────────────────────
UNAME_M := $(shell uname -m)
AVX2_CFLAGS := $(if $(filter x86_64,$(UNAME_M)),-mavx2,)

EXTRA_CFLAGS ?=
OPT_CFLAGS   ?= -O2
CXXFLAGS      = -I ./include -I ./src -g -std=c++17
CFLAGS        = -I ./third_party/NtyCo/core/ -I ./include -I ./src \
                -I $(JEMALLOC_INCLUDE) \
                -DSYNC_MODE=$(SYNC_MODE) \
                -g $(OPT_CFLAGS) $(EXTRA_CFLAGS) $(ONNX_CFLAGS) $(FAISS_CFLAGS) $(HNSWLIB_CFLAGS)
LDFLAGS       = -L ./third_party/NtyCo/ -lntyco -lpthread -luring -ldl -lm \
                $(EXTRA_LIBS) $(JEMALLOC_LIB) $(ONNX_LDFLAGS) $(FAISS_LDFLAGS)
ifeq ($(ENABLE_FAISS),1)
  FAISS_CFLAGS := -DENABLE_FAISS_BASELINE -I$(FAISS_INCLUDEDIR)
  FAISS_CXXFLAGS = $(CXXFLAGS) -I$(FAISS_INCLUDEDIR)
  FAISS_LDFLAGS = $(FAISS_LIB) -fopenmp -lblas -llapack
  SRC_VECTOR_FAISS := $(SRC_VECTOR)/kvs_vector_faiss_ops.c $(SRC_VECTOR)/kvs_vector_faiss_backend.cpp
  OBJ_VECTOR_FAISS := $(SRC_VECTOR_FAISS:.c=.o)
  OBJ_VECTOR_FAISS := $(OBJ_VECTOR_FAISS:.cpp=.o)
else
  FAISS_CFLAGS :=
  FAISS_LDFLAGS :=
  SRC_VECTOR_FAISS :=
  OBJ_VECTOR_FAISS :=
endif

ifeq ($(ENABLE_HNSWLIB),1)
  HNSWLIB_CFLAGS := -DENABLE_HNSWLIB -I$(HNSWLIB_ROOT)
  SRC_VECTOR_HNSWLIB := $(SRC_VECTOR)/kvs_vector_hnswlib_ops.c $(SRC_VECTOR)/kvs_vector_hnswlib_backend.cpp
  OBJ_VECTOR_HNSWLIB := $(SRC_VECTOR_HNSWLIB:.c=.o)
  OBJ_VECTOR_HNSWLIB := $(OBJ_VECTOR_HNSWLIB:.cpp=.o)
else
  HNSWLIB_CFLAGS :=
  SRC_VECTOR_HNSWLIB :=
  OBJ_VECTOR_HNSWLIB :=
endif
VECTOR_OPTIONAL_OBJS := \
	$(SRC_VECTOR)/kvs_vector_faiss_ops.o \
	$(SRC_VECTOR)/kvs_vector_faiss_backend.o \
	$(SRC_VECTOR)/kvs_vector_hnswlib_ops.o \
	$(SRC_VECTOR)/kvs_vector_hnswlib_backend.o
KVSTORE_TEST_LDFLAGS = -L$(BUILD_LIB) -lkvstore $(LDFLAGS)

# ── eBPF 配置 ────────────────────────────────────────────────────────────
ENABLE_EBPF ?= 1

EBPF_DIR       := src/eBPF
EBPF_KERN_OBJ  := $(EBPF_DIR)/ebpf_kern.o
EBPF_SYNCD_BIN := $(EBPF_DIR)/ebpf_syncd
VMLINUX_H      := $(EBPF_DIR)/vmlinux.h
VMLINUX_KVER   := $(EBPF_DIR)/vmlinux.h.kver
CUR_KVER       := $(shell uname -r)
KVER_FILE      := /sys/kernel/btf/vmlinux

BPF_ARCH_MACRO := \
	$(if $(filter x86_64,$(UNAME_M)),-D__TARGET_ARCH_x86, \
	$(if $(filter aarch64,$(UNAME_M)),-D__TARGET_ARCH_arm64,-D__TARGET_ARCH_x86))

BPF_INCLUDES = -I $(EBPF_DIR) -I $(LIBBPF_INC) -I $(LIBBPF_INC)/uapi
BPF_CFLAGS   = -O2 -g -target bpf $(BPF_ARCH_MACRO) $(BPF_INCLUDES)

# eBPF 目标列表 (由 ENABLE_EBPF 控制是否加入 all)
ifeq ($(ENABLE_EBPF),1)
  EBPF_TARGETS := $(EBPF_KERN_OBJ) $(EBPF_SYNCD_BIN)
else
  EBPF_TARGETS :=
endif

# ── 测试目标 ─────────────────────────────────────────────────────────────
TEST_CASE               := $(BUILD_TESTS_BIN)/testcase
TEST_WRITE_QPS          := $(BUILD_TESTS_BIN)/write_qps_test
TEST_TTL_QPS            := $(BUILD_TESTS_BIN)/write_ttl_qps
TEST_TTL_EXPIRED_PATH   := $(BUILD_TESTS_BIN)/ttl_expired_path_bench
TEST_KVSTORE_CLIENT     := $(BUILD_TESTS_BIN)/kvstore_client
TEST_MEMPOOL            := $(BUILD_TESTS_BIN)/mempool_test
TEST_PAPER              := $(BUILD_TESTS_BIN)/test_paper
TEST_PAPER_SYNC         := $(BUILD_TESTS_BIN)/test_paper_sync
TEST_BIGVALUE           := $(BUILD_TESTS_BIN)/test_bigvalue
TEST_AI_CHAT_SCHEMA     := $(BUILD_TESTS_BIN)/ai_chat_schema_test
TEST_VECTOR             := $(BUILD_TESTS_BIN)/vector_testcase
TEST_TTL_LOOKUP_SPLIT   := $(BUILD_TESTS_BIN)/ttl_lookup_split_bench
TEST_SKIPTABLE_TARGETED := $(BUILD_TESTS_BIN)/skiptable_targeted_bench
TEST_VECTOR_IMPORT_BENCH := $(BUILD_TESTS_BIN)/vector_import_bench
TEST_VECTOR_BENCHMARK   := $(BUILD_TESTS_BIN)/vector_benchmark
TEST_BINARY_KEY         := $(BUILD_TESTS_BIN)/test_binary_key
TEST_VCMD_PROFILE_BENCH := $(BUILD_TESTS_BIN)/vcmd_profile_bench
TEST_HNSW_V2_SKELETON := $(BUILD_TESTS_BIN)/vector_core_hnsw_v2_skeleton_test
TEST_HNSW_V2_GRAPH := $(BUILD_TESTS_BIN)/vector_core_hnsw_v2_graph_construction_test
TEST_HNSW_V2_SEARCH := $(BUILD_TESTS_BIN)/vector_core_hnsw_v2_search_recall_test
TEST_HNSW_V2_PROFILE := $(BUILD_TESTS_BIN)/vector_core_hnsw_v2_performance_profile_test
TEST_HNSW_V2_DISTANCE_PROFILE := $(BUILD_TESTS_BIN)/vector_core_hnsw_v2_distance_profile_test
TEST_HNSW_V2_REVERSE_PRUNE_PROFILE := $(BUILD_TESTS_BIN)/vector_core_hnsw_v2_reverse_prune_profile_test
TEST_HNSW_V2_TOMBSTONE := $(BUILD_TESTS_BIN)/vector_core_hnsw_v2_tombstone_test
TEST_VECTOR_REBUILD_STATE_MACHINE := $(BUILD_TESTS_BIN)/vector_rebuild_state_machine_test
TEST_VECTOR_REBUILD_DELTA_CAPTURE := $(BUILD_TESTS_BIN)/vector_rebuild_delta_capture_test
TEST_VECTOR_REBUILD_SHADOW_REPLAY := $(BUILD_TESTS_BIN)/vector_rebuild_shadow_replay_test
TEST_VECTOR_REBUILD_SHADOW_SWAP := $(BUILD_TESTS_BIN)/vector_rebuild_shadow_swap_test
TEST_FAISS_BASELINE     := $(BUILD_TESTS_BIN)/faiss_baseline
TEST_VCMD_ONNX_FAISS_BENCH := $(BUILD_TESTS_BIN)/vcmd_onnx_faiss_bench
TEST_VCMD_EMBEDDING_CACHE_BENCH := $(BUILD_TESTS_BIN)/vcmd_embedding_cache_bench

TEST_BINARIES = \
	$(TEST_CASE) $(TEST_WRITE_QPS) $(TEST_TTL_QPS) \
	$(TEST_TTL_EXPIRED_PATH) $(TEST_KVSTORE_CLIENT) \
	$(TEST_MEMPOOL) $(TEST_PAPER) $(TEST_PAPER_SYNC) \
	$(TEST_BIGVALUE) $(TEST_AI_CHAT_SCHEMA) $(TEST_VECTOR) \
	$(TEST_VECTOR_IMPORT_BENCH) $(TEST_VECTOR_BENCHMARK) \
	$(TEST_TTL_LOOKUP_SPLIT) $(TEST_SKIPTABLE_TARGETED) \
	$(TEST_BINARY_KEY) $(TEST_VCMD_PROFILE_BENCH) \
	$(TEST_HNSW_V2_SKELETON) $(TEST_HNSW_V2_GRAPH) \
	$(TEST_HNSW_V2_SEARCH) $(TEST_HNSW_V2_PROFILE) \
	$(TEST_HNSW_V2_DISTANCE_PROFILE) $(TEST_HNSW_V2_REVERSE_PRUNE_PROFILE) \
	$(TEST_HNSW_V2_TOMBSTONE) \
	$(TEST_VECTOR_REBUILD_STATE_MACHINE) $(TEST_VECTOR_REBUILD_DELTA_CAPTURE) \
	$(TEST_VECTOR_REBUILD_SHADOW_REPLAY) $(TEST_VECTOR_REBUILD_SHADOW_SWAP)

ifeq ($(ENABLE_FAISS),1)
TEST_BINARIES += $(TEST_FAISS_BASELINE) $(TEST_VCMD_ONNX_FAISS_BENCH) $(TEST_VCMD_EMBEDDING_CACHE_BENCH)
endif

TARGET := kvstore

# ╔══════════════════════════════════════════════════════════════════════════╗
# ║                           顶层目标                                     ║
# ╚══════════════════════════════════════════════════════════════════════════╝

# 禁用 GNU Make 内置隐式规则，防止二进制目标（如 kvstore）被自动
# 推断为 %: %.o 而产生循环依赖。所有规则均由本 Makefile 显式定义。
%: %.o
	@echo '  (忽略隐式链接规则 $@)'

.PHONY: all setup deps bootstrap doctor clean distclean
.PHONY: main ebpf tests test check test-embedding help
.PHONY: embedding setup-embedding setup-onnx check-embedding-assets
.PHONY: vcmd_profile_bench faiss_baseline vcmd_onnx_faiss_bench vcmd_embedding_cache_bench

# 默认：完整构建
all: $(ONNX_PREFLIGHT) $(NTYCO_LIB) $(TARGET) $(TEST_BINARIES) $(EBPF_TARGETS)
	@echo ""
	@echo "  ✓ 构建完成"
	@echo "    二进制: $(TARGET) $(if $(filter 1,$(ENABLE_EBPF)),$(EBPF_SYNCD_BIN),)"
	@if [ "$(ENABLE_EBPF)" = "1" ]; then \
		echo "    eBPF:   $(EBPF_KERN_OBJ)"; \
	else \
		echo "    eBPF:   (跳过, ENABLE_EBPF=0)"; \
	fi

# 仅主二进制
main: $(ONNX_PREFLIGHT) $(NTYCO_LIB) $(TARGET)

# 仅测试
tests: $(NTYCO_LIB) $(TEST_BINARIES)

# 快捷目标：分析用 benchmark
vcmd_profile_bench: $(TEST_VCMD_PROFILE_BENCH)

faiss_baseline: $(TEST_FAISS_BASELINE)

vcmd_onnx_faiss_bench: $(TEST_VCMD_ONNX_FAISS_BENCH)

vcmd_embedding_cache_bench: $(TEST_VCMD_EMBEDDING_CACHE_BENCH)

test: tests
	@echo ""
	@echo "  ✓ 默认测试二进制已构建"
	@echo "    示例: ./build/tests/bin/vector_testcase 127.0.0.1 9300"

# 轻量自检目标 — 仅运行不依赖后台服务的测试
check: all
	@echo ""
	@echo "  check: 运行离线单元测试..."
	@./build/tests/bin/mempool_test
	@echo "  mempool_test: PASS"
	@./build/tests/bin/ai_chat_schema_test
	@echo "  ai_chat_schema_test: PASS"
	@echo "  check: 所有离线测试通过"

test-embedding:
	@echo "ONNX embedding semantic check"
	@echo "Please start kvstore first:"
	@echo "  ./kvstore -c config/kvstore-embedding.conf"
	@echo ""
	@echo "Then run:"
	@echo "  bash tests/vector/semantic_onnx_test.sh 127.0.0.1 28080"
	@echo ""
	@bash tests/vector/semantic_onnx_test.sh 127.0.0.1 28080

# 仅 eBPF
ebpf: $(EBPF_TARGETS)

# ONNX embedding 版本：显式检查环境，然后用 ONNX runtime 构建。
embedding: check-embedding-assets
	@$(MAKE) ENABLE_ONNX_EMBEDDING=1 ONNXRUNTIME_ROOT=$(ONNXRUNTIME_ROOT) all

# 帮助
help:
	@echo "用法: make [目标] [选项]"
	@echo ""
	@echo "构建目标:"
	@echo "  all         完整构建 (默认)"
	@echo "  main        仅构建主服务 kvstore"
	@echo "  tests       仅构建测试二进制"
	@echo "  test        构建默认测试二进制"
	@echo "  check       构建全部并运行离线单元测试"
	@echo "  test-embedding  运行 ONNX embedding 语义召回检查（需服务已启动）"
	@echo "  ebpf        仅构建 eBPF 对象"
	@echo "  embedding   构建 ONNX embedding 版本"
	@echo ""
	@echo "环境目标:"
	@echo "  setup       一键初始化 (submodules → bootstrap)"
	@echo "  setup-embedding  下载 ONNX Runtime 和 bge-small-zh-v1.5 模型"
	@echo "  setup-onnx       setup-embedding 的兼容别名"
	@echo "  deps        检查系统依赖 (AUTO_INSTALL_DEPS=1 自动安装)"
	@echo "  bootstrap   构建项目内 libbpf + btf2header"
	@echo "  doctor      全面环境诊断"
	@echo ""
	@echo "清理目标:"
	@echo "  clean       清理编译产物"
	@echo "  distclean   清理编译产物 + third_party"
	@echo ""
	@echo "选项:"
	@echo "  ENABLE_EBPF=0        跳过 eBPF 构建"
	@echo "  ENABLE_ONNX_EMBEDDING=1  编译 ONNX Runtime embedding 空壳"
	@echo "  ONNXRUNTIME_ROOT=/path/to/onnxruntime  指定 ONNX Runtime 根目录"
	@echo "  ONNX_MODEL_DIR=models/bge-small-zh-v1.5  指定 ONNX embedding 模型目录"
	@echo "  AUTO_INSTALL_DEPS=1  deps 时自动安装系统包 (需 sudo)"
	@echo "  SYNC_MODE=1          同步模式编译"

# ── 一键初始化 ──────────────────────────────────────────────────────────
setup: deps _init_submodules bootstrap
	@echo ""
	@echo "  ✓ 环境初始化完成，现在可以执行 make"

setup-embedding:
	@bash scripts/build/setup_onnx_embedding.sh

setup-onnx: setup-embedding

_init_submodules:
	@if [ ! -d third_party/NtyCo/core ]; then \
		echo "[setup] NtyCo vendored source missing: third_party/NtyCo/"; \
		echo "[setup] Run: cp -a <NtyCo backup> third_party/NtyCo/"; \
		exit 1; \
	fi
	@if [ ! -f $(JEMALLOC_LIB) ]; then \
		echo "[setup] 构建 jemalloc..."; \
		cd $(JEMALLOC_DIR) && ./autogen.sh 2>/dev/null || true; \
		cd $(JEMALLOC_DIR) && ./configure --prefix=$(ROOT)/$(JEMALLOC_DIR) \
			--enable-static --disable-shared 2>&1 | tail -1; \
		$(MAKE) -C $(JEMALLOC_DIR) -j$$(nproc) 2>&1 | tail -1; \
		$(MAKE) -C $(JEMALLOC_DIR) install_lib 2>&1 | tail -1; \
	fi

# ── bootstrap: 构建项目内工具链 ────────────────────────────────────────
bootstrap: $(BOOTSTRAP_LIBBPF) $(BTF2HEADER_BIN)
	@if [ "$(ENABLE_EBPF)" = "1" ]; then \
		echo "[bootstrap] ✓ libbpf + btf2header 就绪"; \
	else \
		echo "[bootstrap] ✓ libbpf + btf2header 就绪 (eBPF 禁用，跳过)"; \
	fi

# ── 系统依赖 ────────────────────────────────────────────────────────────
deps:
	@scripts/build/install_deps.sh

# ── 全面诊断 ────────────────────────────────────────────────────────────
doctor:
	@scripts/dev/doctor.sh

# ╔══════════════════════════════════════════════════════════════════════════╗
# ║                       libbpf bootstrap                                 ║
# ╚══════════════════════════════════════════════════════════════════════════╝

LIBBPF_VER := v1.0.3
LIBBPF_URL := https://github.com/libbpf/libbpf.git

$(BOOTSTRAP_LIBBPF):
	@echo "[bootstrap] 构建 libbpf $(LIBBPF_VER) → $(LIBBPF_DIR)"
	@scripts/build/bootstrap_libbpf.sh $(LIBBPF_DIR) $(LIBBPF_VER) $(LIBBPF_URL)

# ╔══════════════════════════════════════════════════════════════════════════╗
# ║                       btf2header 工具                                 ║
# ╚══════════════════════════════════════════════════════════════════════════╝

$(BTF2HEADER_BIN): $(BOOTSTRAP_LIBBPF)
	@echo "[bootstrap] 构建 btf2header"
	@$(MAKE) -C $(BTF2HEADER_DIR) \
		LIBBPF_INC="$(LIBBPF_INC)" \
		LIBBPF_LIB="$(LIBBPF_LIB)" \
		CC="$(CC)"

# ╔══════════════════════════════════════════════════════════════════════════╗
# ║                       NtyCo 协程库                                    ║
# ╚══════════════════════════════════════════════════════════════════════════╝

$(NTYCO_DIR)/objs:
	mkdir -p $@

$(NTYCO_DIR)/objs/%.o: $(NTYCO_CORE_DIR)/%.c | $(NTYCO_DIR)/objs
	$(CC) -c $(CFLAGS) -o $@ $< -I $(NTYCO_CORE_DIR) -O3 -lpthread -ldl

$(NTYCO_LIB): $(NTYCO_OBJS)
	ar rcs $@ $^

# ── kvstore 核心库（供 test/benchmark 链接，排除 main） ──────────────────
# build/lib/libkvstore.a is an internal static library used by tests and benchmarks.
# It excludes the kvstore main object and is not a stable public SDK artifact.
LIBKVSTORE_OBJS := $(filter-out $(SRC_CORE)/kvstore.o, $(OBJS))
# Optional C++ vector backend objects are linked only into kvstore, not this
# C-facing test library, to avoid forcing C++ linkage on all C test binaries.
LIBKVSTORE      := $(BUILD_LIB)/libkvstore.a

$(LIBKVSTORE): $(LIBKVSTORE_OBJS)
	@mkdir -p $(BUILD_LIB)
	ar rcs $@ $^

# ╔══════════════════════════════════════════════════════════════════════════╗
# ║                       主二进制 kvstore                                ║
# ╚══════════════════════════════════════════════════════════════════════════╝

$(TARGET): $(OBJS) $(NTYCO_LIB) $(OBJ_VECTOR_FAISS) $(OBJ_VECTOR_HNSWLIB)
	$(if $(OBJ_VECTOR_FAISS)$(OBJ_VECTOR_HNSWLIB),$(CXX),$(CC)) -o $@ $(OBJS) $(OBJ_VECTOR_FAISS) $(OBJ_VECTOR_HNSWLIB) $(LDFLAGS) -lpthread

$(SRC_VECTOR)/kvs_vector_faiss_backend.o: $(SRC_VECTOR)/kvs_vector_faiss_backend.cpp
	$(CXX) -c $(FAISS_CXXFLAGS) -o $@ $<

$(SRC_VECTOR)/kvs_vector_hnswlib_backend.o: $(SRC_VECTOR)/kvs_vector_hnswlib_backend.cpp
	$(CXX) -c -std=c++11 -I ./include -I ./src -I$(HNSWLIB_ROOT) -o $@ $<

# ╔══════════════════════════════════════════════════════════════════════════╗
# ║                       测试二进制                                      ║
# ╚══════════════════════════════════════════════════════════════════════════╝

# 输出目录：所有测试二进制统一进入 build/tests/bin/
# 库依赖：需要链接生产 .o 的测试走 libkvstore.a，不再直接引用 src/*.o

$(BUILD_TESTS_BIN):
	@mkdir -p $@

# ── 简单客户端测试 (仅自源码，不依赖 libkvstore) ────────────────────────

$(TEST_CASE): $(TEST_DIR)/testcase.c | $(BUILD_TESTS_BIN)
	$(CC) $(CFLAGS) -o $@ $<

$(TEST_WRITE_QPS): $(TEST_DIR)/write_qps_test.c | $(BUILD_TESTS_BIN)
	$(CC) $(CFLAGS) -o $@ $<

$(TEST_TTL_QPS): $(TEST_DIR)/write_ttl_qps_test.c | $(BUILD_TESTS_BIN)
	$(CC) $(CFLAGS) -o $@ $<

$(TEST_TTL_EXPIRED_PATH): $(TEST_DIR)/ttl_expired_path_bench.c | $(BUILD_TESTS_BIN)
	$(CC) $(CFLAGS) -o $@ $<

$(TEST_KVSTORE_CLIENT): $(TEST_DIR)/kvstore_client.c | $(BUILD_TESTS_BIN)
	$(CC) $(CFLAGS) -o $@ $< -lreadline

$(TEST_PAPER): $(TEST_DIR)/test_paper.c | $(BUILD_TESTS_BIN)
	$(CC) $(CFLAGS) -o $@ $<

$(TEST_PAPER_SYNC): $(TEST_DIR)/test_paper_sync.c $(NTYCO_LIB) | $(BUILD_TESTS_BIN)
	$(CC) $(CFLAGS) -o $@ $< $(LDFLAGS) -lpthread

$(TEST_BIGVALUE): $(TEST_DIR)/test_bigvalue.c $(NTYCO_LIB) | $(BUILD_TESTS_BIN)
	$(CC) $(CFLAGS) -o $@ $< $(LDFLAGS) -lpthread

$(TEST_VECTOR): $(TEST_DIR)/vector_testcase.c | $(BUILD_TESTS_BIN)
	$(CC) $(CFLAGS) -o $@ $<

$(TEST_VECTOR_IMPORT_BENCH): $(TEST_DIR)/vector_import_bench.c | $(BUILD_TESTS_BIN)
	$(CC) $(CFLAGS) -o $@ $<

$(TEST_VECTOR_BENCHMARK): $(TEST_DIR)/vector_benchmark.c | $(BUILD_TESTS_BIN)
	$(CC) $(CFLAGS) -o $@ $< -lm

# ── 内核链接测试 (通过 libkvstore.a，不直接链接 src/*.o) ────────────────

$(TEST_TTL_LOOKUP_SPLIT): $(TEST_DIR)/ttl_lookup_split_bench.c $(LIBKVSTORE) $(NTYCO_LIB) | $(BUILD_TESTS_BIN)
	$(CC) $(CFLAGS) -o $@ $< $(KVSTORE_TEST_LDFLAGS)

$(TEST_SKIPTABLE_TARGETED): $(TEST_DIR)/skiptable_targeted_bench.c $(LIBKVSTORE) $(NTYCO_LIB) | $(BUILD_TESTS_BIN)
	$(CC) $(CFLAGS) -o $@ $< $(KVSTORE_TEST_LDFLAGS)

$(TEST_MEMPOOL): $(TEST_DIR)/mempool_test.c $(LIBKVSTORE) $(NTYCO_LIB) | $(BUILD_TESTS_BIN)
	$(CC) $(CFLAGS) -o $@ $< $(KVSTORE_TEST_LDFLAGS)

$(TEST_AI_CHAT_SCHEMA): $(TEST_DIR)/ai_chat_schema_test.c $(LIBKVSTORE) $(NTYCO_LIB) | $(BUILD_TESTS_BIN)
	$(CC) $(CFLAGS) -o $@ $< $(KVSTORE_TEST_LDFLAGS)

$(TEST_BINARY_KEY): $(TEST_DIR)/test_binary_key.c | $(BUILD_TESTS_BIN)
	$(CC) $(CFLAGS) -o $@ $<

$(TEST_VCMD_PROFILE_BENCH): $(TEST_DIR)/vcmd_profile_bench.c $(LIBKVSTORE) $(NTYCO_LIB) | $(BUILD_TESTS_BIN)
	$(CC) $(CFLAGS) -o $@ $< $(KVSTORE_TEST_LDFLAGS) -ldl

$(TEST_HNSW_V2_SKELETON): \
	$(TEST_DIR)/vector_core_hnsw_v2_skeleton_test.c \
	$(LIBKVSTORE) $(NTYCO_LIB) | $(BUILD_TESTS_BIN)
	$(CC) $(CFLAGS) -o $@ \
		$(TEST_DIR)/vector_core_hnsw_v2_skeleton_test.c \
		$(KVSTORE_TEST_LDFLAGS)

$(TEST_HNSW_V2_GRAPH): \
	$(TEST_DIR)/vector_core_hnsw_v2_graph_construction_test.c \
	$(LIBKVSTORE) $(NTYCO_LIB) | $(BUILD_TESTS_BIN)
	$(CC) $(CFLAGS) -o $@ \
		$(TEST_DIR)/vector_core_hnsw_v2_graph_construction_test.c \
		$(KVSTORE_TEST_LDFLAGS)

$(TEST_HNSW_V2_SEARCH): \
	$(TEST_DIR)/vector_core_hnsw_v2_search_recall_test.c \
	$(LIBKVSTORE) $(NTYCO_LIB) | $(BUILD_TESTS_BIN)
	$(CC) $(CFLAGS) -o $@ \
		$(TEST_DIR)/vector_core_hnsw_v2_search_recall_test.c \
		$(KVSTORE_TEST_LDFLAGS)

$(TEST_HNSW_V2_PROFILE): \
	$(TEST_DIR)/vector_core_hnsw_v2_performance_profile_test.c \
	$(LIBKVSTORE) $(NTYCO_LIB) | $(BUILD_TESTS_BIN)
	$(CC) $(CFLAGS) -o $@ \
		$(TEST_DIR)/vector_core_hnsw_v2_performance_profile_test.c \
		$(KVSTORE_TEST_LDFLAGS)

$(TEST_HNSW_V2_DISTANCE_PROFILE): \
	$(TEST_DIR)/vector_core_hnsw_v2_distance_profile_test.c \
	$(LIBKVSTORE) $(NTYCO_LIB) | $(BUILD_TESTS_BIN)
	$(CC) $(CFLAGS) -o $@ \
		$(TEST_DIR)/vector_core_hnsw_v2_distance_profile_test.c \
		$(KVSTORE_TEST_LDFLAGS)

$(TEST_HNSW_V2_REVERSE_PRUNE_PROFILE): \
	$(TEST_DIR)/vector_core_hnsw_v2_reverse_prune_profile_test.c \
	$(LIBKVSTORE) $(NTYCO_LIB) | $(BUILD_TESTS_BIN)
	$(CC) $(CFLAGS) -o $@ \
		$(TEST_DIR)/vector_core_hnsw_v2_reverse_prune_profile_test.c \
		$(KVSTORE_TEST_LDFLAGS)

$(TEST_HNSW_V2_TOMBSTONE): \
	$(TEST_DIR)/vector_core_hnsw_v2_tombstone_test.c \
	$(LIBKVSTORE) $(NTYCO_LIB) | $(BUILD_TESTS_BIN)
	$(CC) $(CFLAGS) -o $@ \
		$(TEST_DIR)/vector_core_hnsw_v2_tombstone_test.c \
		$(KVSTORE_TEST_LDFLAGS)

$(TEST_VECTOR_REBUILD_STATE_MACHINE): \
	$(TEST_DIR)/vector_rebuild_state_machine_test.c \
	$(LIBKVSTORE) $(NTYCO_LIB) | $(BUILD_TESTS_BIN)
	$(CC) $(CFLAGS) -o $@ \
		$(TEST_DIR)/vector_rebuild_state_machine_test.c \
		$(KVSTORE_TEST_LDFLAGS)

$(TEST_VECTOR_REBUILD_DELTA_CAPTURE): \
	$(TEST_DIR)/vector_rebuild_delta_capture_test.c \
	$(LIBKVSTORE) $(NTYCO_LIB) | $(BUILD_TESTS_BIN)
	$(CC) $(CFLAGS) -o $@ \
		$(TEST_DIR)/vector_rebuild_delta_capture_test.c \
		$(KVSTORE_TEST_LDFLAGS)

$(TEST_VECTOR_REBUILD_SHADOW_REPLAY): \
	$(TEST_DIR)/vector_rebuild_shadow_replay_test.c \
	$(LIBKVSTORE) $(NTYCO_LIB) | $(BUILD_TESTS_BIN)
	$(CC) $(CFLAGS) -o $@ \
		$(TEST_DIR)/vector_rebuild_shadow_replay_test.c \
		$(KVSTORE_TEST_LDFLAGS)

$(TEST_VECTOR_REBUILD_SHADOW_SWAP): \
	$(TEST_DIR)/vector_rebuild_shadow_swap_test.c \
	$(LIBKVSTORE) $(NTYCO_LIB) | $(BUILD_TESTS_BIN)
	$(CC) $(CFLAGS) -o $@ \
		$(TEST_DIR)/vector_rebuild_shadow_swap_test.c \
		$(KVSTORE_TEST_LDFLAGS)

$(TEST_FAISS_BASELINE): $(TEST_DIR)/faiss_baseline.cpp | $(BUILD_TESTS_BIN)
	$(CXX) $(FAISS_CXXFLAGS) -O3 -o $@ $< $(FAISS_LDFLAGS) -lpthread -lm

$(TEST_VCMD_ONNX_FAISS_BENCH): $(TEST_DIR)/vcmd_onnx_faiss_bench.cpp $(LIBKVSTORE) $(NTYCO_LIB) | $(BUILD_TESTS_BIN)
	$(CXX) $(FAISS_CXXFLAGS) -O2 -o $@ $< -L$(BUILD_LIB) -lkvstore $(FAISS_LDFLAGS) $(LDFLAGS) -ldl -lpthread -lm

$(TEST_VCMD_EMBEDDING_CACHE_BENCH): $(TEST_DIR)/vcmd_onnx_faiss_bench.cpp $(LIBKVSTORE) $(NTYCO_LIB) | $(BUILD_TESTS_BIN)
	$(CXX) $(FAISS_CXXFLAGS) -O2 -o $@ $< -L$(BUILD_LIB) -lkvstore $(FAISS_LDFLAGS) $(LDFLAGS) -ldl -lpthread -lm

# ╔══════════════════════════════════════════════════════════════════════════╗
# ║                     eBPF 构建 (ENABLE_EBPF=1)                         ║
# ╚══════════════════════════════════════════════════════════════════════════╝

# vmlinux.h 生成规则：
#   - 检查/生成仅在 ENABLE_EBPF=1 且 vmlinux.h 不存在/过期时触发
#   - 通过 vmlinux.h.kver 文件记录生成时的内核版本，换内核后自动重新生成
$(VMLINUX_KVER): FORCE
	@if [ "$(ENABLE_EBPF)" != "1" ]; then \
		touch $@; \
	elif [ ! -f $@ ] || [ "$$(cat $@ 2>/dev/null)" != "$(CUR_KVER)" ] || [ ! -s $(VMLINUX_H) ]; then \
		echo "  [ebpf] 生成 vmlinux.h (kernel $(CUR_KVER))..."; \
		if [ ! -f $(KVER_FILE) ]; then \
			echo "  [ebpf] 错误: $(KVER_FILE) 不存在，需要 CONFIG_DEBUG_INFO_BTF=y"; \
			exit 1; \
		fi; \
		if [ ! -x $(BTF2HEADER_BIN) ]; then \
			echo "  [ebpf] 错误: btf2header 未构建，先执行 make bootstrap"; \
			exit 1; \
		fi; \
		$(BTF2HEADER_BIN) $(KVER_FILE) > $(VMLINUX_H).tmp; \
		if [ $$? -eq 0 ] && [ -s $(VMLINUX_H).tmp ]; then \
			mv $(VMLINUX_H).tmp $(VMLINUX_H); \
			echo "$(CUR_KVER)" > $@; \
			echo "  [ebpf] vmlinux.h 已生成 ($$(wc -l < $(VMLINUX_H)) 行)"; \
		else \
			rm -f $(VMLINUX_H).tmp; \
			echo "  [ebpf] 错误: vmlinux.h 生成失败"; \
			exit 1; \
		fi; \
	else \
		touch $@; \
	fi

$(VMLINUX_H): $(VMLINUX_KVER)

# eBPF 内核对象
$(EBPF_KERN_OBJ): $(EBPF_DIR)/ebpf_kern.c $(VMLINUX_H)
	$(CLANG) $(BPF_CFLAGS) -c $< -o $@

# eBPF 用户态守护进程
EBPF_SYNCD_SRCS = $(EBPF_DIR)/ebpf_syncd.c \
	$(SRC_NETWORK)/resp_protocol.c $(SRC_UTILS)/msg_queue.c \
	$(SRC_STORAGE)/kvs_mempool.c $(KVS_LOG_OBJS:.o=.c) \
	$(SRC_STORAGE)/jemalloc_wrapper.c $(SRC_STORAGE)/kvs_array.c \
	$(SRC_STORAGE)/kvs_hash.c $(SRC_STORAGE)/kvs_rbtree.c \
	$(SRC_STORAGE)/kvs_skiptable.c $(SRC_CORE)/kvs_config.c \
	$(SRC_UTILS)/tx_batcher.c $(SRC_CORE)/kvs_globals.c

$(EBPF_SYNCD_BIN): $(BOOTSTRAP_LIBBPF) $(EBPF_SYNCD_SRCS)
	$(CC) -O2 -g $(EBPF_SYNCD_SRCS) -o $@ \
		-I $(LIBBPF_INC) -I ./include \
		-L $(LIBBPF_LIB) -lbpf -lelf -lz \
		-Wl,-rpath,$(LIBBPF_LIB) \
		-lpthread $(JEMALLOC_LIB) -ldl

# ╔══════════════════════════════════════════════════════════════════════════╗
# ║                        通用编译规则                                   ║
# ╚══════════════════════════════════════════════════════════════════════════╝

# AVX2 kernel — 必须在通用 %.o 规则之前
src/vector/kvs_vector_distance_avx2.o: src/vector/kvs_vector_distance_avx2.c
	$(CC) $(CFLAGS) $(AVX2_CFLAGS) -c $< -o $@

$(HNSW_V2_SRCS:.c=.o): include/vector/kvs_vector_hnsw_v2.h \
	$(SRC_VECTOR)/core/hnsw_v2/kvs_hnsw_v2_heap.h \
	$(SRC_VECTOR)/core/hnsw_v2/kvs_hnsw_v2_score_cache.h

$(SRC_VECTOR)/kvs_vector.o \
$(SRC_VECTOR)/kvs_vector_embedding.o \
$(SRC_VECTOR)/kvs_vector_index.o \
$(SRC_VECTOR)/kvs_vector_rebuild.o \
$(SRC_VECTOR)/kvs_vector_rebuild_shadow.o: \
	include/vector/kvs_vector_manager.h \
	include/vector/kvs_vector_rebuild.h \
	include/vector/kvs_vector_rebuild_shadow.h

check-onnx-runtime:
	@if [ ! -f "$(ONNXRUNTIME_HEADER)" ]; then \
		echo "ONNX Runtime not found. Please run:"; \
		echo "  make setup-embedding"; \
		echo "Expected header: $(ONNXRUNTIME_HEADER)"; \
		exit 1; \
	fi
	@if [ ! -f "$(ONNXRUNTIME_LIBDIR)/libonnxruntime.so" ]; then \
		echo "ONNX Runtime library not found. Please run:"; \
		echo "  make setup-embedding"; \
		echo "Expected library: $(ONNXRUNTIME_LIBDIR)/libonnxruntime.so"; \
		exit 1; \
	fi

check-embedding-assets:
	@if [ ! -f "$(ONNXRUNTIME_HEADER)" ] || \
	    [ ! -f "$(ONNXRUNTIME_LIBDIR)/libonnxruntime.so" ] || \
	    [ ! -f "$(ONNX_MODEL_PATH)" ] || \
	    [ ! -f "$(ONNX_MODEL_DIR)/model.onnx_data" ] || \
	    [ ! -f "$(ONNX_MODEL_DIR)/vocab.txt" ]; then \
		echo "Embedding assets are missing."; \
		echo "Please run:"; \
		echo "  make setup-embedding"; \
		echo ""; \
		echo "Expected files:"; \
		echo "  $(ONNXRUNTIME_HEADER)"; \
		echo "  $(ONNXRUNTIME_LIBDIR)/libonnxruntime.so"; \
		echo "  $(ONNX_MODEL_PATH)"; \
		echo "  $(ONNX_MODEL_DIR)/model.onnx_data"; \
		echo "  $(ONNX_MODEL_DIR)/vocab.txt"; \
		exit 1; \
	fi

$(SRC_EMBED)/embedding_runtime_onnx.o: check-onnx-runtime

%.o: %.c
	$(CC) $(CFLAGS) -c $< -o $@

%.o: %.cc
	$(CXX) $(CXXFLAGS) -c $< -o $@

# ╔══════════════════════════════════════════════════════════════════════════╗
# ║                           清理                                        ║
# ╚══════════════════════════════════════════════════════════════════════════╝

clean:
	rm -f $(OBJS) $(VECTOR_OPTIONAL_OBJS) $(TARGET) $(SRC_DIST)/*.o
	rm -f $(SRC_EMBED)/embedding_runtime.o $(SRC_EMBED)/embedding_runtime_onnx.o
	rm -f $(NTYCO_LIB) $(NTYCO_OBJS)
	rm -f $(EBPF_KERN_OBJ) $(EBPF_SYNCD_BIN) $(VMLINUX_H) $(VMLINUX_KVER)
	rm -rf $(BUILD_DIR)
	@# Legacy runtime artifacts left in tests/ by old scripts
	@find tests -maxdepth 2 -type f \( \
		-name "*.log" -o -name "*.tmp" -o -name "*.pid" \
		-o -name "*.rdb" -o -name "*.aof" -o -name "*.dat" \
		-o -name "*.gcda" -o -name "*.gcno" \
		-o -name "*.profraw" -o -name "*.profdata" \
		-o -name "core" \) -delete 2>/dev/null || true

distclean: clean
	rm -rf $(THIRD_PARTY)
	rm -f $(BTF2HEADER_BIN)
	$(MAKE) -C $(BTF2HEADER_DIR) clean 2>/dev/null || true

FORCE:

.PHONY: all setup deps bootstrap doctor clean distclean help \
        main tests test test-embedding ebpf embedding setup-embedding setup-onnx \
        check-embedding-assets _init_submodules FORCE
