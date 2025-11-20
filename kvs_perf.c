#include "include/kvs_api.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <sys/time.h>
#include <sys/resource.h>

#define TEST_COUNT 100000
#define MEMTEST_KEY_SIZE 16
#define MEMTEST_VALUE_SIZE 32
#define LONG_TEST_COUNT    1000000// 长时间测试的迭代次数
#define VARIED_SIZE_TEST_COUNT 50000// 变长测试的迭代次数

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

//gcc -o kvs_perf kvs_perf.c kvstore.c ntyco.c proactor.c reactor.c kvs_array.c kvs_rbtree.c kvs_hash.c kvs_skiptable.c  kvs_mempool.c distributed.c resp_protocol.c kvs_rdb.c kvs_operation.c kvs_aof.c -I ./NtyCo/core/ -L ./NtyCo/ -lntyco -lpthread -luring -ldl

//./kvs_perf

//获取进程内存使用量
long get_memory_usage(){
    struct rusage usage;
    getrusage(RUSAGE_SELF, &usage);
    return usage.ru_maxrss;//返回最大常驻集大小
}

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

// 性能测试函数 - 基础测试
void run_basic_performance_test(int use_mempool, int test_count) {
    // 初始化引擎
    init_kvengine();

    // 记录初始内存使用
    long initial_memory = get_memory_usage();
    long peak_memory = initial_memory;

    // 准备测试数据
    char** keys = malloc(test_count * sizeof(char*));
    char** values = malloc(test_count * sizeof(char*));

    // 预先生成所有测试数据
    for (int i = 0; i < test_count; i++) {
        keys[i] = malloc(MEMTEST_KEY_SIZE);
        values[i] = malloc(MEMTEST_VALUE_SIZE);
        rand_string(keys[i], MEMTEST_KEY_SIZE);
        rand_string(values[i], MEMTEST_VALUE_SIZE);
    }

    struct timeval start, end;
    double elapsed;

    // 测试SET操作
    gettimeofday(&start, NULL);
    for (int i = 0; i < test_count; i++) {
        #if ENABLE_ARRAY
            kvs_array_set(&global_array, keys[i], values[i]);
        #elif ENABLE_RBTREE
            kvs_rbtree_set(&global_rbtree, keys[i], values[i]);
        #elif ENABLE_HASH
            kvs_hash_set(&global_hash, keys[i], values[i]);
        #elif ENABLE_SKIPTABLE
            kvs_skiptable_set(&global_skiptable, keys[i], values[i]);
        #endif
        
        // 定期检查内存使用峰值
        if (i % 1000 == 0) {
            long current_memory = get_memory_usage();
            if (current_memory > peak_memory) {
                peak_memory = current_memory;
            }
        }
    }
    gettimeofday(&end, NULL);
    elapsed = (end.tv_sec - start.tv_sec) * 1000.0;
    elapsed += (end.tv_usec - start.tv_usec) / 1000.0;
    double set_throughput = test_count / elapsed;

    // 最终内存检查
    long final_memory = get_memory_usage();
    if (final_memory > peak_memory) {
        peak_memory = final_memory;
    }

    // 测试GET操作
    gettimeofday(&start, NULL);
    for (int i = 0; i < test_count; i++) {
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
    double get_throughput = test_count / elapsed;

    printf("%s Performance:\n", use_mempool ? "MEMPOOL" : "MALLOC");
    printf("  SET %d ops: %.2f ms (%.2f ops/ms)\n", 
           test_count, elapsed, set_throughput);
    printf("  GET %d ops: %.2f ms (%.2f ops/ms)\n", 
           test_count, elapsed, get_throughput);
    printf("  Memory Usage: Initial=%ld KB, Peak=%ld KB, Delta=%ld KB\n", 
           initial_memory, peak_memory, peak_memory - initial_memory);
    
    // 清理测试数据
    for (int i = 0; i < test_count; i++) {
        free(keys[i]);
        free(values[i]);
    }
    free(keys);
    free(values);
    
    // 销毁引擎
    dest_kvengine();
}

// 变长数据性能测试
void run_varied_size_test(int use_mempool) {
    printf("\n===== Varied Size Test =====\n");

    // 初始化引擎
    init_kvengine();

    long initial_memory = get_memory_usage();

    // 测试不同大小的数据
    int sizes[] = {16, 32, 64, 128, 256, 512, 1024, 2048};
    int num_sizes = sizeof(sizes) / sizeof(sizes[0]);

    struct timeval start, end;

    for (int i = 0; i < num_sizes; i++) {
        int data_size = sizes[i];
        int test_count = VARIED_SIZE_TEST_COUNT / (data_size / 16 + 1); // 调整测试次数
        
        char** keys = malloc(test_count * sizeof(char*));
        char** values = malloc(test_count * sizeof(char*));

        // 生成测试数据
        for (int j = 0; j < test_count; j++) {
            keys[j] = malloc(MEMTEST_KEY_SIZE);
            values[j] = malloc(data_size);
            rand_string(keys[j], MEMTEST_KEY_SIZE);
            rand_string(values[j], data_size);
        }

        // 测试SET操作
        gettimeofday(&start, NULL);
        for (int j = 0; j < test_count; j++) {
            #if ENABLE_ARRAY
                kvs_array_set(&global_array, keys[j], values[j]);
            #elif ENABLE_RBTREE
                kvs_rbtree_set(&global_rbtree, keys[j], values[j]);
            #elif ENABLE_HASH
                kvs_hash_set(&global_hash, keys[j], values[j]);
            #elif ENABLE_SKIPTABLE
                kvs_skiptable_set(&global_skiptable, keys[j], values[j]);
            #endif
        }

        gettimeofday(&end, NULL);
        double elapsed = (end.tv_sec - start.tv_sec) * 1000.0;
        elapsed += (end.tv_usec - start.tv_usec) / 1000.0;
        
        printf("%s Size %d: %d ops in %.2f ms (%.2f ops/ms)\n", 
               use_mempool ? "MEMPOOL" : "MALLOC",
               data_size, test_count, elapsed, test_count / elapsed);
        
        // 清理
        for (int j = 0; j < test_count; j++) {
            free(keys[j]);
            free(values[j]);
        }
        free(keys);
        free(values);
    }

    long final_memory = get_memory_usage();
    printf("Memory Delta: %ld KB\n", final_memory - initial_memory);
    
    // 销毁引擎
    // dest_kvengine();
}

// 长时间运行稳定性测试
void run_long_term_test(int use_mempool) {
    printf("\n===== Long Term Stability Test =====\n");
    
    // 初始化引擎
    init_kvengine();
    
    long initial_memory = get_memory_usage();
    long peak_memory = initial_memory;
    
    struct timeval start, end;
    gettimeofday(&start, NULL);
    
    // 长时间运行测试
    for (int i = 0; i < LONG_TEST_COUNT; i++) {
        char key[MEMTEST_KEY_SIZE];
        char value[MEMTEST_VALUE_SIZE];
        
        rand_string(key, MEMTEST_KEY_SIZE);
        rand_string(value, MEMTEST_VALUE_SIZE);
        
        // 交替进行SET和GET操作
        if (i % 2 == 0) {
            #if ENABLE_ARRAY
                kvs_array_set(&global_array, key, value);
            #elif ENABLE_RBTREE
                kvs_rbtree_set(&global_rbtree, key, value);
            #elif ENABLE_HASH
                kvs_hash_set(&global_hash, key, value);
            #elif ENABLE_SKIPTABLE
                kvs_skiptable_set(&global_skiptable, key, value);
            #endif
        } else {
            #if ENABLE_ARRAY
                kvs_array_get(&global_array, key);
            #elif ENABLE_RBTREE
                kvs_rbtree_get(&global_rbtree, key);
            #elif ENABLE_HASH
                kvs_hash_get(&global_hash, key);
            #elif ENABLE_SKIPTABLE
                kvs_skiptable_get(&global_skiptable, key);
            #endif
        }
        
        // 定期检查内存使用
        if (i % 10000 == 0) {
            long current_memory = get_memory_usage();
            if (current_memory > peak_memory) {
                peak_memory = current_memory;
            }
            
            if (i % 100000 == 0) {
                printf("Progress: %d/%d operations, Memory: %ld KB\n", 
                       i, LONG_TEST_COUNT, current_memory);
            }
        }
    }
    
    gettimeofday(&end, NULL);
    double elapsed = (end.tv_sec - start.tv_sec) * 1000.0;
    elapsed += (end.tv_usec - start.tv_usec) / 1000.0;
    
    long final_memory = get_memory_usage();
    if (final_memory > peak_memory) {
        peak_memory = final_memory;
    }
    
    printf("%s Long Term Test:\n", use_mempool ? "MEMPOOL" : "MALLOC");
    printf("  Completed %d ops in %.2f ms (%.2f ops/ms)\n", 
           LONG_TEST_COUNT, elapsed, LONG_TEST_COUNT / elapsed);
    printf("  Memory Usage: Initial=%ld KB, Peak=%ld KB, Final=%ld KB\n", 
           initial_memory, peak_memory, final_memory);
    printf("  Memory Leak: %ld KB\n", final_memory - initial_memory);
    
    // 销毁引擎
    // dest_kvengine();
}

// 综合性能测试
void run_comprehensive_test(int use_mempool) {
    printf("\n===== %s Comprehensive Test =====\n", 
           use_mempool ? "MEMPOOL" : "MALLOC");
    
    // 1. 基础性能测试
    printf("\n--- Basic Performance Test ---\n");
    run_basic_performance_test(use_mempool, TEST_COUNT);
    
    // 2. 变长数据测试
    run_varied_size_test(use_mempool);
    
    // 3. 长时间稳定性测试
    run_long_term_test(use_mempool);
}

int main() {
    // 设置随机种子
    srand(time(NULL));
    
    printf("KV Store Memory Management Performance Comparison\n");
    printf("=================================================\n");
    
    // 测试内存池版本
    run_comprehensive_test(1);
    
    // 测试malloc/free版本
    run_comprehensive_test(0);
    
    // 输出总结比较
    printf("\n===== Summary Comparison =====\n");
    printf("Note: Run the full test to see detailed comparison data\n");
    printf("Comparison metrics:\n");
    printf("1. Operation throughput (ops/ms)\n");
    printf("2. Memory efficiency (peak usage)\n");
    printf("3. Memory fragmentation (long-term stability)\n");
    printf("4. Performance with varied data sizes\n");
    printf("5. Long-term memory leak detection\n");
    
    return 0;
}