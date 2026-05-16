#define KVS_VECTOR_INTERNAL
#include "kvs_vector.h"
#include <stdlib.h>

/*
 * 计算不小于 value 的 2 的幂。
 *
 * id map 使用开放寻址，容量使用 2 的幂可以简化取模。
 */
size_t kvs_vector_next_pow2(size_t value) {
	size_t n = 1;

	if (value <= 1) {
		return 1;
	}

	while (n < value) {
		n <<= 1;
	}

	return n;
}

/*
 * 计算字符串 hash。
 *
 * 这里沿用项目 array/hash/skiptable 中已有的 hash 风格：
 * hash ^= ((hash << 5) + ch + (hash >> 2))
 *
 * 注意：
 * 这里按长度处理，不要求 data 必须是 NUL 结尾。
 */
 uint32_t kvs_vector_hash_key(const char *data, size_t len) {
	uint32_t hash = 0;
	size_t i;

	if (!data || len == 0) {
		return 0;
	}

	for (i = 0; i < len; ++i) {
		hash ^= ((hash << 5) + (unsigned char)data[i] + (hash >> 2));
	}

	return hash;
}

/*
 * 组合 question + scope 的 hash。
 *
 * 注意：
 * 1. hash 只用于定位 id_map 槽位；
 * 2. 命中后仍然必须二次比较 question + scope；
 * 3. scope 为空时也参与组合，避免后续规则混乱。
 */
 uint32_t kvs_vector_compose_hash(
	const char *question,
	size_t question_len,
	const char *scope,
	size_t scope_len
) {
	uint32_t hq;
	uint32_t hs;
	uint32_t h;

	hq = kvs_vector_hash_key(question, question_len);
	hs = kvs_vector_hash_key(scope, scope_len);

	h = hq;
	h ^= 0x9e3779b9u + (h << 6) + (h >> 2);
	h ^= hs + 0x85ebca6bu + (h << 6) + (h >> 2);

	return h;
}

/*
 * 复制一段字符串，并额外补 '\0'。
 *
 * 用途：
 * 1. question；
 * 2. answer；
 * 3. scope；
 * 4. normalized_question。
 */
 char *kvs_vector_dup_string(const char *src, size_t src_len, size_t *out_len) {
	char *dst;

	if (out_len) {
		*out_len = 0;
	}

	if (!src && src_len > 0) {
		return NULL;
	}

	dst = (char *)kvs_malloc(src_len + 1);
	if (!dst) {
		return NULL;
	}

	if (src_len > 0 && src) {
		memcpy(dst, src, src_len);
	}
	dst[src_len] = '\0';

	if (out_len) {
		*out_len = src_len;
	}

	return dst;
}

//文本精确比较
 int kvs_vector_text_equal(const char *lhs, size_t lhs_len, const char *rhs, size_t rhs_len) {
    if (lhs_len != rhs_len) {
        return 0;
    }

    if (lhs_len == 0) {
        return 1;
    }

    if(!lhs || !rhs) {
        return 0;
    }

    return memcmp(lhs, rhs, lhs_len) == 0;
}

/*
 * scope 精确比较。
 *
 * 规则：
 * 1. NULL scope 和空字符串 scope 等价；
 * 2. scope_len == 0 统一视为空 scope；
 * 3. 非空 scope 按长度 + memcmp 比较。
 */
 int kvs_vector_scope_equal(const char *lhs, size_t lhs_len, const char *rhs, size_t rhs_len){
    if(!lhs) {
        lhs_len = 0;
    }
    if(!rhs) {
        rhs_len = 0;
    }
    if(lhs_len == 0 && rhs_len == 0) {
        return 1;
    }
    return kvs_vector_text_equal(lhs, lhs_len, rhs, rhs_len);
}

/*
 * 清空 record 字段。
 *
 * 注意：
 * 这个函数不释放 record 内部指针。
 * 如果 record 已经持有内存，必须先调用 kvs_vector_record_destroy。
 */
 void kvs_vector_record_reset(kvs_vector_record_t *record) {
	if (!record) {
		return;
	}

	memset(record, 0, sizeof(*record));
	record->state = KVS_VECTOR_RECORD_EMPTY;
	record->deleted = 0;
}

/*
 * 释放 record 内部资源，并重置为 EMPTY 状态。
 */
 void kvs_vector_record_destroy(kvs_vector_record_t *record) {
	if (!record) {
		return;
	}

	if (record->question) {
		kvs_free(record->question);
		record->question = NULL;
	}

	if (record->answer) {
		kvs_free(record->answer);
		record->answer = NULL;
	}

	if (record->normalized_question) {
		kvs_free(record->normalized_question);
		record->normalized_question = NULL;
	}

	if (record->scope) {
		kvs_free(record->scope);
		record->scope = NULL;
	}

	if (record->embedding) {
		kvs_free(record->embedding);
		record->embedding = NULL;
	}

	kvs_vector_record_reset(record);
}

/*
 * 初始化一条新 record 的字段。
 *
 * 出错时会释放已经分配的中间内存，不留下半成品。
 */
/*
 * 初始化一条新 record 的字段。
 *
 * 出错时会释放已经分配的中间内存，不留下半成品。
 */
 int kvs_vector_record_set_fields(
	kvs_vector_record_t *record,
	const char *question,
	size_t question_len,
	const char *answer,
	size_t answer_len,
	const char *scope,
	size_t scope_len
) {
	char *question_copy = NULL;
	char *answer_copy = NULL;
	char *normalized_copy = NULL;
	char *scope_copy = NULL;
	float *embedding = NULL;
	size_t copied_len = 0;
	int embedding_dim = 0;
	int64_t now_ms;
	int ret;

	if (!record || !question || question_len == 0 || !answer) {
		return KVS_VECTOR_ERR_INVALID_ARG;
	}

	kvs_vector_record_destroy(record);

	question_copy = kvs_vector_dup_string(question, question_len, &copied_len);
	if (!question_copy) {
		return KVS_VECTOR_ERR;
	}

	answer_copy = kvs_vector_dup_string(answer, answer_len, &copied_len);
	if (!answer_copy) {
		kvs_free(question_copy);
		return KVS_VECTOR_ERR;
	}

	/*
	 * 当前 normalized_question 暂时直接复制 question。
	 * 后续可以在这里接入 trim/lower/tokenize。
	 */
	normalized_copy = kvs_vector_dup_string(question, question_len, &copied_len);
	if (!normalized_copy) {
		kvs_free(question_copy);
		kvs_free(answer_copy);
		return KVS_VECTOR_ERR;
	}

	if (scope && scope_len > 0) {
		scope_copy = kvs_vector_dup_string(scope, scope_len, &copied_len);
		if (!scope_copy) {
			kvs_free(question_copy);
			kvs_free(answer_copy);
			kvs_free(normalized_copy);
			return KVS_VECTOR_ERR;
		}
	}

	/*
	 * 第 9 步：VSET 时生成本地 deterministic embedding。
	 */
	ret = kvs_vector_embedding_generate(
		normalized_copy,
		question_len,
		&embedding,
		&embedding_dim
	);
	if (ret != KVS_VECTOR_OK) {
		kvs_free(question_copy);
		kvs_free(answer_copy);
		kvs_free(normalized_copy);
		if (scope_copy) {
			kvs_free(scope_copy);
		}
		return ret;
	}

	now_ms = kv_now_ms();

	record->id = g_vector_state.store.next_id++;
	record->question_hash = kvs_vector_hash_key(question, question_len);

	record->question = question_copy;
	record->question_len = question_len;

	record->answer = answer_copy;
	record->answer_len = answer_len;

	record->normalized_question = normalized_copy;
	record->normalized_question_len = question_len;

	record->scope = scope_copy;
	record->scope_len = (scope && scope_len > 0) ? scope_len : 0;

	record->embedding = embedding;
	record->embedding_dim = embedding_dim;

	record->state = KVS_VECTOR_RECORD_ACTIVE;
	record->deleted = 0;

	record->created_at_ms = now_ms;
	record->updated_at_ms = now_ms;

	return KVS_VECTOR_OK;
}

/*
 * 使用外部已经生成好的 embedding 初始化 record。
 *
 * 用于 VBATCHSET：
 * 1. batch 阶段一次性生成连续 embedding；
 * 2. 每条 record 拷贝自己的 embedding；
 * 3. record 继续保持独立拥有 embedding 内存，destroy 时可正常释放。
 */
int kvs_vector_record_set_fields_with_embedding(
	kvs_vector_record_t *record,
	const char *question,
	size_t question_len,
	const char *answer,
	size_t answer_len,
	const char *scope,
	size_t scope_len,
	const float *embedding,
	int embedding_dim
) {
	char *question_copy = NULL;
	char *answer_copy = NULL;
	char *normalized_copy = NULL;
	char *scope_copy = NULL;
	float *embedding_copy = NULL;
	size_t copied_len = 0;
	size_t embedding_bytes;
	int64_t now_ms;

	if (!record || !question || question_len == 0 || !answer ||
	    !embedding || embedding_dim <= 0) {
		return KVS_VECTOR_ERR_INVALID_ARG;
	}

	kvs_vector_record_destroy(record);

	question_copy = kvs_vector_dup_string(question, question_len, &copied_len);
	if (!question_copy) {
		return KVS_VECTOR_ERR;
	}

	answer_copy = kvs_vector_dup_string(answer, answer_len, &copied_len);
	if (!answer_copy) {
		kvs_free(question_copy);
		return KVS_VECTOR_ERR;
	}

	normalized_copy = kvs_vector_dup_string(question, question_len, &copied_len);
	if (!normalized_copy) {
		kvs_free(question_copy);
		kvs_free(answer_copy);
		return KVS_VECTOR_ERR;
	}

	if (scope && scope_len > 0) {
		scope_copy = kvs_vector_dup_string(scope, scope_len, &copied_len);
		if (!scope_copy) {
			kvs_free(question_copy);
			kvs_free(answer_copy);
			kvs_free(normalized_copy);
			return KVS_VECTOR_ERR;
		}
	}

	embedding_bytes = (size_t)embedding_dim * sizeof(float);
	embedding_copy = (float *)kvs_malloc(embedding_bytes);
	if (!embedding_copy) {
		kvs_free(question_copy);
		kvs_free(answer_copy);
		kvs_free(normalized_copy);
		if (scope_copy) {
			kvs_free(scope_copy);
		}
		return KVS_VECTOR_ERR;
	}

	memcpy(embedding_copy, embedding, embedding_bytes);

	now_ms = kv_now_ms();

	record->id = g_vector_state.store.next_id++;
	record->question_hash = kvs_vector_hash_key(question, question_len);

	record->question = question_copy;
	record->question_len = question_len;

	record->answer = answer_copy;
	record->answer_len = answer_len;

	record->normalized_question = normalized_copy;
	record->normalized_question_len = question_len;

	record->scope = scope_copy;
	record->scope_len = (scope && scope_len > 0) ? scope_len : 0;

	record->embedding = embedding_copy;
	record->embedding_dim = embedding_dim;

	record->state = KVS_VECTOR_RECORD_ACTIVE;
	record->deleted = 0;

	record->created_at_ms = now_ms;
	record->updated_at_ms = now_ms;

	return KVS_VECTOR_OK;
}

