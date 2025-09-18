
CC = gcc

# 正常编译标志
NORMAL_CFLAGS = -I ./NtyCo/core/ #-O2 
NORMAL_LDFLAGS = -L ./NtyCo/ -lntyco -lpthread -luring -ldl

# 调试编译标志
DEBUG_CFLAGS = -g -O0 -I ./NtyCo/core/
DEBUG_LDFLAGS = -L ./NtyCo/ -lntyco -lpthread -luring -ldl


SRCS = kvstore.c ntyco.c proactor.c reactor.c kvs_array.c kvs_rbtree.c kvs_hash.c kvs_skiptable.c  kvs_mempool.c distributed.c resp_protocol.c kvs_rdb.c kvs_operation.c kvs_aof.c
TESTCASE_SRCS = testcase.c

# 默认目标名称
TARGET = kvstore
DEBUG_TARGET = kvstore_debug

SUBDIR = ./NtyCo/

# 测试程序名称
TESTCASE = testcase
DEBUG_TESTCASE = testcase_debug

OBJS = $(SRCS:.c=.o)
DEBUG_OBJS = $(SRCS:.c=.debug.o)

# 默认编译目标 - 正常编译
all: $(SUBDIR) $(TARGET) $(TESTCASE)

# 调试编译目标
debug: $(SUBDIR) $(DEBUG_TARGET) $(DEBUG_TESTCASE)

$(SUBDIR): ECHO
	make -C $@

ECHO:
	@echo $(SUBDIR)

# 正常编译目标
$(TARGET): $(OBJS) 
	$(CC) -o $@ $^ $(NORMAL_LDFLAGS)

$(TESTCASE): $(TESTCASE_SRCS)
	$(CC) -o $@ $^

# 调试编译目标
$(DEBUG_TARGET): $(DEBUG_OBJS) 
	$(CC) -o $@ $^ $(DEBUG_LDFLAGS)

$(DEBUG_TESTCASE): $(TESTCASE_SRCS)
	$(CC) $(DEBUG_CFLAGS) -o $@ $^ $(DEBUG_LDFLAGS)

# 正常编译对象文件规则
%.o: %.c
	$(CC) $(NORMAL_CFLAGS) -c $^ -o $@

# 调试编译对象文件规则
%.debug.o: %.c
	$(CC) $(DEBUG_CFLAGS) -c $^ -o $@

# 清理所有编译文件
clean: 
	rm -rf $(OBJS) $(DEBUG_OBJS) $(TARGET) $(DEBUG_TARGET) $(TESTCASE) $(DEBUG_TESTCASE)

# 只清理调试编译文件
clean-debug:
	rm -rf $(DEBUG_OBJS) $(DEBUG_TARGET)

#调试目标
#debug: $(TARGET)
	#gdb ./$(TARGET)


