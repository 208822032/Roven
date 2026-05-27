#include <stdio.h>
#include <string.h>
#include <stdlib.h>

#include "kvs_api.h"
#include "kvs_hash.h"

// 全局 hash（如有需要）
kvs_hash_t global_hash;

/* BKDR 变种哈希函数 */
unsigned int kvs_hash_key_hash(const char *key) {
    if (!key) return 0;
    unsigned int hash = 0;
    while (*key) {
        hash ^= ((hash << 5) + (unsigned char)(*key) + (hash >> 2));
        key++;
    }
    return hash;
}

/* Binary-safe hash: computes over explicit length */
unsigned int kvs_hash_key_hash_len(const char *key, size_t key_len) {
    if (!key) return 0;
    unsigned int hash = 0;
    for (size_t i = 0; i < key_len; i++) {
        hash ^= ((hash << 5) + (unsigned char)(key[i]) + (hash >> 2));
    }
    return hash;
}

static inline int kvs_hash_min_int(int a, int b) {
    return a < b ? a : b;
}

static int kvs_hash_compare_node_key(const hashnode_t *node, const char *key, int key_len) {
    if (!node || !node->key || !key) return 0;

    int min_len = kvs_hash_min_int((int)node->key_len, key_len);
    int cmp = memcmp(node->key, key, (size_t)min_len);
    if (cmp != 0) return cmp;

    if ((int)node->key_len < key_len) return -1;
    if ((int)node->key_len > key_len) return 1;
    return 0;
}

static int kvs_hash_compare_nodes(const hashnode_t *nodeA, const hashnode_t *nodeB) {
    if (nodeA == nodeB) return 0;
    if (!nodeA) return -1;
    if (!nodeB) return 1;

    int min_len = kvs_hash_min_int((int)nodeA->key_len, (int)nodeB->key_len);
    int cmp = memcmp(nodeA->key, nodeB->key, (size_t)min_len);
    if (cmp != 0) return cmp;

    if (nodeA->key_len < nodeB->key_len) return -1;
    if (nodeA->key_len > nodeB->key_len) return 1;
    return 0;
}

static int kvs_hash_probe_insert_existing(hashnode_t *nodes,
                                          int cap,
                                          const hashnode_t *entry) {
    if (!nodes || !entry || !entry->key || cap <= 0) return -1;

    int idx = (int)(entry->key_hash % (unsigned int)cap);
    for (int step = 0; step < cap; ++step) {
        hashnode_t *slot = &nodes[idx];
        if (slot->used == 0) {
            *slot = *entry;
            slot->used = 1;
            return 0;
        }
        idx++;
        if (idx >= cap) idx = 0;
    }

    return -1;
}

static int kvs_hash_rebuild_same_capacity(kvs_hash_t *hash);
static int kvs_hash_maybe_compact(kvs_hash_t *hash);

static int kvs_hash_store_new_entry(hashnode_t *slot,
                                    char *key,
                                    char *value,
                                    unsigned int key_hash,
                                    unsigned short key_len,
                                    unsigned int value_len,
                                    int is_tombstone) {
    if (!slot || !key || !value) return -1;

    char *kcopy = kvs_malloc((size_t)key_len + 1);
    char *vcopy = kvs_malloc((size_t)value_len + 1);
    if (!kcopy || !vcopy) {
        if (kcopy) kvs_free(kcopy);
        if (vcopy) kvs_free(vcopy);
        return -1;
    }

    memcpy(kcopy, key, (size_t)key_len + 1);
    memcpy(vcopy, value, (size_t)value_len + 1);

    slot->used = 1;
    slot->key_len = key_len;
    slot->value_len = value_len;
    slot->key_hash = key_hash;
    slot->key = kcopy;
    slot->value = vcopy;
    slot->expire_at_ms = KV_TTL_NO_EXPIRE;

    (void)is_tombstone;
    return 0;
}

