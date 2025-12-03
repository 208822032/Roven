#define _GNU_SOURCE
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>
#include <unistd.h>
#include <sys/resource.h>
#include <sys/time.h>

#include "kvstore.h"  // kvs_malloc/kvs_free & mem_init/mem_destroy

#define ALLOC_TIMES      200000
#define SMALL_SIZE       24
#define MID_SIZE         200
#define LARGE_SIZE       800
#define HUGE_SIZE        4096

#define TIME_SUB_MS(tv1, tv2)  (((tv1).tv_sec - (tv2).tv_sec) * 1000 + ((tv1).tv_usec - (tv2).tv_usec) / 1000)

static void print_mem_usage(const char *tag)
{
    struct rusage ru;
    if (getrusage(RUSAGE_SELF, &ru) == 0) {
        // ru_maxrss: Linux 下一般是 KB
        printf("[%s] maxrss(KB)=%ld, minor_faults=%ld, major_faults=%ld\n",
               tag, ru.ru_maxrss, ru.ru_minflt, ru.ru_majflt);
    } else {
        perror("getrusage");
    }
}

static void *xmalloc(size_t size)
{
    void *p = kvs_malloc(size);
    if (!p) {
        fprintf(stderr, "kvs_malloc(%zu) failed\n", size);
        exit(1);
    }
    return p;
}

static void pressure_pattern(const char *name, size_t size, int keep_rate)
{
    printf("\n==== pattern: %s, size=%zu, keep_rate=%d%% ====\n", name, size, keep_rate);

    void **ptrs = (void**)xmalloc(sizeof(void*) * ALLOC_TIMES);
    memset(ptrs, 0, sizeof(void*) * ALLOC_TIMES);

    struct timeval tv_begin, tv_end;
    gettimeofday(&tv_begin, NULL);

    for (int i = 0; i < ALLOC_TIMES; ++i) {
        ptrs[i] = xmalloc(size);
        memset(ptrs[i], 0xA5, size < 16 ? size : 16);

        // 根据 keep_rate 决定是否马上 free
        if (keep_rate < 100) {
            int r = rand() % 100;
            if (r >= keep_rate) {
                kvs_free(ptrs[i]);
                ptrs[i] = NULL;
            }
        }
    }

    gettimeofday(&tv_end, NULL);
    int time_used = TIME_SUB_MS(tv_end, tv_begin);

    size_t alive = 0;
    for (int i = 0; i < ALLOC_TIMES; ++i) {
        if (ptrs[i]) alive++;
    }

    print_mem_usage("after alloc");
    printf("pattern %s: alloc_times=%d, alive=%zu, time_used=%d ms\n",
           name, ALLOC_TIMES, alive, time_used);

    // 释放所有保留的块
    for (int i = 0; i < ALLOC_TIMES; ++i) {
        if (ptrs[i]) kvs_free(ptrs[i]);
    }
    kvs_free(ptrs);

    print_mem_usage("after free");
}

// 为汇总准备的简单统计结构
typedef struct {
    const char *backend;
    int small_fixed_time_ms;
    long small_fixed_maxrss;
    int mid_fixed_time_ms;
    long mid_fixed_maxrss;
    double qps_small;
    double read_heavy_small_ops_s;
    int bigprobe_time_ms;
    long bigprobe_maxrss;
} mempool_summary_t;

static mempool_summary_t g_summary;

// 简单 ops/s 压测：每次分配+写入+释放，统计吞吐
static void perf_qps_test(const char *name, size_t size, int ops)
{
    struct timeval tv_begin, tv_end;
    gettimeofday(&tv_begin, NULL);

    for (int i = 0; i < ops; ++i) {
        void *p = xmalloc(size);
        // 简单写入，避免被优化掉
        memset(p, 0x5A, size < 32 ? size : 32);
        kvs_free(p);
    }

    gettimeofday(&tv_end, NULL);
    int time_used = TIME_SUB_MS(tv_end, tv_begin);
    if (time_used <= 0) time_used = 1;
    double qps = (double)ops * 1000.0 / (double)time_used;
    printf("\n==== QPS test: %s, size=%zu, ops=%d ===="
           "\n time_used=%d ms, qps=%.2f ops/s\n",
           name, size, ops, time_used, qps);

    if (strcmp(name, "qps-small") == 0) {
        g_summary.qps_small = qps;
    }
}

