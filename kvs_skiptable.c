#include "kvstore.h"
#include <time.h>


Node* createNode(int level, char* key, char* value) {
    Node* newNode = (Node*)malloc(sizeof(Node));
    if (!newNode) return NULL;
    
    // 复制字符串
    newNode->key = strdup(key);
    newNode->value = strdup(value);
    
    if (!newNode->key || !newNode->value) {
        if (newNode->key) kvs_free(newNode->key);
        if (newNode->value) kvs_free(newNode->value);
        kvs_free(newNode);
        return NULL;
    }
    
    newNode->forward = (Node**)malloc((level + 1) * sizeof(Node*));
    if (!newNode->forward) {
        kvs_free(newNode->key);
        kvs_free(newNode->value);
        kvs_free(newNode);
        return NULL;
    }
    
    return newNode;
}

SkipList* createSkipList() {
    SkipList* skipList = (SkipList*)malloc(sizeof(SkipList));
    if (!skipList) return NULL;
    
    skipList->level = 0;
    
    // 使用空键创建头节点
    skipList->header = createNode(MAX_LEVEL, "", "");
    if (!skipList->header) {
        kvs_free(skipList);
        return NULL;
    }
    
    for (int i = 0; i <= MAX_LEVEL; ++i) {
        skipList->header->forward[i] = NULL;
    }
    
   return skipList; 
}

int randomLevel() {
   int level = 0;
   while (rand() < RAND_MAX / 2 && level < MAX_LEVEL)
      level++;
   return level;
}

// 插入函数（支持字符串键值）
bool insert(SkipList* skipList, char* key, char* value) {
   if (!skipList || !key || !value) return false;
   
   Node* update[MAX_LEVEL + 1];
   Node* current = skipList->header;

   for (int i = skipList->level; i >= 0; --i) {
      while (current->forward[i] != NULL && 
             strcmp(current->forward[i]->key, key) < 0) {
         current = current->forward[i];
      }
      update[i] = current;
   }

   current = current->forward[0];

   if (current == NULL || strcmp(current->key, key) != 0) {
      int level = randomLevel();

      if (level > skipList->level) {
         for (int i = skipList->level + 1; i <= level; ++i)
            update[i] = skipList->header;
         skipList->level = level;
      }

      Node* newNode = createNode(level, key, value);
      if (!newNode) return false;

      for (int i = 0; i <= level; ++i) {
         newNode->forward[i] = update[i]->forward[i];
         update[i]->forward[i] = newNode;
      }
      
      //printf("Inserted key %s\n", key);
      return true;
   } else {
       printf("Key %s already exists\n", key);
       return false;
   }
}

// 显示函数（支持字符串键值）
void display(SkipList* skipList) {
    if (!skipList) return;
    
    printf("Skip List:\n");
    
    for (int i = 0; i <= skipList->level; ++i) {
        Node* node = skipList->header->forward[i];
        printf("Level %d: ", i);
        
        while (node != NULL) {
            printf("%s ", node->key);
            node = node->forward[i];
        }
        
        printf("\n");
    }
}

// 查找函数（支持字符串键值）
bool search(SkipList* skipList, char* key) {
    if (!skipList || !key) return false;
    
    Node* current = skipList->header;

    for (int i = skipList->level; i >= 0; --i) {
        while (current->forward[i] != NULL && 
               strcmp(current->forward[i]->key, key) < 0) {
            current = current->forward[i];
        }
    }

    current = current->forward[0];

    if (current && strcmp(current->key, key) == 0) {
        printf("Key %s found with value %s\n", key, current->value);
        return true;
    } else {
        printf("Key %s not found\n", key);
        return false;
    }
}



kvs_skiptable_t global_skiptable;

int kvs_skiptable_create(kvs_skiptable_t *table){
    if (!table) return -1;
    
    SkipList* sl = createSkipList();
    if (!sl) return -1;
    
    *table = *sl;
    kvs_free(sl);
    return 0;
}
void kvs_skiptable_destroy(kvs_skiptable_t *table){
    if (!table || !table->header) return;
    
    Node* current = table->header;
    while (current) {
        Node* next = current->forward[0];
        
        if (current->key) kvs_free(current->key);
        if (current->value) kvs_free(current->value);
        if (current->forward) kvs_free(current->forward);
        
        kvs_free(current);
        current = next;
    }
    
    table->header = NULL;
    table->level = 0;
}