static int kvs_hash_update_value(hashnode_t *slot, char *value, unsigned int value_len) {
    if (!slot || !value) return -1;

    if (slot->value && slot->value_len == value_len) {
        memcpy(slot->value, value, (size_t)value_len + 1);
        slot->value_len = value_len;
        slot->expire_at_ms = KV_TTL_NO_EXPIRE;
        return 0;
    }

    char *vcopy = kvs_malloc((size_t)value_len + 1);
    if (!vcopy) return -1;
    memcpy(vcopy, value, (size_t)value_len + 1);

    if (slot->value) {
        kvs_free(slot->value);
    }
    slot->value = vcopy;
    slot->value_len = value_len;
    slot->expire_at_ms = KV_TTL_NO_EXPIRE;
    return 0;
}

static int kvs_hash_delete_at_internal(kvs_hash_t *hash, int idx) {
    if (!hash || !hash->nodes || idx < 0 || idx >= hash->max_slots) return -2;

    hashnode_t *slot = &hash->nodes[idx];
    if (slot->used != 1) return -1;

    if (slot->key) {
        kvs_free(slot->key);
        slot->key = NULL;
    }
    if (slot->value) {
        kvs_free(slot->value);
        slot->value = NULL;
    }

    slot->key_len = 0;
    slot->value_len = 0;
    slot->key_hash = 0;
    slot->expire_at_ms = KV_TTL_NO_EXPIRE;
    slot->used = 2;

    hash->count--;
    hash->tombstones++;
    return 0;
}

static int kvs_hash_lookup_index_impl(kvs_hash_t *hash,
                                      const char *key,
                                      unsigned int key_hash,
                                      int key_len,
                                      int *idx_out) {
    if (!hash || !hash->nodes || !key || !idx_out || hash->max_slots <= 0) return -1;

    int cap = hash->max_slots;
    int idx = (int)(key_hash % (unsigned int)cap);

    for (int step = 0; step < cap; ++step) {
        hashnode_t *slot = &hash->nodes[idx];
        if (slot->used == 0) {
            return -1;
        }

        if (slot->used == 1 &&
            slot->key_hash == key_hash &&
            slot->key_len == (unsigned short)key_len &&
            slot->key && kvs_hash_compare_node_key(slot, key, key_len) == 0) {
            *idx_out = idx;
            return 0;
        }

        idx++;
        if (idx >= cap) idx = 0;
    }

    return -1;
}

int kvs_hash_lookup_index(kvs_hash_t *hash, const char *key, int *idx_out) {
    if (!hash || !key || !idx_out) return -1;

    int key_len = (int)strlen(key);
    if (key_len <= 0) return -1;

    unsigned int key_hash = kvs_hash_key_hash(key);
    return kvs_hash_lookup_index_impl(hash, key, key_hash, key_len, idx_out);
}

int kvs_hash_lookup_index_len(kvs_hash_t *hash, const char *key, size_t key_len, int *idx_out) {
    if (!hash || !key || !idx_out) return -1;
    if (key_len == 0) return -1;

    unsigned int key_hash = kvs_hash_key_hash_len(key, key_len);
    return kvs_hash_lookup_index_impl(hash, key, key_hash, (int)key_len, idx_out);
}

static int kvs_hash_rebuild_same_capacity(kvs_hash_t *hash) {
    if (!hash || !hash->nodes || hash->max_slots <= 0) return -1;

    if (hash->count <= 0) {
        memset(hash->nodes, 0, sizeof(hashnode_t) * (size_t)hash->max_slots);
        for (int i = 0; i < hash->max_slots; ++i) {
            hash->nodes[i].expire_at_ms = KV_TTL_NO_EXPIRE;
        }
        hash->tombstones = 0;
        return 0;
    }

    size_t sz = sizeof(hashnode_t) * (size_t)hash->max_slots;
    hashnode_t *new_nodes = (hashnode_t *)kvs_malloc(sz);
    if (!new_nodes) return -1;
    memset(new_nodes, 0, sz);
    for (int i = 0; i < hash->max_slots; ++i) {
        new_nodes[i].expire_at_ms = KV_TTL_NO_EXPIRE;
    }

    for (int i = 0; i < hash->max_slots; ++i) {
        hashnode_t *slot = &hash->nodes[i];
        if (slot->used != 1 || !slot->key) continue;
        if (kvs_hash_probe_insert_existing(new_nodes, hash->max_slots, slot) != 0) {
            kvs_free(new_nodes);
            return -1;
        }
    }

    kvs_free(hash->nodes);
    hash->nodes = new_nodes;
    hash->tombstones = 0;
    return 0;
}

