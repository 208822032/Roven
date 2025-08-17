#include "kvstore.h"

// 新增：全局大内存块链表
//gmalloc 自定义 系统自带 对比
    //虚拟内存占用率
    //一百万次请求
static mem_block_header_t *large_blocks_head = NULL;

mem_manager_t g_mem_manager;

void mem_init() {
    // 初始化内存池配置
    size_t sizes[] = {16, 24, 32, 48, 64, 96, 128, 192, 256, 384, 512, 768, 1024};
    g_mem_manager = (mem_manager_t){
        .pools = NULL,
        .pool_sizes = {16, 24, 32, 48, 64, 96, 128, 192, 256, 384, 512, 768, 1024},
        .large_threshold = 1024,
        .is_initialized = 1  // 标记为已初始化
    };

    memcpy(g_mem_manager.pool_sizes, sizes, sizeof(sizes));

    large_blocks_head = NULL;

    // 创建不同规格的内存池
    for (int i = 0; i < 13; i++) {
        mem_pool_t* pool = malloc(sizeof(mem_pool_t));
        *pool = (mem_pool_t){
            .block_size = g_mem_manager.pool_sizes[i],
            .free_count = 0,
            .free_list = NULL,
            .next = g_mem_manager.pools,
        };
        g_mem_manager.pools = pool;
    }
}

// 内存对齐 (8字节对齐)
#define MEM_ALIGNMENT 8
#define ALIGN_SIZE(size) (((size) + MEM_ALIGNMENT - 1) & ~(MEM_ALIGNMENT - 1))

void* mem_alloc(size_t size) {
    // 检查内存池是否已初始化，若未初始化或已销毁则返回NULL
    if (!g_mem_manager.is_initialized || size == 0) {
        return NULL;
    }

    size_t aligned_size = ALIGN_SIZE(size);
    
    
    // 寻找合适的内存池
    mem_pool_t* pool = g_mem_manager.pools;
    while (pool) {
        if (aligned_size <= pool->block_size) {
            if (pool->free_list) {
                // 从空闲链表分配
                mem_block_t* block = pool->free_list;
                pool->free_list = block->next;
                pool->free_count--;
                return block;
            } else {
                // 申请新内存块（一次申请多个）
                const int BLOCKS_PER_ALLOC = 20;
                size_t block_total_size = sizeof(mem_block_header_t) + pool->block_size;
                size_t total_size = block_total_size * BLOCKS_PER_ALLOC;
                void *mem = malloc(total_size);
                if (!mem) return NULL;
                
                // 分割内存并加入空闲链表
                char *p = mem;
                for (int i = 0; i < BLOCKS_PER_ALLOC; i++) {
                    mem_block_header_t *header = (mem_block_header_t*)p;
                    header->pool = pool;
                    header->block_size = pool->block_size;
                    header->is_large = 0;
                    
                    // 数据区地址
                    char *data = p + sizeof(mem_block_header_t);
                    
                    // 将数据区地址加入空闲链表
                    mem_block_t *block = (mem_block_t*)data;
                    block->next = pool->free_list;
                    pool->free_list = block;
                    pool->free_count++;
                    
                    p += block_total_size;
                }
                return mem_alloc(size); // 递归调用获取内存
            }
        }
        pool = pool->next;
    }
    
    // 保底分配（添加头部）
    size_t total_size = sizeof(mem_block_header_t) + aligned_size;
    mem_block_header_t *header = malloc(total_size);
    if (!header) return NULL;
    
    header->pool = NULL;
    header->block_size = aligned_size;
    header->is_large = 1;
    
    // 新增：将大内存块加入链表
    header->next_large = large_blocks_head;
    large_blocks_head = header;
    
    return (char*)header + sizeof(mem_block_header_t);
}

void mem_free(void* ptr) {
    if (!ptr || !g_mem_manager.is_initialized) return;

    // 获取内存块头部
    mem_block_header_t *header = (mem_block_header_t*)((char*)ptr - sizeof(mem_block_header_t));
    
    // 大内存直接释放（通过大内存记录列表）
    if (header->is_large) {
        // 大内存会在mem_destroy中统一释放
        return;
    }
    
    // 池内存放回空闲链表
    mem_pool_t *pool = header->pool;
    if (pool) {
        mem_block_t *block = (mem_block_t*)ptr;
        block->next = pool->free_list;
        pool->free_list = block;
        pool->free_count++;
    }
}

void mem_destroy() {
    if (!g_mem_manager.is_initialized) return;

    // 新增：释放所有大内存块
    while (large_blocks_head) {
        mem_block_header_t *next = large_blocks_head->next_large;
        free(large_blocks_head);
        large_blocks_head = next;
    }
    
    // 释放所有内存池
    mem_pool_t* pool = g_mem_manager.pools;
    while (pool) {
        mem_pool_t* next_pool = pool->next;
        free(pool);
        pool = next_pool;
    }
    
    g_mem_manager.pools = NULL;

    // 新增：重置初始化标志
    g_mem_manager.is_initialized = 0;
    
}