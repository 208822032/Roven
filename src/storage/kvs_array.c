

#include "kvs_api.h"
#include <string.h>
#include <stdlib.h>
#include <stdio.h>



// singleton

kvs_array_t global_array = {0};

enum {
    KVS_ARRAY_INDEX_LOAD_NUM = 7,
    KVS_ARRAY_INDEX_LOAD_DEN = 10,
    KVS_ARRAY_INDEX_INITIAL_CAPACITY = KVS_ARRAY_SIZE * 2,
};

static uint32_t kvs_array_hash_key(const char *key) {
    uint32_t hash = 0;
    if (!key) return 0;
    while (*key) {
        hash ^= ((hash << 5) + (unsigned char)(*key) + (hash >> 2));
        key++;
    }
    return hash;
}

static int kvs_array_next_pow2(int value) {
    unsigned int target = 1;
    if (value <= 1) return 1;
    while (target < (unsigned int)value) {
        target <<= 1;
    }
    return (int)target;
}

static void kvs_array_index_reset(kvs_array_t *inst) {
    if (!inst) return;
    if (inst->side_index) {
        kvs_free(inst->side_index);
        inst->side_index = NULL;
    }
    inst->side_capacity = 0;
    inst->side_count = 0;
    inst->side_tombstones = 0;
}

static int kvs_array_index_rehash(kvs_array_t *inst, int new_cap) {
    if (!inst || new_cap <= 0) return -1;

    int target = kvs_array_next_pow2(new_cap);
    size_t bytes = (size_t)target * sizeof(kvs_array_index_entry_t);
    kvs_array_index_entry_t *new_index = (kvs_array_index_entry_t *)kvs_malloc(bytes);
    if (!new_index) return -1;
    memset(new_index, 0, bytes);

    kvs_array_index_entry_t *old_index = inst->side_index;
    int old_cap = inst->side_capacity;

    inst->side_index = new_index;
    inst->side_capacity = target;
    inst->side_count = 0;
    inst->side_tombstones = 0;

    if (!old_index || old_cap <= 0) {
        return 0;
    }

    for (int i = 0; i < old_cap; ++i) {
        kvs_array_index_entry_t *entry = &old_index[i];
        if (entry->state != KVS_ARRAY_INDEX_USED || !entry->key) {
            continue;
        }

        uint32_t mask = (uint32_t)(inst->side_capacity - 1);
        uint32_t pos = entry->key_hash & mask;
        while (inst->side_index[pos].state == KVS_ARRAY_INDEX_USED) {
            pos = (pos + 1) & mask;
        }

        inst->side_index[pos] = *entry;
        inst->side_index[pos].state = KVS_ARRAY_INDEX_USED;
        inst->side_count++;
    }

    kvs_free(old_index);
    return 0;
}

static int kvs_array_index_ensure_capacity(kvs_array_t *inst, int min_items) {
    if (!inst) return -1;

    int target = inst->side_capacity > 0 ? inst->side_capacity : KVS_ARRAY_INDEX_INITIAL_CAPACITY;
    if (target < 1) target = KVS_ARRAY_INDEX_INITIAL_CAPACITY;

    while (min_items * KVS_ARRAY_INDEX_LOAD_DEN >= target * KVS_ARRAY_INDEX_LOAD_NUM) {
        target *= 2;
    }

    if (inst->side_index && inst->side_tombstones > inst->side_count) {
        if (target == inst->side_capacity) {
            target *= 2;
        }
    }

    if (!inst->side_index || target != inst->side_capacity) {
        return kvs_array_index_rehash(inst, target);
    }

    return 0;
}

