

//向量record存储层定义
#pragma once

#include <stddef.h>
#include <stdint.h>

/*
 * 向量记录模块，只用于KVmanager管理记录。
 *
 * 设计指标：
 *   1. record store 是向量模块唯一真实数据源头。
 *   2. vector_core中的flat/HNSW索引都是可重建的派生状态。
 *   3. 本文件不关注core中使用什么索引，也不暴露图结构、heap、visited list等热路径细节。
 *   4. 字符串和embedding指针由record持有，销毁record时统一释放。
 */

 /********** record **********/

 //定义record状态
 typedef enum kvs_vector_record_state_e {
    KVS_VECTOR_RECORD_EMPTY = 0,   //空槽位，尚未写入有效记录，可被新record直接使用
	KVS_VECTOR_RECORD_ACTIVE = 1,  //活跃记录，可被VGET/VSEARCH命中，也是重建index时的数据来源
	KVS_VECTOR_RECORD_DELETED = 2, //逻辑删除记录，不再对外可见，等待复用或重建清理
 } kvs_vector_record_state_t;

 /*
  * 单条向量record。
  *
  * 说明：
  *   1. record保存产品语义字段，包括问题、答案、scope、TTL和embedding。
  *   2. vector_core只能通过record_id和embedding建立索引，不应直接依赖本结构体。
  *   3. question/answer/normalized_question/scope/embedding均由record拥有。
  */
 typedef struct kvs_vector_record_s {
    //全局唯一记录id，是manager/core之间的稳定标识；slot复用时该id不应复用
    uint64_t id;
    //question内容的hash，用于key map快速定位；候选命中后仍需比较question和scope原文
    uint32_t question_hash;

    //原始问题文本，由record持有内存；VGET/VSEARCH响应会引用该字段
    char* question;
    //原始问题文本长度，不依赖NUL结尾；允许文本中包含非NUL安全字节
    size_t question_len;
    //答案文本，由record持有内存；VGET/VSEARCH响应会引用该字段
    char* answer;
    //答案文本长度，不依赖NUL结尾
    size_t answer_len;
    //归一化后的问题文本，用于embedding生成、缓存命中或后续规范化匹配
    char* normalized_question;
    //归一化问题文本长度，不依赖NUL结尾
    size_t normalized_question_len;

    //业务作用域，由record持有内存；空scope与NULL scope按manager规则处理
    char* scope;
    //业务作用域长度，不依赖NUL结尾；scope_len为0表示默认/空scope
    size_t scope_len;

    //问题对应的向量embedding，由record持有；这是重建vector_core索引的source of truth
    float *embedding;
    //embedding维度；ACTIVE记录中应大于0，并与当前vector index维度一致
    int embedding_dim;

    //记录状态，取值为kvs_vector_record_state_t；决定该slot是否可见或可复用
    int state;
    //逻辑删除标记，与state配合用于快速过滤；非0表示不可被查询结果返回
    int deleted;

    //记录创建时间，毫秒时间戳；用于审计、持久化和后续TTL/版本判断
    int64_t created_at_ms;
    //记录最后更新时间，毫秒时间戳；VSET更新answer或record内容时刷新
	int64_t updated_at_ms;

    //过期时间，毫秒时间戳；无过期时使用KV_TTL_NO_EXPIRE语义值
	int64_t expire_at_ms;
    //记录世代，用于重建、同步、并发路径识别记录版本
	uint64_t generation;
    //生成embedding时使用的模型版本，用于判断模型升级后是否需要重新编码
	uint32_t embedding_model_version;
 } kvs_vector_record_t;

 /********** record_id -> slot fast resolve map **********/

/*
 * record_id -> record_index map entry状态。
 *
 * 该map只属于manager内存态，用于把core/backend返回的稳定record_id快速解析到
 * record store slot。它不进入RDB/AOF；load/rebuild后从active records重建。
 */
typedef enum kvs_vector_record_id_entry_state_e {
	KVS_VECTOR_RECORD_ID_ENTRY_EMPTY = 0,
	KVS_VECTOR_RECORD_ID_ENTRY_USED = 1,
	KVS_VECTOR_RECORD_ID_ENTRY_TOMBSTONE = 2,
} kvs_vector_record_id_entry_state_t;

