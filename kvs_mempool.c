#include "kvstore.h"

// 新增：全局大内存块链表
//gmalloc 自定义 系统自带 对比
    //虚拟内存占用率
    //一百万次请求
// static mem_block_header_t *large_blocks_head = NULL;

static large_block_record_t *large_blocks_head = NULL;
static large_block_record_t *large_blocks_tail = NULL;

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

    // large_blocks_head = NULL;
    large_blocks_head = NULL;
    large_blocks_tail = NULL;

    //初始化大内存链表锁
    pthread_mutex_init(&g_mem_manager.large_blocks_lock, NULL);

    // 创建不同规格的内存池
    //与此同时初始化各自的锁
    for (int i = 0; i < 13; i++) {
        mem_pool_t* pool = malloc(sizeof(mem_pool_t));
        *pool = (mem_pool_t){
            .block_size = g_mem_manager.pool_sizes[i],
            .free_count = 0,
            .free_list = NULL,
            .next = g_mem_manager.pools,
            .chunks = NULL //初始化大块内存链表
        };
        pthread_mutex_init(&pool->lock, NULL);
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
            pthread_mutex_lock(&pool->lock);
            if (pool->free_list) {
                // 从空闲链表分配
                mem_block_t* block = pool->free_list;
                pool->free_list = block->next;
                pool->free_count--;
                pthread_mutex_unlock(&pool->lock);
                return block;
            } else {
                pthread_mutex_unlock(&pool->lock);

                // 申请新内存块（一次申请多个）
                const int BLOCKS_PER_ALLOC = 20;
                size_t block_total_size = sizeof(mem_block_header_t) + pool->block_size;
                size_t total_size = block_total_size * BLOCKS_PER_ALLOC;
                void *mem = malloc(total_size);
                if (!mem) return NULL;
                
                //记录这个大块内存
                mem_chunk_t *new_chunk = malloc(sizeof(mem_chunk_t));
                if(!new_chunk){
                    free(mem);
                    return NULL;
                }
                new_chunk->chunk = mem;
                new_chunk->next = NULL;
                
                // 分割内存并加入空闲链表
                char *p = mem;
                mem_block_t *new_blocks = NULL;
                int new_block_count = 0;

                for (int i = 0; i < BLOCKS_PER_ALLOC; i++) {
                    mem_block_header_t *header = (mem_block_header_t*)p;
                    header->pool = pool;
                    header->block_size = pool->block_size;
                    header->is_large = 0;
                    header->large_record = NULL; // 不是大内存
                    
                    // 数据区地址
                    char *data = p + sizeof(mem_block_header_t);
                    mem_block_t *block = (mem_block_t*)data;
                    block->next = new_blocks;
                    new_blocks = block;
                    new_block_count++;

                    p += block_total_size;
                }

                //将新分配的块加入空闲链表
                pthread_mutex_lock(&pool->lock);

                //将大块内存记录添加到内存池
                new_chunk->next = pool->chunks;
                pool->chunks = new_chunk;

                mem_block_t *block = new_blocks;
                while(block){
                    mem_block_t *next = block->next;
                    block->next = pool->free_list;
                    pool->free_list = block;
                    pool->free_count++;
                    block = next;
                }
                pthread_mutex_unlock(&pool->lock);

                return mem_alloc(size); // 递归调用获取内存
            }
        }
        pool = pool->next;
    }
    
    // 大内存块分配
    size_t total_size = sizeof(mem_block_header_t) + aligned_size;
    mem_block_header_t *header = malloc(total_size);
    if (!header) return NULL;
    
    header->pool = NULL;
    header->block_size = aligned_size;
    header->is_large = 1;

    //创建大内存记录
    large_block_record_t *record = malloc(sizeof(large_block_record_t));
    if(!record){
        free(header);
        return NULL;
    }

    record->block = header;
    record->size = total_size;
    record->next = NULL;
    record->prev = NULL;

    header->large_record = record;
    
    // 新增：将大内存块加入链表
    pthread_mutex_lock(&g_mem_manager.large_blocks_lock);
    // header->next_large = large_blocks_head;
    // large_blocks_head = header;
    if (large_blocks_head == NULL) {
        large_blocks_head = record;
        large_blocks_tail = record;
    } else {
        record->next = large_blocks_head;
        large_blocks_head->prev = record;
        large_blocks_head = record;
    }
    pthread_mutex_unlock(&g_mem_manager.large_blocks_lock);
    
    return (char*)header + sizeof(mem_block_header_t);
}

