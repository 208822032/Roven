#pragma once

#include <stddef.h>
#include <stdint.h>

#include "kvs_vector.h"
#include "vector/kvs_vector_record.h"

/*
 * 旧版向量索引内部定义。
 *
 * 这个头文件保留当前已有的精确遍历索引、SQ8量化索引、旧版HNSW-lite和第三方索引适配入口结构。
 * 它用于边界重构过渡期，避免一次性改动所有旧代码。
 *
 * 新增代码优先使用kvs_vector_core.h中的接口：
 *   输入：record_id + vector
 *   输出：record_id + score
 *
 * 注意：
 *   1. 本文件中的旧结构仍大量使用record_index。
 *   2. record_index只是record数组下标，不是稳定外部id。
 *   3. 旧search接口仍直接返回question/answer/scope等manager结果。
 *   4. 后续应逐步迁移到只返回record_id + score的核心检索接口。
 */

/********** 精确遍历索引 **********/

/*
 * 旧版精确遍历索引。
 *
 * 说明：
 *   1. vectors保存连续float向量数组。
 *   2. record_indices用于回到record store查完整记录。
 *   3. record_ids已经存在，但旧搜索路径仍主要依赖record_index。
 */
typedef struct kvs_vector_flat_index_s {
	/* 连续向量数组，布局为capacity * dimension。 */
	float *vectors;
	/* 每个向量对应的record数组下标；这是旧路径使用的manager内部slot。 */
	size_t *record_indices;
	/* 每个向量对应的稳定record_id；目标核心检索路径应优先使用该字段语义。 */
	uint64_t *record_ids;

	/* 当前写入的向量条数，可能包含删除后尚未重建清理的旧条目。 */
	size_t size;
	/* vectors/record_indices/record_ids当前可容纳的最大条数。 */
	size_t capacity;
	/* 向量维度；所有加入和查询的向量都必须匹配。 */
	size_t dimension;
	/* 索引是否已初始化并可搜索。 */
	int ready;
	/* 索引是否因为删除、重载或配置变化而需要从record store重建。 */
	int needs_rebuild;
} kvs_vector_flat_index_t;

/********** SQ8量化遍历索引 **********/

/*
 * 旧版SQ8量化遍历索引。
 *
 * 说明：
 *   1. 这是量化遍历路径，不属于近期精确遍历 + HNSW主线目标。
 *   2. 保留该结构是为了兼容当前代码，不建议在边界重构阶段继续扩展。
 */
typedef struct kvs_vector_sq8_flat_index_s {
    /* 量化后的int8向量编码，通常按size * dimension存放。 */
    int8_t *codes;
    /* 每条向量的缩放系数，用于把float向量映射到int8编码。 */
    float *scales;
    /* 每条编码对应的record数组下标；旧路径用于回表。 */
    size_t *record_indices;
    /* 每条编码对应的稳定record_id；后续核心化时应替代record_index语义。 */
    uint64_t *record_ids;

    /* 当前写入的量化向量条数。 */
    size_t size;
    /* codes/scales/record映射数组的容量。 */
    size_t capacity;
    /* 原始向量维度，也是每条量化编码的维度。 */
    size_t dimension;

    /* 索引是否已初始化并可搜索。 */
    int ready;
    /* 索引是否需要从record store重新生成量化编码。 */
    int needs_rebuild;
} kvs_vector_sq8_flat_index_t;

/********** 旧版近似图索引 **********/

/* 旧版HNSW默认每个节点的邻居数量参数。 */
#define KVS_VECTOR_HNSW_M 32
/* 旧版HNSW构建阶段候选扩展宽度。 */
#define KVS_VECTOR_HNSW_EF_CONSTRUCTION 128
/* 旧版HNSW查询阶段候选扩展宽度。 */
#define KVS_VECTOR_HNSW_EF_SEARCH 256

/*
 * 多层 HNSW 最大层数。
 *
 * level 从 0 开始，0 层是底层完整图；
 * level 越高，节点越稀疏，用于远距离导航。
 */
#define KVS_VECTOR_HNSW_MAX_LEVEL 16

/*
 * 旧版HNSW节点。
 *
 * 说明：
 *   1. 一个节点对应一条record。
 *   2. neighbors按层保存邻居record_index。
 *   3. 该结构是指针数组形式，不是目标core/hnsw的紧凑图布局。
 */