/*
 * 更新已有 record 的 answer。
 *
 * 注意：
 * 先分配新 answer，成功后再释放旧 answer。
 * 这样可以避免内存分配失败时丢失旧答案。
 */
 int kvs_vector_record_update_answer(
	kvs_vector_record_t *record,
	const char *answer,
	size_t answer_len
) {
	char *answer_copy;

	if (!record || record->state != KVS_VECTOR_RECORD_ACTIVE || record->deleted || !answer) {
		return KVS_VECTOR_ERR_INVALID_ARG;
	}

	answer_copy = kvs_vector_dup_string(answer, answer_len, NULL);
	if (!answer_copy) {
		return KVS_VECTOR_ERR;
	}

	if (record->answer) {
		kvs_free(record->answer);
	}

	record->answer = answer_copy;
	record->answer_len = answer_len;
	record->updated_at_ms = kv_now_ms();

	return KVS_VECTOR_OK;
}

/*
 * 将 record 标记为删除。
 *
 * 注意：
 * 这里采用 lazy delete：
 * 1. 不立即释放 question / answer / scope；
 * 2. 只把状态改成 DELETED；
 * 3. 后续由 VINDEX REBUILD 统一清理 tombstone；
 * 4. VGET / VSEARCH 只命中 ACTIVE 且 deleted == 0 的记录。
 */
 int kvs_vector_record_mark_deleted(kvs_vector_record_t *record) {
	if (!record) {
		return KVS_VECTOR_ERR_INVALID_ARG;
	}

	if (record->state != KVS_VECTOR_RECORD_ACTIVE || record->deleted) {
		return KVS_VECTOR_ERR_NOT_FOUND;
	}

	record->state = KVS_VECTOR_RECORD_DELETED;
	record->deleted = 1;
	record->updated_at_ms = kv_now_ms();

	return KVS_VECTOR_OK;
}

/*
 * 判断一条 record 是否匹配 question + scope。
 *
 * 精确命中语义：
 * 1. record 必须是 ACTIVE；
 * 2. record 不能被删除；
 * 3. question 长度和内容必须完全一致；
 * 4. scope 长度和内容必须完全一致；
 * 5. NULL scope 和空 scope 视为等价。
 */
 int kvs_vector_record_match(
	const kvs_vector_record_t *record,
	const char *question,
	size_t question_len,
	const char *scope,
	size_t scope_len
) {
	if (!record) {
		return 0;
	}

	if (record->state != KVS_VECTOR_RECORD_ACTIVE || record->deleted) {
		return 0;
	}

	if (!kvs_vector_text_equal(record->question, record->question_len, question, question_len)) {
		return 0;
	}

	if (!kvs_vector_scope_equal(record->scope, record->scope_len, scope, scope_len)) {
		return 0;
	}

	return 1;
}

/*
 * 判断 record 是否属于指定 scope。
 *
 * VSEARCH 如果传入 scope，只搜索该 scope 下的记录；
 * 如果 scope 为空，只搜索空 scope 记录。
 */
 int kvs_vector_record_scope_match(
	const kvs_vector_record_t *record,
	const char *scope,
	size_t scope_len
) {
	if (!record) {
		return 0;
	}

	if (record->state != KVS_VECTOR_RECORD_ACTIVE || record->deleted) {
		return 0;
	}

	return kvs_vector_scope_equal(record->scope, record->scope_len, scope, scope_len);
}

/*
 * 从 record 填充搜索结果。
 *
 * result 只引用 record 内部字段，不拥有内存。
 * 调用方不能释放 result->question / answer / scope。
 */
 void kvs_vector_search_result_from_record(
	kvs_vector_search_result_t *result,
	const kvs_vector_record_t *record
) {
	if (!result) {
		return;
	}

	kvs_vector_search_result_reset(result);

	if (!record || record->state != KVS_VECTOR_RECORD_ACTIVE || record->deleted) {
		return;
	}

	result->question = record->question;
	result->question_len = record->question_len;

	result->answer = record->answer;
	result->answer_len = record->answer_len;

	result->scope = record->scope;
	result->scope_len = record->scope_len;

	result->score = 1.0f;
}

/*
 * CLEANUP_CANDIDATE:
 * Currently unused legacy text scoring path.
 * Keep temporarily for safe cleanup review.
 *
 * Was: VSEARCH MVP character-level overlap count.
 * Superseded by vector embedding dot product.
 */
static size_t kvs_vector_char_overlap_count(
	const char *lhs,
	size_t lhs_len,
	const char *rhs,
	size_t rhs_len
) {
	size_t i;
	size_t j;
	size_t count = 0;

	if (!lhs || !rhs || lhs_len == 0 || rhs_len == 0) {
		return 0;
	}

	for (i = 0; i < lhs_len; ++i) {
		for (j = 0; j < rhs_len; ++j) {
			if (lhs[i] == rhs[j]) {
				count++;
				break;
			}
		}
	}

	return count;
}

/*
 * CLEANUP_CANDIDATE:
 * Currently unused legacy text scoring path.
 * Keep temporarily for safe cleanup review.
 *
 * Was: character-level similarity scoring.
 * Superseded by vector embedding dot product.
 */
static float kvs_vector_text_score(
	const char *query,
	size_t query_len,
	const char *question,
	size_t question_len
) {
	size_t overlap;
	size_t base_len;
	float score;

	if (!query || !question || query_len == 0 || question_len == 0) {
		return 0.0f;
	}

	if (kvs_vector_text_equal(query, query_len, question, question_len)) {
		return 1.0f;
	}

	if (query_len <= question_len && strstr(question, query) != NULL) {
		return 0.85f;
	}

	if (question_len <= query_len && strstr(query, question) != NULL) {
		return 0.85f;
	}

	overlap = kvs_vector_char_overlap_count(query, query_len, question, question_len);
	base_len = query_len > question_len ? query_len : question_len;

	if (base_len == 0 || overlap == 0) {
		return 0.0f;
	}

	score = (float)overlap / (float)base_len;

	/*
	 * 字符重叠分不要超过包含匹配分。
	 */
	if (score > 0.8f) {
		score = 0.8f;
	}

	return score;
}

/*
 * CLEANUP_CANDIDATE:
 * Currently unused legacy text scoring path. No current caller found.
 * Keep temporarily for safe cleanup review.
 * Remove together with declaration in kvs_vector.h after Index abstraction.
 */
 float kvs_vector_record_score(
	const kvs_vector_record_t *record,
	const char *query,
	size_t query_len
) {
	float score_question;
	float score_normalized;

	if (!record || record->state != KVS_VECTOR_RECORD_ACTIVE || record->deleted) {
		return 0.0f;
	}

	score_question = kvs_vector_text_score(
		query,
		query_len,
		record->question,
		record->question_len
	);

	score_normalized = kvs_vector_text_score(
		query,
		query_len,
		record->normalized_question,
		record->normalized_question_len
	);

	return score_question > score_normalized ? score_question : score_normalized;
}

/*
 * 释放 store free list。
 */
static void kvs_vector_store_free_list_destroy(kvs_vector_store_t *store) {
	if (!store) {
		return;
	}

	if (store->free_indices) {
		kvs_free(store->free_indices);
		store->free_indices = NULL;
	}

	store->free_count = 0;
	store->free_capacity = 0;
}

/*
 * 确保 free list 至少能保存 min_capacity 个槽位。
 */
static int kvs_vector_store_free_list_reserve(
	kvs_vector_store_t *store,
	size_t min_capacity
) {
	size_t new_capacity;
	size_t *new_indices;

	if (!store) {
		return KVS_VECTOR_ERR_INVALID_ARG;
	}

	if (store->free_capacity >= min_capacity) {
		return KVS_VECTOR_OK;
	}

	new_capacity = store->free_capacity ? store->free_capacity : KVS_VECTOR_INITIAL_RECORD_CAPACITY;
	while (new_capacity < min_capacity) {
		new_capacity *= 2;
	}

	new_indices = (size_t *)kvs_malloc(new_capacity * sizeof(size_t));
	if (!new_indices) {
		return KVS_VECTOR_ERR;
	}

	if (store->free_indices && store->free_count > 0) {
		memcpy(new_indices, store->free_indices, store->free_count * sizeof(size_t));
		kvs_free(store->free_indices);
	}

	store->free_indices = new_indices;
	store->free_capacity = new_capacity;

	return KVS_VECTOR_OK;
}

/*
 * 把一个 DELETED 槽位加入 free list。
 */
 int kvs_vector_store_free_list_push(
	kvs_vector_store_t *store,
	size_t record_index
) {
	int ret;

	if (!store) {
		return KVS_VECTOR_ERR_INVALID_ARG;
	}

	ret = kvs_vector_store_free_list_reserve(store, store->free_count + 1);
	if (ret != KVS_VECTOR_OK) {
		return ret;
	}

	store->free_indices[store->free_count++] = record_index;
	return KVS_VECTOR_OK;
}

/*
 * 从 free list 弹出一个可复用槽位。
 *
 * 返回：
 * 1 表示成功弹出；
 * 0 表示 free list 为空；
 * -1 表示参数错误。
 */
 int kvs_vector_store_free_list_pop(
	kvs_vector_store_t *store,
	size_t *out_record_index
) {
	if (!store || !out_record_index) {
		return -1;
	}

	while (store->free_count > 0) {
		size_t idx = store->free_indices[--store->free_count];

		if (idx < store->capacity &&
			store->records[idx].state == KVS_VECTOR_RECORD_DELETED) {
			*out_record_index = idx;
			return 1;
		}
	}

	return 0;
}

/*
 * 清空 free list。
 *
 * VINDEX REBUILD / compact 后 record 下标会重新排列，旧 free list 必须清空。
 */
 void kvs_vector_store_free_list_clear(kvs_vector_store_t *store) {
	if (!store) {
		return;
	}

	store->free_count = 0;
}

/*
 * 初始化 store。
 */
 int kvs_vector_store_init(kvs_vector_store_t *store) {
	if (!store) {
		return KVS_VECTOR_ERR_INVALID_ARG;
	}

	memset(store, 0, sizeof(*store));
	store->next_id = 1;
	store->next_slot = 0;

	return KVS_VECTOR_OK;
}

/*
 * 销毁 store。
 *
 * 会释放所有 record 内部资源和 records 数组。
 */
 void kvs_vector_store_destroy(kvs_vector_store_t *store) {
	size_t i;

	if (!store) {
		return;
	}

	if (store->records) {
		for (i = 0; i < store->capacity; ++i) {
			kvs_vector_record_destroy(&store->records[i]);
		}

		kvs_free(store->records);
		store->records = NULL;
	}

	//释放空闲表
	kvs_vector_store_free_list_destroy(store);

	memset(store, 0, sizeof(*store));
	store->next_id = 1;
	store->next_slot = 0;
}

