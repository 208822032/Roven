#include "kvs_replication_fallback.h"

#include <errno.h>
#include <stdlib.h>
#include <string.h>

static int token_eq(const char *token, size_t len, const char *lit) {
	size_t i;

	if (!token || !lit) {
		return 0;
	}
	for (i = 0; lit[i] != '\0'; ++i) {
		char a;
		char b;

		if (i >= len) {
			return 0;
		}
		a = token[i];
		b = lit[i];
		if (a >= 'a' && a <= 'z') a = (char)(a - 'a' + 'A');
		if (b >= 'a' && b <= 'z') b = (char)(b - 'a' + 'A');
		if (a != b) {
			return 0;
		}
	}
	return i == len;
}

static int parse_i64_token(const char *token, size_t len, int64_t *out) {
	char buf[32];
	char *endptr = NULL;
	long long value;

	if (!token || !out || len == 0 || len >= sizeof(buf)) {
		return -1;
	}
	memcpy(buf, token, len);
	buf[len] = '\0';
	errno = 0;
	value = strtoll(buf, &endptr, 10);
	if (endptr == buf || *endptr != '\0' || errno == ERANGE) {
		return -1;
	}
	*out = (int64_t)value;
	return 0;
}

static int classify_write_op(
	const char *cmd,
	size_t cmd_len,
	int argc,
	ms_op_t *op,
	int *needs_value
) {
	if (token_eq(cmd, cmd_len, "SET")) {
		*op = MS_OP_ARRAY_SET;
		*needs_value = 1;
		return argc == 3 ? 0 : -1;
	}
	if (token_eq(cmd, cmd_len, "RSET")) {
		*op = MS_OP_RBTREE_RSET;
		*needs_value = 1;
		return argc == 3 ? 0 : -1;
	}
	if (token_eq(cmd, cmd_len, "HSET")) {
		*op = MS_OP_HASH_HSET;
		*needs_value = 1;
		return argc == 3 ? 0 : -1;
	}
	if (token_eq(cmd, cmd_len, "SSET")) {
		*op = MS_OP_SKIPTABLE_SSET;
		*needs_value = 1;
		return argc == 3 ? 0 : -1;
	}
	if (token_eq(cmd, cmd_len, "PAPERSET")) {
		*op = MS_OP_PAPERSET;
		*needs_value = 1;
		return argc == 3 ? 0 : -1;
	}
	if (token_eq(cmd, cmd_len, "DEL")) {
		*op = MS_OP_ARRAY_DEL;
		*needs_value = 0;
		return argc == 2 ? 0 : -1;
	}
	if (token_eq(cmd, cmd_len, "RDEL")) {
		*op = MS_OP_RBTREE_RDEL;
		*needs_value = 0;
		return argc == 2 ? 0 : -1;
	}
	if (token_eq(cmd, cmd_len, "HDEL")) {
		*op = MS_OP_HASH_HDEL;
		*needs_value = 0;
		return argc == 2 ? 0 : -1;
	}
	if (token_eq(cmd, cmd_len, "SDEL")) {
		*op = MS_OP_SKIPTABLE_SDEL;
		*needs_value = 0;
		return argc == 2 ? 0 : -1;
	}
	if (token_eq(cmd, cmd_len, "PAPERDEL")) {
		*op = MS_OP_PAPERDEL;
		*needs_value = 0;
		return argc == 2 ? 0 : -1;
	}
	if (token_eq(cmd, cmd_len, "MOD")) {
		*op = MS_OP_ARRAY_MOD;
		*needs_value = 1;
		return argc == 3 ? 0 : -1;
	}
	if (token_eq(cmd, cmd_len, "RMOD")) {
		*op = MS_OP_RBTREE_RMOD;
		*needs_value = 1;
		return argc == 3 ? 0 : -1;
	}
	if (token_eq(cmd, cmd_len, "HMOD")) {
		*op = MS_OP_HASH_HMOD;
		*needs_value = 1;
		return argc == 3 ? 0 : -1;
	}
	if (token_eq(cmd, cmd_len, "SMOD")) {
		*op = MS_OP_SKIPTABLE_SMOD;
		*needs_value = 1;
		return argc == 3 ? 0 : -1;
	}
	if (token_eq(cmd, cmd_len, "PAPERMOD")) {
		*op = MS_OP_PAPERMOD;
		*needs_value = 1;
		return argc == 3 ? 0 : -1;
	}
	if (token_eq(cmd, cmd_len, "PEXPIREAT")) {
		*op = MS_OP_TTL_PEXPIREAT;
		*needs_value = 1;
		return argc == 3 ? 0 : -1;
	}
	if (token_eq(cmd, cmd_len, "PERSIST")) {
		*op = MS_OP_TTL_PERSIST;
		*needs_value = 0;
		return argc == 2 ? 0 : -1;
	}
	return 1;
}

kvs_replication_fallback_kind_t kvs_replication_fallback_classify(
	char **tokens,
	int count,
	size_t *token_lens,
	kvs_replication_apply_t *apply
) {
	ms_op_t op = 0;
	int needs_value = 0;
	int ret;

	if (apply) {
		memset(apply, 0, sizeof(*apply));
	}
	if (!tokens || !token_lens || count <= 0 || !tokens[0]) {
		return KVS_REPL_FALLBACK_UNKNOWN;
	}
	if (token_eq(tokens[0], token_lens[0], "REPLSYNC")) {
		return KVS_REPL_FALLBACK_REPLSYNC;
	}

	ret = classify_write_op(tokens[0], token_lens[0], count, &op, &needs_value);
	if (ret == 1) {
		return KVS_REPL_FALLBACK_NORMAL_COMMAND;
	}
	if (ret < 0 || !apply || count < 2 || !tokens[1]) {
		return KVS_REPL_FALLBACK_UNKNOWN;
	}

	apply->op = op;
	apply->key = tokens[1];
	apply->key_len = token_lens[1];
	if (needs_value) {
		if (count < 3 || !tokens[2]) {
			return KVS_REPL_FALLBACK_UNKNOWN;
		}
		if (op == MS_OP_TTL_PEXPIREAT) {
			if (parse_i64_token(tokens[2], token_lens[2], &apply->expire_at_ms) < 0) {
				return KVS_REPL_FALLBACK_UNKNOWN;
			}
		} else {
			apply->value = tokens[2];
			apply->value_len = token_lens[2];
		}
	}
	return KVS_REPL_FALLBACK_REPLICATED_APPLY;
}
