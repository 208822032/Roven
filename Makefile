CC = gcc
SYNC_MODE ?= 0

# 与 kvs_config.h 中的 KVS_MAX_RESPONSE_SIZE 保持一致
KVS_MAX_RESPONSE_SIZE ?= 2048

SRC_CORE := src/core
SRC_STORAGE := src/storage
SRC_PERSIST := src/persistence
SRC_NETWORK := src/network
SRC_DIST := src/distributed
SRC_UTILS := src/utils

# 提前固定好目录
BPF_DIR := $(SRC_DIST)

JEMALLOC_DIR := jemalloc
JEMALLOC_INCLUDE := $(JEMALLOC_DIR)/include
JEMALLOC_LIB := $(JEMALLOC_DIR)/lib/libjemalloc.a

BASE_SRCS = \
	$(SRC_CORE)/kvstore.c \
	$(SRC_CORE)/kvs_config.c \
	$(SRC_CORE)/kvs_globals.c \
	$(SRC_CORE)/kvs_operation.c \
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
	$(SRC_PERSIST)/kvs_rdb.c \
	$(SRC_PERSIST)/kvs_aof.c \
	$(SRC_PERSIST)/kvs_aof_rewrite.c \
	$(SRC_UTILS)/msg_queue.c \
	$(SRC_UTILS)/master_slave_proto.c \


ifeq ($(SYNC_MODE),1)
SRCS = $(BASE_SRCS) \
		$(SRC_DIST)/ebpf_sync.c \
		$(SRC_DIST)/sync_backend_ebpf.c \

# BPF_DIR = $(SRC_DIST)
BPF_SRC = $(BPF_DIR)/ebpf_sync.bpf.c
BPF_OBJ = $(BPF_DIR)/ebpf_sync.bpf.o
EXTRA_LIBS = -lbpf -lelf -lz
else
SRCS = $(BASE_SRCS) \
		$(SRC_DIST)/sync_backend_tcp.c
BPF_OBJ =
EXTRA_LIBS =
endif

CFLAGS = -I ./NtyCo/core/ -I ./include -I ./src -I $(JEMALLOC_INCLUDE) -DSYNC_MODE=$(SYNC_MODE) -g
LDFLAGS = -L ./NtyCo/ -lntyco -lpthread -luring -ldl $(EXTRA_LIBS)  $(JEMALLOC_LIB)

TARGET = kvstore
OBJS = $(SRCS:.c=.o)

# =====================
# Tests configuration
# =====================

TEST_DIR := tests

TEST_CASE := $(TEST_DIR)/testcase
TEST_WRITE_QPS := $(TEST_DIR)/write_qps_test
TEST_KVSTORE_CLIENT := $(TEST_DIR)/kvstore_client

TEST_BINARIES = \
	$(TEST_CASE) \
	$(TEST_WRITE_QPS) \
	$(TEST_KVSTORE_CLIENT)

all: $(TARGET) $(TEST_BINARIES)

$(TARGET): $(OBJS) $(BPF_OBJ)
	$(CC) -o $@ $(OBJS) $(LDFLAGS)

# 单元/集成测试可执行文件规则

$(TEST_CASE): $(TEST_DIR)/testcase.c
	$(CC) $(CFLAGS) -o $@ $<

$(TEST_WRITE_QPS): $(TEST_DIR)/write_qps_test.c
	$(CC) $(CFLAGS) -o $@ $<

$(TEST_KVSTORE_CLIENT): $(TEST_DIR)/kvstore_client.c
	$(CC) $(CFLAGS) -o $@ $< -lreadline


%.o: %.c
	$(CC) $(CFLAGS) -c $< -o $@

UNAME_M := $(shell uname -m)
ifeq ($(UNAME_M),x86_64)
BPF_ARCH_MACRO = __TARGET_ARCH_x86
else ifeq ($(UNAME_M),aarch64)
BPF_ARCH_MACRO = __TARGET_ARCH_arm64
else
BPF_ARCH_MACRO = __TARGET_ARCH_x86
endif

$(BPF_OBJ): $(BPF_SRC) $(BPF_DIR)/vmlinux.h
	clang -O2 -g -target bpf -D$(BPF_ARCH_MACRO) \
	        -I/usr/include -I/usr/include/bpf -I. -I$(BPF_DIR) \
			-DEBPF_KVS_MAX_RESPONSE_SIZE=$(KVS_MAX_RESPONSE_SIZE) \
        -c $< -o $@

$(BPF_DIR)/vmlinux.h:
	@if [ ! -f $(BPF_DIR)/vmlinux.h ]; then \
        echo "Generate vmlinux.h"; \
	        bpftool btf dump file /sys/kernel/btf/vmlinux format c > $(BPF_DIR)/vmlinux.h; \
	fi

clean:
	rm -f $(OBJS) $(TARGET) $(BPF_OBJ) src/distributed/vmlinux.h $(TEST_BINARIES) $(SRC_DIST)/*.o 

.PHONY: all clean