/*
 * 扩容 record 数组。
 *
 * 当前只支持扩容，不支持缩容。
 * 旧 record 结构体采用浅拷贝迁移：
 * 1. 指针所有权随结构体迁移到新数组；
 * 2. 旧数组释放时不能逐个 destroy；
 * 3. 只释放旧数组本身。
 */
 int kvs_vector_store_reserve(kvs_vector_store_t *store, size_t new_capacity) {
	kvs_vector_record_t *new_records;
	size_t old_capacity;
	size_t i;

	if (!store) {
		return KVS_VECTOR_ERR_INVALID_ARG;
	}

	if (new_capacity <= store->capacity) {
		return KVS_VECTOR_OK;
	}

	new_records = (kvs_vector_record_t *)kvs_malloc(new_capacity * sizeof(kvs_vector_record_t));
	if (!new_records) {
		return KVS_VECTOR_ERR;
	}

	memset(new_records, 0, new_capacity * sizeof(kvs_vector_record_t));

	old_capacity = store->capacity;

	if (store->records && old_capacity > 0) {
		memcpy(new_records, store->records, old_capacity * sizeof(kvs_vector_record_t));
		kvs_free(store->records);
	}

	for (i = old_capacity; i < new_capacity; ++i) {
		kvs_vector_record_reset(&new_records[i]);
	}

	store->records = new_records;
	store->capacity = new_capacity;

	return KVS_VECTOR_OK;
}

/*
 * 确保 store 至少有 min_capacity 个槽位。
 */
 int kvs_vector_store_ensure_capacity(kvs_vector_store_t *store, size_t min_capacity) {
	size_t new_capacity;

	if (!store) {
		return KVS_VECTOR_ERR_INVALID_ARG;
	}

	if (store->capacity >= min_capacity) {
		return KVS_VECTOR_OK;
	}

	new_capacity = store->capacity ? store->capacity : KVS_VECTOR_INITIAL_RECORD_CAPACITY;

	while (new_capacity < min_capacity) {
		new_capacity *= 2;
	}

	return kvs_vector_store_reserve(store, new_capacity);
}

/*
 * 压缩 store，清理 DELETED record。
 *
 * 稳定语义：
 * 1. ACTIVE record 会被移动到新数组前部；
 * 2. DELETED record 会释放内部资源；
 * 3. EMPTY record 直接丢弃；
 * 4. capacity 保持不变，避免频繁缩容扩容；
 * 5. active_records 重新计算；
 * 6. deleted_records 清零；
 * 7. total_records 重置为 active_records。
 *
 * 注意：
 * ACTIVE record 采用浅拷贝移动。
 * 移动后不能对旧 active record 调用 destroy，否则会 double free。
 */
 int kvs_vector_store_compact(kvs_vector_store_t *store) {
	kvs_vector_record_t *old_records;
	kvs_vector_record_t *new_records;
	size_t old_capacity;
	size_t i;
	size_t write_index = 0;
	uint64_t next_id;

	if (!store) {
		return KVS_VECTOR_ERR_INVALID_ARG;
	}

	if (!store->records || store->capacity == 0) {
		store->total_records = 0;
		store->active_records = 0;
		store->deleted_records = 0;
		store->next_slot = 0;
		kvs_vector_store_free_list_clear(store);
		return KVS_VECTOR_OK;
	}

	old_records = store->records;
	old_capacity = store->capacity;
	next_id = store->next_id;

	new_records = (kvs_vector_record_t *)kvs_malloc(
		old_capacity * sizeof(kvs_vector_record_t)
	);
	if (!new_records) {
		return KVS_VECTOR_ERR;
	}

	memset(new_records, 0, old_capacity * sizeof(kvs_vector_record_t));

	for (i = 0; i < old_capacity; ++i) {
		kvs_vector_record_reset(&new_records[i]);
	}

	for (i = 0; i < old_capacity; ++i) {
		kvs_vector_record_t *record = &old_records[i];

		if (record->state == KVS_VECTOR_RECORD_ACTIVE && !record->deleted) {
			/*
			 * 浅拷贝移动 ACTIVE record。
			 * 指针所有权转移到 new_records。
			 */
			new_records[write_index] = *record;
			write_index++;
		} else {
			kvs_vector_record_destroy(record);
		}
	}

	kvs_free(old_records);

	store->records = new_records;
	store->capacity = old_capacity;
	store->active_records = write_index;
	store->deleted_records = 0;
	store->total_records = write_index;
	store->next_slot = write_index;
	store->next_id = next_id;

	kvs_vector_store_free_list_clear(store);

	return KVS_VECTOR_OK;
}

/*
 * 重置 id map entry。
 */
static void kvs_vector_id_entry_reset(kvs_vector_id_entry_t *entry) {
	if (!entry) {
		return;
	}

	memset(entry, 0, sizeof(*entry));
	entry->state = KVS_VECTOR_MAP_ENTRY_EMPTY;
}

/*
 * 初始化 id map。
 */
 int kvs_vector_id_map_init(kvs_vector_id_map_t *map) {
	if (!map) {
		return KVS_VECTOR_ERR_INVALID_ARG;
	}

	memset(map, 0, sizeof(*map));

	return KVS_VECTOR_OK;
}

/*
 * 销毁 id map。
 */
 void kvs_vector_id_map_destroy(kvs_vector_id_map_t *map) {
	if (!map) {
		return;
	}

	if (map->entries) {
		kvs_free(map->entries);
		map->entries = NULL;
	}

	memset(map, 0, sizeof(*map));
}
/*
 * 插入已经存在的 entry。
 *
 * 这个函数用于 reserve / rehash 时搬迁旧 entry。
 * 当前只根据 hash 做开放寻址放置。
 *
 * 注意：
 * 真正业务 lookup 时还必须二次比较 question + scope，
 * 仅靠 hash 不够。
 */
static int kvs_vector_id_map_insert_entry(
	kvs_vector_id_map_t *map,
	const kvs_vector_id_entry_t *src
) {
	size_t mask;
	size_t pos;

	if (!map || !src || !map->entries || map->capacity == 0) {
		return KVS_VECTOR_ERR_INVALID_ARG;
	}

	mask = map->capacity - 1;
	pos = ((size_t)src->hash) & mask;

	while (map->entries[pos].state == KVS_VECTOR_MAP_ENTRY_USED) {
		pos = (pos + 1) & mask;
	}

	map->entries[pos] = *src;
	map->entries[pos].state = KVS_VECTOR_MAP_ENTRY_USED;
	map->size++;

	return KVS_VECTOR_OK;
}

/*
 * 扩容 / 重建 id map。
 *
 * 用于初始化空表，以及 VSET/VDEL 写入时自动扩容。
 */
 int kvs_vector_id_map_reserve(kvs_vector_id_map_t *map, size_t new_capacity) {
	kvs_vector_id_entry_t *old_entries;
	kvs_vector_id_entry_t *new_entries;
	size_t old_capacity;
	size_t target_capacity;
	size_t i;
	int ret;

	if (!map) {
		return KVS_VECTOR_ERR_INVALID_ARG;
	}

	target_capacity = kvs_vector_next_pow2(new_capacity);
	if (target_capacity < KVS_VECTOR_INITIAL_ID_MAP_CAPACITY) {
		target_capacity = KVS_VECTOR_INITIAL_ID_MAP_CAPACITY;
	}

	if (target_capacity <= map->capacity) {
		return KVS_VECTOR_OK;
	}

	new_entries = (kvs_vector_id_entry_t *)kvs_malloc(target_capacity * sizeof(kvs_vector_id_entry_t));
	if (!new_entries) {
		return KVS_VECTOR_ERR;
	}

	memset(new_entries, 0, target_capacity * sizeof(kvs_vector_id_entry_t));

	old_entries = map->entries;
	old_capacity = map->capacity;

	map->entries = new_entries;
	map->capacity = target_capacity;
	map->size = 0;
	map->tombstones = 0;

	for (i = 0; i < target_capacity; ++i) {
		kvs_vector_id_entry_reset(&map->entries[i]);
	}

	if (old_entries) {
		for (i = 0; i < old_capacity; ++i) {
			if (old_entries[i].state == KVS_VECTOR_MAP_ENTRY_USED) {
				ret = kvs_vector_id_map_insert_entry(map, &old_entries[i]);
				if (ret != KVS_VECTOR_OK) {
					kvs_free(old_entries);
					return ret;
				}
			}
		}

		kvs_free(old_entries);
	}

	return KVS_VECTOR_OK;
}

/*
 * 判断 id_map 是否需要扩容。
 *
 * 使用开放寻址时，负载过高会显著增加探测长度。
 * 这里保守控制在 70% 左右。
 */
 static int kvs_vector_id_map_need_grow(kvs_vector_id_map_t *map) {
	if (!map || map->capacity == 0) {
		return 1;
	}

	return ((map->size + map->tombstones + 1) * 100 >= map->capacity * 70);
}

/*
 * 在 id_map 中查找 slot。
 *
 * for_insert:
 *   0：只查找已存在记录；
 *   1：查找插入位置，允许返回 tombstone。
 *
 * found:
 *   1：找到完全匹配的 USED entry；
 *   0：未找到。
 */
static int kvs_vector_id_map_lookup_slot(
	kvs_vector_id_map_t *map,
	uint32_t hash,
	const char *question,
	size_t question_len,
	const char *scope,
	size_t scope_len,
	int for_insert,
	size_t *out_slot,
	int *found
) {
	kvs_vector_store_t *store = &g_vector_state.store;
	size_t mask;
	size_t pos;
	size_t first_tombstone = (size_t)-1;

	if (found) {
		*found = 0;
	}

	if (!map || !map->entries || map->capacity == 0 || !out_slot) {
		return KVS_VECTOR_ERR_INVALID_ARG;
	}

	mask = map->capacity - 1;
	pos = ((size_t)hash) & mask;

	for (;;) {
		kvs_vector_id_entry_t *entry = &map->entries[pos];

		if (entry->state == KVS_VECTOR_MAP_ENTRY_EMPTY) {
			if (for_insert && first_tombstone != (size_t)-1) {
				*out_slot = first_tombstone;
			} else {
				*out_slot = pos;
			}
			return KVS_VECTOR_OK;
		}

		if (entry->state == KVS_VECTOR_MAP_ENTRY_TOMBSTONE) {
			if (for_insert && first_tombstone == (size_t)-1) {
				first_tombstone = pos;
			}
		} else if (entry->state == KVS_VECTOR_MAP_ENTRY_USED) {
			if (entry->hash == hash && entry->record_index < store->capacity) {
				kvs_vector_record_t *record = &store->records[entry->record_index];

				if (kvs_vector_record_match(
						record,
						question,
						question_len,
						scope,
						scope_len)) {
					*out_slot = pos;
					if (found) {
						*found = 1;
					}
					return KVS_VECTOR_OK;
				}
			}
		}

		pos = (pos + 1) & mask;
	}
}

