#include "kvs_api.h"
#include "kvs_globals.h"
#include "resp_protocol.h"
#include "distributed.h"
#include "kvstore.h"
#include "kvs_shutdown.h"
#include "kvs_vector_distance.h"
#include "kvs_embedding_worker_pool.h"
#include "kvs_command_executor.h"
#include <string.h>
#include <strings.h>
#include <stdlib.h>
#include <stdio.h>
#include <limits.h>
#include <errno.h>
#include <time.h>

extern aof_state_t aof_state;
extern distributed_config_t global_dist_config;
extern rbtree_node *rbtree_search(rbtree *T, KEY_TYPE key);
extern rbtree_node *rbtree_delete(rbtree *T, rbtree_node *z);

//四个数据结构
extern kvs_array_t global_array;
extern kvs_rbtree_t global_rbtree;
extern kvs_hash_t global_hash;
extern kvs_skiptable_t global_skiptable;

static uint64_t kvs_operation_now_us(void) {
    struct timespec ts;

    if (clock_gettime(CLOCK_MONOTONIC, &ts) != 0) {
        return 0;
    }
    return (uint64_t)ts.tv_sec * 1000000ull + (uint64_t)ts.tv_nsec / 1000ull;
}

typedef enum {
    TTL_TARGET_NONE = 0,
    TTL_TARGET_ARRAY,
    TTL_TARGET_RBTREE,
    TTL_TARGET_HASH,
    TTL_TARGET_SKIPTABLE,
} ttl_target_kind_t;

typedef struct {
    ttl_target_kind_t kind;
    void *container;
    void *entry;
    Node *skiptable_update[MAX_LEVEL + 1];
} ttl_target_ref_t;

static int64_t ttl_target_get_expire_at(const ttl_target_ref_t *ref);
static void ttl_target_set_expire_at(const ttl_target_ref_t *ref, int64_t expire_at_ms);
static int ttl_target_delete(const ttl_target_ref_t *ref);
static int ttl_target_apply_expired_side_effects(const ttl_target_ref_t *ref, const char *key, size_t key_len);

#if ENABLE_RBTREE
typedef struct {
    char *resp;
    int max_len;
    int *off;
    int overflow;
    int64_t now_ms;
} rbtree_dump_ctx_t;

static void rbtree_dump_cb(rbtree_node *node, void *ud);
#endif

static int ttl_skiptable_key_compare(const Node *node, const char *key, size_t key_len) {
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

    if (node_key_len < key_len) return -1;
    if (node_key_len > key_len) return 1;
    return 0;
}

static int ttl_lookup_array(kvs_array_t *inst, const char *key, size_t key_len, ttl_target_ref_t *ref) {
    if (!inst || !key || !ref || !inst->table) return 0;

    int idx = -1;
    if (kvs_array_lookup_index_len(inst, key, key_len, &idx) == 0 && idx >= 0 && idx < inst->len) {
        ref->kind = TTL_TARGET_ARRAY;
        ref->container = inst;
        ref->entry = &inst->table[idx];
        return 1;
    }

    return 0;
}

static int ttl_lookup_rbtree(kvs_rbtree_t *inst, const char *key, size_t key_len, ttl_target_ref_t *ref) {
    if (!inst || !key || !ref || !inst->root || !inst->nil) return 0;

    rbtree_node *node = kvs_rbtree_lookup_node(inst, key, key_len);
    if (node && node != inst->nil) {
        ref->kind = TTL_TARGET_RBTREE;
        ref->container = inst;
        ref->entry = node;
        return 1;
    }

    return 0;
}

static int ttl_lookup_hash(kvs_hash_t *inst, const char *key, size_t key_len, ttl_target_ref_t *ref) {
    if (!inst || !key || !ref || !inst->nodes || inst->max_slots <= 0) return 0;

    int idx = -1;
    if (kvs_hash_lookup_index_len(inst, key, key_len, &idx) == 0 && idx >= 0 && idx < inst->max_slots) {
        ref->kind = TTL_TARGET_HASH;
        ref->container = inst;
        ref->entry = &inst->nodes[idx];
        return 1;
    }

    return 0;
}

static int ttl_lookup_skiptable(kvs_skiptable_t *table, const char *key, size_t key_len, ttl_target_ref_t *ref) {
    if (!table || !key || !ref || !table->header) return 0;

    Node *current = table->header;
    for (int level = table->level; level >= 0; --level) {
        while (current->forward[level]) {
            int cmp = ttl_skiptable_key_compare(current->forward[level], key, key_len);
            if (cmp < 0) {
                current = current->forward[level];
                continue;
            }
            break;
        }
        ref->skiptable_update[level] = current;
    }

    current = current->forward[0];
    if (current) {
        if (ttl_skiptable_key_compare(current, key, key_len) != 0) {
            return 0;
        }
        ref->kind = TTL_TARGET_SKIPTABLE;
        ref->container = table;
        ref->entry = current;
        return 1;
    }

    return 0;
}

static int ttl_lookup_by_structure(int data_structure, const char *key, size_t key_len, ttl_target_ref_t *ref) {
    switch (data_structure) {
#if ENABLE_ARRAY
        case DATA_STRUCTURE_ARRAY:
            return ttl_lookup_array(kvs_get_global_array(), key, key_len, ref);
#endif
#if ENABLE_RBTREE
        case DATA_STRUCTURE_RBTREE:
            return ttl_lookup_rbtree(kvs_get_global_rbtree(), key, key_len, ref);
#endif
#if ENABLE_HASH
        case DATA_STRUCTURE_HASH:
            return ttl_lookup_hash(kvs_get_global_hash(), key, key_len, ref);
#endif
#if ENABLE_SKIPTABLE
        case DATA_STRUCTURE_SKIPTABLE:
            return ttl_lookup_skiptable(kvs_get_global_skiptable(), key, key_len, ref);
#endif
        default:
            return 0;
    }
}

static int ttl_lookup_any(const char *key, size_t key_len, ttl_target_ref_t *ref) {
#if ENABLE_ARRAY
    if (ttl_lookup_array(kvs_get_global_array(), key, key_len, ref)) return 1;
#endif
#if ENABLE_RBTREE
    if (ttl_lookup_rbtree(kvs_get_global_rbtree(), key, key_len, ref)) return 1;
#endif
#if ENABLE_HASH
    if (ttl_lookup_hash(kvs_get_global_hash(), key, key_len, ref)) return 1;
#endif
#if ENABLE_SKIPTABLE
    if (ttl_lookup_skiptable(kvs_get_global_skiptable(), key, key_len, ref)) return 1;
#endif
    return 0;
}

static void ttl_sync_delete_if_needed(const char *key, size_t key_len) {
    if (!key) return;
    if (global_dist_config.role != ROLE_MASTER) return;
    if (g_cfg.sync_mode != KVS_SYNC_TCP) return;
    if (get_sync_context() == 2) return;
    master_sync_command("DEL", key, key_len, NULL, 0);
}

int ttl_get_any_key_expire_at_ms(const char *key, size_t key_len, int64_t *expire_at_ms) {
    if (!key || !expire_at_ms) {
        return -1;
    }

    ttl_target_ref_t ref;
    memset(&ref, 0, sizeof(ref));
    if (!ttl_lookup_any(key, key_len, &ref)) {
        return 0;
    }

    *expire_at_ms = ttl_target_get_expire_at(&ref);
    return 1;
}

int ttl_set_any_key_expire_at_ms(const char *key, size_t key_len, int64_t expire_at_ms) {
    if (!key) {
        return -1;
    }

    ttl_target_ref_t ref;
    memset(&ref, 0, sizeof(ref));
    if (!ttl_lookup_any(key, key_len, &ref)) {
        return 0;
    }

    if (expire_at_ms < 0) {
        ttl_target_set_expire_at(&ref, KV_TTL_NO_EXPIRE);
        return 1;
    }

    int64_t now_ms = kv_now_ms();
    if (kv_is_expired(expire_at_ms, now_ms)) {
        return ttl_target_delete(&ref) == 0 ? 1 : 0;
    }

    ttl_target_set_expire_at(&ref, expire_at_ms);
    return 1;
}

int ttl_expire_observed_key(const char *key, size_t key_len, int64_t observed_expire_at_ms) {
    ttl_target_ref_t ref;
    int64_t current_expire_at_ms;
    int64_t now_ms;

    if (!key || key_len == 0 || observed_expire_at_ms < 0) {
        return -1;
    }

    memset(&ref, 0, sizeof(ref));
    if (!ttl_lookup_any(key, key_len, &ref)) {
        return 0;
    }

    current_expire_at_ms = ttl_target_get_expire_at(&ref);
    if (current_expire_at_ms < 0) {
        return 0;
    }

    /*
     * Stage 10.1: an active-expire scanner may have observed an old TTL.
     * If the key was SET/EXPIRE/PERSIST after that observation, do not delete
     * the current value.
     */
    if (current_expire_at_ms != observed_expire_at_ms) {
        return 0;
    }

    now_ms = kv_now_ms();
    if (!kv_is_expired(current_expire_at_ms, now_ms)) {
        return 0;
    }

    if (ttl_target_apply_expired_side_effects(&ref, key, key_len) != 0) {
        return -1;
    }

    return 1;
}

int handle_aofrewrite(command_context_t *ctx, char *response){
    (void)ctx;
    aof_rewrite_background();
    return resp_generate_simple_string_n(response, '+', "OK", sizeof("OK") - 1);
}

static int64_t ttl_target_get_expire_at(const ttl_target_ref_t *ref) {
    if (!ref) return KV_TTL_NO_EXPIRE;

    switch (ref->kind) {
        case TTL_TARGET_ARRAY:
            return ((kvs_array_item_t *)ref->entry)->expire_at_ms;
        case TTL_TARGET_RBTREE:
            return ((rbtree_node *)ref->entry)->expire_at_ms;
        case TTL_TARGET_HASH:
            return ((hashnode_t *)ref->entry)->expire_at_ms;
        case TTL_TARGET_SKIPTABLE:
            return ((Node *)ref->entry)->expire_at_ms;
        default:
            return KV_TTL_NO_EXPIRE;
    }
}

static void *ttl_target_get_value(const ttl_target_ref_t *ref) {
    if (!ref) return NULL;

    switch (ref->kind) {
        case TTL_TARGET_ARRAY:
            return ((kvs_array_item_t *)ref->entry)->value;
        case TTL_TARGET_RBTREE:
            return ((rbtree_node *)ref->entry)->value;
        case TTL_TARGET_HASH:
            return ((hashnode_t *)ref->entry)->value;
        case TTL_TARGET_SKIPTABLE:
            return ((Node *)ref->entry)->value;
        default:
            return NULL;
    }
}

static size_t ttl_target_get_value_len(const ttl_target_ref_t *ref) {
    if (!ref) return 0;

    switch (ref->kind) {
        case TTL_TARGET_ARRAY:
            return ((kvs_array_item_t *)ref->entry)->value_len;
        case TTL_TARGET_RBTREE:
            return ((rbtree_node *)ref->entry)->value_len;
        case TTL_TARGET_HASH:
            return ((hashnode_t *)ref->entry)->value_len;
        case TTL_TARGET_SKIPTABLE:
            return ((Node *)ref->entry)->value_len;
        default:
            return 0;
    }
}

static void ttl_target_set_expire_at(const ttl_target_ref_t *ref, int64_t expire_at_ms) {
    if (!ref) return;

    switch (ref->kind) {
        case TTL_TARGET_ARRAY:
            ((kvs_array_item_t *)ref->entry)->expire_at_ms = expire_at_ms;
            break;
        case TTL_TARGET_RBTREE:
            ((rbtree_node *)ref->entry)->expire_at_ms = expire_at_ms;
            break;
        case TTL_TARGET_HASH:
            ((hashnode_t *)ref->entry)->expire_at_ms = expire_at_ms;
            break;
        case TTL_TARGET_SKIPTABLE:
            ((Node *)ref->entry)->expire_at_ms = expire_at_ms;
            break;
        default:
            break;
    }
}

static int ttl_target_delete(const ttl_target_ref_t *ref) {
    if (!ref) return -1;

    switch (ref->kind) {
        case TTL_TARGET_ARRAY:
        {
            kvs_array_t *inst = (kvs_array_t *)ref->container;
            kvs_array_item_t *item = (kvs_array_item_t *)ref->entry;
            if (!inst || !item || !inst->table) return -1;
            int idx = (int)(item - inst->table);
            return kvs_array_del_at(inst, idx);
        }
        case TTL_TARGET_RBTREE:
        {
            kvs_rbtree_t *inst = (kvs_rbtree_t *)ref->container;
            rbtree_node *node = (rbtree_node *)ref->entry;
            if (!inst || !node || node == inst->nil) return -1;
            (void)rbtree_delete(inst, node);
            return 0;
        }
        case TTL_TARGET_HASH:
        {
            kvs_hash_t *hash = (kvs_hash_t *)ref->container;
            hashnode_t *slot = (hashnode_t *)ref->entry;
            if (!hash || !slot || !hash->nodes) return -1;
            int idx = (int)(slot - hash->nodes);
            return kvs_hash_del_at(hash, idx);
        }
        case TTL_TARGET_SKIPTABLE:
        {
            kvs_skiptable_t *table = (kvs_skiptable_t *)ref->container;
            Node *current = (Node *)ref->entry;
            if (!table || !table->header || !current) return -1;

            for (int i = 0; i <= table->level; ++i) {
                Node *prev = ref->skiptable_update[i];
                if (!prev || prev->forward[i] != current) {
                    break;
                }
                prev->forward[i] = current->forward[i];
            }

            table->count--;

            if (current->key) kvs_free(current->key);
            if (current->value) kvs_free(current->value);
            kvs_free(current);

            while (table->level > 0 && table->header->forward[table->level] == NULL) {
                table->level--;
            }
            return 0;
        }
        default:
            return -1;
    }
}

static int ttl_target_is_expired(const ttl_target_ref_t *ref, int64_t now_ms) {
    if (!ref) return 0;

    int64_t expire_at_ms = ttl_target_get_expire_at(ref);
    if (expire_at_ms < 0) {
        return 0;
    }

    return kv_is_expired(expire_at_ms, now_ms) ? 1 : 0;
}

static int ttl_target_apply_expired_side_effects(const ttl_target_ref_t *ref, const char *key, size_t key_len) {
    if (!ref || !key) return -1;

    if (global_dist_config.role != ROLE_MASTER) {
        return 0;
    }

    if (ttl_target_delete(ref) != 0) {
        return -1;
    }

    ttl_sync_delete_if_needed(key, key_len);
    return 0;
}

static int ttl_expire_if_needed_by_structure(int data_structure, const char *key, size_t key_len, ttl_target_ref_t *ref_out, int apply_expired_side_effects) {
    ttl_target_ref_t ref;
    memset(&ref, 0, sizeof(ref));

    if (!ttl_lookup_by_structure(data_structure, key, key_len, &ref)) {
        if (ref_out) {
            memset(ref_out, 0, sizeof(*ref_out));
        }
        return 0;
    }

    if (ref_out) {
        *ref_out = ref;
    }

    int64_t now_ms = kv_now_ms();
    if (!ttl_target_is_expired(&ref, now_ms)) {
        return 0;
    }

    if (!apply_expired_side_effects) {
        return 1;
    }

    if (ttl_target_apply_expired_side_effects(&ref, key, key_len) != 0) {
        return -1;
    }

    return 1;
}

static int ttl_apply_any_key_op(command_context_t *ctx, int op) {
    ttl_target_ref_t ref;
    memset(&ref, 0, sizeof(ref));

    if (!ctx || !ctx->key) {
        return -1;
    }

    if (!ttl_lookup_any(ctx->key, ctx->key_len, &ref)) {
        return (op == KVS_CMD_TTL) ? -2 : 0;
    }

    int64_t now_ms = kv_now_ms();

    if (ttl_target_is_expired(&ref, now_ms)) {
        if (ttl_target_apply_expired_side_effects(&ref, ctx->key, ctx->key_len) != 0) {
            return -1;
        }
        return (op == KVS_CMD_TTL) ? -2 : 0;
    }

    int64_t expire_at_ms = ttl_target_get_expire_at(&ref);

    switch (op) {
        case KVS_CMD_EXPIRE:
        {
            if (!ctx->value) return -1;

            char *endptr = NULL;
            errno = 0;
            long seconds = strtol(ctx->value, &endptr, 10);
            if (endptr == ctx->value || *endptr != '\0' || errno == ERANGE) {
                return -1;
            }

            if (seconds <= 0) {
                return ttl_target_delete(&ref) == 0 ? 1 : 0;
            }

            int64_t delta_ms = (int64_t)seconds * 1000;
            if (now_ms > INT64_MAX - delta_ms) {
                ttl_target_set_expire_at(&ref, INT64_MAX);
            } else {
                ttl_target_set_expire_at(&ref, now_ms + delta_ms);
            }
            return 1;
        }
        case KVS_CMD_PEXPIREAT:
        {
            if (!ctx->value) return -1;

            char *endptr = NULL;
            errno = 0;
            long long expire_at_input = strtoll(ctx->value, &endptr, 10);
            if (endptr == ctx->value || *endptr != '\0' || errno == ERANGE) {
                return -1;
            }

            int64_t expire_at_input_ms = (int64_t)expire_at_input;
            if (expire_at_input_ms <= now_ms) {
                return ttl_target_delete(&ref) == 0 ? 1 : 0;
            }

            ttl_target_set_expire_at(&ref, expire_at_input_ms);
            return 1;
        }
        case KVS_CMD_TTL:
            if (expire_at_ms < 0) {
                return -1;
            }
            return (int)kv_ttl_remaining_seconds(expire_at_ms, now_ms);
        case KVS_CMD_PERSIST:
            if (expire_at_ms < 0) {
                return 0;
            }
            ttl_target_set_expire_at(&ref, KV_TTL_NO_EXPIRE);
            return 1;
        default:
            return -1;
    }
}
const char *command[] = {
    "SET", "GET", "DEL", "MOD", "EXIST","RANGE","SORT",
    "RSET", "RGET", "RDEL", "RMOD", "REXIST","RRANGE","RSORT",
    "HSET", "HGET", "HDEL", "HMOD", "HEXIST","HRANGE","HSORT",
    "SSET", "SGET", "SDEL", "SMOD", "SEXIST","SRANGE","SSORT",
    "PAPERSET", "PAPERGET", "PAPERDEL", "PAPERMOD",
    "EXPIRE", "TTL", "PERSIST",
    "PEXPIREAT",
    "AOFREWRITE",
    "QUIT",
    "FULLDUMP",
    "VSET", "VGET", "VSEARCH", "VDEL", "VINDEX", "VINFO"
};