/*
 * record_id -> record_index 单个桶。
 *
 * record_index仍然只是manager内部slot；使用者回表后必须校验record->id仍等于
 * record_id，以防slot复用导致stale hit误命中新record。
 */
typedef struct kvs_vector_record_id_entry_s {
	uint64_t record_id;
	size_t record_index;
	uint8_t state;
} kvs_vector_record_id_entry_t;

/*
 * record_id -> record_index 的开放寻址hash map。
 *
 * 只保存ACTIVE且未deleted的record_id映射。VDEL、slot复用、RDB load和rebuild
 * 必须同步更新或重建该map。
 */
typedef struct kvs_vector_record_id_map_s {
	kvs_vector_record_id_entry_t *entries;
	size_t capacity;
	size_t size;
	size_t tombstones;
} kvs_vector_record_id_map_t;

 /********** store **********/

 /*
  * record store。
  *
  * 说明：
  *   1. records数组中的下标是record_index，只是manager内部slot，不应作为长期稳定外部id。
  *   2. record_id才是manager/core之间传递的稳定标识。
  *   3. free_indices保存可复用slot，用于避免删除后频繁搬移record。
  */
 typedef struct kvs_vector_store_s {
    //record数组，长度为capacity；每个元素是一个可复用slot
    kvs_vector_record_t *records;
    //records数组容量，即当前最多可容纳的slot数量
	size_t capacity;

    //历史创建过的record数量统计；具体语义由manager维护
	size_t total_records;
    //当前ACTIVE且未逻辑删除的record数量
	size_t active_records;
    //当前处于DELETED或deleted标记状态的record数量
	size_t deleted_records;

    //下一次顺序扫描空slot的起点，用于降低查找空位成本
	size_t next_slot;

    //可复用record_index数组，保存被删除或释放的slot
	size_t *free_indices;
    //free_indices中当前可用元素数量
	size_t free_count;
    //free_indices数组容量
	size_t free_capacity;

	//下一个待分配的全局record_id；只递增，不应因slot复用而回退
	uint64_t next_id;
	//manager-owned record_id -> record_index fast resolve map，不持久化
	kvs_vector_record_id_map_t record_id_map;
 } kvs_vector_store_t;

 /********** key/scope id map **********/

/*
 * key/scope map entry状态。
 *
 * 该map用于把(question, scope)定位到record_index。
 * record_index仍然只是manager内部slot；对vector_core应转换为record_id。
 */
typedef enum kvs_vector_map_entry_state_e {
	KVS_VECTOR_MAP_ENTRY_EMPTY = 0,     //从未使用的空桶，开放寻址查找可在此停止
	KVS_VECTOR_MAP_ENTRY_USED = 1,      //正在使用的桶，hash和record_index有效
	KVS_VECTOR_MAP_ENTRY_TOMBSTONE = 2, //删除后的墓碑桶，查找不能提前停止，后续可复用
} kvs_vector_map_entry_state_t;

/*
 * key/scope map单个桶。
 *
 * hash只用于快速定位；命中后必须回表比较question和scope，避免hash冲突误判。
 */
typedef struct kvs_vector_id_entry_s {
	//组合key hash，通常来自question和scope；用于开放寻址探测
	uint32_t hash;
	//指向store.records的slot下标；不是稳定record_id
	size_t record_index;
	//桶状态，取值为kvs_vector_map_entry_state_t
	uint8_t state;
} kvs_vector_id_entry_t;

/*
 * key/scope -> record_index 的开放寻址hash map。
 *
 * 说明：
 *   1. entries容量通常应保持为2的幂，便于用mask探测。
 *   2. tombstones过多时需要rehash，避免探测链退化。
 *   3. map只保存索引关系，不拥有record内存。
 */
typedef struct kvs_vector_id_map_s {
	//hash桶数组，长度为capacity
	kvs_vector_id_entry_t *entries;
	//entries数组容量
	size_t capacity;
	//USED桶数量
	size_t size;
	//TOMBSTONE桶数量
	size_t tombstones;
} kvs_vector_id_map_t;


/********** record/store/id-map helpers **********/

