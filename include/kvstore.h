


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
#include <ctype.h>
#include "server.h"
#include <sys/time.h>
#include <stdint.h>

#if 0

#define NETWORK_REACTOR 	0
#define NETWORK_PROACTOR	1
#define NETWORK_NTYCO		2

#define NETWORK_SELECT		NETWORK_REACTOR



#define KVS_MAX_TOKENS		128
#define KVS_MAX_RESPONSE_SIZE  2048
#define KVS_MAX_LINE        1000

#define ENABLE_ARRAY		1
#define ENABLE_RBTREE		1
#define ENABLE_HASH			1
#define ENABLE_SKIPTABLE    1

#define ENABLE_RDB          1

extern const char *command[];

enum {
	KVS_CMD_START = 0,
	// array
	KVS_CMD_SET = KVS_CMD_START,
	KVS_CMD_GET,
	KVS_CMD_DEL,
	KVS_CMD_MOD,
	KVS_CMD_EXIST,
	KVS_CMD_RANGE,
	KVS_CMD_SORT,
	// rbtree
	KVS_CMD_RSET,
	KVS_CMD_RGET,
	KVS_CMD_RDEL,
	KVS_CMD_RMOD,
	KVS_CMD_REXIST,
	KVS_CMD_RRANGE,
	KVS_CMD_RSORT,
	// hash
	KVS_CMD_HSET,
	KVS_CMD_HGET,
	KVS_CMD_HDEL,
	KVS_CMD_HMOD,
	KVS_CMD_HEXIST,
	KVS_CMD_HRANGE,
	KVS_CMD_HSORT,
	//skiptable
	KVS_CMD_SSET,
    KVS_CMD_SGET,
    KVS_CMD_SDEL,
    KVS_CMD_SMOD,
    KVS_CMD_SEXIST,
	KVS_CMD_SRANGE,
	KVS_CMD_SSORT,
	
	//退出命令
	KVS_CMD_QUIT,

	KVS_CMD_COUNT,
};

// 定义写操作命令集合
static const int write_commands[] = {
    KVS_CMD_SET, KVS_CMD_DEL, KVS_CMD_MOD,
    KVS_CMD_RSET, KVS_CMD_RDEL, KVS_CMD_RMOD,
    KVS_CMD_HSET, KVS_CMD_HDEL, KVS_CMD_HMOD,
    KVS_CMD_SSET, KVS_CMD_SDEL, KVS_CMD_SMOD
};



//typedef int (*msg_handler)(char *msg, int length, char *response);
typedef int (*msg_handler)(char **tokens, int count, char *response);
//添加连接状态 使用每个连接的专属解析器进行解析
//typedef int (*msg_handler)(struct conn *c, char *msg, int length, char *response);


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
int kvs_rbtree_range(kvs_rbtree_t *inst, char *start_key, char *end_key, int limit, char *response, int max_len);
void rbtree_traversal_with_callback(rbtree *T, rbtree_node *node, 
                                          void (*callback)(rbtree_node *, void *), 
                                          void *userdata);


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
int kvs_skiptable_range(kvs_skiptable_t *table, char *start_key, char *end_key, int limit, char *response, int max_len);

#endif

//优化函数接口
typedef struct {
	int (*set)(void*, char*, char*);
	char* (*get)(void*, char*);
	int (*del)(void*, char*);
	int (*mod)(void*, char*, char*);
	int (*exist)(void*, char*);
	int (*sort)(void*, int, int, char*, int);
	int (*range)(void*, char*, char*, char*, int);
}kvs_operations;

// 为每个数据结构初始化操作表
#if ENABLE_ARRAY
static kvs_operations array_ops = {
    .set = (int (*)(void*, char*, char*))kvs_array_set,
    .get = (char* (*)(void*, char*))kvs_array_get,
    .del = (int (*)(void*, char*))kvs_array_del,
    .mod = (int (*)(void*, char*, char*))kvs_array_mod,
    .exist = (int (*)(void*, char*))kvs_array_exist,
    .sort = (int (*)(void*, int, int, char*, int))kvs_array_sort,
    .range = (int (*)(void*, char*, char*, char*, int))kvs_array_range
};
#endif

#if ENABLE_RBTREE
static kvs_operations rbtree_ops = {
    .set = (int (*)(void*, char*, char*))kvs_rbtree_set,
    .get = (char* (*)(void*, char*))kvs_rbtree_get,
    .del = (int (*)(void*, char*))kvs_rbtree_del,
    .mod = (int (*)(void*, char*, char*))kvs_rbtree_mod,
    .exist = (int (*)(void*, char*))kvs_rbtree_exist,
    .sort = (int (*)(void*, int, int, char*, int))kvs_rbtree_sort,
    .range = (int (*)(void*, char*, char*, char*, int))kvs_rbtree_range
};
#endif

