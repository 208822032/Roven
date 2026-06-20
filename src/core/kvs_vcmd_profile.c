/*
 * kvs_vcmd_profile.c — V-family command profiling implementation.
 *
 * All aggregation uses relaxed atomics — safe for concurrent access without
 * mutex overhead. Percentile estimation uses log2 histogram bucketing.
 */

#include "kvs_vcmd_profile.h"

#ifdef KVLITE_VCMD_PROFILE

#include "kvs_memory.h"
#include <string.h>
#include <time.h>

/* ── global state ─────────────────────────────────────────────────────── */

static kvs_vcmd_profile_t g_profile;

/* ── atomic helpers ───────────────────────────────────────────────────── */

static void atomic_add_u64(uint64_t *p, uint64_t v) {
    __atomic_add_fetch(p, v, __ATOMIC_RELAXED);
}

static void atomic_max_u64(uint64_t *p, uint64_t v) {
    uint64_t old = __atomic_load_n(p, __ATOMIC_RELAXED);
    while (v > old) {
        if (__atomic_compare_exchange_n(p, &old, v, 0,
                                        __ATOMIC_RELAXED, __ATOMIC_RELAXED))
            break;
    }
}

/* ── timing ───────────────────────────────────────────────────────────── */

uint64_t kvs_vcmd_profile_now_us(void) {
    struct timespec ts;
    if (clock_gettime(CLOCK_MONOTONIC, &ts) != 0) return 0;
    return (uint64_t)ts.tv_sec * 1000000ULL + (uint64_t)ts.tv_nsec / 1000ULL;
}

/* ── histogram buckets: log2(us), clamped to [0, HISTO_BUCKETS-1] ────── */

static int histo_bucket(uint64_t us) {
    if (us == 0) return 0;
    int b = 0;
    while (us > 1) { us >>= 1; b++; }
    return b < KVS_VCMD_PROFILE_HISTO_BUCKETS ? b : KVS_VCMD_PROFILE_HISTO_BUCKETS - 1;
}

/* ── stage record ─────────────────────────────────────────────────────── */

void kvs_vcmd_profile_stage_record(kvs_vcmd_stage_t *s, uint64_t elapsed_us) {
    if (!s) return;
    atomic_add_u64(&s->count, 1);
    atomic_add_u64(&s->total_us, elapsed_us);
    atomic_max_u64(&s->max_us, elapsed_us);
    int b = histo_bucket(elapsed_us);
    atomic_add_u64(&s->histogram[b], 1);
}

/* ── timing context ───────────────────────────────────────────────────── */

void kvs_vcmd_timing_init(kvs_vcmd_timing_t *t, int is_vset, int is_vec) {
    if (!t) return;
    memset(t, 0, sizeof(*t));
    t->is_vset = (uint8_t)(is_vset ? 1 : 0);
    t->is_vec  = (uint8_t)(is_vec  ? 1 : 0);
    t->active  = 1;
    t->total_start_us = kvs_vcmd_profile_now_us();
}

void kvs_vcmd_timing_mark(kvs_vcmd_timing_t *t, uint64_t *field) {
    if (!t || !t->active || !field) return;
    *field = kvs_vcmd_profile_now_us();
}