static int kvs_array_index_locate(kvs_array_t *inst,
                                  const char *key,
                                  uint32_t key_len,
                                  uint32_t key_hash,
                                  int *found_slot,
                                  int *insert_slot) {
    if (!inst || !inst->side_index || inst->side_capacity <= 0 || !key) return -1;

    uint32_t mask = (uint32_t)(inst->side_capacity - 1);
    uint32_t pos = key_hash & mask;
    int first_tombstone = -1;

    for (int step = 0; step < inst->side_capacity; ++step) {
        kvs_array_index_entry_t *entry = &inst->side_index[pos];
        if (entry->state == KVS_ARRAY_INDEX_EMPTY) {
            if (insert_slot) {
                *insert_slot = (first_tombstone >= 0) ? first_tombstone : (int)pos;
            }
            return 0;
        }

        if (entry->state == KVS_ARRAY_INDEX_USED &&
            entry->key_hash == key_hash &&
            entry->key_len == key_len &&
            entry->key && memcmp(entry->key, key, (size_t)key_len) == 0) {
            if (found_slot) {
                *found_slot = (int)pos;
            }
            if (insert_slot) {
                *insert_slot = (int)pos;
            }
            return 1;
        }

        if (entry->state == KVS_ARRAY_INDEX_TOMBSTONE && first_tombstone < 0) {
            first_tombstone = (int)pos;
        }

        pos = (pos + 1) & mask;
    }

    if (insert_slot) {
        *insert_slot = (first_tombstone >= 0) ? first_tombstone : -1;
    }
    return 0;
}

static int kvs_array_index_insert_or_update(kvs_array_t *inst,
                                            char *key,
                                            uint32_t key_len,
                                            uint32_t key_hash,
                                            int dense_idx) {
    if (!inst || !key) return -1;

    if (kvs_array_index_ensure_capacity(inst, inst->side_count + 1) != 0) {
        return -1;
    }

    int found_slot = -1;
    int insert_slot = -1;
    int locate_ret = kvs_array_index_locate(inst, key, key_len, key_hash, &found_slot, &insert_slot);
    if (locate_ret < 0) return -1;

    if (found_slot >= 0) {
        inst->side_index[found_slot].dense_idx = dense_idx;
        return 0;
    }

    if (insert_slot < 0) {
        if (kvs_array_index_rehash(inst, inst->side_capacity > 0 ? inst->side_capacity * 2 : KVS_ARRAY_INDEX_INITIAL_CAPACITY) != 0) {
            return -1;
        }
        found_slot = -1;
        insert_slot = -1;
        if (kvs_array_index_locate(inst, key, key_len, key_hash, &found_slot, &insert_slot) < 0) {
            return -1;
        }
        if (found_slot >= 0) {
            inst->side_index[found_slot].dense_idx = dense_idx;
            return 0;
        }
    }

    kvs_array_index_entry_t *entry = &inst->side_index[insert_slot];
    if (entry->state == KVS_ARRAY_INDEX_TOMBSTONE) {
        inst->side_tombstones--;
    }

    entry->key = key;
    entry->key_len = key_len;
    entry->key_hash = key_hash;
    entry->dense_idx = dense_idx;
    entry->state = KVS_ARRAY_INDEX_USED;
    inst->side_count++;
    return 0;
}

static int kvs_array_index_remove(kvs_array_t *inst, const char *key, uint32_t key_len, uint32_t key_hash) {
    if (!inst || !inst->side_index || !key) return -1;

    int found_slot = -1;
    int insert_slot = -1;
    int locate_ret = kvs_array_index_locate(inst, key, key_len, key_hash, &found_slot, &insert_slot);
    if (locate_ret < 0 || found_slot < 0) return 1;

    kvs_array_index_entry_t *entry = &inst->side_index[found_slot];
    entry->key = NULL;
    entry->key_len = 0;
    entry->key_hash = 0;
    entry->dense_idx = -1;
    entry->state = KVS_ARRAY_INDEX_TOMBSTONE;
    if (inst->side_count > 0) inst->side_count--;
    inst->side_tombstones++;

    if (inst->side_tombstones > inst->side_count && inst->side_capacity > 0) {
        (void)kvs_array_index_rehash(inst, inst->side_capacity);
    }

    return 0;
}

static int kvs_array_index_update_dense_idx(kvs_array_t *inst, const char *key, uint32_t key_len, uint32_t key_hash, int dense_idx) {
    if (!inst || !inst->side_index || !key) return -1;

    int found_slot = -1;
    int insert_slot = -1;
    int locate_ret = kvs_array_index_locate(inst, key, key_len, key_hash, &found_slot, &insert_slot);
    if (locate_ret < 0 || found_slot < 0) return 1;

    inst->side_index[found_slot].dense_idx = dense_idx;
    return 0;
}