#if ENABLE_HASH
static kvs_operations hash_ops = {
    .set = (int (*)(void*, char*, char*))kvs_hash_set,
    .get = (char* (*)(void*, char*))kvs_hash_get,
    .del = (int (*)(void*, char*))kvs_hash_del,
    .mod = (int (*)(void*, char*, char*))kvs_hash_mod,
    .exist = (int (*)(void*, char*))kvs_hash_exist,
    .sort = (int (*)(void*, int, int, char*, int))kvs_hash_sort,
    .range = (int (*)(void*, char*, char*, char*, int))kvs_hash_range
};
#endif

#if ENABLE_SKIPTABLE
static kvs_operations skiplist_ops = {
    .set = (int (*)(void*, char*, char*))kvs_skiptable_set,
    .get = (char* (*)(void*, char*))kvs_skiptable_get,
    .del = (int (*)(void*, char*))kvs_skiptable_del,
    .mod = (int (*)(void*, char*, char*))kvs_skiptable_mod,
    .exist = (int (*)(void*, char*))kvs_skiptable_exist,
    .sort = (int (*)(void*, int, int, char*, int))kvs_skiptable_sort,
    .range = (int (*)(void*, char*, char*, char*, int))kvs_skiptable_range
};
#endif



void kvs_save_all(const char *filename);
void kvs_load_all(const char *filename);





//内存池
typedef struct mem_block {
    struct mem_block* next;
} mem_block_t;

typedef struct mem_chunk{
	void *chunk;
	struct mem_chunk *next;
} mem_chunk_t;

typedef struct mem_pool {
    size_t block_size;
    int free_count;
    mem_block_t* free_list;
    struct mem_pool* next;
	pthread_mutex_t lock;	//每个内存池单独的锁
	mem_chunk_t *chunks;	//记录所有分配的大块内存
} mem_pool_t;

// 大内存块记录结构
typedef struct large_block_record {
    void *block;
    size_t size;
    struct large_block_record *next;
	struct large_block_record *prev;  //前向指针 支持双向链表
} large_block_record_t;



typedef struct mem_block_header {
    mem_pool_t *pool;       // 所属内存池
    size_t block_size;      // 实际块大小
    unsigned char is_large; // 是否为大内存
	// struct mem_block_header *next_large; // 大内存块链表指针
	large_block_record_t *large_record; //指向大内存记录
} mem_block_header_t;


// 内存池全局管理结构
typedef struct {
    mem_pool_t* pools;      // 不同大小的内存池链表
    size_t pool_sizes[13];   // 预定义的内存块大小
    size_t large_threshold; // 大内存阈值
	int is_initialized;           // 内存池是否已初始化
	pthread_mutex_t large_blocks_lock;  //大内存块链表的锁
} mem_manager_t;

void mem_init();
void* mem_alloc(size_t size);
void mem_free(void* ptr);
void mem_destroy();
void* kvs_realloc(void *ptr, size_t new_size);
char* kvs_strdup(const char *s);
void *memleak_alloc(size_t size, const char *filename, const char *funcname, int line);
void memleak_free(void *ptr, const char *filename, const char *funcname, int line);

#ifndef USE_MEMPOOL

// 替换原有的分配函数
#define kvs_malloc(size) mem_alloc(size)
#define kvs_free(ptr) mem_free(ptr)

// #define kvs_malloc(size) memleak_alloc(size, __FILE__, __func__, __LINE__)
// #define kvs_free(ptr) memleak_free(ptr, __FILE__, __func__, __LINE__)

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


//RDB
#define RDB_MAGIC "KVSRDB"
#define RDB_VERSION  1

//RDB操作结果
#define RDB_OK  0
#define RDB_ERR 1

//RDB操作类型
#define RDB_TYPE_STRING  0
#define RDB_TYPE_HASH    1
#define RDB_TYPE_SET     2
#define RDB_TYPE_ZSET    3
#define RDB_TYPE_LIST    4

int kvs_rdb_save(const char *filename);
int kvs_rdb_load(const char *filename);
void rdb_save_array(FILE *fp);
void rdb_save_rbtree(FILE *fp);
void rdb_save_hash(FILE *fp);
void rdb_save_skiptable(FILE *fp);
void rdb_load_array(FILE *fp);
void rdb_load_rbtree(FILE *fp);
void rdb_load_hash(FILE *fp);
void rdb_load_skiptable(FILE *fp);




//业务逻辑优化

#if ENABLE_ARRAY
extern kvs_array_t global_array;
#endif

#if ENABLE_RBTREE
extern kvs_rbtree_t global_rbtree;
#endif

#if ENABLE_HASH
extern kvs_hash_t global_hash;
#endif

#if ENABLE_SKIPTABLE
extern kvs_skiptable_t global_skiptable;
#endif

//命令处理函数类型定义