const command_info command_table[] = {
    //数组
    {"SET", handle_set,  3, 1, DATA_STRUCTURE_ARRAY, KVS_CMD_LOCK_WRITE},
    {"GET", handle_get,  2, 0, DATA_STRUCTURE_ARRAY, KVS_CMD_LOCK_READ},
    {"DEL", handle_del,  2, 1, DATA_STRUCTURE_ARRAY, KVS_CMD_LOCK_WRITE},
    {"MOD", handle_mod,  3, 1, DATA_STRUCTURE_ARRAY, KVS_CMD_LOCK_WRITE},
    {"EXIST",  handle_exist,  2, 0, DATA_STRUCTURE_ARRAY, KVS_CMD_LOCK_READ},
    {"RANGE",  handle_range,  3, 0, DATA_STRUCTURE_ARRAY, KVS_CMD_LOCK_WRITE},
    {"SORT",   handle_sort,  -1, 0, DATA_STRUCTURE_ARRAY, KVS_CMD_LOCK_WRITE}, // 至少需要2个参数

    // RBTree commands
    {"RSET",   handle_set,    3, 1, DATA_STRUCTURE_RBTREE, KVS_CMD_LOCK_WRITE},
    {"RGET",   handle_get,    2, 0, DATA_STRUCTURE_RBTREE, KVS_CMD_LOCK_READ},
    {"RDEL",   handle_del,    2, 1, DATA_STRUCTURE_RBTREE, KVS_CMD_LOCK_WRITE},
    {"RMOD",   handle_mod,    3, 1, DATA_STRUCTURE_RBTREE, KVS_CMD_LOCK_WRITE},
    {"REXIST", handle_exist,  2, 0, DATA_STRUCTURE_RBTREE, KVS_CMD_LOCK_READ},
    {"RRANGE", handle_range, -1, 0, DATA_STRUCTURE_RBTREE, KVS_CMD_LOCK_WRITE},
    {"RSORT",  handle_sort,  -1, 0, DATA_STRUCTURE_RBTREE, KVS_CMD_LOCK_WRITE},

    // Hash commands）
    {"HSET",   handle_set,    3, 1, DATA_STRUCTURE_HASH, KVS_CMD_LOCK_WRITE},
    {"HGET",   handle_get,    2, 0, DATA_STRUCTURE_HASH, KVS_CMD_LOCK_READ},
    {"HDEL",   handle_del,    2, 1, DATA_STRUCTURE_HASH, KVS_CMD_LOCK_WRITE},
    {"HMOD",   handle_mod,    3, 1, DATA_STRUCTURE_HASH, KVS_CMD_LOCK_WRITE},
    {"HEXIST", handle_exist,  2, 0, DATA_STRUCTURE_HASH, KVS_CMD_LOCK_READ},
    {"HRANGE", handle_range,  3, 0, DATA_STRUCTURE_HASH, KVS_CMD_LOCK_WRITE},
    {"HSORT",  handle_sort,  -1, 0, DATA_STRUCTURE_HASH, KVS_CMD_LOCK_WRITE},

    // SkipTable commands
    {"SSET",   handle_set,    3, 1, DATA_STRUCTURE_SKIPTABLE, KVS_CMD_LOCK_WRITE},
    {"SGET",   handle_get,    2, 0, DATA_STRUCTURE_SKIPTABLE, KVS_CMD_LOCK_READ},
    {"SDEL",   handle_del,    2, 1, DATA_STRUCTURE_SKIPTABLE, KVS_CMD_LOCK_WRITE},
    {"SMOD",   handle_mod,    3, 1, DATA_STRUCTURE_SKIPTABLE, KVS_CMD_LOCK_WRITE},
    {"SEXIST", handle_exist,  2, 0, DATA_STRUCTURE_SKIPTABLE, KVS_CMD_LOCK_READ},
    {"SRANGE", handle_range, -1, 0, DATA_STRUCTURE_SKIPTABLE, KVS_CMD_LOCK_WRITE},
    {"SSORT",  handle_sort,  -1, 0, DATA_STRUCTURE_SKIPTABLE, KVS_CMD_LOCK_WRITE},

    // Paper commands (use RBTree for large values)
    {"PAPERSET", handle_set,  3, 1, DATA_STRUCTURE_RBTREE, KVS_CMD_LOCK_WRITE},
    {"PAPERGET", handle_get,  2, 0, DATA_STRUCTURE_RBTREE, KVS_CMD_LOCK_READ},
    {"PAPERDEL", handle_del,  2, 1, DATA_STRUCTURE_RBTREE, KVS_CMD_LOCK_WRITE},
    {"PAPERMOD", handle_mod,  3, 1, DATA_STRUCTURE_RBTREE, KVS_CMD_LOCK_WRITE},

    // TTL command
    {"EXPIRE", handle_expire,  3, 1, -1, KVS_CMD_LOCK_WRITE},
    {"TTL",    handle_ttl,     2, 0, -1, KVS_CMD_LOCK_WRITE},
    {"PERSIST", handle_persist, 2, 1, -1, KVS_CMD_LOCK_WRITE},
    {"PEXPIREAT", handle_pexpireat, 3, 1, -1, KVS_CMD_LOCK_WRITE},
    {"AOFREWRITE", handle_aofrewrite, 1, 0, -1, KVS_CMD_LOCK_NONE},

    // Special commands
    {"QUIT",   handle_quit,   1, 0, -1, KVS_CMD_LOCK_NONE},
    {"FULLDUMP", handle_fulldump, 1, 0, -1, KVS_CMD_LOCK_WRITE},

    //V族命令
    {"VSET",    handle_vset_command,    -1, 1, -1, KVS_CMD_LOCK_WRITE},
    {"VGET",    handle_vget_command,    -1, 0, -1, KVS_CMD_LOCK_READ},
    {"VSEARCH", handle_vsearch_command, -1, 0, -1, KVS_CMD_LOCK_READ},
    {"VDEL",    handle_vdel_command,    -1, 1, -1, KVS_CMD_LOCK_WRITE},
    {"VINDEX",  handle_vindex_command,  -1, 0, -1, KVS_CMD_LOCK_WRITE},
    {"VINFO",   handle_vinfo_command,   -1, 0, -1, KVS_CMD_LOCK_READ},

    {NULL, NULL, 0, 0, -1, KVS_CMD_LOCK_NONE} // 结束标记
};

//获取命令索引
int get_command_index(const char *name) {
    for (int i = 0; command_table[i].name != NULL; i++) {
        if (strcmp(command_table[i].name, name) == 0) {
            return i;
        }
    }
    return -1;
}

//多行响应转换
int convert_multiline_to_resp_array(char *multiline_response, char *response, int max_len){
    // 统一限制最大 RESP 长度
    if (max_len > (int)KVS_MAX_RESPONSE_LENGTH) {
        max_len = (int)KVS_MAX_RESPONSE_LENGTH;
    }

    //将多行响应转换为RESP数组，使用调用方提供的缓冲区并做长度检查
    char *response_copy = strdup(multiline_response);
    if (!response_copy) return -1;

    int line_count = 0;
    int pos = 0;

    char *line = strtok(response_copy, "\r\n");

    //计算行数
    while(line != NULL){
        line_count++;
        line = strtok(NULL, "\r\n");
    }

    free(response_copy);
    response_copy = strdup(multiline_response);
    if (!response_copy) return -1;

    //生成数组头部
    int n = snprintf(response + pos, max_len - pos, "*%d\r\n", line_count);
    if (n < 0 || n >= max_len - pos) {
        free(response_copy);
        return -1;
    }
    pos += n;

    line = strtok(response_copy, "\r\n");
    for(int i = 0;i < line_count; i++){
        if (!line) break;
        int need = snprintf(response + pos, max_len - pos, "$%zu\r\n%s\r\n", strlen(line), line);
        if (need < 0 || need >= max_len - pos) {
            free(response_copy);
            return -1;
        }
        pos += need;
        line = strtok(NULL, "\r\n");
    }

    free(response_copy);
    return pos;

}

/**
 * V族命令 解析辅助函数
 */
static size_t kvs_vcmd_token_len(const char *s){
    if(!s) return 0;
    return strlen(s);
}

static int kvs_vcmd_parse_positive_int(const char *s, int *out){
    char *endptr = NULL;
    long value;

    if (!s || !*s || !out) {
        return -1;
    }

    errno = 0;
    value = strtol(s, &endptr, 10);
    if (endptr == s || *endptr != '\0' || errno == ERANGE) {
        return -1;
    }

    if (value <= 0 || value > 1024) {
        return -1;
    }

    *out = (int)value;
    return 0;
}

static int kvs_vcmd_parse_float_arg(const char *s, float *out) {
    char *endptr = NULL;
    float value;

    if (!s || !*s || !out) {
        return -1;
    }

    errno = 0;
    value = strtof(s, &endptr);
    if (endptr == s || *endptr != '\0' || errno == ERANGE) {
        return -1;
    }

    *out = value;
    return 0;
}

static int kvs_vcmd_is_rebuild(const char *s) {
    if (!s) {
        return 0;
    }

    return strcasecmp(s, "REBUILD") == 0;
}

//解析命令参数到上下文
int parse_command_argument(int cmd, char **tokens, int count, size_t *token_lens, command_context_t *ctx){
    memset(ctx, 0, sizeof(command_context_t));
    ctx->limit = -1;
    ctx->lock_mode = KVS_LOCK_MODE_WRITE;
    ctx->scope = NULL;
    ctx->scope_len = 0;
    ctx->subcommand = NULL;
    ctx->subcommand_len = 0;
    ctx->topk = 1;
    ctx->threshold = 0.0f;
    ctx->has_threshold = 0;
    ctx->query_text = NULL;
    ctx->query_text_len = 0;

    //设置数据结构类型
    if(cmd >= KVS_CMD_SET && cmd <= KVS_CMD_SORT ){
        ctx->data_structure = DATA_STRUCTURE_ARRAY;
    }else if(cmd >= KVS_CMD_RSET && cmd <= KVS_CMD_RSORT){
        ctx->data_structure = DATA_STRUCTURE_RBTREE;
    }else if(cmd >= KVS_CMD_HSET && cmd <= KVS_CMD_HSORT){
        ctx->data_structure = DATA_STRUCTURE_HASH;
    }else if(cmd >= KVS_CMD_SSET && cmd <= KVS_CMD_SSORT){
        ctx->data_structure = DATA_STRUCTURE_SKIPTABLE;
    }else if(cmd >= KVS_CMD_PAPERSET && cmd <= KVS_CMD_PAPERMOD){
        ctx->data_structure = DATA_STRUCTURE_RBTREE; // Paper 使用 RBTree 存储大 value
    }else if(cmd == KVS_CMD_EXPIRE || cmd == KVS_CMD_TTL || cmd == KVS_CMD_PERSIST || cmd == KVS_CMD_PEXPIREAT || cmd == KVS_CMD_AOFREWRITE){
        ctx->data_structure = -1;
    }else if(cmd == KVS_CMD_QUIT){
        //QUIT命令不需要额外参数
        return 0;
    }else if(cmd == KVS_CMD_FULLDUMP){
        //FULLDUMP命令不需要额外参数
        return 0;
    }else if(cmd >= KVS_CMD_VSET && cmd <= KVS_CMD_VINFO){
        /**
         * V族命令 使用自己的数据结构
         *
         * 最小协议：
        * VSET question answer [scope]
        * VGET question [scope]
        * VSEARCH query [scope] [topk] [threshold]
        * VDEL question [scope]
        * VINDEX REBUILD
        * VINFO
        */

        ctx->data_structure = -1; // V族命令使用特殊处理

        if(cmd == KVS_CMD_VSET){
            if(count != 3 && count != 4) return -1;

            ctx->key = tokens[1];
            ctx->key_len = token_lens[1];

            ctx->query_text = tokens[1];
            ctx->query_text_len = ctx->key_len;

            ctx->value = tokens[2];
            ctx->value_len = token_lens[2];

            if(count == 4){
                ctx->scope = tokens[3];
                ctx->scope_len = token_lens[3];
            }

            return 0;
        }
        if(cmd == KVS_CMD_VGET){
            if (count != 2 && count != 3) {
                return -1;
            }

            ctx->key = tokens[1];
            ctx->key_len = token_lens[1];

            ctx->query_text = tokens[1];
            ctx->query_text_len = ctx->key_len;

            if (count == 3) {
                ctx->scope = tokens[2];
                ctx->scope_len = token_lens[2];
            }

            return 0;
        }

        if (cmd == KVS_CMD_VSEARCH) {
            int parsed_topk = 0;
            float parsed_threshold = 0.0f;

            /*
            * 旧协议保持兼容：
            *   VSEARCH query
            *   VSEARCH query topk
            *   VSEARCH query scope
            *   VSEARCH query scope topk
            *   VSEARCH query scope topk threshold
            *

            */
            if (count < 2) {
                return -1;
            }

            /*
            * 下面是旧单条协议。
            */
            if (count > 5) {
                return -1;
            }

            ctx->key = tokens[1];
            ctx->key_len = token_lens[1];

            ctx->query_text = tokens[1];
            ctx->query_text_len = ctx->key_len;

            if (count == 3) {
                if (kvs_vcmd_parse_positive_int(tokens[2], &parsed_topk) == 0) {
                    ctx->topk = parsed_topk;
                } else {
                    ctx->scope = tokens[2];
                    ctx->scope_len = token_lens[2];
                }

                return 0;
            }

            if (count >= 4) {
                ctx->scope = tokens[2];
                ctx->scope_len = token_lens[2];

                if (kvs_vcmd_parse_positive_int(tokens[3], &parsed_topk) != 0) {
                    return -1;
                }

                ctx->topk = parsed_topk;
            }

            if (count >= 5) {
                if (kvs_vcmd_parse_float_arg(tokens[4], &parsed_threshold) != 0) {
                    return -1;
                }

                ctx->threshold = parsed_threshold;
                ctx->has_threshold = 1;
            }

            return 0;
        }

        if (cmd == KVS_CMD_VDEL) {
            if (count != 2 && count != 3) {
                return -1;
            }

            ctx->key = tokens[1];
            ctx->key_len = token_lens[1];

            ctx->query_text = tokens[1];
            ctx->query_text_len = ctx->key_len;

            if (count == 3) {
                ctx->scope = tokens[2];
                ctx->scope_len = token_lens[2];
            }
            return 0;
        }

        if (cmd == KVS_CMD_VINDEX) {
            if (count != 2) {
                return -1;
            }

            if (!kvs_vcmd_is_rebuild(tokens[1])) {
                return -1;
            }

            ctx->subcommand = tokens[1];
            ctx->subcommand_len = token_lens[1];

            return 0;
        }

        if (cmd == KVS_CMD_VINFO) {
            if (count != 1) {
                return -1;
            }

            return 0;
        }

        return -1;
    }else{
        return -1; //未知命令
    }

    //解析通用参数
    if (count > 1) {
        ctx->key = tokens[1];
        ctx->key_len = token_lens[1];
    }
    if (count > 2) {
        ctx->value = tokens[2];
        ctx->value_len = token_lens[2];
    }

    //解析特定命令的参数
    switch(cmd){
        case KVS_CMD_SORT:
        case KVS_CMD_RSORT:
        case KVS_CMD_HSORT:
        case KVS_CMD_SSORT:
            //设置默认值
            ctx->order = 0;
            ctx->limit = -1;

            //解析参数
            if (count >= 2) {
                if (strcmp(tokens[1], "high") == 0) {
                    ctx->order = 1; // 降序
                } else if (strcmp(tokens[1], "low") == 0) {
                    ctx->order = 0; // 升序
                } else {
                    // 如果不是 high/low，尝试解析为限制数量
                    ctx->limit = atoi(tokens[1]);
                    if (ctx->limit <= 0) ctx->limit = -1;
                }

                // 处理第二个参数
                if (count >= 3) {
                    ctx->limit = atoi(tokens[2]);
                    if (ctx->limit <= 0) ctx->limit = -1;
                }
            }
            break;

        case KVS_CMD_RANGE:
        case KVS_CMD_HRANGE:
            if (count >= 3) {
                ctx->start_key = tokens[1];
                ctx->end_key = tokens[2];
            }
            break;

        case KVS_CMD_RRANGE:
        case KVS_CMD_SRANGE:
            if (count != 3 && count != 5) {
                return -1;
            }

            ctx->start_key = tokens[1];
            ctx->end_key = tokens[2];

            if (count == 5) {
                char *endptr = NULL;
                long parsed_limit;

                if (strcasecmp(tokens[3], "LIMIT") != 0) {
                    return -1;
                }

                errno = 0;
                parsed_limit = strtol(tokens[4], &endptr, 10);
                if (endptr == tokens[4] || *endptr != '\0') {
                    return -1;
                }

                if (errno == ERANGE && parsed_limit == LONG_MAX) {
                    parsed_limit = INT_MAX;
                } else if (errno != 0) {
                    return -1;
                }

                if (parsed_limit <= 0) {
                    return -1;
                }

                if (parsed_limit > INT_MAX) {
                    parsed_limit = INT_MAX;
                }

                ctx->limit = (int)parsed_limit;
            }
            break;
    }

    return 0;
}

//查找命令
const command_info *lookup_command(const char *name){
    for(int i = 0; command_table[i].name != NULL;i++){
        if(strcmp(command_table[i].name, name) == 0){
            return &command_table[i];
        }
    }

    return NULL;
}

