#pragma once
#include "kvs_config.h"
#include "kvs_array.h"
#include "kvs_rbtree.h"
#include "kvs_hash.h"
#include "kvs_skiptable.h"

// 全局运行时配置
extern kvs_config_t g_cfg;

// 提供集中式访问接口，避免在各处直接 extern 变量名

#if ENABLE_ARRAY
kvs_array_t* kvs_get_global_array(void);
#endif

#if ENABLE_RBTREE
kvs_rbtree_t* kvs_get_global_rbtree(void);
#endif

#if ENABLE_HASH
kvs_hash_t* kvs_get_global_hash(void);
#endif

#if ENABLE_SKIPTABLE
kvs_skiptable_t* kvs_get_global_skiptable(void);
#endif
