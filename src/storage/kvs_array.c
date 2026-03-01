

#include "kvs_api.h"
#include <string.h>
#include <stdlib.h>
#include <stdio.h>



// singleton

kvs_array_t global_array = {0};

static int kvs_array_find_index(kvs_array_t *inst, const char *key, int *out_idx) {

    if (!inst || !key || !out_idx) return -1;

    for (int i = 0; i < inst->len; i++) {
        if (strcmp(inst->table[i].key, key) == 0) {
            *out_idx = i;
            return 0;
        }
    }

    return 1;
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
    int idx = -1;
    if (kvs_array_find_index(inst, key, &idx) == 0) {
        return 1;
    }

    if (inst->len >= inst->capacity) {
        if (kvs_array_reserve(inst, inst->capacity + 1) != 0) {
            return -1;
        }
    }

	char *kcopy = kvs_malloc(strlen(key) + 1);
	if (kcopy == NULL) return -2;
	// memset(kcopy, 0, strlen(key) + 1);
	// strncpy(kcopy, key, strlen(key));
    strcpy(kcopy, key);

	char *kvalue = kvs_malloc(strlen(value) + 1);
	if (kvalue == NULL) {
        kvs_free(kcopy); // 释放已分配的内存
        return -2;
    }
	// memset(kvalue, 0, strlen(value) + 1);
	// strncpy(kvalue, value, strlen(value));
    strcpy(kvalue, value);

    inst->table[inst->len].key = kcopy;
    inst->table[inst->len].value = kvalue;
    inst->len++;
    inst->total = inst->len;

	return 0;
}

char* kvs_array_get(kvs_array_t *inst, char *key) {

	if (inst == NULL || key == NULL) return NULL;

    int idx = -1;
    if (kvs_array_find_index(inst, key, &idx) == 0) {
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
    if (kvs_array_find_index(inst, key, &idx) != 0) {
        return 1; // 键不存在
    }

    kvs_free(inst->table[idx].key);
    kvs_free(inst->table[idx].value);

    if (idx < inst->len - 1) {
        memmove(&inst->table[idx],
                &inst->table[idx + 1],
                (size_t)(inst->len - idx - 1) * sizeof(kvs_array_item_t));
    }

    inst->len--;
    memset(&inst->table[inst->len], 0, sizeof(kvs_array_item_t));
    inst->total = inst->len;

    return 0; // 成功删除
}


/*
 * @return : < 0, error; =0, success; >0, no exist 
 */

int kvs_array_mod(kvs_array_t *inst, char *key, char *value) {

	if (inst == NULL || key == NULL || value == NULL) return -1;
// error: > 1024
    if (inst->len == 0) {
		return KVS_ARRAY_SIZE;
	}

    int idx = -1;
    if (kvs_array_find_index(inst, key, &idx) == 0) {

            kvs_free(inst->table[idx].value);

			char *kvalue = kvs_malloc(strlen(value) + 1);
			if (kvalue == NULL) return -2;
			// memset(kvalue, 0, strlen(value) + 1);
			// strncpy(kvalue, value, strlen(value));
            strcpy(kvalue, value);

            inst->table[idx].value = kvalue;

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
    const kvs_array_item_t *itemA = (const kvs_array_item_t *)a;
    const kvs_array_item_t *itemB = (const kvs_array_item_t *)b;
    return strcmp(itemA->key, itemB->key);
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

    kvs_array_item_t *items = kvs_malloc((size_t)inst->len * sizeof(kvs_array_item_t));
    if(!items) return -1;

    int count = inst->len;
    int i;
    for(i = 0; i < count; i++){
        items[i] = inst->table[i];
    }

    //按键值排序
    qsort(items, count, sizeof(kvs_array_item_t), compare_array_items);

    //处理排序方向
    if(order == 1){
        for(i = 0;i < count/2; i++){
            kvs_array_item_t temp = items[i];
            items[i] = items[count - 1 - i];
            items[count - 1 - i] = temp;
        }
    }

    //数量限制
    int output_count = (limit > 0 && limit < count) ? limit : count;

    // 格式化输出
    int len = 0;
    for (int i = 0; i < output_count; i++) {
        int n = snprintf(response + len, max_len - len, 
                         "%s:%s\r\n", items[i].key, items[i].value);
        if (n < 0 || len + n >= max_len) break;
        len += n;
    }
    
    kvs_free(items);
    return len;
}
#endif

int kvs_array_range(kvs_array_t *inst, char *start_key, char *end_key, char *response, int max_len) {
    if (!inst || !start_key || !end_key) return -1;
    
    // 收集所有有效项
    kvs_array_item_t *items = kvs_malloc((size_t)inst->len * sizeof(kvs_array_item_t));
    if (!items) return -1;
    
    int count = inst->len;
    for (int i = 0; i < count; i++) {
        items[i] = inst->table[i];
    }
    
    // 按键排序
    qsort(items, count, sizeof(kvs_array_item_t), compare_array_items);
    
    // 格式化输出范围内的项
    int len = 0;
    for (int i = 0; i < count; i++) {
        if (strcmp(items[i].key, start_key) >= 0 && 
            strcmp(items[i].key, end_key) <= 0) {
            int n = snprintf(response + len, max_len - len, 
                             "%s:%s\r\n", items[i].key, items[i].value);
            if (n < 0 || len + n >= max_len) break;
            len += n;
        }
    }
    
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