#if defined(KVS_ARRAY_VERIFY_INTEGRITY)
static int kvs_array_validate_consistency(kvs_array_t *inst, const char *reason) {
    if (!inst || !inst->table || !inst->side_index || inst->capacity < 0 || inst->side_capacity < 0) {
        return 0;
    }

    if (inst->len < 0 || inst->len > inst->capacity) {
        fprintf(stderr, "[array-verify][%s] len out of range: len=%d capacity=%d\n",
                reason ? reason : "unknown", inst->len, inst->capacity);
        return -1;
    }

    if (inst->side_count != inst->len) {
        fprintf(stderr, "[array-verify][%s] side_count mismatch: side_count=%d len=%d\n",
                reason ? reason : "unknown", inst->side_count, inst->len);
        return -1;
    }

    for (int i = 0; i < inst->len; ++i) {
        kvs_array_item_t *item = &inst->table[i];
        if (!item->key || !item->value) {
            fprintf(stderr, "[array-verify][%s] empty dense slot: idx=%d\n",
                    reason ? reason : "unknown", i);
            return -1;
        }

        uint32_t key_len = (uint32_t)strlen(item->key);
        uint32_t value_len = (uint32_t)strlen(item->value);
        uint32_t key_hash = kvs_array_hash_key(item->key);
        if (item->key_len != key_len || item->value_len != value_len || item->key_hash != key_hash) {
            fprintf(stderr, "[array-verify][%s] meta mismatch at idx=%d\n",
                    reason ? reason : "unknown", i);
            return -1;
        }

        int idx = -1;
        if (kvs_array_lookup_index(inst, item->key, &idx) != 0 || idx != i) {
            fprintf(stderr, "[array-verify][%s] index lookup mismatch at idx=%d lookup=%d\n",
                    reason ? reason : "unknown", i, idx);
            return -1;
        }
    }

    for (int i = 0; i < inst->side_capacity; ++i) {
        kvs_array_index_entry_t *entry = &inst->side_index[i];
        if (entry->state != KVS_ARRAY_INDEX_USED) {
            continue;
        }

        if (!entry->key || entry->dense_idx < 0 || entry->dense_idx >= inst->len) {
            fprintf(stderr, "[array-verify][%s] side entry invalid: slot=%d dense_idx=%d\n",
                    reason ? reason : "unknown", i, entry->dense_idx);
            return -1;
        }

        kvs_array_item_t *item = &inst->table[entry->dense_idx];
        if (item->key != entry->key || item->key_len != entry->key_len || item->key_hash != entry->key_hash) {
            fprintf(stderr, "[array-verify][%s] side entry mismatch: slot=%d dense_idx=%d\n",
                    reason ? reason : "unknown", i, entry->dense_idx);
            return -1;
        }
    }

    return 0;
}

static void kvs_array_validate_after_mutation(kvs_array_t *inst, const char *reason) {
    if (!inst) return;
    if (kvs_array_validate_consistency(inst, reason) != 0) {
        abort();
    }
}
#else
static void kvs_array_validate_after_mutation(kvs_array_t *inst, const char *reason) {
    (void)inst;
    (void)reason;
}
#endif

static int kvs_array_del_at_impl(kvs_array_t *inst, int idx, int validate_after) {
    if (inst == NULL || inst->table == NULL || idx < 0 || idx >= inst->len) {
        return -1;
    }

    kvs_array_item_t removed = inst->table[idx];
    if (removed.key) {
        (void)kvs_array_index_remove(inst, removed.key, removed.key_len, removed.key_hash);
    }

    int last_idx = inst->len - 1;
    if (idx != last_idx) {
        kvs_array_item_t moved = inst->table[last_idx];
        inst->table[idx] = moved;
        if (moved.key) {
            (void)kvs_array_index_update_dense_idx(inst, moved.key, moved.key_len, moved.key_hash, idx);
        }
    }

    memset(&inst->table[last_idx], 0, sizeof(kvs_array_item_t));
    inst->len--;
    inst->total = inst->len;

    if (removed.key) kvs_free(removed.key);
    if (removed.value) kvs_free(removed.value);

    if (validate_after) {
        kvs_array_validate_after_mutation(inst, "del_at");
    }

    return 0;
}