/*
 * 根据 question + scope 查找 record_index。
 */
 int kvs_vector_id_map_get(
	kvs_vector_id_map_t *map,
	const char *question,
	size_t question_len,
	const char *scope,
	size_t scope_len,
	size_t *out_record_index
) {
	uint32_t hash;
	size_t slot = 0;
	int found = 0;
	int ret;

	if (!map || !question || question_len == 0 || !out_record_index) {
		return KVS_VECTOR_ERR_INVALID_ARG;
	}

	hash = kvs_vector_compose_hash(question, question_len, scope, scope_len);

	ret = kvs_vector_id_map_lookup_slot(
		map,
		hash,
		question,
		question_len,
		scope,
		scope_len,
		0,
		&slot,
		&found
	);
	if (ret != KVS_VECTOR_OK) {
		return ret;
	}

	if (!found) {
		return KVS_VECTOR_ERR_NOT_FOUND;
	}

	*out_record_index = map->entries[slot].record_index;
	return KVS_VECTOR_OK;
}

/*
 * 写入 question + scope 到 record_index 的映射。
 */
 int kvs_vector_id_map_put(
	kvs_vector_id_map_t *map,
	const char *question,
	size_t question_len,
	const char *scope,
	size_t scope_len,
	size_t record_index
) {
	uint32_t hash;
	size_t slot = 0;
	int found = 0;
	int ret;

	if (!map || !question || question_len == 0) {
		return KVS_VECTOR_ERR_INVALID_ARG;
	}

	if (kvs_vector_id_map_need_grow(map)) {
		size_t new_capacity = map->capacity ? map->capacity * 2 : KVS_VECTOR_INITIAL_ID_MAP_CAPACITY;
		ret = kvs_vector_id_map_reserve(map, new_capacity);
		if (ret != KVS_VECTOR_OK) {
			return ret;
		}
	}

	hash = kvs_vector_compose_hash(question, question_len, scope, scope_len);

	ret = kvs_vector_id_map_lookup_slot(
		map,
		hash,
		question,
		question_len,
		scope,
		scope_len,
		1,
		&slot,
		&found
	);
	if (ret != KVS_VECTOR_OK) {
		return ret;
	}

	if (found) {
		map->entries[slot].record_index = record_index;
		return KVS_VECTOR_OK;
	}

	if (map->entries[slot].state == KVS_VECTOR_MAP_ENTRY_TOMBSTONE &&
		map->tombstones > 0) {
		map->tombstones--;
	}

	map->entries[slot].hash = hash;
	map->entries[slot].record_index = record_index;
	map->entries[slot].state = KVS_VECTOR_MAP_ENTRY_USED;
	map->size++;

	return KVS_VECTOR_OK;
}

/*
 * 删除 question + scope 映射。
 */
 int kvs_vector_id_map_remove(
	kvs_vector_id_map_t *map,
	const char *question,
	size_t question_len,
	const char *scope,
	size_t scope_len
) {
	uint32_t hash;
	size_t slot = 0;
	int found = 0;
	int ret;

	if (!map || !question || question_len == 0) {
		return KVS_VECTOR_ERR_INVALID_ARG;
	}

	hash = kvs_vector_compose_hash(question, question_len, scope, scope_len);

	ret = kvs_vector_id_map_lookup_slot(
		map,
		hash,
		question,
		question_len,
		scope,
		scope_len,
		0,
		&slot,
		&found
	);
	if (ret != KVS_VECTOR_OK) {
		return ret;
	}

	if (!found) {
		return KVS_VECTOR_ERR_NOT_FOUND;
	}

	map->entries[slot].state = KVS_VECTOR_MAP_ENTRY_TOMBSTONE;
	map->entries[slot].record_index = 0;
	map->entries[slot].hash = 0;

	if (map->size > 0) {
		map->size--;
	}
	map->tombstones++;

	return KVS_VECTOR_OK;
}

/*
 * 根据当前 store 重建 id_map。
 *
 * VINDEX REBUILD / compact 后 record_index 会变化，所以必须重建 id_map。
 */
 int kvs_vector_id_map_rebuild_from_store(void) {
	kvs_vector_id_map_t *map = &g_vector_state.id_map;
	kvs_vector_store_t *store = &g_vector_state.store;
	size_t target_capacity;
	size_t i;
	int ret;

	kvs_vector_id_map_destroy(map);

	ret = kvs_vector_id_map_init(map);
	if (ret != KVS_VECTOR_OK) {
		return ret;
	}

	target_capacity = store->active_records * 2;
	if (target_capacity < KVS_VECTOR_INITIAL_ID_MAP_CAPACITY) {
		target_capacity = KVS_VECTOR_INITIAL_ID_MAP_CAPACITY;
	}

	ret = kvs_vector_id_map_reserve(map, target_capacity);
	if (ret != KVS_VECTOR_OK) {
		return ret;
	}

	for (i = 0; i < store->capacity; ++i) {
		kvs_vector_record_t *record = &store->records[i];

		if (record->state != KVS_VECTOR_RECORD_ACTIVE || record->deleted) {
			continue;
		}

		ret = kvs_vector_id_map_put(
			map,
			record->question,
			record->question_len,
			record->scope,
			record->scope_len,
			i
		);
		if (ret != KVS_VECTOR_OK) {
			return ret;
		}
	}

	return KVS_VECTOR_OK;
}

/*
 * 初始化 flat index。
 *
 * 当前不分配 vectors，因为 dimension = 0。
 * 后续有 embedding 维度后，再按 capacity * dimension 分配。
 */
 int kvs_vector_flat_index_init(kvs_vector_flat_index_t *index, size_t dimension) {
	if (!index) {
		return KVS_VECTOR_ERR_INVALID_ARG;
	}

	memset(index, 0, sizeof(*index));

	index->vectors = NULL;
	index->record_indices = NULL;
	index->size = 0;
	index->capacity = 0;
	index->dimension = dimension;
	index->ready = (dimension > 0) ? 1 : 0;
	index->needs_rebuild = 0;

	return KVS_VECTOR_OK;
}

/*
 * 释放 flat index。
 */
 void kvs_vector_flat_index_destroy(kvs_vector_flat_index_t *index) {
	if (!index) {
		return;
	}

	if (index->vectors) {
		kvs_free(index->vectors);
		index->vectors = NULL;
	}

	if (index->record_indices) {
		kvs_free(index->record_indices);
		index->record_indices = NULL;
	}

	if(index->record_ids) {
		kvs_free(index->record_ids);
		index->record_ids = NULL;
	}

	memset(index, 0, sizeof(*index));
}

/*
 * 重置 flat index 状态。
 *
 * 当前实现会释放已分配内存。
 * 后续如需优化，可以改成保留 capacity。
 */
 void kvs_vector_flat_index_reset(kvs_vector_flat_index_t *index) {
	if (!index) {
		return;
	}

	kvs_vector_flat_index_destroy(index);
	kvs_vector_flat_index_init(index, kvs_vector_embedding_provider_dimension());
}

/*
 * 标记 flat index 已陈旧。
 *
 * 当前还没有真正的 embedding / flat index。
 * 但 VDEL 会改变可检索记录集合，所以先把 needs_rebuild 置为 1。
 */
 void kvs_vector_flat_index_mark_stale(kvs_vector_flat_index_t *index) {
	if (!index) {
		return;
	}

	index->ready = 0;
	index->needs_rebuild = 1;
}

/*
 * CLEANUP_CANDIDATE:
 * Obsolete placeholder — fully replaced by kvs_vector_rebuild_flat_index_from_store().
 * Safe to remove after Index abstraction phase if still unused.
 */
//  int kvs_vector_rebuild_flat_index_placeholder(void) {
// 	kvs_vector_flat_index_reset(&g_vector_state.flat_index);
// 	g_vector_state.flat_index.ready = 0;
// 	g_vector_state.flat_index.needs_rebuild = 0;
// 	g_vector_state.flat_index.size = 0;
// 	g_vector_state.flat_index.dimension = KVS_VECTOR_DEFAULT_DIMENSION;

// 	return KVS_VECTOR_OK;
// }
/*
 * 确保 flat index 至少能容纳 new_capacity 个向量。
 */
 int kvs_vector_flat_index_reserve(
	kvs_vector_flat_index_t *index,
	size_t new_capacity
) {
	float *new_vectors;
	size_t *new_record_indices;
	uint64_t *new_record_ids;
	size_t old_vector_count;
	size_t new_vector_count;
	size_t new_real_capacity;

	if (!index || index->dimension == 0) {
		return KVS_VECTOR_ERR_INVALID_ARG;
	}

	if (new_capacity <= index->capacity) {
		return KVS_VECTOR_OK;
	}

	new_real_capacity = index->capacity ? index->capacity : KVS_VECTOR_INITIAL_RECORD_CAPACITY;
	while (new_real_capacity < new_capacity) {
		new_real_capacity *= 2;
	}

	old_vector_count = index->capacity * index->dimension;
	new_vector_count = new_real_capacity * index->dimension;

	new_vectors = (float *)kvs_malloc(new_vector_count * sizeof(float));
	if (!new_vectors) {
		return KVS_VECTOR_ERR;
	}

	new_record_indices = (size_t *)kvs_malloc(new_real_capacity * sizeof(size_t));
	if (!new_record_indices) {
		kvs_free(new_vectors);
		return KVS_VECTOR_ERR;
	}

	new_record_ids = (uint64_t *)kvs_malloc(new_real_capacity * sizeof(uint64_t));
	if (!new_record_ids) {
		kvs_free(new_vectors);
		kvs_free(new_record_indices);
		return KVS_VECTOR_ERR;
	}

	memset(new_vectors, 0, new_vector_count * sizeof(float));
	memset(new_record_indices, 0, new_real_capacity * sizeof(size_t));
	memset(new_record_ids, 0, new_real_capacity * sizeof(uint64_t));

	if (index->vectors && old_vector_count > 0) {
		memcpy(new_vectors, index->vectors, old_vector_count * sizeof(float));
	}

	if (index->record_indices && index->capacity > 0) {
		memcpy(new_record_indices, index->record_indices, index->capacity * sizeof(size_t));
	}

	if (index->record_ids && index->capacity > 0) {
		memcpy(new_record_ids, index->record_ids, index->capacity * sizeof(uint64_t));
	}

	if (index->vectors) {
		kvs_free(index->vectors);
	}

	if (index->record_indices) {
		kvs_free(index->record_indices);
	}

	if (index->record_ids) {
		kvs_free(index->record_ids);
	}

	index->vectors = new_vectors;
	index->record_indices = new_record_indices;
	index->record_ids = new_record_ids;
	index->capacity = new_real_capacity;

	return KVS_VECTOR_OK;
}

