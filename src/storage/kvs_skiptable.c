
#include "kvs_api.h"  // 聚合公共头（含内存池声明）
#include <string.h>
#include <stdlib.h>
#include <stdio.h>


kvs_skiptable_t global_skiptable;

// 创建跳表节点（使用内存池）
Node* create_node(int level, const char* key, const char* value) {
    // 计算节点总大小（结构体 + 指针数组）
    size_t node_size = sizeof(Node) + (level + 1) * sizeof(Node*);
    Node* node = kvs_malloc(node_size); //mem_alloc
    if (!node) return NULL;

    node->level = level;
    for (int i = 0; i <= level; i++) {
        node->forward[i] = NULL;
    }

    size_t klen = strlen(key);
    size_t vlen = strlen(value);

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

int kvs_skiptable_set(kvs_skiptable_t *table, char *key, char *value) {
    if (!table || !table->header  || !key || !value) return -1;
    
    Node* update[MAX_LEVEL + 1];
    Node* current = table->header;
    
    // 从最高层开始查找插入位置
    for (int i = table->level; i >= 0; i--) {
        while (current->forward[i] && 
               strcmp(current->forward[i]->key, key) < 0) {
            current = current->forward[i];
        }
        update[i] = current;
    }
    
    current = current->forward[0];
    
    // 如果键已存在，更新值
    if (current && strcmp(current->key, key) == 0) {
        size_t vlen = strlen(value);
        char *new_value = kvs_malloc(vlen + 1);
        if (!new_value) return -1;
        memcpy(new_value, value, vlen + 1);
        if (current->value) kvs_free(current->value);
        current->value = new_value;
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
    if (!table || !table->header || !key) return NULL;//增加对header检查
    
    Node* current = table->header;
    
    // 从最高层开始搜索
    for (int i = table->level; i >= 0; i--) {
        while (current->forward[i] && 
               strcmp(current->forward[i]->key, key) < 0) {
            current = current->forward[i];
        }
    }
    
    current = current->forward[0];
    
    if (current && strcmp(current->key, key) == 0) {
        return current->value;
    }
    
    return NULL;
}

int kvs_skiptable_mod(kvs_skiptable_t *table, char *key, char *value) {
    if (!table || !table->header || !key || !value) return -1;  // 添加header检查

    Node* node = table->header;
    
    // 搜索节点
    for (int i = table->level; i >= 0; i--) {
        while (node->forward[i] && 
               strcmp(node->forward[i]->key, key) < 0) {
            node = node->forward[i];
        }
    }
    node = node->forward[0];
    
    if (node && strcmp(node->key, key) == 0) {
        size_t vlen = strlen(value);
        char *new_value = kvs_malloc(vlen + 1);
        if (!new_value) return -1;
        memcpy(new_value, value, vlen + 1);
        if (node->value) kvs_free(node->value);
        node->value = new_value;
        return 0;
    }
    
    return -1;  // 未找到
}

int kvs_skiptable_del(kvs_skiptable_t *table, char *key) {
    if (!table || !table->header  || !key) return -1;
    
    Node* update[MAX_LEVEL + 1];
    Node* current = table->header;
    
    // 查找要删除的节点及更新路径
    for (int i = table->level; i >= 0; i--) {
        while (current->forward[i] && 
               strcmp(current->forward[i]->key, key) < 0) {
            current = current->forward[i];
        }
        update[i] = current;
    }
    
    current = current->forward[0];
    
    // 如果节点不存在
    if (!current || strcmp(current->key, key) != 0) {
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
    //return table && table->header && kvs_skiptable_get(table, key) != NULL;
    return table && table->header && kvs_skiptable_get(table, key) == NULL;
}

int kvs_skiptable_sort(kvs_skiptable_t *table, int order, int limit, char *response, int max_len) {
    if (!table || !table->header || !response || max_len <= 0) return -1;  // 添加header检查
    
    Node* current;
    int pos = 0;
    int count = 0;

    // Node* current = table->header->forward[0];
    // int pos = 0;
    
    if (order == 0) { // 升序
        current = table->header->forward[0];
    }else { // 降序 - 需要先收集所有节点
        // 收集所有节点
        Node** nodes = kvs_malloc(table->count * sizeof(Node*));
        if (!nodes) return -1;
        
        current = table->header->forward[0];
        int i = 0;
        while (current) {
            nodes[i++] = current;
            current = current->forward[0];
        }
        
        // 反向遍历
        for (int j = i - 1; j >= 0 && (limit <= 0 || count < limit); j--) {
            int written = snprintf(response + pos, max_len - pos, 
                                  "%s:%s\r\n", nodes[j]->key, nodes[j]->value);
            if (written < 0 || written >= max_len - pos) break;
            pos += written;
            count++;
        }
        
        kvs_free(nodes);
        return pos;
    }

    // 升序遍历
    while (current && (limit <= 0 || count < limit) && pos < max_len) {
        int written = snprintf(response + pos, max_len - pos, 
                              "%s:%s\r\n", current->key, current->value);
        if (written < 0 || written >= max_len - pos) break;
        pos += written;
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

    // 处理 start_key > end_key 的情况（空范围）
    if (strcmp(start_key, end_key) > 0) {
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
               strcmp(current->forward[i]->key, start_key) < 0) {
            current = current->forward[i];
        }
    }
    current = current->forward[0];
    
    // 遍历范围内的节点
    while (current && strcmp(current->key, end_key) <= 0 && pos < max_len) {
        if (limit > 0 && returned >= limit) {
            break;
        }
        visited++;
        int written = snprintf(response + pos, max_len - pos, 
                              "%s:%s\r\n", current->key, current->value);
        if (written < 0 || written >= max_len - pos) break;
        pos += written;
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