// 读多写少混合：预先分配一批对象，多数只是读/轻改，少数 free+重新分配
static void read_heavy_pattern(const char *name, size_t size,
                int pool_size, int rounds,
                int write_ratio/*百分比 0-100*/)
{
    printf("\n==== read-heavy pattern: %s, size=%zu, pool=%d, rounds=%d, write_ratio=%d%% ====\n",
           name, size, pool_size, rounds, write_ratio);

    void **pool = (void**)xmalloc(sizeof(void*) * pool_size);
    for (int i = 0; i < pool_size; ++i) {
        pool[i] = xmalloc(size);
        memset(pool[i], 0x11, size < 16 ? size : 16);
    }

    struct timeval tv_begin, tv_end;
    gettimeofday(&tv_begin, NULL);

    int total_ops = 0;
    for (int r = 0; r < rounds; ++r) {
        for (int i = 0; i < pool_size; ++i) {
            unsigned char *p = (unsigned char*)pool[i];
            // 读 + 轻微写，模拟热点读
            volatile unsigned char tmp = p[0];
            (void)tmp;
            p[0] ^= 0x1;
            p[1] ^= 0x2;
            ++total_ops;

            // 按比例做写操作：释放并重新分配
            int rnum = rand() % 100;
            if (rnum < write_ratio) {
                kvs_free(pool[i]);
                pool[i] = xmalloc(size);
                memset(pool[i], 0x22, size < 16 ? size : 16);
                ++total_ops;
            }
        }
    }

    gettimeofday(&tv_end, NULL);
    int time_used = TIME_SUB_MS(tv_end, tv_begin);
    if (time_used <= 0) time_used = 1;
    double ops_per_sec = (double)total_ops * 1000.0 / (double)time_used;

    print_mem_usage("read-heavy after rounds");
    printf("read-heavy %s: total_ops=%d, time_used=%d ms, ops/s=%.2f\n",
           name, total_ops, time_used, ops_per_sec);

    if (strcmp(name, "read-heavy-small") == 0) {
        g_summary.read_heavy_small_ops_s = ops_per_sec;
    }

    for (int i = 0; i < pool_size; ++i) {
        if (pool[i]) kvs_free(pool[i]);
    }
    kvs_free(pool);
    print_mem_usage("read-heavy after free");
}

// 在碎片场景之后探测一次大块分配能力
static void big_block_probe(const char *tag, size_t big_size)
{
    struct timeval tv_begin, tv_end;
    gettimeofday(&tv_begin, NULL);
    void *p = kvs_malloc(big_size);
    gettimeofday(&tv_end, NULL);
    int time_used = TIME_SUB_MS(tv_end, tv_begin);
    print_mem_usage(tag);
    printf("big-block probe: size=%zu, time_used=%d ms, ptr=%p\n",
           big_size, time_used, p);
    if (strcmp(tag, "big-after-fragment") == 0) {
        struct rusage ru;
        if (getrusage(RUSAGE_SELF, &ru) == 0) {
            g_summary.bigprobe_time_ms = time_used;
            g_summary.bigprobe_maxrss = ru.ru_maxrss;
        }
    }
    if (p) kvs_free(p);
}

// 自定义压测脚本：
// 单次写 n(1000000) 两次 -> 删除 n -> 再写 n 一次 -> 最后删除 2n
// 用于测试不同内存池的 QPS 和虚拟内存占用
static void custom_pattern_qps_vm(const char *tag, size_t size, int n)
{
    printf("\n==== custom pattern: %s, size=%zu, n=%d ====\n", tag, size, n);

    struct timeval tv_begin, tv_end;
    struct rusage ru;

    void **arr1 = (void**)xmalloc(sizeof(void*) * n);
    void **arr2 = (void**)xmalloc(sizeof(void*) * n);

    memset(arr1, 0, sizeof(void*) * n);
    memset(arr2, 0, sizeof(void*) * n);

    // step1: 写 n 次（第一批）
    gettimeofday(&tv_begin, NULL);
    for (int i = 0; i < n; ++i) {
        arr1[i] = xmalloc(size);
        memset(arr1[i], 0x3C, size < 32 ? size : 32);
    }

    // step2: 再写 n 次（第二批）
    for (int i = 0; i < n; ++i) {
        arr2[i] = xmalloc(size);
        memset(arr2[i], 0x4D, size < 32 ? size : 32);
    }

    // step3: 删除 n 个（删掉第一批）
    for (int i = 0; i < n; ++i) {
        if (arr1[i]) {
            kvs_free(arr1[i]);
            arr1[i] = NULL;
        }
    }

    // step4: 再写 n 次（第三批）
    for (int i = 0; i < n; ++i) {
        arr1[i] = xmalloc(size);
        memset(arr1[i], 0x5E, size < 32 ? size : 32);
    }

    // step5: 删除 2n 个（第二批 + 第三批）
    for (int i = 0; i < n; ++i) {
        if (arr2[i]) {
            kvs_free(arr2[i]);
            arr2[i] = NULL;
        }
    }
    for (int i = 0; i < n; ++i) {
        if (arr1[i]) {
            kvs_free(arr1[i]);
            arr1[i] = NULL;
        }
    }

    gettimeofday(&tv_end, NULL);
    int time_used = TIME_SUB_MS(tv_end, tv_begin);
    if (time_used <= 0) time_used = 1;

    // 总操作数：三次分配 + 三次释放，共 6n 次操作
    long long total_ops = (long long)n * 6;
    double qps = (double)total_ops * 1000.0 / (double)time_used;

    if (getrusage(RUSAGE_SELF, &ru) == 0) {
        printf("[custom-pattern] tag=%s maxrss(KB)=%ld, minor_faults=%ld, major_faults=%ld\n",
               tag, ru.ru_maxrss, ru.ru_minflt, ru.ru_majflt);
    }

    printf("custom pattern %s: size=%zu, n=%d\n"
           "  total_ops=%lld, time_used=%d ms, qps=%.2f ops/s\n",
           tag, size, n, total_ops, time_used, qps);

    kvs_free(arr1);
    kvs_free(arr2);
}