int kvs_array_lookup_index(kvs_array_t *inst, const char *key, int *out_idx) {
    if (!inst || !key || !out_idx || !inst->side_index || inst->side_capacity <= 0) return 1;

    uint32_t key_len = (uint32_t)strlen(key);
    uint32_t key_hash = kvs_array_hash_key(key);
    int found_slot = -1;
    int insert_slot = -1;
    int locate_ret = kvs_array_index_locate(inst, key, key_len, key_hash, &found_slot, &insert_slot);
    if (locate_ret < 0 || found_slot < 0) return 1;

    *out_idx = inst->side_index[found_slot].dense_idx;
    return 0;
}

static void kvs_array_reap_expired(kvs_array_t *inst, int64_t now_ms) {
    if (!inst || !inst->table) return;

    for (int i = 0; i < inst->len;) {
        kvs_array_item_t *item = &inst->table[i];
        if (item->key && kv_is_expired(item->expire_at_ms, now_ms)) {
            if (kvs_array_del_at_impl(inst, i, 0) == 0) {
                continue;
            }
        }
        i++;
    }

    kvs_array_validate_after_mutation(inst, "reap_expired");
}

int kvs_array_create(kvs_array_t *inst) {

	if (!inst) return -1;
	if (inst->table) {
		printf("table has alloc\n");
		return -1;
	}	
	inst->table = kvs_malloc(KVS_ARRAY_SIZE * sizeof(kvs_array_item_t));
	if (!inst->table) {
		return -1;
	}
    memset(inst->table, 0, KVS_ARRAY_SIZE * sizeof(kvs_array_item_t));

    inst->side_index = NULL;
    if (kvs_array_index_rehash(inst, KVS_ARRAY_INDEX_INITIAL_CAPACITY) != 0) {
        kvs_free(inst->table);
        inst->table = NULL;
        return -1;
    }

    inst->capacity = KVS_ARRAY_SIZE;
    inst->min_capacity = inst->capacity;
    inst->len = 0;
	inst->total = 0;

	return 0;

}

int kvs_array_reserve(kvs_array_t *inst, int new_cap) {

    if (!inst || !inst->table || inst->capacity <= 0) return -1;
    if (new_cap <= inst->capacity) return 0;

    int target = inst->capacity * 2;
    if (target < new_cap) {
        target = new_cap;
    }

    if (target <= inst->capacity) {
        return -1;
    }

    kvs_array_item_t *new_table = (kvs_array_item_t *)kvs_realloc(
        inst->table, (size_t)target * sizeof(kvs_array_item_t));
    if (!new_table) {
        return -1;
    }

    memset(new_table + inst->capacity, 0,
        (size_t)(target - inst->capacity) * sizeof(kvs_array_item_t));

    inst->table = new_table;
    inst->capacity = target;

    return 0;
}

void kvs_array_destory(kvs_array_t *inst) {

	if (!inst) return ;

	if (inst->table) {
        // 先释放每个元素的 key/value
    for (int i = 0; i < inst->len; i++) {
            if (inst->table[i].key) {
                kvs_free(inst->table[i].key);
                inst->table[i].key = NULL;
            }
            if (inst->table[i].value) {
                kvs_free(inst->table[i].value);
                inst->table[i].value = NULL;
            }
        }

		kvs_free(inst->table);
        inst->table = NULL;
	}

	kvs_array_index_reset(inst);

    inst->len = 0;
    inst->capacity = 0;
    inst->min_capacity = 0;
    inst->total = 0;

}


/*
 * @return: <0, error; =0, success; >0, exist
 */