static int kvs_hash_maybe_compact(kvs_hash_t *hash) {
    if (!hash || !hash->nodes || hash->max_slots <= 0) return -1;

    if (hash->count <= 0) {
        memset(hash->nodes, 0, sizeof(hashnode_t) * (size_t)hash->max_slots);
        for (int i = 0; i < hash->max_slots; ++i) {
            hash->nodes[i].expire_at_ms = KV_TTL_NO_EXPIRE;
        }
        hash->tombstones = 0;
        return 0;
    }

    if (hash->tombstones < 64 || hash->tombstones <= hash->count) {
        return 0;
    }

    return kvs_hash_rebuild_same_capacity(hash);
}

/* 创建哈希表：分配 max_slots 个槽位并清零 */
int kvs_hash_create(kvs_hash_t *hash) {
    if (!hash) return -1;

    hash->max_slots = MAX_TABLE_SIZE;
    hash->count     = 0;
    hash->tombstones = 0;

    size_t sz = sizeof(hashnode_t) * (size_t)hash->max_slots;
    hash->nodes = (hashnode_t *)kvs_malloc(sz);
    if (!hash->nodes) return -1;

    memset(hash->nodes, 0, sz);
    for (int i = 0; i < hash->max_slots; ++i) {
        hash->nodes[i].used = 0;
        hash->nodes[i].key_len = 0;
        hash->nodes[i].value_len = 0;
        hash->nodes[i].key_hash = 0;
        hash->nodes[i].key = NULL;
        hash->nodes[i].value = NULL;
        hash->nodes[i].expire_at_ms = KV_TTL_NO_EXPIRE;
    }
    return 0;
}

/* 销毁哈希表 */
void kvs_hash_destory(kvs_hash_t *hash) {
    if (!hash || !hash->nodes) return;

    // 释放每个槽位的 key/value
    for (int i = 0; i < hash->max_slots; ++i) {
        hashnode_t *slot = &hash->nodes[i];
        if (slot->used != 0) {
            if (slot->key) kvs_free(slot->key);
            if (slot->value) kvs_free(slot->value);
            slot->key = NULL;
            slot->value = NULL;
        }
    }
    kvs_free(hash->nodes);
    hash->nodes     = NULL;
    hash->max_slots = 0;
    hash->count     = 0;
    hash->tombstones = 0;
}

/* 内部辅助：查找 key 所在槽位 idx，找不到返回 -1 */
static int _find_idx(kvs_hash_t *hash, const char *key) {
    int idx = -1;
    if (kvs_hash_lookup_index(hash, key, &idx) == 0) {
        return idx;
    }
    return -1;
}

static void kvs_hash_reap_expired(kvs_hash_t *inst, int64_t now_ms) {
    if (!inst || !inst->nodes) return;

    int expired_count = 0;
    for (int i = 0; i < inst->max_slots; ++i) {
        hashnode_t *slot = &inst->nodes[i];
        if (slot->used == 1 && slot->key && kv_is_expired(slot->expire_at_ms, now_ms)) {
            (void)kvs_hash_delete_at_internal(inst, i);
            expired_count++;
        }
    }

    if (expired_count > 0) {
        (void)kvs_hash_maybe_compact(inst);
    }
}

