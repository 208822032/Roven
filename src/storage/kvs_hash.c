#include <stdio.h>
#include <string.h>
#include <stdlib.h>

#include "kvs_api.h"
#include "kvs_hash.h"

// 全局 hash（如有需要）
kvs_hash_t global_hash;

/* BKDR 变种哈希函数 */
static inline unsigned int _hash_str(const char *key) {
    if (!key) return 0;
    unsigned int hash = 0;
    unsigned int seed = 1315423911u;
    while (*key) {
        hash ^= ((hash << 5) + (unsigned char)(*key) + (hash >> 2));
        key++;
    }
    return hash;
}

/* 创建哈希表：分配 max_slots 个槽位并清零 */
int kvs_hash_create(kvs_hash_t *hash) {
    if (!hash) return -1;

    hash->max_slots = MAX_TABLE_SIZE;
    hash->count     = 0;

    size_t sz = sizeof(hashnode_t) * (size_t)hash->max_slots;
    hash->nodes = (hashnode_t *)kvs_malloc(sz);
    if (!hash->nodes) return -1;

    memset(hash->nodes, 0, sz);
    for (int i = 0; i < hash->max_slots; ++i) {
        hash->nodes[i].used = 0;
        hash->nodes[i].klen = 0;
        hash->nodes[i].vlen = 0;
        hash->nodes[i].key = NULL;
        hash->nodes[i].value = NULL;
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
}

/* 内部辅助：查找 key 所在槽位 idx，找不到返回 -1 */
static int _find_idx(kvs_hash_t *hash, const char *key) {
    if (!hash || !hash->nodes || !key) return -1;

    int cap    = hash->max_slots;
    unsigned int h = _hash_str(key);
    int idx    = (int)(h % (unsigned int)cap);
    int key_len = (int)strlen(key);

    for (int step = 0; step < cap; ++step) {
        hashnode_t *slot = &hash->nodes[idx];
        if (slot->used == 0) {
            // 碰到空槽，链断了，肯定不存在
            return -1;
        }
        if (slot->used == 1 &&
            slot->klen == (unsigned char)key_len &&
            memcmp(slot->key, key, (size_t)key_len) == 0) {
            return idx;
        }
        idx++;
        if (idx >= cap) idx = 0;
    }
    return -1;
}

/* 写入：key 不存在 -> 插入；已存在 -> 返回 1（保持现有语义） */
int kvs_hash_set(kvs_hash_t *hash, char *key, char *value) {
    if (!hash || !hash->nodes || !key || !value) return -1;

    int key_len = (int)strlen(key);
    int val_len = (int)strlen(value);
    if (key_len <= 0)   return -2;
    if (val_len <  0)   return -3;

    int cap    = hash->max_slots;
    unsigned int h = _hash_str(key);
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

            char *kcopy = kvs_malloc((size_t)key_len + 1);
            char *vcopy = kvs_malloc((size_t)val_len + 1);
            if (!kcopy || !vcopy) {
                if (kcopy) kvs_free(kcopy);
                if (vcopy) kvs_free(vcopy);
                return -4;
            }
            memcpy(kcopy, key, (size_t)key_len + 1);
            memcpy(vcopy, value, (size_t)val_len + 1);

            slot->used = 1;
            slot->klen = (unsigned short)key_len;
            slot->vlen = (unsigned int)val_len;
            slot->key = kcopy;
            slot->value = vcopy;

            hash->count++;
            return 0;
        } else if (slot->used == 1) {
            // 已占用，检查是否同 key
            if (slot->klen == (unsigned char)key_len &&
	            memcmp(slot->key, key, (size_t)key_len) == 0) {
				// 按现有语义：已存在返回 1，不覆盖
				return 1;
            }
        } else if (slot->used == 2) {
            // tombstone，记录第一个
            if (first_tombstone == -1) first_tombstone = idx;
        }

        idx++;
        if (idx >= cap) idx = 0;
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

    char *vcopy = kvs_malloc((size_t)val_len + 1);
    if (!vcopy) return -2;
    memcpy(vcopy, value, (size_t)val_len + 1);

    if (slot->value) kvs_free(slot->value);
    slot->value = vcopy;
    slot->vlen = (unsigned int)val_len;
    return 0;
}

/* 删除：成功返回 0，不存在返回 -1 */
int kvs_hash_del(kvs_hash_t *hash, char *key) {
    if (!hash || !hash->nodes || !key) return -2;

    int idx = _find_idx(hash, key);
    if (idx < 0) return -1;

    hashnode_t *slot = &hash->nodes[idx];
    // 标记 tombstone，并释放已有 key/value
    if (slot->key) {
        kvs_free(slot->key);
        slot->key = NULL;
    }
    if (slot->value) {
        kvs_free(slot->value);
        slot->value = NULL;
    }
    slot->klen = 0;
    slot->vlen = 0;
    slot->used = 2;     // tombstone
    hash->count--;
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
    return strcmp(nodeA->key, nodeB->key);
}

/* 排序输出：order 0=升序，1=降序；limit 控制最多输出条数 */
int kvs_hash_sort(kvs_hash_t *inst, int order, int limit,
                  char *response, int max_len) {
    if (!inst || !inst->nodes) return -1;

    int total = inst->count;
    if (total <= 0) return 0;

    hashnode_t **arr = (hashnode_t **)kvs_malloc((size_t)total * sizeof(hashnode_t *));
    if (!arr) return -1;

    int cnt = 0;
    for (int i = 0; i < inst->max_slots; ++i) {
        hashnode_t *slot = &inst->nodes[i];
        if (slot->used == 1) {
            arr[cnt++] = slot;
        }
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

    int total = inst->count;
    if (total <= 0) return 0;

    hashnode_t **arr = (hashnode_t **)kvs_malloc((size_t)total * sizeof(hashnode_t *));
    if (!arr) return -1;

    int cnt = 0;
    for (int i = 0; i < inst->max_slots; ++i) {
        hashnode_t *slot = &inst->nodes[i];
        if (slot->used == 1) {
            arr[cnt++] = slot;
        }
    }

    qsort(arr, cnt, sizeof(hashnode_t *), compare_hash_items);

    int len = 0;
    for (int i = 0; i < cnt; ++i) {
        hashnode_t *slot = arr[i];
        if (strcmp(slot->key, start_key) < 0) continue;
        if (strcmp(slot->key, end_key)   > 0) break;

        int n = snprintf(response + len, max_len - len,
                         "%s:%s\r\n", slot->key, slot->value);
        if (n < 0 || len + n >= max_len) break;
        len += n;
    }

    kvs_free(arr);
    return len;
}