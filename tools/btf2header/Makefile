# btf2header — 从 raw BTF 生成 vmlinux.h
# 用法:
#   make                            # 从项目根 Makefile 调用
#   make LIBBPF_INC=... LIBBPF_LIB=...

CC       ?= gcc
CFLAGS   := -O2
BIN       = btf2header
SRC       = $(BIN).c

# 由上级 Makefile 传入
LIBBPF_INC ?= ../../third_party/libbpf/include
LIBBPF_LIB ?= ../../third_party/libbpf/lib

$(BIN): $(SRC)
	$(CC) $(CFLAGS) -I $(LIBBPF_INC) -o $@ $< \
		-L $(LIBBPF_LIB) -lbpf -lelf -lz \
		-Wl,-rpath,$(LIBBPF_LIB)

clean:
	rm -f $(BIN)

.PHONY: clean