//命令上下文结构
typedef struct {
	char *key;
	char *value;
	char *start_key;
	char *end_key;
	int order;
	int limit;
	int data_structure;
} command_context_t;
//typedef int (*command_handler)(char **args, int argc, char *response);
typedef int (*command_handler)(command_context_t *ctx, char *response);

//命令信息结构
typedef struct {
    char *name;
    command_handler handler;
    int arity; //命令需要的参数数量
	int write_command;  //是否是写命令
	int data_structure; //使用的数据结构类型
} command_info;

//数据结构类型定义
enum {
	DATA_STRUCTURE_ARRAY = 0,
	DATA_STRUCTURE_RBTREE,
	DATA_STRUCTURE_HASH,
	DATA_STRUCTURE_SKIPTABLE
};

int get_command_index(const char *name);
const command_info *lookup_command(const char *name);
int check_arity(const command_info *cmd, int argc);
int parse_command_argument(int cmd, char **tokens, int count, command_context_t *ctx);

// 声明所有命令处理函数
int handle_set(command_context_t *ctx, char *response);
int handle_get(command_context_t *ctx, char *response);
int handle_del(command_context_t *ctx, char *response);
int handle_mod(command_context_t *ctx, char *response);
int handle_exist(command_context_t *ctx, char *response);
int handle_range(command_context_t *ctx, char *response);
int handle_sort(command_context_t *ctx, char *response);
int handle_quit(command_context_t *ctx, char *response);

//命令表
extern const command_info command_table[];

/*
正常命令处理通过单线程模型避免了并发问题
AOF重写功能缺失关键的安全机制
 直接实现AOF重写会导致数据竞争和内存安全问题
以redis实现aof机制的思路为准 尽可能模仿redis的实现逻辑
参考redis的fork+COW机制以及redis记录重写期间新命令的缓冲区和子进程完成后的命令追加逻辑
重新实现aof所有功能 可以考虑删改kvstore.h中有关aof的结构 
处理kvstore在AOF重写时的数据不一致和内存管理风险
*/

//AOF
#define ENABLE_AOF 1
#define AOF_BUFFER_SIZE (1024 * 1024 * 16)  //16MB缓冲区
#define AOF_REWRITE_MIN_SIZE (1024 * 1024 * 64)  //64MB最小重写大小
#define AOF_REWRITE_PERCENTAGE 50 //增长100%时触发重写
#define AOF_REWRITE_MIN_GROWTH (1024 * 1024 * 10) //10MB最小增长量

//同步模式
#define AOF_FSYNC_NO  0   //不主动同步
#define AOF_FSYNC_EVERYSEC 1  //每秒同步
#define AOF_FSYNC_ALWAYS   2  //每次写入后同步

//AOF 状态
typedef struct {
	FILE *fp;                  // AOF文件指针
    char *filename;            // AOF文件名
    char *temp_filename;       // 临时文件名(用于重写)
    char *buf;                 // AOF缓冲区
    size_t buf_len;            // 缓冲区当前长度
    size_t buf_size;           // 缓冲区总大小
    int fsync_mode;            // 同步模式
    off_t current_size;        // 当前AOF文件大小
    off_t rewrite_base_size;   // 重写基准大小
    time_t last_fsync;         // 上次同步时间
    pthread_mutex_t mutex;     // 缓冲区互斥锁
    pthread_cond_t cond;       // 条件变量(用于后台线程)
    pthread_t bgthread;        // 后台线程ID
    volatile int stop;         // 停止标志

	//重写相关字段
    int rewrite_in_progress;   // 重写进行中标志
    pid_t rewrite_child_pid;   // 重写子进程ID
	char *rewrite_buf;        // 重写缓冲区
	size_t rewrite_buf_len;   // 重写缓冲区当前长度
	size_t rewrite_buf_size;  // 重写缓冲区总大小
	pthread_mutex_t rewrite_mutex; // 重写缓冲区互斥锁
	int aof_fd;			   // AOF文件描述符用于同步

	//COW相关字段
	int cow_enabled;      // 是否启用COW
	pthread_rwlock_t data_rwlock; // 数据读写锁
	volatile int cow_snapshot_ready; // COW快照准备就绪标志
	time_t snapshot_timestamp; // 快照时间戳

} aof_state_t;

int aof_init(const char *filename, int fsync_mode);
void aof_append(int argc, char **argv);
void aof_fsync(int force);
void aof_rewrite(void);
int aof_load(const char *filename);
void aof_free(void);
void aof_background_fsync(void *arg);
void aof_rewrite_background(void);
int aof_rewrite_tmpfile(void);
int aof_rewrite_buffer_write(int fd);

// 在AOF相关声明部分添加
void aof_rewrite_append_command(int argc, char **argv);
void aof_stop_rewrite(void);

int should_buffer_rewrite_command(void);

extern aof_state_t aof_state;


#endif // __KV_STORE_H__

#endif

#pragma once
// 兼容旧入口：只聚合新的模块化头
#include "kvs_api.h"