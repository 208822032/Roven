#include "kvstore.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <sys/time.h>

#define TEST_COUNT 100000
#define MEMTEST_KEY_SIZE 16
#define MEMTEST_VALUE_SIZE 32

#if ENABLE_ARRAY
extern kvs_array_t global_array;
#endif

#if ENABLE_RBTREE
extern kvs_rbtree_t global_rbtree;
#endif

#if ENABLE_HASH
extern kvs_hash_t global_hash;
#endif

#if ENABLE_SKIPTABLE
extern kvs_skiptable_t global_skiptable;
#endif

//gcc -o kvs_perf kvs_perf.c kvs_array.c kvs_hash.c kvs_rbtree.c kvstore.c ntyco.c proactor.c reactor.c kvs_mempool.c kvs_skiptable.c -I ./NtyCo/core/ -L ./NtyCo/ -lntyco -lpthread -luring -ldl
//./kvs_perf

// 生成随机字符串
char* rand_string(char *str, size_t size) {
    const char charset[] = "abcdefghijklmnopqrstuvwxyzABCDEFGHIJKLMNOPQRSTUVWXYZ0123456789";
    if (size) {
        --size;
        for (size_t n = 0; n < size; n++) {
            int key = rand() % (int)(sizeof(charset) - 1);
            str[n] = charset[key];
        }
        str[size] = '\0';
    }
    return str;
}

// 性能测试函数
void run_performance_test(int use_mempool) {
    // 初始化引擎
    init_kvengine();
    
    // 准备测试数据
    char** keys = malloc(TEST_COUNT * sizeof(char*));
    char** values = malloc(TEST_COUNT * sizeof(char*));
    
    // 预先生成所有测试数据
    for (int i = 0; i < TEST_COUNT; i++) {
        keys[i] = malloc(MEMTEST_KEY_SIZE);
        values[i] = malloc(MEMTEST_VALUE_SIZE);
        rand_string(keys[i], MEMTEST_KEY_SIZE);
        rand_string(values[i], MEMTEST_VALUE_SIZE);
    }
    
    struct timeval start, end;
    double elapsed;
    
    // 测试SET操作
    gettimeofday(&start, NULL);
    for (int i = 0; i < TEST_COUNT; i++) {
        #if ENABLE_ARRAY
            kvs_array_set(&global_array, keys[i], values[i]);
        #elif ENABLE_RBTREE
            kvs_rbtree_set(&global_rbtree, keys[i], values[i]);
        #elif ENABLE_HASH
            kvs_hash_set(&global_hash, keys[i], values[i]);
        #elif ENABLE_SKIPTABLE
            kvs_skiptable_set(&global_skiptable, keys[i], values[i]);
        #endif
    }
    gettimeofday(&end, NULL);
    elapsed = (end.tv_sec - start.tv_sec) * 1000.0;
    elapsed += (end.tv_usec - start.tv_usec) / 1000.0;
    printf("%s SET %d ops: %.2f ms (%.2f ops/ms)\n", 
           use_mempool ? "MEMPOOL" : "MALLOC",
           TEST_COUNT, elapsed, TEST_COUNT / elapsed);
    
    // 测试GET操作
    gettimeofday(&start, NULL);
    for (int i = 0; i < TEST_COUNT; i++) {
        #if ENABLE_ARRAY
            kvs_array_get(&global_array, keys[i]);
        #elif ENABLE_RBTREE
            kvs_rbtree_get(&global_rbtree, keys[i]);
        #elif ENABLE_HASH
            kvs_hash_get(&global_hash, keys[i]);
        #elif ENABLE_SKIPTABLE
            kvs_skiptable_get(&global_skiptable, keys[i]);
        #endif
    }
    gettimeofday(&end, NULL);
    elapsed = (end.tv_sec - start.tv_sec) * 1000.0;
    elapsed += (end.tv_usec - start.tv_usec) / 1000.0;
    printf("%s GET %d ops: %.2f ms (%.2f ops/ms)\n", 
           use_mempool ? "MEMPOOL" : "MALLOC",
           TEST_COUNT, elapsed, TEST_COUNT / elapsed);
    
    // 清理测试数据
    for (int i = 0; i < TEST_COUNT; i++) {
        free(keys[i]);
        free(values[i]);
    }
    free(keys);
    free(values);
    
    // 销毁引擎
    dest_kvengine();
}

int main() {
    // 设置随机种子
    srand(time(NULL));

    // 测试内存池版本
    printf("===== 自定义内存池 =====\n");
    run_performance_test(1);
    
    // 测试malloc/free版本
    printf("\n===== malloc/free =====\n");
    run_performance_test(0);
    
    return 0;
}