void kvs_vcmd_timing_commit_vset(const kvs_vcmd_timing_t *t) {
    uint64_t now, elapsed;
    kvs_vcmd_stage_t *stages;
    int have_embedding;

    if (!t || !t->active) return;

    now = kvs_vcmd_profile_now_us();

    /* total */
    elapsed = now >= t->total_start_us ? now - t->total_start_us : 0;
    kvs_vcmd_profile_stage_record(&g_profile.vset_total, elapsed);
    if (t->is_vec) {
        kvs_vcmd_profile_stage_record(&g_profile.vset_vec_total, elapsed);
    } else {
        kvs_vcmd_profile_stage_record(&g_profile.vset_text_total, elapsed);
    }

    have_embedding = !t->is_vec;

    /* resp_parse: total_start → parse_end */
    if (t->parse_end_us > 0 && t->parse_end_us >= t->total_start_us) {
        elapsed = t->parse_end_us - t->total_start_us;
        kvs_vcmd_profile_stage_record(&g_profile.vset_resp_parse, elapsed);
    }

    /* task_enqueue: parse_end → task_enqueue_end */
    if (t->task_enqueue_end_us > 0 && t->parse_end_us > 0 &&
        t->task_enqueue_end_us >= t->parse_end_us) {
        elapsed = t->task_enqueue_end_us - t->parse_end_us;
        kvs_vcmd_profile_stage_record(&g_profile.vset_task_enqueue, elapsed);
    }

    /* embedding_queue_wait: task_enqueue_end → embedding_queue_end */
    if (have_embedding && t->embedding_queue_end_us > 0 &&
        t->task_enqueue_end_us > 0 &&
        t->embedding_queue_end_us >= t->task_enqueue_end_us) {
        elapsed = t->embedding_queue_end_us - t->task_enqueue_end_us;
        kvs_vcmd_profile_stage_record(&g_profile.vset_embedding_queue_wait, elapsed);
    }

    /* embedding_encode: embedding_queue_end → embedding_encode_end */
    if (have_embedding && t->embedding_encode_end_us > 0 &&
        t->embedding_queue_end_us > 0 &&
        t->embedding_encode_end_us >= t->embedding_queue_end_us) {
        elapsed = t->embedding_encode_end_us - t->embedding_queue_end_us;
        kvs_vcmd_profile_stage_record(&g_profile.vset_embedding_encode, elapsed);
    }

    /* normalize: embedding_encode_end → normalize_end */
    if (have_embedding && t->normalize_end_us > 0 &&
        t->embedding_encode_end_us > 0 &&
        t->normalize_end_us >= t->embedding_encode_end_us) {
        elapsed = t->normalize_end_us - t->embedding_encode_end_us;
        kvs_vcmd_profile_stage_record(&g_profile.vset_normalize, elapsed);
    }

    /* executor_queue_wait: normalize_end (or task_enqueue_end for VEC) → executor_queue_end */
    {
        uint64_t ref = have_embedding ? t->normalize_end_us : t->task_enqueue_end_us;
        if (ref > 0 && t->executor_queue_end_us > 0 && t->executor_queue_end_us >= ref) {
            elapsed = t->executor_queue_end_us - ref;
            kvs_vcmd_profile_stage_record(&g_profile.vset_executor_queue_wait, elapsed);
        }
    }

    /* store_set: executor_queue_end → store_set_end */
    if (t->store_set_end_us > 0 && t->executor_queue_end_us > 0 &&
        t->store_set_end_us >= t->executor_queue_end_us) {
        elapsed = t->store_set_end_us - t->executor_queue_end_us;
        kvs_vcmd_profile_stage_record(&g_profile.vset_store_set, elapsed);
    }

    /* vector_index_add: store_set_end → index_add_end */
    if (t->index_add_end_us > 0 && t->store_set_end_us > 0 &&
        t->index_add_end_us >= t->store_set_end_us) {
        elapsed = t->index_add_end_us - t->store_set_end_us;
        kvs_vcmd_profile_stage_record(&g_profile.vset_vector_index_add, elapsed);
    }

    /* aof_append: index_add_end → aof_append_end */
    if (t->aof_append_end_us > 0 && t->index_add_end_us > 0 &&
        t->aof_append_end_us >= t->index_add_end_us) {
        elapsed = t->aof_append_end_us - t->index_add_end_us;
        kvs_vcmd_profile_stage_record(&g_profile.vset_aof_append, elapsed);
    }

    /* replication: aof_append_end → replication_end */
    if (t->replication_end_us > 0 && t->aof_append_end_us > 0 &&
        t->replication_end_us >= t->aof_append_end_us) {
        elapsed = t->replication_end_us - t->aof_append_end_us;
        kvs_vcmd_profile_stage_record(&g_profile.vset_replication, elapsed);
    }

    /* response_build: replication_end (or index_add_end) → response_build_end */
    {
        uint64_t ref = t->replication_end_us > 0 ? t->replication_end_us :
                       (t->index_add_end_us > 0 ? t->index_add_end_us :
                        t->store_set_end_us);
        if (ref > 0 && t->response_build_end_us > 0 &&
            t->response_build_end_us >= ref) {
            elapsed = t->response_build_end_us - ref;
            kvs_vcmd_profile_stage_record(&g_profile.vset_response_build, elapsed);
        }
    }

    /* network_send: response_build_end → network_send_end */
    if (t->network_send_end_us > 0 && t->response_build_end_us > 0 &&
        t->network_send_end_us >= t->response_build_end_us) {
        elapsed = t->network_send_end_us - t->response_build_end_us;
        kvs_vcmd_profile_stage_record(&g_profile.vset_network_send, elapsed);
    }
}

