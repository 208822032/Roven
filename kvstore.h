


#ifndef __KV_STORE_H__
#define __KV_STORE_H__


#include <stddef.h>
#include <stdio.h>
#include <string.h>
#include <stdlib.h>
#include <assert.h>
#include <stdbool.h>
#include <sys/stat.h>
#include <signal.h>
#include <unistd.h>
#include <pthread.h>
#include <sys/mman.h>



#define NETWORK_REACTOR 	0
#define NETWORK_PROACTOR	1
#define NETWORK_NTYCO		2

#define NETWORK_SELECT		NETWORK_NTYCO



#define KVS_MAX_TOKENS		128
#define KVS_MAX_RESPONSE_SIZE  2048
#define KVS_MAX_LINE        1000

#define ENABLE_ARRAY		1
#define ENABLE_RBTREE		1
#define ENABLE_HASH			1
#define ENABLE_SKIPTABLE    1


typedef int (*msg_handler)(char *msg, int length, char *response);


extern int reactor_start(unsigned short port, msg_handler handler);
extern int proactor_start(unsigned short port, msg_handler handler);
extern int ntyco_start(unsigned short port, msg_handler handler);
extern int g_shutdown_flag;



#if ENABLE_ARRAY

typedef struct kvs_array_item_s {
	char *key;
	char *value;
} kvs_array_item_t;

#define KVS_ARRAY_SIZE		1024

typedef struct kvs_array_s {
	kvs_array_item_t *table;
	int idx;
	int total;
} kvs_array_t;

int kvs_array_create(kvs_array_t *inst);
void kvs_array_destory(kvs_array_t *inst);

int kvs_array_set(kvs_array_t *inst, char *key, char *value);
char* kvs_array_get(kvs_array_t *inst, char *key);
int kvs_array_del(kvs_array_t *inst, char *key);
int kvs_array_mod(kvs_array_t *inst, char *key, char *value);
int kvs_array_exist(kvs_array_t *inst, char *key);
//int kvs_array_sort(kvs_array_t *inst, char *response, int max_len);
int kvs_array_sort(kvs_array_t *inst, int order, int limit, char *response, int max_len);
int kvs_array_range(kvs_array_t *inst, char *start_key, char *end_key, char *response, int max_len);


#endif


#if ENABLE_RBTREE

#define RED				1
#define BLACK 			2

#define ENABLE_KEY_CHAR		1

#if ENABLE_KEY_CHAR
typedef char* KEY_TYPE;
#else
typedef int KEY_TYPE; // key
#endif

typedef struct _rbtree_node {
	unsigned char color;
	struct _rbtree_node *right;
	struct _rbtree_node *left;
	struct _rbtree_node *parent;
	KEY_TYPE key;
	void *value;
} rbtree_node;

typedef struct _rbtree {
	rbtree_node *root;
	rbtree_node *nil;
} rbtree;


typedef struct _rbtree kvs_rbtree_t;

int kvs_rbtree_create(kvs_rbtree_t *inst);
void kvs_rbtree_destory(kvs_rbtree_t *inst);
int kvs_rbtree_set(kvs_rbtree_t *inst, char *key, char *value);
char* kvs_rbtree_get(kvs_rbtree_t *inst, char *key);
int kvs_rbtree_del(kvs_rbtree_t *inst, char *key);
int kvs_rbtree_mod(kvs_rbtree_t *inst, char *key, char *value);
int kvs_rbtree_exist(kvs_rbtree_t *inst, char *key);
//int kvs_rbtree_sort(kvs_rbtree_t *inst, char *response, int max_len);
int kvs_rbtree_sort(kvs_rbtree_t *inst, int order, int limit, char *response, int max_len);
int kvs_rbtree_range(kvs_rbtree_t *inst, char *start_key, char *end_key, char *response, int max_len);


#endif


#if ENABLE_HASH

#define MAX_KEY_LEN	128
#define MAX_VALUE_LEN	512
#define MAX_TABLE_SIZE	1024

#define ENABLE_KEY_POINTER	1


typedef struct hashnode_s {
#if ENABLE_KEY_POINTER
	char *key;
	char *value;
#else
	char key[MAX_KEY_LEN];
	char value[MAX_VALUE_LEN];
#endif
	struct hashnode_s *next;
	
} hashnode_t;