/*
 * 向 flat index 追加一个 record 的 embedding。
 */
 int kvs_vector_flat_index_add(
	kvs_vector_flat_index_t *index,
	size_t record_index,
	const float *embedding,
	int embedding_dim
) {
	int ret;
	float *dst;
	kvs_vector_record_t *record;

	if (!index || !embedding || embedding_dim <= 0) {
		return KVS_VECTOR_ERR_INVALID_ARG;
	}

	if (record_index >= g_vector_state.store.capacity) {
		return KVS_VECTOR_ERR_INVALID_ARG;
	}

	record = &g_vector_state.store.records[record_index];
	if (record->state != KVS_VECTOR_RECORD_ACTIVE || record->deleted) {
		return KVS_VECTOR_ERR_INVALID_ARG;
	}

	if (index->dimension == 0) {
		index->dimension = (size_t)embedding_dim;
	}

	if ((size_t)embedding_dim != index->dimension) {
		return KVS_VECTOR_ERR_INVALID_ARG;
	}

	ret = kvs_vector_flat_index_reserve(index, index->size + 1);
	if (ret != KVS_VECTOR_OK) {
		return ret;
	}

	dst = index->vectors + (index->size * index->dimension);
	memcpy(dst, embedding, index->dimension * sizeof(float));

	index->record_indices[index->size] = record_index;
	index->record_ids[index->size] = record->id;

	index->size++;
	index->ready = 1;

	return KVS_VECTOR_OK;
}

/*
 * 重建 flat index。
 *
 * 会扫描当前 store 中所有 ACTIVE record，把它们的 embedding 重新写入连续数组。
 */
 int kvs_vector_rebuild_flat_index_from_store(void) {
	kvs_vector_flat_index_t *index = &g_vector_state.flat_index;
	kvs_vector_store_t *store = &g_vector_state.store;
	size_t i;
	int ret;

	kvs_vector_flat_index_reset(index);
	index->dimension = kvs_vector_embedding_provider_dimension();

	if (!store->records || store->capacity == 0 || store->active_records == 0) {
		index->ready = 1;
		index->needs_rebuild = 0;
		index->size = 0;
		return KVS_VECTOR_OK;
	}

	ret = kvs_vector_flat_index_reserve(index, store->active_records);
	if (ret != KVS_VECTOR_OK) {
		return ret;
	}

	for (i = 0; i < store->capacity; ++i) {
		kvs_vector_record_t *record = &store->records[i];

		if (record->state != KVS_VECTOR_RECORD_ACTIVE || record->deleted) {
			continue;
		}

		if (!record->embedding || record->embedding_dim <= 0) {
			continue;
		}

		ret = kvs_vector_flat_index_add(
			index,
			i,
			record->embedding,
			record->embedding_dim
		);
		if (ret != KVS_VECTOR_OK) {
			return ret;
		}
	}

	index->ready = 1;
	index->needs_rebuild = 0;

	return KVS_VECTOR_OK;
}

/*
 * dot product。
 *
 * 当前 embedding 已做 L1 归一化，所以 dot product 可以作为稳定的相似度分量。
 */
 float kvs_vector_dot_product(
	const float *lhs,
	const float *rhs,
	size_t dim
) {
	size_t i;
	float score = 0.0f;

	if (!lhs || !rhs || dim == 0) {
		return 0.0f;
	}

	for (i = 0; i < dim; ++i) {
		score += lhs[i] * rhs[i];
	}

	return score;
}





/********** topK min heap **********/
 void kvs_vector_topk_heap_swap(
	kvs_vector_candidate_t *lhs,
	kvs_vector_candidate_t *rhs
) {
	kvs_vector_candidate_t tmp;

	if (!lhs || !rhs) {
		return;
	}

	tmp = *lhs;
	*lhs = *rhs;
	*rhs = tmp;
}

/*
 * 小根堆比较：
 * score 小的优先级更低，放在堆顶。
 * 如果 score 相同，record_index 大的优先级更低，保证结果更稳定。
 */
 int kvs_vector_topk_candidate_less(
	const kvs_vector_candidate_t *lhs,
	const kvs_vector_candidate_t *rhs
) {
	if (lhs->score < rhs->score) {
		return 1;
	}

	if (lhs->score > rhs->score) {
		return 0;
	}

	return lhs->record_index > rhs->record_index;
}

 void kvs_vector_topk_heap_sift_up(
	kvs_vector_topk_heap_t *heap,
	size_t pos
) {
	while (pos > 0) {
		size_t parent = (pos - 1) / 2;

		if (!kvs_vector_topk_candidate_less(&heap->items[pos], &heap->items[parent])) {
			break;
		}

		kvs_vector_topk_heap_swap(&heap->items[pos], &heap->items[parent]);
		pos = parent;
	}
}

 void kvs_vector_topk_heap_sift_down(
	kvs_vector_topk_heap_t *heap,
	size_t pos
) {
	for (;;) {
		size_t left = pos * 2 + 1;
		size_t right = left + 1;
		size_t smallest = pos;

		if (left < heap->size &&
			kvs_vector_topk_candidate_less(&heap->items[left], &heap->items[smallest])) {
			smallest = left;
		}

		if (right < heap->size &&
			kvs_vector_topk_candidate_less(&heap->items[right], &heap->items[smallest])) {
			smallest = right;
		}

		if (smallest == pos) {
			break;
		}

		kvs_vector_topk_heap_swap(&heap->items[pos], &heap->items[smallest]);
		pos = smallest;
	}
}

int kvs_vector_topk_heap_init(kvs_vector_topk_heap_t *heap, size_t capacity) {
	if (!heap || capacity == 0) {
		return KVS_VECTOR_ERR_INVALID_ARG;
	}

	memset(heap, 0, sizeof(*heap));

	heap->items = (kvs_vector_candidate_t *)kvs_malloc(
		capacity * sizeof(kvs_vector_candidate_t)
	);
	if (!heap->items) {
		return KVS_VECTOR_ERR;
	}

	memset(heap->items, 0, capacity * sizeof(kvs_vector_candidate_t));
	heap->capacity = capacity;
	heap->size = 0;

	return KVS_VECTOR_OK;
}

void kvs_vector_topk_heap_destroy(kvs_vector_topk_heap_t *heap) {
	if (!heap) {
		return;
	}

	if (heap->items) {
		kvs_free(heap->items);
		heap->items = NULL;
	}

	heap->size = 0;
	heap->capacity = 0;
}

void kvs_vector_topk_heap_reset(kvs_vector_topk_heap_t *heap) {
	if (!heap) {
		return;
	}

	heap->size = 0;
}

int kvs_vector_topk_heap_push(
	kvs_vector_topk_heap_t *heap,
	size_t record_index,
	float score
) {
	kvs_vector_candidate_t candidate;

	if (!heap || !heap->items || heap->capacity == 0) {
		return KVS_VECTOR_ERR_INVALID_ARG;
	}

	candidate.record_index = record_index;
	candidate.score = score;

	if (heap->size < heap->capacity) {
		heap->items[heap->size] = candidate;
		kvs_vector_topk_heap_sift_up(heap, heap->size);
		heap->size++;
		return KVS_VECTOR_OK;
	}

	/*
	 * 堆顶是当前 topK 中分数最低的候选。
	 * 新候选不比堆顶好，就直接丢弃。
	 */
	if (score < heap->items[0].score) {
		return KVS_VECTOR_OK;
	}

	if (score == heap->items[0].score && record_index >= heap->items[0].record_index) {
		return KVS_VECTOR_OK;
	}

	heap->items[0] = candidate;
	kvs_vector_topk_heap_sift_down(heap, 0);

	return KVS_VECTOR_OK;
}

 int kvs_vector_candidate_desc_cmp(
	const void *lhs,
	const void *rhs
) {
	const kvs_vector_candidate_t *a = (const kvs_vector_candidate_t *)lhs;
	const kvs_vector_candidate_t *b = (const kvs_vector_candidate_t *)rhs;

	if (a->score < b->score) {
		return 1;
	}

	if (a->score > b->score) {
		return -1;
	}

	if (a->record_index < b->record_index) {
		return -1;
	}

	if (a->record_index > b->record_index) {
		return 1;
	}

	return 0;
}

int kvs_vector_topk_heap_export_desc(
	kvs_vector_topk_heap_t *heap,
	kvs_vector_search_result_t *results,
	size_t results_capacity,
	size_t *results_count,
	const kvs_vector_store_t *store
) {
	size_t i;
	size_t count;

	if (!heap || !results || !results_count || !store) {
		return KVS_VECTOR_ERR_INVALID_ARG;
	}

	*results_count = 0;

	if (results_capacity == 0 || heap->size == 0) {
		return KVS_VECTOR_OK;
	}

	count = heap->size;
	if (count > results_capacity) {
		count = results_capacity;
	}

	qsort(
		heap->items,
		heap->size,
		sizeof(kvs_vector_candidate_t),
		kvs_vector_candidate_desc_cmp
	);

	for (i = 0; i < count; ++i) {
		size_t record_index = heap->items[i].record_index;
		kvs_vector_record_t *record;

		if (record_index >= store->capacity) {
			continue;
		}

		record = &store->records[record_index];
		if (record->state != KVS_VECTOR_RECORD_ACTIVE || record->deleted) {
			continue;
		}

		kvs_vector_search_result_from_record(&results[*results_count], record);
		results[*results_count].score = heap->items[i].score;
		(*results_count)++;
	}

	return KVS_VECTOR_OK;
}

/********** ANN / HNSW multi-layer index **********/

typedef struct kvs_vector_ann_tmp_candidate_s {
	size_t node_index;
	float score;
	int expanded;
} kvs_vector_ann_tmp_candidate_t;

static int kvs_vector_ann_candidate_better(
	const kvs_vector_ann_tmp_candidate_t *lhs,
	const kvs_vector_ann_tmp_candidate_t *rhs
) {
	if (!lhs || !rhs) {
		return 0;
	}

	if (lhs->score > rhs->score) {
		return 1;
	}

	if (lhs->score < rhs->score) {
		return 0;
	}

	return lhs->node_index < rhs->node_index;
}

static int kvs_vector_ann_find_best_unexpanded(
	kvs_vector_ann_tmp_candidate_t *candidates,
	size_t candidate_count,
	size_t *out_index
) {
	size_t i;
	size_t best = (size_t)-1;
	int has_best = 0;

	if (!candidates || !out_index) {
		return 0;
	}

	for (i = 0; i < candidate_count; ++i) {
		if (candidates[i].expanded) {
			continue;
		}

		if (!has_best ||
		    kvs_vector_ann_candidate_better(&candidates[i], &candidates[best])) {
			best = i;
			has_best = 1;
		}
	}

	if (!has_best) {
		return 0;
	}

	*out_index = best;
	return 1;
}