/* 写入：key 不存在 -> 插入；已存在 -> 覆盖并清除 TTL */
int kvs_hash_set(kvs_hash_t *hash, char *key, char *value) {
    if (!hash || !hash->nodes || !key || !value) return -1;

    int key_len = (int)strlen(key);
    int val_len = (int)strlen(value);
    if (key_len <= 0)   return -2;
    if (val_len <  0)   return -3;

    int cap    = hash->max_slots;
    unsigned int h = kvs_hash_key_hash(key);
    int idx    = (int)(h % (unsigned int)cap);

    int first_tombstone = -1;

    for (int step = 0; step < cap; ++step) {
        hashnode_t *slot = &hash->nodes[idx];
        if (slot->used == 0) {
            // 空槽：可插入；如果之前有 tombstone，则复用 tombstone
            if (first_tombstone != -1) {
                slot = &hash->nodes[first_tombstone];
                idx  = first_tombstone;
            }

            if (kvs_hash_store_new_entry(slot,
                                         key,
                                         value,
                                         h,
                                         (unsigned short)key_len,
                                         (unsigned int)val_len,
                                         first_tombstone != -1) != 0) {
                return -4;
            }

            if (first_tombstone != -1) {
                hash->tombstones--;
            }

            hash->count++;
            return 0;
        } else if (slot->used == 1) {
            // 已占用，检查是否同 key
            if (slot->key_hash == h &&
                slot->key_len == (unsigned short)key_len &&
	            memcmp(slot->key, key, (size_t)key_len) == 0) {
                if (kvs_hash_update_value(slot, value, (unsigned int)val_len) != 0) {
                    return -4;
                }
                return 1;
            }
        } else if (slot->used == 2) {
            // tombstone，记录第一个
            if (first_tombstone == -1) first_tombstone = idx;
        }

        idx++;
        if (idx >= cap) idx = 0;
    }

    if (first_tombstone != -1) {
        hashnode_t *slot = &hash->nodes[first_tombstone];
        if (kvs_hash_store_new_entry(slot,
                                     key,
                                     value,
                                     h,
                                     (unsigned short)key_len,
                                     (unsigned int)val_len,
                                     1) != 0) {
            return -4;
        }
        hash->tombstones--;
        hash->count++;
        return 0;
    }

    // 表满或负载过高
    return -4;
}

/* 读：返回内部 value 指针 */
char *kvs_hash_get(kvs_hash_t *hash, char *key) {
    int idx = _find_idx(hash, key);
    if (idx < 0) return NULL;
    return hash->nodes[idx].value;
}

/* 修改：key 存在则覆盖 value；不存在返回 1 */
int kvs_hash_mod(kvs_hash_t *hash, char *key, char *value) {
    if (!hash || !hash->nodes || !key || !value) return -1;

    int idx = _find_idx(hash, key);
    if (idx < 0) return 1;   // 不存在

    hashnode_t *slot = &hash->nodes[idx];
    int val_len = (int)strlen(value);
    if (val_len < 0) return -2;

    if (kvs_hash_update_value(slot, value, (unsigned int)val_len) != 0) return -2;
    return 0;
}

/* 删除：成功返回 0，不存在返回 -1 */
int kvs_hash_del(kvs_hash_t *hash, char *key) {
    if (!hash || !hash->nodes || !key) return -2;

    int idx = _find_idx(hash, key);
    if (idx < 0) return -1;

    int ret = kvs_hash_delete_at_internal(hash, idx);
    if (ret != 0) return ret;

    (void)kvs_hash_maybe_compact(hash);
    return 0;
}

int kvs_hash_del_at(kvs_hash_t *hash, int idx) {
    if (!hash || !hash->nodes) return -2;

    int ret = kvs_hash_delete_at_internal(hash, idx);
    if (ret != 0) return ret;

    (void)kvs_hash_maybe_compact(hash);
    return 0;
}

/* 统计数量 */
int kvs_hash_count(kvs_hash_t *hash) {
    if (!hash) return 0;
    return hash->count;
}

/* 是否存在：0 存在，1 不存在 */
int kvs_hash_exist(kvs_hash_t *hash, char *key) {
    return _find_idx(hash, key) >= 0 ? 0 : 1;
}