typedef struct hashtable_s {

	hashnode_t **nodes; //* change **, 

	int max_slots;
	int count;

} hashtable_t;

typedef struct hashtable_s kvs_hash_t;


int kvs_hash_create(kvs_hash_t *hash);
void kvs_hash_destory(kvs_hash_t *hash);
int kvs_hash_set(hashtable_t *hash, char *key, char *value);
char * kvs_hash_get(kvs_hash_t *hash, char *key);
int kvs_hash_mod(kvs_hash_t *hash, char *key, char *value);
int kvs_hash_del(kvs_hash_t *hash, char *key);
int kvs_hash_exist(kvs_hash_t *hash, char *key);
//int kvs_hash_sort(kvs_hash_t *inst, char *response, int max_len);
int kvs_hash_sort(kvs_hash_t *inst, int order, int limit, char *response, int max_len);
int kvs_hash_range(kvs_hash_t *inst, char *start_key, char *end_key, char *response, int max_len);


#endif

#if 0
void *kvs_malloc(size_t size);
void kvs_free(void *ptr);
#endif

#if ENABLE_SKIPTABLE

#define MAX_LEVEL 16
#define KEY_SIZE 128
#define VALUE_SIZE 256

typedef struct Node {
    char key[KEY_SIZE];
    char value[VALUE_SIZE];
    int level;  // 节点的实际层数
    struct Node* forward[];  // 柔性数组用于存储各级指针
} Node;

typedef struct {
    int level;      // 当前跳表的最大层数
    Node* header;   // 头节点
	int count;//节点总数
} kvs_skiptable_t;


int kvs_skiptable_create(kvs_skiptable_t *table);
void kvs_skiptable_destroy(kvs_skiptable_t *table);
int kvs_skiptable_set(kvs_skiptable_t *table, char *key, char *value);
char * kvs_skiptable_get(kvs_skiptable_t *table, char *key);
int kvs_skiptable_mod(kvs_skiptable_t *table, char *key, char *value);
int kvs_skiptable_del(kvs_skiptable_t *table, char *key);
int kvs_skiptable_exist(kvs_skiptable_t *table, char *key);
//int kvs_skiptable_sort(kvs_skiptable_t *table, char *response, int max_len);
int kvs_skiptable_sort(kvs_skiptable_t *table, int order, int limit, char *response, int max_len);
int kvs_skiptable_range(kvs_skiptable_t *table, char *start_key, char *end_key, char *response, int max_len);

#endif

void kvs_save_all(const char *filename);
void kvs_load_all(const char *filename);

#endif



//内存池
typedef struct mem_block {
    struct mem_block* next;
} mem_block_t;

typedef struct mem_pool {
    size_t block_size;
    int free_count;
    mem_block_t* free_list;
    struct mem_pool* next;
} mem_pool_t;

// 大内存块记录结构
typedef struct large_block_record {
    void *block;
    size_t size;
    struct large_block_record *next;
} large_block_record_t;


typedef struct mem_block_header {
    mem_pool_t *pool;       // 所属内存池
    size_t block_size;      // 实际块大小
    unsigned char is_large; // 是否为大内存
	struct mem_block_header *next_large; // 大内存块链表指针
} mem_block_header_t;


// 内存池全局管理结构
typedef struct {
    mem_pool_t* pools;      // 不同大小的内存池链表
    size_t pool_sizes[13];   // 预定义的内存块大小
    size_t large_threshold; // 大内存阈值
	int is_initialized;           // 内存池是否已初始化
} mem_manager_t;

void mem_init();
void* mem_alloc(size_t size);
void mem_free(void* ptr);
void mem_destroy();

#ifndef USE_MEMPOOL

// 替换原有的分配函数
#define kvs_malloc(size) mem_alloc(size)
#define kvs_free(ptr) mem_free(ptr)

#else
// 使用系统内存管理
#define kvs_malloc(size) malloc(size)
#define kvs_free(ptr) free(ptr)
#endif

extern mem_manager_t g_mem_manager;

int init_kvengine(void);
void dest_kvengine(void);


int kvs_filter_protocol(char **tokens, int count, char *response);
//分布式



extern void set_sync_context(int context);
extern int get_sync_context();