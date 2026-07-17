
#include "kvs_api.h"  // 聚合公共头（含内存池声明）
#include <string.h>
#include <stdlib.h>
#include <stdio.h>


kvs_skiptable_t global_skiptable;

static unsigned int skiptable_key_hash_with_len(const char *key, size_t *key_len_out) {
    unsigned int hash = 0;
    size_t key_len = 0;

    if (!key) {
        if (key_len_out) {
            *key_len_out = 0;
        }
        return 0;
    }

    while (key[key_len] != '\0') {
        hash ^= ((hash << 5) + (unsigned char)key[key_len] + (hash >> 2));
        key_len++;
    }

    if (key_len_out) {
        *key_len_out = key_len;
    }
    return hash;
}

static unsigned int skiptable_key_hash_len(const char *key, size_t key_len) {
    if (!key) return 0;
    unsigned int hash = 0;
    for (size_t i = 0; i < key_len; i++) {
        hash ^= ((hash << 5) + (unsigned char)key[i] + (hash >> 2));
    }
    return hash;
}

static size_t skiptable_key_len(const char *key) {
    size_t key_len = 0;

    if (!key) {
        return 0;
    }

    while (key[key_len] != '\0') {
        key_len++;
    }

    return key_len;
}

static size_t skiptable_value_len(const char *value) {
    size_t value_len = 0;

    if (!value) {
        return 0;
    }

    while (value[value_len] != '\0') {
        value_len++;
    }

    return value_len;
}

