/*
 * vcmd_profile_bench.c — Comprehensive V-family profiling benchmark.
 *
 * Three test levels:
 *   Level 1 (ANN-core):   float* query → index_search → topK (no RESP, no network)
 *   Level 2 (VEC):        raw float vector via _with_embedding APIs (no embedding gen)
 *   Level 3 (TEXT):       full text → embedding → index → response (end-to-end)
 *
 * Four benchmark modes:
 *   VSET_TEXT, VSET_VEC, VSEARCH_TEXT, VSEARCH_VEC
 *
 * Links against libkvstore.a for direct access to vector internals.
 *
 * Build: make vcmd_profile_bench KVLITE_VCMD_PROFILE=1
 * Usage: ./build/tests/bin/vcmd_profile_bench <mode> <nb> <dim> <topk> [concurrency] [iterations]
 *
 * Mode: vset_text | vset_vec | vsearch_text | vsearch_vec | ann_core
 * Outputs: CSV with per-stage timing breakdown
 */

#define _GNU_SOURCE
#define KVS_VECTOR_INTERNAL
#include "kvs_api.h"
#include "kvs_vector.h"
#include "kvs_vcmd_profile.h"
#include "kvs_config.h"
#include "kvs_globals.h"
#include "kvs_memory.h"
#include "kvs_command_task.h"
#include "kvs_response_task.h"
#include "kvs_command_executor.h"
#include "kvs_embedding_worker_pool.h"
#include "kvs_owner_lock.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <math.h>
#include <pthread.h>
#include <unistd.h>

/* ── constants ──────────────────────────────────────────────────────── */

#define MAX_QUERY_LEN  256
#define MAX_ANSWER_LEN 512
#define DEFAULT_DIM    512
#define DEFAULT_NB     10000
#define DEFAULT_TOPK   10
#define DEFAULT_ITER   1000

/* ── globals from libkvstore ─────────────────────────────────────────── */

extern kvs_config_t g_cfg;
extern kvs_vector_state_t g_vector_state;
pthread_rwlock_t g_kv_data_rwlock = PTHREAD_RWLOCK_INITIALIZER;

/* ── random float vector ─────────────────────────────────────────────── */

static void rand_vec(float *v, int dim) {
    for (int i = 0; i < dim; i++) {
        v[i] = (float)((double)rand() / (double)RAND_MAX * 2.0 - 1.0);
    }
}

static void rand_text(char *buf, size_t cap, int idx) {
    snprintf(buf, cap, "This is a benchmark query text number %d for semantic search testing", idx);
}

/* ── timing ──────────────────────────────────────────────────────────── */

static int cmp_u64(const void *pa, const void *pb);

static uint64_t now_us(void) {
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (uint64_t)ts.tv_sec * 1000000ULL + (uint64_t)ts.tv_nsec / 1000ULL;
}

/* ══════════════════════════════════════════════════════════════════════
 * Level 1: ANN-core benchmark
 * Direct float* → index_search → topK, no RESP/network/embedding/KV
 * ══════════════════════════════════════════════════════════════════════ */

