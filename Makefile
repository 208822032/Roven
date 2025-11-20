CC = gcc
SYNC_MODE ?= 0

BASE_SRCS = kvstore.c ntyco.c proactor.c reactor.c kvs_array.c kvs_rbtree.c kvs_hash.c kvs_skiptable.c kvs_mempool.c distributed.c resp_protocol.c kvs_rdb.c kvs_operation.c kvs_aof.c kvs_aof_rewrite.c kvs_globals.c

ifeq ($(SYNC_MODE),1)
SRCS = $(BASE_SRCS) ebpf_sync.c
BPF_SRC = ebpf_sync.bpf.c
BPF_OBJ = ebpf_sync.bpf.o
EXTRA_LIBS = -lbpf -lelf -lz
else
SRCS = $(BASE_SRCS)
BPF_OBJ =
EXTRA_LIBS =
endif

CFLAGS = -I ./NtyCo/core/ -I ./include -DSYNC_MODE=$(SYNC_MODE) -g -no-pie
LDFLAGS = -L ./NtyCo/ -lntyco -lpthread -luring -ldl $(EXTRA_LIBS)

TARGET = kvstore
OBJS = $(SRCS:.c=.o)

#测试
TEST_TARGET = testbpf
TEST_SRCS = testbpf.c ebpf_sync.c distributed.c kvs_array.c kvs_rbtree.c kvs_hash.c kvs_skiptable.c kvs_mempool.c kvs_globals.c resp_protocol.c kvs_operation.c
TEST_OBJS = $(TEST_SRCS:.c=.o)

all: $(TARGET)

$(TARGET): $(OBJS) $(BPF_OBJ)
	$(CC) -o $@ $(OBJS) $(LDFLAGS)

$(TEST_TARGET): $(TEST_OBJS) $(BPF_OBJ)
	$(CC) -o $@ $(TEST_OBJS) $(LDFLAGS)

%.o: %.c
	$(CC) $(CFLAGS) -c $< -o $@

# 自动架构宏
UNAME_M := $(shell uname -m)
ifeq ($(UNAME_M),x86_64)
BPF_ARCH_MACRO = __TARGET_ARCH_x86
else ifeq ($(UNAME_M),aarch64)
BPF_ARCH_MACRO = __TARGET_ARCH_arm64
else
BPF_ARCH_MACRO = __TARGET_ARCH_x86
endif

$(BPF_OBJ): $(BPF_SRC) vmlinux.h
	clang -O2 -g -target bpf -D$(BPF_ARCH_MACRO) \
        -I/usr/include -I/usr/include/bpf -I. \
        -c $< -o $@

vmlinux.h:
	@if [ ! -f vmlinux.h ]; then \
        echo "Generate vmlinux.h"; \
        bpftool btf dump file /sys/kernel/btf/vmlinux format c > vmlinux.h; \
	fi

clean:
	rm -f $(OBJS) $(TARGET) $(BPF_OBJ) $(BPF_OBJ) $(TEST_OBJS) $(TEST_TARGET)

.PHONY: all clean