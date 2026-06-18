#ifndef KVS_REPLICATION_FALLBACK_H
#define KVS_REPLICATION_FALLBACK_H

#include <stddef.h>
#include <stdint.h>

#include "master_slave_proto.h"

typedef enum kvs_replication_fallback_kind_e {
	KVS_REPL_FALLBACK_UNKNOWN = 0,
	KVS_REPL_FALLBACK_REPLSYNC,
	KVS_REPL_FALLBACK_REPLICATED_APPLY,
	KVS_REPL_FALLBACK_NORMAL_COMMAND,
} kvs_replication_fallback_kind_t;

typedef struct kvs_replication_apply_s {
	ms_op_t op;
	const char *key;
	size_t key_len;
	const char *value;
	size_t value_len;
	int64_t expire_at_ms;
} kvs_replication_apply_t;

kvs_replication_fallback_kind_t kvs_replication_fallback_classify(
	char **tokens,
	int count,
	size_t *token_lens,
	kvs_replication_apply_t *apply
);

#endif /* KVS_REPLICATION_FALLBACK_H */
