/*
 * kvs_vcmd_profile.h — V-family command profiling infrastructure.
 *
 * Controlled by compile-time flag KVLITE_VCMD_PROFILE.
 * When disabled (default), all instrumentation compiles to zero-cost no-ops.
 *
 * Profiling stages for VSET:
 *   total → resp_parse → task_enqueue → embedding_queue_wait → embedding_encode
 *   → normalize → executor_queue_wait → store_set → vector_index_add
 *   → aof_append → replication → response_build → network_send
 *
 * Profiling stages for VSEARCH:
 *   total → resp_parse → task_enqueue → embedding_queue_wait → embedding_encode
 *   → normalize → executor_queue_wait → ann_search → candidate_lookup
 *   → ttl_filter → response_build → network_send
 *
 * All timestamps use CLOCK_MONOTONIC (microseconds).
 * Stats are aggregated with relaxed atomics — no mutex contention.
 * Percentile estimation uses log2 histogram buckets.
 */

#ifndef KVS_VCMD_PROFILE_H
#define KVS_VCMD_PROFILE_H

#include <stdint.h>
#include <stdio.h>

#ifdef KVLITE_VCMD_PROFILE

/* ── histogram for percentile estimation ──────────────────────────────── */
#define KVS_VCMD_PROFILE_HISTO_BUCKETS 32

typedef struct {
    uint64_t count;          /* number of samples */
    uint64_t total_us;       /* sum of all samples (microseconds) */
    uint64_t max_us;         /* maximum observed */
    uint64_t histogram[KVS_VCMD_PROFILE_HISTO_BUCKETS]; /* log2 buckets */
} kvs_vcmd_stage_t;

/* ── per-request timing context (stack-allocated, thread-local) ───────── */
typedef struct {
    uint64_t total_start_us;
    uint64_t stage_start_us;
    uint64_t parse_end_us;
    uint64_t task_enqueue_end_us;
    uint64_t embedding_queue_end_us;
    uint64_t embedding_encode_end_us;
    uint64_t normalize_end_us;
    uint64_t executor_queue_end_us;
    /* VSET-specific */
    uint64_t store_set_end_us;
    uint64_t index_add_end_us;
    uint64_t aof_append_end_us;
    uint64_t replication_end_us;
    /* VSEARCH-specific */
    uint64_t ann_search_end_us;
    uint64_t candidate_lookup_end_us;
    uint64_t ttl_filter_end_us;
    /* common tail */
    uint64_t response_build_end_us;
    uint64_t network_send_end_us;
    uint8_t  is_vset;        /* 1 = VSET, 0 = VSEARCH */
    uint8_t  is_vec;         /* 1 = VEC (raw float), 0 = TEXT (embedding needed) */
    uint8_t  active;         /* profiling active for this request */
} kvs_vcmd_timing_t;

/* ── global profile state ─────────────────────────────────────────────── */
typedef struct {
    /* VSET stages */
    kvs_vcmd_stage_t vset_total;
    kvs_vcmd_stage_t vset_resp_parse;
    kvs_vcmd_stage_t vset_task_enqueue;
    kvs_vcmd_stage_t vset_embedding_queue_wait;
    kvs_vcmd_stage_t vset_embedding_encode;
    kvs_vcmd_stage_t vset_normalize;
    kvs_vcmd_stage_t vset_executor_queue_wait;
    kvs_vcmd_stage_t vset_store_set;
    kvs_vcmd_stage_t vset_vector_index_add;
    kvs_vcmd_stage_t vset_aof_append;
    kvs_vcmd_stage_t vset_replication;
    kvs_vcmd_stage_t vset_response_build;
    kvs_vcmd_stage_t vset_network_send;

    /* VSEARCH stages */
    kvs_vcmd_stage_t vsearch_total;
    kvs_vcmd_stage_t vsearch_resp_parse;
    kvs_vcmd_stage_t vsearch_task_enqueue;
    kvs_vcmd_stage_t vsearch_embedding_queue_wait;
    kvs_vcmd_stage_t vsearch_embedding_encode;
    kvs_vcmd_stage_t vsearch_normalize;
    kvs_vcmd_stage_t vsearch_executor_queue_wait;
    kvs_vcmd_stage_t vsearch_ann_search;
    kvs_vcmd_stage_t vsearch_candidate_lookup;
    kvs_vcmd_stage_t vsearch_ttl_filter;
    kvs_vcmd_stage_t vsearch_response_build;
    kvs_vcmd_stage_t vsearch_network_send;

    /* separation: VEC vs TEXT */
    kvs_vcmd_stage_t vset_vec_total;
    kvs_vcmd_stage_t vset_text_total;
    kvs_vcmd_stage_t vsearch_vec_total;
    kvs_vcmd_stage_t vsearch_text_total;
} kvs_vcmd_profile_t;

/* ── API ──────────────────────────────────────────────────────────────── */

void kvs_vcmd_profile_init(void);
void kvs_vcmd_profile_reset(void);
void kvs_vcmd_profile_dump(FILE *fp);
void kvs_vcmd_profile_dump_csv(FILE *fp);
const kvs_vcmd_profile_t *kvs_vcmd_profile_snapshot(void);

/* timing helpers */
uint64_t kvs_vcmd_profile_now_us(void);
void kvs_vcmd_profile_stage_record(kvs_vcmd_stage_t *s, uint64_t elapsed_us);
void kvs_vcmd_timing_init(kvs_vcmd_timing_t *t, int is_vset, int is_vec);
void kvs_vcmd_timing_mark(kvs_vcmd_timing_t *t, uint64_t *field);
void kvs_vcmd_timing_commit_vset(const kvs_vcmd_timing_t *t);
void kvs_vcmd_timing_commit_vsearch(const kvs_vcmd_timing_t *t);

/* percentile helpers */
uint64_t kvs_vcmd_stage_avg_us(const kvs_vcmd_stage_t *s);
uint64_t kvs_vcmd_stage_p50_us(const kvs_vcmd_stage_t *s);
uint64_t kvs_vcmd_stage_p95_us(const kvs_vcmd_stage_t *s);
uint64_t kvs_vcmd_stage_p99_us(const kvs_vcmd_stage_t *s);

#else /* !KVLITE_VCMD_PROFILE — zero-cost no-ops */

typedef struct { uint8_t _unused; } kvs_vcmd_timing_t;

#define kvs_vcmd_profile_init()            do {} while (0)
#define kvs_vcmd_profile_reset()           do {} while (0)
#define kvs_vcmd_profile_dump(fp)          do { (void)(fp); } while (0)
#define kvs_vcmd_profile_dump_csv(fp)      do { (void)(fp); } while (0)
#define kvs_vcmd_profile_snapshot()        ((const void *)0)

static inline uint64_t kvs_vcmd_profile_now_us(void) { return 0; }
static inline void kvs_vcmd_profile_stage_record(void *s, uint64_t us) {
    (void)s; (void)us;
}
static inline void kvs_vcmd_timing_init(kvs_vcmd_timing_t *t, int is_vset, int is_vec) {
    (void)t; (void)is_vset; (void)is_vec;
}
static inline void kvs_vcmd_timing_mark(kvs_vcmd_timing_t *t, uint64_t *field) {
    (void)t; (void)field;
}
#define kvs_vcmd_timing_commit_vset(t)     do { (void)(t); } while (0)
#define kvs_vcmd_timing_commit_vsearch(t)  do { (void)(t); } while (0)

#endif /* KVLITE_VCMD_PROFILE */

#endif /* KVS_VCMD_PROFILE_H */