static int kvs_vector_ann_find_worst_result(
	kvs_vector_ann_tmp_candidate_t *results,
	size_t result_count,
	size_t *out_index
) {
	size_t i;
	size_t worst = 0;
	int has_worst = 0;

	if (!results || !out_index || result_count == 0) {
		return 0;
	}

	for (i = 0; i < result_count; ++i) {
		if (!has_worst) {
			worst = i;
			has_worst = 1;
			continue;
		}

		if (results[i].score < results[worst].score ||
		    (results[i].score == results[worst].score &&
		     results[i].node_index > results[worst].node_index)) {
			worst = i;
		}
	}

	*out_index = worst;
	return 1;
}

static float kvs_vector_ann_worst_result_score(
	kvs_vector_ann_tmp_candidate_t *results,
	size_t result_count
) {
	size_t worst_index = 0;

	if (!results || result_count == 0) {
		return -1.0f;
	}

	if (!kvs_vector_ann_find_worst_result(results, result_count, &worst_index)) {
		return -1.0f;
	}

	return results[worst_index].score;
}

static int kvs_vector_ann_result_maybe_add(
	kvs_vector_ann_tmp_candidate_t *results,
	size_t result_capacity,
	size_t *result_count,
	size_t node_index,
	float score
) {
	kvs_vector_ann_tmp_candidate_t candidate;
	size_t worst_index = 0;

	if (!results || !result_count || result_capacity == 0) {
		return KVS_VECTOR_ERR_INVALID_ARG;
	}

	candidate.node_index = node_index;
	candidate.score = score;
	candidate.expanded = 0;

	if (*result_count < result_capacity) {
		results[*result_count] = candidate;
		(*result_count)++;
		return KVS_VECTOR_OK;
	}

	if (!kvs_vector_ann_find_worst_result(results, *result_count, &worst_index)) {
		return KVS_VECTOR_ERR;
	}

	if (!kvs_vector_ann_candidate_better(&candidate, &results[worst_index])) {
		return KVS_VECTOR_OK;
	}

	results[worst_index] = candidate;
	return KVS_VECTOR_OK;
}

static void kvs_vector_ann_node_reset(kvs_vector_ann_node_t *node) {
	if (!node) {
		return;
	}

	memset(node, 0, sizeof(*node));
	node->record_index = (size_t)-1;
	node->record_id = 0;
	node->level = 0;
	node->neighbors = NULL;
	node->neighbor_counts = NULL;
	node->neighbor_capacities = NULL;
	node->active = 0;
}

static void kvs_vector_ann_node_destroy(kvs_vector_ann_node_t *node) {
	int level;

	if (!node) {
		return;
	}

	if (node->neighbors) {
		for (level = 0; level <= node->level; ++level) {
			if (node->neighbors[level]) {
				kvs_free(node->neighbors[level]);
				node->neighbors[level] = NULL;
			}
		}

		kvs_free(node->neighbors);
		node->neighbors = NULL;
	}

	if (node->neighbor_counts) {
		kvs_free(node->neighbor_counts);
		node->neighbor_counts = NULL;
	}

	if (node->neighbor_capacities) {
		kvs_free(node->neighbor_capacities);
		node->neighbor_capacities = NULL;
	}

	kvs_vector_ann_node_reset(node);
}

static int kvs_vector_ann_node_init_layers(
	kvs_vector_ann_node_t *node,
	int level,
	size_t m
) {
	int l;

	if (!node || level < 0 || m == 0) {
		return KVS_VECTOR_ERR_INVALID_ARG;
	}

	if (level >= KVS_VECTOR_HNSW_MAX_LEVEL) {
		level = KVS_VECTOR_HNSW_MAX_LEVEL - 1;
	}

	node->level = level;

	node->neighbors = (size_t **)kvs_malloc((size_t)(level + 1) * sizeof(size_t *));
	if (!node->neighbors) {
		return KVS_VECTOR_ERR;
	}

	node->neighbor_counts = (size_t *)kvs_malloc((size_t)(level + 1) * sizeof(size_t));
	if (!node->neighbor_counts) {
		kvs_free(node->neighbors);
		node->neighbors = NULL;
		return KVS_VECTOR_ERR;
	}

	node->neighbor_capacities = (size_t *)kvs_malloc((size_t)(level + 1) * sizeof(size_t));
	if (!node->neighbor_capacities) {
		kvs_free(node->neighbor_counts);
		kvs_free(node->neighbors);
		node->neighbor_counts = NULL;
		node->neighbors = NULL;
		return KVS_VECTOR_ERR;
	}

	memset(node->neighbors, 0, (size_t)(level + 1) * sizeof(size_t *));
	memset(node->neighbor_counts, 0, (size_t)(level + 1) * sizeof(size_t));
	memset(node->neighbor_capacities, 0, (size_t)(level + 1) * sizeof(size_t));

	for (l = 0; l <= level; ++l) {
		size_t capacity = m;

		/*
		 * HNSW 常见做法是第 0 层连接数更大。
		 * 这里为了保持实现简单，先所有层使用同一个 M。
		 */
		node->neighbors[l] = (size_t *)kvs_malloc(capacity * sizeof(size_t));
		if (!node->neighbors[l]) {
			kvs_vector_ann_node_destroy(node);
			return KVS_VECTOR_ERR;
		}

		memset(node->neighbors[l], 0, capacity * sizeof(size_t));
		node->neighbor_counts[l] = 0;
		node->neighbor_capacities[l] = capacity;
	}

	return KVS_VECTOR_OK;
}

static int kvs_vector_ann_random_level(uint64_t seed, size_t m) {
	int level = 0;
	uint64_t x;

	if (m < 2) {
		m = 2;
	}

	/*
	 * 确定性 pseudo random level。
	 *
	 * 不依赖 rand()，保证 rebuild 后图结构稳定。
	 * 概率近似为：
	 *   P(level >= k) ~= (1 / m)^k
	 */
	x = seed;
	x ^= x >> 33;
	x *= 0xff51afd7ed558ccdULL;
	x ^= x >> 33;
	x *= 0xc4ceb9fe1a85ec53ULL;
	x ^= x >> 33;

	while (level + 1 < KVS_VECTOR_HNSW_MAX_LEVEL) {
		if ((x % m) != 0) {
			break;
		}

		level++;
		x = x / m;
		if (x == 0) {
			x = seed + (uint64_t)level * 0x9e3779b97f4a7c15ULL;
		}
	}

	return level;
}

int kvs_vector_ann_index_init(
	kvs_vector_ann_index_t *index,
	size_t dimension,
	size_t m,
	size_t ef_construction,
	size_t ef_search
) {
	if (!index) {
		return KVS_VECTOR_ERR_INVALID_ARG;
	}

	memset(index, 0, sizeof(*index));

	index->nodes = NULL;
	index->size = 0;
	index->capacity = 0;
	index->dimension = dimension;

	index->entry_node = 0;
	index->has_entry = 0;
	index->max_level = 0;

	index->m = m ? m : KVS_VECTOR_HNSW_M;
	index->ef_construction = ef_construction ? ef_construction : KVS_VECTOR_HNSW_EF_CONSTRUCTION;
	index->ef_search = ef_search ? ef_search : KVS_VECTOR_HNSW_EF_SEARCH;

	if (index->m < 4) {
		index->m = 4;
	}

	if (index->ef_construction < index->m) {
		index->ef_construction = index->m;
	}

	if (index->ef_search < 1) {
		index->ef_search = 1;
	}

	index->ready = dimension > 0 ? 1 : 0;
	index->needs_rebuild = 0;

	return KVS_VECTOR_OK;
}

void kvs_vector_ann_index_destroy(kvs_vector_ann_index_t *index) {
	size_t i;

	if (!index) {
		return;
	}

	if (index->nodes) {
		for (i = 0; i < index->capacity; ++i) {
			kvs_vector_ann_node_destroy(&index->nodes[i]);
		}

		kvs_free(index->nodes);
		index->nodes = NULL;
	}

	memset(index, 0, sizeof(*index));
}

void kvs_vector_ann_index_reset(kvs_vector_ann_index_t *index) {
	size_t dimension;
	size_t m;
	size_t ef_construction;
	size_t ef_search;

	if (!index) {
		return;
	}

	dimension = index->dimension;
	m = index->m;
	ef_construction = index->ef_construction;
	ef_search = index->ef_search;

	kvs_vector_ann_index_destroy(index);
	kvs_vector_ann_index_init(index, dimension, m, ef_construction, ef_search);
}

int kvs_vector_ann_index_reserve(
	kvs_vector_ann_index_t *index,
	size_t new_capacity
) {
	kvs_vector_ann_node_t *new_nodes;
	size_t old_capacity;
	size_t new_real_capacity;
	size_t i;

	if (!index) {
		return KVS_VECTOR_ERR_INVALID_ARG;
	}

	if (new_capacity <= index->capacity) {
		return KVS_VECTOR_OK;
	}

	new_real_capacity = index->capacity ? index->capacity : KVS_VECTOR_INITIAL_RECORD_CAPACITY;
	while (new_real_capacity < new_capacity) {
		new_real_capacity *= 2;
	}

	new_nodes = (kvs_vector_ann_node_t *)kvs_malloc(
		new_real_capacity * sizeof(kvs_vector_ann_node_t)
	);
	if (!new_nodes) {
		return KVS_VECTOR_ERR;
	}

	memset(new_nodes, 0, new_real_capacity * sizeof(kvs_vector_ann_node_t));

	old_capacity = index->capacity;

	if (index->nodes && old_capacity > 0) {
		memcpy(new_nodes, index->nodes, old_capacity * sizeof(kvs_vector_ann_node_t));
		kvs_free(index->nodes);
	}

	for (i = old_capacity; i < new_real_capacity; ++i) {
		kvs_vector_ann_node_reset(&new_nodes[i]);
	}

	index->nodes = new_nodes;
	index->capacity = new_real_capacity;

	return KVS_VECTOR_OK;
}

static const float *kvs_vector_ann_node_embedding(
	kvs_vector_ann_index_t *index,
	size_t node_index
) {
	kvs_vector_ann_node_t *node;
	kvs_vector_record_t *record;

	if (!index || !index->nodes || node_index >= index->size) {
		return NULL;
	}

	node = &index->nodes[node_index];
	if (!node->active || node->record_index >= g_vector_state.store.capacity) {
		return NULL;
	}

	record = &g_vector_state.store.records[node->record_index];
	if (record->state != KVS_VECTOR_RECORD_ACTIVE || record->deleted) {
		return NULL;
	}

	if (!record->embedding || record->embedding_dim <= 0) {
		return NULL;
	}

	if ((size_t)record->embedding_dim != index->dimension) {
		return NULL;
	}

	return record->embedding;
}

static int kvs_vector_ann_candidate_append(
	kvs_vector_ann_tmp_candidate_t *candidates,
	size_t candidate_capacity,
	size_t *candidate_count,
	size_t node_index,
	float score
) {
	if (!candidates || !candidate_count || *candidate_count >= candidate_capacity) {
		return KVS_VECTOR_ERR;
	}

	candidates[*candidate_count].node_index = node_index;
	candidates[*candidate_count].score = score;
	candidates[*candidate_count].expanded = 0;
	(*candidate_count)++;

	return KVS_VECTOR_OK;
}