void kvs_vcmd_timing_commit_vsearch(const kvs_vcmd_timing_t *t) {
    uint64_t now, elapsed;
    int have_embedding;

    if (!t || !t->active) return;

    now = kvs_vcmd_profile_now_us();

    /* total */
    elapsed = now >= t->total_start_us ? now - t->total_start_us : 0;
    kvs_vcmd_profile_stage_record(&g_profile.vsearch_total, elapsed);
    if (t->is_vec) {
        kvs_vcmd_profile_stage_record(&g_profile.vsearch_vec_total, elapsed);
    } else {
        kvs_vcmd_profile_stage_record(&g_profile.vsearch_text_total, elapsed);
    }

    have_embedding = !t->is_vec;

    /* resp_parse */
    if (t->parse_end_us > 0 && t->parse_end_us >= t->total_start_us) {
        elapsed = t->parse_end_us - t->total_start_us;
        kvs_vcmd_profile_stage_record(&g_profile.vsearch_resp_parse, elapsed);
    }

    /* task_enqueue */
    if (t->task_enqueue_end_us > 0 && t->parse_end_us > 0 &&
        t->task_enqueue_end_us >= t->parse_end_us) {
        elapsed = t->task_enqueue_end_us - t->parse_end_us;
        kvs_vcmd_profile_stage_record(&g_profile.vsearch_task_enqueue, elapsed);
    }

    /* embedding_queue_wait */
    if (have_embedding && t->embedding_queue_end_us > 0 &&
        t->task_enqueue_end_us > 0 &&
        t->embedding_queue_end_us >= t->task_enqueue_end_us) {
        elapsed = t->embedding_queue_end_us - t->task_enqueue_end_us;
        kvs_vcmd_profile_stage_record(&g_profile.vsearch_embedding_queue_wait, elapsed);
    }

    /* embedding_encode */
    if (have_embedding && t->embedding_encode_end_us > 0 &&
        t->embedding_queue_end_us > 0 &&
        t->embedding_encode_end_us >= t->embedding_queue_end_us) {
        elapsed = t->embedding_encode_end_us - t->embedding_queue_end_us;
        kvs_vcmd_profile_stage_record(&g_profile.vsearch_embedding_encode, elapsed);
    }

    /* normalize */
    if (have_embedding && t->normalize_end_us > 0 &&
        t->embedding_encode_end_us > 0 &&
        t->normalize_end_us >= t->embedding_encode_end_us) {
        elapsed = t->normalize_end_us - t->embedding_encode_end_us;
        kvs_vcmd_profile_stage_record(&g_profile.vsearch_normalize, elapsed);
    }

    /* executor_queue_wait */
    {
        uint64_t ref = have_embedding ? t->normalize_end_us : t->task_enqueue_end_us;
        if (ref > 0 && t->executor_queue_end_us > 0 &&
            t->executor_queue_end_us >= ref) {
            elapsed = t->executor_queue_end_us - ref;
            kvs_vcmd_profile_stage_record(&g_profile.vsearch_executor_queue_wait, elapsed);
        }
    }

    /* ann_search: executor_queue_end → ann_search_end */
    if (t->ann_search_end_us > 0 && t->executor_queue_end_us > 0 &&
        t->ann_search_end_us >= t->executor_queue_end_us) {
        elapsed = t->ann_search_end_us - t->executor_queue_end_us;
        kvs_vcmd_profile_stage_record(&g_profile.vsearch_ann_search, elapsed);
    }

    /* candidate_lookup: ann_search_end → candidate_lookup_end */
    if (t->candidate_lookup_end_us > 0 && t->ann_search_end_us > 0 &&
        t->candidate_lookup_end_us >= t->ann_search_end_us) {
        elapsed = t->candidate_lookup_end_us - t->ann_search_end_us;
        kvs_vcmd_profile_stage_record(&g_profile.vsearch_candidate_lookup, elapsed);
    }

    /* ttl_filter: candidate_lookup_end → ttl_filter_end */
    if (t->ttl_filter_end_us > 0 && t->candidate_lookup_end_us > 0 &&
        t->ttl_filter_end_us >= t->candidate_lookup_end_us) {
        elapsed = t->ttl_filter_end_us - t->candidate_lookup_end_us;
        kvs_vcmd_profile_stage_record(&g_profile.vsearch_ttl_filter, elapsed);
    }

    /* response_build */
    {
        uint64_t ref = t->ttl_filter_end_us > 0 ? t->ttl_filter_end_us :
                       t->candidate_lookup_end_us;
        if (ref > 0 && t->response_build_end_us > 0 &&
            t->response_build_end_us >= ref) {
            elapsed = t->response_build_end_us - ref;
            kvs_vcmd_profile_stage_record(&g_profile.vsearch_response_build, elapsed);
        }
    }

    /* network_send */
    if (t->network_send_end_us > 0 && t->response_build_end_us > 0 &&
        t->network_send_end_us >= t->response_build_end_us) {
        elapsed = t->network_send_end_us - t->response_build_end_us;
        kvs_vcmd_profile_stage_record(&g_profile.vsearch_network_send, elapsed);
    }
}