static int bench_ann_core(int nb, int dim, int topk, int iterations) {
    kvs_vector_index_t *index;
    float *query;
    kvs_vector_search_result_t *results;
    size_t results_count;
    uint64_t *add_times, *search_times;
    uint64_t t_start, t_end;
    int i, ret;
    double add_qps, search_qps;
    uint64_t add_total = 0, search_total = 0;
    uint64_t add_max = 0, search_max = 0, search_min = UINT64_MAX;

    printf("\n=== Level 1: ANN-core Benchmark ===\n");
    printf("  nb=%d dim=%d topk=%d iterations=%d\n", nb, dim, topk, iterations);
    printf("  index_type=%s metric=%s\n",
           g_cfg.vector_index_type ? g_cfg.vector_index_type : "flat",
           kvs_vector_metric_name(g_vector_state.metric_type));

    query = calloc((size_t)dim, sizeof(float));
    results = calloc((size_t)topk, sizeof(kvs_vector_search_result_t));
    add_times = calloc((size_t)nb, sizeof(uint64_t));
    search_times = calloc((size_t)iterations, sizeof(uint64_t));

    if (!query || !results || !add_times || !search_times) {
        fprintf(stderr, "OOM\n");
        ret = -1;
        goto cleanup;
    }

    /* Reset state */
    kvs_vector_reset_for_load();
    index = &g_vector_state.current_index;

    /* Phase 1: Add vectors via proper record creation */
    printf("\n  Phase 1: Adding %d vectors (via VSET commit path)...\n", nb);
    for (i = 0; i < nb; i++) {
        kvs_vector_set_prepared_t prep;
        char q[128], a[128];
        rand_vec(query, dim);
        snprintf(q, sizeof(q), "ann_core_q_%d", i);
        snprintf(a, sizeof(a), "ann_core_a_%d", i);

        kvs_vector_set_prepared_reset(&prep);
        ret = kvs_vector_set_prepare_with_embedding(
            q, strlen(q), a, strlen(a), NULL, 0, query, dim, &prep);
        if (ret != KVS_VECTOR_OK) { fprintf(stderr, "  prepare[%d] failed: %d\n", i, ret); goto cleanup; }

        t_start = now_us();
        kvs_owner_rwlock_wrlock(&g_kv_data_rwlock, "ann_core_add");
        ret = kvs_vector_set_commit_prepared(&prep);
        kvs_owner_rwlock_unlock(&g_kv_data_rwlock, "ann_core_add");
        t_end = now_us();
        kvs_vector_set_prepared_destroy(&prep);

        if (ret != KVS_VECTOR_OK) { fprintf(stderr, "  commit[%d] failed: %d\n", i, ret); goto cleanup; }

        add_times[i] = t_end - t_start;
        add_total += add_times[i];
        if (add_times[i] > add_max) add_max = add_times[i];
    }
    add_qps = (double)nb / ((double)add_total / 1000000.0);
    printf("  Add complete: total=%lu us avg=%lu us max=%lu us QPS=%.1f\n",
           (unsigned long)add_total, (unsigned long)(add_total / (uint64_t)nb),
           (unsigned long)add_max, add_qps);

    /* Phase 2: Search vectors — pure index search, no embedding */
    printf("\n  Phase 2: Searching %d queries (pure index API, no embedding)...\n", iterations);
    for (i = 0; i < iterations; i++) {
        rand_vec(query, dim);
        kvs_vector_search_results_reset(results, (size_t)topk);
        t_start = now_us();
        ret = kvs_vector_index_search_current(
            query, dim, NULL, 0, topk, -1.0f,
            results, (size_t)topk, &results_count);
        t_end = now_us();
        if (ret != KVS_VECTOR_OK) {
            fprintf(stderr, "  search[%d] failed: %d\n", i, ret);
            goto cleanup;
        }
        search_times[i] = t_end - t_start;
        search_total += search_times[i];
        if (search_times[i] > search_max) search_max = search_times[i];
        if (search_times[i] < search_min) search_min = search_times[i];
    }
    search_qps = (double)iterations / ((double)search_total / 1000000.0);
    printf("  Search complete: total=%lu us avg=%lu us min=%lu us max=%lu us QPS=%.1f\n",
           (unsigned long)search_total,
           (unsigned long)(search_total / (uint64_t)iterations),
           (unsigned long)search_min,
           (unsigned long)search_max, search_qps);

    {
        qsort(search_times, (size_t)iterations, sizeof(uint64_t), cmp_u64);
        printf("  Search p50=%lu us p95=%lu us p99=%lu us\n",
               (unsigned long)search_times[(int)(iterations * 0.50)],
               (unsigned long)search_times[(int)(iterations * 0.95)],
               (unsigned long)search_times[(int)(iterations * 0.99)]);
    }

    /* CSV output */
    printf("\n--- ANN_CORE_CSV ---\n");
    printf("level,phase,nb,dim,topk,iterations,count,total_us,avg_us,min_us,max_us,p50_us,p95_us,p99_us,qps\n");
    printf("ANN_CORE,add,%d,%d,%d,%d,%d,%lu,%lu,%lu,%lu,0,0,0,%.1f\n",
           nb, dim, topk, iterations, nb,
           (unsigned long)add_total,
           (unsigned long)(add_total / (uint64_t)nb),
           (unsigned long)0, (unsigned long)add_max, add_qps);
    printf("ANN_CORE,search,%d,%d,%d,%d,%d,%lu,%lu,%lu,%lu,%lu,%lu,%lu,%.1f\n",
           nb, dim, topk, iterations, iterations,
           (unsigned long)search_total,
           (unsigned long)(search_total / (uint64_t)iterations),
           (unsigned long)search_min, (unsigned long)search_max,
           (unsigned long)search_times[(int)(iterations * 0.50)],
           (unsigned long)search_times[(int)(iterations * 0.95)],
           (unsigned long)search_times[(int)(iterations * 0.99)],
           search_qps);

    ret = 0;
cleanup:
    free(query);
    free(results);
    free(add_times);
    free(search_times);
    return ret;
}

