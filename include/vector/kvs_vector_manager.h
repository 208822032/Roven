#pragma once

#include <stddef.h>
#include <stdint.h>

#include "embedding_c_api.h"

#include "kvs_vector.h"
#include "vector/kvs_vector_record.h"
#include "vector/kvs_vector_index_internal.h"
#include "vector/kvs_vector_core.h"
#include "vector/kvs_vector_flat.h"
#include "vector/kvs_vector_hnsw.h"
#include "vector/kvs_vector_hnsw_v2.h"
#include "vector/kvs_vector_rebuild.h"

/*
 * 向量管理层内部状态。
 *
 * KVmanager负责保存和维护业务语义：
 *   1. 向量record store。
 *   2. question/scope到record槽位的映射。
 *   3. embedding生成器和embedding运行时句柄。
 *   4. 当前索引/backend的生命周期。
 *   5. 从record store重建索引的调度状态。
 *
 * 向量计算层只负责：
 *   输入：record_id + vector
 *   输出：record_id + score
 *
 * 设计规则：
 *   1. record store是唯一真实数据源。
 *   2. flat/HNSW/SQ8/backend索引都是派生状态。
 *   3. 派生索引损坏或配置变化时，应基于record store重建。
 */

/********** embedding provider **********/

typedef enum kvs_vector_embedding_provider_type_e {
	/* 使用内置hash方式生成确定性embedding，通常用于无本地模型时的轻量路径。 */
	KVS_VECTOR_EMBEDDING_PROVIDER_HASH = 0,
	/* 使用本地C接口embedding运行时，例如ONNX Runtime封装。 */
	KVS_VECTOR_EMBEDDING_PROVIDER_LOCAL_C = 1,
} kvs_vector_embedding_provider_type_t;

/*
 * embedding生成器状态。
 *
 * 说明：
 *   1. KVmanager通过该结构选择和调用embedding provider。
 *   2. record中保存生成后的embedding，索引只消费record里的向量。
 *   3. runtime_handle只属于embedding运行时，不应暴露给vector_core。
 */
typedef struct kvs_vector_embedding_provider_s {
	/* 当前provider类型，决定使用hash生成还是本地C运行时生成。 */
	kvs_vector_embedding_provider_type_t type;
	/* provider名称，用于日志、VINFO或调试输出；字符串生命周期由provider实现保证。 */
	const char *name;
	/* provider输出的embedding维度，record和索引维度应与该值一致。 */
	size_t dimension;
	/* embedding模型版本号或模型标识，用于判断record是否需要重新生成embedding。 */
	uint32_t model_id;
	/* 本地embedding运行时句柄；hash provider通常不需要该字段。 */
	EmbeddingHandle *runtime_handle;
	/* provider是否已经初始化完成；未初始化时不应执行embedding生成。 */
	int initialized;
} kvs_vector_embedding_provider_t;

/********** global vector manager state **********/

/*
 * 向量模块全局manager状态。
 *
 * 说明：
 *   1. 该结构聚合record store、key map、旧版索引、backend句柄和embedding provider。
 *   2. 当前代码仍有旧版flat/HNSW/SQ8/adapter状态，后续会逐步迁移到vector_core。
 *   3. 不应把该结构传入vector_core热路径；core只需要record_id和vector。
 */
typedef struct kvs_vector_state_s {
	/* 向量模块是否已经初始化；未初始化时V族命令应返回错误。 */
	int initialized;
	/* record存储，保存question/answer/scope/embedding/TTL等真实数据。 */
	kvs_vector_store_t store;
	/* question/scope到record_index的映射，用于VSET更新、VGET和VDEL定位record。 */
	kvs_vector_id_map_t id_map;

	/* 当前配置的索引类型，例如flat、HNSW、Faiss HNSW或hnswlib HNSW。 */
	kvs_vector_index_type_t index_type;
	/* 当前配置的距离或相似度类型，例如IP、L2或COSINE。 */
	kvs_vector_metric_type_t metric_type;

	/* 旧版精确flat索引状态；属于可重建派生状态。 */
	kvs_vector_flat_index_t flat_index;
	/* 旧版HNSW-lite索引状态；属于可重建派生状态。 */
	kvs_vector_ann_index_t ann_index;
	/* 旧版SQ8 flat索引状态；属于可重建派生状态，不是近期主线。 */
	kvs_vector_sq8_flat_index_t sq8_flat_index;
	/* 新版core_flat索引状态；只保存record_id和向量计算缓存。 */
	kvs_flat_index_t core_flat_index;
	/* 新版core_hnsw索引状态；只保存record_id、向量和HNSW图缓存。 */
	kvs_hnsw_index_t core_hnsw_index;
	/* core_hnsw_v2索引状态；Stage 9G2 后为默认ANN backend。 */
	kvs_hnsw_v2_index_t core_hnsw_v2_index;

	/* 当前被VSEARCH使用的旧版统一索引包装对象。 */
	kvs_vector_index_t current_index;

#ifdef ENABLE_FAISS_BASELINE
	/* Faiss baseline backend句柄，仅在ENABLE_FAISS_BASELINE启用时存在。 */
	void *faiss_backend_handle;
#endif
#ifdef ENABLE_HNSWLIB
	/* hnswlib backend句柄，仅在ENABLE_HNSWLIB启用时存在。 */
	void *hnswlib_backend_handle;
#endif

	/* 最近一次索引重建的返回状态；0通常表示成功，非0表示失败。 */
	int last_rebuild_status;
	/* 最近一次索引重建耗时，单位毫秒。 */
	int64_t last_rebuild_ms;
	/* 最近一次索引重建发生时间，单位毫秒时间戳。 */
	int64_t last_rebuild_at_ms;

	/* manager全局世代号；用于标记record版本、重建或同步过程中的状态变化。 */
	uint64_t generation;

	/* 当前embedding provider状态。 */
	kvs_vector_embedding_provider_t embedding_provider;

	/* Manager-owned background rebuild control-plane skeleton. */
	kvs_vector_rebuild_context_t rebuild;
} kvs_vector_state_t;