/* ── percentile estimation from histogram ─────────────────────────────── */

static uint64_t histo_percentile(const kvs_vcmd_stage_t *s, double p) {
    uint64_t total = s->count;
    uint64_t threshold;
    uint64_t cumulative = 0;
    int b;

    if (total == 0) return 0;
    threshold = (uint64_t)((double)total * p / 100.0);
    if (threshold == 0 && total > 0) threshold = 1;

    for (b = 0; b < KVS_VCMD_PROFILE_HISTO_BUCKETS; b++) {
        cumulative += s->histogram[b];
        if (cumulative >= threshold) {
            /* bucket b corresponds to values in [2^(b-1), 2^b) us */
            if (b == 0) return 1;
            return 1ULL << (uint64_t)(b - 1);
        }
    }
    return s->max_us;
}

uint64_t kvs_vcmd_stage_avg_us(const kvs_vcmd_stage_t *s) {
    if (!s || s->count == 0) return 0;
    return s->total_us / s->count;
}

uint64_t kvs_vcmd_stage_p50_us(const kvs_vcmd_stage_t *s) {
    return histo_percentile(s, 50.0);
}

uint64_t kvs_vcmd_stage_p95_us(const kvs_vcmd_stage_t *s) {
    return histo_percentile(s, 95.0);
}

uint64_t kvs_vcmd_stage_p99_us(const kvs_vcmd_stage_t *s) {
    return histo_percentile(s, 99.0);
}

/* ── init / reset ─────────────────────────────────────────────────────── */

void kvs_vcmd_profile_init(void) {
    memset(&g_profile, 0, sizeof(g_profile));
}

void kvs_vcmd_profile_reset(void) {
    memset(&g_profile, 0, sizeof(g_profile));
}

