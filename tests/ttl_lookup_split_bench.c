#include "kvs_api.h"
#include "kvs_memory_internal.h"
#include "kvs_rbtree.h"
#include "kvs_skiptable.h"

#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

kvs_config_t g_cfg;

extern kvs_rbtree_t global_rbtree;
extern kvs_skiptable_t global_skiptable;

#define DEFAULT_KEYSPACE 100000
#define DEFAULT_REQUESTS 500000
#define DEFAULT_WARMUP 10000
#define DEFAULT_VALUE_SIZE 32
#define KEY_BUFFER_SIZE 32

typedef struct {
    char **keys;
    size_t key_count;
    size_t *key_lens;
    uint32_t *indices;
    size_t request_count;
    char *value;
    size_t value_size;
} bench_data_t;

static volatile unsigned long g_checksum = 0;

static uint32_t lcg_next(uint32_t *state) {
    *state = (*state * 1664525u) + 1013904223u;
    return *state;
}

static double elapsed_seconds(const struct timespec *begin, const struct timespec *end) {
    return (double)(end->tv_sec - begin->tv_sec) + (double)(end->tv_nsec - begin->tv_nsec) / 1000000000.0;
}

static void make_value(char *buffer, size_t size) {
    if (!buffer || size == 0) return;
    if (size == 1) {
        buffer[0] = '\0';
        return;
    }
    memset(buffer, 'v', size - 1);
    buffer[size - 1] = '\0';
}

static int prepare_bench_data(bench_data_t *data, size_t key_count, size_t request_count, size_t value_size) {
    if (!data || key_count == 0 || request_count == 0 || value_size == 0) return -1;

    memset(data, 0, sizeof(*data));
    data->keys = calloc(key_count, sizeof(char *));
    data->key_lens = calloc(key_count, sizeof(size_t));
    data->indices = calloc(request_count, sizeof(uint32_t));
    data->value = calloc(value_size + 1, sizeof(char));
    if (!data->keys || !data->key_lens || !data->indices || !data->value) {
        return -1;
    }

    data->key_count = key_count;
    data->request_count = request_count;
    data->value_size = value_size;

    make_value(data->value, value_size + 1);

    for (size_t i = 0; i < key_count; ++i) {
        data->keys[i] = calloc(KEY_BUFFER_SIZE, sizeof(char));
        if (!data->keys[i]) {
            return -1;
        }
        snprintf(data->keys[i], KEY_BUFFER_SIZE, "ttlkey%06zu", i);
        data->key_lens[i] = strlen(data->keys[i]);
    }

    uint32_t seed = 0x12345678u;
    for (size_t i = 0; i < request_count; ++i) {
        data->indices[i] = lcg_next(&seed) % (uint32_t)key_count;
    }

    return 0;
}

static void destroy_bench_data(bench_data_t *data) {
    if (!data) return;
    if (data->keys) {
        for (size_t i = 0; i < data->key_count; ++i) {
            free(data->keys[i]);
        }
        free(data->keys);
    }
    free(data->key_lens);
    free(data->indices);
    free(data->value);
    memset(data, 0, sizeof(*data));
}

static rbtree_node *lookup_rbtree_node(const char *key) {
    if (!key || !global_rbtree.root || !global_rbtree.nil) return NULL;

    return kvs_rbtree_lookup_node(&global_rbtree, key, strlen(key));
}

static Node *lookup_skiptable_node(const char *key) {
    if (!key || !global_skiptable.header) return NULL;

    Node *current = global_skiptable.header;
    for (int i = global_skiptable.level; i >= 0; --i) {
        while (current->forward[i] && strcmp(current->forward[i]->key, key) < 0) {
            current = current->forward[i];
        }
    }

    current = current->forward[0];
    if (current && strcmp(current->key, key) == 0) {
        return current;
    }

    return NULL;
}

static int ttl_probe_rbtree(const char *key) {
    rbtree_node *node = lookup_rbtree_node(key);
    if (!node) return 0;
    return !kv_is_expired(node->expire_at_ms, kv_now_ms());
}

static int ttl_probe_skiptable(const char *key) {
    Node *node = lookup_skiptable_node(key);
    if (!node) return 0;
    return !kv_is_expired(node->expire_at_ms, kv_now_ms());
}

