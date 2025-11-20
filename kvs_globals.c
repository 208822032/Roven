#include "include/kvs_config.h"
#include "include/kvs_array.h"
#include "include/kvs_rbtree.h"
#include "include/kvs_hash.h"
#include "include/kvs_skiptable.h"
#include "include/kvs_globals.h"

// 使用现有的全局变量（在各模块或测试中已定义），这里仅提供集中访问函数

#if ENABLE_ARRAY
extern kvs_array_t global_array;
kvs_array_t* kvs_get_global_array(void) { return &global_array; }
#endif

#if ENABLE_RBTREE
extern kvs_rbtree_t global_rbtree;
kvs_rbtree_t* kvs_get_global_rbtree(void) { return &global_rbtree; }
#endif

#if ENABLE_HASH
extern kvs_hash_t global_hash;
kvs_hash_t* kvs_get_global_hash(void) { return &global_hash; }
#endif

#if ENABLE_SKIPTABLE
extern kvs_skiptable_t global_skiptable;
kvs_skiptable_t* kvs_get_global_skiptable(void) { return &global_skiptable; }
#endif
