#pragma once

#include <stddef.h>
#include <stdint.h>

/*
 * 向量计算层边界。
 *
 * 本文件只定义KVmanager调用向量计算层所需的最小接口。
 * 向量计算层只接收“记录id + 向量”，搜索时只返回“记录id + 分数”。
 *
 * 向量计算层不保存、也不理解下列业务字段：
 *   question / answer / scope / TTL / metadata / AOF / RDB / replication
 *
 * record store是唯一真实数据源。
 * flat和HNSW索引只是从record store重建出来的计算缓存。
 */

/*
 * 单条core搜索命中结果。
 *
 * 注意：
 *   1. 只包含record_id和score，不包含question/answer/scope等manager字段。
 *   2. manager负责用record_id回表，并执行scope/TTL/deleted/metadata过滤。
 *   3. score采用KVLite约定：越大越好。
 */
typedef struct kvs_vector_hit_s {
	/* 稳定record id，由KVmanager分配；不是record数组下标，也不是core内部slot。 */
	uint64_t record_id;
	/* 相似度分数；不管底层使用哪种距离算法，对外都统一为分数越大越相似。 */
	float score;
} kvs_vector_hit_t;

/*
 * vector_core索引信息。
 *
 * 用于VINFO或调试统计，只描述core自身状态，不包含record store统计。
 */
typedef struct kvs_vector_core_info_s {
	/* core当前预留的最大向量数量。 */
	size_t capacity;
	/* core当前持有的向量槽位数量，可能包含已标记删除但尚未重建清理的槽位。 */
	size_t size;
	/* 向量维度；add/search输入dim必须与该字段一致。 */
	size_t dim;

	/* 距离或相似度类型，取值应与KVLite向量模块的metric枚举保持一致。 */
	int metric_type;
	/* 索引是否已初始化并可用于搜索。 */
	int ready;
	/* 索引是否已陈旧，需要由manager基于record store触发rebuild。 */
	int needs_rebuild;

	/* core内部估算内存占用，包含向量数组、图结构、id映射、搜索临时缓冲等内存。 */
	size_t memory_bytes;
	/* core内部mark_delete产生的逻辑删除数量。 */
	size_t deleted_count;
} kvs_vector_core_info_t;

/* 不透明索引句柄；调用方不能直接访问具体实现内部字段。 */
struct kvs_vector_core_index_s;

/*
 * vector_core统一操作表。
 *
 * 约束：
 *   1. 所有回调只处理record_id/vector/score。
 *   2. 不允许回调访问KVmanager record字段。
 *   3. 不允许search回调在热路径依赖manager回调。
 *   4. index实现负责维护record_id <-> internal slot映射。
 */
typedef struct kvs_vector_core_ops_s {
	/*
	 * 预留至少capacity条向量的core容量。
	 *
	 * flat通常分配连续向量数组。
	 * HNSW还应分配图结构、访问标记数组和搜索临时缓冲。
	 *
	 * 返回0表示成功，返回非0表示参数错误或内存分配失败。
	 */
	int (*reserve)(
		struct kvs_vector_core_index_s *index,
		size_t capacity
	);

	/*
	 * 添加或更新一条向量。
	 *
	 * record_id是外部稳定id；core内部应映射到slot。
	 * vector由调用方持有，core需要复制其内容或明确实现自己的生命周期策略。
	 * dim必须与index->dim一致。
	 *
	 * 返回0表示成功，返回非0表示参数错误、维度不一致或容量不足。
	 */
	int (*add)(
		struct kvs_vector_core_index_s *index,
		uint64_t record_id,
		const float *vector,
		size_t dim
	);

	/*
	 * 执行topK搜索。
	 *
	 * out由调用方提供，容量至少为topk；out_count返回实际写入数量。
	 * 返回结果只包含record_id + score，manager随后执行回表和业务过滤。
	 * query由调用方持有，search不会修改query内容。
	 * dim必须与index->dim一致。
	 *
	 * 返回0表示成功，返回非0表示参数错误、索引未就绪或维度不一致。
	 */
	int (*search)(
		struct kvs_vector_core_index_s *index,
		const float *query,
		size_t dim,
		size_t topk,
		kvs_vector_hit_t *out,
		size_t *out_count
	);

	/*
	 * 按record_id进行逻辑删除。
	 *
	 * 第一阶段不要求物理compact；实现可设置deleted flag并等待rebuild。
	 * 被删除的record_id不应再从search结果中返回。
	 *
	 * 返回0表示成功；record_id不存在时由具体实现决定返回“未找到”还是幂等成功。
	 */
	int (*mark_delete)(
		struct kvs_vector_core_index_s *index,
		uint64_t record_id
	);

	/*
	 * 清空索引内容。
	 *
	 * 实现可以保留已分配capacity以便后续快速rebuild。
	 * reset后size应为0，旧record_id到slot的映射应失效。
	 *
	 * 返回0表示成功，返回非0表示索引句柄无效或内部清理失败。
	 */
	int (*reset)(
		struct kvs_vector_core_index_s *index
	);

	/*
	 * 导出core状态和内存统计。
	 *
	 * out由调用方提供；函数只填写统计字段，不转移任何内存所有权。
	 * 主要用于VINFO、日志和压测诊断。
	 */
	int (*info)(
		struct kvs_vector_core_index_s *index,
		kvs_vector_core_info_t *out
	);

	/*
	 * 销毁core实现并释放所有由core持有的资源。
	 *
	 * destroy只释放impl内部资源，不释放KVmanager中的record store。
	 */
	void (*destroy)(
		struct kvs_vector_core_index_s *index
	);
} kvs_vector_core_ops_t;

/*
 * vector_core统一index包装对象。
 *
 * manager只依赖该包装对象和ops调用，不应直接读取impl内部结构。
 */
typedef struct kvs_vector_core_index_s {
	/* 索引类型，例如exact_flat或hnsw；取值由KVLite内部枚举定义。 */
	int index_type;
	/* 距离或相似度类型，例如IP/L2/COSINE；取值应与KVLite metric枚举一致。 */
	int metric_type;
	/* 当前索引维度；所有add/search请求必须匹配。 */
	size_t dim;

	/* 具体索引实现指针；flat和HNSW会指向不同的内部结构。 */
	void *impl;
	/* 该索引实现对应的操作表；manager通过这些函数访问索引。 */
	const kvs_vector_core_ops_t *ops;
} kvs_vector_core_index_t;
