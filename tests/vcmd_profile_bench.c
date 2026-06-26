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
#include "kvs_vector_distance.h"
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
#include <errno.h>
#include <sys/stat.h>
#include <sys/types.h>

/* ── constants ──────────────────────────────────────────────────────── */

#define MAX_QUERY_LEN  256
#define MAX_ANSWER_LEN 512
#define DEFAULT_DIM    512
#define DEFAULT_NB     10000
#define DEFAULT_TOPK   10
#define DEFAULT_ITER   1000

typedef struct {
    float score;
    int id;
} scored_id_t;

typedef struct {
    uint64_t add_total_us;
    uint64_t add_avg_us;
    uint64_t add_max_us;
    uint64_t search_total_us;
    uint64_t search_avg_us;
    uint64_t search_min_us;
    uint64_t search_max_us;
    uint64_t search_p50_us;
    uint64_t search_p95_us;
    uint64_t search_p99_us;
    uint64_t rebuild_wall_us;
    double add_qps;
    double search_qps;
    double recall_at_k;
    kvs_vector_info_t info;
} ann_core_summary_t;

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
static int cmp_scored_id_desc(const void *pa, const void *pb);
static int exact_topk_ip(
    const float *base,
    int nb,
    int dim,
    const float *query,
    int topk,
    scored_id_t *scores,
    int *truth
);
static double recall_for_results(
    const kvs_vector_search_result_t *results,
    size_t results_count,
    const int *truth,
    int topk
);
static int write_ann_core_stage7d_files(
    int nb,
    int dim,
    int topk,
    int iterations,
    const ann_core_summary_t *summary
);
static int use_synthetic_direct_vector_dimension(int dim);

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
    float *base_vectors = NULL;
    kvs_vector_search_result_t *results;
    scored_id_t *exact_scores = NULL;
    int *truth = NULL;
    size_t results_count;
    uint64_t *add_times, *search_times;
    uint64_t t_start, t_end;
    uint64_t rebuild_start, rebuild_end;
    int i, ret;
    double add_qps, search_qps;
    uint64_t add_total = 0, search_total = 0;
    uint64_t add_max = 0, search_max = 0, search_min = UINT64_MAX;
    double recall_total = 0.0;
    ann_core_summary_t summary;

    printf("\n=== Level 1: ANN-core Benchmark ===\n");
    printf("  nb=%d dim=%d topk=%d iterations=%d\n", nb, dim, topk, iterations);
    printf("  index_type=%s metric=%s\n",
           g_cfg.vector_index_type ? g_cfg.vector_index_type : "flat",
           kvs_vector_metric_name(g_vector_state.metric_type));

    memset(&summary, 0, sizeof(summary));

    query = calloc((size_t)dim, sizeof(float));
    base_vectors = calloc((size_t)nb * (size_t)dim, sizeof(float));
    results = calloc((size_t)topk, sizeof(kvs_vector_search_result_t));
    exact_scores = calloc((size_t)nb, sizeof(scored_id_t));
    truth = calloc((size_t)topk, sizeof(int));
    add_times = calloc((size_t)nb, sizeof(uint64_t));
    search_times = calloc((size_t)iterations, sizeof(uint64_t));

    if (!query || !base_vectors || !results ||
        !exact_scores || !truth || !add_times || !search_times) {
        fprintf(stderr, "OOM\n");
        ret = -1;
        goto cleanup;
    }

    /* Reset state */
    kvs_vector_reset_for_load();
    index = &g_vector_state.current_index;

    /* Phase 1: Add direct vectors into manager record store. */
    printf("\n  Phase 1: Loading %d direct vectors into record store...\n", nb);
    for (i = 0; i < nb; i++) {
        char q[128], a[128];
        rand_vec(query, dim);
        memcpy(base_vectors + ((size_t)i * (size_t)dim),
               query, (size_t)dim * sizeof(float));
        snprintf(q, sizeof(q), "ann_core_q_%d", i);
        snprintf(a, sizeof(a), "ann_core_a_%d", i);

        t_start = now_us();
        ret = kvs_vector_restore_record_with_embedding(
            (size_t)i,
            (uint64_t)i + 1,
            KVS_VECTOR_RECORD_ACTIVE,
            0,
            q,
            strlen(q),
            a,
            strlen(a),
            NULL,
            0,
            q,
            strlen(q),
            query,
            dim,
            0,
            0,
            -1,
            0,
            kvs_vector_embedding_current_model_version());
        t_end = now_us();

        if (ret != KVS_VECTOR_OK) { fprintf(stderr, "  restore[%d] failed: %d\n", i, ret); goto cleanup; }

        add_times[i] = t_end - t_start;
        add_total += add_times[i];
        if (add_times[i] > add_max) add_max = add_times[i];
    }
    add_qps = (double)nb / ((double)add_total / 1000000.0);
    printf("  Add complete: total=%lu us avg=%lu us max=%lu us QPS=%.1f\n",
           (unsigned long)add_total, (unsigned long)(add_total / (uint64_t)nb),
           (unsigned long)add_max, add_qps);

    printf("\n  Phase 2: Rebuilding core_hnsw from direct-vector records...\n");
    ret = use_synthetic_direct_vector_dimension(dim);
    if (ret != KVS_VECTOR_OK) {
        fprintf(stderr, "  direct-vector index reinit before rebuild failed: %d\n", ret);
        goto cleanup;
    }
    rebuild_start = now_us();
    ret = kvs_vector_index_rebuild_current();
    rebuild_end = now_us();
    if (ret != KVS_VECTOR_OK) {
        fprintf(stderr, "  rebuild failed: %d\n", ret);
        goto cleanup;
    }
    printf("  Rebuild complete: wall=%lu us\n",
           (unsigned long)(rebuild_end - rebuild_start));

    /* Phase 3: Search vectors — pure index search, no embedding */
    printf("\n  Phase 3: Searching %d queries (pure index API, no embedding)...\n", iterations);
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
        if (exact_topk_ip(base_vectors, nb, dim, query, topk, exact_scores, truth) != 0) {
            fprintf(stderr, "  exact_topk[%d] failed\n", i);
            ret = -1;
            goto cleanup;
        }
        recall_total += recall_for_results(results, results_count, truth, topk);
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
    printf("  Recall@%d=%.4f\n", topk, recall_total / (double)iterations);

    {
        qsort(search_times, (size_t)iterations, sizeof(uint64_t), cmp_u64);
        printf("  Search p50=%lu us p95=%lu us p99=%lu us\n",
               (unsigned long)search_times[(int)(iterations * 0.50)],
               (unsigned long)search_times[(int)(iterations * 0.95)],
               (unsigned long)search_times[(int)(iterations * 0.99)]);
    }

    if (kvs_vector_info(&summary.info) != KVS_VECTOR_OK) {
        fprintf(stderr, "  kvs_vector_info failed\n");
        ret = -1;
        goto cleanup;
    }
    if ((int)summary.info.dimension != dim || (int)summary.info.backend_dim != dim) {
        fprintf(stderr,
                "  dimension mismatch: requested=%d dimension=%zu backend_dim=%zu\n",
                dim, summary.info.dimension, summary.info.backend_dim);
        ret = -1;
        goto cleanup;
    }

    summary.add_total_us = add_total;
    summary.add_avg_us = add_total / (uint64_t)nb;
    summary.add_max_us = add_max;
    summary.search_total_us = search_total;
    summary.search_avg_us = search_total / (uint64_t)iterations;
    summary.search_min_us = search_min;
    summary.search_max_us = search_max;
    summary.search_p50_us = search_times[(int)(iterations * 0.50)];
    summary.search_p95_us = search_times[(int)(iterations * 0.95)];
    summary.search_p99_us = search_times[(int)(iterations * 0.99)];
    summary.rebuild_wall_us = rebuild_end - rebuild_start;
    summary.add_qps = add_qps;
    summary.search_qps = search_qps;
    summary.recall_at_k = recall_total / (double)iterations;

    printf("\n--- STAGE7D_NEIGHBOR_SELECT_SUMMARY ---\n");
    printf("backend_dim=%zu\n", summary.info.backend_dim);
    printf("distance_backend=%s\n", kvs_vector_distance_backend_name());
    printf("rebuild_core_add_total_us=%lu\n",
           (unsigned long)summary.info.rebuild_core_add_total_us);
    printf("hnsw_add_layer_search_us=%lu\n",
           (unsigned long)summary.info.hnsw_add_layer_search_us);
    printf("hnsw_add_neighbor_select_us=%lu\n",
           (unsigned long)summary.info.hnsw_add_neighbor_select_us);
    printf("hnsw_add_link_us=%lu\n",
           (unsigned long)summary.info.hnsw_add_link_us);
    printf("neighbor_select_call_count=%lu\n",
           (unsigned long)summary.info.neighbor_select_call_count);
    printf("neighbor_select_candidate_avg=%.2f\n",
           summary.info.neighbor_select_call_count ?
           (double)summary.info.neighbor_select_candidate_total /
           (double)summary.info.neighbor_select_call_count : 0.0);
    printf("neighbor_select_selected_avg=%.2f\n",
           summary.info.neighbor_select_call_count ?
           (double)summary.info.neighbor_select_selected_total /
           (double)summary.info.neighbor_select_call_count : 0.0);
    printf("neighbor_select_distance_recompute_count=%lu\n",
           (unsigned long)summary.info.neighbor_select_distance_recompute_count);
    printf("neighbor_select_reused_candidate_score_count=%lu\n",
           (unsigned long)summary.info.neighbor_select_reused_candidate_score_count);
    printf("neighbor_select_level0_distance_recompute_count=%lu\n",
           (unsigned long)summary.info.neighbor_select_level0_distance_recompute_count);
    printf("neighbor_select_upper_distance_recompute_count=%lu\n",
           (unsigned long)summary.info.neighbor_select_upper_distance_recompute_count);
    printf("vsearch_backend_search_avg_us=%lu\n",
           (unsigned long)(summary.info.vsearch_backend_search_count ?
           summary.info.vsearch_backend_search_total_us /
           summary.info.vsearch_backend_search_count : 0));
    printf("backend_memory_bytes=%zu\n", summary.info.backend_memory_bytes);
    printf("recall@%d=%.4f\n", topk, summary.recall_at_k);

    if (write_ann_core_stage7d_files(nb, dim, topk, iterations, &summary) != 0) {
        fprintf(stderr, "  warning: failed to write Stage 7D result files\n");
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
    free(base_vectors);
    free(results);
    free(exact_scores);
    free(truth);
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

static int cmp_scored_id_desc(const void *pa, const void *pb) {
    const scored_id_t *a = (const scored_id_t *)pa;
    const scored_id_t *b = (const scored_id_t *)pb;

    if (a->score < b->score) return 1;
    if (a->score > b->score) return -1;
    return (a->id > b->id) - (a->id < b->id);
}

static int ensure_dir(const char *path) {
    if (mkdir(path, 0777) == 0 || errno == EEXIST) {
        return 0;
    }
    return -1;
}

static int ensure_results_dir(void) {
    if (ensure_dir("results") != 0) return -1;
    if (ensure_dir("results/vector_core_benchmark") != 0) return -1;
    return 0;
}

static float ip_score(const float *lhs, const float *rhs, int dim) {
    float score = 0.0f;

    for (int i = 0; i < dim; i++) {
        score += lhs[i] * rhs[i];
    }
    return score;
}

static int exact_topk_ip(
    const float *base,
    int nb,
    int dim,
    const float *query,
    int topk,
    scored_id_t *scores,
    int *truth
) {
    if (!base || nb <= 0 || dim <= 0 || !query || topk <= 0 || !scores || !truth) {
        return -1;
    }

    for (int i = 0; i < nb; i++) {
        scores[i].score = ip_score(base + ((size_t)i * (size_t)dim), query, dim);
        scores[i].id = i;
    }
    qsort(scores, (size_t)nb, sizeof(scored_id_t), cmp_scored_id_desc);
    for (int i = 0; i < topk; i++) {
        truth[i] = i < nb ? scores[i].id : -1;
    }
    return 0;
}

static double recall_for_results(
    const kvs_vector_search_result_t *results,
    size_t results_count,
    const int *truth,
    int topk
) {
    int hits = 0;

    if (!results || !truth || topk <= 0) {
        return 0.0;
    }

    for (size_t i = 0; i < results_count; i++) {
        int got = -1;

        if (!results[i].question ||
            sscanf(results[i].question, "ann_core_q_%d", &got) != 1) {
            continue;
        }
        for (int j = 0; j < topk; j++) {
            if (truth[j] == got) {
                hits++;
                break;
            }
        }
    }
    return (double)hits / (double)topk;
}

static int write_ann_core_stage7d_files(
    int nb,
    int dim,
    int topk,
    int iterations,
    const ann_core_summary_t *summary
) {
    char json_path[256];
    char md_path[256];
    FILE *fp;
    const uint64_t backend_avg =
        summary->info.vsearch_backend_search_count ?
        summary->info.vsearch_backend_search_total_us /
            summary->info.vsearch_backend_search_count : 0;

    if (!summary || ensure_results_dir() != 0) {
        return -1;
    }

    snprintf(json_path, sizeof(json_path),
             "results/vector_core_benchmark/stage7d_highdim_neighbor_select_%dk_%dd.json",
             nb / 1000, dim);
    snprintf(md_path, sizeof(md_path),
             "results/vector_core_benchmark/stage7d_highdim_neighbor_select_%dk_%dd.md",
             nb / 1000, dim);

    fp = fopen(json_path, "w");
    if (!fp) {
        return -1;
    }
    fprintf(fp, "{\n");
    fprintf(fp, "  \"stage\": \"7D\",\n");
    fprintf(fp, "  \"mode\": \"ann_core_direct_vector\",\n");
    fprintf(fp, "  \"index_type\": \"core_hnsw\",\n");
    fprintf(fp, "  \"nb\": %d,\n", nb);
    fprintf(fp, "  \"dim\": %d,\n", dim);
    fprintf(fp, "  \"topk\": %d,\n", topk);
    fprintf(fp, "  \"queries\": %d,\n", iterations);
    fprintf(fp, "  \"dimension\": %zu,\n", summary->info.dimension);
    fprintf(fp, "  \"backend_dim\": %zu,\n", summary->info.backend_dim);
    fprintf(fp, "  \"distance_backend\": \"%s\",\n", kvs_vector_distance_backend_name());
    fprintf(fp, "  \"backend_memory_bytes\": %zu,\n", summary->info.backend_memory_bytes);
    fprintf(fp, "  \"backend_memory_bytes_source\": \"%s\",\n",
            summary->info.backend_memory_bytes_source ?
            summary->info.backend_memory_bytes_source : "");
    fprintf(fp, "  \"rebuild_wall_us\": %lu,\n", (unsigned long)summary->rebuild_wall_us);
    fprintf(fp, "  \"rebuild_core_add_total_us\": %lu,\n",
            (unsigned long)summary->info.rebuild_core_add_total_us);
    fprintf(fp, "  \"hnsw_add_layer_search_us\": %lu,\n",
            (unsigned long)summary->info.hnsw_add_layer_search_us);
    fprintf(fp, "  \"hnsw_add_neighbor_select_us\": %lu,\n",
            (unsigned long)summary->info.hnsw_add_neighbor_select_us);
    fprintf(fp, "  \"hnsw_add_link_us\": %lu,\n",
            (unsigned long)summary->info.hnsw_add_link_us);
    fprintf(fp, "  \"neighbor_select_call_count\": %lu,\n",
            (unsigned long)summary->info.neighbor_select_call_count);
    fprintf(fp, "  \"neighbor_select_level0_call_count\": %lu,\n",
            (unsigned long)summary->info.neighbor_select_level0_call_count);
    fprintf(fp, "  \"neighbor_select_upper_call_count\": %lu,\n",
            (unsigned long)summary->info.neighbor_select_upper_call_count);
    fprintf(fp, "  \"neighbor_select_candidate_total\": %lu,\n",
            (unsigned long)summary->info.neighbor_select_candidate_total);
    fprintf(fp, "  \"neighbor_select_candidate_avg\": %.2f,\n",
            summary->info.neighbor_select_call_count ?
            (double)summary->info.neighbor_select_candidate_total /
                (double)summary->info.neighbor_select_call_count : 0.0);
    fprintf(fp, "  \"neighbor_select_candidate_max\": %lu,\n",
            (unsigned long)summary->info.neighbor_select_candidate_max);
    fprintf(fp, "  \"neighbor_select_selected_total\": %lu,\n",
            (unsigned long)summary->info.neighbor_select_selected_total);
    fprintf(fp, "  \"neighbor_select_selected_avg\": %.2f,\n",
            summary->info.neighbor_select_call_count ?
            (double)summary->info.neighbor_select_selected_total /
                (double)summary->info.neighbor_select_call_count : 0.0);
    fprintf(fp, "  \"neighbor_select_selected_max\": %lu,\n",
            (unsigned long)summary->info.neighbor_select_selected_max);
    fprintf(fp, "  \"neighbor_select_reject_count\": %lu,\n",
            (unsigned long)summary->info.neighbor_select_reject_count);
    fprintf(fp, "  \"neighbor_select_heuristic_prune_count\": %lu,\n",
            (unsigned long)summary->info.neighbor_select_heuristic_prune_count);
    fprintf(fp, "  \"neighbor_select_distance_recompute_count\": %lu,\n",
            (unsigned long)summary->info.neighbor_select_distance_recompute_count);
    fprintf(fp, "  \"neighbor_select_reused_candidate_score_count\": %lu,\n",
            (unsigned long)summary->info.neighbor_select_reused_candidate_score_count);
    fprintf(fp, "  \"neighbor_select_duplicate_scan_count\": %lu,\n",
            (unsigned long)summary->info.neighbor_select_duplicate_scan_count);
    fprintf(fp, "  \"neighbor_select_worst_scan_count\": %lu,\n",
            (unsigned long)summary->info.neighbor_select_worst_scan_count);
    fprintf(fp, "  \"neighbor_select_worst_cache_hit_count\": %lu,\n",
            (unsigned long)summary->info.neighbor_select_worst_cache_hit_count);
    fprintf(fp, "  \"neighbor_select_level0_candidate_total\": %lu,\n",
            (unsigned long)summary->info.neighbor_select_level0_candidate_total);
    fprintf(fp, "  \"neighbor_select_upper_candidate_total\": %lu,\n",
            (unsigned long)summary->info.neighbor_select_upper_candidate_total);
    fprintf(fp, "  \"neighbor_select_level0_selected_total\": %lu,\n",
            (unsigned long)summary->info.neighbor_select_level0_selected_total);
    fprintf(fp, "  \"neighbor_select_upper_selected_total\": %lu,\n",
            (unsigned long)summary->info.neighbor_select_upper_selected_total);
    fprintf(fp, "  \"neighbor_select_level0_reject_count\": %lu,\n",
            (unsigned long)summary->info.neighbor_select_level0_reject_count);
    fprintf(fp, "  \"neighbor_select_upper_reject_count\": %lu,\n",
            (unsigned long)summary->info.neighbor_select_upper_reject_count);
    fprintf(fp, "  \"neighbor_select_level0_distance_recompute_count\": %lu,\n",
            (unsigned long)summary->info.neighbor_select_level0_distance_recompute_count);
    fprintf(fp, "  \"neighbor_select_upper_distance_recompute_count\": %lu,\n",
            (unsigned long)summary->info.neighbor_select_upper_distance_recompute_count);
    fprintf(fp, "  \"neighbor_select_level0_reused_candidate_score_count\": %lu,\n",
            (unsigned long)summary->info.neighbor_select_level0_reused_candidate_score_count);
    fprintf(fp, "  \"neighbor_select_upper_reused_candidate_score_count\": %lu,\n",
            (unsigned long)summary->info.neighbor_select_upper_reused_candidate_score_count);
    fprintf(fp, "  \"neighbor_select_level0_duplicate_scan_count\": %lu,\n",
            (unsigned long)summary->info.neighbor_select_level0_duplicate_scan_count);
    fprintf(fp, "  \"neighbor_select_upper_duplicate_scan_count\": %lu,\n",
            (unsigned long)summary->info.neighbor_select_upper_duplicate_scan_count);
    fprintf(fp, "  \"neighbor_select_level0_worst_scan_count\": %lu,\n",
            (unsigned long)summary->info.neighbor_select_level0_worst_scan_count);
    fprintf(fp, "  \"neighbor_select_upper_worst_scan_count\": %lu,\n",
            (unsigned long)summary->info.neighbor_select_upper_worst_scan_count);
    fprintf(fp, "  \"neighbor_select_level0_worst_cache_hit_count\": %lu,\n",
            (unsigned long)summary->info.neighbor_select_level0_worst_cache_hit_count);
    fprintf(fp, "  \"neighbor_select_upper_worst_cache_hit_count\": %lu,\n",
            (unsigned long)summary->info.neighbor_select_upper_worst_cache_hit_count);
    fprintf(fp, "  \"vsearch_backend_search_avg_us\": %lu,\n",
            (unsigned long)backend_avg);
    fprintf(fp, "  \"recall_at_%d\": %.4f,\n", topk, summary->recall_at_k);
    fprintf(fp, "  \"ann_add_total_us\": %lu,\n", (unsigned long)summary->add_total_us);
    fprintf(fp, "  \"ann_search_avg_us\": %lu\n", (unsigned long)summary->search_avg_us);
    fprintf(fp, "}\n");
    fclose(fp);

    fp = fopen(md_path, "w");
    if (!fp) {
        return -1;
    }
    fprintf(fp, "# Stage 7D high-dim neighbor_select benchmark %dk/%dd\n\n", nb / 1000, dim);
    fprintf(fp, "| field | value |\n");
    fprintf(fp, "| --- | --- |\n");
    fprintf(fp, "| nb | %d |\n", nb);
    fprintf(fp, "| dim | %d |\n", dim);
    fprintf(fp, "| topk | %d |\n", topk);
    fprintf(fp, "| queries | %d |\n", iterations);
    fprintf(fp, "| backend_dim | %zu |\n", summary->info.backend_dim);
    fprintf(fp, "| distance_backend | %s |\n", kvs_vector_distance_backend_name());
    fprintf(fp, "| rebuild_core_add_total_us | %lu |\n",
            (unsigned long)summary->info.rebuild_core_add_total_us);
    fprintf(fp, "| hnsw_add_layer_search_us | %lu |\n",
            (unsigned long)summary->info.hnsw_add_layer_search_us);
    fprintf(fp, "| hnsw_add_neighbor_select_us | %lu |\n",
            (unsigned long)summary->info.hnsw_add_neighbor_select_us);
    fprintf(fp, "| hnsw_add_link_us | %lu |\n",
            (unsigned long)summary->info.hnsw_add_link_us);
    fprintf(fp, "| neighbor_select_call_count | %lu |\n",
            (unsigned long)summary->info.neighbor_select_call_count);
    fprintf(fp, "| neighbor_select_level0_call_count | %lu |\n",
            (unsigned long)summary->info.neighbor_select_level0_call_count);
    fprintf(fp, "| neighbor_select_upper_call_count | %lu |\n",
            (unsigned long)summary->info.neighbor_select_upper_call_count);
    fprintf(fp, "| neighbor_select_candidate_total | %lu |\n",
            (unsigned long)summary->info.neighbor_select_candidate_total);
    fprintf(fp, "| neighbor_select_candidate_avg | %.2f |\n",
            summary->info.neighbor_select_call_count ?
            (double)summary->info.neighbor_select_candidate_total /
                (double)summary->info.neighbor_select_call_count : 0.0);
    fprintf(fp, "| neighbor_select_candidate_max | %lu |\n",
            (unsigned long)summary->info.neighbor_select_candidate_max);
    fprintf(fp, "| neighbor_select_selected_total | %lu |\n",
            (unsigned long)summary->info.neighbor_select_selected_total);
    fprintf(fp, "| neighbor_select_selected_avg | %.2f |\n",
            summary->info.neighbor_select_call_count ?
            (double)summary->info.neighbor_select_selected_total /
                (double)summary->info.neighbor_select_call_count : 0.0);
    fprintf(fp, "| neighbor_select_selected_max | %lu |\n",
            (unsigned long)summary->info.neighbor_select_selected_max);
    fprintf(fp, "| neighbor_select_reject_count | %lu |\n",
            (unsigned long)summary->info.neighbor_select_reject_count);
    fprintf(fp, "| neighbor_select_heuristic_prune_count | %lu |\n",
            (unsigned long)summary->info.neighbor_select_heuristic_prune_count);
    fprintf(fp, "| neighbor_select_distance_recompute_count | %lu |\n",
            (unsigned long)summary->info.neighbor_select_distance_recompute_count);
    fprintf(fp, "| neighbor_select_reused_candidate_score_count | %lu |\n",
            (unsigned long)summary->info.neighbor_select_reused_candidate_score_count);
    fprintf(fp, "| neighbor_select_duplicate_scan_count | %lu |\n",
            (unsigned long)summary->info.neighbor_select_duplicate_scan_count);
    fprintf(fp, "| neighbor_select_worst_scan_count | %lu |\n",
            (unsigned long)summary->info.neighbor_select_worst_scan_count);
    fprintf(fp, "| neighbor_select_worst_cache_hit_count | %lu |\n",
            (unsigned long)summary->info.neighbor_select_worst_cache_hit_count);
    fprintf(fp, "| neighbor_select_level0_candidate_total | %lu |\n",
            (unsigned long)summary->info.neighbor_select_level0_candidate_total);
    fprintf(fp, "| neighbor_select_upper_candidate_total | %lu |\n",
            (unsigned long)summary->info.neighbor_select_upper_candidate_total);
    fprintf(fp, "| neighbor_select_level0_selected_total | %lu |\n",
            (unsigned long)summary->info.neighbor_select_level0_selected_total);
    fprintf(fp, "| neighbor_select_upper_selected_total | %lu |\n",
            (unsigned long)summary->info.neighbor_select_upper_selected_total);
    fprintf(fp, "| neighbor_select_level0_reject_count | %lu |\n",
            (unsigned long)summary->info.neighbor_select_level0_reject_count);
    fprintf(fp, "| neighbor_select_upper_reject_count | %lu |\n",
            (unsigned long)summary->info.neighbor_select_upper_reject_count);
    fprintf(fp, "| neighbor_select_level0_distance_recompute_count | %lu |\n",
            (unsigned long)summary->info.neighbor_select_level0_distance_recompute_count);
    fprintf(fp, "| neighbor_select_upper_distance_recompute_count | %lu |\n",
            (unsigned long)summary->info.neighbor_select_upper_distance_recompute_count);
    fprintf(fp, "| neighbor_select_level0_reused_candidate_score_count | %lu |\n",
            (unsigned long)summary->info.neighbor_select_level0_reused_candidate_score_count);
    fprintf(fp, "| neighbor_select_upper_reused_candidate_score_count | %lu |\n",
            (unsigned long)summary->info.neighbor_select_upper_reused_candidate_score_count);
    fprintf(fp, "| neighbor_select_level0_duplicate_scan_count | %lu |\n",
            (unsigned long)summary->info.neighbor_select_level0_duplicate_scan_count);
    fprintf(fp, "| neighbor_select_upper_duplicate_scan_count | %lu |\n",
            (unsigned long)summary->info.neighbor_select_upper_duplicate_scan_count);
    fprintf(fp, "| neighbor_select_level0_worst_scan_count | %lu |\n",
            (unsigned long)summary->info.neighbor_select_level0_worst_scan_count);
    fprintf(fp, "| neighbor_select_upper_worst_scan_count | %lu |\n",
            (unsigned long)summary->info.neighbor_select_upper_worst_scan_count);
    fprintf(fp, "| neighbor_select_level0_worst_cache_hit_count | %lu |\n",
            (unsigned long)summary->info.neighbor_select_level0_worst_cache_hit_count);
    fprintf(fp, "| neighbor_select_upper_worst_cache_hit_count | %lu |\n",
            (unsigned long)summary->info.neighbor_select_upper_worst_cache_hit_count);
    fprintf(fp, "| vsearch_backend_search_avg_us | %lu |\n",
            (unsigned long)backend_avg);
    fprintf(fp, "| backend_memory_bytes | %zu |\n", summary->info.backend_memory_bytes);
    fprintf(fp, "| backend_memory_bytes_source | %s |\n",
            summary->info.backend_memory_bytes_source ?
            summary->info.backend_memory_bytes_source : "");
    fprintf(fp, "| recall@%d | %.4f |\n", topk, summary->recall_at_k);
    fclose(fp);

    printf("  Stage 7D result files:\n");
    printf("    %s\n", json_path);
    printf("    %s\n", md_path);
    return 0;
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

static int use_synthetic_direct_vector_dimension(int dim) {
    int ret;

    if (dim <= 0 || !g_vector_state.initialized) {
        return KVS_VECTOR_ERR_INVALID_ARG;
    }

    /*
     * This benchmark bypasses text embedding and feeds direct vectors through
     * *_with_embedding APIs. The production hash provider remains 64d, so the
     * harness explicitly reinitializes derived indexes at the caller-requested
     * dimension instead of pretending the text hash path is high-dimensional.
     */
    kvs_vector_sq8_flat_index_destroy(&g_vector_state.sq8_flat_index);
    kvs_hnsw_index_destroy(&g_vector_state.core_hnsw_index);
    kvs_flat_index_destroy(&g_vector_state.core_flat_index);
    kvs_vector_ann_index_destroy(&g_vector_state.ann_index);
    kvs_vector_flat_index_destroy(&g_vector_state.flat_index);

    ret = kvs_vector_flat_index_init(&g_vector_state.flat_index, (size_t)dim);
    if (ret != KVS_VECTOR_OK) return ret;

    ret = kvs_flat_index_init(
        &g_vector_state.core_flat_index,
        (size_t)dim,
        g_vector_state.metric_type);
    if (ret != KVS_VECTOR_OK) return ret;

    ret = kvs_hnsw_index_init(
        &g_vector_state.core_hnsw_index,
        (size_t)dim,
        g_vector_state.metric_type);
    if (ret != KVS_VECTOR_OK) return ret;
    g_vector_state.core_hnsw_index.M = (size_t)g_cfg.vector_hnsw_m;
    g_vector_state.core_hnsw_index.M0 = g_vector_state.core_hnsw_index.M * 2;
    g_vector_state.core_hnsw_index.ef_construction =
        (size_t)g_cfg.vector_hnsw_ef_construction;
    g_vector_state.core_hnsw_index.ef_search =
        (size_t)g_cfg.vector_hnsw_ef_search;
    ret = kvs_hnsw_index_reserve(
        &g_vector_state.core_hnsw_index,
        g_vector_state.store.capacity);
    if (ret != KVS_VECTOR_OK) return ret;

    ret = kvs_vector_ann_index_init(
        &g_vector_state.ann_index,
        (size_t)dim,
        (size_t)g_cfg.vector_hnsw_m,
        (size_t)g_cfg.vector_hnsw_ef_construction,
        (size_t)g_cfg.vector_hnsw_ef_search);
    if (ret != KVS_VECTOR_OK) return ret;

    ret = kvs_vector_sq8_flat_index_init(
        &g_vector_state.sq8_flat_index,
        (size_t)dim);
    if (ret != KVS_VECTOR_OK &&
        g_vector_state.index_type == KVS_VECTOR_INDEX_TYPE_SQ8_FLAT) {
        return ret;
    } else if (ret != KVS_VECTOR_OK) {
        memset(&g_vector_state.sq8_flat_index, 0, sizeof(g_vector_state.sq8_flat_index));
    }

    return kvs_vector_index_bind_current();
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
    strncpy(g_cfg.vector_index_type, "core_hnsw", sizeof(g_cfg.vector_index_type) - 1);
    g_cfg.vector_index_type[sizeof(g_cfg.vector_index_type) - 1] = '\0';
    strncpy(g_cfg.embedding_provider, "hash", sizeof(g_cfg.embedding_provider) - 1);
    g_cfg.embedding_provider[sizeof(g_cfg.embedding_provider) - 1] = '\0';
    g_cfg.embedding_dim = dim;
    strncpy(g_cfg.vector_metric, "ip", sizeof(g_cfg.vector_metric) - 1);
    g_cfg.vector_metric[sizeof(g_cfg.vector_metric) - 1] = '\0';
    strncpy(g_cfg.vector_distance_backend, "auto", sizeof(g_cfg.vector_distance_backend) - 1);
    g_cfg.vector_distance_backend[sizeof(g_cfg.vector_distance_backend) - 1] = '\0';
    g_cfg.vector_hnsw_m = 32;
    g_cfg.vector_hnsw_ef_construction = 128;
    g_cfg.vector_hnsw_ef_search = 256;

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

    if (strcmp(mode, "ann_core") == 0 ||
        strcmp(mode, "vset_vec") == 0 ||
        strcmp(mode, "vsearch_vec") == 0 ||
        strcmp(mode, "all") == 0) {
        ret = use_synthetic_direct_vector_dimension(dim);
        if (ret != KVS_VECTOR_OK) {
            fprintf(stderr, "synthetic direct-vector dimension init failed: %d\n", ret);
            kvs_vector_destroy();
            return 1;
        }
        printf("  Synthetic direct-vector dimension: %zu\n",
               g_vector_state.flat_index.dimension);
    }

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
