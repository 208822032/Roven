#include "kvstore.h"
#include <sys/time.h>
#include <sys/resource.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <fcntl.h>
#include <math.h>

// 获取内存使用情况
void get_memory_usage(size_t *vss, size_t *rss) {
    FILE *f = fopen("/proc/self/statm", "r");
    if (f) {
        unsigned long vm, r;
        fscanf(f, "%lu %lu", &vm, &r);
        *vss = vm * getpagesize() / 1024; // KB
        *rss = r * getpagesize() / 1024;  // KB
        fclose(f);
    } else {
        *vss = 0;
        *rss = 0;
    }
}

// 测试结果结构体
typedef struct {
    long time_used;      // 测试用时(μs)
    size_t initial_rss;  // 初始内存(KB)
    size_t peak_rss;     // 峰值内存(KB)
    size_t final_rss;    // 结束内存(KB)
    size_t frag_factor;  // 碎片因子(越大表示碎片越多)
    int large_success;   // 大内存分配是否成功
} TestResult;

// 运行单个测试场景
TestResult run_test_scenario(int use_mempool, int allocation_pattern) {
    const int NUM_ALLOCATIONS = 10000;
    const int FREE_INTERVAL = 2;
    const size_t LARGE_BLOCK_SIZE = 1024 * 1024; // 1MB

    TestResult result = {0};
    size_t initial_vss, initial_rss;
    get_memory_usage(&initial_vss, &initial_rss);
    result.initial_rss = initial_rss;

    void **pointers = malloc(NUM_ALLOCATIONS * sizeof(void*));
    memset(pointers, 0, NUM_ALLOCATIONS * sizeof(void*));

    // 生成分配大小数组
    size_t *alloc_sizes = malloc(NUM_ALLOCATIONS * sizeof(size_t));
    for (int i = 0; i < NUM_ALLOCATIONS; i++) {
        switch (allocation_pattern) {
            case 0: // 随机大小
                alloc_sizes[i] = 16 + (rand() % 2032); // 16-2048字节
                break;
            case 1: // 小内存块
                alloc_sizes[i] = 16 + (rand() % 48); // 16-64字节
                break;
            case 2: // 大内存块
                alloc_sizes[i] = 512 + (rand() % 1536); // 512-2048字节
                break;
        }
    }

    struct timeval start, end;
    gettimeofday(&start, NULL);

    // 第一次分配
    for (int i = 0; i < NUM_ALLOCATIONS; i++) {
        if (use_mempool) {
            pointers[i] = mem_alloc(alloc_sizes[i]);
        } else {
            pointers[i] = malloc(alloc_sizes[i]);
        }
        if (pointers[i]) {
            memset(pointers[i], 0, alloc_sizes[i]);
        }
    }

    // 间隔释放
    for (int i = 0; i < NUM_ALLOCATIONS; i += FREE_INTERVAL) {
        if (pointers[i]) {
            if (use_mempool) {
                mem_free(pointers[i]);
            } else {
                free(pointers[i]);
            }
            pointers[i] = NULL;
        }
    }

    // 第二次分配（填补释放的空位）
    for (int i = 0; i < NUM_ALLOCATIONS; i += FREE_INTERVAL) {
        if (pointers[i] == NULL) {
            if (use_mempool) {
                pointers[i] = mem_alloc(alloc_sizes[i]);
            } else {
                pointers[i] = malloc(alloc_sizes[i]);
            }
            if (pointers[i]) {
                memset(pointers[i], 0, alloc_sizes[i]);
            }
        }
    }

    size_t mid_vss, mid_rss;
    get_memory_usage(&mid_vss, &mid_rss);
    result.peak_rss = mid_rss;

    // 尝试分配大内存块
    void *large_block = NULL;
    if (use_mempool) {
        large_block = mem_alloc(LARGE_BLOCK_SIZE);
    } else {
        large_block = malloc(LARGE_BLOCK_SIZE);
    }
    result.large_success = (large_block != NULL) ? 1 : 0;

    // 计算碎片因子 = (实际分配内存 - 有效数据内存) / 有效数据内存
    size_t total_allocated = 0;
    size_t total_used = 0;
    for (int i = 0; i < NUM_ALLOCATIONS; i++) {
        if (pointers[i]) {
            total_used += alloc_sizes[i];
            // 估算实际分配内存
            if (use_mempool) {
                // 内存池有额外开销
                total_allocated += alloc_sizes[i] + sizeof(mem_block_header_t);
            } else {
                // 系统malloc有额外开销
                total_allocated += alloc_sizes[i] + sizeof(size_t) * 2;
            }
        }
    }
    if (large_block) total_allocated += LARGE_BLOCK_SIZE + sizeof(mem_block_header_t);
    if (total_used > 0) {
        result.frag_factor = (total_allocated - total_used) * 100 / total_used;
    } else {
        result.frag_factor = 0;
    }

    // 清理
    for (int i = 0; i < NUM_ALLOCATIONS; i++) {
        if (pointers[i]) {
            if (use_mempool) {
                mem_free(pointers[i]);
            } else {
                free(pointers[i]);
            }
        }
    }
    
    if (large_block) {
        if (use_mempool) {
            mem_free(large_block);
        } else {
            free(large_block);
        }
    }

    free(pointers);
    free(alloc_sizes);

    gettimeofday(&end, NULL);
    result.time_used = (end.tv_sec - start.tv_sec) * 1000000 + 
                      (end.tv_usec - start.tv_usec);

    size_t final_vss, final_rss;
    get_memory_usage(&final_vss, &final_rss);
    result.final_rss = final_rss;

    return result;
}