const kvs_vcmd_profile_t *kvs_vcmd_profile_snapshot(void) {
    return &g_profile;
}

/* ── human-readable dump ──────────────────────────────────────────────── */

static void dump_stage(FILE *fp, const char *name, const kvs_vcmd_stage_t *s) {
    if (s->count == 0) {
        fprintf(fp, "  %-35s  (no samples)\n", name);
        return;
    }
    fprintf(fp, "  %-35s  cnt=%8lu  avg=%8lu us  p50=%8lu us  p95=%8lu us  p99=%8lu us  max=%8lu us\n",
            name,
            (unsigned long)s->count,
            (unsigned long)kvs_vcmd_stage_avg_us(s),
            (unsigned long)kvs_vcmd_stage_p50_us(s),
            (unsigned long)kvs_vcmd_stage_p95_us(s),
            (unsigned long)kvs_vcmd_stage_p99_us(s),
            (unsigned long)s->max_us);
}

void kvs_vcmd_profile_dump(FILE *fp) {
    if (!fp) fp = stderr;

    fprintf(fp, "\n========== KVLite V-Cmd Profile Report ==========\n\n");

    /* VSET */
    fprintf(fp, "--- VSET ---\n");
    dump_stage(fp, "vset_total",               &g_profile.vset_total);
    dump_stage(fp, "vset_resp_parse",           &g_profile.vset_resp_parse);
    dump_stage(fp, "vset_task_enqueue",         &g_profile.vset_task_enqueue);
    dump_stage(fp, "vset_embedding_queue_wait", &g_profile.vset_embedding_queue_wait);
    dump_stage(fp, "vset_embedding_encode",     &g_profile.vset_embedding_encode);
    dump_stage(fp, "vset_normalize",            &g_profile.vset_normalize);
    dump_stage(fp, "vset_executor_queue_wait",  &g_profile.vset_executor_queue_wait);
    dump_stage(fp, "vset_store_set",            &g_profile.vset_store_set);
    dump_stage(fp, "vset_vector_index_add",     &g_profile.vset_vector_index_add);
    dump_stage(fp, "vset_aof_append",           &g_profile.vset_aof_append);
    dump_stage(fp, "vset_replication",          &g_profile.vset_replication);
    dump_stage(fp, "vset_response_build",       &g_profile.vset_response_build);
    dump_stage(fp, "vset_network_send",         &g_profile.vset_network_send);

    fprintf(fp, "\n--- VSEARCH ---\n");
    dump_stage(fp, "vsearch_total",               &g_profile.vsearch_total);
    dump_stage(fp, "vsearch_resp_parse",           &g_profile.vsearch_resp_parse);
    dump_stage(fp, "vsearch_task_enqueue",         &g_profile.vsearch_task_enqueue);
    dump_stage(fp, "vsearch_embedding_queue_wait", &g_profile.vsearch_embedding_queue_wait);
    dump_stage(fp, "vsearch_embedding_encode",     &g_profile.vsearch_embedding_encode);
    dump_stage(fp, "vsearch_normalize",            &g_profile.vsearch_normalize);
    dump_stage(fp, "vsearch_executor_queue_wait",  &g_profile.vsearch_executor_queue_wait);
    dump_stage(fp, "vsearch_ann_search",           &g_profile.vsearch_ann_search);
    dump_stage(fp, "vsearch_candidate_lookup",     &g_profile.vsearch_candidate_lookup);
    dump_stage(fp, "vsearch_ttl_filter",           &g_profile.vsearch_ttl_filter);
    dump_stage(fp, "vsearch_response_build",       &g_profile.vsearch_response_build);
    dump_stage(fp, "vsearch_network_send",         &g_profile.vsearch_network_send);

    fprintf(fp, "\n--- VEC vs TEXT ─────────────────────────────────\n");
    dump_stage(fp, "vset_vec_total",   &g_profile.vset_vec_total);
    dump_stage(fp, "vset_text_total",  &g_profile.vset_text_total);
    dump_stage(fp, "vsearch_vec_total",  &g_profile.vsearch_vec_total);
    dump_stage(fp, "vsearch_text_total", &g_profile.vsearch_text_total);

    fprintf(fp, "\n=================================================\n\n");
}

