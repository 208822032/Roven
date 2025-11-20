#pragma once
#include <stddef.h>

// 内存管理对外接口（内部实现隐藏在 .c 或 _internal.h）
void mem_init(void);
void mem_destroy(void);
void* mem_alloc(size_t size);
void mem_free(void* ptr);
void* kvs_realloc(void* ptr, size_t new_size);
char* kvs_strdup(const char* s);

// 可选内存泄漏跟踪接口（实现中决定是否启用）
void* memleak_alloc(size_t size, const char *filename, const char *funcname, int line);
void memleak_free(void *ptr, const char *filename, const char *funcname, int line);

// 统一宏：允许通过 USE_MEMPOOL 切换系统分配或自定义池分配
#ifndef USE_MEMPOOL
#define kvs_malloc(size) mem_alloc(size)
#define kvs_free(ptr)    mem_free(ptr)
#else
#include <stdlib.h>
#define kvs_malloc(size) malloc(size)
#define kvs_free(ptr)    free(ptr)
#endif

