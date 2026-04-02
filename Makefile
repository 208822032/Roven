CC = gcc

# 与 kvs_config.h 中的 KVS_MAX_RESPONSE_SIZE 保持一致
KVS_MAX_RESPONSE_SIZE ?= 1048576

SRC_CORE := src/core
SRC_STORAGE := src/storage
SRC_PERSIST := src/persistence
SRC_NETWORK := src/network
SRC_DIST := src/distributed
SRC_UTILS := src/utils

NTYCO_DIR := NtyCo
NTYCO_CORE_DIR := $(NTYCO_DIR)/core
NTYCO_LIB := $(NTYCO_DIR)/libntyco.a
NTYCO_OBJS := \
	$(NTYCO_DIR)/objs/nty_socket.o \
	$(NTYCO_DIR)/objs/nty_coroutine.o \
	$(NTYCO_DIR)/objs/nty_epoll.o \
	$(NTYCO_DIR)/objs/nty_schedule.o

JEMALLOC_DIR := jemalloc
JEMALLOC_INCLUDE := $(JEMALLOC_DIR)/include
JEMALLOC_LIB := $(JEMALLOC_DIR)/lib/libjemalloc.a

BASE_SRCS = \
	$(SRC_CORE)/kvstore.c \
	$(SRC_CORE)/kvs_config.c \
	$(SRC_CORE)/kvs_globals.c \
	$(SRC_CORE)/kvs_operation.c \
	$(SRC_CORE)/kvs_ttl_background.c \
	$(SRC_STORAGE)/kvs_array.c \
	$(SRC_STORAGE)/kvs_rbtree.c \
	$(SRC_STORAGE)/kvs_hash.c \
	$(SRC_STORAGE)/kvs_skiptable.c \
	$(SRC_STORAGE)/kvs_mempool.c \
	$(SRC_STORAGE)/jemalloc_wrapper.c \
	$(SRC_DIST)/distributed.c \
	$(SRC_NETWORK)/resp_protocol.c \
	$(SRC_NETWORK)/reactor.c \
    $(SRC_NETWORK)/proactor.c \
    $(SRC_NETWORK)/ntyco.c \
	$(SRC_NETWORK)/net_session.c \
	$(SRC_PERSIST)/kvs_rdb.c \
	$(SRC_PERSIST)/kvs_aof.c \
	$(SRC_PERSIST)/kvs_aof_rewrite.c \
	$(SRC_UTILS)/msg_queue.c \
	$(SRC_UTILS)/master_slave_proto.c \
	$(SRC_UTILS)/kvs_log.c \
	$(SRC_UTILS)/kvs_log_writer.c \
	$(SRC_UTILS)/netbuf.c \
	$(SRC_UTILS)/tx_batcher.c
	

SRCS = $(BASE_SRCS) \
		$(SRC_DIST)/sync_backend_tcp.c

EXTRA_CFLAGS ?=
CFLAGS = -I ./NtyCo/core/ -I ./include -I ./src -I $(JEMALLOC_INCLUDE) -DSYNC_MODE=$(SYNC_MODE) -g $(EXTRA_CFLAGS)
LDFLAGS = -L ./NtyCo/ -lntyco -lpthread -luring -ldl $(EXTRA_LIBS)  $(JEMALLOC_LIB)

TARGET = kvstore
OBJS = $(SRCS:.c=.o)

TEST_DIR := tests

TEST_CASE := $(TEST_DIR)/testcase
TEST_WRITE_QPS := $(TEST_DIR)/write_qps_test
TEST_TTL_QPS := $(TEST_DIR)/write_ttl_qps
TEST_KVSTORE_CLIENT := $(TEST_DIR)/kvstore_client
TEST_MEMPOOL := $(TEST_DIR)/mempool_test
TEST_PAPER := $(TEST_DIR)/test_paper
TEST_PAPER_SYNC := $(TEST_DIR)/test_paper_sync
TEST_BIGVALUE := $(TEST_DIR)/test_bigvalue

TEST_BINARIES = \
	$(TEST_CASE) \
	$(TEST_WRITE_QPS) \
	$(TEST_TTL_QPS) \
	$(TEST_KVSTORE_CLIENT) \
	$(TEST_MEMPOOL) \
	$(TEST_PAPER) \
	$(TEST_PAPER_SYNC) \
	$(TEST_BIGVALUE) \

CLANG ?= clang
# 尝试查找 bpftool，如果找不到则默认为 bpftool
BPFTOOL := $(shell which bpftool 2>/dev/null)
ifeq ($(BPFTOOL),)
	BPFTOOL := bpftool
endif
EBPF_DIR := src/eBPF
EBPF_KERN_OBJ := $(EBPF_DIR)/ebpf_kern.o
EBPF_SYNCD_BIN := $(EBPF_DIR)/ebpf_syncd
VMLINUX_H := $(EBPF_DIR)/vmlinux.h

# 自动检测架构
UNAME_M := $(shell uname -m)
ifeq ($(UNAME_M),x86_64)
    BPF_ARCH_MACRO := -D__TARGET_ARCH_x86