//检查参数
int check_arity(const command_info *cmd, int argc){
    /*
     * V 族命令参数数量校验。
     *
     * 注意：
     * argc 包含命令名本身。
     *
     * VSET question answer [scope]                 => 3 或 4
     * VGET question [scope]                        => 2 或 3
     * VSEARCH query [scope] [topk] [threshold]     => 2 到 5
     * VDEL question [scope]                        => 2 或 3
     * VINDEX REBUILD                               => 2
     * VINFO                                        => 1
     */
    if (strcmp(cmd->name, "VSET") == 0) {
        return argc == 3 || argc == 4;
    }    if (strcmp(cmd->name, "VGET") == 0) {
        return argc == 2 || argc == 3;
    }

    if (strcmp(cmd->name, "VSEARCH") == 0) {
        /*
        * 旧协议由 parse_command_argument 精确限制到 2..5；

        */
        return argc >= 2;
    }

    if (strcmp(cmd->name, "VDEL") == 0) {
        return argc == 2 || argc == 3;
    }

    if (strcmp(cmd->name, "VINDEX") == 0) {
        return argc == 2;
    }

    if (strcmp(cmd->name, "VINFO") == 0) {
        return argc == 1;
    }

    if(strcmp(cmd->name, "RRANGE") == 0 || strcmp(cmd->name, "SRANGE") == 0){
        return argc == 3 || argc == 5;
    }

    if(strcmp(cmd->name, "EXPIRE") == 0 || strcmp(cmd->name, "PEXPIREAT") == 0){
        return argc == 3;
    }

    if(strcmp(cmd->name, "AOFREWRITE") == 0){
        return argc == 1;
    }

    if(strcmp(cmd->name, "TTL") == 0 || strcmp(cmd->name, "PERSIST") == 0){
        return argc == 2;
    }

    if(cmd->arity >= 0) return argc == cmd->arity;
    else{
        //至少需要 |arity|个参数
        int min_args = -cmd->arity;

        //对于某些命令 还需检查最大参数数量
        if(strcmp(cmd->name, "SORT") == 0 || strcmp(cmd->name, "RSORT") == 0 || strcmp(cmd->name, "HSORT") == 0 || strcmp(cmd->name, "SSORT") == 0){
            //SORT 命令最多接受3个参数
            return argc >= min_args && argc <= 3;
        }

        //其他命令只需要满足最小参数要求
        return argc >= min_args;
    }
}

// 声明所有命令处理函数
int handle_set(command_context_t *ctx, char *response){
    int ret = -1;

    // 按配置限制单条 value 大小；0 表示不限制
    // if (g_cfg.max_value_size > 0 && ctx->value) {
    //     size_t vlen = strlen(ctx->value);
    //     if (vlen > g_cfg.max_value_size) {
    //         return resp_generate_error(response, "ERR value too large, check max_value_size");
    //     }
    // }

    switch(ctx->data_structure){
#if ENABLE_ARRAY
        case DATA_STRUCTURE_ARRAY:
            ret = kvs_array_set_len(kvs_get_global_array(), ctx->key, ctx->key_len, ctx->value, ctx->value_len);
            break;
#endif
#if ENABLE_RBTREE
        case DATA_STRUCTURE_RBTREE:
            ret = kvs_rbtree_set_len(kvs_get_global_rbtree(), ctx->key, ctx->key_len, ctx->value, ctx->value_len);
            break;
#endif
#if ENABLE_HASH
        case DATA_STRUCTURE_HASH:
            ret = kvs_hash_set_len(kvs_get_global_hash(), ctx->key, ctx->key_len, ctx->value, ctx->value_len);
            break;
#endif
#if ENABLE_SKIPTABLE
        case DATA_STRUCTURE_SKIPTABLE:
            ret = kvs_skiptable_set_len(kvs_get_global_skiptable(), ctx->key, ctx->key_len, ctx->value, ctx->value_len);
            break;
#endif
        default:
            return resp_generate_simple_string_n(response, '-', "Data structure not enabled", sizeof("Data structure not enabled") - 1);
    }

    if (ret < 0) {
        return resp_generate_simple_string_n(response, '-', "ERROR", sizeof("ERROR") - 1);
    } else if (ret == 0) {
        return resp_generate_simple_string_n(response, '+', "OK", sizeof("OK") - 1);
    } else {
        return resp_generate_simple_string_n(response, '+', "EXIST", sizeof("EXIST") - 1);
    }

}

int handle_get(command_context_t *ctx, char *response){
    char *result = NULL;
    ttl_target_ref_t ref;
    memset(&ref, 0, sizeof(ref));

    int apply_expired_side_effects = (ctx->lock_mode != KVS_LOCK_MODE_READ);
    int ttl_ret = ttl_expire_if_needed_by_structure(ctx->data_structure, ctx->key, ctx->key_len, &ref, apply_expired_side_effects);
    if (ttl_ret < 0) {
        return resp_generate_simple_string_n(response, '-', "ERROR", sizeof("ERROR") - 1);
    } else if (ttl_ret > 0) {
        if (!apply_expired_side_effects) {
            return KVS_CMD_RETRY_WITH_WRITE_LOCK;
        }
        return resp_generate_simple_string_n(response, '+', "NO EXIST", sizeof("NO EXIST") - 1);
    }

    if (ref.kind == TTL_TARGET_NONE) {
        return resp_generate_simple_string_n(response, '+', "NO EXIST", sizeof("NO EXIST") - 1);
    }

    result = (char *)ttl_target_get_value(&ref);
    if (result == NULL) {
        return resp_generate_simple_string_n(response, '+', "NO EXIST", sizeof("NO EXIST") - 1);
    } else {
        size_t result_len = ttl_target_get_value_len(&ref);
        size_t est = resp_estimate_bulk_string_len_from_size(result_len);
        if (est > KVS_MAX_RESPONSE_LENGTH) {
            return resp_generate_simple_string_n(response, '-', "ERR value too large", sizeof("ERR value too large") - 1);
        }
        return resp_generate_bulk_string_n(response, result, result_len);
    }
}
int handle_del(command_context_t *ctx, char *response){
    int ret = -1;

    switch (ctx->data_structure) {
#if ENABLE_ARRAY
        case DATA_STRUCTURE_ARRAY:
            ret = kvs_array_del_len(kvs_get_global_array(), ctx->key, ctx->key_len);
            break;
#endif
#if ENABLE_RBTREE
        case DATA_STRUCTURE_RBTREE:
            ret = kvs_rbtree_del_len(kvs_get_global_rbtree(), ctx->key, ctx->key_len);
            break;
#endif
#if ENABLE_HASH
        case DATA_STRUCTURE_HASH:
            ret = kvs_hash_del_len(kvs_get_global_hash(), ctx->key, ctx->key_len);
            break;
#endif
#if ENABLE_SKIPTABLE
        case DATA_STRUCTURE_SKIPTABLE:
            ret = kvs_skiptable_del_len(kvs_get_global_skiptable(), ctx->key, ctx->key_len);
            break;
#endif
        default:
            return resp_generate_simple_string_n(response, '-', "Data structure not enabled", sizeof("Data structure not enabled") - 1);
    }

    if (ret < 0) {
        return resp_generate_simple_string_n(response, '-', "ERROR", sizeof("ERROR") - 1);
    } else if (ret == 0) {
        return resp_generate_simple_string_n(response, '+', "OK", sizeof("OK") - 1);
    } else {
        return resp_generate_simple_string_n(response, '+', "NO EXIST", sizeof("NO EXIST") - 1);
    }
}
int handle_mod(command_context_t *ctx, char *response){
    int ret = -1;

    // 按配置限制单条 value 大小；0 表示不限制
    // if (g_cfg.max_value_size > 0 && ctx->value) {
    //     size_t vlen = strlen(ctx->value);
    //     if (vlen > g_cfg.max_value_size) {
    //         return resp_generate_error(response, "ERR value too large, check max_value_size");
    //     }
    // }

    switch(ctx->data_structure){
#if ENABLE_ARRAY
        case DATA_STRUCTURE_ARRAY:
            ret = kvs_array_mod_len(kvs_get_global_array(), ctx->key, ctx->key_len, ctx->value, ctx->value_len);
            break;
#endif
#if ENABLE_RBTREE
        case DATA_STRUCTURE_RBTREE:
            ret = kvs_rbtree_mod_len(kvs_get_global_rbtree(), ctx->key, ctx->key_len, ctx->value, ctx->value_len);
            break;
#endif
#if ENABLE_HASH
        case DATA_STRUCTURE_HASH:
            ret = kvs_hash_mod_len(kvs_get_global_hash(), ctx->key, ctx->key_len, ctx->value, ctx->value_len);
            break;
#endif
#if ENABLE_SKIPTABLE
        case DATA_STRUCTURE_SKIPTABLE:
            ret = kvs_skiptable_mod_len(kvs_get_global_skiptable(), ctx->key, ctx->key_len, ctx->value, ctx->value_len);
            break;
#endif
        default:
            return resp_generate_simple_string_n(response, '-', "Data structure not enabled", sizeof("Data structure not enabled") - 1);
    }

    if (ret < 0) {
        return resp_generate_simple_string_n(response, '-', "ERROR", sizeof("ERROR") - 1);
    } else if (ret == 0) {
        return resp_generate_simple_string_n(response, '+', "OK", sizeof("OK") - 1);
    } else {
        return resp_generate_simple_string_n(response, '+', "NO EXIST", sizeof("NO EXIST") - 1);
    }
}
int handle_exist(command_context_t *ctx, char *response){
    int ret = -1;
    ttl_target_ref_t ref;
    memset(&ref, 0, sizeof(ref));

    int apply_expired_side_effects = (ctx->lock_mode != KVS_LOCK_MODE_READ);
    int ttl_ret = ttl_expire_if_needed_by_structure(ctx->data_structure, ctx->key, ctx->key_len, &ref, apply_expired_side_effects);
    if (ttl_ret < 0) {
        return resp_generate_simple_string_n(response, '-', "ERROR", sizeof("ERROR") - 1);
    } else if (ttl_ret > 0) {
        if (!apply_expired_side_effects) {
            return KVS_CMD_RETRY_WITH_WRITE_LOCK;
        }
        return resp_generate_simple_string_n(response, '+', "NO EXIST", sizeof("NO EXIST") - 1);
    }

    if (ref.kind == TTL_TARGET_NONE) {
        return resp_generate_simple_string_n(response, '+', "NO EXIST", sizeof("NO EXIST") - 1);
    }

    ret = 0;
    if (ret == 0) {
        return resp_generate_simple_string_n(response, '+', "EXIST", sizeof("EXIST") - 1);
    } else {
        return resp_generate_simple_string_n(response, '+', "NO EXIST", sizeof("NO EXIST") - 1);
    }
}

int handle_expire(command_context_t *ctx, char *response){
    int ret = ttl_apply_any_key_op(ctx, KVS_CMD_EXPIRE);
    if (ret < 0) {
        return resp_generate_simple_string_n(response, '-', "ERROR", sizeof("ERROR") - 1);
    }
    if (ret == 0) {
        return resp_generate_simple_string_n(response, '+', "NO EXIST", sizeof("NO EXIST") - 1);
    }
    return resp_generate_simple_string_n(response, '+', "OK", sizeof("OK") - 1);
}

int handle_ttl(command_context_t *ctx, char *response){
    int ret = ttl_apply_any_key_op(ctx, KVS_CMD_TTL);
    if (ret < -2) {
        return resp_generate_simple_string_n(response, '-', "ERROR", sizeof("ERROR") - 1);
    }
    return resp_generate_integer(response, ret);
}

int handle_persist(command_context_t *ctx, char *response){
    int ret = ttl_apply_any_key_op(ctx, KVS_CMD_PERSIST);
    if (ret < 0) {
        return resp_generate_simple_string_n(response, '-', "ERROR", sizeof("ERROR") - 1);
    }
    if (ret == 0) {
        return resp_generate_simple_string_n(response, '+', "NO EXIST", sizeof("NO EXIST") - 1);
    }
    return resp_generate_simple_string_n(response, '+', "OK", sizeof("OK") - 1);
}

int handle_pexpireat(command_context_t *ctx, char *response){
    int ret = ttl_apply_any_key_op(ctx, KVS_CMD_PEXPIREAT);
    if (ret < 0) {
        return resp_generate_simple_string_n(response, '-', "ERROR", sizeof("ERROR") - 1);
    }
    if (ret == 0) {
        return resp_generate_simple_string_n(response, '+', "NO EXIST", sizeof("NO EXIST") - 1);
    }
    return resp_generate_simple_string_n(response, '+', "OK", sizeof("OK") - 1);
}

int handle_range(command_context_t *ctx, char *response){
    int ret = -1;
	char *temp_response = NULL;

    temp_response = kvs_malloc(KVS_MAX_RESPONSE_LENGTH);
    if (!temp_response) {
        return resp_generate_simple_string_n(response, '-', "ERROR", sizeof("ERROR") - 1);
    }
    temp_response[0] = '\0';

    switch(ctx->data_structure){
#if ENABLE_ARRAY
        case DATA_STRUCTURE_ARRAY:
    			ret = kvs_array_range(kvs_get_global_array(), ctx->start_key, ctx->end_key, temp_response, KVS_MAX_RESPONSE_LENGTH);
            break;
#endif
#if ENABLE_RBTREE
        case DATA_STRUCTURE_RBTREE:
			ret = kvs_rbtree_range(kvs_get_global_rbtree(), ctx->start_key, ctx->end_key, ctx->limit, temp_response, KVS_MAX_RESPONSE_LENGTH);
            break;
#endif
#if ENABLE_HASH
        case DATA_STRUCTURE_HASH:
    			ret = kvs_hash_range(kvs_get_global_hash(), ctx->start_key, ctx->end_key, temp_response, KVS_MAX_RESPONSE_LENGTH);
            break;
#endif
#if ENABLE_SKIPTABLE
        case DATA_STRUCTURE_SKIPTABLE:
		    	ret = kvs_skiptable_range(kvs_get_global_skiptable(), ctx->start_key, ctx->end_key, ctx->limit, temp_response, KVS_MAX_RESPONSE_LENGTH);
            break;
#endif
        default:
            kvs_free(temp_response);
            return resp_generate_simple_string_n(response, '-', "Data structure not enabled", sizeof("Data structure not enabled") - 1);
    }

    if (ret <= 0) {
        kvs_free(temp_response);
        return resp_generate_array_start(response, 0);
    } else {
		// 这里假定 temp_response 中的内容长度不会超过 KVS_MAX_RESPONSE_LENGTH
		int n = convert_multiline_to_resp_array(temp_response, response, KVS_MAX_RESPONSE_LENGTH);
		kvs_free(temp_response);
		return n;
    }
}
int handle_sort(command_context_t *ctx, char *response){
    int ret = -1;
	char *temp_response = NULL;

    temp_response = kvs_malloc(KVS_MAX_RESPONSE_LENGTH);
    if (!temp_response) {
        return resp_generate_simple_string_n(response, '-', "ERROR", sizeof("ERROR") - 1);
    }

    temp_response[0] = '\0';

    switch(ctx->data_structure){
#if ENABLE_ARRAY
        case DATA_STRUCTURE_ARRAY:
    			ret = kvs_array_sort(kvs_get_global_array(), ctx->order, ctx->limit, temp_response, KVS_MAX_RESPONSE_LENGTH);
            break;
#endif
#if ENABLE_RBTREE
        case DATA_STRUCTURE_RBTREE:
    			ret = kvs_rbtree_sort(kvs_get_global_rbtree(), ctx->order, ctx->limit, temp_response, KVS_MAX_RESPONSE_LENGTH);
            break;
#endif
#if ENABLE_HASH
        case DATA_STRUCTURE_HASH:
    			ret = kvs_hash_sort(kvs_get_global_hash(), ctx->order, ctx->limit, temp_response, KVS_MAX_RESPONSE_LENGTH);
            break;
#endif
#if ENABLE_SKIPTABLE
        case DATA_STRUCTURE_SKIPTABLE:
    			ret = kvs_skiptable_sort(kvs_get_global_skiptable(), ctx->order, ctx->limit, temp_response, KVS_MAX_RESPONSE_LENGTH);
            break;
#endif

        default:
        kvs_free(temp_response);
        return resp_generate_simple_string_n(response, '-', "Data structure not enabled", sizeof("Data structure not enabled") - 1);
    }

        if (ret <= 0) {
        kvs_free(temp_response);
        return resp_generate_array_start(response, 0);
        } else {
		int n = convert_multiline_to_resp_array(temp_response, response, KVS_MAX_RESPONSE_LENGTH);
		kvs_free(temp_response);
		return n;
        }

}
int handle_quit(command_context_t *ctx, char *response){
    (void)ctx;
    kvs_shutdown_request();
    return resp_generate_simple_string_n(response, '+', "Server shutting down...", sizeof("Server shutting down...") - 1);
}

//安全地往 response[] 末尾追加数据
static int fulldump_append_raw(char* resp,
                                int max_len,
                                int* off,
                                const char* data,
                                int len)
{
    if(*off + len > max_len){
        return -1; // 超出缓冲区
    }
    memcpy(resp + *off, data, len);
    *off += len;
    return 0;
}
// 追加 "*<n>\r\n"
static int fulldump_append_array_header(char *resp,
                                        int max_len,
                                        int *off,
                                        int count)
{

    char tmp[64];
    int n = snprintf(tmp, sizeof(tmp), "*%d\r\n", count);
    if (n < 0) return -1;
    return fulldump_append_raw(resp, max_len, off, tmp, n);
}

// 追加 "$<len>\r\n<buf>\r\n"
static int fulldump_append_bulk(char *resp, int max_len, int *off,
                                const char *data, int len) {
    char tmp[64];
    int n = snprintf(tmp, sizeof(tmp), "$%d\r\n", len);
    if (n <= 0) return -1;
    if (fulldump_append_raw(resp, max_len, off, tmp, n) < 0) return -1;
    if (fulldump_append_raw(resp, max_len, off, data, len) < 0) return -1;
    if (fulldump_append_raw(resp, max_len, off, "\r\n", 2) < 0) return -1;
    return 0;
}

static int fulldump_append_cmd3_len(char *resp, int max_len, int *off,
                                    const char *cmd, int cmdlen,
                                    const char *key, int keylen,
                                    const char *val, int vallen) {
    if (fulldump_append_array_header(resp, max_len, off, 3) < 0) return -1;
    if (fulldump_append_bulk(resp, max_len, off, cmd, cmdlen) < 0) return -1;
    if (fulldump_append_bulk(resp, max_len, off, key, keylen) < 0) return -1;
    if (fulldump_append_bulk(resp, max_len, off, val, vallen) < 0) return -1;
    return 0;
}

// 追加一条简单的 3 参数命令：CMD key value
static int fulldump_append_cmd3(char *resp, int max_len, int *off,
                                const char *cmd,
                                const char *key, const char *val) {
    int cmdlen = (int)strlen(cmd);
    int klen   = (int)strlen(key);
    int vlen   = (int)strlen(val);

    if (fulldump_append_array_header(resp, max_len, off, 3) < 0) return -1;
    if (fulldump_append_bulk(resp, max_len, off, cmd, cmdlen)   < 0) return -1;
    if (fulldump_append_bulk(resp, max_len, off, key, klen)     < 0) return -1;
    if (fulldump_append_bulk(resp, max_len, off, val, vlen)     < 0) return -1;
    return 0;
}

