#include "kvs_api.h"
#include <string.h>

/*
 * 向量模块骨架实现。
 *
 * 当前第 2 步只做模块边界和 API 骨架。
 *
 * 本文件当前故意不实现：
 * 1. record / store / index 数据结构；
 * 2. question 规范化；
 * 3. embedding 生成；
 * 4. question_hash / id 映射；
 * 5. Flat 向量索引；
 * 6. topK 堆；
 * 7. VSET / VGET / VSEARCH / VDEL / VINDEX 的真实业务逻辑；
 * 8. 持久化。
 *
 * 以上内容放到后续步骤逐步实现。
 */

/*
 * 向量模块初始化标记。
 *
 * 当前先使用一个最小全局状态，后续第 3 步可以扩展为：
 * 1. 全局 vector store；
 * 2. flat index；
 * 3. id 映射；
 * 4. 统计信息。
 */
static int g_vector_initialized = 0;

//初试record容量
#define KVS_VECTOR_INITIAL_RECORD_CAPACITY 16
//初始id map容量
#define KVS_VECTOR_INITIAL_ID_MAP_CAPACITY 32
#define KVS_VECTOR_DEFAULT_DIMENSION 0

/*
 * record 状态。
 *
 * EMPTY:
 *   空槽位。
 *
 * ACTIVE:
 *   有效记录。
 *
 * DELETED:
 *   tombstone / lazy delete 记录。
 */
typedef enum kvs_vector_record_state_e {
	KVS_VECTOR_RECORD_EMPTY = 0,
	KVS_VECTOR_RECORD_ACTIVE = 1,
	KVS_VECTOR_RECORD_DELETED = 2,
} kvs_vector_record_state_t;

/*
 * id map 槽位状态。
 *
 * EMPTY:
 *   从未使用。
 *
 * USED:
 *   当前有效。
 *
 * TOMBSTONE:
 *   曾经使用过，后来删除。
 */
typedef enum kvs_vector_map_entry_state_e {
	KVS_VECTOR_MAP_ENTRY_EMPTY = 0,
	KVS_VECTOR_MAP_ENTRY_USED = 1,
	KVS_VECTOR_MAP_ENTRY_TOMBSTONE = 2,
} kvs_vector_map_entry_state_t;

/*
 * 单条问答记录。
 *
 * 注意：
 * 1. question / answer / scope / normalized_question 都由 vector 模块复制并持有；
 * 2. embedding 后续由 vector 模块分配并持有；
 * 3. 当前第 3 步只定义结构和释放逻辑，不写入真实记录。
 */
typedef struct kvs_vector_record_s {
	uint64_t id;
	uint32_t question_hash;

	char *question;
	size_t question_len;

	char *answer;
	size_t answer_len;

	char *normalized_question;
	size_t normalized_question_len;

	char *scope;
	size_t scope_len;

	float *embedding;
	int embedding_dim;

	int state;
	int deleted;

	int64_t created_at_ms;
	int64_t updated_at_ms;
} kvs_vector_record_t;

/*
 * record 存储区。
 *
 * total_records:
 *   历史写入过的记录数量。后续可能包含已删除记录。
 *
 * active_records:
 *   当前有效记录数量。
 *
 * deleted_records:
 *   当前被 lazy delete 的记录数量。
 *
 * next_id:
 *   下一个 record id。
 */
typedef struct kvs_vector_store_s {
	kvs_vector_record_t *records;
	size_t capacity;
	size_t total_records;
	size_t active_records;
	size_t deleted_records;
	uint64_t next_id;
} kvs_vector_store_t;

/*
 * question + scope 到 record_index 的映射项。
 *
 * MVP 不复用 array/hash/rbtree/skiptable 的内部结构。
 * 后续会在本文件内部实现开放寻址 hash 表。
 *
 * 当前第 3 步只完成内存管理和结构定义。
 */
typedef struct kvs_vector_id_entry_s {
	uint32_t hash;
	size_t record_index;
	uint8_t state;
} kvs_vector_id_entry_t;

/*
 * id map。
 *
 * size:
 *   USED 槽位数量。
 *
 * tombstones:
 *   TOMBSTONE 槽位数量。
 */