typedef struct kvs_vector_ann_node_s {
	/* 对应record store中的数组下标；旧搜索路径用它回表。 */
	size_t record_index;
	/* 对应record的稳定id；目标核心检索路径应使用该字段作为对外命中id。 */
	uint64_t record_id;

	/*
	 * 当前节点最高层级。
	 * 该节点拥有 [0, level] 共 level + 1 层邻居表。
	 */
	int level;

	/* 每层邻居数组；neighbors[level][i]保存邻居节点下标。 */
	size_t **neighbors;
	/* 每层当前邻居数量。 */
	size_t *neighbor_counts;
	/* 每层邻居数组容量。 */
	size_t *neighbor_capacities;

	/* 节点是否参与搜索；0表示该节点不应作为有效候选返回。 */
	int active;
} kvs_vector_ann_node_t;

/*
 * 旧版HNSW-lite索引。
 *
 * 说明：
 *   1. nodes数组保存图节点。
 *   2. entry_node和max_level用于从高层入口开始搜索。
 *   3. 后续核心HNSW应改为slot、访问标记、搜索上下文池和紧凑邻接存储。
 */
typedef struct kvs_vector_ann_index_s {
	/* HNSW节点数组，长度为capacity。 */
	kvs_vector_ann_node_t *nodes;

	/* 当前节点数量。 */
	size_t size;
	/* nodes数组容量。 */
	size_t capacity;
	/* 向量维度；add/search必须匹配。 */
	size_t dimension;

	/*
	 * HNSW 全局入口点和当前最高层。
	 */
	/* 入口节点下标；has_entry为0时该字段无效。 */
	size_t entry_node;
	/* 是否已经存在可用入口节点。 */
	int has_entry;
	/* 当前图中的最高层级。 */
	int max_level;

	/* 每层目标邻居数量参数。 */
	size_t m;
	/* 构建阶段候选扩展宽度。 */
	size_t ef_construction;
	/* 搜索阶段候选扩展宽度。 */
	size_t ef_search;

	/* 索引是否已初始化并可搜索。 */
	int ready;
	/* 索引是否需要从record store重新构图。 */
	int needs_rebuild;
} kvs_vector_ann_index_t;

/********** 第三方索引适配层 **********/

/*
 * 旧版索引搜索参数。
 *
 * 注意：
 *   1. scope和threshold属于管理层过滤语义，不应长期留在核心搜索接口中。
 *   2. 新核心接口应只接收查询向量和topk，并返回record_id + score。
 */
typedef struct kvs_vector_index_search_params_s {
    /* 查询限定的业务scope；旧适配层会直接用它过滤record。 */
    const char *scope;
    /* scope长度；scope为NULL时应按空scope处理。 */
    size_t scope_len;
    /* 期望返回的最大命中数量。 */
    int topk;
    /* 分数阈值；旧适配层可能在回表前后使用它过滤结果。 */
    float threshold;
} kvs_vector_index_search_params_t;

/* 旧版索引包装对象前置声明。 */
struct kvs_vector_index_s;

/*
 * 旧版索引操作表。
 *
 * 注意：
 *   1. add仍使用record_index。
 *   2. search仍返回kvs_vector_search_result_t，即question/answer/scope等管理层结果。
 *   3. 该接口应逐步替换为kvs_vector_core_ops_t。
 */
typedef struct kvs_vector_index_ops_s {
    /*
     * 添加一条向量到旧索引。
     *
     * record_index是record数组下标，不是稳定record_id。
     * embedding由调用方持有，旧索引实现通常会复制或交给backend处理。
     */
    int (*add)(
        struct kvs_vector_index_s *index,
        size_t record_index,
        const float *embedding,
        int embedding_dim
    );

    /*
     * 执行旧版搜索。
     *
     * 该接口直接输出管理层结果，因此适配层可能需要访问record store。
     * 新核心接口不应复制这种设计。
     */
    int (*search)(
        struct kvs_vector_index_s *index,
        const float *query_embedding,
        int query_dim,
        const kvs_vector_index_search_params_t *params,
        kvs_vector_search_result_t *results,
        size_t results_capacity,
        size_t *results_count
    );

    /*
     * 从record store重建旧索引。
     *
     * 旧实现通常会扫描g_vector_state.store。
     */
    int (*rebuild)(struct kvs_vector_index_s *index);

    /*
     * 填充VINFO使用的旧索引统计。
     *
     * 统计可能包含后端字段、HNSW字段或精确遍历字段。
     */
    int (*info)(
        struct kvs_vector_index_s *index,
        kvs_vector_info_t *info
    );

    /*
     * 销毁旧索引实现。
     *
     * 只应释放索引和后端资源，不应释放record store。
     */
    void (*destroy)(struct kvs_vector_index_s *index);
} kvs_vector_index_ops_t;