int handle_fulldump(command_context_t* ctx, char* response){
    (void)ctx;

    //只允许在主节点执行
    if(global_dist_config.role != ROLE_MASTER){
        return resp_generate_simple_string_n(response, '-', "ERR FULLDUMP allowed only on master node", sizeof("ERR FULLDUMP allowed only on master node") - 1);
    }

    int off = 0;
    const int max_len = KVS_MAX_RESPONSE_LENGTH;
    int64_t now_ms = kv_now_ms();
    char expire_buf[32];

    //遍历生成
#if ENABLE_ARRAY
    {
        extern kvs_array_t global_array; // 已在头部声明，这里只是强调一下
        if (global_array.table && global_array.len > 0) {
            for (int i = 0; i < global_array.len; i++) {
                kvs_array_item_t *it = &global_array.table[i];
                if (!it->key || !it->value) continue;
                if (kv_is_expired(it->expire_at_ms, now_ms)) continue;

                if (fulldump_append_cmd3_len(response, max_len, &off,
                                             "SET", 3,
                                             it->key, (int)it->key_len,
                                             it->value, (int)it->value_len) < 0) {
                    // 缓冲区满了，直接停止，返回已填充长度
                    return off;
                }

                if (it->expire_at_ms >= 0) {
                    snprintf(expire_buf, sizeof(expire_buf), "%lld", (long long)it->expire_at_ms);
                    if (fulldump_append_cmd3_len(response, max_len, &off,
                                                 "PEXPIREAT", 9,
                                                 it->key, (int)it->key_len,
                                                 expire_buf, (int)strlen(expire_buf)) < 0) {
                        return off;
                    }
                }
            }
        }
    }
#endif
#if ENABLE_HASH
    {
        extern kvs_hash_t global_hash;
        if (global_hash.nodes && global_hash.max_slots > 0) {
            for (int i = 0; i < global_hash.max_slots; ++i) {
                hashnode_t *slot = &global_hash.nodes[i];
                if (slot->used != 1 || !slot->key || !slot->value) {
                    continue;
                }
                if (kv_is_expired(slot->expire_at_ms, now_ms)) continue;
                if (fulldump_append_cmd3_len(response, max_len, &off,
                                             "HSET", 4,
                                             slot->key, (int)slot->key_len,
                                             slot->value, (int)slot->value_len) < 0) {
                    return off;
                }

                if (slot->expire_at_ms >= 0) {
                    snprintf(expire_buf, sizeof(expire_buf), "%lld", (long long)slot->expire_at_ms);
                    if (fulldump_append_cmd3_len(response, max_len, &off,
                                                 "PEXPIREAT", 9,
                                                 slot->key, (int)slot->key_len,
                                                 expire_buf, (int)strlen(expire_buf)) < 0) {
                        return off;
                    }
                }
            }
        }
    }
#endif
#if ENABLE_RBTREE
    rbtree_dump_ctx_t rbt_ctx = { response, max_len, &off, 0, now_ms };

    rbtree_traversal_with_callback((rbtree *)&global_rbtree,
                                   global_rbtree.root,
                                   rbtree_dump_cb,
                                   &rbt_ctx);
    if (rbt_ctx.overflow) {
        return off;
    }
#endif
#if ENABLE_SKIPTABLE
    {
        extern kvs_skiptable_t global_skiptable;
        Node *current = global_skiptable.header ? global_skiptable.header->forward[0] : NULL;
        while (current) {
            if (current->key && current->value && !kv_is_expired(current->expire_at_ms, now_ms)) {
                if (fulldump_append_cmd3_len(response, max_len, &off,
                                             "SSET", 4,
                                             current->key, (int)current->key_len,
                                             current->value, (int)current->value_len) < 0) {
                    return off;
                }

                if (current->expire_at_ms >= 0) {
                    snprintf(expire_buf, sizeof(expire_buf), "%lld", (long long)current->expire_at_ms);
                    if (fulldump_append_cmd3_len(response, max_len, &off,
                                                 "PEXPIREAT", 9,
                                                 current->key, (int)current->key_len,
                                                 expire_buf, (int)strlen(expire_buf)) < 0) {
                        return off;
                    }
                }
            }
            current = current->forward[0];
        }
    }
#endif

    return off;
}

/*
 * CLEANUP_CANDIDATE:
 * Unused handler — no current caller in command table or elsewhere.
 * Each V-family command has its own handler.
 * Safe to remove after Index abstraction phase if still unused.
 */
static int handle_v_not_implemented(command_context_t *ctx, char *response) {
    (void)ctx;
    return resp_generate_simple_string_n(response, '-', "NOT IMPLEMENTED", sizeof("NOT IMPLEMENTED") - 1);
}

/*
 * 将 vector 模块内部状态码转换为 RESP 响应。
 *
 * 注意：
 * handler 的返回值是 RESP 写入长度，不是业务状态码。
 */
static int handle_vector_status_response(int status, char *response) {
    switch (status) {
        case KVS_VECTOR_OK:
            return resp_generate_simple_string_n(
                response,
                '+',
                "OK",
                sizeof("OK") - 1
            );

        case KVS_VECTOR_ERR_INVALID_ARG:
            return resp_generate_simple_string_n(
                response,
                '-',
                "ERR invalid vector argument",
                sizeof("ERR invalid vector argument") - 1
            );

        case KVS_VECTOR_ERR_NOT_FOUND:
            return resp_generate_simple_string_n(
                response,
                '+',
                "NO EXIST",
                sizeof("NO EXIST") - 1
            );

        case KVS_VECTOR_ERR_NOT_IMPLEMENTED:
            return resp_generate_simple_string_n(
                response,
                '-',
                "NOT IMPLEMENTED",
                sizeof("NOT IMPLEMENTED") - 1
            );

        case KVS_VECTOR_ERR_NOT_INITIALIZED:
            return resp_generate_simple_string_n(
                response,
                '-',
                "ERR vector not initialized",
                sizeof("ERR vector not initialized") - 1
            );

        case KVS_VECTOR_ERR:
        default:
            return resp_generate_simple_string_n(
                response,
                '-',
                "ERR vector internal error",
                sizeof("ERR vector internal error") - 1
            );
    }
}

/*
 * 生成 VINFO 的 RESP 数组。
 *
 * 返回格式：
 * [
 *   "total_records:<n>",
 *   "active_records:<n>",
 *   "deleted_records:<n>",
 *   "capacity:<n>",
 *   "index_size:<n>",
 *   "dimension:<n>",
 *   "index_ready:<n>",
 *   "needs_rebuild:<n>"
 * ]
 */
