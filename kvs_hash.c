


#include <stdio.h>
#include <string.h>
#include <stdlib.h>
#include <pthread.h>


#include "kvstore.h"


// Key, Value --> 
// Modify 



kvs_hash_t global_hash;


//Connection 
// 'C' + 'o' + 'n'
static int _hash(char *key, int size) {

	if (!key) return -1;

	int sum = 0;
	int i = 0;

	while (key[i] != 0) {
		sum += key[i];
		i ++;
	}

	return sum % size;

}

hashnode_t *_create_node(char *key, char *value) {

	hashnode_t *node = (hashnode_t*)kvs_malloc(sizeof(hashnode_t));
	if (!node) return NULL;
	
#if ENABLE_KEY_POINTER
	char *kcopy = kvs_malloc(strlen(key) + 1);
	if (kcopy == NULL) return NULL;
	memset(kcopy, 0, strlen(key) + 1);
	strncpy(kcopy, key, strlen(key));

	node->key = kcopy;

	char *kvalue = kvs_malloc(strlen(value) + 1);
	if (kvalue == NULL) { 
		kvs_free(kvalue);
		return NULL;
	}
	memset(kvalue, 0, strlen(value) + 1);
	strncpy(kvalue, value, strlen(value));

	node->value = kvalue;
	
#else
	strncpy(node->key, key, MAX_KEY_LEN);
	strncpy(node->value, value, MAX_VALUE_LEN);
#endif
	node->next = NULL;

	return node;
}


//
int kvs_hash_create(kvs_hash_t *hash) {

	if (!hash) return -1;

	hash->nodes = (hashnode_t**)kvs_malloc(sizeof(hashnode_t*) * MAX_TABLE_SIZE);
	if (!hash->nodes) return -1;

	hash->max_slots = MAX_TABLE_SIZE;
	hash->count = 0; 
	return 0;
}

// 
void kvs_hash_destory(kvs_hash_t *hash) {

	if (!hash) return;

	int i = 0;
	for (i = 0;i < hash->max_slots;i ++) {
		hashnode_t *node = hash->nodes[i];

		while (node != NULL) { // error

			hashnode_t *tmp = node;
			node = node->next;
			hash->nodes[i] = node;
			
			kvs_free(tmp);
			
		}
	}

	kvs_free(hash->nodes);
	
}

// 5 + 2

// mp
int kvs_hash_set(kvs_hash_t *hash, char *key, char *value) {

	if (!hash || !key || !value) return -1;

	int idx = _hash(key, MAX_TABLE_SIZE);

	hashnode_t *node = hash->nodes[idx];
#if 1
	while (node != NULL) {
		if (strcmp(node->key, key) == 0) { // exist
			return 1;
		}
		node = node->next;
	}
#endif

	hashnode_t *new_node = _create_node(key, value);
	new_node->next = hash->nodes[idx];
	hash->nodes[idx] = new_node;
	
	hash->count ++;

	return 0;
}


char * kvs_hash_get(kvs_hash_t *hash, char *key) {

	if (!hash || !key) return NULL;

	int idx = _hash(key, MAX_TABLE_SIZE);

	hashnode_t *node = hash->nodes[idx];

	while (node != NULL) {

		if (strcmp(node->key, key) == 0) {
			return node->value;
		}

		node = node->next;
	}

	return NULL;

}


int kvs_hash_mod(kvs_hash_t *hash, char *key, char *value) {

	if (!hash || !key) return -1;

	int idx = _hash(key, MAX_TABLE_SIZE);

	hashnode_t *node = hash->nodes[idx];

	while (node != NULL) {

		if (strcmp(node->key, key) == 0) {
			break;
		}

		node = node->next;
	}

	if (node == NULL) {
		return 1;
	}

	// node --> 
	kvs_free(node->value);

	char *kvalue = kvs_malloc(strlen(value) + 1);
	if (kvalue == NULL) return -2;
	memset(kvalue, 0, strlen(value) + 1);
	strncpy(kvalue, value, strlen(value));

	node->value = kvalue;
	
	return 0;
}

int kvs_hash_count(kvs_hash_t *hash) {
	return hash->count;
}

int kvs_hash_del(kvs_hash_t *hash, char *key) {
	if (!hash || !key) return -2;

	int idx = _hash(key, MAX_TABLE_SIZE);

	hashnode_t *head = hash->nodes[idx];
	if (head == NULL) return -1; // noexist
	// head node
	if (strcmp(head->key, key) == 0) {
		hashnode_t *tmp = head->next;
		hash->nodes[idx] = tmp;
		
		kvs_free(head);
		hash->count --;
		
		return 0;
	}

	hashnode_t *cur = head;
	while (cur->next != NULL) {
		if (strcmp(cur->next->key, key) == 0) break; // search node
		
		cur = cur->next;
	}

	if (cur->next == NULL) {
		
		return -1;
	}

	hashnode_t *tmp = cur->next;
	cur->next = tmp->next;
#if ENABLE_KEY_POINTER
	kvs_free(tmp->key);
	kvs_free(tmp->value);
#endif
	kvs_free(tmp);
	
	hash->count --;

	return 0;
}


int kvs_hash_exist(kvs_hash_t *hash, char *key) {

	char *value = kvs_hash_get(hash, key);
	if (!value) return 1;

	return 0;
	
}

// void *kvs_malloc(size_t size) {
// 	return malloc(size);
// }

// void kvs_free(void *ptr) {
// 	return free(ptr);
// }


