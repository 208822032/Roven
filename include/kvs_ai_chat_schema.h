#pragma once

#include <stdarg.h>
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>

#include "kvs_hash.h"
#include "kvs_skiptable.h"

/*
 * AI-chat 语义检索最小 schema helper。
 * 当前 KVLite 仍是 NUL 终止字符串存储，因此这里约定的 payload 都是文本 blobs。
 */

#define KVS_AI_CHAT_SCOPE_GLOBAL "global"
#define KVS_AI_CHAT_KEY_QA_PREFIX "qa:"
#define KVS_AI_CHAT_KEY_QNORM_PREFIX "qnorm:"
#define KVS_AI_CHAT_KEY_SESSION_PREFIX "sess:"
#define KVS_AI_CHAT_KEY_VECTOR_PREFIX "vec:"
#define KVS_AI_CHAT_KEY_CAND_PREFIX "cand:"

typedef enum kvs_ai_chat_embedding_status_e {
	KVS_AI_CHAT_EMBEDDING_PENDING = 0,
	KVS_AI_CHAT_EMBEDDING_READY = 1,
	KVS_AI_CHAT_EMBEDDING_FAILED = 2,
} kvs_ai_chat_embedding_status_t;

static inline const char *kvs_ai_chat_embedding_status_name(kvs_ai_chat_embedding_status_t status) {
	switch (status) {
		case KVS_AI_CHAT_EMBEDDING_PENDING:
			return "pending";
		case KVS_AI_CHAT_EMBEDDING_READY:
			return "ready";
		case KVS_AI_CHAT_EMBEDDING_FAILED:
			return "failed";
		default:
			return "unknown";
	}
}

static inline int kvs_ai_chat_format_key(char *buf, size_t cap, const char *fmt, ...) {
	if (!buf || cap == 0 || !fmt) return -1;
	va_list ap;
	va_start(ap, fmt);
	int n = vsnprintf(buf, cap, fmt, ap);
	va_end(ap);
	if (n < 0 || (size_t)n >= cap) return -1;
	return n;
}

static inline int kvs_ai_chat_build_qa_key(char *buf, size_t cap, const char *record_id) {
	if (!record_id || record_id[0] == '\0') return -1;
	return kvs_ai_chat_format_key(buf, cap, "%s%s", KVS_AI_CHAT_KEY_QA_PREFIX, record_id);
}

static inline int kvs_ai_chat_build_qnorm_key(char *buf, size_t cap, const char *question_hash) {
	if (!question_hash || question_hash[0] == '\0') return -1;
	return kvs_ai_chat_format_key(buf, cap, "%s%s", KVS_AI_CHAT_KEY_QNORM_PREFIX, question_hash);
}

static inline int kvs_ai_chat_build_qnorm_scope_key(char *buf, size_t cap, const char *question_hash, const char *scope) {
	if (!question_hash || question_hash[0] == '\0') return -1;
	if (!scope || scope[0] == '\0') scope = KVS_AI_CHAT_SCOPE_GLOBAL;
	return kvs_ai_chat_format_key(buf, cap, "%s%s:scope:%s", KVS_AI_CHAT_KEY_QNORM_PREFIX, question_hash, scope);
}

static inline int kvs_ai_chat_build_session_turn_key(char *buf, size_t cap, const char *session_id, int64_t turn_seq) {
	if (!session_id || session_id[0] == '\0') return -1;
	return kvs_ai_chat_format_key(buf, cap, "%s%s:turn:%010lld", KVS_AI_CHAT_KEY_SESSION_PREFIX, session_id, (long long)turn_seq);
}

static inline int kvs_ai_chat_build_embedding_key(char *buf, size_t cap, const char *record_id) {
	if (!record_id || record_id[0] == '\0') return -1;
	return kvs_ai_chat_format_key(buf, cap, "%s%s", KVS_AI_CHAT_KEY_VECTOR_PREFIX, record_id);
}

static inline int kvs_ai_chat_build_candidate_key(char *buf, size_t cap, const char *scope, const char *question_hash, const char *record_id) {
	if (!question_hash || question_hash[0] == '\0' || !record_id || record_id[0] == '\0') return -1;
	if (!scope || scope[0] == '\0') scope = KVS_AI_CHAT_SCOPE_GLOBAL;
	return kvs_ai_chat_format_key(buf, cap, "%s%s:%s:%s", KVS_AI_CHAT_KEY_CAND_PREFIX, scope, question_hash, record_id);
}

static inline int kvs_ai_chat_build_candidate_scope_range(char *start_key,
													 size_t start_cap,
													 char *end_key,
													 size_t end_cap,
													 const char *scope) {
	if (!scope || scope[0] == '\0') scope = KVS_AI_CHAT_SCOPE_GLOBAL;
	if (kvs_ai_chat_format_key(start_key, start_cap, "%s%s:", KVS_AI_CHAT_KEY_CAND_PREFIX, scope) < 0) return -1;
	if (kvs_ai_chat_format_key(end_key, end_cap, "%s%s;", KVS_AI_CHAT_KEY_CAND_PREFIX, scope) < 0) return -1;
	return 0;
}

