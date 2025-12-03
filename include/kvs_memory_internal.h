#pragma once
#include <stddef.h>
#include <pthread.h>
#include "kvs_config.h"
#include <stdio.h>

typedef struct mem_block {
    struct mem_block* next;
} mem_block_t;

typedef struct mem_chunk{
    void *chunk;
    struct mem_chunk *next;
} mem_chunk_t;

typedef struct mem_pool {
    size_t block_size;
    int free_count;
    mem_block_t* free_list;
    struct mem_pool* next;
    pthread_mutex_t lock;
    mem_chunk_t *chunks;
} mem_pool_t;

typedef struct large_block_record {
    void *block;
    size_t size;
    struct large_block_record *next;
    struct large_block_record *prev;
} large_block_record_t;

typedef struct mem_block_header {
    mem_pool_t *pool;
    size_t block_size;
    unsigned char is_large;
    large_block_record_t *large_record;
} mem_block_header_t;

typedef struct {
    mem_pool_t* pools;
    size_t pool_sizes[13];
    size_t large_threshold;
    int is_initialized;
    pthread_mutex_t large_blocks_lock;
} mem_manager_t;

extern mem_manager_t g_mem_manager;


// 允许外部根据配置设置当前内存后端
void mem_set_backend(kvs_mem_backend_t backend);
void mem_set_jemalloc_funcs(void *(*fn_alloc)(size_t), void (*fn_free)(void *), void *(*fn_realloc)(void *, size_t));