void mem_free(void* ptr) {
    if (!ptr || !g_mem_manager.is_initialized) return;

    // 获取内存块头部
    mem_block_header_t *header = (mem_block_header_t*)((char*)ptr - sizeof(mem_block_header_t));
    
    // 大内存直接释放（通过大内存记录列表）
    // if (header->is_large) {
    //     // 大内存会在mem_destroy中统一释放
    //     return;
    // }

    //大内存处理
    if(header->is_large){
        if (header->large_record == NULL || header->large_record->block != header) {
        printf("错误：无效的大内存记录\n");
        // 直接释放header？但是这样可能会崩溃，因为记录可能已经无效
        free(header);
        return;
    }

        pthread_mutex_lock(&g_mem_manager.large_blocks_lock);

        //从链表中移除记录
        large_block_record_t *record = header->large_record;
        if(record->prev){
            record->prev->next = record->next;
        }else{
            large_blocks_head = record->next;
        }

        if(record->next){
            record->next->prev = record->prev;
        }else{
            large_blocks_tail = record->prev;
        }

        pthread_mutex_unlock(&g_mem_manager.large_blocks_lock);

        //释放内存和记录
        free(record);
        free(header);
        return;
    }
    
    // 池内存放回空闲链表
    mem_pool_t *pool = header->pool;
    if (pool) {
        pthread_mutex_lock(&pool->lock);
        mem_block_t *block = (mem_block_t*)ptr;
        block->next = pool->free_list;
        pool->free_list = block;
        pool->free_count++;
        pthread_mutex_unlock(&pool->lock);
    }
}

void mem_destroy() {
    if (!g_mem_manager.is_initialized) return;

    // 新增：释放所有大内存块
    
    pthread_mutex_lock(&g_mem_manager.large_blocks_lock);
    large_block_record_t *record = large_blocks_head;
    
    while (record) {
        large_block_record_t  *next = record->next;
        free(record->block);//释放大内存块
        free(record);//释放记录
        record = next;
    }
    
    large_blocks_head = NULL;
    large_blocks_tail = NULL;
    pthread_mutex_unlock(&g_mem_manager.large_blocks_lock);
    
    // 释放所有内存池
    mem_pool_t* pool = g_mem_manager.pools;
    while (pool) {
        //释放前先获取锁 确保没有其他线程正在使用
        pthread_mutex_lock(&pool->lock);
        mem_pool_t *next_pool = pool->next;

        //释放内存池中所有的块
        mem_chunk_t *chunk = pool->chunks;
        while(chunk){
            mem_chunk_t *next_chunk = chunk->next;
            free(chunk->chunk);//释放大块内存
            free(chunk);//释放记录
            chunk = next_chunk;
        }
        pool->chunks = NULL;

        // 注意：不需要释放free_list中的单个块，因为它们属于大块内存的一部分
        pool->free_list = NULL;
        pool->free_count = 0;

        pthread_mutex_unlock(&pool->lock);
        pthread_mutex_destroy(&pool->lock);
        free(pool);
        pool = next_pool;
    }

    
    g_mem_manager.pools = NULL;

    // 新增：重置初始化标志
    g_mem_manager.is_initialized = 0;
    pthread_mutex_destroy(&g_mem_manager.large_blocks_lock);
    
}

//重新分配
void* kvs_realloc(void *ptr, size_t new_size){
    if(!ptr){
        //如果原指针为空 等同于分配新内存
        return mem_alloc(new_size);
    }

    if(new_size == 0){
        //等于释放内存
        mem_free(ptr);
        return NULL;
    }

    //获取内存块头部
    mem_block_header_t *old_header = (mem_block_header_t *)((char *)ptr - sizeof(mem_block_header_t));

    //对齐新大小
    size_t aligend_new_size = ALIGN_SIZE(new_size);

    //检查是否可以在原内存块中容纳新大小
    if(aligend_new_size <= old_header->block_size){
        //新大小小于或等于原块大小 无需重新分配
        return ptr;
    }

    //若需要重新分配更大的内存块
    void *new_ptr = mem_alloc(new_size);
    if(!new_ptr){
        return NULL;//分配失败
    }

    //计算需要复制的数据量 原块大小和新大小 的较小值
    size_t copy_size = old_header->block_size < new_size ? old_header->block_size : new_size;

    //复制数据
    memcpy(new_ptr, ptr, copy_size);

    //释放原内存
    mem_free(ptr);

    return new_ptr;
}