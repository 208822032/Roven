#pragma once

#include <stddef.h>

/*
 * 向量模块内部状态码。
 *
 * 说明：
 * 1. 向量模块不要直接复用 RESP 字符串作为内部错误码。
 * 2. 后续命令层 handler 可以把这些状态码转换成 RESP 响应。
 * 3. 当前第 2 步只定义骨架，绝大多数业务接口会返回 NOT_IMPLEMENTED。
 */

typedef enum kvs_vector_status_e {
	KVS_VECTOR_OK = 0,
	KVS_VECTOR_ERR = -1,
	KVS_VECTOR_ERR_INVALID_ARG = -2,
	KVS_VECTOR_ERR_NOT_FOUND = -3,
	KVS_VECTOR_ERR_NOT_IMPLEMENTED = -4,
	KVS_VECTOR_ERR_NOT_INITIALIZED = -5,
} kvs_vector_status_t;

/*
 * VGET / VSEARCH 返回的单条检索结果。
 *
 * 当前骨架阶段不分配、不持有这些字段的内存。
 * 后续真正实现时，可以选择两种策略：
 * 1. 返回模块内部只读指针，并由上层锁保护生命周期；
 * 2. 为结果字段单独分配副本，并提供释放接口。
 *
 * 当前 reset 函数只会把字段清零，不会释放任何指针。
 */
typedef struct kvs_vector_search_result_s {
    char* question;
    size_t question_len;

    char* answer;
    size_t answer_len;

    char* scope;
    size_t scope_len;

    float score;
} kvs_vector_search_result_t;

/*
 * VINFO 返回的向量模块统计信息。
 *
 * total_records:
 *   历史总记录数，后续可能包含已删除记录。
 *
 * active_records:
 *   当前有效记录数。
 *
 * deleted_records:
 *   当前 tombstone / lazy delete 记录数。
 *
 * capacity:
 *   当前记录存储容量。
 *
 * index_size:
 *   当前索引中的向量数量。
 *
 * dimension:
 *   当前 embedding 维度。
 *
 * index_ready:
 *   0 表示索引不可用或尚未构建。
 *   1 表示索引可用。
 *
 * needs_rebuild:
 *   0 表示暂不需要重建。
 *   1 表示存在 tombstone 或索引陈旧，需要重建。
 */
typedef struct kvs_vector_info_s {
    size_t total_records;
    size_t active_records;
    size_t deleted_records;
    size_t capacity;
    size_t index_size;

    size_t dimension;
    int index_ready;
    int needs_rebuild;
} kvs_vector_info_t;

/********** 向量模块接口定义 **********/

// 初始化向量模块，分配必要资源
int kvs_vector_init(void);

// 销毁向量模块，释放资源
void kvs_vector_destroy(void);

/*
 * VSET 写入接口。
 *
 * 参数：
 * question/question_len:
 *   用户问题文本。
 *
 * answer/answer_len:
 *   问题对应答案文本。
 *
 * scope/scope_len:
 *   可选业务范围。scope 可以为 NULL，scope_len 可以为 0。
 *
 * 当前骨架阶段不执行实际写入。
 */
int kvs_vector_set(const char* question, size_t question_len,
                    const char* answer, size_t answer_len,
                    const char* scope, size_t scope_len);

  /*
 * VGET 查询接口。
 *
 * 后续语义：
 * 1. 先做精确命中；
 * 2. 再做字符级候选过滤；
 * 3. 最后做向量相似度检索；
 * 4. 返回最佳答案。
 *
 * 当前骨架阶段只清空 result，并返回未实现。
 */
int kvs_vector_get(
	const char *question,
	size_t question_len,
	const char *scope,
	size_t scope_len,
	kvs_vector_search_result_t *result
);

/*
 * VSEARCH 检索接口。
 *
 * 参数：
 * query/query_len:
 *   查询文本。
 *
 * scope/scope_len:
 *   可选业务范围。
 *
 * topk:
 *   期望返回的候选数量。
 *
 * threshold:
 *   相似度阈值。
 *
 * results/results_capacity:
 *   调用方提供的结果数组和容量。
 *
 * results_count:
 *   实际返回的结果数量。
 *
 * 当前骨架阶段只清空结果数组和 results_count，并返回未实现。
 */
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
);

/*
 * VDEL 删除接口。
 *
 * 后续可以实现为 lazy delete：
 * 1. record 标记删除；
 * 2. id 映射移除；
 * 3. flat 索引延迟重建或同步删除。
 *
 * 当前骨架阶段不执行实际删除。
 */
int kvs_vector_del(
	const char *question,
	size_t question_len,
	const char *scope,
	size_t scope_len
);

/*
 * VINDEX REBUILD 接口。
 *
 * 后续用于：
 * 1. 压缩 records；
 * 2. 清理 tombstone；
 * 3. 重建 flat 向量索引。
 *
 * 当前骨架阶段不执行实际重建。
 */
int kvs_vector_index_rebuild(void);

/*
 * VINFO 统计接口。
 *
 * 当前骨架阶段返回安全的空统计信息。
 */
int kvs_vector_info(kvs_vector_info_t *info);

/*
 * 清空单条搜索结果。
 *
 * 注意：
 * 只把结构体字段置零，不释放任何指针。
 */
void kvs_vector_search_result_reset(kvs_vector_search_result_t *result);

/*
 * 批量清空搜索结果数组。
 *
 * 注意：
 * 只把结构体字段置零，不释放任何指针。
 */
void kvs_vector_search_results_reset(kvs_vector_search_result_t *results, size_t count);