static int skiptable_key_compare(const Node *node, const char *key, size_t key_len) {
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

static int skiptable_key_equals(const Node *node, const char *key, size_t key_len, unsigned int key_hash) {
    if (!node || !node->key || !key) {
        return 0;
    }

    if (node->key_hash != key_hash || node->key_len != key_len) {
        return 0;
    }

    return skiptable_key_compare(node, key, key_len) == 0;
}

static int skiptable_update_value_with_len(Node *node, const char *value, size_t value_len);

static int skiptable_update_value(Node *node, const char *value) {
    if (!node || !value) {
        return -1;
    }

    return skiptable_update_value_with_len(node, value, skiptable_value_len(value));
}

static int skiptable_update_value_with_len(Node *node, const char *value, size_t value_len) {
    if (!node || !value) {
        return -1;
    }

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

static Node *skiptable_find_with_update(kvs_skiptable_t *table,
                                        const char *key,
                                        size_t key_len,
                                        Node **update) {
    if (!table || !table->header || !key) {
        return NULL;
    }

    Node *current = table->header;
    for (int i = table->level; i >= 0; i--) {
        while (current->forward[i] && skiptable_key_compare(current->forward[i], key, key_len) < 0) {
            current = current->forward[i];
        }
        if (update) {
            update[i] = current;
        }
    }

    return current->forward[0];
}

static Node *skiptable_find_with_update_or_exact(kvs_skiptable_t *table,
                                                 const char *key,
                                                 size_t key_len,
                                                 Node **update) {
    if (!table || !table->header || !key) {
        return NULL;
    }

    Node *current = table->header;
    for (int i = table->level; i >= 0; i--) {
        while (current->forward[i]) {
            int cmp = skiptable_key_compare(current->forward[i], key, key_len);
            if (cmp < 0) {
                current = current->forward[i];
                continue;
            }

            if (cmp == 0) {
                if (update) {
                    update[i] = current;
                }
                return current->forward[i];
            }

            break;
        }

        if (update) {
            update[i] = current;
        }
    }

    return current->forward[0];
}

static Node *skiptable_find_exact(kvs_skiptable_t *table, const char *key, size_t key_len) {
    if (!table || !table->header || !key) {
        return NULL;
    }

    Node *current = table->header;
    for (int i = table->level; i >= 0; i--) {
        while (current->forward[i]) {
            int cmp = skiptable_key_compare(current->forward[i], key, key_len);
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

Node* kvs_skiptable_lookup_node(kvs_skiptable_t *table, char *key) {
    if (!table || !table->header || !key) {
        return NULL;
    }

    size_t key_len = skiptable_key_len(key);
    Node *current = skiptable_find_exact(table, key, key_len);
    return current;
}

static int skiptable_append_kv_line(char *response, int max_len, int *pos, const Node *node) {
    if (!response || !pos || !node || !node->key || !node->value) {
        return -1;
    }

    int needed = (int)node->key_len + 1 + (int)node->value_len + 2;
    if (*pos < 0 || *pos + needed >= max_len) {
        return -1;
    }

    memcpy(response + *pos, node->key, node->key_len);
    *pos += (int)node->key_len;
    response[(*pos)++] = ':';
    memcpy(response + *pos, node->value, node->value_len);
    *pos += (int)node->value_len;
    response[(*pos)++] = '\r';
    response[(*pos)++] = '\n';
    return 0;
}

// 创建跳表节点（使用内存池）
Node* create_node(int level, const char* key, const char* value) {
    // 计算节点总大小（结构体 + 指针数组）
    size_t node_size = sizeof(Node) + (level + 1) * sizeof(Node*);
    Node* node = kvs_malloc(node_size); //mem_alloc
    if (!node) return NULL;

    node->level = level;
    node->expire_at_ms = KV_TTL_NO_EXPIRE;
    node->key_len = 0;
    node->value_len = 0;
    node->key_hash = 0;
    for (int i = 0; i <= level; i++) {
        node->forward[i] = NULL;
    }

    size_t klen = 0;
    size_t vlen = 0;
    node->key_hash = skiptable_key_hash_with_len(key, &klen);
    vlen = skiptable_value_len(value);
    node->key_len = (uint32_t)klen;
    node->value_len = (uint32_t)vlen;

    node->key = kvs_malloc(klen + 1);
    node->value = kvs_malloc(vlen + 1);
    if (!node->key || !node->value) {
        if (node->key) kvs_free(node->key);
        if (node->value) kvs_free(node->value);
        kvs_free(node);
        return NULL;
    }
    memcpy(node->key, key, klen + 1);
    memcpy(node->value, value, vlen + 1);
    return node;
}

static Node* create_node_len(int level,
                              const char* key, size_t key_len,
                              const char* value, size_t value_len) {
    size_t node_size = sizeof(Node) + (level + 1) * sizeof(Node*);
    Node* node = kvs_malloc(node_size);
    if (!node) return NULL;

    node->level = level;
    node->expire_at_ms = KV_TTL_NO_EXPIRE;
    node->key_hash = skiptable_key_hash_len(key, key_len);
    node->key_len = (uint32_t)key_len;
    node->value_len = (uint32_t)value_len;

    for (int i = 0; i <= level; i++) node->forward[i] = NULL;

    node->key = kvs_malloc(key_len + 1);
    node->value = kvs_malloc(value_len + 1);
    if (!node->key || !node->value) {
        if (node->key) kvs_free(node->key);
        if (node->value) kvs_free(node->value);
        kvs_free(node);
        return NULL;
    }
    memcpy(node->key, key, key_len);
    node->key[key_len] = '\0';
    memcpy(node->value, value, value_len);
    node->value[value_len] = '\0';
    return node;
}

// 销毁节点（使用内存池）
void free_node(Node* node) {
    if (!node) return;
    if (node->key) kvs_free(node->key);
    if (node->value) kvs_free(node->value);
    kvs_free(node);
}

int kvs_skiptable_create(kvs_skiptable_t *table) {
    if (!table) return -1;

    // 初始化跳表结构
    table->level = 0;
    table->count = 0;


    // 创建头节点（使用最大层数）
    table->header = create_node(MAX_LEVEL, "", "");
    if (!table->header) return -1;

    // 初始化所有指针为NULL
    for (int i = 0; i <= MAX_LEVEL; i++) {
        table->header->forward[i] = NULL;
    }
    
    return 0;
}

void kvs_skiptable_destroy(kvs_skiptable_t *table) {
    if (!table || !table->header) return;
    
    // 遍历释放所有节点
    Node* current = table->header->forward[0];
    while (current) {
        Node* next = current->forward[0];
        free_node(current);
        current = next;
    }
    
    // 释放头节点
    free_node(table->header);
    table->header = NULL;
    table->level = 0;
    table->count = 0;
}

// 随机生成节点层数
int random_level() {
    int level = 0;
    while (rand() < RAND_MAX / 2 && level < MAX_LEVEL)
        level++;
    return level;
}

static void kvs_skiptable_reap_expired(kvs_skiptable_t *table, int64_t now_ms) {
    if (!table || !table->header) return;

    Node *current = table->header->forward[0];
    while (current) {
        Node *next = current->forward[0];
        if (current->key && kv_is_expired(current->expire_at_ms, now_ms)) {
            (void)kvs_skiptable_del(table, current->key);
        }
        current = next;
    }
}

int kvs_skiptable_set(kvs_skiptable_t *table, char *key, char *value) {
    if (!table || !table->header  || !key || !value) return -1;
    
    size_t key_len = 0;
    unsigned int key_hash = skiptable_key_hash_with_len(key, &key_len);
    Node* update[MAX_LEVEL + 1];
    Node *current = skiptable_find_with_update_or_exact(table, key, key_len, update);
    if (current && skiptable_key_equals(current, key, key_len, key_hash)) {
        size_t value_len = skiptable_value_len(value);
        if (skiptable_update_value_with_len(current, value, value_len) != 0) {
            return -1;
        }
        current->expire_at_ms = KV_TTL_NO_EXPIRE;
        return 1;  // 表示更新
    }
    
    // 创建新节点
    int new_level = random_level();
    Node* new_node = create_node(new_level, key, value);
    if (!new_node) return -1;

    table->count++;
    
    // 如果新节点的层数大于当前跳表层数，更新跳表层数
    if (new_level > table->level) {
        for (int i = table->level + 1; i <= new_level; i++) {
            update[i] = table->header;
        }
        table->level = new_level;
    }
    
    // 插入新节点
    for (int i = 0; i <= new_level; i++) {
        new_node->forward[i] = update[i]->forward[i];
        update[i]->forward[i] = new_node;
    }
    
    return 0;  // 表示插入
}

char* kvs_skiptable_get(kvs_skiptable_t *table, char *key) {
    Node *current = kvs_skiptable_lookup_node(table, key);
    if (current) {
        return current->value;
    }
    
    return NULL;
}

int kvs_skiptable_mod(kvs_skiptable_t *table, char *key, char *value) {
    if (!table || !table->header || !key || !value) return -1;  // 添加header检查

    size_t key_len = 0;
    unsigned int key_hash = skiptable_key_hash_with_len(key, &key_len);
    Node *node = skiptable_find_with_update_or_exact(table, key, key_len, NULL);

    if (node && skiptable_key_equals(node, key, key_len, key_hash)) {
        size_t value_len = skiptable_value_len(value);
        if (skiptable_update_value_with_len(node, value, value_len) != 0) {
            return -1;
        }
        node->expire_at_ms = KV_TTL_NO_EXPIRE;
        return 0;
    }
    
    return -1;  // 未找到
}

int kvs_skiptable_del(kvs_skiptable_t *table, char *key) {
    if (!table || !table->header  || !key) return -1;
    
    Node* update[MAX_LEVEL + 1];
    size_t key_len = 0;
    unsigned int key_hash = skiptable_key_hash_with_len(key, &key_len);
    
    // 查找要删除的节点及更新路径
    Node* current = skiptable_find_with_update(table, key, key_len, update);
    
    // 如果节点不存在
    if (!current || !skiptable_key_equals(current, key, key_len, key_hash)) {
        return -1;
    }
    
    // 更新前驱节点的指针
    for (int i = 0; i <= table->level; i++) {
        if (update[i]->forward[i] != current) break;
        update[i]->forward[i] = current->forward[i];
    }

    table->count--;
    
    // 释放节点内存
    free_node(current);
    
    // 降低跳表层数（如果最高层变为空）
    while (table->level > 0 && table->header->forward[table->level] == NULL) {
        table->level--;
    }
    
    return 0;
}

int kvs_skiptable_exist(kvs_skiptable_t *table, char *key) {
    if (!kvs_skiptable_lookup_node(table, key)) {
        return 1;
    }

    return 0;
}

int kvs_skiptable_sort(kvs_skiptable_t *table, int order, int limit, char *response, int max_len) {
    if (!table || !table->header || !response || max_len <= 0) return -1;  // 添加header检查

    kvs_skiptable_reap_expired(table, kv_now_ms());
    
    Node* current;
    int pos = 0;
    int count = 0;

    // Node* current = table->header->forward[0];
    // int pos = 0;
    
    if (order == 0) { // 升序
        current = table->header->forward[0];
    }else { // 降序 - 需要先收集所有节点
        size_t buffer_size = (limit > 0 && limit < table->count) ? (size_t)limit : (size_t)table->count;
        if (buffer_size == 0) {
            response[0] = '\0';
            return 0;
        }

        // 收集所有节点或最近 limit 个节点
        Node** nodes = kvs_malloc(buffer_size * sizeof(Node*));
        if (!nodes) return -1;
        
        current = table->header->forward[0];
        size_t stored = 0;
        while (current) {
            nodes[stored % buffer_size] = current;
            stored++;
            current = current->forward[0];
        }
        
        // 反向遍历
        size_t output_count = (limit > 0 && (size_t)limit < stored) ? (size_t)limit : stored;
        size_t start = stored > buffer_size ? stored % buffer_size : 0;
        for (size_t j = 0; j < output_count && (limit <= 0 || count < limit); j++) {
            size_t idx = (start + output_count - 1 - j) % buffer_size;
            if (skiptable_append_kv_line(response, max_len, &pos, nodes[idx]) != 0) break;
            count++;
        }
        
        kvs_free(nodes);
        if (pos < max_len) response[pos] = '\0';
        else response[max_len - 1] = '\0';
        return pos;
    }

    // 升序遍历
    while (current && (limit <= 0 || count < limit) && pos < max_len) {
        if (skiptable_append_kv_line(response, max_len, &pos, current) != 0) break;
        count++;
        current = current->forward[0];
    }
    
    // 确保字符串终止
    if (pos < max_len) response[pos] = '\0';
    else response[max_len - 1] = '\0';
    
    return pos;
}

int kvs_skiptable_range(kvs_skiptable_t *table, char *start_key, char *end_key, 
                        int limit, char *response, int max_len) {
    if (!table || !table->header || !start_key || !end_key || !response || max_len <= 0) 
        return -1;  // 添加header检查

    kvs_skiptable_reap_expired(table, kv_now_ms());

    // 处理 start_key > end_key 的情况（空范围）
    size_t start_key_len = strlen(start_key);
    size_t end_key_len = strlen(end_key);
    if (start_key_len > end_key_len ||
        (start_key_len == end_key_len && memcmp(start_key, end_key, start_key_len) > 0)) {
        response[0] = '\0';
        return 0;
    }
    
    Node* current = table->header;
    int pos = 0;
    int returned = 0;
    int visited = 0;
    
    // 定位起始位置
    for (int i = table->level; i >= 0; i--) {
        while (current->forward[i] && 
               skiptable_key_compare(current->forward[i], start_key, start_key_len) < 0) {
            current = current->forward[i];
        }
    }
    current = current->forward[0];
    
    // 遍历范围内的节点
    while (current && skiptable_key_compare(current, end_key, end_key_len) <= 0 && pos < max_len) {
        if (limit > 0 && returned >= limit) {
            break;
        }
        visited++;
        if (skiptable_append_kv_line(response, max_len, &pos, current) != 0) break;
        returned++;
        current = current->forward[0];
    }

#if KVS_RANGE_VISITED_DEBUG
    fprintf(stderr, "[RANGE_VISITED][SRANGE] start=%s end=%s limit=%d returned=%d visited=%d\n",
            start_key, end_key, limit, returned, visited);
#endif

    
    // 确保字符串终止
    if (pos < max_len) response[pos] = '\0';
    else response[max_len - 1] = '\0';
    
    return pos;
}

/* === Binary-safe _len API implementations === */

int kvs_skiptable_set_len(kvs_skiptable_t *table, const char *key, size_t key_len, const char *value, size_t value_len) {
    if (!table || !table->header || !key || !value) return -1;
    Node* update[MAX_LEVEL + 1];
    Node *current = skiptable_find_with_update_or_exact(table, key, key_len, update);
    unsigned int key_hash = skiptable_key_hash_len(key, key_len);
    if (current && skiptable_key_equals(current, key, key_len, key_hash)) {
        if (skiptable_update_value_with_len(current, value, value_len) != 0) return -1;
        current->expire_at_ms = KV_TTL_NO_EXPIRE;
        return 1;
    }
    int new_level = random_level();
    Node* new_node = create_node_len(new_level, key, key_len, value, value_len);
    if (!new_node) return -1;
    table->count++;
    if (new_level > table->level) {
        for (int i = table->level + 1; i <= new_level; i++) update[i] = table->header;
        table->level = new_level;
    }
    for (int i = 0; i <= new_level; i++) {
        new_node->forward[i] = update[i]->forward[i];
        update[i]->forward[i] = new_node;
    }
    return 0;
}

char* kvs_skiptable_get_len(kvs_skiptable_t *table, const char *key, size_t key_len, size_t *value_len_out) {
    if (!table || !table->header || !key) { if (value_len_out) *value_len_out = 0; return NULL; }
    Node *node = skiptable_find_exact(table, key, key_len);
    if (!node) { if (value_len_out) *value_len_out = 0; return NULL; }
    if (value_len_out) *value_len_out = node->value_len;
    return node->value;
}

int kvs_skiptable_del_len(kvs_skiptable_t *table, const char *key, size_t key_len) {
    if (!table || !table->header || !key) return -1;
    Node* update[MAX_LEVEL + 1];
    Node *current = skiptable_find_with_update(table, key, key_len, update);
    unsigned int key_hash = skiptable_key_hash_len(key, key_len);
    if (!current || !skiptable_key_equals(current, key, key_len, key_hash)) return 1;
    for (int i = 0; i <= table->level; i++) {
        if (update[i]->forward[i] != current) break;
        update[i]->forward[i] = current->forward[i];
    }
    while (table->level > 0 && table->header->forward[table->level] == NULL) table->level--;
    free_node(current);
    table->count--;
    return 0;
}

int kvs_skiptable_mod_len(kvs_skiptable_t *table, const char *key, size_t key_len, const char *value, size_t value_len) {
    if (!table || !table->header || !key || !value) return -1;
    Node *node = skiptable_find_exact(table, key, key_len);
    if (!node) return 1;
    if (skiptable_update_value_with_len(node, value, value_len) != 0) return -1;
    node->expire_at_ms = KV_TTL_NO_EXPIRE;
    return 0;
}

int kvs_skiptable_exist_len(kvs_skiptable_t *table, const char *key, size_t key_len) {
    if (!table || !table->header || !key) return 0;
    Node *node = skiptable_find_exact(table, key, key_len);
    return node ? 1 : 0;
}