static double bench_rbtree_pure_get(const bench_data_t *data) {
    struct timespec begin;
    struct timespec end;
    volatile unsigned long checksum = 0;

    clock_gettime(CLOCK_MONOTONIC, &begin);
    for (size_t i = 0; i < data->request_count; ++i) {
        char *value = kvs_rbtree_get(&global_rbtree, data->keys[data->indices[i]]);
        if (value) {
            checksum += (unsigned char)value[0];
        }
    }
    clock_gettime(CLOCK_MONOTONIC, &end);

    g_checksum += checksum;
    return elapsed_seconds(&begin, &end);
}

static double bench_rbtree_ttl_probe(const bench_data_t *data) {
    struct timespec begin;
    struct timespec end;
    volatile unsigned long checksum = 0;

    clock_gettime(CLOCK_MONOTONIC, &begin);
    for (size_t i = 0; i < data->request_count; ++i) {
        checksum += (unsigned long)ttl_probe_rbtree(data->keys[data->indices[i]]);
    }
    clock_gettime(CLOCK_MONOTONIC, &end);

    g_checksum += checksum;
    return elapsed_seconds(&begin, &end);
}

static double bench_rbtree_current_get_core(const bench_data_t *data) {
    struct timespec begin;
    struct timespec end;
    volatile unsigned long checksum = 0;

    clock_gettime(CLOCK_MONOTONIC, &begin);
    for (size_t i = 0; i < data->request_count; ++i) {
        const char *key = data->keys[data->indices[i]];
        rbtree_node *node = lookup_rbtree_node(key);
        if (node && !kv_is_expired(node->expire_at_ms, kv_now_ms())) {
            checksum += (unsigned char)((char *)node->value)[0];
        }
    }
    clock_gettime(CLOCK_MONOTONIC, &end);

    g_checksum += checksum;
    return elapsed_seconds(&begin, &end);
}

static double bench_skiptable_pure_get(const bench_data_t *data) {
    struct timespec begin;
    struct timespec end;
    volatile unsigned long checksum = 0;

    clock_gettime(CLOCK_MONOTONIC, &begin);
    for (size_t i = 0; i < data->request_count; ++i) {
        char *value = kvs_skiptable_get(&global_skiptable, data->keys[data->indices[i]]);
        if (value) {
            checksum += (unsigned char)value[0];
        }
    }
    clock_gettime(CLOCK_MONOTONIC, &end);

    g_checksum += checksum;
    return elapsed_seconds(&begin, &end);
}

static double bench_skiptable_ttl_probe(const bench_data_t *data) {
    struct timespec begin;
    struct timespec end;
    volatile unsigned long checksum = 0;

    clock_gettime(CLOCK_MONOTONIC, &begin);
    for (size_t i = 0; i < data->request_count; ++i) {
        checksum += (unsigned long)ttl_probe_skiptable(data->keys[data->indices[i]]);
    }
    clock_gettime(CLOCK_MONOTONIC, &end);

    g_checksum += checksum;
    return elapsed_seconds(&begin, &end);
}

static double bench_skiptable_current_get_core(const bench_data_t *data) {
    struct timespec begin;
    struct timespec end;
    volatile unsigned long checksum = 0;

    clock_gettime(CLOCK_MONOTONIC, &begin);
    for (size_t i = 0; i < data->request_count; ++i) {
        const char *key = data->keys[data->indices[i]];
        Node *node = lookup_skiptable_node(key);
        if (node && !kv_is_expired(node->expire_at_ms, kv_now_ms())) {
            checksum += (unsigned char)node->value[0];
        }
    }
    clock_gettime(CLOCK_MONOTONIC, &end);

    g_checksum += checksum;
    return elapsed_seconds(&begin, &end);
}

static void populate_rbtree_dataset(const bench_data_t *data) {
    for (size_t i = 0; i < data->key_count; ++i) {
        (void)kvs_rbtree_set(&global_rbtree, data->keys[i], data->value);
    }
}

static void populate_skiptable_dataset(const bench_data_t *data) {
    for (size_t i = 0; i < data->key_count; ++i) {
        (void)kvs_skiptable_set(&global_skiptable, data->keys[i], data->value);
    }
}

