#include "kvs_api.h"
#include "kvs_globals.h"
#include "distributed.h"

#include <sys/time.h>
#include <string.h>
#include <time.h>

typedef struct ttl_scan_cursor {
    int array_idx;
    int hash_idx;
    int64_t now_ms;
    int budget;
    char *expired_keys[32];
    int expired_count;
} ttl_scan_cursor_t;

static pthread_t g_ttl_scan_thread;
static volatile int g_ttl_scan_running = 0;
static volatile int g_ttl_scan_stop = 0;

static int64_t ttl_now_ms(void) {
    struct timeval tv;
    gettimeofday(&tv, NULL);
    return (int64_t)tv.tv_sec * 1000 + (int64_t)tv.tv_usec / 1000;
}

static void ttl_scan_sync_delete_key(const char *key) {
    if (!key) return;
    if (global_dist_config.role != ROLE_MASTER) return;
    if (g_cfg.sync_mode != KVS_SYNC_TCP) return;
    master_sync_command("DEL", key, NULL);
}

static int ttl_scan_delete_key(const char *key) {
    if (!key) return 0;

#if ENABLE_ARRAY
    if (kvs_array_del(kvs_get_global_array(), (char *)key) == 0) return 1;
#endif
#if ENABLE_RBTREE
    if (kvs_rbtree_del(kvs_get_global_rbtree(), (char *)key) == 0) return 1;
#endif
#if ENABLE_HASH
    if (kvs_hash_del(kvs_get_global_hash(), (char *)key) == 0) return 1;
#endif
#if ENABLE_SKIPTABLE
    if (kvs_skiptable_del(kvs_get_global_skiptable(), (char *)key) == 0) return 1;
#endif

    return 0;
}

static void ttl_scan_free_key(char *key) {
    if (key) {
        kvs_free(key);
    }
}

static void ttl_scan_record_expired(ttl_scan_cursor_t *cursor, const char *key) {
    if (!cursor || !key || cursor->expired_count >= (int)(sizeof(cursor->expired_keys) / sizeof(cursor->expired_keys[0]))) {
        return;
    }

    size_t key_len = strlen(key);
    char *dup_key = kvs_malloc(key_len + 1);
    if (!dup_key) {
        return;
    }

    memcpy(dup_key, key, key_len + 1);
    cursor->expired_keys[cursor->expired_count++] = dup_key;
}

static void ttl_scan_flush_expired(ttl_scan_cursor_t *cursor) {
    if (!cursor) return;
    for (int i = 0; i < cursor->expired_count; ++i) {
        char *key = cursor->expired_keys[i];
        if (ttl_scan_delete_key(key)) {
            ttl_scan_sync_delete_key(key);
        }
        ttl_scan_free_key(key);
    }
    cursor->expired_count = 0;
}

#if ENABLE_ARRAY
static void ttl_scan_array(kvs_array_t *inst, ttl_scan_cursor_t *cursor) {
    if (!inst || !inst->table || cursor->budget <= 0) return;

    int len = inst->len;
    if (len <= 0) return;

    int visited = 0;
    for (int i = cursor->array_idx; i < len && visited < cursor->budget; ++i) {
        kvs_array_item_t *item = &inst->table[i];
        cursor->array_idx = i + 1;
        if (!item->key) continue;
        visited++;
        if (!kv_is_expired(item->expire_at_ms, cursor->now_ms)) continue;
        ttl_scan_record_expired(cursor, item->key);
    }
    cursor->budget -= visited;
}
#endif

#if ENABLE_HASH
static void ttl_scan_hash(kvs_hash_t *inst, ttl_scan_cursor_t *cursor) {
    if (!inst || !inst->nodes || cursor->budget <= 0) return;

    int visited = 0;
    for (int i = cursor->hash_idx; i < inst->max_slots && visited < cursor->budget; ++i) {
        hashnode_t *slot = &inst->nodes[i];
        cursor->hash_idx = i + 1;
        if (slot->used != 1 || !slot->key) continue;
        visited++;
        if (!kv_is_expired(slot->expire_at_ms, cursor->now_ms)) continue;
        ttl_scan_record_expired(cursor, slot->key);
    }
    cursor->budget -= visited;
}
#endif

