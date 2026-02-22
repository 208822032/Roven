


#include <stdio.h>
#include <stdlib.h>
#include <string.h>


#include "kvs_api.h"

rbtree_node *rbtree_mini(rbtree *T, rbtree_node *x) {
	if (x == T->nil) return T->nil; // 空树检查
	while (x->left != T->nil) {
		x = x->left;
	}
	return x;
}

rbtree_node *rbtree_maxi(rbtree *T, rbtree_node *x) {
	while (x->right != T->nil) {
		x = x->right;
	}
	return x;
}

rbtree_node *rbtree_successor(rbtree *T, rbtree_node *x) {
	rbtree_node *y = x->parent;

	if (x->right != T->nil) {
		return rbtree_mini(T, x->right);
	}

	while ((y != T->nil) && (x == y->right)) {
		x = y;
		y = y->parent;
	}
	return y;
}


void rbtree_left_rotate(rbtree *T, rbtree_node *x) {

	rbtree_node *y = x->right;  // x  --> y  ,  y --> x,   right --> left,  left --> right

	x->right = y->left; //1 1
	if (y->left != T->nil) { //1 2
		y->left->parent = x;
	}

	y->parent = x->parent; //1 3
	if (x->parent == T->nil) { //1 4
		T->root = y;
	} else if (x == x->parent->left) {
		x->parent->left = y;
	} else {
		x->parent->right = y;
	}

	y->left = x; //1 5
	x->parent = y; //1 6
}


void rbtree_right_rotate(rbtree *T, rbtree_node *y) {

	rbtree_node *x = y->left;

	y->left = x->right;
	if (x->right != T->nil) {
		x->right->parent = y;
	}

	x->parent = y->parent;
	if (y->parent == T->nil) {
		T->root = x;
	} else if (y == y->parent->right) {
		y->parent->right = x;
	} else {
		y->parent->left = x;
	}

	x->right = y;
	y->parent = x;
}

void rbtree_insert_fixup(rbtree *T, rbtree_node *z) {

	while (z->parent->color == RED) { //z ---> RED
		if (z->parent == z->parent->parent->left) {
			rbtree_node *y = z->parent->parent->right;
			if (y->color == RED) {
				z->parent->color = BLACK;
				y->color = BLACK;
				z->parent->parent->color = RED;

				z = z->parent->parent; //z --> RED
			} else {

				if (z == z->parent->right) {
					z = z->parent;
					rbtree_left_rotate(T, z);
				}

				z->parent->color = BLACK;
				z->parent->parent->color = RED;
				rbtree_right_rotate(T, z->parent->parent);
			}
		}else {
			rbtree_node *y = z->parent->parent->left;
			if (y->color == RED) {
				z->parent->color = BLACK;
				y->color = BLACK;
				z->parent->parent->color = RED;

				z = z->parent->parent; //z --> RED
			} else {
				if (z == z->parent->left) {
					z = z->parent;
					rbtree_right_rotate(T, z);
				}

				z->parent->color = BLACK;
				z->parent->parent->color = RED;
				rbtree_left_rotate(T, z->parent->parent);
			}
		}
		
	}

	T->root->color = BLACK;
}


void rbtree_insert(rbtree *T, rbtree_node *z) {

	rbtree_node *y = T->nil;
	rbtree_node *x = T->root;

	while (x != T->nil) {
		y = x;
#if ENABLE_KEY_CHAR

		if (strcmp(z->key, x->key) < 0) {
			x = x->left;
		} else if (strcmp(z->key, x->key) > 0) {
			x = x->right;
		} else {//如果已存在值
			kvs_free(z->key);
			kvs_free(z->value);
			kvs_free(z);
			return;
		}

#else
		if (z->key < x->key) {
			x = x->left;
		} else if (z->key > x->key) {
			x = x->right;
		} else { //Exist
			return ;
		}
#endif
	}

	z->parent = y;
	if (y == T->nil) {
		T->root = z;
#if ENABLE_KEY_CHAR
	} else if (strcmp(z->key, y->key) < 0) {
#else
	} else if (z->key < y->key) {
#endif
		y->left = z;
	} else {
		y->right = z;
	}

	z->left = T->nil;
	z->right = T->nil;
	z->color = RED;

	rbtree_insert_fixup(T, z);
}