/* ── CSV dump (machine-readable) ──────────────────────────────────────── */

static void dump_stage_csv(FILE *fp, const char *prefix, const char *name,
                           const kvs_vcmd_stage_t *s) {
    fprintf(fp, "%s,%s,%lu,%lu,%lu,%lu,%lu,%lu\n",
            prefix, name,
            (unsigned long)s->count,
            (unsigned long)kvs_vcmd_stage_avg_us(s),
            (unsigned long)kvs_vcmd_stage_p50_us(s),
            (unsigned long)kvs_vcmd_stage_p95_us(s),
            (unsigned long)kvs_vcmd_stage_p99_us(s),
            (unsigned long)s->max_us);
}

void kvs_vcmd_profile_dump_csv(FILE *fp) {
    if (!fp) return;
    fprintf(fp, "command,stage,count,avg_us,p50_us,p95_us,p99_us,max_us\n");

    dump_stage_csv(fp, "VSET", "total",               &g_profile.vset_total);
    dump_stage_csv(fp, "VSET", "resp_parse",           &g_profile.vset_resp_parse);
    dump_stage_csv(fp, "VSET", "task_enqueue",         &g_profile.vset_task_enqueue);
    dump_stage_csv(fp, "VSET", "embedding_queue_wait", &g_profile.vset_embedding_queue_wait);
    dump_stage_csv(fp, "VSET", "embedding_encode",     &g_profile.vset_embedding_encode);
    dump_stage_csv(fp, "VSET", "normalize",            &g_profile.vset_normalize);
    dump_stage_csv(fp, "VSET", "executor_queue_wait",  &g_profile.vset_executor_queue_wait);
    dump_stage_csv(fp, "VSET", "store_set",            &g_profile.vset_store_set);
    dump_stage_csv(fp, "VSET", "vector_index_add",     &g_profile.vset_vector_index_add);
    dump_stage_csv(fp, "VSET", "aof_append",           &g_profile.vset_aof_append);
    dump_stage_csv(fp, "VSET", "replication",          &g_profile.vset_replication);
    dump_stage_csv(fp, "VSET", "response_build",       &g_profile.vset_response_build);
    dump_stage_csv(fp, "VSET", "network_send",         &g_profile.vset_network_send);

    dump_stage_csv(fp, "VSEARCH", "total",               &g_profile.vsearch_total);
    dump_stage_csv(fp, "VSEARCH", "resp_parse",           &g_profile.vsearch_resp_parse);
    dump_stage_csv(fp, "VSEARCH", "task_enqueue",         &g_profile.vsearch_task_enqueue);
    dump_stage_csv(fp, "VSEARCH", "embedding_queue_wait", &g_profile.vsearch_embedding_queue_wait);
    dump_stage_csv(fp, "VSEARCH", "embedding_encode",     &g_profile.vsearch_embedding_encode);
    dump_stage_csv(fp, "VSEARCH", "normalize",            &g_profile.vsearch_normalize);
    dump_stage_csv(fp, "VSEARCH", "executor_queue_wait",  &g_profile.vsearch_executor_queue_wait);
    dump_stage_csv(fp, "VSEARCH", "ann_search",           &g_profile.vsearch_ann_search);
    dump_stage_csv(fp, "VSEARCH", "candidate_lookup",     &g_profile.vsearch_candidate_lookup);
    dump_stage_csv(fp, "VSEARCH", "ttl_filter",           &g_profile.vsearch_ttl_filter);
    dump_stage_csv(fp, "VSEARCH", "response_build",       &g_profile.vsearch_response_build);
    dump_stage_csv(fp, "VSEARCH", "network_send",         &g_profile.vsearch_network_send);
}

#endif /* KVLITE_VCMD_PROFILE */