static inline int kvs_ai_chat_put_text(kvs_hash_t *store, const char *key, const char *value) {
	if (!store || !key || !value) return -1;
	return kvs_hash_set(store, (char *)key, (char *)value);
}

static inline const char *kvs_ai_chat_get_text(kvs_hash_t *store, const char *key) {
	if (!store || !key) return NULL;
	return kvs_hash_get(store, (char *)key);
}

static inline int kvs_ai_chat_put_qa_record(kvs_hash_t *store, const char *record_id, const char *record_json) {
	char key[256];
	if (kvs_ai_chat_build_qa_key(key, sizeof(key), record_id) < 0) return -1;
	return kvs_ai_chat_put_text(store, key, record_json);
}

static inline const char *kvs_ai_chat_load_qa_record(kvs_hash_t *store, const char *record_id) {
	char key[256];
	if (kvs_ai_chat_build_qa_key(key, sizeof(key), record_id) < 0) return NULL;
	return kvs_ai_chat_get_text(store, key);
}

static inline int kvs_ai_chat_put_qnorm_index(kvs_hash_t *store, const char *question_hash, const char *candidate_json) {
	char key[256];
	if (kvs_ai_chat_build_qnorm_key(key, sizeof(key), question_hash) < 0) return -1;
	return kvs_ai_chat_put_text(store, key, candidate_json);
}

static inline const char *kvs_ai_chat_load_qnorm_index(kvs_hash_t *store, const char *question_hash) {
	char key[256];
	if (kvs_ai_chat_build_qnorm_key(key, sizeof(key), question_hash) < 0) return NULL;
	return kvs_ai_chat_get_text(store, key);
}

static inline int kvs_ai_chat_put_qnorm_scope_index(kvs_hash_t *store, const char *question_hash, const char *scope, const char *candidate_json) {
	char key[320];
	if (kvs_ai_chat_build_qnorm_scope_key(key, sizeof(key), question_hash, scope) < 0) return -1;
	return kvs_ai_chat_put_text(store, key, candidate_json);
}

static inline const char *kvs_ai_chat_load_qnorm_scope_index(kvs_hash_t *store, const char *question_hash, const char *scope) {
	char key[320];
	if (kvs_ai_chat_build_qnorm_scope_key(key, sizeof(key), question_hash, scope) < 0) return NULL;
	return kvs_ai_chat_get_text(store, key);
}

static inline int kvs_ai_chat_put_session_turn(kvs_hash_t *store, const char *session_id, int64_t turn_seq, const char *record_id) {
	char key[256];
	if (kvs_ai_chat_build_session_turn_key(key, sizeof(key), session_id, turn_seq) < 0) return -1;
	return kvs_ai_chat_put_text(store, key, record_id);
}

static inline const char *kvs_ai_chat_load_session_turn(kvs_hash_t *store, const char *session_id, int64_t turn_seq) {
	char key[256];
	if (kvs_ai_chat_build_session_turn_key(key, sizeof(key), session_id, turn_seq) < 0) return NULL;
	return kvs_ai_chat_get_text(store, key);
}

static inline const char *kvs_ai_chat_load_record_by_session_turn(kvs_hash_t *store, const char *session_id, int64_t turn_seq) {
	const char *record_id = kvs_ai_chat_load_session_turn(store, session_id, turn_seq);
	if (!record_id || record_id[0] == '\0') return NULL;
	return kvs_ai_chat_load_qa_record(store, record_id);
}

static inline int kvs_ai_chat_put_embedding(kvs_hash_t *store, const char *record_id, const char *embedding_json) {
	char key[256];
	if (kvs_ai_chat_build_embedding_key(key, sizeof(key), record_id) < 0) return -1;
	return kvs_ai_chat_put_text(store, key, embedding_json);
}

static inline const char *kvs_ai_chat_load_embedding(kvs_hash_t *store, const char *record_id) {
	char key[256];
	if (kvs_ai_chat_build_embedding_key(key, sizeof(key), record_id) < 0) return NULL;
	return kvs_ai_chat_get_text(store, key);
}

static inline int kvs_ai_chat_put_candidate(kvs_skiptable_t *index, const char *scope, const char *question_hash, const char *record_id, const char *candidate_text) {
	char key[384];
	if (kvs_ai_chat_build_candidate_key(key, sizeof(key), scope, question_hash, record_id) < 0) return -1;
	if (!candidate_text) candidate_text = record_id;
	return kvs_skiptable_set(index, key, (char *)candidate_text);
}

static inline int kvs_ai_chat_scan_candidates(kvs_skiptable_t *index, const char *scope, int limit, char *response, int max_len) {
	char start_key[256];
	char end_key[256];
	if (!response || max_len <= 0) return -1;
	if (kvs_ai_chat_build_candidate_scope_range(start_key, sizeof(start_key), end_key, sizeof(end_key), scope) < 0) return -1;
	return kvs_skiptable_range(index, start_key, end_key, limit, response, max_len);
}