// 比较函数用于排序
static int compare_hash_items(const void *a, const void *b) {
    const hashnode_t *nodeA = *(const hashnode_t **)a;
    const hashnode_t *nodeB = *(const hashnode_t **)b;
    return strcmp(nodeA->key, nodeB->key);
}

int kvs_hash_sort(kvs_hash_t *inst, int order, int limit, char *response, int max_len){
	if (!inst) return -1;
    
    // 收集所有节点
    hashnode_t **nodes = kvs_malloc(inst->count * sizeof(hashnode_t *));
    if (!nodes) return -1;
    
    int count = 0;
    for (int i = 0; i < inst->max_slots; i++) {
        hashnode_t *node = inst->nodes[i];
        while (node) {
            nodes[count++] = node;
            node = node->next;
        }
    }
    
    // 按键排序
    qsort(nodes, count, sizeof(hashnode_t *), compare_hash_items);
    
    // 处理排序方向
    if (order == 1) { // 降序
        for (int i = 0; i < count / 2; i++) {
            hashnode_t *temp = nodes[i];
            nodes[i] = nodes[count - 1 - i];
            nodes[count - 1 - i] = temp;
        }
    }

	// 处理数量限制
    int output_count = (limit > 0 && limit < count) ? limit : count;

	// 格式化输出
    int len = 0;
    for (int i = 0; i < output_count; i++) {
        int n = snprintf(response + len, max_len - len, 
                         "%s:%s\r\n", nodes[i]->key, nodes[i]->value);
        if (n < 0 || len + n >= max_len) break;
        len += n;
    }
    
    kvs_free(nodes);
    return len;
}
int kvs_hash_range(kvs_hash_t *inst, char *start_key, char *end_key, char *response, int max_len){
	if (!inst || !start_key || !end_key) return -1;
    
    // 收集所有节点
    hashnode_t **nodes = kvs_malloc(inst->count * sizeof(hashnode_t *));
    if (!nodes) return -1;
    
    int count = 0;
    for (int i = 0; i < inst->max_slots; i++) {
        hashnode_t *node = inst->nodes[i];
        while (node) {
            nodes[count++] = node;
            node = node->next;
        }
    }
    
    // 按键排序
    qsort(nodes, count, sizeof(hashnode_t *), compare_hash_items);
    
    // 格式化输出范围内的项
    int len = 0;
    for (int i = 0; i < count; i++) {
        if (strcmp(nodes[i]->key, start_key) >= 0 && 
            strcmp(nodes[i]->key, end_key) <= 0) {
            int n = snprintf(response + len, max_len - len, 
                             "%s:%s\r\n", nodes[i]->key, nodes[i]->value);
            if (n < 0 || len + n >= max_len) break;
            len += n;
        }
    }
    
    kvs_free(nodes);
    return len;
}

#if 0

int main() {
    // 初始化测试哈希表
    kvs_hash_t test_hash;
    test_hash.max_slots = 10;
    test_hash.count = 0;
    test_hash.nodes = kvs_malloc(test_hash.max_slots * sizeof(hashnode_t*));
    memset(test_hash.nodes, 0, test_hash.max_slots * sizeof(hashnode_t*));

    // 插入测试数据(故意乱序插入)
    const char *keys[] = {"banana", "apple", "grape", "cherry", "orange", "pear"};
    const char *values[] = {"yellow", "red", "purple", "dark red", "orange", "green"};
    int item_count = sizeof(keys)/sizeof(keys[0]);
    
    for (int i = 0; i < item_count; i++) {
        // 简单哈希函数计算槽位
        unsigned int slot = 0;
        for (const char *p = keys[i]; *p; p++) {
            slot = slot * 31 + *p;
        }
        slot %= test_hash.max_slots;
        
        // 创建新节点
        hashnode_t *node = kvs_malloc(sizeof(hashnode_t));
        node->key = kvs_malloc(strlen(keys[i]) + 1);
        strcpy(node->key, keys[i]);
        node->value = kvs_malloc(strlen(values[i]) + 1);
        strcpy(node->value, values[i]);
        
        // 插入到链表头部
        node->next = test_hash.nodes[slot];
        test_hash.nodes[slot] = node;
        test_hash.count++;
    }

    // 测试1: 排序功能
    printf("=== Testing kvs_hash_sort ===\n");
    char response[1024];
    int len = kvs_hash_sort(&test_hash, response, sizeof(response));
    if (len > 0) {
        printf("Sorted items:\n%s", response);
    } else {
        printf("Sort failed\n");
    }

    // 测试2: 范围查询
    printf("\n=== Testing kvs_hash_range ===\n");
    printf("Range from 'banana' to 'orange':\n");
    len = kvs_hash_range(&test_hash, "banana", "orange", response, sizeof(response));
    if (len > 0) {
        printf("%s", response);
    } else {
        printf("Range query failed\n");
    }

    // 测试3: 边界情况
    printf("\n=== Testing edge cases ===\n");
    // 空哈希表测试
    kvs_hash_t empty_hash = {0};
    printf("Empty hash sort: %d (expected 0)\n", 
           kvs_hash_sort(&empty_hash, response, sizeof(response)));
    
    // 无效范围测试
    printf("Invalid range test: %d (expected 0)\n", 
           kvs_hash_range(&test_hash, "zebra", "aardvark", response, sizeof(response)));

    // 清理
    for (int i = 0; i < test_hash.max_slots; i++) {
        hashnode_t *node = test_hash.nodes[i];
        while (node) {
            hashnode_t *temp = node;
            node = node->next;
            kvs_free(temp->key);
            kvs_free(temp->value);
            kvs_free(temp);
        }
    }
    kvs_free(test_hash.nodes);
    
    return 0;
}

#endif