#if ENABLE_RBTREE
typedef struct ttl_rbtree_scan_ctx {
    ttl_scan_cursor_t *cursor;
    int visited;
} ttl_rbtree_scan_ctx_t;

static void ttl_scan_rbtree_cb(rbtree_node *node, void *userdata) {
    ttl_rbtree_scan_ctx_t *ctx = (ttl_rbtree_scan_ctx_t *)userdata;
    if (!ctx || !ctx->cursor || ctx->cursor->budget <= 0 || !node || !node->key) return;
    ctx->visited++;
    ctx->cursor->budget--;
    if (kv_is_expired(node->expire_at_ms, ctx->cursor->now_ms)) {
        ttl_scan_record_expired(ctx->cursor, node->key);
    }
}

static void ttl_scan_rbtree(kvs_rbtree_t *inst, ttl_scan_cursor_t *cursor) {
    if (!inst || !inst->root || !inst->nil || cursor->budget <= 0) return;
    ttl_rbtree_scan_ctx_t ctx = { .cursor = cursor, .visited = 0 };
    rbtree_traversal_with_callback((rbtree *)inst, inst->root, ttl_scan_rbtree_cb, &ctx);
}
#endif

#if ENABLE_SKIPTABLE
static void ttl_scan_skiptable(kvs_skiptable_t *table, ttl_scan_cursor_t *cursor) {
    if (!table || !table->header || cursor->budget <= 0) return;

    Node *current = table->header->forward[0];
    int visited = 0;
    while (current && visited < cursor->budget) {
        Node *next = current->forward[0];
        visited++;
        if (current->key && kv_is_expired(current->expire_at_ms, cursor->now_ms)) {
            ttl_scan_record_expired(cursor, current->key);
        }
        current = next;
    }
    cursor->budget -= visited;
}
#endif

static void ttl_scan_once(void) {
    ttl_scan_cursor_t cursor = {
        .array_idx = 0,
        .hash_idx = 0,
        .now_ms = ttl_now_ms(),
        .budget = 32,
    };

    if (pthread_rwlock_wrlock(&g_kv_data_rwlock) != 0) {
        return;
    }

#if ENABLE_ARRAY
    ttl_scan_array(kvs_get_global_array(), &cursor);
#endif
#if ENABLE_HASH
    ttl_scan_hash(kvs_get_global_hash(), &cursor);
#endif
#if ENABLE_RBTREE
    ttl_scan_rbtree(kvs_get_global_rbtree(), &cursor);
#endif
#if ENABLE_SKIPTABLE
    ttl_scan_skiptable(kvs_get_global_skiptable(), &cursor);
#endif

    ttl_scan_flush_expired(&cursor);

    pthread_rwlock_unlock(&g_kv_data_rwlock);
}

static void *ttl_scan_thread_main(void *arg) {
    (void)arg;
    while (!g_ttl_scan_stop) {
        struct timespec ts = { .tv_sec = 1, .tv_nsec = 0 };
        nanosleep(&ts, NULL);

        if (g_ttl_scan_stop) {
            break;
        }
        if (global_dist_config.role == ROLE_SLAVE) {
            continue;
        }
        ttl_scan_once();
    }
    return NULL;
}

int kvs_ttl_background_start(void) {
    if (g_ttl_scan_running) {
        return 0;
    }
    g_ttl_scan_stop = 0;
    if (pthread_create(&g_ttl_scan_thread, NULL, ttl_scan_thread_main, NULL) != 0) {
        return -1;
    }
    g_ttl_scan_running = 1;
    return 0;
}

void kvs_ttl_background_stop(void) {
    if (!g_ttl_scan_running) {
        return;
    }
    g_ttl_scan_stop = 1;
    pthread_join(g_ttl_scan_thread, NULL);
    g_ttl_scan_running = 0;
}