int main(int argc, char **argv)
{
    // 仅测试内存分配：显式选择内存池后端并初始化
    printf("mempool_test: ALLOC_TIMES=%d\n", ALLOC_TIMES);

    // 为 mem_init 提供最小 g_cfg，并强制使用内存池后端
    extern kvs_config_t g_cfg;
    memset(&g_cfg, 0, sizeof(g_cfg));
    //KVS_MEM_BACKEND_POOL||KVS_MEM_BACKEND_SYSTEM||KVS_MEM_BACKEND_JEMALLOC
    g_cfg.mem_backend = KVS_MEM_BACKEND_JEMALLOC;

    // 初始化汇总结构中的 backend 名称
    switch (g_cfg.mem_backend) {
    case KVS_MEM_BACKEND_POOL:   g_summary.backend = "POOL"; break;
    case KVS_MEM_BACKEND_SYSTEM: g_summary.backend = "SYSTEM"; break;
    case KVS_MEM_BACKEND_JEMALLOC: g_summary.backend = "JEMALLOC"; break;
    default: g_summary.backend = "UNKNOWN"; break;
    }

    mem_init();

    print_mem_usage("start");

    srand((unsigned int)getpid());

    // 1. 小块分配（典型走内存池小块）
    pressure_pattern("small-fixed", SMALL_SIZE, 100);

    // 记录 small-fixed 汇总信息
    {
        struct rusage ru;
        if (getrusage(RUSAGE_SELF, &ru) == 0) {
            // 这里只能近似记录当前 maxrss；
            // time_ms 已在 pressure_pattern 打印，简单不再重复精确拆分
            g_summary.small_fixed_maxrss = ru.ru_maxrss;
        }
    }

    // 2. 中等块分配
    pressure_pattern("mid-fixed", MID_SIZE, 100);

    {
        struct rusage ru;
        if (getrusage(RUSAGE_SELF, &ru) == 0) {
            g_summary.mid_fixed_maxrss = ru.ru_maxrss;
        }
    }

    // 3. 大块但仍在 pool 阈值以内
    pressure_pattern("large-fixed", LARGE_SIZE, 100);

    // 4. 超过 pool 阈值，走大块分配逻辑
    pressure_pattern("huge-fixed", HUGE_SIZE, 100);

    // 5. 小块 + 部分长期保留，模拟碎片
    pressure_pattern("small-keep50", SMALL_SIZE, 50);

    // 6. 中块 + 部分长期保留
    pressure_pattern("mid-keep30", MID_SIZE, 30);

    // 在碎片场景后尝试一次大块分配，观察是否有明显差异
    big_block_probe("big-after-fragment", 4 * 1024 * 1024);

    // ops/s 压测：小块/中块
    perf_qps_test("qps-small", 64, 1000000);
    perf_qps_test("qps-mid", 256, 1000000);

    // 读多写少混合模式：80% 读，20% 写
    read_heavy_pattern("read-heavy-small", 64, 100000, 5, 20);
    read_heavy_pattern("read-heavy-mid", 256, 50000, 5, 20);

    // 自定义脚本：单次写1000000两次 删除1000000
    // 再写1000000一次 最后删除2000000，用于测 QPS 和虚拟内存
    custom_pattern_qps_vm("custom-small-1M", 64, 1000000);

    mem_destroy();

    print_mem_usage("end");

    // 汇总展示
    printf("\n===== SUMMARY (for BACKEND=%s) =====\n", g_summary.backend ? g_summary.backend : "UNKNOWN");
    printf("PATTERN  small-fixed      maxrss=%ld KB\n", g_summary.small_fixed_maxrss);
    printf("PATTERN  mid-fixed        maxrss=%ld KB\n", g_summary.mid_fixed_maxrss);
    printf("QPS      qps-small        qps=%.2f ops/s\n", g_summary.qps_small);
    printf("READHVY  read-heavy-small ops/s=%.2f\n", g_summary.read_heavy_small_ops_s);
    printf("BIGPROBE big-after-fragment time=%d ms maxrss=%ld KB\n",
           g_summary.bigprobe_time_ms, g_summary.bigprobe_maxrss);
    printf("======================================\n");

    return 0;
}