static int kvs_vector_ann_neighbor_add_or_replace(
	kvs_vector_ann_index_t *index,
	size_t owner_node_index,
	size_t neighbor_node_index,
	int level
) {
	kvs_vector_ann_node_t *owner;
	const float *owner_embedding;
	const float *neighbor_embedding;
	float new_score;
	size_t i;

	if (!index || !index->nodes ||
	    owner_node_index >= index->size ||
	    neighbor_node_index >= index->size ||
	    owner_node_index == neighbor_node_index ||
	    level < 0) {
		return KVS_VECTOR_ERR_INVALID_ARG;
	}

	owner = &index->nodes[owner_node_index];

	if (!owner->active || owner->level < level ||
	    !owner->neighbors || !owner->neighbor_counts || !owner->neighbor_capacities) {
		return KVS_VECTOR_OK;
	}

	for (i = 0; i < owner->neighbor_counts[level]; ++i) {
		if (owner->neighbors[level][i] == neighbor_node_index) {
			return KVS_VECTOR_OK;
		}
	}

	if (owner->neighbor_counts[level] < owner->neighbor_capacities[level]) {
		owner->neighbors[level][owner->neighbor_counts[level]++] = neighbor_node_index;
		return KVS_VECTOR_OK;
	}

	owner_embedding = kvs_vector_ann_node_embedding(index, owner_node_index);
	neighbor_embedding = kvs_vector_ann_node_embedding(index, neighbor_node_index);

	if (!owner_embedding || !neighbor_embedding || owner->neighbor_capacities[level] == 0) {
		return KVS_VECTOR_ERR;
	}

	new_score = kvs_vector_dot_product(
		owner_embedding,
		neighbor_embedding,
		index->dimension
	);

	{
		size_t worst_pos = 0;
		float worst_score = 0.0f;
		int has_worst = 0;

		for (i = 0; i < owner->neighbor_counts[level]; ++i) {
			const float *old_neighbor_embedding;
			float old_score;

			old_neighbor_embedding = kvs_vector_ann_node_embedding(
				index,
				owner->neighbors[level][i]
			);
			if (!old_neighbor_embedding) {
				worst_pos = i;
				worst_score = -1.0f;
				has_worst = 1;
				break;
			}

			old_score = kvs_vector_dot_product(
				owner_embedding,
				old_neighbor_embedding,
				index->dimension
			);

			if (!has_worst || old_score < worst_score) {
				worst_pos = i;
				worst_score = old_score;
				has_worst = 1;
			}
		}

		if (!has_worst || new_score <= worst_score) {
			return KVS_VECTOR_OK;
		}

		owner->neighbors[level][worst_pos] = neighbor_node_index;
	}

	return KVS_VECTOR_OK;
}

static size_t kvs_vector_ann_greedy_search_layer(
	kvs_vector_ann_index_t *index,
	const float *query_embedding,
	int query_dim,
	size_t entry_node,
	int level
) {
	size_t current;
	const float *current_embedding;
	float current_score;
	int changed = 1;

	if (!index || !query_embedding || query_dim <= 0 ||
	    entry_node >= index->size || level < 0 ||
	    (size_t)query_dim != index->dimension) {
		return entry_node;
	}

	current = entry_node;
	current_embedding = kvs_vector_ann_node_embedding(index, current);
	if (!current_embedding) {
		return current;
	}

	current_score = kvs_vector_dot_product(
		query_embedding,
		current_embedding,
		index->dimension
	);

	while (changed) {
		kvs_vector_ann_node_t *node;
		size_t i;

		changed = 0;

		if (current >= index->size) {
			break;
		}

		node = &index->nodes[current];
		if (!node->active || node->level < level || !node->neighbors) {
			break;
		}

		for (i = 0; i < node->neighbor_counts[level]; ++i) {
			size_t neighbor_index = node->neighbors[level][i];
			const float *neighbor_embedding;
			float score;

			if (neighbor_index >= index->size) {
				continue;
			}

			neighbor_embedding = kvs_vector_ann_node_embedding(index, neighbor_index);
			if (!neighbor_embedding) {
				continue;
			}

			score = kvs_vector_dot_product(
				query_embedding,
				neighbor_embedding,
				index->dimension
			);

			if (score > current_score ||
			    (score == current_score && neighbor_index < current)) {
				current = neighbor_index;
				current_score = score;
				changed = 1;
			}
		}
	}

	return current;
}

static int kvs_vector_ann_collect_candidates_layer(
	kvs_vector_ann_index_t *index,
	const float *query_embedding,
	int query_dim,
	size_t entry_node,
	int level,
	size_t ef,
	kvs_vector_ann_tmp_candidate_t **out_candidates,
	size_t *out_count
) {
	kvs_vector_ann_tmp_candidate_t *candidate_set = NULL;
	kvs_vector_ann_tmp_candidate_t *result_set = NULL;
	unsigned char *visited = NULL;

	size_t candidate_capacity;
	size_t candidate_count = 0;

	size_t result_capacity;
	size_t result_count = 0;

	const float *entry_embedding;
	float entry_score;

	if (out_candidates) {
		*out_candidates = NULL;
	}

	if (out_count) {
		*out_count = 0;
	}

	if (!index || !query_embedding || query_dim <= 0 ||
	    !out_candidates || !out_count ||
	    !index->nodes || index->size == 0 ||
	    entry_node >= index->size ||
	    level < 0 ||
	    (size_t)query_dim != index->dimension) {
		return KVS_VECTOR_ERR_INVALID_ARG;
	}

	if (ef == 0) {
		ef = index->ef_search;
	}

	if (ef == 0) {
		ef = KVS_VECTOR_HNSW_EF_SEARCH;
	}

	if (ef < 1) {
		ef = 1;
	}

	candidate_capacity = ef * (index->m + 1);
	if (candidate_capacity < ef + 1) {
		candidate_capacity = ef + 1;
	}

	result_capacity = ef;

	candidate_set = (kvs_vector_ann_tmp_candidate_t *)kvs_malloc(
		candidate_capacity * sizeof(kvs_vector_ann_tmp_candidate_t)
	);
	if (!candidate_set) {
		return KVS_VECTOR_ERR;
	}

	result_set = (kvs_vector_ann_tmp_candidate_t *)kvs_malloc(
		result_capacity * sizeof(kvs_vector_ann_tmp_candidate_t)
	);
	if (!result_set) {
		kvs_free(candidate_set);
		return KVS_VECTOR_ERR;
	}

	visited = (unsigned char *)kvs_malloc(index->capacity);
	if (!visited) {
		kvs_free(result_set);
		kvs_free(candidate_set);
		return KVS_VECTOR_ERR;
	}

	memset(candidate_set, 0, candidate_capacity * sizeof(kvs_vector_ann_tmp_candidate_t));
	memset(result_set, 0, result_capacity * sizeof(kvs_vector_ann_tmp_candidate_t));
	memset(visited, 0, index->capacity);

	entry_embedding = kvs_vector_ann_node_embedding(index, entry_node);
	if (!entry_embedding) {
		kvs_free(visited);
		kvs_free(result_set);
		kvs_free(candidate_set);
		return KVS_VECTOR_ERR;
	}

	entry_score = kvs_vector_dot_product(
		query_embedding,
		entry_embedding,
		index->dimension
	);

	visited[entry_node] = 1;

	if (kvs_vector_ann_candidate_append(
			candidate_set,
			candidate_capacity,
			&candidate_count,
			entry_node,
			entry_score) != KVS_VECTOR_OK) {
		kvs_free(visited);
		kvs_free(result_set);
		kvs_free(candidate_set);
		return KVS_VECTOR_ERR;
	}

	if (kvs_vector_ann_result_maybe_add(
			result_set,
			result_capacity,
			&result_count,
			entry_node,
			entry_score) != KVS_VECTOR_OK) {
		kvs_free(visited);
		kvs_free(result_set);
		kvs_free(candidate_set);
		return KVS_VECTOR_ERR;
	}

	for (;;) {
		size_t best_candidate_pos;
		kvs_vector_ann_tmp_candidate_t *candidate;
		kvs_vector_ann_node_t *node;
		float worst_result_score;
		size_t n;

		if (!kvs_vector_ann_find_best_unexpanded(
				candidate_set,
				candidate_count,
				&best_candidate_pos)) {
			break;
		}

		candidate = &candidate_set[best_candidate_pos];

		if (result_count >= result_capacity) {
			worst_result_score = kvs_vector_ann_worst_result_score(
				result_set,
				result_count
			);

			if (candidate->score < worst_result_score) {
				break;
			}
		}

		candidate->expanded = 1;

		if (candidate->node_index >= index->size) {
			continue;
		}

		node = &index->nodes[candidate->node_index];

		if (!node->active || node->level < level ||
		    !node->neighbors || !node->neighbor_counts) {
			continue;
		}

		for (n = 0; n < node->neighbor_counts[level]; ++n) {
			size_t neighbor_index = node->neighbors[level][n];
			const float *neighbor_embedding;
			float score;
			int should_add = 0;

			if (neighbor_index >= index->size || neighbor_index >= index->capacity) {
				continue;
			}

			if (visited[neighbor_index]) {
				continue;
			}

			visited[neighbor_index] = 1;

			neighbor_embedding = kvs_vector_ann_node_embedding(index, neighbor_index);
			if (!neighbor_embedding) {
				continue;
			}

			score = kvs_vector_dot_product(
				query_embedding,
				neighbor_embedding,
				index->dimension
			);

			if (result_count < result_capacity) {
				should_add = 1;
			} else {
				kvs_vector_ann_tmp_candidate_t tmp;
				size_t worst_index = 0;

				tmp.node_index = neighbor_index;
				tmp.score = score;
				tmp.expanded = 0;

				if (kvs_vector_ann_find_worst_result(
						result_set,
						result_count,
						&worst_index) &&
				    kvs_vector_ann_candidate_better(&tmp, &result_set[worst_index])) {
					should_add = 1;
				}
			}

			if (!should_add) {
				continue;
			}

			if (candidate_count < candidate_capacity) {
				kvs_vector_ann_candidate_append(
					candidate_set,
					candidate_capacity,
					&candidate_count,
					neighbor_index,
					score
				);
			}

			kvs_vector_ann_result_maybe_add(
				result_set,
				result_capacity,
				&result_count,
				neighbor_index,
				score
			);
		}
	}

	kvs_free(visited);
	kvs_free(candidate_set);

	*out_candidates = result_set;
	*out_count = result_count;

	return KVS_VECTOR_OK;
}

