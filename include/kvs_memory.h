#pragma once
#include <stddef.h>

// 内存管理对外接口（内部实现隐藏在 .c 或 _internal.h）
// 注意：实际使用的后端（自定义池/系统/gmalloc）由配置决定
void mem_init(void);
void mem_destroy(void);
void* mem_alloc(size_t size);
void mem_free(void* ptr);
void* mem_realloc(void* ptr, size_t new_size);

// 可选内存泄漏跟踪接口（实现中决定是否启用）
void* memleak_alloc(size_t size, const char *filename, const char *funcname, int line);
void memleak_free(void *ptr, const char *filename, const char *funcname, int line);

// 统一宏：对外统一使用 kvs_malloc/kvs_free
// 内部会根据当前选择的内存后端转发到对应实现
#define kvs_malloc(size) mem_alloc(size)
#define kvs_free(ptr)    mem_free(ptr)
#define kvs_realloc(ptr, new_size) mem_realloc(ptr, new_size)

//工具函数 始终通过kvs_malloc分配内存
char* kvs_strdup(const char* s);