void rbtree_delete_fixup(rbtree *T, rbtree_node *x) {

	while ((x != T->root) && (x->color == BLACK)) {
		if (x == x->parent->left) {

			rbtree_node *w= x->parent->right;
			if (w->color == RED) {
				w->color = BLACK;
				x->parent->color = RED;

				rbtree_left_rotate(T, x->parent);
				w = x->parent->right;
			}

			if ((w->left->color == BLACK) && (w->right->color == BLACK)) {
				w->color = RED;
				x = x->parent;
			} else {

				if (w->right->color == BLACK) {
					w->left->color = BLACK;
					w->color = RED;
					rbtree_right_rotate(T, w);
					w = x->parent->right;
				}

				w->color = x->parent->color;
				x->parent->color = BLACK;
				w->right->color = BLACK;
				rbtree_left_rotate(T, x->parent);

				x = T->root;
			}

		} else {

			rbtree_node *w = x->parent->left;
			if (w->color == RED) {
				w->color = BLACK;
				x->parent->color = RED;
				rbtree_right_rotate(T, x->parent);
				w = x->parent->left;
			}

			if ((w->left->color == BLACK) && (w->right->color == BLACK)) {
				w->color = RED;
				x = x->parent;
			} else {

				if (w->left->color == BLACK) {
					w->right->color = BLACK;
					w->color = RED;
					rbtree_left_rotate(T, w);
					w = x->parent->left;
				}

				w->color = x->parent->color;
				x->parent->color = BLACK;
				w->left->color = BLACK;
				rbtree_right_rotate(T, x->parent);

				x = T->root;
			}

		}
	}

	x->color = BLACK;
}

rbtree_node *rbtree_delete(rbtree *T, rbtree_node *z) {

	rbtree_node *y = T->nil;
	rbtree_node *x = T->nil;

	if ((z->left == T->nil) || (z->right == T->nil)) {
		y = z;
	} else {
		y = rbtree_successor(T, z);
	}

	if (y->left != T->nil) {
		x = y->left;
	} else if (y->right != T->nil) {
		x = y->right;
	}

	x->parent = y->parent;
	if (y->parent == T->nil) {
		T->root = x;
	} else if (y == y->parent->left) {
		y->parent->left = x;
	} else {
		y->parent->right = x;
	}

	if (y != z) {
#if ENABLE_KEY_CHAR

		void *tmp = z->key;
		z->key = y->key;
		y->key = tmp;

		tmp = z->value;
		z->value= y->value;
		y->value = tmp;

#else
		z->key = y->key;
		z->value = y->value;
#endif
	}

	if (y->color == BLACK) {
		rbtree_delete_fixup(T, x);
	}

	// 释放节点内存
    kvs_free(y->key);
    kvs_free(y->value);
    kvs_free(y);

	return y;
}

rbtree_node *rbtree_search(rbtree *T, KEY_TYPE key) {

	rbtree_node *node = T->root;
	while (node != T->nil) {
#if ENABLE_KEY_CHAR

		if (strcmp(key, node->key) < 0) {
			node = node->left;
		} else if (strcmp(key, node->key) > 0) {
			node = node->right;
		} else {
			return node;
		}

#else
		if (key < node->key) {
			node = node->left;
		} else if (key > node->key) {
			node = node->right;
		} else {
			return node;
		}	
#endif
	}
	return T->nil;
}


void rbtree_traversal(rbtree *T, rbtree_node *node) {
	if (node != T->nil) {
		rbtree_traversal(T, node->left);
#if ENABLE_KEY_CHAR
		printf("key:%s, value:%s\n", node->key, (char *)node->value);
#else
		printf("key:%d, color:%d\n", node->key, node->color);
#endif
		rbtree_traversal(T, node->right);
	}
}


#if 0

