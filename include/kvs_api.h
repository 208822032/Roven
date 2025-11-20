#pragma once
// 对外单入口：按需包含所有公共组件头
#include "kvs_platform.h"
#include "kvs_config.h"
#include "kvs_types.h"
#include "kvs_version.h"

#include "kvs_memory.h"

#if ENABLE_ARRAY
#include "kvs_array.h"
#endif
#if ENABLE_RBTREE
#include "kvs_rbtree.h"
#endif
#if ENABLE_HASH
#include "kvs_hash.h"
#endif
#if ENABLE_SKIPTABLE
#include "kvs_skiptable.h"
#endif

#include "kvs_operations.h"
#include "kvs_command.h"
#include "kvs_resp.h"

#if ENABLE_RDB
#include "kvs_rdb.h"
#endif

#if ENABLE_AOF
#include "kvs_aof.h"
#endif

#include "kvs_distributed.h"
#include "kvs_server.h"