/*
 * 旧版统一索引包装对象。
 *
 * 管理层通过ops调用具体实现；impl可能指向精确遍历、HNSW-lite、Faiss或hnswlib后端。
 */
typedef struct kvs_vector_index_s {
    /* 索引类型，取值来自kvs_vector_index_type_t。 */
    kvs_vector_index_type_t type;
    /* 距离或相似度类型，取值来自kvs_vector_metric_type_t。 */
    kvs_vector_metric_type_t metric;
    /* 当前索引维度。 */
    size_t dimension;
    /* 具体索引或后端句柄。 */
    void *impl;
    /* 具体实现的旧版操作表。 */
    const kvs_vector_index_ops_t *ops;
} kvs_vector_index_t;

/********** topK候选保留 **********/

/*
 * 旧版候选结果。
 *
 * 使用record_index回表，目标core接口应改为record_id。
 */
typedef struct kvs_vector_candidate_s {
	/* record数组下标；不是稳定id。 */
	size_t record_index;
	/* 候选相似度分数；分数越大越好。 */
	float score;
} kvs_vector_candidate_t;

/*
 * 旧版topK小根堆。
 *
 * 用于在搜索过程中保留当前分数最高的若干候选。
 */
typedef struct kvs_vector_topk_heap_s {
	/* 候选数组，由heap持有或由初始化函数分配。 */
	kvs_vector_candidate_t *items;
	/* 当前候选数量。 */
	size_t size;
	/* items数组容量，通常等于topk。 */
	size_t capacity;
} kvs_vector_topk_heap_t;

/*
 * 旧版查询上下文。
 *
 * 保存VSEARCH解析后的业务参数。新核心搜索上下文不应保存question/scope等业务字段。
 */
typedef struct kvs_vector_query_ctx_s {
	/* 原始查询文本，由调用方持有。 */
	const char *query;
	/* 查询文本长度。 */
	size_t query_len;
	/* 查询限定scope，由调用方持有。 */
	const char *scope;
	/* scope长度。 */
	size_t scope_len;
	/* 期望返回数量。 */
	int topk;
	/* 分数阈值。 */
	float threshold;
} kvs_vector_query_ctx_t;

/********** 旧版精确遍历索引接口 **********/

/*
 * 初始化旧版精确遍历索引。
 *
 * dimension为向量维度；如果暂时未知，可由实现决定是否允许0维延后初始化。
 */
int kvs_vector_flat_index_init(
	kvs_vector_flat_index_t *index,
	size_t dimension
);

/*
 * 销毁旧版精确遍历索引。
 *
 * 释放vectors、record_indices、record_ids等索引内部内存。
 * 不释放record store中的record。
 */
void kvs_vector_flat_index_destroy(kvs_vector_flat_index_t *index);

/*
 * 重置旧版精确遍历索引。
 *
 * 清空已有向量条目；实现可能释放并重新初始化内部数组。
 */
void kvs_vector_flat_index_reset(kvs_vector_flat_index_t *index);

/*
 * 标记旧版精确遍历索引已经陈旧。
 *
 * 通常在VDEL或record集合变化后调用，提示manager后续需要rebuild。
 */
void kvs_vector_flat_index_mark_stale(kvs_vector_flat_index_t *index);

/*
 * 预留旧版精确遍历索引容量。
 *
 * new_capacity表示至少可容纳的向量条数，不是字节数。
 */
int kvs_vector_flat_index_reserve(
	kvs_vector_flat_index_t *index,
	size_t new_capacity
);

/*
 * 向旧版精确遍历索引添加一条record向量。
 *
 * record_index是record数组下标；embedding由调用方提供。
 * embedding_dim必须与index->dimension一致。
 */
int kvs_vector_flat_index_add(
	kvs_vector_flat_index_t *index,
	size_t record_index,
	const float *embedding,
	int embedding_dim
);

/*
 * 从record store重建旧版精确遍历索引。
 *
 * 会扫描当前有效record，把embedding重新写入连续向量数组。
 */
int kvs_vector_rebuild_flat_index_from_store(void);

/*
 * 在旧版精确遍历索引上执行精确搜索。
 *
 * 该接口仍接收scope和threshold，并直接输出管理层搜索结果。
 * 新核心精确遍历接口应改为只返回record_id + score。
 */
