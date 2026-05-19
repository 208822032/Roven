#pragma once
#include <stddef.h>
#include <stdint.h>
#include <time.h>
#include "kvs_config.h"

// 命令枚举（保留与现有实现一致的顺序与值）
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
	// skiptable
	KVS_CMD_SSET,
	KVS_CMD_SGET,
	KVS_CMD_SDEL,
	KVS_CMD_SMOD,
	KVS_CMD_SEXIST,
	KVS_CMD_SRANGE,
	KVS_CMD_SSORT,
	//paper
	KVS_CMD_PAPERSET,
	KVS_CMD_PAPERGET,
	KVS_CMD_PAPERDEL,
	KVS_CMD_PAPERMOD,
	// ttl
	KVS_CMD_EXPIRE,
	KVS_CMD_TTL,
	KVS_CMD_PERSIST,
	KVS_CMD_PEXPIREAT,
	KVS_CMD_AOFREWRITE,
	// quit
	KVS_CMD_QUIT,
	KVS_CMD_FULLDUMP,
	//vector
	KVS_CMD_VSET,
	KVS_CMD_VGET,
	KVS_CMD_VSEARCH,
	KVS_CMD_VDEL,
	KVS_CMD_VINDEX,
	KVS_CMD_VINFO,
};

// 数据结构类型
enum {
	DATA_STRUCTURE_ARRAY = 0,
	DATA_STRUCTURE_RBTREE,
	DATA_STRUCTURE_HASH,
	DATA_STRUCTURE_SKIPTABLE
};

// 命令字符串表（定义在实现文件中）
extern const char *command[];

#define KV_TTL_NO_EXPIRE (-1)

static inline int64_t kv_now_ms(void) {
	struct timespec ts;
	clock_gettime(CLOCK_REALTIME, &ts);
	return (int64_t)ts.tv_sec * 1000 + (int64_t)(ts.tv_nsec / 1000000);
}

static inline int kv_is_expired(int64_t expire_at_ms, int64_t now_ms) {
	return expire_at_ms >= 0 && expire_at_ms <= now_ms;
}

static inline int64_t kv_ttl_remaining_seconds(int64_t expire_at_ms, int64_t now_ms) {
	if (expire_at_ms < 0) return -1;
	if (expire_at_ms <= now_ms) return -2;
	return (expire_at_ms - now_ms) / 1000;
}
