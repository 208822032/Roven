#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <pthread.h>
#include "kvstore.h"

//测试内存池基本功能
void test_basic_functionality(){
    printf("=== 测试基本功能 ===\n");

    // 初始化内存池
    mem_init();
    printf("内存池初始化成功\n");

    // 测试不同大小的内存分配
    void *ptr1 = mem_alloc(16);
    void *ptr2 = mem_alloc(32);
    void *ptr3 = mem_alloc(64);
    void *ptr4 = mem_alloc(128);
    void *ptr5 = mem_alloc(256);

    printf("分配不同大小内存块: %p, %p, %p, %p, %p\n", ptr1, ptr2, ptr3, ptr4, ptr5);

    // 测试内存写入和读取
    strcpy((char*)ptr1, "Hello, World!");
    strcpy((char*)ptr2, "Memory Pool Test");

    printf("ptr1 内容: %s\n", (char*)ptr1);
    printf("ptr2 内容: %s\n", (char*)ptr2);
    
    // 测试内存释放
    mem_free(ptr1);
    mem_free(ptr2);
    printf("内存释放成功\n");

    // 测试重新分配
    void *new_ptr = mem_alloc(48);
    printf("重新分配内存: %p\n", new_ptr);
    strcpy((char*)new_ptr, "Realloc Test");
    printf("重新分配内存内容: %s\n", (char*)new_ptr);
    
    // 测试realloc函数
    void *realloc_ptr = kvs_realloc(new_ptr, 96);
    printf("kvs_realloc 从 48 到 96: %p\n", realloc_ptr);
    if (realloc_ptr) {
        printf("realloc后内容: %s\n", (char*)realloc_ptr);
        mem_free(realloc_ptr);
    }
    
    // 测试大内存分配（超过阈值）
    void *large_ptr = mem_alloc(2048);
    printf("大内存分配: %p\n", large_ptr);
    if (large_ptr) {
        memset(large_ptr, 'A', 2048);
        printf("大内存分配和初始化成功\n");
        mem_free(large_ptr);
    }
    
    // 销毁内存池
    mem_destroy();
    printf("内存池销毁成功\n\n");
}

// 测试内存池性能
void test_performance() {
    printf("=== 测试性能 ===\n");

    mem_init();

    const int NUM_ALLOCS = 10000;
    void *pointers[NUM_ALLOCS];
    int sizes[NUM_ALLOCS];

    // 分配大量内存块
    clock_t start = clock();
    for (int i = 0; i < NUM_ALLOCS; i++) {
        sizes[i] = (rand() % 1000) + 1;
        pointers[i] = mem_alloc(sizes[i]);
        if (pointers[i]) {
            memset(pointers[i], i % 256, sizes[i]);
        }
    }
    clock_t end = clock();

    printf("分配 %d 个内存块耗时: %f 秒\n", 
           NUM_ALLOCS, (double)(end - start) / CLOCKS_PER_SEC);

    // 释放一半内存块
    start = clock();
    for (int i = 0; i < NUM_ALLOCS; i += 2) {
        if (pointers[i]) {
            mem_free(pointers[i]);
            pointers[i] = NULL;
        }
    }
    end = clock();

    printf("释放一半内存块耗时: %f 秒\n", 
           (double)(end - start) / CLOCKS_PER_SEC);

    // 重新分配一些内存块
    start = clock();
    for (int i = 0; i < NUM_ALLOCS; i += 2) {
        sizes[i] = (rand() % 1000) + 1;
        pointers[i] = mem_alloc(sizes[i]);
        if (pointers[i]) {
            memset(pointers[i], i % 256, sizes[i]);
        }
    }
    end = clock();
    
    printf("重新分配一半内存块耗时: %f 秒\n", 
           (double)(end - start) / CLOCKS_PER_SEC);

    // 释放所有内存块
    start = clock();
    for (int i = 0; i < NUM_ALLOCS; i++) {
        if (pointers[i]) {
            mem_free(pointers[i]);
        }
    }
    end = clock();
    
    printf("释放所有内存块耗时: %f 秒\n", 
           (double)(end - start) / CLOCKS_PER_SEC);
    
    mem_destroy();
    printf("性能测试完成\n\n");
}

// 线程函数参数
typedef struct {
    int thread_id;
    int num_allocations;
    int max_size;
} thread_params_t;