int kvs_array_set(kvs_array_t *inst, char *key, char *value) {

	if (inst == NULL || key == NULL || value == NULL) return -1;
    uint32_t key_len = (uint32_t)strlen(key);
    uint32_t value_len = (uint32_t)strlen(value);
    uint32_t key_hash = kvs_array_hash_key(key);

    int idx = -1;
    if (kvs_array_lookup_index(inst, key, &idx) == 0) {
        char *kvalue = kvs_malloc((size_t)value_len + 1);
        if (kvalue == NULL) {
            return -2;
        }
        memcpy(kvalue, value, (size_t)value_len + 1);

        if (inst->table[idx].value) {
            kvs_free(inst->table[idx].value);
        }
        inst->table[idx].value = kvalue;
        inst->table[idx].value_len = value_len;
        inst->table[idx].expire_at_ms = KV_TTL_NO_EXPIRE;
        return 1;
    }

    if (inst->len >= inst->capacity) {
        if (kvs_array_reserve(inst, inst->capacity + 1) != 0) {
            return -1;
        }
    }

    char *kcopy = kvs_malloc((size_t)key_len + 1);
	if (kcopy == NULL) return -2;
    memcpy(kcopy, key, (size_t)key_len + 1);

    char *kvalue = kvs_malloc((size_t)value_len + 1);
	if (kvalue == NULL) {
        kvs_free(kcopy); // 释放已分配的内存
        return -2;
    }
    memcpy(kvalue, value, (size_t)value_len + 1);

    inst->table[inst->len].key = kcopy;
    inst->table[inst->len].value = kvalue;
    inst->table[inst->len].key_len = key_len;
    inst->table[inst->len].value_len = value_len;
    inst->table[inst->len].key_hash = key_hash;
    inst->table[inst->len].expire_at_ms = KV_TTL_NO_EXPIRE;

    if (kvs_array_index_insert_or_update(inst, kcopy, key_len, key_hash, inst->len) != 0) {
        kvs_free(kcopy);
        kvs_free(kvalue);
        memset(&inst->table[inst->len], 0, sizeof(kvs_array_item_t));
        return -2;
    }

    inst->len++;
    inst->total = inst->len;
    kvs_array_validate_after_mutation(inst, "set");

	return 0;
}

char* kvs_array_get(kvs_array_t *inst, char *key) {

	if (inst == NULL || key == NULL) return NULL;

    int idx = -1;
    if (kvs_array_lookup_index(inst, key, &idx) == 0) {
        return inst->table[idx].value;
	}
    
	return NULL;
}


/*
 * @return < 0, error;  =0,  success; >0, no exist
 */

#if 0
int kvs_array_del(kvs_array_t *inst, char *key) {

	if (inst == NULL || key == NULL) return -1;

	int i = 0;
	for (i = 0;i < inst->total;i ++) {

		if (strcmp(inst->table[i].key, key) == 0) {

			kvs_free(inst->table[i].key);
			inst->table[i].key = NULL;

			kvs_free(inst->table[i].value);
			inst->table[i].value = NULL;
// error: > 1024
			if (inst->total-1 == i) {
				inst->total --;
			}
			

			return 0;
		}
	}

	return i;
}
#endif
int kvs_array_del(kvs_array_t *inst, char *key) {
    if (inst == NULL || key == NULL || inst->table == NULL) {
        return -1; // 无效参数
    }

    int idx = -1;
	if (kvs_array_lookup_index(inst, key, &idx) != 0) {
        return 1; // 键不存在
    }

    return kvs_array_del_at(inst, idx);
}

int kvs_array_del_at(kvs_array_t *inst, int idx) {
    return kvs_array_del_at_impl(inst, idx, 1);
}


/*
 * @return : < 0, error; =0, success; >0, no exist 
 */