static void kvs_vector_ann_sort_candidates_desc(
	kvs_vector_ann_tmp_candidate_t *candidates,
	size_t count
) {
	size_t i;

	if (!candidates || count <= 1) {
		return;
	}

	for (i = 1; i < count; ++i) {
		kvs_vector_ann_tmp_candidate_t key = candidates[i];
		size_t j = i;

		while (j > 0 && candidates[j - 1].score < key.score) {
			candidates[j] = candidates[j - 1];
			j--;
		}

		candidates[j] = key;
	}
}

int kvs_vector_ann_index_add(
	kvs_vector_ann_index_t *index,
	size_t record_index,
	const float *embedding,
	int embedding_dim
) {
	kvs_vector_ann_node_t *node;
	kvs_vector_ann_tmp_candidate_t *candidates = NULL;
	size_t candidate_count = 0;
	size_t node_index;
	size_t old_size;
	size_t selected;
	size_t current;
	int node_level;
	int level;
	int ret;

	if (!index || !embedding || embedding_dim <= 0 ||
	    record_index >= g_vector_state.store.capacity) {
		return KVS_VECTOR_ERR_INVALID_ARG;
	}

	if (index->dimension == 0) {
		index->dimension = (size_t)embedding_dim;
		index->ready = 1;
	}

	if ((size_t)embedding_dim != index->dimension) {
		return KVS_VECTOR_ERR_INVALID_ARG;
	}

	ret = kvs_vector_ann_index_reserve(index, index->size + 1);
	if (ret != KVS_VECTOR_OK) {
		return ret;
	}

	old_size = index->size;
	node_index = old_size;
	node = &index->nodes[node_index];

	kvs_vector_ann_node_destroy(node);
	kvs_vector_ann_node_reset(node);

	node_level = kvs_vector_ann_random_level(
		g_vector_state.store.records[record_index].id,
		index->m
	);

	ret = kvs_vector_ann_node_init_layers(node, node_level, index->m);
	if (ret != KVS_VECTOR_OK) {
		return ret;
	}

	node->record_index = record_index;
	node->record_id = g_vector_state.store.records[record_index].id;
	node->active = 1;

	if (old_size == 0) {
		index->size = 1;
		index->entry_node = 0;
		index->has_entry = 1;
		index->max_level = node_level;
		index->ready = 1;
		index->needs_rebuild = 0;
		return KVS_VECTOR_OK;
	}

	index->size = old_size + 1;

	current = index->entry_node;

	for (level = index->max_level; level > node_level; --level) {
		current = kvs_vector_ann_greedy_search_layer(
			index,
			embedding,
			embedding_dim,
			current,
			level
		);
	}

	for (level = node_level < index->max_level ? node_level : index->max_level;
	     level >= 0;
	     --level) {
		ret = kvs_vector_ann_collect_candidates_layer(
			index,
			embedding,
			embedding_dim,
			current,
			level,
			index->ef_construction,
			&candidates,
			&candidate_count
		);
		if (ret != KVS_VECTOR_OK) {
			return ret;
		}

		kvs_vector_ann_sort_candidates_desc(candidates, candidate_count);

		selected = 0;
		for (size_t i = 0; i < candidate_count && selected < index->m; ++i) {
			size_t neighbor_node_index = candidates[i].node_index;

			if (neighbor_node_index >= old_size) {
				continue;
			}

			ret = kvs_vector_ann_neighbor_add_or_replace(
				index,
				node_index,
				neighbor_node_index,
				level
			);
			if (ret != KVS_VECTOR_OK) {
				continue;
			}

			kvs_vector_ann_neighbor_add_or_replace(
				index,
				neighbor_node_index,
				node_index,
				level
			);

			if (selected == 0) {
				current = neighbor_node_index;
			}

			selected++;
		}

		kvs_free(candidates);
		candidates = NULL;
		candidate_count = 0;

		if (level == 0) {
			break;
		}
	}

	if (node_level > index->max_level) {
		index->entry_node = node_index;
		index->max_level = node_level;
		index->has_entry = 1;
	}

	index->ready = 1;
	index->needs_rebuild = 0;

	return KVS_VECTOR_OK;
}

int kvs_vector_ann_index_search(
	kvs_vector_ann_index_t *index,
	const float *query_embedding,
	int query_dim,
	const char *scope,
	size_t scope_len,
	int topk,
	float threshold,
	kvs_vector_search_result_t *results,
	size_t results_capacity,
	size_t *results_count
) {
	kvs_vector_ann_tmp_candidate_t *candidates = NULL;
	size_t candidate_count = 0;
	kvs_vector_topk_heap_t heap;
	size_t limit;
	size_t current;
	float effective_threshold;
	int level;
	int ret;

	memset(&heap, 0, sizeof(heap));

	if (results && results_capacity > 0) {
		kvs_vector_search_results_reset(results, results_capacity);
	}

	if (results_count) {
		*results_count = 0;
	}

	if (!index || !query_embedding || query_dim <= 0 ||
	    !results || results_capacity == 0 || !results_count) {
		return KVS_VECTOR_ERR_INVALID_ARG;
	}

	if (!scope) {
		scope_len = 0;
	}

	if (topk <= 0) {
		topk = 1;
	}

	limit = (size_t)topk;
	if (limit > results_capacity) {
		limit = results_capacity;
	}

	if (!index->ready || !index->nodes || index->size == 0 || !index->has_entry) {
		return KVS_VECTOR_OK;
	}

	if ((size_t)query_dim != index->dimension) {
		return KVS_VECTOR_ERR_INVALID_ARG;
	}

	effective_threshold = threshold;
	if (effective_threshold <= 0.0f) {
		effective_threshold = -1.0f;
	}

	current = index->entry_node;

	for (level = index->max_level; level > 0; --level) {
		current = kvs_vector_ann_greedy_search_layer(
			index,
			query_embedding,
			query_dim,
			current,
			level
		);
	}

	ret = kvs_vector_ann_collect_candidates_layer(
		index,
		query_embedding,
		query_dim,
		current,
		0,
		index->ef_search,
		&candidates,
		&candidate_count
	);
	if (ret != KVS_VECTOR_OK) {
		return ret;
	}

	ret = kvs_vector_topk_heap_init(&heap, limit);
	if (ret != KVS_VECTOR_OK) {
		kvs_free(candidates);
		return ret;
	}

	for (size_t i = 0; i < candidate_count; ++i) {
		kvs_vector_ann_node_t *node;
		kvs_vector_record_t *record;

		if (candidates[i].score < effective_threshold) {
			continue;
		}

		if (candidates[i].node_index >= index->size) {
			continue;
		}

		node = &index->nodes[candidates[i].node_index];
		if (!node->active || node->record_index >= g_vector_state.store.capacity) {
			continue;
		}

		record = &g_vector_state.store.records[node->record_index];

		if (node->record_id != record->id) {
			continue;
		}

		if (!kvs_vector_record_scope_match(record, scope, scope_len)) {
			continue;
		}

		ret = kvs_vector_topk_heap_push(
			&heap,
			node->record_index,
			candidates[i].score
		);
		if (ret != KVS_VECTOR_OK) {
			kvs_vector_topk_heap_destroy(&heap);
			kvs_free(candidates);
			return ret;
		}
	}

	ret = kvs_vector_topk_heap_export_desc(
		&heap,
		results,
		results_capacity,
		results_count,
		&g_vector_state.store
	);

	kvs_vector_topk_heap_destroy(&heap);
	kvs_free(candidates);

	return ret;
}

int kvs_vector_rebuild_ann_index_from_store(void) {
	kvs_vector_ann_index_t *index = &g_vector_state.ann_index;
	kvs_vector_store_t *store = &g_vector_state.store;
	size_t dimension;
	size_t m;
	size_t ef_construction;
	size_t ef_search;
	size_t i;
	int ret;

	dimension = g_vector_state.flat_index.dimension;
	if (dimension == 0) {
		dimension = kvs_vector_embedding_provider_dimension();
	}

	m = index->m ? index->m : KVS_VECTOR_HNSW_M;
	ef_construction = index->ef_construction ? index->ef_construction : KVS_VECTOR_HNSW_EF_CONSTRUCTION;
	ef_search = index->ef_search ? index->ef_search : KVS_VECTOR_HNSW_EF_SEARCH;

	kvs_vector_ann_index_destroy(index);

	ret = kvs_vector_ann_index_init(
		index,
		dimension,
		m,
		ef_construction,
		ef_search
	);
	if (ret != KVS_VECTOR_OK) {
		return ret;
	}

	if (!store->records || store->capacity == 0 || store->active_records == 0) {
		index->ready = 1;
		index->needs_rebuild = 0;
		return KVS_VECTOR_OK;
	}

	for (i = 0; i < store->capacity; ++i) {
		kvs_vector_record_t *record = &store->records[i];

		if (record->state != KVS_VECTOR_RECORD_ACTIVE || record->deleted) {
			continue;
		}

		if (!record->embedding || record->embedding_dim <= 0) {
			continue;
		}

		ret = kvs_vector_ann_index_add(
			index,
			i,
			record->embedding,
			record->embedding_dim
		);
		if (ret != KVS_VECTOR_OK) {
			return ret;
		}
	}

	index->ready = 1;
	index->needs_rebuild = 0;

	return KVS_VECTOR_OK;
}

int kvs_vector_set_index_type_internal(kvs_vector_index_type_t index_type) {
	int ret;

	if (!g_vector_state.initialized) {
		return KVS_VECTOR_ERR_NOT_INITIALIZED;
	}

	if (index_type != KVS_VECTOR_INDEX_TYPE_FLAT &&
	    index_type != KVS_VECTOR_INDEX_TYPE_HNSW) {
		return KVS_VECTOR_ERR_INVALID_ARG;
	}

	g_vector_state.index_type = index_type;

	if (index_type == KVS_VECTOR_INDEX_TYPE_HNSW) {
		ret = kvs_vector_rebuild_ann_index_from_store();
		if (ret != KVS_VECTOR_OK) {
			return ret;
		}
	}

	return KVS_VECTOR_OK;
}

int kvs_vector_ann_index_set_params_internal(
	size_t m,
	size_t ef_construction,
	size_t ef_search
) {
	kvs_vector_ann_index_t *index;
	int ret;

	if (!g_vector_state.initialized) {
		return KVS_VECTOR_ERR_NOT_INITIALIZED;
	}

	index = &g_vector_state.ann_index;

	if (m == 0) {
		m = KVS_VECTOR_HNSW_M;
	}

	if (ef_construction == 0) {
		ef_construction = KVS_VECTOR_HNSW_EF_CONSTRUCTION;
	}

	if (ef_search == 0) {
		ef_search = KVS_VECTOR_HNSW_EF_SEARCH;
	}

	if (m < 4) {
		m = 4;
	}

	if (ef_construction < m) {
		ef_construction = m;
	}

	if (ef_search < 1) {
		ef_search = 1;
	}

	index->m = m;
	index->ef_construction = ef_construction;
	index->ef_search = ef_search;

	ret = kvs_vector_rebuild_ann_index_from_store();
	if (ret != KVS_VECTOR_OK) {
		return ret;
	}

	return KVS_VECTOR_OK;
}