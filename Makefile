
CC = gcc
FLAGS = -I ./NtyCo/core/ -L ./NtyCo/ -lntyco -lpthread -luring -ldl
SRCS = kvstore.c ntyco.c proactor.c reactor.c kvs_array.c kvs_rbtree.c kvs_hash.c kvs_skiptable.c  kvs_mempool.c distributed.c resp_protocol.c kvs_rdb.c kvs_operation.c kvs_aof.c
TESTCASE_SRCS = testcase.c
TARGET = kvstore
SUBDIR = ./NtyCo/
TESTCASE = testcase

OBJS = $(SRCS:.c=.o)


all: $(SUBDIR) $(TARGET) $(TESTCASE)

$(SUBDIR): ECHO
	make -C $@

ECHO:
	@echo $(SUBDIR)

$(TARGET): $(OBJS) 
	$(CC) -o $@ $^ $(FLAGS)

$(TESTCASE): $(TESTCASE_SRCS)
	$(CC) -o $@ $^

%.o: %.c
	$(CC) $(FLAGS) -c $^ -o $@

clean: 
	rm -rf $(OBJS) $(TARGET) $(TESTCASE)