int kvs_vector_flat_index_search(
	kvs_vector_flat_index_t *index,
	const float *query_embedding,
	int query_dim,
	const char *scope,
	size_t scope_len,
	int topk,
	float threshold,
	kvs_vector_metric_type_t metric,
	kvs_vector_search_result_t *results,
	size_t results_capacity,
	size_t *results_count
);

/********** 相似度分数计算 **********/

/*
 * 计算两个向量的点积。
 *
 * lhs和rhs长度都必须至少为dim。
 */
float kvs_vector_dot_product(
	const float *lhs,
	const float *rhs,
	size_t dim
);

/*
 * 计算L2分数。
 *
 * KVLite对外约定分数越大越好，因此该函数不是直接返回距离越小越好的原始L2距离。
 */
float kvs_vector_l2_score(
	const float *lhs,
	const float *rhs,
	size_t dim
);

/*
 * 计算余弦相似度分数。
 *
 * lhs和rhs长度都必须至少为dim。
 */
float kvs_vector_cosine_score(
	const float *lhs,
	const float *rhs,
	size_t dim
);

/*
 * 按metric选择具体距离/相似度算法。
 *
 * 返回值遵守KVLite约定：分数越大越相似。
 */
float kvs_vector_metric_score(
	const float *lhs,
	const float *rhs,
	size_t dim,
	kvs_vector_metric_type_t metric
);

/*
 * 返回metric枚举对应的可读名称。
 *
 * 用于日志、VINFO或调试输出；调用方不拥有返回字符串。
 */
const char *kvs_vector_metric_name(kvs_vector_metric_type_t metric);

/*
 * 从当前配置读取metric类型。
 *
 * 具体配置来源由实现决定，通常来自全局配置。
 */
kvs_vector_metric_type_t kvs_vector_metric_from_config(void);

/********** 当前旧版索引分发接口 **********/

/*
 * 根据当前配置绑定g_vector_state.current_index。
 *
 * 旧路径用于在精确遍历、HNSW、SQ8、Faiss、hnswlib之间选择当前索引实现。
 */
int kvs_vector_index_bind_current(void);

/*
 * 向当前旧版索引添加一条record向量。
 *
 * record_index是record数组下标，不是稳定record_id。
 */
int kvs_vector_index_add_current(
	size_t record_index,
	const float *embedding,
	int embedding_dim
);

/*
 * 在当前旧版索引上执行搜索。
 *
 * 该接口直接输出kvs_vector_search_result_t，仍包含管理层回表和业务过滤语义。
 */
int kvs_vector_index_search_current(
	const float *query_embedding,
	int query_dim,
	const char *scope,
	size_t scope_len,
	int topk,
	float threshold,
	kvs_vector_search_result_t *results,
	size_t results_capacity,
	size_t *results_count
);

/*
 * 重建当前旧版索引。
 *
 * 应基于record store中的ACTIVE记录重建派生索引状态。
 */
int kvs_vector_index_rebuild_current(void);

/********** 旧版HNSW-lite索引接口 **********/

/*
 * 初始化旧版HNSW-lite索引。
 *
 * m、ef_construction、ef_search分别控制邻居数量、构建候选宽度和搜索候选宽度。
 */
int kvs_vector_ann_index_init(
	kvs_vector_ann_index_t *index,
	size_t dimension,
	size_t m,
	size_t ef_construction,
	size_t ef_search
);

/*
 * 销毁旧版HNSW-lite索引。
 *
 * 释放节点数组和每个节点的邻居数组。
 */
void kvs_vector_ann_index_destroy(kvs_vector_ann_index_t *index);

/*
 * 重置旧版HNSW-lite索引内容。
 *
 * 清空节点、入口点和层级状态。
 */
void kvs_vector_ann_index_reset(kvs_vector_ann_index_t *index);

/*
 * 预留旧版HNSW-lite节点容量。
 *
 * new_capacity表示至少可容纳的节点数量。
 */
int kvs_vector_ann_index_reserve(
	kvs_vector_ann_index_t *index,
	size_t new_capacity
);

/*
 * 向旧版HNSW-lite索引添加一条record向量。
 *
 * record_index用于回表；embedding_dim必须与index->dimension一致。
 */
int kvs_vector_ann_index_add(
	kvs_vector_ann_index_t *index,
	size_t record_index,
	const float *embedding,
	int embedding_dim
);

/*
 * 在旧版HNSW-lite索引上执行搜索。
 *
 * 该接口仍接收scope/threshold并返回管理层结果。
 * 目标核心HNSW搜索不应回调管理层，也不应直接处理scope。
 */