else ifeq ($(UNAME_M),aarch64)
    BPF_ARCH_MACRO := -D__TARGET_ARCH_arm64
else
    BPF_ARCH_MACRO := -D__TARGET_ARCH_x86
endif

# eBPF 内核态编译选项
# BPF_NO_UNROLL := -fno-unroll-loops  $(BPF_NO_UNROLL) 
BPF_CFLAGS := -O2 -g -target bpf $(BPF_ARCH_MACRO) -I $(EBPF_DIR)

all: $(NTYCO_LIB) $(TARGET) $(TEST_BINARIES) $(EBPF_KERN_OBJ) $(EBPF_SYNCD_BIN)

$(NTYCO_DIR)/objs:
	mkdir -p $@

$(NTYCO_DIR)/objs/%.o: $(NTYCO_CORE_DIR)/%.c | $(NTYCO_DIR)/objs
	$(CC) -c $(CFLAGS) -o $@ $< -I $(NTYCO_CORE_DIR) -O3 -lpthread -ldl

$(NTYCO_LIB): $(NTYCO_OBJS)
	ar rcs $@ $^

$(TARGET): $(OBJS)
	$(CC) -o $@ $(OBJS) $(LDFLAGS)

# 单元/集成测试可执行文件规则

$(TEST_CASE): $(TEST_DIR)/testcase.c
	$(CC) $(CFLAGS) -o $@ $<

$(TEST_WRITE_QPS): $(TEST_DIR)/write_qps_test.c
	$(CC) $(CFLAGS) -o $@ $<

$(TEST_TTL_QPS): $(TEST_DIR)/write_ttl_qps_test.c
	$(CC) $(CFLAGS) -o $@ $<

write_ttl_qps: $(TEST_TTL_QPS)

$(TEST_KVSTORE_CLIENT): $(TEST_DIR)/kvstore_client.c
	$(CC) $(CFLAGS) -o $@ $< -lreadline

$(TEST_PAPER): $(TEST_DIR)/test_paper.c
	$(CC) $(CFLAGS) -o $@ $<

MEMPOOL_OBJS := \
	$(SRC_STORAGE)/kvs_mempool.o \
	$(SRC_CORE)/kvs_config.o \
	$(SRC_CORE)/kvs_globals.o \
	$(SRC_UTILS)/kvs_log.o \
	$(SRC_UTILS)/kvs_log_writer.o \
	$(SRC_STORAGE)/jemalloc_wrapper.o \
	$(SRC_STORAGE)/kvs_array.o \
	$(SRC_STORAGE)/kvs_rbtree.o \
	$(SRC_STORAGE)/kvs_hash.o \
	$(SRC_STORAGE)/kvs_skiptable.o

$(TEST_MEMPOOL): $(TEST_DIR)/mempool_test.c $(MEMPOOL_OBJS)
	$(CC) $(CFLAGS) -o $@ $< $(MEMPOOL_OBJS) -lpthread $(JEMALLOC_LIB) -ldl

# 生成 vmlinux.h
$(VMLINUX_H):
	@echo "Generating vmlinux.h..."
	@if [ ! -f /sys/kernel/btf/vmlinux ]; then \
		echo "Error: /sys/kernel/btf/vmlinux not found. Ensure CONFIG_DEBUG_INFO_BTF is enabled."; \
		exit 1; \
	fi
	$(BPFTOOL) btf dump file /sys/kernel/btf/vmlinux format c > $@

$(EBPF_KERN_OBJ): $(EBPF_DIR)/ebpf_kern.c $(VMLINUX_H)
	$(CLANG) $(BPF_CFLAGS) -c $< -o $@

#eBPF用户态守护进程
$(EBPF_SYNCD_BIN): \
	$(EBPF_DIR)/ebpf_syncd.c \
	$(SRC_NETWORK)/resp_protocol.c \
	$(SRC_UTILS)/msg_queue.c \
	$(SRC_STORAGE)/kvs_mempool.c \
	$(SRC_UTILS)/kvs_log.c \
	$(SRC_UTILS)/kvs_log_writer.c \
	$(SRC_STORAGE)/jemalloc_wrapper.c \
	$(SRC_STORAGE)/kvs_array.c \
	$(SRC_STORAGE)/kvs_hash.c \
	$(SRC_STORAGE)/kvs_rbtree.c \
	$(SRC_STORAGE)/kvs_skiptable.c \
	$(SRC_CORE)/kvs_config.c \
	$(SRC_UTILS)/tx_batcher.c \
	$(SRC_CORE)/kvs_globals.c
	$(CC) -O2 -g $^ -o $@ \
		-lbpf -lelf -lz -lpthread $(JEMALLOC_LIB) -ldl -I ./include

%.o: %.c
	$(CC) $(CFLAGS) -c $< -o $@


clean:
	rm -f $(OBJS) $(TARGET) $(TEST_BINARIES) $(SRC_DIST)/*.o
	rm -f $(TEST_DIR)/write_ttl_qps_test
	rm -f $(NTYCO_LIB) $(NTYCO_OBJS)
	rm -f $(EBPF_KERN_OBJ) $(EBPF_SYNCD_BIN) $(VMLINUX_H)
.PHONY: all clean