typedef struct kvs_vector_id_map_s {
	kvs_vector_id_entry_t *entries;
	size_t capacity;
	size_t size;
	size_t tombstones;
} kvs_vector_id_map_t;

/*
 * Flat 向量索引。
 *
 * vectors:
 *   连续 float 数组，布局为：
 *   [vec0_dim0, vec0_dim1, ..., vec1_dim0, vec1_dim1, ...]
 *
 * record_indices:
 *   vectors 中每个向量对应的 record 下标。
 *
 * 当前第 3 步不写入向量，不做搜索。
 */
typedef struct kvs_vector_flat_index_s {
	float *vectors;
	size_t *record_indices;
	size_t size;
	size_t capacity;
	size_t dimension;
	int ready;
	int needs_rebuild;
} kvs_vector_flat_index_t;

/*
 * 搜索候选。
 *
 * 当前只预留给后续 topK 使用。
 */
typedef struct kvs_vector_candidate_s {
	size_t record_index;
	float score;
} kvs_vector_candidate_t;

/*
 * topK 最小堆结构。
 *
 * 当前第 3 步只定义结构，不实现堆算法。
 */
typedef struct kvs_vector_topk_heap_s {
	kvs_vector_candidate_t *items;
	size_t size;
	size_t capacity;
} kvs_vector_topk_heap_t;

/*
 * 查询上下文。
 *
 * 当前第 3 步只定义结构，不进入查询流程。
 */
typedef struct kvs_vector_query_ctx_s {
	const char *query;
	size_t query_len;
	const char *scope;
	size_t scope_len;
	int topk;
	float threshold;
} kvs_vector_query_ctx_t;

/*
 * 向量模块全局状态。
 *
 * 目前 KVLite 里的存储模块大多采用全局单例风格，
 * vector 模块也先保持一致。
 */
typedef struct kvs_vector_state_s {
	int initialized;
	kvs_vector_store_t store;
	kvs_vector_id_map_t id_map;
	kvs_vector_flat_index_t flat_index;
} kvs_vector_state_t;

static kvs_vector_state_t g_vector_state;

/*
 * 计算不小于 value 的 2 的幂。
 *
 * id map 使用开放寻址，容量使用 2 的幂可以简化取模。
 */
