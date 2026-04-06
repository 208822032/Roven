#include "kvs_api.h"
#include "kvs_memory_internal.h"
#include "kvs_skiptable.h"

#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

extern int random_level(void);
extern Node *create_node(int level, const char *key, const char *value);

kvs_config_t g_cfg;

#define DEFAULT_KEYSPACE 100000
#define DEFAULT_LOOKUP_REQUESTS 300000
#define DEFAULT_INSERT_REQUESTS 200000
#define DEFAULT_WARMUP 10000
#define DEFAULT_VALUE_SIZE 32
#define KEY_BUFFER_SIZE 32

typedef struct {
    char **existing_keys;
    char **missing_keys;
    char **insert_keys;
    size_t keyspace;
    size_t lookup_requests;
    size_t insert_requests;
    size_t *lookup_indices;
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
    if (!buffer || size == 0) {
        return;
    }
    if (size == 1) {
        buffer[0] = '\0';
        return;
    }
    memset(buffer, 'v', size - 1);
    buffer[size - 1] = '\0';
}

static int prepare_bench_data(bench_data_t *data,
                              size_t keyspace,
                              size_t lookup_requests,
                              size_t insert_requests,
                              size_t value_size) {
    if (!data || keyspace == 0 || lookup_requests == 0 || insert_requests == 0 || value_size == 0) {
        return -1;
    }

    memset(data, 0, sizeof(*data));
    data->existing_keys = calloc(keyspace, sizeof(char *));
    data->missing_keys = calloc(lookup_requests, sizeof(char *));
    data->insert_keys = calloc(insert_requests, sizeof(char *));
    data->lookup_indices = calloc(lookup_requests, sizeof(size_t));
    data->value = calloc(value_size + 1, sizeof(char));
    if (!data->existing_keys || !data->missing_keys || !data->insert_keys || !data->lookup_indices || !data->value) {
        return -1;
    }

    data->keyspace = keyspace;
    data->lookup_requests = lookup_requests;
    data->insert_requests = insert_requests;
    data->value_size = value_size;

    make_value(data->value, value_size + 1);

    for (size_t i = 0; i < keyspace; ++i) {
        data->existing_keys[i] = calloc(KEY_BUFFER_SIZE, sizeof(char));
        if (!data->existing_keys[i]) {
            return -1;
        }
        snprintf(data->existing_keys[i], KEY_BUFFER_SIZE, "skipkey%06zu", i);
    }

    for (size_t i = 0; i < lookup_requests; ++i) {
        data->missing_keys[i] = calloc(KEY_BUFFER_SIZE, sizeof(char));
        if (!data->missing_keys[i]) {
            return -1;
        }
        snprintf(data->missing_keys[i], KEY_BUFFER_SIZE, "misskey%06zu", keyspace + i);
    }

    for (size_t i = 0; i < insert_requests; ++i) {
        data->insert_keys[i] = calloc(KEY_BUFFER_SIZE, sizeof(char));
        if (!data->insert_keys[i]) {
            return -1;
        }
        snprintf(data->insert_keys[i], KEY_BUFFER_SIZE, "inskey%06zu", i);
    }

    uint32_t seed = 0x13572468u;
    for (size_t i = 0; i < lookup_requests; ++i) {
        data->lookup_indices[i] = (size_t)(lcg_next(&seed) % (uint32_t)keyspace);
    }

    return 0;
}

static void destroy_bench_data(bench_data_t *data) {
    if (!data) {
        return;
    }
    if (data->existing_keys) {
        for (size_t i = 0; i < data->keyspace; ++i) {
            free(data->existing_keys[i]);
        }
        free(data->existing_keys);
    }
    if (data->missing_keys) {
        for (size_t i = 0; i < data->lookup_requests; ++i) {
            free(data->missing_keys[i]);
        }
        free(data->missing_keys);
    }
    if (data->insert_keys) {
        for (size_t i = 0; i < data->insert_requests; ++i) {
            free(data->insert_keys[i]);
        }
        free(data->insert_keys);
    }
    free(data->lookup_indices);
    free(data->value);
    memset(data, 0, sizeof(*data));
}

static int node_key_compare(const Node *node, const char *key, size_t key_len) {
    if (!node || !node->key || !key) {
        return -1;
    }

    size_t node_key_len = node->key_len;
    size_t min_len = node_key_len < key_len ? node_key_len : key_len;

    if (min_len > 0) {
        int cmp = memcmp(node->key, key, min_len);
        if (cmp != 0) {
            return cmp;
        }
    }

    if (node_key_len < key_len) {
        return -1;
    }
    if (node_key_len > key_len) {
        return 1;
    }
    return 0;
}