/*
 * 计算不小于value的2的幂。
 *
 * 用途：
 *   1. 给开放寻址hash map选择容量。
 *   2. 让探测位置可以使用capacity - 1作为mask。
 */
size_t kvs_vector_next_pow2(size_t value);

/*
 * 计算一段字节内容的hash。
 *
 * data不要求以NUL结尾；len决定参与hash的字节数。
 * 返回值只用于快速定位，不能代替原文比较。
 */
uint32_t kvs_vector_hash_key(const char *data, size_t len);

/*
 * 组合question和scope的hash。
 *
 * 用途：
 *   key/scope id map用该hash定位候选桶。
 *
 * 注意：
 *   即使hash命中，也必须回表比较question和scope，避免hash冲突。
 */
uint32_t kvs_vector_compose_hash(
	const char *question,
	size_t question_len,
	const char *scope,
	size_t scope_len
);

/*
 * 复制一段字符串并补一个NUL结尾。
 *
 * src_len控制复制长度，src可以不是NUL结尾字符串。
 * out_len非空时写入复制的有效文本长度。
 * 返回的新内存由调用方负责释放。
 */
char *kvs_vector_dup_string(
	const char *src,
	size_t src_len,
	size_t *out_len
);

/*
 * 按长度比较两段文本。
 *
 * 不依赖NUL结尾。
 * 返回非0表示长度和内容都相同。
 */
int kvs_vector_text_equal(
	const char *lhs,
	size_t lhs_len,
	const char *rhs,
	size_t rhs_len
);

/*
 * 比较两个scope是否相同。
 *
 * manager规则中，NULL scope和长度为0的scope都视为空scope。
 * 返回非0表示scope相同。
 */
int kvs_vector_scope_equal(
	const char *lhs,
	size_t lhs_len,
	const char *rhs,
	size_t rhs_len
);

/********** record lifecycle **********/

/*
 * 将record字段重置为空槽位状态。
 *
 * 注意：
 *   该函数只清空字段，不释放record内部已经持有的指针。
 *   如果record持有question/answer/scope/embedding，应先调用destroy。
 */
void kvs_vector_record_reset(kvs_vector_record_t *record);

/*
 * 释放record持有的所有内存并重置为空槽位。
 *
 * 会释放question、answer、normalized_question、scope和embedding。
 * 不释放record本身，因为record通常是store.records数组中的一个元素。
 */
void kvs_vector_record_destroy(kvs_vector_record_t *record);

/*
 * 用question/answer/scope初始化record，并由manager生成embedding。
 *
 * record会接管新分配出的字段内存。
 * question不能为空且question_len必须大于0。
 * answer不能为空；answer_len可以为0。
 * scope为空时表示默认/空scope。
 */
int kvs_vector_record_set_fields(
	kvs_vector_record_t *record,
	const char *question,
	size_t question_len,
	const char *answer,
	size_t answer_len,
	const char *scope,
	size_t scope_len
);

/*
 * 更新已有ACTIVE record的answer字段。
 *
 * 只替换answer和answer_len，并刷新updated_at_ms。
 * 不改变question、scope、embedding和record_id。
 */
int kvs_vector_record_update_answer(
	kvs_vector_record_t *record,
	const char *answer,
	size_t answer_len
);

/*
 * 将record标记为逻辑删除。
 *
 * 不立即释放字段内存，不移动record槽位。
 * 后续VSEARCH/VGET应跳过该record，索引可等待rebuild清理。
 */
int kvs_vector_record_mark_deleted(kvs_vector_record_t *record);

/*
 * 判断record是否精确匹配question和scope。
 *
 * 要求record为ACTIVE且未删除。
 * question按长度和内容比较；scope按空scope规则比较。
 */
int kvs_vector_record_match(
	const kvs_vector_record_t *record,
	const char *question,
	size_t question_len,
	const char *scope,
	size_t scope_len
);

/*
 * 判断record是否属于指定scope。
 *
 * 要求record为ACTIVE且未删除。
 * 用于VSEARCH按scope过滤候选record。
 */
int kvs_vector_record_scope_match(
	const kvs_vector_record_t *record,
	const char *scope,
	size_t scope_len
);

/********** record store **********/

/*
 * 初始化record store。
 *
 * 初始化后store可接收reserve/ensure_capacity等操作。
 */