static size_t kvs_vector_next_pow2(size_t value) {
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
 *
 * 当前第 3 步暂时未被业务路径调用，后续 VSET / VGET 会使用。
 */
static uint32_t kvs_vector_hash_key(const char *data, size_t len) {
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
 * 复制一段字符串，并额外补 '\0'。
 *
 * 用途：
 * 1. question；
 * 2. answer；
 * 3. scope；
 * 4. normalized_question。
 *
 * 当前第 3 步还没有真实写入流程，但先提供该 helper，
 * 后续 VSET 可以直接复用。
 */
static char *kvs_vector_dup_string(const char *src, size_t src_len, size_t *out_len) {
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
static int kvs_vector_text_equal(const char *lhs, size_t lhs_len, const char *rhs, size_t rhs_len) {
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
static int kvs_vector_scope_equal(const char *lhs, size_t lhs_len, const char *rhs, size_t rhs_len){
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
 * 判断一条 record 是否匹配 question + scope。
 *
 * 第 5 步只做精确命中：
 * 1. record 必须是 ACTIVE；
 * 2. record 不能被删除；
 * 3. question 长度和内容必须完全一致；
 * 4. scope 长度和内容必须完全一致；
 * 5. NULL scope 和空 scope 视为等价。
 */
static int kvs_vector_record_match(
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
 * 在线性 records 数组中查找 active record。
 *
 * 当前第 5 步先不使用 id_map。
 * 后续数据量变大后，再把这里替换成 hash lookup。
 */
static kvs_vector_record_t *kvs_vector_find_record(
	const char *question,
	size_t question_len,
	const char *scope,
	size_t scope_len
) {
	kvs_vector_store_t *store = &g_vector_state.store;
	size_t i;

	if (!store->records || store->capacity == 0) {
		return NULL;
	}

	for (i = 0; i < store->capacity; ++i) {
		if (kvs_vector_record_match(&store->records[i], question, question_len, scope, scope_len)) {
			return &store->records[i];
		}
	}

	return NULL;
}

/*
 * 查找可写入的空槽。
 *
 * 当前第 5 步还没有 VDEL，但这里兼容后续 DELETED 槽位复用。
 */
static kvs_vector_record_t *kvs_vector_find_free_slot(void) {
	kvs_vector_store_t *store = &g_vector_state.store;
	size_t i;

	if (!store->records || store->capacity == 0) {
		return NULL;
	}

	for (i = 0; i < store->capacity; ++i) {
		if (store->records[i].state == KVS_VECTOR_RECORD_EMPTY ||
			store->records[i].state == KVS_VECTOR_RECORD_DELETED) {
			return &store->records[i];
		}
	}

	return NULL;
}



/*
 * 清空 record 字段。
 *
 * 注意：
 * 这个函数不释放 record 内部指针。
 * 如果 record 已经持有内存，必须先调用 kvs_vector_record_destroy。
 */
static void kvs_vector_record_reset(kvs_vector_record_t *record) {
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
static void kvs_vector_record_destroy(kvs_vector_record_t *record) {
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
static int kvs_vector_record_set_fields(
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
	size_t copied_len = 0;
	int64_t now_ms;

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
	 * 第 5 步 normalized_question 暂时直接复制 question。
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

	record->embedding = NULL;
	record->embedding_dim = 0;

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
static int kvs_vector_record_update_answer(
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
 * 从 record 填充搜索结果。
 *
 * result 只引用 record 内部字段，不拥有内存。
 * 调用方不能释放 result->question / answer / scope。
 */
static void kvs_vector_search_result_from_record(
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
 * 将 record 标记为删除。
 *
 * 注意：
 * 这里采用 lazy delete：
 * 1. 不立即释放 question / answer / scope；
 * 2. 只把状态改成 DELETED；
 * 3. 后续由 VINDEX REBUILD 统一清理 tombstone；
 * 4. VGET / VSEARCH 只命中 ACTIVE 且 deleted == 0 的记录。
 */
static int kvs_vector_record_mark_deleted(kvs_vector_record_t *record) {
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
 * 判断 record 是否属于指定 scope。
 *
 * VSEARCH 如果传入 scope，只搜索该 scope 下的记录；
 * 如果 scope 为空，只搜索空 scope 记录。
 */
static int kvs_vector_record_scope_match(
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
 * 简单字符重叠计数。
 *
 * 这是 VSEARCH MVP 的字符级相似度基础，不是最终 embedding 检索。
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
 * 字符级相似度打分。
 *
 * MVP 规则：
 * 1. exact match：1.0
 * 2. query 被 question 包含，或 question 被 query 包含：0.85
 * 3. 字符重叠：0.1 ~ 0.8
 *
 * 后续接 embedding 后，可以替换这里为 dot product / cosine / L2。
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
 * 计算一条 record 对 query 的搜索分数。
 *
 * 第 7 步优先使用 question 和 normalized_question。
 * answer 暂不参与打分，避免答案文本过长导致误召回。
 */
static float kvs_vector_record_score(
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
 * 把候选插入 topK 结果数组。
 *
 * 当前 results 数组始终保持按 score 降序排列。
 * capacity 通常等于 topk。
 */
static void kvs_vector_topk_insert(
	kvs_vector_search_result_t *results,
	size_t *results_count,
	size_t results_capacity,
	const kvs_vector_record_t *record,
	float score
) {
	size_t count;
	size_t pos;
	size_t i;

	if (!results || !results_count || !record || results_capacity == 0) {
		return;
	}

	count = *results_count;

	/*
	 * 如果结果数组已满，并且新分数不超过最后一名，直接丢弃。
	 */
	if (count >= results_capacity && score <= results[count - 1].score) {
		return;
	}

	if (count < results_capacity) {
		count++;
	}

	pos = count - 1;
	while (pos > 0 && results[pos - 1].score < score) {
		if (pos < results_capacity) {
			results[pos] = results[pos - 1];
		}
		pos--;
	}

	kvs_vector_search_result_from_record(&results[pos], record);
	results[pos].score = score;

	/*
	 * 如果中间移动留下了尾部旧数据，确保 count 之后不暴露。
	 */
	for (i = count; i < results_capacity; ++i) {
		kvs_vector_search_result_reset(&results[i]);
	}

	*results_count = count;
}


/*
 * 初始化 store。
 */
static int kvs_vector_store_init(kvs_vector_store_t *store) {
	if (!store) {
		return KVS_VECTOR_ERR_INVALID_ARG;
	}

	memset(store, 0, sizeof(*store));
	store->next_id = 1;

	return KVS_VECTOR_OK;
}

/*
 * 销毁 store。
 *
 * 会释放所有 record 内部资源和 records 数组。
 */
static void kvs_vector_store_destroy(kvs_vector_store_t *store) {
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

	memset(store, 0, sizeof(*store));
	store->next_id = 1;
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
static int kvs_vector_store_reserve(kvs_vector_store_t *store, size_t new_capacity) {
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
static int kvs_vector_store_ensure_capacity(kvs_vector_store_t *store, size_t min_capacity) {
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
 * 语义：
 * 1. ACTIVE record 会被移动到新数组前部；
 * 2. DELETED record 会释放内部字符串和 embedding；
 * 3. EMPTY record 直接丢弃；
 * 4. capacity 保持不变，避免频繁缩容扩容；
 * 5. active_records 重新计算；
 * 6. deleted_records 清零；
 * 7. total_records 保持历史创建数量，不在 rebuild 时回退。
 *
 * 注意：
 * ACTIVE record 采用浅拷贝移动。
 * 移动后不能对旧 active record 调用 destroy，否则会 double free。
 */
static int kvs_vector_store_compact(kvs_vector_store_t *store) {
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
			 * 字符串指针所有权转移到 new_records。
			 */
			new_records[write_index] = *record;
			write_index++;
		} else {
			/*
			 * DELETED record 在 rebuild 时释放内部资源。
			 * EMPTY record destroy 也是安全的。
			 */
			kvs_vector_record_destroy(record);
		}
	}

	kvs_free(old_records);

	store->records = new_records;
	store->capacity = old_capacity;
	store->active_records = write_index;
	store->deleted_records = 0;
	store->next_id = next_id;

	/*
	 * total_records 当前语义保持为“历史创建过的记录数”。
	 * 因此 rebuild 不回退 total_records。
	 */

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
static int kvs_vector_id_map_init(kvs_vector_id_map_t *map) {
	if (!map) {
		return KVS_VECTOR_ERR_INVALID_ARG;
	}

	memset(map, 0, sizeof(*map));

	return KVS_VECTOR_OK;
}

/*
 * 销毁 id map。
 */
static void kvs_vector_id_map_destroy(kvs_vector_id_map_t *map) {
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
 * 当前第 3 步主要用于初始化空表。
 * 后续 VSET/VDEL 实现时会复用该逻辑。
 */
static int kvs_vector_id_map_reserve(kvs_vector_id_map_t *map, size_t new_capacity) {
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
 * 初始化 flat index。
 *
 * 当前不分配 vectors，因为 dimension = 0。
 * 后续有 embedding 维度后，再按 capacity * dimension 分配。
 */
static int kvs_vector_flat_index_init(kvs_vector_flat_index_t *index, size_t dimension) {
	if (!index) {
		return KVS_VECTOR_ERR_INVALID_ARG;
	}

	memset(index, 0, sizeof(*index));

	index->vectors = NULL;
	index->record_indices = NULL;
	index->size = 0;
	index->capacity = 0;
	index->dimension = dimension;
	index->ready = 0;
	index->needs_rebuild = 0;

	return KVS_VECTOR_OK;
}

/*
 * 释放 flat index。
 */
static void kvs_vector_flat_index_destroy(kvs_vector_flat_index_t *index) {
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

	memset(index, 0, sizeof(*index));
}

/*
 * 重置 flat index 状态。
 *
 * 当前实现会释放已分配内存。
 * 后续如需优化，可以改成保留 capacity。
 */
static void kvs_vector_flat_index_reset(kvs_vector_flat_index_t *index) {
	if (!index) {
		return;
	}

	kvs_vector_flat_index_destroy(index);
	kvs_vector_flat_index_init(index, KVS_VECTOR_DEFAULT_DIMENSION);
}

/*
 * 标记 flat index 已陈旧。
 *
 * 当前还没有真正的 embedding / flat index。
 * 但 VDEL 会改变可检索记录集合，所以先把 needs_rebuild 置为 1。
 */
static void kvs_vector_flat_index_mark_stale(kvs_vector_flat_index_t *index) {
	if (!index) {
		return;
	}

	index->ready = 0;
	index->needs_rebuild = 1;
}

/*
 * 第 6 步的 VINDEX REBUILD 占位实现。
 *
 * 当前还没有真正的向量索引，所以 rebuild 只做：
 * 1. 清理 flat index 内部状态；
 * 2. index_size 归零；
 * 3. index_ready 保持 0；
 * 4. needs_rebuild 清零。
 */
static int kvs_vector_rebuild_flat_index_placeholder(void) {
	kvs_vector_flat_index_reset(&g_vector_state.flat_index);
	g_vector_state.flat_index.ready = 0;
	g_vector_state.flat_index.needs_rebuild = 0;
	g_vector_state.flat_index.size = 0;
	g_vector_state.flat_index.dimension = KVS_VECTOR_DEFAULT_DIMENSION;

	return KVS_VECTOR_OK;
}

/*
 * 从内部状态生成 VINFO。
 *
 * 注意：
 * 这里完全匹配你当前头文件中的 kvs_vector_info_t：
 *
 * typedef struct kvs_vector_info_s {
 *     size_t total_records;
 *     size_t active_records;
 *     size_t deleted_records;
 *     size_t capacity;
 *     size_t index_size;
 *     size_t dimension;
 *     int index_ready;
 *     int needs_rebuild;
 * } kvs_vector_info_t;
 */
static void kvs_vector_info_from_state(
	kvs_vector_info_t *info,
	const kvs_vector_state_t *state
) {
	if (!info) {
		return;
	}

	memset(info, 0, sizeof(*info));

	if (!state || !state->initialized) {
		info->total_records = 0;
		info->active_records = 0;
		info->deleted_records = 0;
		info->capacity = 0;
		info->index_size = 0;
		info->dimension = 0;
		info->index_ready = 0;
		info->needs_rebuild = 0;
		return;
	}

	info->total_records = state->store.total_records;
	info->active_records = state->store.active_records;
	info->deleted_records = state->store.deleted_records;
	info->capacity = state->store.capacity;

	info->index_size = state->flat_index.size;
	info->dimension = state->flat_index.dimension;
	info->index_ready = state->flat_index.ready;
	info->needs_rebuild = state->flat_index.needs_rebuild;
}

/*
 * 统一检查 vector 模块是否已经初始化。
 */
static int kvs_vector_validate_initialized(void) {
	if (!g_vector_state.initialized) {
		return KVS_VECTOR_ERR_NOT_INITIALIZED;
	}

	return KVS_VECTOR_OK;
}



// /*
//  * 重置 VINFO 统计结构。
//  *
//  * 当前返回空统计，保证 VINFO 后续接入时即使没有真实索引，
//  * 也不会读到未初始化字段。
//  */
// static void kvs_vector_info_reset(kvs_vector_info_t *info) {
//     if (!info) return;
//     memset(info, 0, sizeof(kvs_vector_info_t));

//     info->total_records = 0;
// 	info->active_records = 0;
// 	info->deleted_records = 0;
// 	info->capacity = 0;
// 	info->index_size = 0;
// 	info->dimension = 0;
// 	info->index_ready = 0;
// 	info->needs_rebuild = 0;
// }

//初始化全局状态
int kvs_vector_init(void) {
	int ret;

	if (g_vector_state.initialized) {
		return KVS_VECTOR_OK;
	}

	memset(&g_vector_state, 0, sizeof(g_vector_state));

	ret = kvs_vector_store_init(&g_vector_state.store);
	if (ret != KVS_VECTOR_OK) {
		goto fail;
	}

	ret = kvs_vector_store_reserve(
		&g_vector_state.store,
		KVS_VECTOR_INITIAL_RECORD_CAPACITY
	);
	if (ret != KVS_VECTOR_OK) {
		goto fail;
	}

	ret = kvs_vector_id_map_init(&g_vector_state.id_map);
	if (ret != KVS_VECTOR_OK) {
		goto fail;
	}

	ret = kvs_vector_id_map_reserve(
		&g_vector_state.id_map,
		KVS_VECTOR_INITIAL_ID_MAP_CAPACITY
	);
	if (ret != KVS_VECTOR_OK) {
		goto fail;
	}

	ret = kvs_vector_flat_index_init(
		&g_vector_state.flat_index,
		KVS_VECTOR_DEFAULT_DIMENSION
	);
	if (ret != KVS_VECTOR_OK) {
		goto fail;
	}

	g_vector_state.initialized = 1;

	return KVS_VECTOR_OK;

fail:
	kvs_vector_flat_index_destroy(&g_vector_state.flat_index);
	kvs_vector_id_map_destroy(&g_vector_state.id_map);
	kvs_vector_store_destroy(&g_vector_state.store);
	memset(&g_vector_state, 0, sizeof(g_vector_state));

	return ret;
}

//销毁全局状态
void kvs_vector_destroy(void) {
	if (!g_vector_state.initialized) {
		return;
	}

	kvs_vector_flat_index_destroy(&g_vector_state.flat_index);
	kvs_vector_id_map_destroy(&g_vector_state.id_map);
	kvs_vector_store_destroy(&g_vector_state.store);

	memset(&g_vector_state, 0, sizeof(g_vector_state));
}

//写入接口
//写入接口
int kvs_vector_set(const char* question, size_t question_len,
                    const char* answer, size_t answer_len,
                    const char* scope, size_t scope_len) {
	kvs_vector_record_t *record;
	kvs_vector_record_t *free_slot;
	kvs_vector_store_t *store;
	int ret;

	ret = kvs_vector_validate_initialized();
	if (ret != KVS_VECTOR_OK) {
		return ret;
	}

	if (!question || question_len == 0 || !answer) {
		return KVS_VECTOR_ERR_INVALID_ARG;
	}

	if (!scope) {
		scope_len = 0;
	}

	/*
	 * 如果同 question + scope 已存在，则覆盖 answer。
	 */
	record = kvs_vector_find_record(question, question_len, scope, scope_len);
	if (record) {
		return kvs_vector_record_update_answer(record, answer, answer_len);
	}

	store = &g_vector_state.store;

	free_slot = kvs_vector_find_free_slot();
	if (!free_slot) {
		ret = kvs_vector_store_ensure_capacity(store, store->capacity + 1);
		if (ret != KVS_VECTOR_OK) {
			return ret;
		}

		free_slot = kvs_vector_find_free_slot();
		if (!free_slot) {
			return KVS_VECTOR_ERR;
		}
	}

	if (free_slot->state == KVS_VECTOR_RECORD_DELETED && store->deleted_records > 0) {
		store->deleted_records--;
	}

	ret = kvs_vector_record_set_fields(
		free_slot,
		question,
		question_len,
		answer,
		answer_len,
		scope,
		scope_len
	);
	if (ret != KVS_VECTOR_OK) {
		return ret;
	}

	store->total_records++;
	store->active_records++;

	return KVS_VECTOR_OK;
}

//查询接口
//查询接口
int kvs_vector_get(
	const char *question,
	size_t question_len,
	const char *scope,
	size_t scope_len,
	kvs_vector_search_result_t *result
) {
	kvs_vector_record_t *record;
	int ret;

	if (result) {
		kvs_vector_search_result_reset(result);
	}

	ret = kvs_vector_validate_initialized();
	if (ret != KVS_VECTOR_OK) {
		return ret;
	}

	if (!question || question_len == 0) {
		return KVS_VECTOR_ERR_INVALID_ARG;
	}

	if (!scope) {
		scope_len = 0;
	}

	record = kvs_vector_find_record(question, question_len, scope, scope_len);
	if (!record) {
		return KVS_VECTOR_ERR_NOT_FOUND;
	}

	if (result) {
		kvs_vector_search_result_from_record(result, record);
	}

	return KVS_VECTOR_OK;
}

//检索接口
//检索接口
int kvs_vector_search(
	const char *query,
	size_t query_len,
	const char *scope,
	size_t scope_len,
	int topk,
	float threshold,
	kvs_vector_search_result_t *results,
	size_t results_capacity,
	size_t *results_count
) {
	kvs_vector_store_t *store;
	size_t i;
	size_t limit;
	int ret;
	float effective_threshold;

	if (results && results_capacity > 0) {
		kvs_vector_search_results_reset(results, results_capacity);
	}

	if (results_count) {
		*results_count = 0;
	}

	ret = kvs_vector_validate_initialized();
	if (ret != KVS_VECTOR_OK) {
		return ret;
	}

	if (!query || query_len == 0 || !results || results_capacity == 0 || !results_count) {
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

	/*
	 * 默认阈值。
	 *
	 * 如果调用方没有显式传 threshold，命令层会传 0.0。
	 * 直接使用 0 会导致任意字符重叠都被召回，例如：
	 * not_existing_query 也可能命中 search_key。
	 *
	 * MVP 阶段先使用 0.5 作为默认召回门槛：
	 * 1. exact match = 1.0，可以返回；
	 * 2. substring match = 0.85，可以返回；
	 * 3. 弱字符重叠通常低于 0.5，会被过滤。
	 */
	effective_threshold = threshold;
	if (effective_threshold <= 0.0f) {
		effective_threshold = 0.5f;
	}

	store = &g_vector_state.store;
	if (!store->records || store->capacity == 0 || store->active_records == 0) {
		return KVS_VECTOR_OK;
	}

	for (i = 0; i < store->capacity; ++i) {
		kvs_vector_record_t *record = &store->records[i];
		float score;

		if (!kvs_vector_record_scope_match(record, scope, scope_len)) {
			continue;
		}

		score = kvs_vector_record_score(record, query, query_len);
		if (score <= 0.0f) {
			continue;
		}

		if (score < effective_threshold) {
			continue;
		}

		kvs_vector_topk_insert(
			results,
			results_count,
			limit,
			record,
			score
		);
	}

	return KVS_VECTOR_OK;
}

//删除接口
int kvs_vector_del(
	const char *question,
	size_t question_len,
	const char *scope,
	size_t scope_len
) {
	kvs_vector_record_t *record;
	kvs_vector_store_t *store;
	int ret;

	ret = kvs_vector_validate_initialized();
	if (ret != KVS_VECTOR_OK) {
		return ret;
	}

	if (!question || question_len == 0) {
		return KVS_VECTOR_ERR_INVALID_ARG;
	}

	if (!scope) {
		scope_len = 0;
	}

	record = kvs_vector_find_record(question, question_len, scope, scope_len);
	if (!record) {
		return KVS_VECTOR_ERR_NOT_FOUND;
	}

	ret = kvs_vector_record_mark_deleted(record);
	if (ret != KVS_VECTOR_OK) {
		return ret;
	}

	store = &g_vector_state.store;

	if (store->active_records > 0) {
		store->active_records--;
	}

	store->deleted_records++;

	kvs_vector_flat_index_mark_stale(&g_vector_state.flat_index);

	return KVS_VECTOR_OK;
}

//重建接口
int kvs_vector_index_rebuild(void) {
	int ret;

	ret = kvs_vector_validate_initialized();
	if (ret != KVS_VECTOR_OK) {
		return ret;
	}

	ret = kvs_vector_store_compact(&g_vector_state.store);
	if (ret != KVS_VECTOR_OK) {
		return ret;
	}

	ret = kvs_vector_rebuild_flat_index_placeholder();
	if (ret != KVS_VECTOR_OK) {
		return ret;
	}

	return KVS_VECTOR_OK;
}

/*
 * VINFO 统计接口。
 *
 * 当前已经接入内部真实状态。
 * 即使没有任何数据，也会返回初始化后的 store/index 统计。
 */
int kvs_vector_info(kvs_vector_info_t *info) {
	if (!info) {
		return KVS_VECTOR_ERR_INVALID_ARG;
	}

	kvs_vector_info_from_state(info, &g_vector_state);

	return KVS_VECTOR_OK;
}


/*
 * 清空单条搜索结果。
 *
 * 注意：
 * 当前不释放 question / answer / scope 指针。
 * result 字段所有权还没有对外定义。
 */
void kvs_vector_search_result_reset(kvs_vector_search_result_t *result) {
	if (!result) {
		return;
	}

	memset(result, 0, sizeof(*result));
}

//清空批量搜索结果
void kvs_vector_search_results_reset(
	kvs_vector_search_result_t *results,
	size_t count
) {
	size_t i;

	if (!results) {
		return;
	}

	for (i = 0; i < count; ++i) {
		kvs_vector_search_result_reset(&results[i]);
	}
}