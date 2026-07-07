#define KVS_VECTOR_INTERNAL
#include "kvs_vector.h"
#include "kvs_vector_distance.h"
#include <stdlib.h>

/*
 * Manager-side vector compatibility helpers.
 *
 * This file is retained for record-store/id-map helpers, metric helpers, and
 * the small compatibility entry point that maps removed public index enum
 * values onto supported backends. Current product backends live in
 * src/vector/core/flat and src/vector/core/hnsw_v2.
 */

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

	record->expire_at_ms = KV_TTL_NO_EXPIRE;
	record->generation = g_vector_state.generation;
	record->embedding_model_version = kvs_vector_embedding_current_model_version();

	return KVS_VECTOR_OK;
}

/*
 * 使用外部已经生成好的 embedding 初始化 record。
 *
 *
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

	record->expire_at_ms = KV_TTL_NO_EXPIRE;
	record->generation = g_vector_state.generation;
	record->embedding_model_version = kvs_vector_embedding_current_model_version();

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

	return kvs_vector_record_id_map_init(&store->record_id_map);
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
	kvs_vector_record_id_map_destroy(&store->record_id_map);

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
	int ret;

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

	ret = kvs_vector_record_id_map_reserve(
		&store->record_id_map,
		kvs_vector_next_pow2(new_capacity * 2)
	);
	if (ret != KVS_VECTOR_OK) {
		return ret;
	}

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
		kvs_vector_record_id_map_destroy(&store->record_id_map);
		if (kvs_vector_record_id_map_init(&store->record_id_map) != KVS_VECTOR_OK) {
			return KVS_VECTOR_ERR;
		}
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
	kvs_vector_record_id_map_destroy(&store->record_id_map);
	if (kvs_vector_record_id_map_init(&store->record_id_map) != KVS_VECTOR_OK) {
		return KVS_VECTOR_ERR;
	}
	if (write_index > 0 &&
	    kvs_vector_record_id_map_reserve(
		    &store->record_id_map,
		    kvs_vector_next_pow2(write_index * 2)) != KVS_VECTOR_OK) {
		return KVS_VECTOR_ERR;
	}
	for (i = 0; i < write_index; ++i) {
		if (kvs_vector_record_id_map_put(
				&store->record_id_map,
				store->records[i].id,
				i) != KVS_VECTOR_OK) {
			return KVS_VECTOR_ERR;
		}
	}

	return KVS_VECTOR_OK;
}

/********** record_id -> record_index fast resolve map **********/

static uint64_t kvs_vector_record_id_hash_u64(uint64_t value) {
	value ^= value >> 33;
	value *= 0xff51afd7ed558ccdULL;
	value ^= value >> 33;
	value *= 0xc4ceb9fe1a85ec53ULL;
	value ^= value >> 33;
	return value;
}

static int kvs_vector_record_id_map_need_grow(kvs_vector_record_id_map_t *map) {
	if (!map || map->capacity == 0) {
		return 1;
	}

	return ((map->size + map->tombstones + 1) * 100 >= map->capacity * 70);
}

int kvs_vector_record_id_map_init(kvs_vector_record_id_map_t *map) {
	if (!map) {
		return KVS_VECTOR_ERR_INVALID_ARG;
	}

	memset(map, 0, sizeof(*map));
	return kvs_vector_record_id_map_reserve(
		map,
		KVS_VECTOR_INITIAL_ID_MAP_CAPACITY
	);
}

void kvs_vector_record_id_map_destroy(kvs_vector_record_id_map_t *map) {
	if (!map) {
		return;
	}

	if (map->entries) {
		kvs_free(map->entries);
	}
	memset(map, 0, sizeof(*map));
}

static int kvs_vector_record_id_map_insert_entry(
	kvs_vector_record_id_map_t *map,
	const kvs_vector_record_id_entry_t *src
) {
	size_t mask;
	size_t pos;

	if (!map || !map->entries || !src ||
	    src->state != KVS_VECTOR_RECORD_ID_ENTRY_USED) {
		return KVS_VECTOR_ERR_INVALID_ARG;
	}

	mask = map->capacity - 1;
	pos = (size_t)kvs_vector_record_id_hash_u64(src->record_id) & mask;
	for (;;) {
		kvs_vector_record_id_entry_t *entry = &map->entries[pos];

		if (entry->state != KVS_VECTOR_RECORD_ID_ENTRY_USED) {
			*entry = *src;
			map->size++;
			return KVS_VECTOR_OK;
		}
		pos = (pos + 1) & mask;
	}
}