int kvs_array_mod(kvs_array_t *inst, char *key, char *value) {

	if (inst == NULL || key == NULL || value == NULL) return -1;
    uint32_t value_len = (uint32_t)strlen(value);
// error: > 1024
    if (inst->len == 0) {
		return KVS_ARRAY_SIZE;
	}

    int idx = -1;
    if (kvs_array_lookup_index(inst, key, &idx) == 0) {

            kvs_free(inst->table[idx].value);

            char *kvalue = kvs_malloc((size_t)value_len + 1);
			if (kvalue == NULL) return -2;
            memcpy(kvalue, value, (size_t)value_len + 1);

            inst->table[idx].value = kvalue;
            inst->table[idx].value_len = value_len;
            inst->table[idx].expire_at_ms = KV_TTL_NO_EXPIRE;
            kvs_array_validate_after_mutation(inst, "mod");

			return 0;
	}

    return inst->len;
}


/*
 * @return 0: exist, 1: no exist
 */
int kvs_array_exist(kvs_array_t *inst, char *key) {

	if (!inst || !key) return -1;
	
	char *str = kvs_array_get(inst, key);
	if (!str) {
		return 1; // 
	}
	return 0;
}

//用于比较
static int compare_array_items(const void *a, const void *b) {
    const kvs_array_item_t *itemA = *(const kvs_array_item_t * const *)a;
    const kvs_array_item_t *itemB = *(const kvs_array_item_t * const *)b;
    return strcmp(itemA->key, itemB->key);
}

static int kvs_array_append_raw(char *response, int max_len, int *off, const char *data, int len) {
    if (!response || !off || !data || len < 0) return -1;
    if (*off + len > max_len) return -1;
    memcpy(response + *off, data, (size_t)len);
    *off += len;
    return 0;
}

static int kvs_array_append_kv_line(char *response, int max_len, int *off, const kvs_array_item_t *item) {
    static const char sep[] = ":";
    static const char eol[] = "\r\n";

    if (!response || !off || !item || !item->key || !item->value) return -1;
    if (kvs_array_append_raw(response, max_len, off, item->key, (int)item->key_len) < 0) return -1;
    if (kvs_array_append_raw(response, max_len, off, sep, 1) < 0) return -1;
    if (kvs_array_append_raw(response, max_len, off, item->value, (int)item->value_len) < 0) return -1;
    if (kvs_array_append_raw(response, max_len, off, eol, 2) < 0) return -1;
    return 0;
}

static int kvs_array_collect_sorted_candidates(kvs_array_t *inst, kvs_array_item_t ***out_items, int *out_count) {
    if (!inst || !out_items || !out_count || !inst->table) return -1;

    *out_items = NULL;
    *out_count = 0;

    kvs_array_reap_expired(inst, kv_now_ms());
    if (inst->len <= 0) return 0;

    kvs_array_item_t **items = kvs_malloc((size_t)inst->len * sizeof(kvs_array_item_t *));
    if (!items) return -1;

    for (int i = 0; i < inst->len; ++i) {
        items[i] = &inst->table[i];
    }

    qsort(items, (size_t)inst->len, sizeof(kvs_array_item_t *), compare_array_items);

    *out_items = items;
    *out_count = inst->len;
    return 0;
}

static int kvs_array_append_sorted_candidates(kvs_array_item_t **items,
                                              int count,
                                              int order,
                                              int limit,
                                              char *response,
                                              int max_len) {
    if (!items || count < 0 || !response || max_len <= 0) return -1;

    if (order == 1) {
        for (int i = 0; i < count / 2; ++i) {
            kvs_array_item_t *temp = items[i];
            items[i] = items[count - 1 - i];
            items[count - 1 - i] = temp;
        }
    }

    int output_count = (limit > 0 && limit < count) ? limit : count;
    int len = 0;
    for (int i = 0; i < output_count; ++i) {
        if (kvs_array_append_kv_line(response, max_len, &len, items[i]) < 0) break;
    }

    return len;
}

static int kvs_array_append_sorted_range(kvs_array_item_t **items,
                                         int count,
                                         const char *start_key,
                                         const char *end_key,
                                         char *response,
                                         int max_len) {
    if (!items || count < 0 || !start_key || !end_key || !response || max_len <= 0) return -1;

    int len = 0;
    for (int i = 0; i < count; ++i) {
        kvs_array_item_t *item = items[i];
        if (!item->key) continue;
        if (strcmp(item->key, start_key) >= 0 && strcmp(item->key, end_key) <= 0) {
            if (kvs_array_append_kv_line(response, max_len, &len, item) < 0) break;
        }
    }

    return len;
}
/**
 * @return >0 success; <0 false 
 */