/* ══════════════════════════════════════════════════════════════════════
 * Level 2 & 3: VEC and TEXT benchmarks
 * ══════════════════════════════════════════════════════════════════════ */

typedef struct {
    const char *name;
    int is_vset;      /* 1=VSET, 0=VSEARCH */
    int is_vec;       /* 1=VEC (raw float), 0=TEXT */
    int nb;
    int dim;
    int topk;
    int iterations;
    int concurrency;
} bench_config_t;

typedef struct {
    uint64_t *latencies_us;
    int count;
    int cap;
} latency_buf_t;

static latency_buf_t *latency_buf_new(int cap) {
    latency_buf_t *b = calloc(1, sizeof(*b));
    if (!b) return NULL;
    b->latencies_us = calloc((size_t)cap, sizeof(uint64_t));
    if (!b->latencies_us) { free(b); return NULL; }
    b->cap = cap;
    b->count = 0;
    return b;
}

static void latency_buf_add(latency_buf_t *b, uint64_t us) {
    if (!b || b->count >= b->cap) return;
    b->latencies_us[b->count++] = us;
}

static void latency_buf_free(latency_buf_t *b) {
    if (!b) return;
    free(b->latencies_us);
    free(b);
}

static int cmp_u64(const void *pa, const void *pb) {
    uint64_t a = *(const uint64_t *)pa;
    uint64_t b = *(const uint64_t *)pb;
    return (a > b) - (a < b);
}

static void latency_buf_stats(latency_buf_t *b, uint64_t *total,
                               uint64_t *avg, uint64_t *min_us,
                               uint64_t *max_us, uint64_t *p50,
                               uint64_t *p95, uint64_t *p99) {
    int n = b ? b->count : 0;
    if (n == 0) return;

    uint64_t sum = 0, mn = UINT64_MAX, mx = 0;
    int i;
    for (i = 0; i < n; i++) {
        sum += b->latencies_us[i];
        if (b->latencies_us[i] < mn) mn = b->latencies_us[i];
        if (b->latencies_us[i] > mx) mx = b->latencies_us[i];
    }

    /* sort for percentiles */
    qsort(b->latencies_us, (size_t)n, sizeof(uint64_t), cmp_u64);

    *total = sum;
    *avg = sum / (uint64_t)n;
    *min_us = mn;
    *max_us = mx;
    *p50 = b->latencies_us[(int)(n * 0.50)];
    *p95 = b->latencies_us[(int)(n * 0.95)];
    *p99 = b->latencies_us[(int)(n * 0.99)];
}

/* ── VSET VEC ──────────────────────────────────────────────────────── */