/* 排序比较函数（按 key 字典序） */
static int compare_hash_items(const void *a, const void *b) {
    const hashnode_t *nodeA = *(const hashnode_t * const *)a;
    const hashnode_t *nodeB = *(const hashnode_t * const *)b;
    return kvs_hash_compare_nodes(nodeA, nodeB);
}

static hashnode_t **kvs_hash_collect_candidates(kvs_hash_t *inst, int *cnt_out) {
    if (!inst || !inst->nodes || !cnt_out) return NULL;

    int total = inst->count;
    if (total <= 0) {
        *cnt_out = 0;
        return NULL;
    }

    hashnode_t **arr = (hashnode_t **)kvs_malloc((size_t)total * sizeof(hashnode_t *));
    if (!arr) return NULL;

    int cnt = 0;
    for (int i = 0; i < inst->max_slots; ++i) {
        hashnode_t *slot = &inst->nodes[i];
        if (slot->used == 1) {
            arr[cnt++] = slot;
        }
    }

    *cnt_out = cnt;
    return arr;
}

static int kvs_hash_lower_bound(hashnode_t **arr, int cnt, const char *key) {
    if (!arr || cnt <= 0 || !key) return 0;

    int key_len = (int)strlen(key);
    int lo = 0;
    int hi = cnt;
    while (lo < hi) {
        int mid = lo + (hi - lo) / 2;
        if (kvs_hash_compare_node_key(arr[mid], key, key_len) < 0) {
            lo = mid + 1;
        } else {
            hi = mid;
        }
    }
    return lo;
}

static int kvs_hash_upper_bound(hashnode_t **arr, int cnt, const char *key) {
    if (!arr || cnt <= 0 || !key) return 0;

    int key_len = (int)strlen(key);
    int lo = 0;
    int hi = cnt;
    while (lo < hi) {
        int mid = lo + (hi - lo) / 2;
        if (kvs_hash_compare_node_key(arr[mid], key, key_len) <= 0) {
            lo = mid + 1;
        } else {
            hi = mid;
        }
    }
    return lo;
}

/* 排序输出：order 0=升序，1=降序；limit 控制最多输出条数 */
int kvs_hash_sort(kvs_hash_t *inst, int order, int limit,
                  char *response, int max_len) {
    if (!inst || !inst->nodes) return -1;

    kvs_hash_reap_expired(inst, kv_now_ms());

    int cnt = 0;
    hashnode_t **arr = kvs_hash_collect_candidates(inst, &cnt);
    if (!arr || cnt <= 0) {
        if (arr) kvs_free(arr);
        return 0;
    }

    qsort(arr, cnt, sizeof(hashnode_t *), compare_hash_items);

    int start = 0, end = cnt, step = 1;
    if (order == 1) {
        start = cnt - 1;
        end   = -1;
        step  = -1;
    }

    int out_limit = (limit > 0 && limit < cnt) ? limit : cnt;
    int len = 0;
    int out_cnt = 0;

    for (int i = start; i != end && out_cnt < out_limit; i += step) {
        hashnode_t *slot = arr[i];
        int n = snprintf(response + len, max_len - len,
                         "%s:%s\r\n", slot->key, slot->value);
        if (n < 0 || len + n >= max_len) break;
        len += n;
        out_cnt++;
    }

    kvs_free(arr);
    return len;
}

/* 范围查询：key 在 [start_key, end_key] 区间内，按升序输出 */
int kvs_hash_range(kvs_hash_t *inst, char *start_key, char *end_key,
                   char *response, int max_len) {
    if (!inst || !inst->nodes || !start_key || !end_key) return -1;

    kvs_hash_reap_expired(inst, kv_now_ms());

    int cnt = 0;
    hashnode_t **arr = kvs_hash_collect_candidates(inst, &cnt);
    if (!arr || cnt <= 0) {
        if (arr) kvs_free(arr);
        return 0;
    }

    qsort(arr, cnt, sizeof(hashnode_t *), compare_hash_items);

    int begin = kvs_hash_lower_bound(arr, cnt, start_key);
    int finish = kvs_hash_upper_bound(arr, cnt, end_key);

    int len = 0;
    for (int i = begin; i < finish; ++i) {
        hashnode_t *slot = arr[i];
        int n = snprintf(response + len, max_len - len,
                         "%s:%s\r\n", slot->key, slot->value);
        if (n < 0 || len + n >= max_len) break;
        len += n;
    }

    kvs_free(arr);
    return len;
}