//sort hight/low num    高num个/低num个
//升序降序  order 0/1
#if 0
int kvs_array_sort(kvs_array_t *inst, char *response, int max_len) {
    if (!inst) return -1;
    
    // 收集所有有效项
    kvs_array_item_t *items = kvs_malloc(inst->total * sizeof(kvs_array_item_t));
    if (!items) return -1;
    
    int count = 0;
    for (int i = 0; i < inst->total; i++) {
        if (inst->table[i].key) {
            items[count++] = inst->table[i];
        }
    }
    
    // 按键排序
    qsort(items, count, sizeof(kvs_array_item_t), compare_array_items);
    
    // 格式化输出
    int len = 0;
    for (int i = 0; i < count; i++) {
        int n = snprintf(response + len, max_len - len, 
                         "%s:%s\r\n", items[i].key, items[i].value);
        if (n < 0 || len + n >= max_len) break;
        len += n;
    }
    
    kvs_free(items);
    return len;
}
#else
int kvs_array_sort(kvs_array_t *inst, int order, int limit, char *response, int max_len){
    if (!inst || !response || max_len <= 0) return -1;

    kvs_array_item_t **items = NULL;
    int count = 0;
    if (kvs_array_collect_sorted_candidates(inst, &items, &count) != 0) {
        return -1;
    }
    if (count <= 0) {
        kvs_free(items);
        return 0;
    }

    int len = kvs_array_append_sorted_candidates(items, count, order, limit, response, max_len);
    kvs_free(items);
    return len;
}
#endif

int kvs_array_range(kvs_array_t *inst, char *start_key, char *end_key, char *response, int max_len) {
    if (!inst || !start_key || !end_key) return -1;

    kvs_array_item_t **items = NULL;
    int count = 0;
    if (kvs_array_collect_sorted_candidates(inst, &items, &count) != 0) {
        return -1;
    }
    if (count <= 0) {
        kvs_free(items);
        return 0;
    }

    int len = kvs_array_append_sorted_range(items, count, start_key, end_key, response, max_len);
    kvs_free(items);
    return len;
}

#if 0
int main(){
	kvs_array_t test_array;
	test_array.total = 5;
	test_array.table = kvs_malloc(test_array.total * sizeof(kvs_array_item_t));

	// 填充测试数据 (故意乱序)
    test_array.table[0].key = "banana";
    test_array.table[0].value = "yellow";
    
    test_array.table[1].key = "apple";
    test_array.table[1].value = "red";
    
    test_array.table[2].key = NULL;  // 测试空键
    
    test_array.table[3].key = "grape";
    test_array.table[3].value = "purple";
    
    test_array.table[4].key = "cherry";
    test_array.table[4].value = "dark red";

	// 测试1: 排序功能
    printf("=== Testing kvs_array_sort ===\n");
    char response[1024];
    int len = kvs_array_sort(&test_array, response, sizeof(response));
    if (len > 0) {
        printf("Sorted items:\n%s", response);
    } else {
        printf("Sort failed\n");
    }

	// 测试2: 范围查询
    printf("\n=== Testing kvs_array_range ===\n");
    printf("Range from 'banana' to 'grape':\n");
    len = kvs_array_range(&test_array, "banana", "grape", response, sizeof(response));
    if (len > 0) {
        printf("%s", response);
    } else {
        printf("Range query failed\n");
    }

	// 测试3: 边界情况
    printf("\n=== Testing edge cases ===\n");
    // 空实例测试
    printf("Null instance test: %d (expected -1)\n", kvs_array_sort(NULL, response, sizeof(response)));
    
    // 空响应缓冲区测试
    printf("Zero buffer test: %d (expected no crash)\n", kvs_array_sort(&test_array, response, 0));

    // 清理s
    kvs_free(test_array.table);
    return 0;
}
#endif