int kvs_vector_ann_index_search(
	kvs_vector_ann_index_t *index,
	const float *query_embedding,
	int query_dim,
	const char *scope,
	size_t scope_len,
	int topk,
	float threshold,
	kvs_vector_metric_type_t metric,
	kvs_vector_search_result_t *results,
	size_t results_capacity,
	size_t *results_count
);

/*
 * 从record store重建旧版HNSW-lite索引。
 *
 * 通常在启动加载、VINDEX或删除后重建时调用。
 */
int kvs_vector_rebuild_ann_index_from_store(void);

/********** 内存占用估算 **********/

/* 估算record store及record字段占用的内存。 */
size_t kvs_vector_estimate_record_memory_bytes(void);
/* 估算旧版精确遍历索引占用的内存。 */
size_t kvs_vector_estimate_flat_memory_bytes(void);
/* 估算旧版HNSW-lite索引占用的内存。 */
size_t kvs_vector_estimate_hnsw_memory_bytes(void);
/* 估算旧版SQ8量化遍历索引占用的内存。 */
size_t kvs_vector_estimate_sq8_flat_memory_bytes(void);
/* 估算当前选中索引占用的内存。 */
size_t kvs_vector_estimate_current_index_memory_bytes(void);

/********** SQ8量化遍历索引接口 **********/

/*
 * 初始化旧版SQ8量化遍历索引。
 *
 * SQ8是量化遍历路径，不属于近期精确遍历 + HNSW主线。
 */
int kvs_vector_sq8_flat_index_init(
	kvs_vector_sq8_flat_index_t *index,
	size_t dimension
);

/*
 * 销毁旧版SQ8量化遍历索引并释放量化编码、缩放系数和record映射数组。
 */
void kvs_vector_sq8_flat_index_destroy(
	kvs_vector_sq8_flat_index_t *index
);

/*
 * 向旧版SQ8量化遍历索引添加一条record向量。
 *
 * 实现会把float embedding量化为int8编码。
 */
int kvs_vector_sq8_flat_index_add(
	kvs_vector_sq8_flat_index_t *index,
	size_t record_index,
	const float *embedding,
	int embedding_dim
);

/*
 * 从record store重建旧版SQ8量化遍历索引。
 */
int kvs_vector_sq8_flat_index_rebuild(
	kvs_vector_sq8_flat_index_t *index
);

/*
 * 在旧版SQ8量化遍历索引上执行搜索。
 *
 * 该接口仍输出管理层搜索结果，后续不应作为新核心接口模板。
 */
int kvs_vector_sq8_flat_index_search(
	kvs_vector_sq8_flat_index_t *index,
	const float *query_embedding,
	int query_dim,
	const char *scope,
	size_t scope_len,
	int topk,
	float threshold,
	kvs_vector_metric_type_t metric,
	kvs_vector_search_result_t *results,
	size_t results_capacity,
	size_t *results_count
);

/*
 * 填充SQ8量化遍历相关VINFO字段。
 */
int kvs_vector_sq8_flat_index_info(
	const kvs_vector_sq8_flat_index_t *index,
	kvs_vector_info_t *info
);

/********** topK小根堆接口 **********/

/*
 * 初始化旧版topK堆。
 *
 * capacity通常等于topk，表示最多保留多少个候选。
 */
int kvs_vector_topk_heap_init(
	kvs_vector_topk_heap_t *heap,
	size_t capacity
);

/*
 * 销毁旧版topK堆并释放候选数组。
 */
void kvs_vector_topk_heap_destroy(kvs_vector_topk_heap_t *heap);

/*
 * 清空旧版topK堆内容，但保留已分配数组。
 */
void kvs_vector_topk_heap_reset(kvs_vector_topk_heap_t *heap);

/*
 * 向旧版topK堆插入一个候选。
 *
 * record_index用于后续回表；score越大越好。
 */
int kvs_vector_topk_heap_push(
	kvs_vector_topk_heap_t *heap,
	size_t record_index,
	float score
);

/*
 * 按分数从高到低导出旧版topK结果。
 *
 * 该函数会根据record_index访问store并填充kvs_vector_search_result_t。
 * 新核心接口应避免在小根堆导出阶段直接依赖record store。
 */
int kvs_vector_topk_heap_export_desc(
	kvs_vector_topk_heap_t *heap,
	kvs_vector_search_result_t *results,
	size_t results_capacity,
	size_t *results_count,
	const kvs_vector_store_t *store
);