int kvs_skiptable_set(kvs_skiptable_t *table, char *key, char *value){
    if (!table || !key || !value) return -1;
    return insert(table, key, value) ? 0 : 1;
}
char * kvs_skiptable_get(kvs_skiptable_t *table, char *key){
    if (!table || !key) return NULL;
    
    Node* current = table->header;
    for (int i = table->level; i >= 0; --i) {
        while (current->forward[i] && 
               strcmp(current->forward[i]->key, key) < 0) {
            current = current->forward[i];
        }
    }
    
    current = current->forward[0];
    return (current && strcmp(current->key, key) == 0) ? current->value : NULL;
}
int kvs_skiptable_mod(kvs_skiptable_t *table, char *key, char *value){
    if (!table || !key || !value) return -1;
    
    Node* current = table->header;
    for (int i = table->level; i >= 0; --i) {
        while (current->forward[i] && 
               strcmp(current->forward[i]->key, key) < 0) {
            current = current->forward[i];
        }
    }
    
    current = current->forward[0];
    if (!current || strcmp(current->key, key) != 0) {
        return 1; // 键不存在
    }
    
    char* new_value = strdup(value);
    if (!new_value) return -1;
    
    kvs_free(current->value);
    current->value = new_value;
    return 0;
}
int kvs_skiptable_del(kvs_skiptable_t *table, char *key){
    if (!table || !key) return -1;
    
    Node* update[MAX_LEVEL + 1];
    Node* current = table->header;
    
    // 查找节点并记录路径
    for (int i = table->level; i >= 0; --i) {
        while (current->forward[i] && 
               strcmp(current->forward[i]->key, key) < 0) {
            current = current->forward[i];
        }
        update[i] = current;
    }
    
    current = current->forward[0];
    if (!current || strcmp(current->key, key) != 0) {
        return 1; // 键不存在
    }
    
    // 从各层链表中移除节点
    for (int i = 0; i <= table->level; i++) {
        if (update[i]->forward[i] != current) break;
        update[i]->forward[i] = current->forward[i];
    }
    
    // 释放节点内存
    kvs_free(current->key);
    kvs_free(current->value);
    kvs_free(current->forward);
    kvs_free(current);
    
    // 更新跳表层数
    while (table->level > 0 && table->header->forward[table->level] == NULL) {
        table->level--;
    }
    
    return 0;
}
int kvs_skiptable_exist(kvs_skiptable_t *table, char *key){
    return (kvs_skiptable_get(table, key) != NULL) ? 0 : 1;
}

// void *kvs_malloc(size_t size) {
// 	return malloc(size);
// }

// void kvs_free(void *ptr) {
// 	return free(ptr);
// }

int kvs_skiptable_sort(kvs_skiptable_t *table, char *response, int max_len) {
    if (!table) return -1;
    
    int len = 0;
    Node *current = table->header->forward[0];
    
    while (current != NULL) {
        int n = snprintf(response + len, max_len - len, 
                         "%s:%s\r\n", current->key, current->value);
        if (n < 0 || len + n >= max_len) break;
        len += n;
        current = current->forward[0];
    }
    
    return len;
}

int kvs_skiptable_range(kvs_skiptable_t *table, char *start_key, char *end_key, 
                        char *response, int max_len) {
    if (!table || !start_key || !end_key) return -1;
    
    int len = 0;
    Node *current = table->header->forward[0];
    
    while (current != NULL) {
        if (strcmp(current->key, start_key) >= 0) {
            if (strcmp(current->key, end_key) <= 0) {
                int n = snprintf(response + len, max_len - len, 
                                 "%s:%s\r\n", current->key, current->value);
                if (n < 0 || len + n >= max_len) break;
                len += n;
            } else {
                // 超出结束键，停止遍历
                break;
            }
        }
        current = current->forward[0];
    }
    
    return len;
}

#if 0
int main() {
    // 初始化随机数种子
    srand(time(NULL));
    
    // 创建跳表
    kvs_skiptable_t table;
    if (kvs_skiptable_create(&table) != 0) {
        printf("Failed to create skip table\n");
        return -1;
    }

    // 插入测试数据(故意乱序插入)
    const char *keys[] = {"banana", "apple", "grape", "cherry", "orange", "pear", "mango", "kiwi"};
    const char *values[] = {"yellow", "red", "purple", "dark red", "orange", "green", "yellow-red", "brown"};
    int item_count = sizeof(keys)/sizeof(keys[0]);
    
    for (int i = 0; i < item_count; i++) {
        if (kvs_skiptable_set(&table, (char*)keys[i], (char*)values[i]) != 0) {
            printf("Failed to insert key: %s\n", keys[i]);
        }
    }

    // 测试1: 排序功能
    printf("=== Testing kvs_skiptable_sort ===\n");
    char response[1024];
    int len = kvs_skiptable_sort(&table, response, sizeof(response));
    if (len > 0) {
        printf("Sorted items:\n%s", response);
    } else {
        printf("Sort failed\n");
    }

    // 测试2: 范围查询
    printf("\n=== Testing kvs_skiptable_range ===\n");
    printf("Range from 'cherry' to 'orange':\n");
    len = kvs_skiptable_range(&table, "cherry", "orange", response, sizeof(response));
    if (len > 0) {
        printf("%s", response);
    } else {
        printf("Range query failed\n");
    }

    // 测试3: 边界情况
    printf("\n=== Testing edge cases ===\n");
    // 空跳表测试
    kvs_skiptable_t empty_table;
    kvs_skiptable_create(&empty_table);
    printf("Empty table sort: %d (expected 0)\n", 
           kvs_skiptable_sort(&empty_table, response, sizeof(response)));
    
    // 无效范围测试
    printf("Invalid range test: %d (expected 0)\n", 
           kvs_skiptable_range(&table, "zebra", "aardvark", response, sizeof(response)));
    
    // 测试4: 精确边界测试
    printf("\n=== Testing exact boundaries ===\n");
    printf("Range from 'apple' to 'pear':\n");
    len = kvs_skiptable_range(&table, "apple", "pear", response, sizeof(response));
    if (len > 0) {
        printf("%s", response);
    }
    
    // 测试5: 单键范围测试
    printf("\nSingle key range (banana to banana):\n");
    len = kvs_skiptable_range(&table, "banana", "banana", response, sizeof(response));
    if (len > 0) {
        printf("%s", response);
    }

    // 测试6: 不存在的范围测试
    printf("\nNon-existent range (watermelon to zucchini):\n");
    len = kvs_skiptable_range(&table, "watermelon", "zucchini", response, sizeof(response));
    printf("Result length: %d\n", len);

    // 清理
    kvs_skiptable_destroy(&table);
    kvs_skiptable_destroy(&empty_table);
    
    return 0;
}
#endif