static Node *legacy_find_with_update(kvs_skiptable_t *table,
                                     const char *key,
                                     size_t key_len,
                                     Node **update) {
    if (!table || !table->header || !key) {
        return NULL;
    }

    Node *current = table->header;
    for (int i = table->level; i >= 0; --i) {
        while (current->forward[i] && node_key_compare(current->forward[i], key, key_len) < 0) {
            current = current->forward[i];
        }
        if (update) {
            update[i] = current;
        }
    }

    return current->forward[0];
}

static Node *legacy_find_exact(kvs_skiptable_t *table, const char *key, size_t key_len) {
    if (!table || !table->header || !key) {
        return NULL;
    }

    Node *current = table->header;
    for (int i = table->level; i >= 0; --i) {
        while (current->forward[i]) {
            int cmp = node_key_compare(current->forward[i], key, key_len);
            if (cmp < 0) {
                current = current->forward[i];
                continue;
            }
            if (cmp == 0) {
                return current->forward[i];
            }
            break;
        }
    }

    return NULL;
}

static int legacy_update_value(Node *node, const char *value) {
    if (!node || !value) {
        return -1;
    }

    size_t value_len = strlen(value);
    if (node->value && node->value_len >= value_len) {
        memcpy(node->value, value, value_len + 1);
    } else if (node->value) {
        char *new_value = kvs_realloc(node->value, value_len + 1);
        if (!new_value) {
            return -1;
        }
        memcpy(new_value, value, value_len + 1);
        node->value = new_value;
    } else {
        char *new_value = kvs_malloc(value_len + 1);
        if (!new_value) {
            return -1;
        }
        memcpy(new_value, value, value_len + 1);
        node->value = new_value;
    }

    node->value_len = (uint32_t)value_len;
    return 0;
}

static int legacy_insert_like_previous_code(kvs_skiptable_t *table, char *key, char *value) {
    if (!table || !table->header || !key || !value) {
        return -1;
    }

    size_t key_len = strlen(key);
    Node *current = legacy_find_exact(table, key, key_len);
    if (current) {
        if (legacy_update_value(current, value) != 0) {
            return -1;
        }
        current->expire_at_ms = KV_TTL_NO_EXPIRE;
        return 1;
    }

    Node *update[MAX_LEVEL + 1];
    current = legacy_find_with_update(table, key, key_len, update);
    (void)current;

    int new_level = random_level();
    Node *new_node = create_node(new_level, key, value);
    if (!new_node) {
        return -1;
    }

    table->count++;
    if (new_level > table->level) {
        for (int i = table->level + 1; i <= new_level; ++i) {
            update[i] = table->header;
        }
        table->level = new_level;
    }

    for (int i = 0; i <= new_level; ++i) {
        new_node->forward[i] = update[i]->forward[i];
        update[i]->forward[i] = new_node;
    }

    return 0;
}

static void populate_table(kvs_skiptable_t *table, const bench_data_t *data) {
    for (size_t i = 0; i < data->keyspace; ++i) {
        (void)kvs_skiptable_set(table, data->existing_keys[i], data->value);
    }
}

static double benchmark_existing_lookup(const bench_data_t *data) {
    kvs_skiptable_t table;
    struct timespec begin;
    struct timespec end;
    volatile unsigned long checksum = 0;

    if (kvs_skiptable_create(&table) != 0) {
        return -1.0;
    }

    populate_table(&table, data);

    for (size_t i = 0; i < DEFAULT_WARMUP; ++i) {
        (void)kvs_skiptable_get(&table, data->existing_keys[data->lookup_indices[i % data->lookup_requests]]);
    }

    clock_gettime(CLOCK_MONOTONIC, &begin);
    for (size_t i = 0; i < data->lookup_requests; ++i) {
        char *value = kvs_skiptable_get(&table, data->existing_keys[data->lookup_indices[i]]);
        if (value) {
            checksum += (unsigned char)value[0];
        }
    }
    clock_gettime(CLOCK_MONOTONIC, &end);

    g_checksum += checksum;
    kvs_skiptable_destroy(&table);
    return elapsed_seconds(&begin, &end);
}

static double benchmark_missing_lookup(const bench_data_t *data) {
    kvs_skiptable_t table;
    struct timespec begin;
    struct timespec end;
    volatile unsigned long checksum = 0;

    if (kvs_skiptable_create(&table) != 0) {
        return -1.0;
    }

    populate_table(&table, data);

    for (size_t i = 0; i < DEFAULT_WARMUP; ++i) {
        (void)kvs_skiptable_get(&table, data->missing_keys[i % data->lookup_requests]);
    }

    clock_gettime(CLOCK_MONOTONIC, &begin);
    for (size_t i = 0; i < data->lookup_requests; ++i) {
        char *value = kvs_skiptable_get(&table, data->missing_keys[i]);
        if (value) {
            checksum += (unsigned char)value[0];
        }
    }
    clock_gettime(CLOCK_MONOTONIC, &end);

    g_checksum += checksum;
    kvs_skiptable_destroy(&table);
    return elapsed_seconds(&begin, &end);
}