int kvs_vector_record_id_map_reserve(
	kvs_vector_record_id_map_t *map,
	size_t new_capacity
) {
	kvs_vector_record_id_entry_t *old_entries;
	kvs_vector_record_id_entry_t *new_entries;
	size_t old_capacity;
	size_t i;
	int ret;

	if (!map) {
		return KVS_VECTOR_ERR_INVALID_ARG;
	}

	new_capacity = kvs_vector_next_pow2(new_capacity);
	if (new_capacity < KVS_VECTOR_INITIAL_ID_MAP_CAPACITY) {
		new_capacity = KVS_VECTOR_INITIAL_ID_MAP_CAPACITY;
	}
	if (new_capacity <= map->capacity) {
		return KVS_VECTOR_OK;
	}

	new_entries = (kvs_vector_record_id_entry_t *)kvs_malloc(
		new_capacity * sizeof(kvs_vector_record_id_entry_t)
	);
	if (!new_entries) {
		return KVS_VECTOR_ERR;
	}
	memset(new_entries, 0, new_capacity * sizeof(kvs_vector_record_id_entry_t));

	old_entries = map->entries;
	old_capacity = map->capacity;
	map->entries = new_entries;
	map->capacity = new_capacity;
	map->size = 0;
	map->tombstones = 0;

	if (old_entries) {
		for (i = 0; i < old_capacity; ++i) {
			if (old_entries[i].state != KVS_VECTOR_RECORD_ID_ENTRY_USED) {
				continue;
			}
			ret = kvs_vector_record_id_map_insert_entry(map, &old_entries[i]);
			if (ret != KVS_VECTOR_OK) {
				kvs_free(old_entries);
				return ret;
			}
		}
		kvs_free(old_entries);
	}

	return KVS_VECTOR_OK;
}

static int kvs_vector_record_id_map_lookup_slot(
	kvs_vector_record_id_map_t *map,
	uint64_t record_id,
	int for_insert,
	size_t *out_slot,
	int *found
) {
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
	pos = (size_t)kvs_vector_record_id_hash_u64(record_id) & mask;
	for (;;) {
		kvs_vector_record_id_entry_t *entry = &map->entries[pos];

		if (entry->state == KVS_VECTOR_RECORD_ID_ENTRY_EMPTY) {
			*out_slot = (for_insert && first_tombstone != (size_t)-1) ?
				first_tombstone : pos;
			return KVS_VECTOR_OK;
		}

		if (entry->state == KVS_VECTOR_RECORD_ID_ENTRY_TOMBSTONE) {
			if (for_insert && first_tombstone == (size_t)-1) {
				first_tombstone = pos;
			}
		} else if (entry->record_id == record_id) {
			*out_slot = pos;
			if (found) {
				*found = 1;
			}
			return KVS_VECTOR_OK;
		}

		pos = (pos + 1) & mask;
	}
}