int kvs_vector_store_init(kvs_vector_store_t *store);

/*
 * 销毁record store。
 *
 * 会销毁records数组中的record字段，并释放records和free list。
 * 不负责销毁任何vector_core索引。
 */
void kvs_vector_store_destroy(kvs_vector_store_t *store);

/*
 * 将record store容量扩展到至少new_capacity。
 *
 * 扩容后原有record内容保持不变，新槽位应处于EMPTY状态。
 */
int kvs_vector_store_reserve(
	kvs_vector_store_t *store,
	size_t new_capacity
);

/*
 * 确保record store至少能容纳min_capacity个槽位。
 *
 * 容量不足时按实现策略扩容。
 */
int kvs_vector_store_ensure_capacity(
	kvs_vector_store_t *store,
	size_t min_capacity
);

/*
 * 压缩record store。
 *
 * 预期用途是清理删除槽位或整理存储。
 * 调用后record_index可能变化，因此相关id_map和索引必须同步重建。
 */
int kvs_vector_store_compact(kvs_vector_store_t *store);

/*
 * 将一个record_index放入可复用槽位列表。
 *
 * record_index必须是store.records中的有效下标。
 */
int kvs_vector_store_free_list_push(
	kvs_vector_store_t *store,
	size_t record_index
);

/*
 * 从可复用槽位列表弹出一个record_index。
 *
 * out_record_index由调用方提供。
 * 如果没有可复用槽位，返回值由实现定义为未找到或错误。
 */
int kvs_vector_store_free_list_pop(
	kvs_vector_store_t *store,
	size_t *out_record_index
);

/*
 * 清空可复用槽位列表。
 *
 * 不销毁records数组，也不改变record本身状态。
 */
void kvs_vector_store_free_list_clear(kvs_vector_store_t *store);

/********** key/scope id map **********/

/*
 * 初始化key/scope id map。
 *
 * 初始化后map可用于put/get/remove。
 */
int kvs_vector_id_map_init(kvs_vector_id_map_t *map);

/*
 * 销毁key/scope id map。
 *
 * 只释放entries数组，不释放record store或record字段。
 */
void kvs_vector_id_map_destroy(kvs_vector_id_map_t *map);

/*
 * 将map容量扩展到至少new_capacity。
 *
 * 扩容时需要重新插入已有USED桶，避免开放寻址探测链失效。
 */
int kvs_vector_id_map_reserve(
	kvs_vector_id_map_t *map,
	size_t new_capacity
);

/*
 * 按question和scope查找record_index。
 *
 * out_record_index由调用方提供。
 * hash命中后实现必须回表比较原文，避免hash冲突。
 */
int kvs_vector_id_map_get(
	kvs_vector_id_map_t *map,
	const char *question,
	size_t question_len,
	const char *scope,
	size_t scope_len,
	size_t *out_record_index
);

/*
 * 写入或更新question/scope到record_index的映射。
 *
 * record_index是record store内部槽位，不是稳定record_id。
 */
int kvs_vector_id_map_put(
	kvs_vector_id_map_t *map,
	const char *question,
	size_t question_len,
	const char *scope,
	size_t scope_len,
	size_t record_index
);

/*
 * 删除question/scope对应的map条目。
 *
 * 开放寻址实现通常会留下TOMBSTONE，避免破坏后续探测链。
 */
int kvs_vector_id_map_remove(
	kvs_vector_id_map_t *map,
	const char *question,
	size_t question_len,
	const char *scope,
	size_t scope_len
);

int kvs_vector_record_id_map_init(kvs_vector_record_id_map_t *map);

void kvs_vector_record_id_map_destroy(kvs_vector_record_id_map_t *map);

int kvs_vector_record_id_map_reserve(
	kvs_vector_record_id_map_t *map,
	size_t new_capacity
);

int kvs_vector_record_id_map_get(
	kvs_vector_record_id_map_t *map,
	uint64_t record_id,
	size_t *out_record_index
);

int kvs_vector_record_id_map_put(
	kvs_vector_record_id_map_t *map,
	uint64_t record_id,
	size_t record_index
);

int kvs_vector_record_id_map_remove(
	kvs_vector_record_id_map_t *map,
	uint64_t record_id
);