static int bench_vset_vec(int nb, int dim, int iterations) {
    kvs_vector_set_prepared_t prepared;
    float *embedding;
    char question[256], answer[256];
    uint64_t *lats;
    uint64_t t_start, t_end;
    uint64_t total = 0, mn = UINT64_MAX, mx = 0;
    int i, ret;
    double qps;

    printf("\n=== VSET_VEC: raw float vector, no embedding ===\n");
    printf("  nb=%d dim=%d iterations=%d\n", nb, dim, iterations);

    embedding = calloc((size_t)dim, sizeof(float));
    lats = calloc((size_t)iterations, sizeof(uint64_t));
    if (!embedding || !lats) {
        fprintf(stderr, "OOM\n");
        free(embedding); free(lats);
        return -1;
    }

    for (i = 0; i < iterations; i++) {
        rand_vec(embedding, dim);
        snprintf(question, sizeof(question), "bench_q_%d", i);
        snprintf(answer, sizeof(answer), "bench_a_%d_%s", i,
                 "Lorem ipsum dolor sit amet consectetur adipiscing elit");

        kvs_vector_set_prepared_reset(&prepared);
        ret = kvs_vector_set_prepare_with_embedding(
            question, strlen(question),
            answer, strlen(answer),
            NULL, 0,
            embedding, dim,
            &prepared);
        if (ret != KVS_VECTOR_OK) {
            fprintf(stderr, "  prepare[%d] failed: %d\n", i, ret);
            continue;
        }

        /* Lock + commit */
        kvs_owner_rwlock_wrlock(&g_kv_data_rwlock, "bench_vset_vec");
        t_start = now_us();
        ret = kvs_vector_set_commit_prepared(&prepared);
        t_end = now_us();
        kvs_owner_rwlock_unlock(&g_kv_data_rwlock, "bench_vset_vec");

        kvs_vector_set_prepared_destroy(&prepared);

        if (ret != KVS_VECTOR_OK) {
            fprintf(stderr, "  commit[%d] failed: %d\n", i, ret);
            continue;
        }

        lats[i] = t_end - t_start;
        total += lats[i];
        if (lats[i] < mn) mn = lats[i];
        if (lats[i] > mx) mx = lats[i];
    }

    qps = (double)iterations / ((double)total / 1000000.0);
    printf("  VSET_VEC: total=%lu us avg=%lu us min=%lu us max=%lu us QPS=%.1f\n",
           (unsigned long)total, (unsigned long)(total / (uint64_t)iterations),
           (unsigned long)mn, (unsigned long)mx, qps);

    free(embedding);
    free(lats);
    return 0;
}

/* ── VSET TEXT ──────────────────────────────────────────────────────── */

static int bench_vset_text(int nb, int dim, int iterations) {
    kvs_vector_set_prepared_t prepared;
    char question[256], answer[256];
    uint64_t *lats;
    uint64_t t_start, t_end;
    uint64_t total = 0, mn = UINT64_MAX, mx = 0;
    int i, ret;
    double qps;

    printf("\n=== VSET_TEXT: text input, includes embedding ===\n");
    printf("  nb=%d dim=%d iterations=%d\n", nb, dim, iterations);

    lats = calloc((size_t)iterations, sizeof(uint64_t));
    if (!lats) { fprintf(stderr, "OOM\n"); return -1; }

    for (i = 0; i < iterations; i++) {
        snprintf(question, sizeof(question), "bench_text_q_%d", i);
        snprintf(answer, sizeof(answer), "bench_text_a_%d_Lorem_ipsum_dolor_sit_amet", i);

        kvs_vector_set_prepared_reset(&prepared);

        /* Prepare (includes embedding generation) */
        t_start = now_us();
        ret = kvs_vector_set_prepare(
            question, strlen(question),
            answer, strlen(answer),
            NULL, 0, &prepared);
        t_end = now_us();

        if (ret != KVS_VECTOR_OK) {
            fprintf(stderr, "  prepare[%d] failed: %d\n", i, ret);
            continue;
        }

        uint64_t prep_us = t_end - t_start;

        /* Lock + commit */
        kvs_owner_rwlock_wrlock(&g_kv_data_rwlock, "bench_vset_text");
        t_start = now_us();
        ret = kvs_vector_set_commit_prepared(&prepared);
        t_end = now_us();
        kvs_owner_rwlock_unlock(&g_kv_data_rwlock, "bench_vset_text");

        kvs_vector_set_prepared_destroy(&prepared);

        if (ret != KVS_VECTOR_OK) {
            fprintf(stderr, "  commit[%d] failed: %d\n", i, ret);
            continue;
        }

        lats[i] = prep_us + (t_end - t_start);
        total += lats[i];
        if (lats[i] < mn) mn = lats[i];
        if (lats[i] > mx) mx = lats[i];
    }

    qps = (double)iterations / ((double)total / 1000000.0);
    printf("  VSET_TEXT: total=%lu us avg=%lu us min=%lu us max=%lu us QPS=%.1f\n",
           (unsigned long)total, (unsigned long)(total / (uint64_t)iterations),
           (unsigned long)mn, (unsigned long)mx, qps);

    free(lats);
    return 0;
}

/* ── VSEARCH VEC ────────────────────────────────────────────────────── */