int kvs_vector_record_id_map_get(
	kvs_vector_record_id_map_t *map,
	uint64_t record_id,
	size_t *out_record_index
) {
	size_t slot = 0;
	int found = 0;
	int ret;

	if (!map || !out_record_index) {
		return KVS_VECTOR_ERR_INVALID_ARG;
	}

	ret = kvs_vector_record_id_map_lookup_slot(
		map,
		record_id,
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

int kvs_vector_record_id_map_put(
	kvs_vector_record_id_map_t *map,
	uint64_t record_id,
	size_t record_index
) {
	size_t slot = 0;
	int found = 0;
	int ret;

	if (!map) {
		return KVS_VECTOR_ERR_INVALID_ARG;
	}

	if (kvs_vector_record_id_map_need_grow(map)) {
		size_t new_capacity = map->capacity ?
			map->capacity * 2 : KVS_VECTOR_INITIAL_ID_MAP_CAPACITY;
		ret = kvs_vector_record_id_map_reserve(map, new_capacity);
		if (ret != KVS_VECTOR_OK) {
			return ret;
		}
	}

	ret = kvs_vector_record_id_map_lookup_slot(
		map,
		record_id,
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

	if (map->entries[slot].state == KVS_VECTOR_RECORD_ID_ENTRY_TOMBSTONE &&
	    map->tombstones > 0) {
		map->tombstones--;
	}

	map->entries[slot].record_id = record_id;
	map->entries[slot].record_index = record_index;
	map->entries[slot].state = KVS_VECTOR_RECORD_ID_ENTRY_USED;
	map->size++;

	return KVS_VECTOR_OK;
}

int kvs_vector_record_id_map_remove(
	kvs_vector_record_id_map_t *map,
	uint64_t record_id
) {
	size_t slot = 0;
	int found = 0;
	int ret;

	if (!map) {
		return KVS_VECTOR_ERR_INVALID_ARG;
	}

	ret = kvs_vector_record_id_map_lookup_slot(
		map,
		record_id,
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

	map->entries[slot].state = KVS_VECTOR_RECORD_ID_ENTRY_TOMBSTONE;
	map->entries[slot].record_id = 0;
	map->entries[slot].record_index = 0;
	if (map->size > 0) {
		map->size--;
	}
	map->tombstones++;

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
	kvs_vector_record_id_map_destroy(&store->record_id_map);

	ret = kvs_vector_id_map_init(map);
	if (ret != KVS_VECTOR_OK) {
		return ret;
	}
	ret = kvs_vector_record_id_map_init(&store->record_id_map);
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
	ret = kvs_vector_record_id_map_reserve(
		&store->record_id_map,
		target_capacity
	);
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
		ret = kvs_vector_record_id_map_put(
			&store->record_id_map,
			record->id,
			i
		);
		if (ret != KVS_VECTOR_OK) {
			return ret;
		}
	}

	return KVS_VECTOR_OK;
}

float kvs_vector_metric_score(
    const float *lhs,
    const float *rhs,
    size_t dim,
    kvs_vector_metric_type_t metric
) {
    return kvs_vector_distance_metric_score(lhs, rhs, dim, metric);
}

const char *kvs_vector_metric_name(kvs_vector_metric_type_t metric) {
    switch (metric) {
        case KVS_VECTOR_METRIC_L2:
            return "l2";

        case KVS_VECTOR_METRIC_COSINE:
            return "cosine";

        case KVS_VECTOR_METRIC_IP:
        default:
            return "ip";
    }
}

kvs_vector_metric_type_t kvs_vector_metric_from_config(void) {
    if (strcasecmp(g_cfg.vector_metric, "l2") == 0) {
        return KVS_VECTOR_METRIC_L2;
    }

    if (strcasecmp(g_cfg.vector_metric, "cosine") == 0) {
        return KVS_VECTOR_METRIC_COSINE;
    }

    return KVS_VECTOR_METRIC_IP;
}
int kvs_vector_set_index_type_internal(kvs_vector_index_type_t index_type) {
	if (!g_vector_state.initialized) {
		return KVS_VECTOR_ERR_NOT_INITIALIZED;
	}

	switch (index_type) {
	case KVS_VECTOR_INDEX_CORE_HNSW_V2:
	case KVS_VECTOR_INDEX_CORE_FLAT:
		g_vector_state.index_type = index_type;
		break;
	case KVS_VECTOR_INDEX_TYPE_FLAT:
	case KVS_VECTOR_INDEX_TYPE_HNSW:
	case KVS_VECTOR_INDEX_TYPE_SQ8_FLAT:
	case KVS_VECTOR_INDEX_CORE_HNSW:
	case KVS_VECTOR_INDEX_HNSWLIB_HNSW:
	case KVS_VECTOR_INDEX_FAISS_HNSW:
	case KVS_VECTOR_INDEX_FAISS_FLAT:
		g_vector_state.index_type = KVS_VECTOR_INDEX_CORE_FLAT;
		break;
	default:
		return KVS_VECTOR_ERR_INVALID_ARG;
	}

	return kvs_vector_index_bind_current();
}