// 线程函数 - 测试多线程环境下的内存分配
void* thread_func(void *arg) {
    thread_params_t *params = (thread_params_t*)arg;
    
    void **pointers = malloc(sizeof(void*) * params->num_allocations);
    if (!pointers) {
        printf("线程 %d: 无法分配指针数组\n", params->thread_id);
        return NULL;
    }
    
    // 分配内存
    for (int i = 0; i < params->num_allocations; i++) {
        int size = (rand() % params->max_size) + 1;
        pointers[i] = mem_alloc(size);
        if (pointers[i]) {
            memset(pointers[i], params->thread_id, size);
        }
    }
    
    // 释放部分内存
    for (int i = 0; i < params->num_allocations; i += 2) {
        if (pointers[i]) {
            mem_free(pointers[i]);
            pointers[i] = NULL;
        }
    }
    
    // 重新分配一些内存
    for (int i = 0; i < params->num_allocations; i += 2) {
        int size = (rand() % params->max_size) + 1;
        pointers[i] = mem_alloc(size);
        if (pointers[i]) {
            memset(pointers[i], params->thread_id, size);
        }
    }
    
    // 释放所有内存
    for (int i = 0; i < params->num_allocations; i++) {
        if (pointers[i]) {
            mem_free(pointers[i]);
        }
    }
    
    free(pointers);
    printf("线程 %d 完成\n", params->thread_id);
    return NULL;
}

// 测试多线程环境
void test_multithreading(){
    printf("=== 测试多线程环境 ===\n");

    mem_init();
    
    const int NUM_THREADS = 4;
    const int ALLOCS_PER_THREAD = 1000;
    const int MAX_ALLOC_SIZE = 512;
    
    pthread_t threads[NUM_THREADS];
    thread_params_t params[NUM_THREADS];

    // 创建线程
    for (int i = 0; i < NUM_THREADS; i++) {
        params[i].thread_id = i;
        params[i].num_allocations = ALLOCS_PER_THREAD;
        params[i].max_size = MAX_ALLOC_SIZE;
        
        if (pthread_create(&threads[i], NULL, thread_func, &params[i]) != 0) {
            printf("创建线程 %d 失败\n", i);
        }
    }

    // 等待所有线程完成
    for (int i = 0; i < NUM_THREADS; i++) {
        pthread_join(threads[i], NULL);
    }
    
    mem_destroy();
    printf("多线程测试完成\n\n");
}

// 测试边界情况
void test_edge_cases() {
    printf("=== 测试边界情况 ===\n");
    
    mem_init();
    
    // 测试分配0字节
    void *ptr0 = mem_alloc(0);
    printf("分配0字节: %p (应为NULL)\n", ptr0);
    
    // 测试分配非常大的内存
    void *huge_ptr = mem_alloc(1024 * 1024); // 1MB
    printf("分配1MB内存: %p\n", huge_ptr);
    if (huge_ptr) {
        memset(huge_ptr, 0xAA, 1024 * 1024);
        printf("大内存初始化成功\n");
        mem_free(huge_ptr);
    }
    
    // 测试多次分配和释放
    void *ptrs[100];
    for (int i = 0; i < 100; i++) {
        ptrs[i] = mem_alloc(64);
        if (ptrs[i]) {
            memset(ptrs[i], i, 64);
        }
    }
    
    for (int i = 0; i < 100; i++) {
        if (ptrs[i]) {
            mem_free(ptrs[i]);
        }
    }
    printf("多次分配和释放测试完成\n");
    
    // 测试realloc边界情况
    void *ptr = mem_alloc(32);
    if (ptr) {
        strcpy((char*)ptr, "Original");
        
        // realloc到更小尺寸
        void *smaller = kvs_realloc(ptr, 16);
        printf("realloc到更小尺寸: %p, 内容: %s\n", smaller, smaller ? (char*)smaller : "NULL");
        
        // realloc到更大尺寸
        void *larger = kvs_realloc(smaller, 64);
        printf("realloc到更大尺寸: %p, 内容: %s\n", larger, larger ? (char*)larger : "NULL");
        
        if (larger) {
            mem_free(larger);
        }
    }
    
    // 测试释放NULL指针
    mem_free(NULL);
    printf("释放NULL指针测试完成\n");
    
    mem_destroy();
    printf("边界情况测试完成\n\n");
}

int main() {
    printf("开始内存池测试\n\n");
    
    // 运行所有测试
    test_basic_functionality();
    // test_performance();
    //test_multithreading();
    //test_edge_cases();
    
    printf("所有测试完成!\n");
    return 0;
}

/*
gcc -o kvs_mempool_test kvs_mempool_test.c kvs_aof.c kvs_operation.c kvstore.c kvs_array.c kvs_rbtree.c kvs_hash.c kvs_skiptable.c resp_protocol.c kvs_mempool.c distributed.c  -I. -lpthread 
*/