static int bench_vsearch_vec(int nb, int dim, int topk, int iterations) {
    kvs_vector_search_prepared_t prepared;
    float *embedding;
    kvs_vector_search_result_t *results;
    size_t results_count;
    uint64_t *lats;
    uint64_t t_start, t_end;
    uint64_t total = 0, mn = UINT64_MAX, mx = 0;
    int i, ret;
    double qps;

    printf("\n=== VSEARCH_VEC: raw float query, no embedding ===\n");
    printf("  nb=%d dim=%d topk=%d iterations=%d\n", nb, dim, topk, iterations);

    embedding = calloc((size_t)dim, sizeof(float));
    results = calloc((size_t)topk, sizeof(kvs_vector_search_result_t));
    lats = calloc((size_t)iterations, sizeof(uint64_t));
    if (!embedding || !results || !lats) {
        fprintf(stderr, "OOM\n");
        goto cleanup;
    }

    for (i = 0; i < iterations; i++) {
        rand_vec(embedding, dim);

        kvs_vector_search_prepared_reset(&prepared);
        ret = kvs_vector_search_prepare_with_embedding(
            "bench_q", 7, NULL, 0, embedding, dim, &prepared);
        if (ret != KVS_VECTOR_OK) {
            fprintf(stderr, "  prepare[%d] failed: %d\n", i, ret);
            continue;
        }

        kvs_vector_search_results_reset(results, (size_t)topk);

        /* Lock + search */
        kvs_owner_rwlock_rdlock(&g_kv_data_rwlock, "bench_vsearch_vec");
        t_start = now_us();
        ret = kvs_vector_search_with_prepared(
            &prepared, topk, -1.0f,
            results, (size_t)topk, &results_count);
        t_end = now_us();
        kvs_owner_rwlock_unlock(&g_kv_data_rwlock, "bench_vsearch_vec");

        kvs_vector_search_prepared_destroy(&prepared);

        if (ret != KVS_VECTOR_OK) {
            fprintf(stderr, "  search[%d] failed: %d\n", i, ret);
            continue;
        }

        lats[i] = t_end - t_start;
        total += lats[i];
        if (lats[i] < mn) mn = lats[i];
        if (lats[i] > mx) mx = lats[i];
    }

    qps = (double)iterations / ((double)total / 1000000.0);
    printf("  VSEARCH_VEC: total=%lu us avg=%lu us min=%lu us max=%lu us QPS=%.1f\n",
           (unsigned long)total, (unsigned long)(total / (uint64_t)iterations),
           (unsigned long)mn, (unsigned long)mx, qps);

    /* percentiles */
    {
        int n = iterations;
        qsort(lats, (size_t)n, sizeof(uint64_t), cmp_u64);
        printf("  p50=%lu us p95=%lu us p99=%lu us\n",
               (unsigned long)lats[(int)(n * 0.50)],
               (unsigned long)lats[(int)(n * 0.95)],
               (unsigned long)lats[(int)(n * 0.99)]);
    }

    ret = 0;
cleanup:
    free(embedding);
    free(results);
    free(lats);
    return ret;
}

/* ── VSEARCH TEXT ───────────────────────────────────────────────────── */