int main() {

#if ENABLE_KEY_CHAR

	char* keyArray[10] = {"King", "Darren", "Mark", "Vico", "Nick", "qiuxiang", "youzi", "taozi", "123", "234"};
	char* valueArray[10] = {"1King", "2Darren", "3Mark", "4Vico", "5Nick", "6qiuxiang", "7youzi", "8taozi", "9123", "10234"};

	rbtree *T = (rbtree *)malloc(sizeof(rbtree));
	if (T == NULL) {
		printf("malloc failed\n");
		return -1;
	}
	
	T->nil = (rbtree_node*)malloc(sizeof(rbtree_node));
	T->nil->color = BLACK;
	T->root = T->nil;

	rbtree_node *node = T->nil;
	int i = 0;
	for (i = 0;i < 10;i ++) {
		node = (rbtree_node*)malloc(sizeof(rbtree_node));
		
		node->key = malloc(strlen(keyArray[i]) + 1);
		memset(node->key, 0, strlen(keyArray[i]) + 1);
		strcpy(node->key, keyArray[i]);
		
		node->value = malloc(strlen(valueArray[i]) + 1);
		memset(node->value, 0, strlen(valueArray[i]) + 1);
		strcpy(node->value, valueArray[i]);

		rbtree_insert(T, node);
		
	}

	rbtree_traversal(T, T->root);
	printf("----------------------------------------\n");

	for (i = 0;i < 10;i ++) {

		rbtree_node *node = rbtree_search(T, keyArray[i]);
		rbtree_node *cur = rbtree_delete(T, node);
		free(cur);

		rbtree_traversal(T, T->root);
		printf("----------------------------------------\n");
	}

#else


	int keyArray[20] = {24,25,13,35,23, 26,67,47,38,98, 20,19,17,49,12, 21,9,18,14,15};

	rbtree *T = (rbtree *)malloc(sizeof(rbtree));
	if (T == NULL) {
		printf("malloc failed\n");
		return -1;
	}
	
	T->nil = (rbtree_node*)malloc(sizeof(rbtree_node));
	T->nil->color = BLACK;
	T->root = T->nil;

	rbtree_node *node = T->nil;
	int i = 0;
	for (i = 0;i < 20;i ++) {
		node = (rbtree_node*)malloc(sizeof(rbtree_node));
		node->key = keyArray[i];
		node->value = NULL;

		rbtree_insert(T, node);
		
	}

	rbtree_traversal(T, T->root);
	printf("----------------------------------------\n");

	for (i = 0;i < 20;i ++) {

		rbtree_node *node = rbtree_search(T, keyArray[i]);
		rbtree_node *cur = rbtree_delete(T, node);
		free(cur);

		rbtree_traversal(T, T->root);
		printf("----------------------------------------\n");
	}
#endif

	
}

#endif


typedef struct _rbtree kvs_rbtree_t; 
kvs_rbtree_t global_rbtree;


// 5 + 2
int kvs_rbtree_create(kvs_rbtree_t *inst) {

	if (inst == NULL) return 1;

	inst->nil = (rbtree_node*)kvs_malloc(sizeof(rbtree_node));
	if (!inst->nil) return 1; // 分配失败检查

	inst->nil->color = BLACK;
	inst->root = inst->nil;
#if 1
	inst->nil->right = inst->nil;
    inst->nil->parent = inst->nil;
	inst->root = inst->nil;
#endif
	return 0;

}

void kvs_rbtree_destory(kvs_rbtree_t *inst) {

	if (inst == NULL) return ;

	rbtree_node *node = NULL;

	//while (!(node = inst->root)) {
	while(inst->root != inst->nil){
		
		//rbtree_node *mini = rbtree_mini(inst, node);
		rbtree_node *mini = rbtree_mini(inst, inst->root);
		
		rbtree_node *cur = rbtree_delete(inst, mini);
		//kvs_free(cur);
		
	}

	kvs_free(inst->nil);
	inst->nil = NULL;
	inst->root = NULL;

	return ;

}

// 辅助函数：安全字符串复制
// char* kvs_strdup(const char *src) {
//     if (!src) return NULL;
//     size_t len = strlen(src) + 1;
//     char *dest = kvs_malloc(len);
//     if (dest) memcpy(dest, src, len);  // 比strcpy更安全
//     return dest;
// }