static double benchmark_insert_legacy(const bench_data_t *data) {
    kvs_skiptable_t table;
    struct timespec begin;
    struct timespec end;
    volatile unsigned long checksum = 0;

    if (kvs_skiptable_create(&table) != 0) {
        return -1.0;
    }

    clock_gettime(CLOCK_MONOTONIC, &begin);
    for (size_t i = 0; i < data->insert_requests; ++i) {
        checksum += (unsigned long)legacy_insert_like_previous_code(&table, data->insert_keys[i], data->value);
    }
    clock_gettime(CLOCK_MONOTONIC, &end);

    g_checksum += checksum;
    kvs_skiptable_destroy(&table);
    return elapsed_seconds(&begin, &end);
}

static double benchmark_insert_current(const bench_data_t *data) {
    kvs_skiptable_t table;
    struct timespec begin;
    struct timespec end;
    volatile unsigned long checksum = 0;

    if (kvs_skiptable_create(&table) != 0) {
        return -1.0;
    }

    clock_gettime(CLOCK_MONOTONIC, &begin);
    for (size_t i = 0; i < data->insert_requests; ++i) {
        checksum += (unsigned long)kvs_skiptable_set(&table, data->insert_keys[i], data->value);
    }
    clock_gettime(CLOCK_MONOTONIC, &end);

    g_checksum += checksum;
    kvs_skiptable_destroy(&table);
    return elapsed_seconds(&begin, &end);
}

static void report_case(const char *variant, size_t requests, double seconds) {
    double qps = (double)requests / seconds;
    double ns_per_op = (seconds * 1000000000.0) / (double)requests;
    printf("[RESULT][skiptable][%s] requests=%zu seconds=%.6f qps=%.2f ns_per_op=%.2f\n",
           variant, requests, seconds, qps, ns_per_op);
}

static void report_delta(const char *label, double legacy_ns, double current_ns) {
    double saved_ns = legacy_ns - current_ns;
    double saved_pct = 0.0;
    if (legacy_ns > 0.0) {
        saved_pct = (saved_ns / legacy_ns) * 100.0;
    }
    printf("[DELTA][skiptable][%s] legacy_ns=%.2f current_ns=%.2f saved_ns=%.2f saved_pct=%.2f%%\n",
           label, legacy_ns, current_ns, saved_ns, saved_pct);
}

int main(void) {
    const size_t keyspace = DEFAULT_KEYSPACE;
    const size_t lookup_requests = DEFAULT_LOOKUP_REQUESTS;
    const size_t insert_requests = DEFAULT_INSERT_REQUESTS;
    const size_t value_size = DEFAULT_VALUE_SIZE;

    bench_data_t data;
    if (prepare_bench_data(&data, keyspace, lookup_requests, insert_requests, value_size) != 0) {
        fprintf(stderr, "failed to prepare bench data\n");
        destroy_bench_data(&data);
        return 1;
    }

    mem_set_backend(KVS_MEM_BACKEND_SYSTEM);
    srand(1);

    double existing_sec = benchmark_existing_lookup(&data);
    double missing_sec = benchmark_missing_lookup(&data);
    double insert_legacy_sec = benchmark_insert_legacy(&data);
    double insert_current_sec = benchmark_insert_current(&data);

    if (existing_sec < 0.0 || missing_sec < 0.0 || insert_legacy_sec < 0.0 || insert_current_sec < 0.0) {
        fprintf(stderr, "benchmark failed\n");
        destroy_bench_data(&data);
        return 1;
    }

    printf("[INFO] skiptable_targeted_bench keyspace=%zu lookup_requests=%zu insert_requests=%zu value_size=%zu\n",
           keyspace, lookup_requests, insert_requests, value_size);

    report_case("existing_lookup", lookup_requests, existing_sec);
    report_case("missing_lookup", lookup_requests, missing_sec);
    report_case("insert_heavy_legacy", insert_requests, insert_legacy_sec);
    report_case("insert_heavy_current", insert_requests, insert_current_sec);
    report_delta("insert_heavy", (insert_legacy_sec * 1000000000.0) / (double)insert_requests,
                 (insert_current_sec * 1000000000.0) / (double)insert_requests);

    printf("[CHECKSUM] %lu\n", g_checksum);

    destroy_bench_data(&data);
    return 0;
}