static int bench_vsearch_text(int nb, int dim, int topk, int iterations) {
    kvs_vector_search_prepared_t prepared;
    kvs_vector_search_result_t *results;
    size_t results_count;
    uint64_t *lats;
    uint64_t t_start, t_end;
    uint64_t total = 0, mn = UINT64_MAX, mx = 0;
    int i, ret;
    double qps;
    char query[256];

    printf("\n=== VSEARCH_TEXT: text query, includes embedding ===\n");
    printf("  nb=%d dim=%d topk=%d iterations=%d\n", nb, dim, topk, iterations);

    results = calloc((size_t)topk, sizeof(kvs_vector_search_result_t));
    lats = calloc((size_t)iterations, sizeof(uint64_t));
    if (!results || !lats) {
        fprintf(stderr, "OOM\n");
        goto cleanup;
    }

    for (i = 0; i < iterations; i++) {
        snprintf(query, sizeof(query), "bench_text_search_query_%d", i);

        kvs_vector_search_prepared_reset(&prepared);

        /* Prepare (includes embedding) */
        t_start = now_us();
        ret = kvs_vector_search_prepare(query, strlen(query), NULL, 0, &prepared);
        t_end = now_us();
        uint64_t prep_us = t_end - t_start;

        if (ret != KVS_VECTOR_OK) {
            fprintf(stderr, "  prepare[%d] failed: %d\n", i, ret);
            continue;
        }

        kvs_vector_search_results_reset(results, (size_t)topk);

        /* Lock + search */
        kvs_owner_rwlock_rdlock(&g_kv_data_rwlock, "bench_vsearch_text");
        t_start = now_us();
        ret = kvs_vector_search_with_prepared(
            &prepared, topk, -1.0f,
            results, (size_t)topk, &results_count);
        t_end = now_us();
        kvs_owner_rwlock_unlock(&g_kv_data_rwlock, "bench_vsearch_text");

        kvs_vector_search_prepared_destroy(&prepared);

        if (ret != KVS_VECTOR_OK) {
            fprintf(stderr, "  search[%d] failed: %d\n", i, ret);
            continue;
        }

        lats[i] = prep_us + (t_end - t_start);
        total += lats[i];
        if (lats[i] < mn) mn = lats[i];
        if (lats[i] > mx) mx = lats[i];
    }

    qps = (double)iterations / ((double)total / 1000000.0);
    printf("  VSEARCH_TEXT: total=%lu us avg=%lu us min=%lu us max=%lu us QPS=%.1f\n",
           (unsigned long)total, (unsigned long)(total / (uint64_t)iterations),
           (unsigned long)mn, (unsigned long)mx, qps);

    {
        int n = iterations;
        qsort(lats, (size_t)n, sizeof(uint64_t), cmp_u64);
        printf("  p50=%lu us p95=%lu us p99=%lu us\n",
               (unsigned long)lats[(int)(n * 0.50)],
               (unsigned long)lats[(int)(n * 0.95)],
               (unsigned long)lats[(int)(n * 0.99)]);
    }

    ret = 0;
cleanup:
    free(results);
    free(lats);
    return ret;
}

/* ══════════════════════════════════════════════════════════════════════
 * Main
 * ══════════════════════════════════════════════════════════════════════ */

static void print_usage(const char *prog) {
    fprintf(stderr,
        "Usage: %s <mode> <nb> <dim> <topk> [iterations]\n"
        "\n"
        "Modes:\n"
        "  ann_core      Level 1: direct index API calls (no RESP/network/embedding)\n"
        "  vset_vec      Level 2: VSET with raw float vectors (no embedding)\n"
        "  vset_text     Level 3: VSET with text (includes embedding)\n"
        "  vsearch_vec   Level 2: VSEARCH with raw float query (no embedding)\n"
        "  vsearch_text  Level 3: VSEARCH with text query (includes embedding)\n"
        "  all           Run all modes sequentially\n"
        "\n"
        "Arguments:\n"
        "  nb           Number of base vectors (for setup)\n"
        "  dim          Vector dimension\n"
        "  topk         Number of results to return\n"
        "  iterations   Number of operations to benchmark (default: 1000)\n"
        "\n"
        "Examples:\n"
        "  %s ann_core 10000 512 10 1000\n"
        "  %s vsearch_vec 10000 512 10 1000\n"
        "  %s vsearch_text 10000 512 10 100\n"
        "  %s all 10000 512 10 1000\n",
        prog, prog, prog, prog, prog);
}

static int setup_vector_state(int nb, int dim) {
    /* Pre-populate with random vectors */
    float *v = calloc((size_t)dim, sizeof(float));
    if (!v) return -1;

    for (int i = 0; i < nb; i++) {
        rand_vec(v, dim);
        char q[128], a[128];
        snprintf(q, sizeof(q), "setup_q_%d", i);
        snprintf(a, sizeof(a), "setup_a_%d", i);

        kvs_vector_set_prepared_t prep;
        kvs_vector_set_prepared_reset(&prep);
        int ret = kvs_vector_set_prepare_with_embedding(
            q, strlen(q), a, strlen(a), NULL, 0, v, dim, &prep);
        if (ret != KVS_VECTOR_OK) { free(v); return -1; }

        kvs_owner_rwlock_wrlock(&g_kv_data_rwlock, "setup");
        ret = kvs_vector_set_commit_prepared(&prep);
        kvs_owner_rwlock_unlock(&g_kv_data_rwlock, "setup");
        kvs_vector_set_prepared_destroy(&prep);
        if (ret != KVS_VECTOR_OK) { free(v); return -1; }
    }
    free(v);
    return 0;
}