int kvs_rbtree_set(kvs_rbtree_t *inst, char *key, char *value) {

	if (!inst || !key || !value || inst->nil == NULL)  return -1;


	rbtree_node *node = (rbtree_node*)kvs_malloc(sizeof(rbtree_node));
	if (!node) return -2;


	// 分配并复制键
    node->key = kvs_strdup(key);
    if (!node->key) {
        kvs_free(node);
        return -2;
    }


	node->value = kvs_strdup(value);
	if (!node->value) {
        kvs_free(node->key);
        kvs_free(node);
        return -2;
    }

	rbtree_insert(inst, node);

	return 0;
}


char* kvs_rbtree_get(kvs_rbtree_t *inst, char *key)  {

	if (!inst || !key) return NULL;
	rbtree_node *node = rbtree_search(inst, key);
	if (!node) return NULL; // no exist
	if (node == inst->nil) return NULL;

	return node->value;
	
}

int kvs_rbtree_del(kvs_rbtree_t *inst, char *key) {

	if (!inst || !key) return -1;

	rbtree_node *node = rbtree_search(inst, key);
	if (!node || node == inst->nil) return 1; // 键不存在
	
	rbtree_node *cur = rbtree_delete(inst, node);
	
#if 0
	// 释放节点内存
    kvs_free(cur->key);
    kvs_free(cur->value);
    kvs_free(cur);
#endif

	return 0;
}

int kvs_rbtree_mod(kvs_rbtree_t *inst, char *key, char *value) {

	if (!inst || !key || !value) return -1;

	rbtree_node *node = rbtree_search(inst, key);
	if (!node) return 1; // no exist
	if (node == inst->nil) return 1;
	
	kvs_free(node->value);

	node->value = kvs_malloc(strlen(value) + 1);
	if (!node->value) return -2;
	
	memset(node->value, 0, strlen(value) + 1);
	strcpy(node->value, value);

	return 0;

}

int kvs_rbtree_exist(kvs_rbtree_t *inst, char *key) {

	if (!inst || !key) return -1;

	rbtree_node *node = rbtree_search(inst, key);
	if (!node) return 1; // no exist
	if (node == inst->nil) return 1;

	return 0;
}

// void *kvs_malloc(size_t size) {
// 	return malloc(size);
// }

// void kvs_free(void *ptr) {
// 	return free(ptr);
// }

// 中序遍历辅助函数
static void inorder_traversal_with_limit(rbtree *T, rbtree_node *node, int order, int *count, int limit, char *response, int *len, int max_len) {
    if (node == T->nil) return;
    
	if(order == 0){//升序
		inorder_traversal_with_limit(T, node->left, order, count, limit, response, len, max_len);

		if (*count < limit || limit <= 0) {
            int n = snprintf(response + *len, max_len - *len, 
                             "%s:%s\r\n", node->key, (char*)node->value);
            if (n > 0 && *len + n < max_len) {
                *len += n;
                (*count)++;
            }
        }

		if (*count < limit || limit <= 0) {
            inorder_traversal_with_limit(T, node->right, order, count, limit, response, len, max_len);
        }
	}else{//升序

		inorder_traversal_with_limit(T, node->right, order, count, limit, response, len, max_len);

		if (*count < limit || limit <= 0) {
            int n = snprintf(response + *len, max_len - *len, 
                             "%s:%s\r\n", node->key, (char*)node->value);
            if (n > 0 && *len + n < max_len) {
                *len += n;
                (*count)++;
            }
        }

		if (*count < limit || limit <= 0) {
            inorder_traversal_with_limit(T, node->left, order, count, limit, response, len, max_len);
        }

	}
}

int kvs_rbtree_sort(kvs_rbtree_t *inst, int order, int limit, char *response, int max_len){
	if (!inst) return -1;
    
    int len = 0;
    int count = 0;
    inorder_traversal_with_limit(inst, inst->root, order, &count, limit, response, &len, max_len);
    return len;
}