/* 向量模块全局状态实例，由vector manager实现文件定义。 */
extern kvs_vector_state_t g_vector_state;

/********** embedding provider接口 **********/

/*
 * 初始化当前embedding生成器。
 *
 * 根据配置选择hash生成器或本地C运行时，并写入provider状态。
 */
int kvs_vector_embedding_provider_init(kvs_vector_embedding_provider_t *provider);

/*
 * 销毁当前embedding生成器。
 *
 * 释放provider持有的本地运行时句柄，并把状态恢复为未初始化。
 */
void kvs_vector_embedding_provider_destroy(kvs_vector_embedding_provider_t *provider);

/*
 * 返回当前embedding生成器名称。
 *
 * 返回字符串由provider实现持有，调用方不释放。
 */
const char *kvs_vector_embedding_provider_name(void);

/*
 * 返回当前embedding维度。
 *
 * record写入和索引构建都必须使用同一维度。
 */
size_t kvs_vector_embedding_provider_dimension(void);

/*
 * 为一段文本生成embedding。
 *
 * out_embedding由函数分配，调用方负责释放。
 */
int kvs_vector_embedding_generate(
	const char *text,
	size_t text_len,
	float **out_embedding,
	int *out_dim
);

/*
 * 返回当前embedding模型版本。
 *
 * record持久化时保存该版本，用于后续判断是否需要重新生成embedding。
 */
uint32_t kvs_vector_embedding_current_model_version(void);

/********** manager记录和加载接口 **********/

/*
 * 从record填充搜索结果。
 *
 * 结果中的question/answer/scope指向record内部字段，调用方不释放。
 */
void kvs_vector_search_result_from_record(
	kvs_vector_search_result_t *result,
	const kvs_vector_record_t *record
);

/*
 * 旧版文本分数接口。
 *
 * 当前主要为兼容旧代码保留，不属于新vector_core搜索路径。
 */
float kvs_vector_record_score(
	const kvs_vector_record_t *record,
	const char *query,
	size_t query_len
);

/*
 * 从record store重建question/scope到record槽位的映射。
 */
int kvs_vector_id_map_rebuild_from_store(void);

/*
 * 设置record字段并写入调用方提供的embedding。
 *
 * 用于已经提前生成embedding的VSET或恢复路径。
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
);

/*
 * 从持久化数据恢复一条vector record。
 *
 * 该接口保留record_id、时间戳、TTL、generation和embedding模型版本。
 */
int kvs_vector_restore_record_with_embedding(
	size_t record_index,
	uint64_t record_id,
	int state,
	int deleted,
	const char *question,
	size_t question_len,
	const char *answer,
	size_t answer_len,
	const char *scope,
	size_t scope_len,
	const char *normalized_question,
	size_t normalized_question_len,
	const float *embedding,
	int embedding_dim,
	int64_t created_at_ms,
	int64_t updated_at_ms,
	int64_t expire_at_ms,
	uint64_t generation,
	uint32_t embedding_model_version
);

/*
 * 为RDB加载清空vector内存状态。
 */
int kvs_vector_reset_for_load(void);

/*
 * RDB加载完成后重建id_map和当前索引。
 */
int kvs_vector_rebuild_after_load(void);

int kvs_vector_manager_resolve_hits(
	const kvs_vector_hit_t *hits,
	size_t hit_count,
	const char *scope,
	size_t scope_len,
	float threshold,
	kvs_vector_search_result_t *results,
	size_t results_capacity,
	size_t *results_count
);

int kvs_vector_manager_get_active_record_id_by_index(
	size_t record_index,
	uint64_t *out_record_id
);

int kvs_vector_manager_get_record_by_id(
	uint64_t record_id,
	kvs_vector_record_t **out_record
);

typedef int (*kvs_vector_manager_active_embedding_iter_fn)(
	uint64_t record_id,
	const float *embedding,
	int embedding_dim,
	void *arg
);

int kvs_vector_manager_for_each_active_embedding(
	kvs_vector_manager_active_embedding_iter_fn fn,
	void *arg
);

int kvs_vector_manager_capture_upsert_delta(
	uint64_t record_id,
	const float *embedding,
	int embedding_dim
);

int kvs_vector_manager_capture_delete_delta(
	uint64_t record_id
);

int kvs_vector_manager_rebuild_capture_begin_for_test(void);
int kvs_vector_manager_rebuild_capture_stop_for_test(void);

/********** manager控制接口 **********/

/*
 * 设置当前vector索引类型。
 *
 * 命令层VINDEX使用该接口切换后续重建和搜索路径。
 */
int kvs_vector_set_index_type_internal(kvs_vector_index_type_t index_type);

/*
 * 设置旧版HNSW-lite参数。
 *
 * m控制邻居数量，ef_construction控制建图候选宽度，ef_search控制查询候选宽度。
 */
int kvs_vector_ann_index_set_params_internal(
	size_t m,
	size_t ef_construction,
	size_t ef_search
);