int main(int argc, char *argv[]) {
    const char *mode;
    int nb, dim, topk, iterations;
    int ret;

    if (argc < 5) {
        print_usage(argv[0]);
        return 1;
    }

    mode = argv[1];
    nb = atoi(argv[2]);
    dim = atoi(argv[3]);
    topk = atoi(argv[4]);
    iterations = argc >= 6 ? atoi(argv[5]) : 1000;

    if (nb <= 0) nb = DEFAULT_NB;
    if (dim <= 0) dim = DEFAULT_DIM;
    if (topk <= 0) topk = DEFAULT_TOPK;
    if (iterations <= 0) iterations = DEFAULT_ITER;

    printf("KVLite V-Cmd Profile Benchmark\n");
    printf("  mode=%s nb=%d dim=%d topk=%d iterations=%d\n",
           mode, nb, dim, topk, iterations);

    /* Initialize config */
    kvs_config_init_default(&g_cfg);

    /* Override config for benchmark */
    strncpy(g_cfg.vector_index_type, "hnsw", sizeof(g_cfg.vector_index_type) - 1);
    g_cfg.vector_index_type[sizeof(g_cfg.vector_index_type) - 1] = '\0';
    strncpy(g_cfg.vector_metric, "ip", sizeof(g_cfg.vector_metric) - 1);
    g_cfg.vector_metric[sizeof(g_cfg.vector_metric) - 1] = '\0';
    g_cfg.vector_hnsw_m = 16;
    g_cfg.vector_hnsw_ef_construction = 200;
    g_cfg.vector_hnsw_ef_search = 128;

    /* Init memory subsystem (required before kvs_malloc works) */
    mem_init();

    /* Init vector module */
    ret = kvs_vector_init();
    if (ret != KVS_VECTOR_OK) {
        fprintf(stderr, "kvs_vector_init failed: %d\n", ret);
        return 1;
    }
    printf("  Vector module initialized: index=%s dim=%zu\n",
           g_cfg.vector_index_type,
           g_vector_state.flat_index.dimension);

    printf("  Embedding provider: %s\n",
           kvs_vector_embedding_provider_name());

    /* Pre-populate with data */
    if (strcmp(mode, "ann_core") != 0) {
        printf("\n  Populating %d vectors...\n", nb);
        ret = setup_vector_state(nb, dim);
        if (ret != 0) {
            fprintf(stderr, "setup_vector_state failed\n");
            kvs_vector_destroy();
            return 1;
        }
        printf("  Population complete. active=%zu\n",
               g_vector_state.store.active_records);
    }

    /* Run benchmark */
    if (strcmp(mode, "ann_core") == 0) {
        ret = bench_ann_core(nb, dim, topk, iterations);
    } else if (strcmp(mode, "vset_vec") == 0) {
        ret = bench_vset_vec(nb, dim, iterations);
    } else if (strcmp(mode, "vset_text") == 0) {
        ret = bench_vset_text(nb, dim, iterations);
    } else if (strcmp(mode, "vsearch_vec") == 0) {
        ret = bench_vsearch_vec(nb, dim, topk, iterations);
    } else if (strcmp(mode, "vsearch_text") == 0) {
        ret = bench_vsearch_text(nb, dim, topk, iterations);
    } else if (strcmp(mode, "all") == 0) {
        bench_vset_vec(nb, dim, iterations);
        bench_vset_text(nb, dim, iterations);
        bench_vsearch_vec(nb, dim, topk, iterations);
        bench_vsearch_text(nb, dim, topk, iterations);
        bench_ann_core(nb, dim, topk, iterations);
        ret = 0;
    } else {
        fprintf(stderr, "Unknown mode: %s\n", mode);
        print_usage(argv[0]);
        ret = 1;
    }

    /* Dump profile if compiled with KVLITE_VCMD_PROFILE */
#ifdef KVLITE_VCMD_PROFILE
    kvs_vcmd_profile_dump(stdout);
#endif

    kvs_vector_destroy();
    return ret;
}