// 计算并显示提升百分比
void print_improvement(TestResult pooled, TestResult system, const char* pattern_name) {
    // 计算时间提升百分比
    double time_improvement = 0.0;
    if (system.time_used > 0) {
        time_improvement = (1.0 - (double)pooled.time_used / system.time_used) * 100.0;
    }
    
    // 计算峰值内存提升百分比
    double mem_improvement = 0.0;
    if (system.peak_rss > 0) {
        mem_improvement = (1.0 - (double)pooled.peak_rss / system.peak_rss) * 100.0;
    }
    
    // 计算碎片减少百分比
    double frag_improvement = 0.0;
    if (system.frag_factor > 0) {
        frag_improvement = (1.0 - (double)pooled.frag_factor / system.frag_factor) * 100.0;
    }
    
    // 计算大内存分配成功率提升
    double success_improvement = 0.0;
    if (!system.large_success && pooled.large_success) {
        success_improvement = 100.0; // 从失败到成功
    } else if (system.large_success && pooled.large_success) {
        success_improvement = 0.0; // 两者都成功
    } else {
        success_improvement = -100.0; // 从成功到失败(理论上不应该发生)
    }
    
    printf("\n=== %s 模式性能提升 ===\n", pattern_name);
    printf("时间效率:    %.2f%% (池化: %ldμs vs 系统: %ldμs)\n", 
           time_improvement, pooled.time_used, system.time_used);
    printf("内存使用:    %.2f%% (池化: %ldKB vs 系统: %ldKB)\n", 
           mem_improvement, pooled.peak_rss, system.peak_rss);
    printf("碎片减少:    %.2f%% (池化: %ld%% vs 系统: %ld%%)\n", 
           frag_improvement, pooled.frag_factor, system.frag_factor);
    printf("大块分配:    %s (池化: %s vs 系统: %s)\n",
           success_improvement > 0 ? "成功" : "无变化",
           pooled.large_success ? "成功" : "失败",
           system.large_success ? "成功" : "失败");
    
    // 显示性能总结
    printf("\n[性能总结]\n");
    printf("内存池在 %s 场景下:\n", pattern_name);
    printf("  - 分配速度提升: %s%.2f%%\n", 
           time_improvement > 0 ? "+" : "", time_improvement);
    printf("  - 内存占用减少: %s%.2f%%\n", 
           mem_improvement > 0 ? "+" : "", mem_improvement);
    printf("  - 内存碎片减少: %s%.2f%%\n", 
           frag_improvement > 0 ? "+" : "", frag_improvement);
    printf("  - 大块分配成功率: %s\n", 
           success_improvement > 0 ? "提升" : (success_improvement == 0 ? "相同" : "下降"));
    printf("----------------------------------------\n");
}

int main() {
    srand(time(NULL)); // 随机种子
    
    const char* patterns[] = {"混合大小", "小内存块", "大内存块"};
    
    for (int pattern = 0; pattern < 3; pattern++) {
        // 初始化内存池
        mem_init();
        
        // 测试内存池版本
        TestResult pooled = run_test_scenario(1, pattern);
        
        // 销毁内存池
        mem_destroy();
        
        // 测试系统malloc版本
        TestResult system = run_test_scenario(0, pattern);
        
        // 显示提升百分比
        print_improvement(pooled, system, patterns[pattern]);
    }
    
    return 0;
}