static int handle_vector_info_response(const kvs_vector_info_t *info, char *response) {
    char item[128];
    int off = 0;
    int n;

    if (!info || !response) {
        return resp_generate_simple_string_n(
            response,
            '-',
            "ERR invalid vector info",
            sizeof("ERR invalid vector info") - 1
        );
    }

	    {
		        int array_count = 88;  /* base + backend stats + worker pool + backend + VSEARCH profile stats */
	        if (info->index_type == KVS_VECTOR_INDEX_TYPE_SQ8_FLAT) {
	            array_count += 7;
	        }
	        if (info->index_type == KVS_VECTOR_INDEX_CORE_HNSW) {
		            array_count += 128;  /* core_hnsw stats + HNSW profile + Stage 7A/7D/7F/7H/7L/9A1 counters */
	        }
	        off += resp_generate_array_start(response + off, array_count);
	    }
    n = snprintf(item, sizeof(item), "total_records:%zu", info->total_records);
    if (n < 0 || n >= (int)sizeof(item)) {
        return resp_generate_simple_string_n(response, '-', "ERR response too large", sizeof("ERR response too large") - 1);
    }
    off += resp_generate_bulk_string_n(response + off, item, (size_t)n);

    n = snprintf(item, sizeof(item), "active_records:%zu", info->active_records);
    if (n < 0 || n >= (int)sizeof(item)) {
        return resp_generate_simple_string_n(response, '-', "ERR response too large", sizeof("ERR response too large") - 1);
    }
    off += resp_generate_bulk_string_n(response + off, item, (size_t)n);

    n = snprintf(item, sizeof(item), "deleted_records:%zu", info->deleted_records);
    if (n < 0 || n >= (int)sizeof(item)) {
        return resp_generate_simple_string_n(response, '-', "ERR response too large", sizeof("ERR response too large") - 1);
    }
    off += resp_generate_bulk_string_n(response + off, item, (size_t)n);

    n = snprintf(item, sizeof(item), "capacity:%zu", info->capacity);
    if (n < 0 || n >= (int)sizeof(item)) {
        return resp_generate_simple_string_n(response, '-', "ERR response too large", sizeof("ERR response too large") - 1);
    }
    off += resp_generate_bulk_string_n(response + off, item, (size_t)n);

    n = snprintf(item, sizeof(item), "index_size:%zu", info->index_size);
    if (n < 0 || n >= (int)sizeof(item)) {
        return resp_generate_simple_string_n(response, '-', "ERR response too large", sizeof("ERR response too large") - 1);
    }
    off += resp_generate_bulk_string_n(response + off, item, (size_t)n);

    n = snprintf(item, sizeof(item), "dimension:%zu", info->dimension);
    if (n < 0 || n >= (int)sizeof(item)) {
        return resp_generate_simple_string_n(response, '-', "ERR response too large", sizeof("ERR response too large") - 1);
    }
    off += resp_generate_bulk_string_n(response + off, item, (size_t)n);

    n = snprintf(item, sizeof(item), "index_ready:%d", info->index_ready);
    if (n < 0 || n >= (int)sizeof(item)) {
        return resp_generate_simple_string_n(response, '-', "ERR response too large", sizeof("ERR response too large") - 1);
    }
    off += resp_generate_bulk_string_n(response + off, item, (size_t)n);

    n = snprintf(item, sizeof(item), "needs_rebuild:%d", info->needs_rebuild);
    if (n < 0 || n >= (int)sizeof(item)) {
        return resp_generate_simple_string_n(response, '-', "ERR response too large", sizeof("ERR response too large") - 1);
    }
    off += resp_generate_bulk_string_n(response + off, item, (size_t)n);
        n = snprintf(item, sizeof(item), "index_type:%s",
        info->index_type == KVS_VECTOR_INDEX_TYPE_HNSW ? "hnsw" :
        (info->index_type == KVS_VECTOR_INDEX_HNSWLIB_HNSW ? "hnswlib_hnsw" :
         (info->index_type == KVS_VECTOR_INDEX_FAISS_HNSW ? "faiss_hnsw" :
          (info->index_type == KVS_VECTOR_INDEX_FAISS_FLAT ? "faiss_flat" :
           (info->index_type == KVS_VECTOR_INDEX_TYPE_SQ8_FLAT ? "sq8_flat" :
            (info->index_type == KVS_VECTOR_INDEX_CORE_FLAT ? "core_flat" :
             (info->index_type == KVS_VECTOR_INDEX_CORE_HNSW ? "core_hnsw" : "flat")))))));
    if (n < 0 || n >= (int)sizeof(item)) {
        return resp_generate_simple_string_n(response, '-', "ERR response too large", sizeof("ERR response too large") - 1);
    }
    off += resp_generate_bulk_string_n(response + off, item, (size_t)n);

    n = snprintf(item, sizeof(item), "metric:%s",
        info->metric_type == KVS_VECTOR_METRIC_L2 ? "l2" :
        (info->metric_type == KVS_VECTOR_METRIC_COSINE ? "cosine" : "ip"));
    if (n < 0 || n >= (int)sizeof(item)) {
        return resp_generate_simple_string_n(response, '-', "ERR response too large", sizeof("ERR response too large") - 1);
    }
    off += resp_generate_bulk_string_n(response + off, item, (size_t)n);

    n = snprintf(item, sizeof(item), "distance_backend:%s",
                 kvs_vector_distance_backend_name());
    if (n < 0 || n >= (int)sizeof(item)) {
        return resp_generate_simple_string_n(response, '-', "ERR response too large", sizeof("ERR response too large") - 1);
    }
    off += resp_generate_bulk_string_n(response + off, item, (size_t)n);

    n = snprintf(item, sizeof(item), "flat_physical_size:%zu", info->flat_physical_size);
    if (n < 0 || n >= (int)sizeof(item)) {
        return resp_generate_simple_string_n(response, '-', "ERR response too large", sizeof("ERR response too large") - 1);
    }
    off += resp_generate_bulk_string_n(response + off, item, (size_t)n);

    n = snprintf(item, sizeof(item), "core_flat_deleted_count:%zu", info->core_flat_deleted_count);
    if (n < 0 || n >= (int)sizeof(item)) {
        return resp_generate_simple_string_n(response, '-', "ERR response too large", sizeof("ERR response too large") - 1);
    }
    off += resp_generate_bulk_string_n(response + off, item, (size_t)n);

    n = snprintf(item, sizeof(item), "core_flat_size:%zu",
        info->index_type == KVS_VECTOR_INDEX_CORE_FLAT ? info->backend_size : 0);
    if (n < 0 || n >= (int)sizeof(item)) {
        return resp_generate_simple_string_n(response, '-', "ERR response too large", sizeof("ERR response too large") - 1);
    }
    off += resp_generate_bulk_string_n(response + off, item, (size_t)n);

    n = snprintf(item, sizeof(item), "core_flat_capacity:%zu",
        info->index_type == KVS_VECTOR_INDEX_CORE_FLAT ? info->backend_capacity : 0);
    if (n < 0 || n >= (int)sizeof(item)) {
        return resp_generate_simple_string_n(response, '-', "ERR response too large", sizeof("ERR response too large") - 1);
    }
    off += resp_generate_bulk_string_n(response + off, item, (size_t)n);

    n = snprintf(item, sizeof(item), "core_flat_memory_bytes:%zu",
        info->index_type == KVS_VECTOR_INDEX_CORE_FLAT ? info->backend_memory_bytes : 0);
    if (n < 0 || n >= (int)sizeof(item)) {
        return resp_generate_simple_string_n(response, '-', "ERR response too large", sizeof("ERR response too large") - 1);
    }
    off += resp_generate_bulk_string_n(response + off, item, (size_t)n);

    n = snprintf(item, sizeof(item), "backend_name:%s", info->backend_name ? info->backend_name : "unknown");
    if (n < 0 || n >= (int)sizeof(item)) {
        return resp_generate_simple_string_n(response, '-', "ERR response too large", sizeof("ERR response too large") - 1);
    }
    off += resp_generate_bulk_string_n(response + off, item, (size_t)n);

    n = snprintf(item, sizeof(item), "backend_size:%zu", info->backend_size);
    if (n < 0 || n >= (int)sizeof(item)) {
        return resp_generate_simple_string_n(response, '-', "ERR response too large", sizeof("ERR response too large") - 1);
    }
    off += resp_generate_bulk_string_n(response + off, item, (size_t)n);

    n = snprintf(item, sizeof(item), "backend_capacity:%zu", info->backend_capacity);
    if (n < 0 || n >= (int)sizeof(item)) {
        return resp_generate_simple_string_n(response, '-', "ERR response too large", sizeof("ERR response too large") - 1);
    }
    off += resp_generate_bulk_string_n(response + off, item, (size_t)n);

    n = snprintf(item, sizeof(item), "backend_dim:%zu", info->backend_dim);
    if (n < 0 || n >= (int)sizeof(item)) {
        return resp_generate_simple_string_n(response, '-', "ERR response too large", sizeof("ERR response too large") - 1);
    }
    off += resp_generate_bulk_string_n(response + off, item, (size_t)n);

    n = snprintf(item, sizeof(item), "backend_metric:%s", info->backend_metric ? info->backend_metric : "unknown");
    if (n < 0 || n >= (int)sizeof(item)) {
        return resp_generate_simple_string_n(response, '-', "ERR response too large", sizeof("ERR response too large") - 1);
    }
    off += resp_generate_bulk_string_n(response + off, item, (size_t)n);

    n = snprintf(item, sizeof(item), "backend_memory_bytes:%zu", info->backend_memory_bytes);
    if (n < 0 || n >= (int)sizeof(item)) {
        return resp_generate_simple_string_n(response, '-', "ERR response too large", sizeof("ERR response too large") - 1);
    }
    off += resp_generate_bulk_string_n(response + off, item, (size_t)n);

    n = snprintf(item, sizeof(item), "backend_memory_bytes_source:%s",
        info->backend_memory_bytes_source ? info->backend_memory_bytes_source : "unknown");
    if (n < 0 || n >= (int)sizeof(item)) {
        return resp_generate_simple_string_n(response, '-', "ERR response too large", sizeof("ERR response too large") - 1);
    }
    off += resp_generate_bulk_string_n(response + off, item, (size_t)n);

    n = snprintf(item, sizeof(item), "backend_deleted_count:%zu", info->backend_deleted_count);
    if (n < 0 || n >= (int)sizeof(item)) {
        return resp_generate_simple_string_n(response, '-', "ERR response too large", sizeof("ERR response too large") - 1);
    }
    off += resp_generate_bulk_string_n(response + off, item, (size_t)n);

    n = snprintf(item, sizeof(item), "backend_needs_rebuild:%d", info->backend_needs_rebuild);
    if (n < 0 || n >= (int)sizeof(item)) {
        return resp_generate_simple_string_n(response, '-', "ERR response too large", sizeof("ERR response too large") - 1);
    }
    off += resp_generate_bulk_string_n(response + off, item, (size_t)n);

    n = snprintf(item, sizeof(item), "backend_resize_count:%s",
        info->backend_resize_count ? info->backend_resize_count : "n/a");
    if (n < 0 || n >= (int)sizeof(item)) {
        return resp_generate_simple_string_n(response, '-', "ERR response too large", sizeof("ERR response too large") - 1);
    }
    off += resp_generate_bulk_string_n(response + off, item, (size_t)n);

    n = snprintf(item, sizeof(item), "backend_reserve_count:%s",
        info->backend_reserve_count ? info->backend_reserve_count : "n/a");
    if (n < 0 || n >= (int)sizeof(item)) {
        return resp_generate_simple_string_n(response, '-', "ERR response too large", sizeof("ERR response too large") - 1);
    }
    off += resp_generate_bulk_string_n(response + off, item, (size_t)n);

    n = snprintf(item, sizeof(item), "backend_resize_count_source:%s",
        info->backend_resize_count_source ? info->backend_resize_count_source : "unsupported");
    if (n < 0 || n >= (int)sizeof(item)) {
        return resp_generate_simple_string_n(response, '-', "ERR response too large", sizeof("ERR response too large") - 1);
    }
    off += resp_generate_bulk_string_n(response + off, item, (size_t)n);

    n = snprintf(item, sizeof(item), "backend_reserve_count_source:%s",
        info->backend_reserve_count_source ? info->backend_reserve_count_source : "unsupported");
    if (n < 0 || n >= (int)sizeof(item)) {
        return resp_generate_simple_string_n(response, '-', "ERR response too large", sizeof("ERR response too large") - 1);
    }
    off += resp_generate_bulk_string_n(response + off, item, (size_t)n);

    if (info->index_type == KVS_VECTOR_INDEX_CORE_HNSW) {
        n = snprintf(item, sizeof(item), "core_hnsw_size:%zu", info->core_hnsw_size);
        if (n < 0 || n >= (int)sizeof(item)) {
            return resp_generate_simple_string_n(response, '-', "ERR response too large", sizeof("ERR response too large") - 1);
        }
        off += resp_generate_bulk_string_n(response + off, item, (size_t)n);

        n = snprintf(item, sizeof(item), "core_hnsw_capacity:%zu", info->core_hnsw_capacity);
        if (n < 0 || n >= (int)sizeof(item)) {
            return resp_generate_simple_string_n(response, '-', "ERR response too large", sizeof("ERR response too large") - 1);
        }
        off += resp_generate_bulk_string_n(response + off, item, (size_t)n);

        n = snprintf(item, sizeof(item), "core_hnsw_m:%zu", info->core_hnsw_m);
        if (n < 0 || n >= (int)sizeof(item)) {
            return resp_generate_simple_string_n(response, '-', "ERR response too large", sizeof("ERR response too large") - 1);
        }
        off += resp_generate_bulk_string_n(response + off, item, (size_t)n);

        n = snprintf(item, sizeof(item), "core_hnsw_m0:%zu", info->core_hnsw_m0);
        if (n < 0 || n >= (int)sizeof(item)) {
            return resp_generate_simple_string_n(response, '-', "ERR response too large", sizeof("ERR response too large") - 1);
        }
        off += resp_generate_bulk_string_n(response + off, item, (size_t)n);

        n = snprintf(item, sizeof(item), "core_hnsw_ef_construction:%zu", info->core_hnsw_ef_construction);
        if (n < 0 || n >= (int)sizeof(item)) {
            return resp_generate_simple_string_n(response, '-', "ERR response too large", sizeof("ERR response too large") - 1);
        }
        off += resp_generate_bulk_string_n(response + off, item, (size_t)n);

        n = snprintf(item, sizeof(item), "core_hnsw_ef_search:%zu", info->core_hnsw_ef_search);
        if (n < 0 || n >= (int)sizeof(item)) {
            return resp_generate_simple_string_n(response, '-', "ERR response too large", sizeof("ERR response too large") - 1);
        }
        off += resp_generate_bulk_string_n(response + off, item, (size_t)n);

        n = snprintf(item, sizeof(item), "core_hnsw_max_level:%d", info->core_hnsw_max_level);
        if (n < 0 || n >= (int)sizeof(item)) {
            return resp_generate_simple_string_n(response, '-', "ERR response too large", sizeof("ERR response too large") - 1);
        }
        off += resp_generate_bulk_string_n(response + off, item, (size_t)n);

        n = snprintf(item, sizeof(item), "core_hnsw_deleted_count:%zu", info->core_hnsw_deleted_count);
        if (n < 0 || n >= (int)sizeof(item)) {
            return resp_generate_simple_string_n(response, '-', "ERR response too large", sizeof("ERR response too large") - 1);
        }
        off += resp_generate_bulk_string_n(response + off, item, (size_t)n);

	        n = snprintf(item, sizeof(item), "core_hnsw_memory_bytes:%zu", info->backend_memory_bytes);
	        if (n < 0 || n >= (int)sizeof(item)) {
	            return resp_generate_simple_string_n(response, '-', "ERR response too large", sizeof("ERR response too large") - 1);
	        }
	        off += resp_generate_bulk_string_n(response + off, item, (size_t)n);

	        n = snprintf(item, sizeof(item), "rebuild_total_us:%llu", (unsigned long long)info->rebuild_total_us);
	        if (n < 0 || n >= (int)sizeof(item)) {
	            return resp_generate_simple_string_n(response, '-', "ERR response too large", sizeof("ERR response too large") - 1);
	        }
	        off += resp_generate_bulk_string_n(response + off, item, (size_t)n);

	        n = snprintf(item, sizeof(item), "rebuild_manager_scan_us:%llu", (unsigned long long)info->rebuild_manager_scan_us);
	        if (n < 0 || n >= (int)sizeof(item)) {
	            return resp_generate_simple_string_n(response, '-', "ERR response too large", sizeof("ERR response too large") - 1);
	        }
	        off += resp_generate_bulk_string_n(response + off, item, (size_t)n);

	        n = snprintf(item, sizeof(item), "rebuild_core_reset_us:%llu", (unsigned long long)info->rebuild_core_reset_us);
	        if (n < 0 || n >= (int)sizeof(item)) {
	            return resp_generate_simple_string_n(response, '-', "ERR response too large", sizeof("ERR response too large") - 1);
	        }
	        off += resp_generate_bulk_string_n(response + off, item, (size_t)n);

	        n = snprintf(item, sizeof(item), "rebuild_core_reserve_us:%llu", (unsigned long long)info->rebuild_core_reserve_us);
	        if (n < 0 || n >= (int)sizeof(item)) {
	            return resp_generate_simple_string_n(response, '-', "ERR response too large", sizeof("ERR response too large") - 1);
	        }
	        off += resp_generate_bulk_string_n(response + off, item, (size_t)n);

	        n = snprintf(item, sizeof(item), "rebuild_core_add_total_us:%llu", (unsigned long long)info->rebuild_core_add_total_us);
	        if (n < 0 || n >= (int)sizeof(item)) {
	            return resp_generate_simple_string_n(response, '-', "ERR response too large", sizeof("ERR response too large") - 1);
	        }
	        off += resp_generate_bulk_string_n(response + off, item, (size_t)n);

	        n = snprintf(item, sizeof(item), "rebuild_core_add_avg_us:%llu", (unsigned long long)info->rebuild_core_add_avg_us);
	        if (n < 0 || n >= (int)sizeof(item)) {
	            return resp_generate_simple_string_n(response, '-', "ERR response too large", sizeof("ERR response too large") - 1);
	        }
	        off += resp_generate_bulk_string_n(response + off, item, (size_t)n);

	        n = snprintf(item, sizeof(item), "rebuild_core_add_max_us:%llu", (unsigned long long)info->rebuild_core_add_max_us);
	        if (n < 0 || n >= (int)sizeof(item)) {
	            return resp_generate_simple_string_n(response, '-', "ERR response too large", sizeof("ERR response too large") - 1);
	        }
	        off += resp_generate_bulk_string_n(response + off, item, (size_t)n);

	        n = snprintf(item, sizeof(item), "rebuild_core_add_count:%llu", (unsigned long long)info->rebuild_core_add_count);
	        if (n < 0 || n >= (int)sizeof(item)) {
	            return resp_generate_simple_string_n(response, '-', "ERR response too large", sizeof("ERR response too large") - 1);
	        }
	        off += resp_generate_bulk_string_n(response + off, item, (size_t)n);

	        n = snprintf(item, sizeof(item), "hnsw_add_random_level_us:%llu", (unsigned long long)info->hnsw_add_random_level_us);
	        if (n < 0 || n >= (int)sizeof(item)) {
	            return resp_generate_simple_string_n(response, '-', "ERR response too large", sizeof("ERR response too large") - 1);
	        }
	        off += resp_generate_bulk_string_n(response + off, item, (size_t)n);

	        n = snprintf(item, sizeof(item), "hnsw_add_greedy_search_us:%llu", (unsigned long long)info->hnsw_add_greedy_search_us);
	        if (n < 0 || n >= (int)sizeof(item)) {
	            return resp_generate_simple_string_n(response, '-', "ERR response too large", sizeof("ERR response too large") - 1);
	        }
	        off += resp_generate_bulk_string_n(response + off, item, (size_t)n);

	        n = snprintf(item, sizeof(item), "hnsw_add_layer_search_us:%llu", (unsigned long long)info->hnsw_add_layer_search_us);
	        if (n < 0 || n >= (int)sizeof(item)) {
	            return resp_generate_simple_string_n(response, '-', "ERR response too large", sizeof("ERR response too large") - 1);
	        }
	        off += resp_generate_bulk_string_n(response + off, item, (size_t)n);

	        n = snprintf(item, sizeof(item), "hnsw_add_neighbor_select_us:%llu", (unsigned long long)info->hnsw_add_neighbor_select_us);
	        if (n < 0 || n >= (int)sizeof(item)) {
	            return resp_generate_simple_string_n(response, '-', "ERR response too large", sizeof("ERR response too large") - 1);
	        }
	        off += resp_generate_bulk_string_n(response + off, item, (size_t)n);

	        n = snprintf(item, sizeof(item), "hnsw_add_link_us:%llu", (unsigned long long)info->hnsw_add_link_us);
	        if (n < 0 || n >= (int)sizeof(item)) {
	            return resp_generate_simple_string_n(response, '-', "ERR response too large", sizeof("ERR response too large") - 1);
	        }
	        off += resp_generate_bulk_string_n(response + off, item, (size_t)n);

	        n = snprintf(item, sizeof(item), "hnsw_add_vector_copy_us:%llu", (unsigned long long)info->hnsw_add_vector_copy_us);
	        if (n < 0 || n >= (int)sizeof(item)) {
	            return resp_generate_simple_string_n(response, '-', "ERR response too large", sizeof("ERR response too large") - 1);
	        }
	        off += resp_generate_bulk_string_n(response + off, item, (size_t)n);

	        n = snprintf(item, sizeof(item), "add_distance_compute_count:%llu", (unsigned long long)info->add_distance_compute_count);
	        if (n < 0 || n >= (int)sizeof(item)) {
	            return resp_generate_simple_string_n(response, '-', "ERR response too large", sizeof("ERR response too large") - 1);
	        }
	        off += resp_generate_bulk_string_n(response + off, item, (size_t)n);

	        n = snprintf(item, sizeof(item), "add_neighbor_visit_count:%llu", (unsigned long long)info->add_neighbor_visit_count);
	        if (n < 0 || n >= (int)sizeof(item)) {
	            return resp_generate_simple_string_n(response, '-', "ERR response too large", sizeof("ERR response too large") - 1);
	        }
	        off += resp_generate_bulk_string_n(response + off, item, (size_t)n);

	        n = snprintf(item, sizeof(item), "add_visited_skip_count:%llu", (unsigned long long)info->add_visited_skip_count);
	        if (n < 0 || n >= (int)sizeof(item)) {
	            return resp_generate_simple_string_n(response, '-', "ERR response too large", sizeof("ERR response too large") - 1);
	        }
	        off += resp_generate_bulk_string_n(response + off, item, (size_t)n);

	        n = snprintf(item, sizeof(item), "add_active_check_count:%llu", (unsigned long long)info->add_active_check_count);
	        if (n < 0 || n >= (int)sizeof(item)) {
	            return resp_generate_simple_string_n(response, '-', "ERR response too large", sizeof("ERR response too large") - 1);
	        }
	        off += resp_generate_bulk_string_n(response + off, item, (size_t)n);

	        n = snprintf(item, sizeof(item), "add_top_heap_push_count:%llu", (unsigned long long)info->add_top_heap_push_count);
	        if (n < 0 || n >= (int)sizeof(item)) {
	            return resp_generate_simple_string_n(response, '-', "ERR response too large", sizeof("ERR response too large") - 1);
	        }
	        off += resp_generate_bulk_string_n(response + off, item, (size_t)n);

	        n = snprintf(item, sizeof(item), "add_top_heap_replace_count:%llu", (unsigned long long)info->add_top_heap_replace_count);
	        if (n < 0 || n >= (int)sizeof(item)) {
	            return resp_generate_simple_string_n(response, '-', "ERR response too large", sizeof("ERR response too large") - 1);
	        }
	        off += resp_generate_bulk_string_n(response + off, item, (size_t)n);

	        n = snprintf(item, sizeof(item), "add_candidate_push_count:%llu", (unsigned long long)info->add_candidate_push_count);
	        if (n < 0 || n >= (int)sizeof(item)) {
	            return resp_generate_simple_string_n(response, '-', "ERR response too large", sizeof("ERR response too large") - 1);
	        }
	        off += resp_generate_bulk_string_n(response + off, item, (size_t)n);

	        n = snprintf(item, sizeof(item), "add_level0_neighbor_scan_count:%llu", (unsigned long long)info->add_level0_neighbor_scan_count);
	        if (n < 0 || n >= (int)sizeof(item)) {
	            return resp_generate_simple_string_n(response, '-', "ERR response too large", sizeof("ERR response too large") - 1);
	        }
	        off += resp_generate_bulk_string_n(response + off, item, (size_t)n);

	        n = snprintf(item, sizeof(item), "add_upper_neighbor_scan_count:%llu", (unsigned long long)info->add_upper_neighbor_scan_count);
	        if (n < 0 || n >= (int)sizeof(item)) {
	            return resp_generate_simple_string_n(response, '-', "ERR response too large", sizeof("ERR response too large") - 1);
	        }
	        off += resp_generate_bulk_string_n(response + off, item, (size_t)n);

#define KVS_APPEND_U64_INFO_FIELD(field) \
	        do { \
	            n = snprintf(item, sizeof(item), #field ":%llu", (unsigned long long)info->field); \
	            if (n < 0 || n >= (int)sizeof(item)) { \
	                return resp_generate_simple_string_n(response, '-', "ERR response too large", sizeof("ERR response too large") - 1); \
	            } \
	            off += resp_generate_bulk_string_n(response + off, item, (size_t)n); \
	        } while (0)
	        KVS_APPEND_U64_INFO_FIELD(layer_search_call_count);
	        KVS_APPEND_U64_INFO_FIELD(layer_search_level0_call_count);
	        KVS_APPEND_U64_INFO_FIELD(layer_search_upper_call_count);
	        KVS_APPEND_U64_INFO_FIELD(layer_search_level0_us);
	        KVS_APPEND_U64_INFO_FIELD(layer_search_upper_us);
	        KVS_APPEND_U64_INFO_FIELD(layer_search_distance_compute_count);
	        KVS_APPEND_U64_INFO_FIELD(layer_search_visited_skip_count);
	        KVS_APPEND_U64_INFO_FIELD(layer_search_neighbor_visit_count);
	        KVS_APPEND_U64_INFO_FIELD(layer_search_level0_neighbor_scan_count);
	        KVS_APPEND_U64_INFO_FIELD(layer_search_upper_neighbor_scan_count);
	        KVS_APPEND_U64_INFO_FIELD(layer_search_candidate_heap_push_count);
	        KVS_APPEND_U64_INFO_FIELD(layer_search_candidate_heap_pop_count);
	        KVS_APPEND_U64_INFO_FIELD(layer_search_candidate_heap_replace_count);
	        KVS_APPEND_U64_INFO_FIELD(layer_search_top_heap_push_count);
		        KVS_APPEND_U64_INFO_FIELD(layer_search_top_heap_replace_count);
		        KVS_APPEND_U64_INFO_FIELD(layer_search_lower_bound_update_count);
		        KVS_APPEND_U64_INFO_FIELD(layer_search_deleted_active_check_count);
		        KVS_APPEND_U64_INFO_FIELD(layer_search_unique_visit_count);
		        KVS_APPEND_U64_INFO_FIELD(layer_search_repeated_visit_count);
		        KVS_APPEND_U64_INFO_FIELD(layer_search_visited_tag_access_count);
		        KVS_APPEND_U64_INFO_FIELD(layer_search_slot_state_touch_count);
		        KVS_APPEND_U64_INFO_FIELD(layer_search_level0_unique_visit_count);
		        KVS_APPEND_U64_INFO_FIELD(layer_search_level0_repeated_visit_count);
		        KVS_APPEND_U64_INFO_FIELD(layer_search_level0_visited_tag_access_count);
		        KVS_APPEND_U64_INFO_FIELD(layer_search_level0_slot_state_touch_count);
		        KVS_APPEND_U64_INFO_FIELD(layer_search_level0_neighbor_row_access_count);
		        KVS_APPEND_U64_INFO_FIELD(layer_search_level0_neighbor_row_sequential_count);
		        KVS_APPEND_U64_INFO_FIELD(layer_search_level0_neighbor_row_random_count);
		        KVS_APPEND_U64_INFO_FIELD(layer_search_level0_neighbor_slot_sequential_count);
		        KVS_APPEND_U64_INFO_FIELD(layer_search_level0_neighbor_slot_random_count);
		        KVS_APPEND_U64_INFO_FIELD(layer_search_level0_candidate_row_expansion_count);
		        KVS_APPEND_U64_INFO_FIELD(layer_search_level0_distance_compute_count);
		        KVS_APPEND_U64_INFO_FIELD(layer_search_upper_distance_compute_count);
		        KVS_APPEND_U64_INFO_FIELD(layer_search_level0_deleted_active_check_count);
		        KVS_APPEND_U64_INFO_FIELD(layer_search_upper_deleted_active_check_count);
		        KVS_APPEND_U64_INFO_FIELD(layer_search_candidate_heap_skip_count);
		        KVS_APPEND_U64_INFO_FIELD(layer_search_candidate_heap_full_skip_count);
		        KVS_APPEND_U64_INFO_FIELD(layer_search_top_heap_skip_count);
		        KVS_APPEND_U64_INFO_FIELD(layer_search_level0_candidate_heap_push_count);
		        KVS_APPEND_U64_INFO_FIELD(layer_search_level0_candidate_heap_pop_count);
		        KVS_APPEND_U64_INFO_FIELD(layer_search_level0_candidate_heap_skip_count);
		        KVS_APPEND_U64_INFO_FIELD(layer_search_level0_top_heap_push_count);
		        KVS_APPEND_U64_INFO_FIELD(layer_search_level0_top_heap_replace_count);
		        KVS_APPEND_U64_INFO_FIELD(layer_search_level0_top_heap_skip_count);
		        KVS_APPEND_U64_INFO_FIELD(layer_search_level0_lower_bound_update_count);
		        KVS_APPEND_U64_INFO_FIELD(core_hnsw_level0_count_min);
		        KVS_APPEND_U64_INFO_FIELD(core_hnsw_level0_count_avg);
		        KVS_APPEND_U64_INFO_FIELD(core_hnsw_level0_count_p50);
		        KVS_APPEND_U64_INFO_FIELD(core_hnsw_level0_count_p95);
		        KVS_APPEND_U64_INFO_FIELD(core_hnsw_level0_count_max);
#undef KVS_APPEND_U64_INFO_FIELD

#define KVS_APPEND_SIZE_INFO_FIELD(field) \
		        do { \
		            n = snprintf(item, sizeof(item), #field ":%zu", info->field); \
		            if (n < 0 || n >= (int)sizeof(item)) { \
		                return resp_generate_simple_string_n(response, '-', "ERR response too large", sizeof("ERR response too large") - 1); \
		            } \
		            off += resp_generate_bulk_string_n(response + off, item, (size_t)n); \
		        } while (0)
		        KVS_APPEND_SIZE_INFO_FIELD(core_hnsw_memory_vectors_bytes);
		        KVS_APPEND_SIZE_INFO_FIELD(core_hnsw_memory_level0_neighbors_bytes);
		        KVS_APPEND_SIZE_INFO_FIELD(core_hnsw_memory_level0_scores_bytes);
		        KVS_APPEND_SIZE_INFO_FIELD(core_hnsw_memory_upper_neighbors_bytes);
		        KVS_APPEND_SIZE_INFO_FIELD(core_hnsw_memory_visited_tags_bytes);
		        KVS_APPEND_SIZE_INFO_FIELD(core_hnsw_memory_slot_state_bytes);
		        KVS_APPEND_SIZE_INFO_FIELD(core_hnsw_memory_context_bytes);
#undef KVS_APPEND_SIZE_INFO_FIELD

		        n = snprintf(item, sizeof(item), "neighbor_select_call_count:%llu", (unsigned long long)info->neighbor_select_call_count);
	        if (n < 0 || n >= (int)sizeof(item)) {
	            return resp_generate_simple_string_n(response, '-', "ERR response too large", sizeof("ERR response too large") - 1);
	        }
	        off += resp_generate_bulk_string_n(response + off, item, (size_t)n);

	        n = snprintf(item, sizeof(item), "neighbor_select_level0_call_count:%llu", (unsigned long long)info->neighbor_select_level0_call_count);
	        if (n < 0 || n >= (int)sizeof(item)) {
	            return resp_generate_simple_string_n(response, '-', "ERR response too large", sizeof("ERR response too large") - 1);
	        }
	        off += resp_generate_bulk_string_n(response + off, item, (size_t)n);

	        n = snprintf(item, sizeof(item), "neighbor_select_upper_call_count:%llu", (unsigned long long)info->neighbor_select_upper_call_count);
	        if (n < 0 || n >= (int)sizeof(item)) {
	            return resp_generate_simple_string_n(response, '-', "ERR response too large", sizeof("ERR response too large") - 1);
	        }
	        off += resp_generate_bulk_string_n(response + off, item, (size_t)n);

	        n = snprintf(item, sizeof(item), "neighbor_select_candidate_total:%llu", (unsigned long long)info->neighbor_select_candidate_total);
	        if (n < 0 || n >= (int)sizeof(item)) {
	            return resp_generate_simple_string_n(response, '-', "ERR response too large", sizeof("ERR response too large") - 1);
	        }
	        off += resp_generate_bulk_string_n(response + off, item, (size_t)n);

	        n = snprintf(item, sizeof(item), "neighbor_select_candidate_max:%llu", (unsigned long long)info->neighbor_select_candidate_max);
	        if (n < 0 || n >= (int)sizeof(item)) {
	            return resp_generate_simple_string_n(response, '-', "ERR response too large", sizeof("ERR response too large") - 1);
	        }
	        off += resp_generate_bulk_string_n(response + off, item, (size_t)n);

	        n = snprintf(item, sizeof(item), "neighbor_select_selected_total:%llu", (unsigned long long)info->neighbor_select_selected_total);
	        if (n < 0 || n >= (int)sizeof(item)) {
	            return resp_generate_simple_string_n(response, '-', "ERR response too large", sizeof("ERR response too large") - 1);
	        }
	        off += resp_generate_bulk_string_n(response + off, item, (size_t)n);

	        n = snprintf(item, sizeof(item), "neighbor_select_selected_max:%llu", (unsigned long long)info->neighbor_select_selected_max);
	        if (n < 0 || n >= (int)sizeof(item)) {
	            return resp_generate_simple_string_n(response, '-', "ERR response too large", sizeof("ERR response too large") - 1);
	        }
	        off += resp_generate_bulk_string_n(response + off, item, (size_t)n);

	        n = snprintf(item, sizeof(item), "neighbor_select_reject_count:%llu", (unsigned long long)info->neighbor_select_reject_count);
	        if (n < 0 || n >= (int)sizeof(item)) {
	            return resp_generate_simple_string_n(response, '-', "ERR response too large", sizeof("ERR response too large") - 1);
	        }
	        off += resp_generate_bulk_string_n(response + off, item, (size_t)n);

	        n = snprintf(item, sizeof(item), "neighbor_select_heuristic_prune_count:%llu", (unsigned long long)info->neighbor_select_heuristic_prune_count);
	        if (n < 0 || n >= (int)sizeof(item)) {
	            return resp_generate_simple_string_n(response, '-', "ERR response too large", sizeof("ERR response too large") - 1);
	        }
	        off += resp_generate_bulk_string_n(response + off, item, (size_t)n);

	        n = snprintf(item, sizeof(item), "neighbor_select_distance_recompute_count:%llu", (unsigned long long)info->neighbor_select_distance_recompute_count);
	        if (n < 0 || n >= (int)sizeof(item)) {
	            return resp_generate_simple_string_n(response, '-', "ERR response too large", sizeof("ERR response too large") - 1);
	        }
	        off += resp_generate_bulk_string_n(response + off, item, (size_t)n);

	        n = snprintf(item, sizeof(item), "neighbor_select_reused_candidate_score_count:%llu", (unsigned long long)info->neighbor_select_reused_candidate_score_count);
	        if (n < 0 || n >= (int)sizeof(item)) {
	            return resp_generate_simple_string_n(response, '-', "ERR response too large", sizeof("ERR response too large") - 1);
	        }
	        off += resp_generate_bulk_string_n(response + off, item, (size_t)n);

	        n = snprintf(item, sizeof(item), "neighbor_select_duplicate_scan_count:%llu", (unsigned long long)info->neighbor_select_duplicate_scan_count);
	        if (n < 0 || n >= (int)sizeof(item)) {
	            return resp_generate_simple_string_n(response, '-', "ERR response too large", sizeof("ERR response too large") - 1);
	        }
	        off += resp_generate_bulk_string_n(response + off, item, (size_t)n);

	        n = snprintf(item, sizeof(item), "neighbor_select_worst_scan_count:%llu", (unsigned long long)info->neighbor_select_worst_scan_count);
	        if (n < 0 || n >= (int)sizeof(item)) {
	            return resp_generate_simple_string_n(response, '-', "ERR response too large", sizeof("ERR response too large") - 1);
	        }
	        off += resp_generate_bulk_string_n(response + off, item, (size_t)n);

	        n = snprintf(item, sizeof(item), "neighbor_select_worst_cache_hit_count:%llu", (unsigned long long)info->neighbor_select_worst_cache_hit_count);
	        if (n < 0 || n >= (int)sizeof(item)) {
	            return resp_generate_simple_string_n(response, '-', "ERR response too large", sizeof("ERR response too large") - 1);
	        }
	        off += resp_generate_bulk_string_n(response + off, item, (size_t)n);

	        n = snprintf(item, sizeof(item), "neighbor_select_level0_candidate_total:%llu", (unsigned long long)info->neighbor_select_level0_candidate_total);
	        if (n < 0 || n >= (int)sizeof(item)) {
	            return resp_generate_simple_string_n(response, '-', "ERR response too large", sizeof("ERR response too large") - 1);
	        }
	        off += resp_generate_bulk_string_n(response + off, item, (size_t)n);

	        n = snprintf(item, sizeof(item), "neighbor_select_upper_candidate_total:%llu", (unsigned long long)info->neighbor_select_upper_candidate_total);
	        if (n < 0 || n >= (int)sizeof(item)) {
	            return resp_generate_simple_string_n(response, '-', "ERR response too large", sizeof("ERR response too large") - 1);
	        }
	        off += resp_generate_bulk_string_n(response + off, item, (size_t)n);

	        n = snprintf(item, sizeof(item), "neighbor_select_level0_selected_total:%llu", (unsigned long long)info->neighbor_select_level0_selected_total);
	        if (n < 0 || n >= (int)sizeof(item)) {
	            return resp_generate_simple_string_n(response, '-', "ERR response too large", sizeof("ERR response too large") - 1);
	        }
	        off += resp_generate_bulk_string_n(response + off, item, (size_t)n);

	        n = snprintf(item, sizeof(item), "neighbor_select_upper_selected_total:%llu", (unsigned long long)info->neighbor_select_upper_selected_total);
	        if (n < 0 || n >= (int)sizeof(item)) {
	            return resp_generate_simple_string_n(response, '-', "ERR response too large", sizeof("ERR response too large") - 1);
	        }
	        off += resp_generate_bulk_string_n(response + off, item, (size_t)n);

	        n = snprintf(item, sizeof(item), "neighbor_select_level0_reject_count:%llu", (unsigned long long)info->neighbor_select_level0_reject_count);
	        if (n < 0 || n >= (int)sizeof(item)) {
	            return resp_generate_simple_string_n(response, '-', "ERR response too large", sizeof("ERR response too large") - 1);
	        }
	        off += resp_generate_bulk_string_n(response + off, item, (size_t)n);

	        n = snprintf(item, sizeof(item), "neighbor_select_upper_reject_count:%llu", (unsigned long long)info->neighbor_select_upper_reject_count);
	        if (n < 0 || n >= (int)sizeof(item)) {
	            return resp_generate_simple_string_n(response, '-', "ERR response too large", sizeof("ERR response too large") - 1);
	        }
	        off += resp_generate_bulk_string_n(response + off, item, (size_t)n);

	        n = snprintf(item, sizeof(item), "neighbor_select_level0_heuristic_prune_count:%llu", (unsigned long long)info->neighbor_select_level0_heuristic_prune_count);
	        if (n < 0 || n >= (int)sizeof(item)) {
	            return resp_generate_simple_string_n(response, '-', "ERR response too large", sizeof("ERR response too large") - 1);
	        }
	        off += resp_generate_bulk_string_n(response + off, item, (size_t)n);

	        n = snprintf(item, sizeof(item), "neighbor_select_upper_heuristic_prune_count:%llu", (unsigned long long)info->neighbor_select_upper_heuristic_prune_count);
	        if (n < 0 || n >= (int)sizeof(item)) {
	            return resp_generate_simple_string_n(response, '-', "ERR response too large", sizeof("ERR response too large") - 1);
	        }
	        off += resp_generate_bulk_string_n(response + off, item, (size_t)n);

	        n = snprintf(item, sizeof(item), "neighbor_select_level0_distance_recompute_count:%llu", (unsigned long long)info->neighbor_select_level0_distance_recompute_count);
	        if (n < 0 || n >= (int)sizeof(item)) {
	            return resp_generate_simple_string_n(response, '-', "ERR response too large", sizeof("ERR response too large") - 1);
	        }
	        off += resp_generate_bulk_string_n(response + off, item, (size_t)n);

	        n = snprintf(item, sizeof(item), "neighbor_select_upper_distance_recompute_count:%llu", (unsigned long long)info->neighbor_select_upper_distance_recompute_count);
	        if (n < 0 || n >= (int)sizeof(item)) {
	            return resp_generate_simple_string_n(response, '-', "ERR response too large", sizeof("ERR response too large") - 1);
	        }
	        off += resp_generate_bulk_string_n(response + off, item, (size_t)n);

	        n = snprintf(item, sizeof(item), "neighbor_select_level0_reused_candidate_score_count:%llu", (unsigned long long)info->neighbor_select_level0_reused_candidate_score_count);
	        if (n < 0 || n >= (int)sizeof(item)) {
	            return resp_generate_simple_string_n(response, '-', "ERR response too large", sizeof("ERR response too large") - 1);
	        }
	        off += resp_generate_bulk_string_n(response + off, item, (size_t)n);

	        n = snprintf(item, sizeof(item), "neighbor_select_upper_reused_candidate_score_count:%llu", (unsigned long long)info->neighbor_select_upper_reused_candidate_score_count);
	        if (n < 0 || n >= (int)sizeof(item)) {
	            return resp_generate_simple_string_n(response, '-', "ERR response too large", sizeof("ERR response too large") - 1);
	        }
	        off += resp_generate_bulk_string_n(response + off, item, (size_t)n);

	        n = snprintf(item, sizeof(item), "neighbor_select_level0_duplicate_scan_count:%llu", (unsigned long long)info->neighbor_select_level0_duplicate_scan_count);
	        if (n < 0 || n >= (int)sizeof(item)) {
	            return resp_generate_simple_string_n(response, '-', "ERR response too large", sizeof("ERR response too large") - 1);
	        }
	        off += resp_generate_bulk_string_n(response + off, item, (size_t)n);

	        n = snprintf(item, sizeof(item), "neighbor_select_upper_duplicate_scan_count:%llu", (unsigned long long)info->neighbor_select_upper_duplicate_scan_count);
	        if (n < 0 || n >= (int)sizeof(item)) {
	            return resp_generate_simple_string_n(response, '-', "ERR response too large", sizeof("ERR response too large") - 1);
	        }
	        off += resp_generate_bulk_string_n(response + off, item, (size_t)n);

	        n = snprintf(item, sizeof(item), "neighbor_select_level0_worst_scan_count:%llu", (unsigned long long)info->neighbor_select_level0_worst_scan_count);
	        if (n < 0 || n >= (int)sizeof(item)) {
	            return resp_generate_simple_string_n(response, '-', "ERR response too large", sizeof("ERR response too large") - 1);
	        }
	        off += resp_generate_bulk_string_n(response + off, item, (size_t)n);

	        n = snprintf(item, sizeof(item), "neighbor_select_upper_worst_scan_count:%llu", (unsigned long long)info->neighbor_select_upper_worst_scan_count);
	        if (n < 0 || n >= (int)sizeof(item)) {
	            return resp_generate_simple_string_n(response, '-', "ERR response too large", sizeof("ERR response too large") - 1);
	        }
	        off += resp_generate_bulk_string_n(response + off, item, (size_t)n);

	        n = snprintf(item, sizeof(item), "neighbor_select_level0_worst_cache_hit_count:%llu", (unsigned long long)info->neighbor_select_level0_worst_cache_hit_count);
	        if (n < 0 || n >= (int)sizeof(item)) {
	            return resp_generate_simple_string_n(response, '-', "ERR response too large", sizeof("ERR response too large") - 1);
	        }
	        off += resp_generate_bulk_string_n(response + off, item, (size_t)n);

	        n = snprintf(item, sizeof(item), "neighbor_select_upper_worst_cache_hit_count:%llu", (unsigned long long)info->neighbor_select_upper_worst_cache_hit_count);
	        if (n < 0 || n >= (int)sizeof(item)) {
	            return resp_generate_simple_string_n(response, '-', "ERR response too large", sizeof("ERR response too large") - 1);
	        }
	        off += resp_generate_bulk_string_n(response + off, item, (size_t)n);

#define KVS_APPEND_U64_INFO_FIELD(field) \
	        do { \
	            n = snprintf(item, sizeof(item), #field ":%llu", (unsigned long long)info->field); \
	            if (n < 0 || n >= (int)sizeof(item)) { \
	                return resp_generate_simple_string_n(response, '-', "ERR response too large", sizeof("ERR response too large") - 1); \
	            } \
	            off += resp_generate_bulk_string_n(response + off, item, (size_t)n); \
	        } while (0)
	        KVS_APPEND_U64_INFO_FIELD(bidirectional_selected_neighbor_count);
	        KVS_APPEND_U64_INFO_FIELD(bidirectional_forward_edge_attempt_count);
	        KVS_APPEND_U64_INFO_FIELD(bidirectional_forward_edge_added_count);
	        KVS_APPEND_U64_INFO_FIELD(bidirectional_reverse_edge_attempt_count);
	        KVS_APPEND_U64_INFO_FIELD(bidirectional_reverse_edge_accepted_count);
	        KVS_APPEND_U64_INFO_FIELD(bidirectional_reverse_edge_rejected_count);
	        KVS_APPEND_U64_INFO_FIELD(bidirectional_forward_edge_skipped_due_to_reverse_reject_count);
#undef KVS_APPEND_U64_INFO_FIELD
	    }

    n = snprintf(item, sizeof(item), "ann_size:%zu", info->ann_size);
    if (n < 0 || n >= (int)sizeof(item)) {
        return resp_generate_simple_string_n(response, '-', "ERR response too large", sizeof("ERR response too large") - 1);
    }
    off += resp_generate_bulk_string_n(response + off, item, (size_t)n);

    n = snprintf(item, sizeof(item), "ann_m:%zu", info->ann_m);
    if (n < 0 || n >= (int)sizeof(item)) {
        return resp_generate_simple_string_n(response, '-', "ERR response too large", sizeof("ERR response too large") - 1);
    }
    off += resp_generate_bulk_string_n(response + off, item, (size_t)n);

    n = snprintf(item, sizeof(item), "ann_ef_construction:%zu", info->ann_ef_construction);
    if (n < 0 || n >= (int)sizeof(item)) {
        return resp_generate_simple_string_n(response, '-', "ERR response too large", sizeof("ERR response too large") - 1);
    }
    off += resp_generate_bulk_string_n(response + off, item, (size_t)n);

    n = snprintf(item, sizeof(item), "ann_ef_search:%zu", info->ann_ef_search);
    if (n < 0 || n >= (int)sizeof(item)) {
        return resp_generate_simple_string_n(response, '-', "ERR response too large", sizeof("ERR response too large") - 1);
    }
    off += resp_generate_bulk_string_n(response + off, item, (size_t)n);

    n = snprintf(item, sizeof(item), "ann_max_level:%d", info->ann_max_level);
    if (n < 0 || n >= (int)sizeof(item)) {
        return resp_generate_simple_string_n(response, '-', "ERR response too large", sizeof("ERR response too large") - 1);
    }
    off += resp_generate_bulk_string_n(response + off, item, (size_t)n);

    n = snprintf(item, sizeof(item), "ann_ready:%d", info->ann_ready);
    if (n < 0 || n >= (int)sizeof(item)) {
        return resp_generate_simple_string_n(response, '-', "ERR response too large", sizeof("ERR response too large") - 1);
    }
    off += resp_generate_bulk_string_n(response + off, item, (size_t)n);

    if (info->index_type == 2) {
        if (n < 0 || n >= (int)sizeof(item)) {
            return resp_generate_simple_string_n(response, '-', "ERR response too large", sizeof("ERR response too large") - 1);
        }
        off += resp_generate_bulk_string_n(response + off, item, (size_t)n);
        if (n < 0 || n >= (int)sizeof(item)) {
            return resp_generate_simple_string_n(response, '-', "ERR response too large", sizeof("ERR response too large") - 1);
        }
        off += resp_generate_bulk_string_n(response + off, item, (size_t)n);
        if (n < 0 || n >= (int)sizeof(item)) {
            return resp_generate_simple_string_n(response, '-', "ERR response too large", sizeof("ERR response too large") - 1);
        }
        off += resp_generate_bulk_string_n(response + off, item, (size_t)n);
        if (n < 0 || n >= (int)sizeof(item)) {
            return resp_generate_simple_string_n(response, '-', "ERR response too large", sizeof("ERR response too large") - 1);
        }
        off += resp_generate_bulk_string_n(response + off, item, (size_t)n);
        if (n < 0 || n >= (int)sizeof(item)) {
            return resp_generate_simple_string_n(response, '-', "ERR response too large", sizeof("ERR response too large") - 1);
        }
        off += resp_generate_bulk_string_n(response + off, item, (size_t)n);
        if (n < 0 || n >= (int)sizeof(item)) {
            return resp_generate_simple_string_n(response, '-', "ERR response too large", sizeof("ERR response too large") - 1);
        }
        off += resp_generate_bulk_string_n(response + off, item, (size_t)n);
        if (n < 0 || n >= (int)sizeof(item)) {
            return resp_generate_simple_string_n(response, '-', "ERR response too large", sizeof("ERR response too large") - 1);
        }
        off += resp_generate_bulk_string_n(response + off, item, (size_t)n);
        if (n < 0 || n >= (int)sizeof(item)) {
            return resp_generate_simple_string_n(response, '-', "ERR response too large", sizeof("ERR response too large") - 1);
        }
        off += resp_generate_bulk_string_n(response + off, item, (size_t)n);
    }

    if (info->index_type == KVS_VECTOR_INDEX_TYPE_SQ8_FLAT) {
        n = snprintf(item, sizeof(item), "sq8_size:%zu", info->sq8_size);
        if (n < 0 || n >= (int)sizeof(item)) {
            return resp_generate_simple_string_n(response, '-', "ERR response too large", sizeof("ERR response too large") - 1);
        }
        off += resp_generate_bulk_string_n(response + off, item, (size_t)n);

        n = snprintf(item, sizeof(item), "sq8_dimension:%zu", info->sq8_dimension);
        if (n < 0 || n >= (int)sizeof(item)) {
            return resp_generate_simple_string_n(response, '-', "ERR response too large", sizeof("ERR response too large") - 1);
        }
        off += resp_generate_bulk_string_n(response + off, item, (size_t)n);

        n = snprintf(item, sizeof(item), "sq8_code_bytes:%zu", info->sq8_code_bytes);
        if (n < 0 || n >= (int)sizeof(item)) {
            return resp_generate_simple_string_n(response, '-', "ERR response too large", sizeof("ERR response too large") - 1);
        }
        off += resp_generate_bulk_string_n(response + off, item, (size_t)n);

        n = snprintf(item, sizeof(item), "sq8_scale_bytes:%zu", info->sq8_scale_bytes);
        if (n < 0 || n >= (int)sizeof(item)) {
            return resp_generate_simple_string_n(response, '-', "ERR response too large", sizeof("ERR response too large") - 1);
        }
        off += resp_generate_bulk_string_n(response + off, item, (size_t)n);

        n = snprintf(item, sizeof(item), "sq8_memory_bytes:%zu", info->sq8_memory_bytes);
        if (n < 0 || n >= (int)sizeof(item)) {
            return resp_generate_simple_string_n(response, '-', "ERR response too large", sizeof("ERR response too large") - 1);
        }
        off += resp_generate_bulk_string_n(response + off, item, (size_t)n);

        n = snprintf(item, sizeof(item), "sq8_ready:%d", info->sq8_ready);
        if (n < 0 || n >= (int)sizeof(item)) {
            return resp_generate_simple_string_n(response, '-', "ERR response too large", sizeof("ERR response too large") - 1);
        }
        off += resp_generate_bulk_string_n(response + off, item, (size_t)n);

        n = snprintf(item, sizeof(item), "sq8_needs_rebuild:%d", info->sq8_needs_rebuild);
        if (n < 0 || n >= (int)sizeof(item)) {
            return resp_generate_simple_string_n(response, '-', "ERR response too large", sizeof("ERR response too large") - 1);
        }
        off += resp_generate_bulk_string_n(response + off, item, (size_t)n);
    }

    /* Stage 8: universal runtime stats */
    n = snprintf(item, sizeof(item), "last_rebuild_status:%d", info->last_rebuild_status);
    if (n < 0 || n >= (int)sizeof(item)) {
        return resp_generate_simple_string_n(response, '-', "ERR response too large", sizeof("ERR response too large") - 1);
    }
    off += resp_generate_bulk_string_n(response + off, item, (size_t)n);

    n = snprintf(item, sizeof(item), "last_rebuild_ms:%lld", (long long)info->last_rebuild_ms);
    if (n < 0 || n >= (int)sizeof(item)) {
        return resp_generate_simple_string_n(response, '-', "ERR response too large", sizeof("ERR response too large") - 1);
    }
    off += resp_generate_bulk_string_n(response + off, item, (size_t)n);
    if (n < 0 || n >= (int)sizeof(item)) {
        return resp_generate_simple_string_n(response, '-', "ERR response too large", sizeof("ERR response too large") - 1);
    }
    off += resp_generate_bulk_string_n(response + off, item, (size_t)n);
    if (n < 0 || n >= (int)sizeof(item)) {
        return resp_generate_simple_string_n(response, '-', "ERR response too large", sizeof("ERR response too large") - 1);
    }
    off += resp_generate_bulk_string_n(response + off, item, (size_t)n);

    n = snprintf(item, sizeof(item), "vector_memory_bytes:%zu", info->vector_memory_bytes);
    if (n < 0 || n >= (int)sizeof(item)) {
        return resp_generate_simple_string_n(response, '-', "ERR response too large", sizeof("ERR response too large") - 1);
    }
    off += resp_generate_bulk_string_n(response + off, item, (size_t)n);

    n = snprintf(item, sizeof(item), "index_memory_bytes:%zu", info->index_memory_bytes);
    if (n < 0 || n >= (int)sizeof(item)) {
        return resp_generate_simple_string_n(response, '-', "ERR response too large", sizeof("ERR response too large") - 1);
    }
    off += resp_generate_bulk_string_n(response + off, item, (size_t)n);

    n = snprintf(item, sizeof(item), "total_vector_memory_bytes:%zu", info->total_vector_memory_bytes);
    if (n < 0 || n >= (int)sizeof(item)) {
        return resp_generate_simple_string_n(response, '-', "ERR response too large", sizeof("ERR response too large") - 1);
    }
    off += resp_generate_bulk_string_n(response + off, item, (size_t)n);

    /* Stage 2B: worker pool + executor prepared stats */
    n = snprintf(item, sizeof(item), "embedding_worker_pool_enabled:%d", info->embedding_worker_pool_enabled);
    if (n < 0 || n >= (int)sizeof(item)) {
        return resp_generate_simple_string_n(response, '-', "ERR response too large", sizeof("ERR response too large") - 1);
    }
    off += resp_generate_bulk_string_n(response + off, item, (size_t)n);

    n = snprintf(item, sizeof(item), "embedding_worker_threads:%d", info->embedding_worker_threads);
    if (n < 0 || n >= (int)sizeof(item)) {
        return resp_generate_simple_string_n(response, '-', "ERR response too large", sizeof("ERR response too large") - 1);
    }
    off += resp_generate_bulk_string_n(response + off, item, (size_t)n);

    n = snprintf(item, sizeof(item), "embedding_worker_submitted:%llu", (unsigned long long)info->embedding_worker_submitted);
    if (n < 0 || n >= (int)sizeof(item)) {
        return resp_generate_simple_string_n(response, '-', "ERR response too large", sizeof("ERR response too large") - 1);
    }
    off += resp_generate_bulk_string_n(response + off, item, (size_t)n);

    n = snprintf(item, sizeof(item), "embedding_worker_completed:%llu", (unsigned long long)info->embedding_worker_completed);
    if (n < 0 || n >= (int)sizeof(item)) {
        return resp_generate_simple_string_n(response, '-', "ERR response too large", sizeof("ERR response too large") - 1);
    }
    off += resp_generate_bulk_string_n(response + off, item, (size_t)n);

    n = snprintf(item, sizeof(item), "embedding_worker_failed:%llu", (unsigned long long)info->embedding_worker_failed);
    if (n < 0 || n >= (int)sizeof(item)) {
        return resp_generate_simple_string_n(response, '-', "ERR response too large", sizeof("ERR response too large") - 1);
    }
    off += resp_generate_bulk_string_n(response + off, item, (size_t)n);

    n = snprintf(item, sizeof(item), "embedding_worker_queue_depth:%llu", (unsigned long long)info->embedding_worker_queue_depth);
    if (n < 0 || n >= (int)sizeof(item)) {
        return resp_generate_simple_string_n(response, '-', "ERR response too large", sizeof("ERR response too large") - 1);
    }
    off += resp_generate_bulk_string_n(response + off, item, (size_t)n);

    n = snprintf(item, sizeof(item), "embedding_worker_max_queue_depth:%llu", (unsigned long long)info->embedding_worker_max_queue_depth);
    if (n < 0 || n >= (int)sizeof(item)) {
        return resp_generate_simple_string_n(response, '-', "ERR response too large", sizeof("ERR response too large") - 1);
    }
    off += resp_generate_bulk_string_n(response + off, item, (size_t)n);

    n = snprintf(item, sizeof(item), "executor_prepare_task_submitted:%llu", (unsigned long long)info->executor_prepare_task_submitted);
    if (n < 0 || n >= (int)sizeof(item)) {
        return resp_generate_simple_string_n(response, '-', "ERR response too large", sizeof("ERR response too large") - 1);
    }
    off += resp_generate_bulk_string_n(response + off, item, (size_t)n);

    n = snprintf(item, sizeof(item), "executor_prepared_task_executed:%llu", (unsigned long long)info->executor_prepared_task_executed);
    if (n < 0 || n >= (int)sizeof(item)) {
        return resp_generate_simple_string_n(response, '-', "ERR response too large", sizeof("ERR response too large") - 1);
    }
    off += resp_generate_bulk_string_n(response + off, item, (size_t)n);

    n = snprintf(item, sizeof(item), "executor_prepared_task_failed:%llu", (unsigned long long)info->executor_prepared_task_failed);
    if (n < 0 || n >= (int)sizeof(item)) {
        return resp_generate_simple_string_n(response, '-', "ERR response too large", sizeof("ERR response too large") - 1);
    }
    off += resp_generate_bulk_string_n(response + off, item, (size_t)n);

    n = snprintf(item, sizeof(item), "vector_backend:%s",
        info->index_type == KVS_VECTOR_INDEX_FAISS_HNSW ? "faiss_hnsw" :
        (info->index_type == KVS_VECTOR_INDEX_FAISS_FLAT ? "faiss_flat" :
         (info->index_type == KVS_VECTOR_INDEX_HNSWLIB_HNSW ? "hnswlib_hnsw" :
          (info->index_type == KVS_VECTOR_INDEX_TYPE_SQ8_FLAT ? "native_sq8_flat" :
           (info->index_type == KVS_VECTOR_INDEX_CORE_FLAT ? "core_flat" :
            (info->index_type == KVS_VECTOR_INDEX_CORE_HNSW ? "core_hnsw" : "native_flat"))))));
    if (n < 0 || n >= (int)sizeof(item)) {
        return resp_generate_simple_string_n(response, '-', "ERR response too large", sizeof("ERR response too large") - 1);
    }
    off += resp_generate_bulk_string_n(response + off, item, (size_t)n);

    n = snprintf(item, sizeof(item), "faiss_backend_enabled:%d", info->faiss_backend_enabled);
    if (n < 0 || n >= (int)sizeof(item)) {
        return resp_generate_simple_string_n(response, '-', "ERR response too large", sizeof("ERR response too large") - 1);
    }
    off += resp_generate_bulk_string_n(response + off, item, (size_t)n);

    n = snprintf(item, sizeof(item), "faiss_ntotal:%zu", info->faiss_ntotal);
    if (n < 0 || n >= (int)sizeof(item)) {
        return resp_generate_simple_string_n(response, '-', "ERR response too large", sizeof("ERR response too large") - 1);
    }
    off += resp_generate_bulk_string_n(response + off, item, (size_t)n);

    n = snprintf(item, sizeof(item), "faiss_add_count:%llu", (unsigned long long)info->faiss_add_count);
    if (n < 0 || n >= (int)sizeof(item)) {
        return resp_generate_simple_string_n(response, '-', "ERR response too large", sizeof("ERR response too large") - 1);
    }
    off += resp_generate_bulk_string_n(response + off, item, (size_t)n);

    n = snprintf(item, sizeof(item), "faiss_search_count:%llu", (unsigned long long)info->faiss_search_count);
    if (n < 0 || n >= (int)sizeof(item)) {
        return resp_generate_simple_string_n(response, '-', "ERR response too large", sizeof("ERR response too large") - 1);
    }
    off += resp_generate_bulk_string_n(response + off, item, (size_t)n);

    n = snprintf(item, sizeof(item), "hnswlib_backend_enabled:%d", info->hnswlib_backend_enabled);
    if (n < 0 || n >= (int)sizeof(item)) {
        return resp_generate_simple_string_n(response, '-', "ERR response too large", sizeof("ERR response too large") - 1);
    }
    off += resp_generate_bulk_string_n(response + off, item, (size_t)n);

    n = snprintf(item, sizeof(item), "hnswlib_ntotal:%zu", info->hnswlib_ntotal);
    if (n < 0 || n >= (int)sizeof(item)) {
        return resp_generate_simple_string_n(response, '-', "ERR response too large", sizeof("ERR response too large") - 1);
    }
    off += resp_generate_bulk_string_n(response + off, item, (size_t)n);

    n = snprintf(item, sizeof(item), "hnswlib_capacity:%zu", info->hnswlib_capacity);
    if (n < 0 || n >= (int)sizeof(item)) {
        return resp_generate_simple_string_n(response, '-', "ERR response too large", sizeof("ERR response too large") - 1);
    }
    off += resp_generate_bulk_string_n(response + off, item, (size_t)n);

    n = snprintf(item, sizeof(item), "hnswlib_m:%zu", info->hnswlib_m);
    if (n < 0 || n >= (int)sizeof(item)) {
        return resp_generate_simple_string_n(response, '-', "ERR response too large", sizeof("ERR response too large") - 1);
    }
    off += resp_generate_bulk_string_n(response + off, item, (size_t)n);

    n = snprintf(item, sizeof(item), "hnswlib_ef_construction:%zu", info->hnswlib_ef_construction);
    if (n < 0 || n >= (int)sizeof(item)) {
        return resp_generate_simple_string_n(response, '-', "ERR response too large", sizeof("ERR response too large") - 1);
    }
    off += resp_generate_bulk_string_n(response + off, item, (size_t)n);

    n = snprintf(item, sizeof(item), "hnswlib_ef_search:%zu", info->hnswlib_ef_search);
    if (n < 0 || n >= (int)sizeof(item)) {
        return resp_generate_simple_string_n(response, '-', "ERR response too large", sizeof("ERR response too large") - 1);
    }
    off += resp_generate_bulk_string_n(response + off, item, (size_t)n);

    n = snprintf(item, sizeof(item), "hnswlib_add_count:%llu", (unsigned long long)info->hnswlib_add_count);
    if (n < 0 || n >= (int)sizeof(item)) {
        return resp_generate_simple_string_n(response, '-', "ERR response too large", sizeof("ERR response too large") - 1);
    }
    off += resp_generate_bulk_string_n(response + off, item, (size_t)n);

    n = snprintf(item, sizeof(item), "hnswlib_search_count:%llu", (unsigned long long)info->hnswlib_search_count);
    if (n < 0 || n >= (int)sizeof(item)) {
        return resp_generate_simple_string_n(response, '-', "ERR response too large", sizeof("ERR response too large") - 1);
    }
    off += resp_generate_bulk_string_n(response + off, item, (size_t)n);

    n = snprintf(item, sizeof(item), "vsearch_total_count:%llu", (unsigned long long)info->vsearch_total_count);
    if (n < 0 || n >= (int)sizeof(item)) {
        return resp_generate_simple_string_n(response, '-', "ERR response too large", sizeof("ERR response too large") - 1);
    }
    off += resp_generate_bulk_string_n(response + off, item, (size_t)n);

    n = snprintf(item, sizeof(item), "vsearch_total_avg_us:%llu",
        (unsigned long long)(info->vsearch_total_count ? info->vsearch_total_us / info->vsearch_total_count : 0));
    if (n < 0 || n >= (int)sizeof(item)) {
        return resp_generate_simple_string_n(response, '-', "ERR response too large", sizeof("ERR response too large") - 1);
    }
    off += resp_generate_bulk_string_n(response + off, item, (size_t)n);

    n = snprintf(item, sizeof(item), "vsearch_total_us:%llu", (unsigned long long)info->vsearch_total_us);
    if (n < 0 || n >= (int)sizeof(item)) {
        return resp_generate_simple_string_n(response, '-', "ERR response too large", sizeof("ERR response too large") - 1);
    }
    off += resp_generate_bulk_string_n(response + off, item, (size_t)n);

    n = snprintf(item, sizeof(item), "vsearch_total_max_us:%llu", (unsigned long long)info->vsearch_total_max_us);
    if (n < 0 || n >= (int)sizeof(item)) {
        return resp_generate_simple_string_n(response, '-', "ERR response too large", sizeof("ERR response too large") - 1);
    }
    off += resp_generate_bulk_string_n(response + off, item, (size_t)n);

    n = snprintf(item, sizeof(item), "vsearch_backend_search_count:%llu", (unsigned long long)info->vsearch_backend_search_count);
    if (n < 0 || n >= (int)sizeof(item)) {
        return resp_generate_simple_string_n(response, '-', "ERR response too large", sizeof("ERR response too large") - 1);
    }
    off += resp_generate_bulk_string_n(response + off, item, (size_t)n);

    n = snprintf(item, sizeof(item), "vsearch_backend_search_avg_us:%llu",
        (unsigned long long)(info->vsearch_backend_search_count ? info->vsearch_backend_search_total_us / info->vsearch_backend_search_count : 0));
    if (n < 0 || n >= (int)sizeof(item)) {
        return resp_generate_simple_string_n(response, '-', "ERR response too large", sizeof("ERR response too large") - 1);
    }
    off += resp_generate_bulk_string_n(response + off, item, (size_t)n);

    n = snprintf(item, sizeof(item), "vsearch_backend_search_total_us:%llu", (unsigned long long)info->vsearch_backend_search_total_us);
    if (n < 0 || n >= (int)sizeof(item)) {
        return resp_generate_simple_string_n(response, '-', "ERR response too large", sizeof("ERR response too large") - 1);
    }
    off += resp_generate_bulk_string_n(response + off, item, (size_t)n);

    n = snprintf(item, sizeof(item), "vsearch_backend_search_max_us:%llu", (unsigned long long)info->vsearch_backend_search_max_us);
    if (n < 0 || n >= (int)sizeof(item)) {
        return resp_generate_simple_string_n(response, '-', "ERR response too large", sizeof("ERR response too large") - 1);
    }
    off += resp_generate_bulk_string_n(response + off, item, (size_t)n);

    n = snprintf(item, sizeof(item), "vsearch_manager_resolve_count:%llu", (unsigned long long)info->vsearch_manager_resolve_count);
    if (n < 0 || n >= (int)sizeof(item)) {
        return resp_generate_simple_string_n(response, '-', "ERR response too large", sizeof("ERR response too large") - 1);
    }
    off += resp_generate_bulk_string_n(response + off, item, (size_t)n);

    n = snprintf(item, sizeof(item), "vsearch_manager_resolve_avg_us:%llu",
        (unsigned long long)(info->vsearch_manager_resolve_count ? info->vsearch_manager_resolve_total_us / info->vsearch_manager_resolve_count : 0));
    if (n < 0 || n >= (int)sizeof(item)) {
        return resp_generate_simple_string_n(response, '-', "ERR response too large", sizeof("ERR response too large") - 1);
    }
    off += resp_generate_bulk_string_n(response + off, item, (size_t)n);

    n = snprintf(item, sizeof(item), "vsearch_manager_resolve_total_us:%llu", (unsigned long long)info->vsearch_manager_resolve_total_us);
    if (n < 0 || n >= (int)sizeof(item)) {
        return resp_generate_simple_string_n(response, '-', "ERR response too large", sizeof("ERR response too large") - 1);
    }
    off += resp_generate_bulk_string_n(response + off, item, (size_t)n);

    n = snprintf(item, sizeof(item), "vsearch_manager_resolve_max_us:%llu", (unsigned long long)info->vsearch_manager_resolve_max_us);
    if (n < 0 || n >= (int)sizeof(item)) {
        return resp_generate_simple_string_n(response, '-', "ERR response too large", sizeof("ERR response too large") - 1);
    }
    off += resp_generate_bulk_string_n(response + off, item, (size_t)n);

    n = snprintf(item, sizeof(item), "vsearch_response_assembly_count:%llu", (unsigned long long)info->vsearch_response_assembly_count);
    if (n < 0 || n >= (int)sizeof(item)) {
        return resp_generate_simple_string_n(response, '-', "ERR response too large", sizeof("ERR response too large") - 1);
    }
    off += resp_generate_bulk_string_n(response + off, item, (size_t)n);

    n = snprintf(item, sizeof(item), "vsearch_response_assembly_avg_us:%llu",
        (unsigned long long)(info->vsearch_response_assembly_count ? info->vsearch_response_assembly_total_us / info->vsearch_response_assembly_count : 0));
    if (n < 0 || n >= (int)sizeof(item)) {
        return resp_generate_simple_string_n(response, '-', "ERR response too large", sizeof("ERR response too large") - 1);
    }
    off += resp_generate_bulk_string_n(response + off, item, (size_t)n);

    n = snprintf(item, sizeof(item), "vsearch_response_assembly_total_us:%llu", (unsigned long long)info->vsearch_response_assembly_total_us);
    if (n < 0 || n >= (int)sizeof(item)) {
        return resp_generate_simple_string_n(response, '-', "ERR response too large", sizeof("ERR response too large") - 1);
    }
    off += resp_generate_bulk_string_n(response + off, item, (size_t)n);

    n = snprintf(item, sizeof(item), "vsearch_response_assembly_max_us:%llu", (unsigned long long)info->vsearch_response_assembly_max_us);
    if (n < 0 || n >= (int)sizeof(item)) {
        return resp_generate_simple_string_n(response, '-', "ERR response too large", sizeof("ERR response too large") - 1);
    }
    off += resp_generate_bulk_string_n(response + off, item, (size_t)n);

    n = snprintf(item, sizeof(item), "vsearch_raw_hit_count:%llu", (unsigned long long)info->vsearch_raw_hit_count);
    if (n < 0 || n >= (int)sizeof(item)) {
        return resp_generate_simple_string_n(response, '-', "ERR response too large", sizeof("ERR response too large") - 1);
    }
    off += resp_generate_bulk_string_n(response + off, item, (size_t)n);

    n = snprintf(item, sizeof(item), "vsearch_resolved_hit_count:%llu", (unsigned long long)info->vsearch_resolved_hit_count);
    if (n < 0 || n >= (int)sizeof(item)) {
        return resp_generate_simple_string_n(response, '-', "ERR response too large", sizeof("ERR response too large") - 1);
    }
    off += resp_generate_bulk_string_n(response + off, item, (size_t)n);

    n = snprintf(item, sizeof(item), "vsearch_filtered_hit_count:%llu", (unsigned long long)info->vsearch_filtered_hit_count);
    if (n < 0 || n >= (int)sizeof(item)) {
        return resp_generate_simple_string_n(response, '-', "ERR response too large", sizeof("ERR response too large") - 1);
    }
    off += resp_generate_bulk_string_n(response + off, item, (size_t)n);

    n = snprintf(item, sizeof(item), "vsearch_legacy_search_count:%llu", (unsigned long long)info->vsearch_total_count);
    if (n < 0 || n >= (int)sizeof(item)) {
        return resp_generate_simple_string_n(response, '-', "ERR response too large", sizeof("ERR response too large") - 1);
    }
    off += resp_generate_bulk_string_n(response + off, item, (size_t)n);

    n = snprintf(item, sizeof(item), "vsearch_legacy_search_avg_us:%llu",
        (unsigned long long)(info->vsearch_total_count ? info->vsearch_total_us / info->vsearch_total_count : 0));
    if (n < 0 || n >= (int)sizeof(item)) {
        return resp_generate_simple_string_n(response, '-', "ERR response too large", sizeof("ERR response too large") - 1);
    }
    off += resp_generate_bulk_string_n(response + off, item, (size_t)n);

    n = snprintf(item, sizeof(item), "vsearch_legacy_response_build_count:%llu", (unsigned long long)info->vsearch_response_assembly_count);
    if (n < 0 || n >= (int)sizeof(item)) {
        return resp_generate_simple_string_n(response, '-', "ERR response too large", sizeof("ERR response too large") - 1);
    }
    off += resp_generate_bulk_string_n(response + off, item, (size_t)n);

    n = snprintf(item, sizeof(item), "vsearch_legacy_response_build_avg_us:%llu",
        (unsigned long long)(info->vsearch_response_assembly_count ? info->vsearch_response_assembly_total_us / info->vsearch_response_assembly_count : 0));
    if (n < 0 || n >= (int)sizeof(item)) {
        return resp_generate_simple_string_n(response, '-', "ERR response too large", sizeof("ERR response too large") - 1);
    }
    off += resp_generate_bulk_string_n(response + off, item, (size_t)n);

    return off;
}

int handle_vset_command(command_context_t *ctx, char *response) {
    int status;
    kvs_vector_set_prepared_t prepared;

    if (!ctx || !ctx->query_text || !ctx->value) {
        return handle_vector_status_response(KVS_VECTOR_ERR_INVALID_ARG, response);
    }

    kvs_vector_set_prepared_reset(&prepared);

    status = kvs_vector_set_prepare(
        ctx->query_text,
        ctx->query_text_len,
        ctx->value,
        strlen(ctx->value),
        ctx->scope,
        ctx->scope_len,
        &prepared
    );
    if (status != KVS_VECTOR_OK) {
        /* prepare cleans up internally on failure */
        return handle_vector_status_response(status, response);
    }

    status = kvs_vector_set_commit_prepared(&prepared);
    /*
     * commit_prepared always consumes the prepared object:
     *   - on success: take_prepared_fields transfers ownership
     *   - on failure: commit_prepared destroys prepared internally
     * Caller must NOT call kvs_vector_set_prepared_destroy after commit.
     */
    return handle_vector_status_response(status, response);
}

int handle_vget_command(command_context_t *ctx, char *response) {
    int status;
    kvs_vector_search_result_t result;

    if (!ctx || !ctx->query_text) {
        return handle_vector_status_response(KVS_VECTOR_ERR_INVALID_ARG, response);
    }

    kvs_vector_search_result_reset(&result);

    status = kvs_vector_get(
        ctx->query_text,
        ctx->query_text_len,
        ctx->scope,
        ctx->scope_len,
        &result
    );

    if (status == KVS_VECTOR_OK) {
        if (!result.answer) {
            return handle_vector_status_response(KVS_VECTOR_ERR_NOT_FOUND, response);
        }

        return resp_generate_bulk_string_n(
            response,
            result.answer,
            result.answer_len
        );
    }

    return handle_vector_status_response(status, response);
}

int handle_vsearch_command(command_context_t *ctx, char *response) {
	int status;
	int topk;
	size_t results_capacity;
	int off = 0;
	int n;
	kvs_vector_search_prepared_t prepared;
	kvs_vector_search_result_t *results = NULL;
	size_t results_count = 0;
	size_t i;
	float threshold;
	uint64_t response_start_us;
	uint64_t response_end_us;
	uint64_t total_start_us;

	if (!ctx) {
		return handle_vector_status_response(KVS_VECTOR_ERR_INVALID_ARG, response);
	}

	if (!ctx->query_text) {
		return handle_vector_status_response(KVS_VECTOR_ERR_INVALID_ARG, response);
	}

	topk = ctx->topk;
	if (topk <= 0) {
		topk = 1;
	}

	if (topk > 1024) {
		return handle_vector_status_response(KVS_VECTOR_ERR_INVALID_ARG, response);
	}

	results_capacity = (size_t)topk;

	/* VSEARCH BATCH removed */

	/*
	 * Stage 2A: synchronous two-stage search path.
	 * Prepare generates the query embedding (may hit embedding cache).
	 * search_with_prepared runs ANN search against the current index.
	 * Response format is unchanged from the original single-invocation path.
	 */

	kvs_vector_search_prepared_reset(&prepared);

	status = kvs_vector_search_prepare(
		ctx->query_text,
		ctx->query_text_len,
		ctx->scope,
		ctx->scope_len,
		&prepared
	);
	if (status != KVS_VECTOR_OK) {
		/* prepare cleans up internally on failure */
		return handle_vector_status_response(status, response);
	}

	total_start_us = kvs_operation_now_us();
	results = (kvs_vector_search_result_t *)kvs_malloc(
		results_capacity * sizeof(kvs_vector_search_result_t)
	);
	if (!results) {
		kvs_vector_search_prepared_destroy(&prepared);
		return handle_vector_status_response(KVS_VECTOR_ERR, response);
	}

	kvs_vector_search_results_reset(results, results_capacity);

	threshold = ctx->has_threshold ? ctx->threshold : g_cfg.vector_score_threshold;

	status = kvs_vector_search_with_prepared(
		&prepared,
		topk,
		threshold,
		results,
		results_capacity,
		&results_count
	);

	/*
	 * search_with_prepared takes a const pointer — it does NOT consume
	 * the prepared object.  Caller must destroy prepared after search.
	 */
	kvs_vector_search_prepared_destroy(&prepared);

	if (status != KVS_VECTOR_OK) {
		kvs_free(results);
		if (status == KVS_VECTOR_ERR_NOT_INITIALIZED) {
			kvs_vector_info_t info;
			if (kvs_vector_info(&info) == KVS_VECTOR_OK && info.index_type == 2) {
				return handle_vector_status_response(status, response);
			}
		}
		return handle_vector_status_response(status, response);
	}

	response_start_us = kvs_operation_now_us();
	off += resp_generate_array_start(response + off, (int)results_count);

	for (i = 0; i < results_count; ++i) {
		char *item = NULL;
		int item_cap;

		item_cap = snprintf(
			NULL, 0,
			"score:%.6f|scope:%.*s|question:%.*s|answer:%.*s",
			results[i].score,
			(int)results[i].scope_len,
			results[i].scope ? results[i].scope : "",
			(int)results[i].question_len,
			results[i].question ? results[i].question : "",
			(int)results[i].answer_len,
			results[i].answer ? results[i].answer : ""
		);

		if (item_cap < 0) {
			kvs_free(results);
			return resp_generate_simple_string_n(
				response,
				'-',
				"ERR response too large",
				sizeof("ERR response too large") - 1
			);
		}

		item_cap += 1; /* NUL terminator */
		item = (char *)kvs_malloc((size_t)item_cap);
		if (!item) {
			kvs_free(results);
			return resp_generate_simple_string_n(
				response,
				'-',
				"ERR out of memory",
				sizeof("ERR out of memory") - 1
			);
		}

		n = snprintf(
			item, (size_t)item_cap,
			"score:%.6f|scope:%.*s|question:%.*s|answer:%.*s",
			results[i].score,
			(int)results[i].scope_len,
			results[i].scope ? results[i].scope : "",
			(int)results[i].question_len,
			results[i].question ? results[i].question : "",
			(int)results[i].answer_len,
			results[i].answer ? results[i].answer : ""
		);

		if (n < 0 || n >= item_cap) {
			kvs_free(item);
			kvs_free(results);
			return resp_generate_simple_string_n(
				response,
				'-',
				"ERR response too large",
				sizeof("ERR response too large") - 1
			);
		}

		off += resp_generate_bulk_string_n(response + off, item, (size_t)n);
		kvs_free(item);
	}
	response_end_us = kvs_operation_now_us();
	kvs_vector_vsearch_record_response_build(
		response_end_us >= response_start_us ? response_end_us - response_start_us : 0
	);
	kvs_vector_vsearch_record_search(
		response_end_us >= total_start_us ? response_end_us - total_start_us : 0
	);

	kvs_free(results);
	return off;
}

int handle_vdel_command(command_context_t *ctx, char *response) {
    int status;

    if (!ctx || !ctx->query_text) {
        return handle_vector_status_response(KVS_VECTOR_ERR_INVALID_ARG, response);
    }

    status = kvs_vector_del(
        ctx->query_text,
        ctx->query_text_len,
        ctx->scope,
        ctx->scope_len
    );

    return handle_vector_status_response(status, response);
}

int handle_vindex_command(command_context_t *ctx, char *response) {
    int status;

    if (!ctx || !ctx->subcommand) {
        return handle_vector_status_response(KVS_VECTOR_ERR_INVALID_ARG, response);
    }

    if (kvs_vcmd_is_rebuild(ctx->subcommand)) {
        status = kvs_vector_index_rebuild();
        return handle_vector_status_response(status, response);
    }

    return handle_vector_status_response(KVS_VECTOR_ERR_INVALID_ARG, response);
}

int handle_vinfo_command(command_context_t *ctx, char *response) {
    int status;
    kvs_vector_info_t info;
    kvs_embedding_worker_pool_stats_t wp_stats;
    kvs_command_executor_stats_t exec_stats;

    (void)ctx;

    memset(&info, 0, sizeof(info));
    memset(&wp_stats, 0, sizeof(wp_stats));
    memset(&exec_stats, 0, sizeof(exec_stats));

    status = kvs_vector_info(&info);
    if (status != KVS_VECTOR_OK) {
        return handle_vector_status_response(status, response);
    }

    kvs_embedding_worker_pool_get_stats(&wp_stats);
    kvs_command_executor_get_stats(&exec_stats);

    info.embedding_worker_pool_enabled = (int)wp_stats.enabled;
    info.embedding_worker_threads  = (int)wp_stats.worker_threads;
    info.embedding_worker_submitted   = wp_stats.submitted_prepare_tasks;
    info.embedding_worker_completed   = wp_stats.completed_prepare_tasks;
    info.embedding_worker_failed      = wp_stats.failed_prepare_tasks;
    info.embedding_worker_queue_depth      = wp_stats.prepare_queue_depth;
    info.embedding_worker_max_queue_depth  = wp_stats.prepare_queue_max_depth;
    info.executor_prepare_task_submitted   = exec_stats.prepare_task_submitted;
    info.executor_prepared_task_executed   = exec_stats.prepared_task_executed;
    info.executor_prepared_task_failed     = exec_stats.prepared_task_failed;

    return handle_vector_info_response(&info, response);
}

#if ENABLE_RBTREE
static void rbtree_dump_cb(rbtree_node *node, void *ud) {
    rbtree_dump_ctx_t *c = (rbtree_dump_ctx_t *)ud;
    if (!c || c->overflow) return;
    if (!node->key || !node->value) return;
    if (kv_is_expired(node->expire_at_ms, c->now_ms)) return;

    if (fulldump_append_cmd3_len(c->resp, c->max_len, c->off,
                                 "RSET", 4,
                                 (const char *)node->key, (int)node->key_len,
                                 (const char *)node->value, (int)node->value_len) < 0) {
        c->overflow = 1;
        return;
    }

    if (node->expire_at_ms >= 0) {
        char expire_buf[32];
        snprintf(expire_buf, sizeof(expire_buf), "%lld", (long long)node->expire_at_ms);
        if (fulldump_append_cmd3_len(c->resp, c->max_len, c->off,
                                     "PEXPIREAT", 9,
                                     (const char *)node->key, (int)node->key_len,
                                     expire_buf, (int)strlen(expire_buf)) < 0) {
            c->overflow = 1;
        }
    }
}
#endif