/* === Binary-safe _len API implementations === */

int kvs_hash_set_len(kvs_hash_t *hash, const char *key, size_t key_len, const char *value, size_t value_len) {
    if (!hash || !hash->nodes || !key || !value) return -1;
    if (key_len <= 0) return -2;

    int cap = hash->max_slots;
    unsigned int h = kvs_hash_key_hash_len(key, key_len);
    int idx = (int)(h % (unsigned int)cap);
    int first_tombstone = -1;

    for (int step = 0; step < cap; ++step) {
        hashnode_t *slot = &hash->nodes[idx];
        if (slot->used == 0) {
            if (first_tombstone != -1) { slot = &hash->nodes[first_tombstone]; idx = first_tombstone; }
            if (kvs_hash_store_new_entry(slot, (char*)key, (char*)value, h,
                                         (unsigned short)key_len, (unsigned int)value_len,
                                         first_tombstone != -1) != 0) return -2;
            if (first_tombstone == -1) hash->count++;
            return 0;
        }
        if (slot->used == 2 && first_tombstone == -1) first_tombstone = idx;
        if (slot->used == 1 && (int)slot->key_len == (int)key_len &&
            slot->key && memcmp(slot->key, key, key_len) == 0) {
            char *new_val = kvs_malloc(value_len + 1);
            if (!new_val) return -2;
            memcpy(new_val, value, value_len + 1);
            if (slot->value) kvs_free(slot->value);
            slot->value = new_val;
            slot->value_len = (unsigned int)value_len;
            return 1;
        }
        idx = (idx + 1) % cap;
    }
    return -1;
}

char* kvs_hash_get_len(kvs_hash_t *hash, const char *key, size_t key_len, size_t *value_len_out) {
    if (!hash || !hash->nodes || !key) { if (value_len_out) *value_len_out = 0; return NULL; }
    int idx = -1;
    if (kvs_hash_lookup_index_len(hash, key, key_len, &idx) != 0) { if (value_len_out) *value_len_out = 0; return NULL; }
    if (value_len_out) *value_len_out = hash->nodes[idx].value_len;
    return hash->nodes[idx].value;
}

int kvs_hash_del_len(kvs_hash_t *hash, const char *key, size_t key_len) {
    if (!hash || !hash->nodes || !key) return -1;
    int idx = -1;
    if (kvs_hash_lookup_index_len(hash, key, key_len, &idx) != 0) return 0;
    return kvs_hash_delete_at_internal(hash, idx);
}

int kvs_hash_mod_len(kvs_hash_t *hash, const char *key, size_t key_len, const char *value, size_t value_len) {
    if (!hash || !hash->nodes || !key || !value) return -1;
    int idx = -1;
    if (kvs_hash_lookup_index_len(hash, key, key_len, &idx) != 0) return 1;
    char *new_val = kvs_malloc(value_len + 1);
    if (!new_val) return -2;
    memcpy(new_val, value, value_len + 1);
    if (hash->nodes[idx].value) kvs_free(hash->nodes[idx].value);
    hash->nodes[idx].value = new_val;
    hash->nodes[idx].value_len = (unsigned int)value_len;
    return 0;
}

int kvs_hash_exist_len(kvs_hash_t *hash, const char *key, size_t key_len) {
    if (!hash || !hash->nodes || !key) return 0;
    int idx = -1;
    return (kvs_hash_lookup_index_len(hash, key, key_len, &idx) == 0) ? 1 : 0;
}