// 中序遍历辅助函数（支持 limit 和 early stop）
static void inorder_traversal_with_range_limit(rbtree *T, rbtree_node *node,
							  char *start_key, char *end_key, int limit,
							  int *returned, int *visited, int *stop,
							  char *response, int *len, int max_len) {
	if (node == T->nil || *stop) return;
	(*visited)++;

	if (strcmp(node->key, start_key) >= 0) {
		inorder_traversal_with_range_limit(T, node->left, start_key, end_key, limit,
									   returned, visited, stop, response, len, max_len);
	}

	if (*stop) return;

	if (strcmp(node->key, start_key) >= 0 &&
		strcmp(node->key, end_key) <= 0) {
		int n = snprintf(response + *len, max_len - *len,
						 "%s:%s\r\n", node->key, (char*)node->value);
		if (n > 0 && *len + n < max_len) {
			*len += n;
			(*returned)++;
			if (limit > 0 && *returned >= limit) {
				*stop = 1;
				return;
			}
		}
	}

	if (strcmp(node->key, end_key) <= 0) {
		inorder_traversal_with_range_limit(T, node->right, start_key, end_key, limit,
									   returned, visited, stop, response, len, max_len);
	}
}


int kvs_rbtree_range(kvs_rbtree_t *inst, char *start_key, char *end_key, int limit, char *response, int max_len){
	if (!inst || !start_key || !end_key) return -1;
    
    int len = 0;
	int returned = 0;
	int visited = 0;
	int stop = 0;
	inorder_traversal_with_range_limit(inst, inst->root, start_key, end_key, limit,
									   &returned, &visited, &stop, response, &len, max_len);
#if KVS_RANGE_VISITED_DEBUG
	fprintf(stderr, "[RANGE_VISITED][RRANGE] start=%s end=%s limit=%d returned=%d visited=%d\n",
			start_key, end_key, limit, returned, visited);
#endif
    return len;
}

// 中序遍历红黑树并调用回调函数处理每个节点
void rbtree_traversal_with_callback(rbtree *T, rbtree_node *node, 
                                          void (*callback)(rbtree_node *, void *), 
                                          void *userdata) {
    if (node == T->nil) return;
    
    rbtree_traversal_with_callback(T, node->left, callback, userdata);
    callback(node, userdata);
    rbtree_traversal_with_callback(T, node->right, callback, userdata);
}
#if 0
int main(){
	// 初始化红黑树
    kvs_rbtree_t tree;
    if (kvs_rbtree_create(&tree) != 0) {
        printf("Failed to create RB tree\n");
        return -1;
    }

	// 插入测试数据(故意乱序插入)
    const char *keys[] = {"banana", "apple", "grape", "cherry", "orange", "pear"};
    const char *values[] = {"yellow", "red", "purple", "dark red", "orange", "green"};
    int count = sizeof(keys)/sizeof(keys[0]);
    
    for (int i = 0; i < count; i++) {
        if (kvs_rbtree_set(&tree, (char*)keys[i], (char*)values[i]) != 0) {
            printf("Failed to insert key: %s\n", keys[i]);
        }
    }

	// 测试1: 排序功能
    printf("=== Testing kvs_rbtree_sort ===\n");
    char response[1024];
    int len = kvs_rbtree_sort(&tree, response, sizeof(response));
    if (len > 0) {
        printf("Sorted items:\n%s", response);
    } else {
        printf("Sort failed\n");
    }

	// 测试2: 范围查询
    printf("\n=== Testing kvs_rbtree_range ===\n");
    printf("Range from 'banana' to 'orange':\n");
	len = kvs_rbtree_range(&tree, "banana", "orange", -1, response, sizeof(response));
    if (len > 0) {
        printf("%s", response);
    } else {
        printf("Range query failed\n");
    }

	// 测试3: 边界情况
    printf("\n=== Testing edge cases ===\n");
    // 空树测试
    kvs_rbtree_t empty_tree;
    kvs_rbtree_create(&empty_tree);
    printf("Empty tree sort: %d (expected 0)\n", 
           kvs_rbtree_sort(&empty_tree, response, sizeof(response)));
    
    // 无效范围测试
    printf("Invalid range test: %d (expected 0)\n", 
		   kvs_rbtree_range(&tree, "zebra", "aardvark", -1, response, sizeof(response)));

    // 清理
    kvs_rbtree_destory(&tree);
    kvs_rbtree_destory(&empty_tree);
    return 0;
}
#endif