static void report_case(const char *ds, const char *variant, size_t requests, double seconds) {
    double qps = (double)requests / seconds;
    double ns_per_op = (seconds * 1000000000.0) / (double)requests;
    printf("[RESULT][%s][%s] requests=%zu seconds=%.6f qps=%.2f ns_per_op=%.2f\n",
           ds, variant, requests, seconds, qps, ns_per_op);
}

static void report_split(const char *ds, double pure_ns, double ttl_probe_ns, double current_ns) {
    double ttl_penalty_ns = ttl_probe_ns - pure_ns;
    double total_penalty_ns = current_ns - pure_ns;
    double ttl_share = 0.0;
    if (total_penalty_ns > 0.0) {
        ttl_share = (ttl_penalty_ns / total_penalty_ns) * 100.0;
    }

    printf("[DELTA][%s] pure_ns=%.2f ttl_probe_ns=%.2f current_get_ns=%.2f ttl_penalty_ns=%.2f total_penalty_ns=%.2f ttl_share=%.2f%%\n",
           ds, pure_ns, ttl_probe_ns, current_ns, ttl_penalty_ns, total_penalty_ns, ttl_share);
}

int main(void) {
    const size_t keyspace = DEFAULT_KEYSPACE;
    const size_t requests = DEFAULT_REQUESTS;
    const size_t warmup = DEFAULT_WARMUP;
    const size_t value_size = DEFAULT_VALUE_SIZE;

    bench_data_t data;
    if (prepare_bench_data(&data, keyspace, requests, value_size) != 0) {
        fprintf(stderr, "failed to prepare bench data\n");
        destroy_bench_data(&data);
        return 1;
    }

    mem_set_backend(KVS_MEM_BACKEND_SYSTEM);
    srand(1);

    if (kvs_rbtree_create(&global_rbtree) != 0) {
        fprintf(stderr, "failed to create rbtree\n");
        destroy_bench_data(&data);
        return 1;
    }
    if (kvs_skiptable_create(&global_skiptable) != 0) {
        fprintf(stderr, "failed to create skiptable\n");
        destroy_bench_data(&data);
        return 1;
    }

    populate_rbtree_dataset(&data);
    populate_skiptable_dataset(&data);

    printf("[INFO] ttl_lookup_split_bench keyspace=%zu requests=%zu warmup=%zu value_size=%zu\n",
           keyspace, requests, warmup, value_size);

    for (size_t i = 0; i < warmup; ++i) {
        (void)kvs_rbtree_get(&global_rbtree, data.keys[data.indices[i % requests]]);
        (void)kvs_skiptable_get(&global_skiptable, data.keys[data.indices[i % requests]]);
    }

    double rbtree_pure_sec = bench_rbtree_pure_get(&data);
    double rbtree_ttl_probe_sec = bench_rbtree_ttl_probe(&data);
    double rbtree_current_sec = bench_rbtree_current_get_core(&data);

    double skiptable_pure_sec = bench_skiptable_pure_get(&data);
    double skiptable_ttl_probe_sec = bench_skiptable_ttl_probe(&data);
    double skiptable_current_sec = bench_skiptable_current_get_core(&data);

    report_case("rbtree", "pure_get", requests, rbtree_pure_sec);
    report_case("rbtree", "ttl_probe", requests, rbtree_ttl_probe_sec);
    report_case("rbtree", "current_get_core", requests, rbtree_current_sec);
    report_split("rbtree",
                 (rbtree_pure_sec * 1000000000.0) / (double)requests,
                 (rbtree_ttl_probe_sec * 1000000000.0) / (double)requests,
                 (rbtree_current_sec * 1000000000.0) / (double)requests);

    report_case("skiptable", "pure_get", requests, skiptable_pure_sec);
    report_case("skiptable", "ttl_probe", requests, skiptable_ttl_probe_sec);
    report_case("skiptable", "current_get_core", requests, skiptable_current_sec);
    report_split("skiptable",
                 (skiptable_pure_sec * 1000000000.0) / (double)requests,
                 (skiptable_ttl_probe_sec * 1000000000.0) / (double)requests,
                 (skiptable_current_sec * 1000000000.0) / (double)requests);

    printf("[CHECKSUM] %lu\n", g_checksum);

    destroy_bench_data(&data);
    return 0;
}
