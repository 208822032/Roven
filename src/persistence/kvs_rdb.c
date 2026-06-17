#define KVS_VECTOR_INTERNAL
#include "kvstore.h"
#include "kvs_command_executor.h"
#include "kvs_globals.h"
#include <stdio.h>
#include <string.h>
#include <time.h>
#include <stdint.h>
#include "kvs_memory.h"
#include "kvs_log.h"

#define RDB_TYPE_ARRAY     0x01
#define RDB_TYPE_RBTREE    0x02
#define RDB_TYPE_HASH      0x03
#define RDB_TYPE_SKIPTABLE 0x04
#define RDB_TYPE_VECTOR    0x05
#define RDB_TYPE_EOF       0xFF

#define RDB_VECTOR_SECTION_VERSION 1

#if ENABLE_ARRAY
extern kvs_array_t global_array;
#endif

#if ENABLE_RBTREE
extern kvs_rbtree_t global_rbtree;
#endif

#if ENABLE_HASH
extern kvs_hash_t global_hash;
#endif

#if ENABLE_SKIPTABLE
extern kvs_skiptable_t global_skiptable;
#endif

extern rbtree_node *rbtree_search(rbtree *T, KEY_TYPE key);

// 内部函数前向声明（需在首次调用前声明以避免隐式声明）
static int rdb_save_array_snapshot(FILE *fp, const kvs_rdb_snapshot_t *snapshot);
static int rdb_load_array(FILE *fp, uint8_t version, int64_t now_ms);
static int rdb_save_rbtree_snapshot(FILE *fp, const kvs_rdb_snapshot_t *snapshot);
static int rdb_load_rbtree(FILE *fp, uint8_t version, int64_t now_ms);
static int rdb_save_hash_snapshot(FILE *fp, const kvs_rdb_snapshot_t *snapshot);
static int rdb_load_hash(FILE *fp, uint8_t version, int64_t now_ms);
static int rdb_save_skiptable_snapshot(FILE *fp, const kvs_rdb_snapshot_t *snapshot);
static int rdb_load_skiptable(FILE *fp, uint8_t version, int64_t now_ms);

static int rdb_write_exact(FILE *fp, const void *buf, size_t size) {
    return fwrite(buf, size, 1, fp) == 1 ? RDB_OK : RDB_ERR;
}

static int rdb_read_exact(FILE *fp, void *buf, size_t size) {
    return fread(buf, size, 1, fp) == 1 ? RDB_OK : RDB_ERR;
}

static int rdb_write_int64(FILE *fp, int64_t value) {
    return rdb_write_exact(fp, &value, sizeof(value));
}

static int rdb_read_int64(FILE *fp, int64_t *value) {
    return rdb_read_exact(fp, value, sizeof(*value));
}

static int rdb_write_u8(FILE *fp, uint8_t value) {
    return rdb_write_exact(fp, &value, sizeof(value));
}

static int rdb_read_u8(FILE *fp, uint8_t *value) {
    return rdb_read_exact(fp, value, sizeof(*value));
}

static int rdb_write_u32(FILE *fp, uint32_t value) {
    return rdb_write_exact(fp, &value, sizeof(value));
}

static int rdb_read_u32(FILE *fp, uint32_t *value) {
    return rdb_read_exact(fp, value, sizeof(*value));
}

static int rdb_write_u64(FILE *fp, uint64_t value) {
    return rdb_write_exact(fp, &value, sizeof(value));
}

static int rdb_read_u64(FILE *fp, uint64_t *value) {
    return rdb_read_exact(fp, value, sizeof(*value));
}

static int rdb_write_i32(FILE *fp, int32_t value) {
    return rdb_write_exact(fp, &value, sizeof(value));
}

static int rdb_read_i32(FILE *fp, int32_t *value) {
    return rdb_read_exact(fp, value, sizeof(*value));
}

static int rdb_write_i64(FILE *fp, int64_t value) {
    return rdb_write_exact(fp, &value, sizeof(value));
}

static int rdb_read_i64(FILE *fp, int64_t *value) {
    return rdb_read_exact(fp, value, sizeof(*value));
}

static int rdb_write_blob(FILE *fp, const void *data, uint32_t len) {
    if (rdb_write_u32(fp, len) != RDB_OK) return RDB_ERR;
    if (len > 0 && data) {
        return rdb_write_exact(fp, data, len);
    }
    return RDB_OK;
}

static int rdb_read_blob(FILE *fp, char **out, uint32_t *out_len) {
    uint32_t len;
    char *buf = NULL;

    if (rdb_read_u32(fp, &len) != RDB_OK) return RDB_ERR;

    if (len > 0) {
        buf = kvs_malloc((size_t)len + 1);
        if (!buf) return RDB_ERR;
        if (rdb_read_exact(fp, buf, len) != RDB_OK) {
            kvs_free(buf);
            return RDB_ERR;
        }
        buf[len] = '\0';
    }

    *out = buf;
    *out_len = len;
    return RDB_OK;
}

static int rdb_save_vector_section_snapshot(
    FILE *fp,
    const kvs_rdb_snapshot_t *snapshot
) {
    const kvs_rdb_snapshot_vector_section_t *vector;
    uint8_t type = RDB_TYPE_VECTOR;
    uint8_t section_version = RDB_VECTOR_SECTION_VERSION;
    size_t i;

    if (!snapshot) {
        return RDB_ERR;
    }

    vector = &snapshot->vector_section;
    if (!vector->initialized) {
        return RDB_OK;
    }

    if (rdb_write_u8(fp, type) != RDB_OK) return RDB_ERR;
    if (rdb_write_u8(fp, section_version) != RDB_OK) return RDB_ERR;

    if (rdb_write_i32(fp, (int32_t)vector->metric_type) != RDB_OK) return RDB_ERR;
    if (rdb_write_i32(fp, (int32_t)vector->index_type) != RDB_OK) return RDB_ERR;
    if (rdb_write_u64(fp, vector->next_id) != RDB_OK) return RDB_ERR;
    if (rdb_write_u64(fp, (uint64_t)vector->record_count) != RDB_OK) return RDB_ERR;

    for (i = 0; i < vector->record_count; ++i) {
        const kvs_rdb_snapshot_vector_record_t *record = &vector->records[i];

        if (rdb_write_u64(fp, (uint64_t)record->record_index) != RDB_OK) return RDB_ERR;
        if (rdb_write_u8(fp, (uint8_t)record->state) != RDB_OK) return RDB_ERR;
        if (rdb_write_u8(fp, (uint8_t)record->deleted) != RDB_OK) return RDB_ERR;
        if (rdb_write_u64(fp, record->id) != RDB_OK) return RDB_ERR;

        if (rdb_write_blob(fp, record->question, (uint32_t)record->question_len) != RDB_OK) return RDB_ERR;
        if (rdb_write_blob(fp, record->answer, (uint32_t)record->answer_len) != RDB_OK) return RDB_ERR;
        if (rdb_write_blob(fp, record->scope, (uint32_t)record->scope_len) != RDB_OK) return RDB_ERR;
        if (rdb_write_blob(fp, record->normalized_question, (uint32_t)record->normalized_question_len) != RDB_OK) return RDB_ERR;

        if (rdb_write_i32(fp, (int32_t)record->embedding_dim) != RDB_OK) return RDB_ERR;
        if (record->embedding && record->embedding_dim > 0) {
            size_t embedding_bytes = (size_t)record->embedding_dim * sizeof(float);
            if (rdb_write_exact(fp, record->embedding, embedding_bytes) != RDB_OK) return RDB_ERR;
        }

        if (rdb_write_i64(fp, record->created_at_ms) != RDB_OK) return RDB_ERR;
        if (rdb_write_i64(fp, record->updated_at_ms) != RDB_OK) return RDB_ERR;
    }

    KVS_LOG_INFO("persistence", "RDB vector section saved: records=%llu active=%zu deleted=%zu",
                 (unsigned long long)vector->record_count,
                 vector->active_records,
                 vector->deleted_records);

    return RDB_OK;
}

static int rdb_load_vector_section(FILE *fp) {
    uint8_t section_version;
    int32_t metric_type_from_rdb;
    int32_t index_type_from_rdb;
    uint64_t next_id;
    uint64_t record_count;
    uint64_t i;
    char *question = NULL;
    char *answer = NULL;
    char *scope = NULL;
    char *normalized_question = NULL;
    float *embedding = NULL;
    int ret;

    if (rdb_read_u8(fp, &section_version) != RDB_OK) {
        KVS_LOG_ERROR("persistence", "RDB vector load failed: unable to read section version");
        return RDB_ERR;
    }

    if (section_version != RDB_VECTOR_SECTION_VERSION) {
        KVS_LOG_ERROR("persistence", "RDB vector load failed: unsupported section version %u", section_version);
        return RDB_ERR;
    }

    if (rdb_read_i32(fp, &metric_type_from_rdb) != RDB_OK) {
        KVS_LOG_ERROR("persistence", "RDB vector load failed: unable to read metric_type");
        return RDB_ERR;
    }

    if (rdb_read_i32(fp, &index_type_from_rdb) != RDB_OK) {
        KVS_LOG_ERROR("persistence", "RDB vector load failed: unable to read index_type");
        return RDB_ERR;
    }

    if (rdb_read_u64(fp, &next_id) != RDB_OK) {
        KVS_LOG_ERROR("persistence", "RDB vector load failed: unable to read next_id");
        return RDB_ERR;
    }

    if (rdb_read_u64(fp, &record_count) != RDB_OK) {
        KVS_LOG_ERROR("persistence", "RDB vector load failed: unable to read record_count");
        return RDB_ERR;
    }

    KVS_LOG_INFO("persistence", "RDB vector section: record_count=%llu next_id=%llu metric=%d index=%d",
                 (unsigned long long)record_count,
                 (unsigned long long)next_id,
                 metric_type_from_rdb,
                 index_type_from_rdb);

    (void)metric_type_from_rdb;
    (void)index_type_from_rdb;

    ret = kvs_vector_reset_for_load();
    if (ret != KVS_VECTOR_OK) {
        KVS_LOG_ERROR("persistence", "RDB vector load failed: kvs_vector_reset_for_load ret=%d", ret);
        return RDB_ERR;
    }

    for (i = 0; i < record_count; ++i) {
        uint64_t record_index;
        uint8_t state_u8;
        uint8_t deleted_u8;
        uint64_t record_id;
        uint32_t question_len = 0;
        uint32_t answer_len = 0;
        uint32_t scope_len = 0;
        uint32_t normalized_question_len = 0;
        int32_t embedding_dim;
        int64_t created_at_ms;
        int64_t updated_at_ms;

        question = NULL;
        answer = NULL;
        scope = NULL;
        normalized_question = NULL;
        embedding = NULL;

        if (rdb_read_u64(fp, &record_index) != RDB_OK) goto load_error;
        if (rdb_read_u8(fp, &state_u8) != RDB_OK) goto load_error;
        if (rdb_read_u8(fp, &deleted_u8) != RDB_OK) goto load_error;
        if (rdb_read_u64(fp, &record_id) != RDB_OK) goto load_error;

        if (rdb_read_blob(fp, &question, &question_len) != RDB_OK) goto load_error;
        if (rdb_read_blob(fp, &answer, &answer_len) != RDB_OK) goto load_error;
        if (rdb_read_blob(fp, &scope, &scope_len) != RDB_OK) goto load_error;
        if (rdb_read_blob(fp, &normalized_question, &normalized_question_len) != RDB_OK) goto load_error;

        if (rdb_read_i32(fp, &embedding_dim) != RDB_OK) goto load_error;
        if (embedding_dim > 0) {
            size_t embedding_bytes = (size_t)embedding_dim * sizeof(float);
            embedding = (float *)kvs_malloc(embedding_bytes);
            if (!embedding) goto load_error;
            if (rdb_read_exact(fp, embedding, embedding_bytes) != RDB_OK) {
                kvs_free(embedding);
                goto load_error;
            }
        }

        if (rdb_read_i64(fp, &created_at_ms) != RDB_OK) {
            if (embedding) kvs_free(embedding);
            goto load_error;
        }
        if (rdb_read_i64(fp, &updated_at_ms) != RDB_OK) {
            if (embedding) kvs_free(embedding);
            goto load_error;
        }

        ret = kvs_vector_restore_record_with_embedding(
            (size_t)record_index,
            record_id,
            (int)state_u8,
            (int)deleted_u8,
            question, (size_t)question_len,
            answer, (size_t)answer_len,
            scope, (size_t)scope_len,
            normalized_question, (size_t)normalized_question_len,
            embedding, (int)embedding_dim,
            created_at_ms,
            updated_at_ms
        );

        if (question) kvs_free(question);
        if (answer) kvs_free(answer);
        if (scope) kvs_free(scope);
        if (normalized_question) kvs_free(normalized_question);
        if (embedding) kvs_free(embedding);

        if (ret != KVS_VECTOR_OK) {
            KVS_LOG_ERROR("persistence", "RDB vector load failed: restore record %llu ret=%d",
                         (unsigned long long)i, ret);
            return RDB_ERR;
        }
    }

    if (g_vector_state.store.next_id < next_id) {
        g_vector_state.store.next_id = next_id;
    }

    /*
     * metric_type/index_type are preserved from kvs_vector_init().
     * RDB values are read for diagnostic compatibility only.
     */

    ret = kvs_vector_rebuild_after_load();
    if (ret != KVS_VECTOR_OK) {
        KVS_LOG_ERROR("persistence", "RDB vector load failed: rebuild_after_load ret=%d", ret);
        return RDB_ERR;
    }

    KVS_LOG_INFO("persistence", "RDB vector section loaded: records=%llu active=%zu deleted=%zu",
                 (unsigned long long)record_count,
                 g_vector_state.store.active_records,
                 g_vector_state.store.deleted_records);

    return RDB_OK;

load_error:
    KVS_LOG_ERROR("persistence", "RDB vector load failed at record %llu", (unsigned long long)i);
    if (question) kvs_free(question);
    if (answer) kvs_free(answer);
    if (scope) kvs_free(scope);
    if (normalized_question) kvs_free(normalized_question);
    return RDB_ERR;
}

static void rdb_apply_expire_array(const char *key, int64_t expire_at_ms) {
#if ENABLE_ARRAY
    for (int i = 0; i < global_array.len; ++i) {
        if (global_array.table[i].key && global_array.table[i].key_len > 0 && memcmp(global_array.table[i].key, key, global_array.table[i].key_len) == 0) {
            global_array.table[i].expire_at_ms = expire_at_ms;
            return;
        }
    }
#endif
}

static void rdb_apply_expire_rbtree(const char *key, size_t key_len, int64_t expire_at_ms) {
#if ENABLE_RBTREE
    rbtree_node *node = kvs_rbtree_lookup_node(&global_rbtree, key, key_len);
    if (node && node != global_rbtree.nil) {
        node->expire_at_ms = expire_at_ms;
    }
#endif
}

static void rdb_apply_expire_hash(const char *key, int64_t expire_at_ms) {
#if ENABLE_HASH
    if (!global_hash.nodes) return;
    int idx = -1;
    if (kvs_hash_lookup_index(&global_hash, key, &idx) == 0 && idx >= 0 && idx < global_hash.max_slots) {
        global_hash.nodes[idx].expire_at_ms = expire_at_ms;
    }
#endif
}

static void rdb_apply_expire_skiptable(const char *key, int64_t expire_at_ms) {
#if ENABLE_SKIPTABLE
    if (!global_skiptable.header) return;
    Node *current = global_skiptable.header->forward[0];
    while (current) {
        if (current->key && current->key_len > 0 && memcmp(current->key, key, current->key_len) == 0) {
            current->expire_at_ms = expire_at_ms;
            return;
        }
        current = current->forward[0];
    }
#endif
}

//检查文件是否存在
static int file_exists(const char *filename){

	struct stat buffer;
	return (stat(filename, &buffer) == 0);

}

static int kvs_rdb_save_snapshot_file(
    const char *filename,
    const kvs_rdb_snapshot_t *snapshot
){
    char tmpname[1024];

    if (!filename || !snapshot) {
        return RDB_ERR;
    }

    // 构造临时文件名：<filename>.tmp
    if (snprintf(tmpname, sizeof(tmpname), "%s.tmp", filename) >= (int)sizeof(tmpname)) {
        KVS_LOG_ERROR("persistence", "RDB filename too long");
        return RDB_ERR;
    }

    FILE *fp = fopen(tmpname, "wb");
    if(!fp){
        KVS_LOG_ERROR_ERRNO("persistence", "failed to open tmp RDB file for writing: %s", tmpname);
        return RDB_ERR;
    }

    //写入魔数和版本
    if (rdb_write_exact(fp, RDB_MAGIC, strlen(RDB_MAGIC)) != RDB_OK) goto write_error;
    uint8_t version = RDB_VERSION;
    if (rdb_write_exact(fp, &version, sizeof(version)) != RDB_OK) goto write_error;

    //保存当前时间戳
    time_t timestamp = time(NULL);
    if (rdb_write_exact(fp, &timestamp, sizeof(timestamp)) != RDB_OK) goto write_error;

#if ENABLE_ARRAY
    if (rdb_save_array_snapshot(fp, snapshot) != RDB_OK) goto write_error;
#endif

#if ENABLE_RBTREE
    if (rdb_save_rbtree_snapshot(fp, snapshot) != RDB_OK) goto write_error;
#endif

#if ENABLE_HASH
    if (rdb_save_hash_snapshot(fp, snapshot) != RDB_OK) goto write_error;
#endif

#if ENABLE_SKIPTABLE
    if (rdb_save_skiptable_snapshot(fp, snapshot) != RDB_OK) goto write_error;
#endif

    if (rdb_save_vector_section_snapshot(fp, snapshot) != RDB_OK) goto write_error;

    //写入结束标识
    {
        uint8_t eof = 0xFF;
        if (fwrite(&eof, sizeof(eof), 1, fp) != 1) goto write_error;
    }

    // 确保用户空间缓冲 flush 到内核
    if (fflush(fp) == EOF) goto write_error;

    // fsync 保证写到磁盘
    int fd = fileno(fp);
    if (fd == -1) goto write_error;
    if (fsync(fd) == -1) {
        KVS_LOG_ERROR_ERRNO("persistence", "fsync RDB tmp file failed");
        goto write_error;
    }

    if (fclose(fp) == EOF) {
        fp = NULL;
        goto write_error_no_close; // 已经 close，无需再次 fclose
    }
    fp = NULL;

    // 原子替换：rename(tmp, filename)
    if (rename(tmpname, filename) != 0) {
        KVS_LOG_ERROR_ERRNO("persistence", "rename RDB tmp file failed: %s -> %s", tmpname, filename);
        unlink(tmpname); // 清理临时文件
        return RDB_ERR;
    }

    KVS_LOG_INFO("persistence", "RDB snapshot saved to %s", filename);
    return RDB_OK;

write_error:
    KVS_LOG_ERROR_ERRNO("persistence", "write RDB tmp file failed");
    fclose(fp);
    fp = NULL;
write_error_no_close:
    unlink(tmpname); // 写失败时删除半成品
    return RDB_ERR;
}

//保存rdb文件
int kvs_rdb_save(const char *filename){
    kvs_rdb_snapshot_t *snapshot = NULL;
    int ret;

    ret = kvs_command_executor_build_rdb_snapshot(&snapshot);
    if (ret != 0 || !snapshot) {
        KVS_LOG_ERROR("persistence", "failed to build RDB snapshot via executor");
        return RDB_ERR;
    }

    ret = kvs_rdb_save_snapshot_file(filename, snapshot);
    kvs_rdb_snapshot_destroy(snapshot);
    return ret;
}

//加载rdb文件
int kvs_rdb_load(const char *filename){
    if(!file_exists(filename)){
        KVS_LOG_INFO("persistence", "No RDB file found");
        return RDB_ERR;
    }

    FILE *fp = fopen(filename, "rb");
    if (!fp) {
        KVS_LOG_ERROR_ERRNO("persistence", "failed to open RDB file for reading: %s", filename);
        return RDB_ERR;
    }

    //检查魔数和版本 
    char magic[sizeof(RDB_MAGIC)];
    if (rdb_read_exact(fp, magic, sizeof(RDB_MAGIC) - 1) != RDB_OK) {
        KVS_LOG_ERROR("persistence", "failed to read RDB magic");
        fclose(fp);
        return RDB_ERR;
    }
    magic[sizeof(RDB_MAGIC) - 1] = '\0';

    if(strcmp(magic, RDB_MAGIC) != 0){
        KVS_LOG_ERROR("persistence", "invalid RDB file format");
        fclose(fp);
        return RDB_ERR;
    }

    uint8_t version;
    if (rdb_read_exact(fp, &version, sizeof(version)) != RDB_OK) {
        KVS_LOG_ERROR("persistence", "failed to read RDB version");
        fclose(fp);
        return RDB_ERR;
    }
    if(version != 1 && version != 2){
        KVS_LOG_ERROR("persistence", "unsupported RDB version: %d", version);
        fclose(fp);
        return RDB_ERR;
    }
    int64_t load_now_ms = kv_now_ms();

    //读取时间戳
    time_t timestamp;
    if (rdb_read_exact(fp, &timestamp, sizeof(timestamp)) != RDB_OK) {
        KVS_LOG_ERROR("persistence", "failed to read RDB timestamp");
        fclose(fp);
        return RDB_ERR;
    }
    KVS_LOG_INFO("persistence", "Loading RDB snapshot created at: %s", ctime(&timestamp));

    //清空当前数据
    #if ENABLE_ARRAY
    kvs_array_destory(&global_array);
    kvs_array_create(&global_array);
    #endif

    #if ENABLE_RBTREE
    kvs_rbtree_destory(&global_rbtree);
    kvs_rbtree_create(&global_rbtree);
    #endif
    
    #if ENABLE_HASH
    kvs_hash_destory(&global_hash);
    kvs_hash_create(&global_hash);
    #endif
    
    #if ENABLE_SKIPTABLE
    kvs_skiptable_destroy(&global_skiptable);
    kvs_skiptable_create(&global_skiptable);
    #endif

    //加载各数据结构
    uint8_t type;
    while(fread(&type, sizeof(type), 1, fp) == 1){
        if(type == 0xFF) break;//结束标记

        switch (type){
           #if ENABLE_ARRAY
            case 0x01:
                if (rdb_load_array(fp, version, load_now_ms) != RDB_OK) {
                    fclose(fp);
                    return RDB_ERR;
                }
                break;
           #endif
                
           #if ENABLE_RBTREE
            case 0x02:
                if (rdb_load_rbtree(fp, version, load_now_ms) != RDB_OK) {
                    fclose(fp);
                    return RDB_ERR;
                }
                break;
           #endif
                
           #if ENABLE_HASH
            case 0x03:
                if (rdb_load_hash(fp, version, load_now_ms) != RDB_OK) {
                    fclose(fp);
                    return RDB_ERR;
                }
                break;
           #endif
                
           #if ENABLE_SKIPTABLE
            case 0x04:
                if (rdb_load_skiptable(fp, version, load_now_ms) != RDB_OK) {
                    fclose(fp);
                    return RDB_ERR;
                }
                break; 
           #endif

           case 0x05:
                if (rdb_load_vector_section(fp) != RDB_OK) {
                    fclose(fp);
                    return RDB_ERR;
                }
                break;

           default:
             KVS_LOG_ERROR("persistence", "Unknown RDB data type: %d", type);
                fclose(fp);
                return RDB_ERR;
        }
    }

    fclose(fp);
    return RDB_OK;

}

 

static int rdb_save_snapshot_entries(
    FILE *fp,
    uint8_t type,
    const kvs_rdb_snapshot_kv_entry_t *entries,
    size_t count
) {
    uint32_t count_u32;
    size_t i;

    if (!fp) {
        return RDB_ERR;
    }
    if (count > UINT32_MAX) {
        return RDB_ERR;
    }
    count_u32 = (uint32_t)count;
    if (rdb_write_u8(fp, type) != RDB_OK) return RDB_ERR;
    if (rdb_write_exact(fp, &count_u32, sizeof(count_u32)) != RDB_OK) return RDB_ERR;
    for (i = 0; i < count; ++i) {
        uint32_t key_len = (uint32_t)entries[i].key_len;
        uint32_t value_len = (uint32_t)entries[i].value_len;

        if (rdb_write_exact(fp, &key_len, sizeof(key_len)) != RDB_OK) return RDB_ERR;
        if (key_len > 0 && rdb_write_exact(fp, entries[i].key, key_len) != RDB_OK) return RDB_ERR;
        if (rdb_write_exact(fp, &value_len, sizeof(value_len)) != RDB_OK) return RDB_ERR;
        if (value_len > 0 && rdb_write_exact(fp, entries[i].value, value_len) != RDB_OK) return RDB_ERR;
#if RDB_VERSION >= 2
        if (rdb_write_exact(fp, &entries[i].expire_at_ms, sizeof(int64_t)) != RDB_OK) return RDB_ERR;
#endif
    }
    return RDB_OK;
}

//保存数组
static int rdb_save_array_snapshot(FILE *fp, const kvs_rdb_snapshot_t *snapshot){
    if (!snapshot) {
        return RDB_ERR;
    }
    return rdb_save_snapshot_entries(
        fp,
        RDB_TYPE_ARRAY,
        snapshot->array_entries,
        snapshot->array_count
    );
}

//加载数组
static int rdb_load_array(FILE *fp, uint8_t version, int64_t now_ms){
    uint32_t count;
    if (rdb_read_exact(fp, &count, sizeof(count)) != RDB_OK) {
        KVS_LOG_ERROR("persistence", "RDB array load failed: unable to read count");
        return RDB_ERR;
    }

    for (uint32_t i = 0; i < count; i++) {
        uint32_t key_len, value_len;

        if (rdb_read_exact(fp, &key_len, sizeof(key_len)) != RDB_OK) {
            KVS_LOG_ERROR("persistence", "RDB array load failed: unable to read key_len at index %u", i);
            return RDB_ERR;
        }
        char *key = kvs_malloc(key_len + 1);
        if (!key) {
            KVS_LOG_ERROR("persistence", "RDB array load failed: key alloc failed at index %u", i);
            return RDB_ERR;
        }
        if (rdb_read_exact(fp, key, key_len) != RDB_OK) {
            KVS_LOG_ERROR("persistence", "RDB array load failed: unable to read key at index %u", i);
            kvs_free(key);
            return RDB_ERR;
        }
        key[key_len] = '\0';

        if (rdb_read_exact(fp, &value_len, sizeof(value_len)) != RDB_OK) {
            KVS_LOG_ERROR("persistence", "RDB array load failed: unable to read value_len at index %u", i);
            kvs_free(key);
            return RDB_ERR;
        }
        char *value = kvs_malloc(value_len + 1);
        if (!value) {
            KVS_LOG_ERROR("persistence", "RDB array load failed: value alloc failed at index %u", i);
            kvs_free(key);
            return RDB_ERR;
        }
        if (rdb_read_exact(fp, value, value_len) != RDB_OK) {
            KVS_LOG_ERROR("persistence", "RDB array load failed: unable to read value at index %u", i);
            kvs_free(key);
            kvs_free(value);
            return RDB_ERR;
        }
        value[value_len] = '\0';

        int64_t expire_at_ms = KV_TTL_NO_EXPIRE;
        if (version >= 2) {
            if (rdb_read_int64(fp, &expire_at_ms) != RDB_OK) {
                KVS_LOG_ERROR("persistence", "RDB array load failed: unable to read expire_at_ms at index %u", i);
                kvs_free(key);
                kvs_free(value);
                return RDB_ERR;
            }
        }

        if (kv_is_expired(expire_at_ms, now_ms)) {
            kvs_free(key);
            kvs_free(value);
            continue;
        }

        int set_ret = kvs_array_set(&global_array, key, value);
        kvs_free(key);
        kvs_free(value);

        if (set_ret < 0) {
            KVS_LOG_ERROR("persistence", "RDB array load failed: kvs_array_set ret=%d at index %u", set_ret, i);
            return RDB_ERR;
        }

        if (global_array.len > 0) {
            global_array.table[global_array.len - 1].expire_at_ms = expire_at_ms;
        }

        rdb_apply_expire_array(key, expire_at_ms);
    }

    return RDB_OK;
}

//保存红黑树
static int rdb_save_rbtree_snapshot(FILE *fp, const kvs_rdb_snapshot_t *snapshot){
    if (!snapshot) {
        return RDB_ERR;
    }
    return rdb_save_snapshot_entries(
        fp,
        RDB_TYPE_RBTREE,
        snapshot->rbtree_entries,
        snapshot->rbtree_count
    );
}

//红黑树加载实现
static int rdb_load_rbtree(FILE *fp, uint8_t version, int64_t now_ms){
    uint32_t count;
    if (rdb_read_exact(fp, &count, sizeof(count)) != RDB_OK) {
        KVS_LOG_ERROR("persistence", "RDB rbtree load failed: unable to read count");
        return RDB_ERR;
    }

    for (uint32_t i = 0; i < count; i++){
        uint32_t key_len, value_len;
        if (rdb_read_exact(fp, &key_len, sizeof(key_len)) != RDB_OK) {
            KVS_LOG_ERROR("persistence", "RDB rbtree load failed: unable to read key_len at index %u", i);
            return RDB_ERR;
        }
        char *key = kvs_malloc(key_len + 1);
        if (!key) return RDB_ERR;
        if (rdb_read_exact(fp, key, key_len) != RDB_OK) {
            kvs_free(key);
            return RDB_ERR;
        }
        key[key_len] = '\0';

        if (rdb_read_exact(fp, &value_len, sizeof(value_len)) != RDB_OK) {
            kvs_free(key);
            return RDB_ERR;
        }
        char *value = kvs_malloc(value_len + 1);
        if (!value) {
            kvs_free(key);
            return RDB_ERR;
        }
        if (rdb_read_exact(fp, value, value_len) != RDB_OK) {
            kvs_free(key);
            kvs_free(value);
            return RDB_ERR;
        }
        value[value_len] = '\0';

        int64_t expire_at_ms = KV_TTL_NO_EXPIRE;
        if (version >= 2) {
            if (rdb_read_int64(fp, &expire_at_ms) != RDB_OK) {
                kvs_free(key);
                kvs_free(value);
                return RDB_ERR;
            }
        }

        if (kv_is_expired(expire_at_ms, now_ms)) {
            kvs_free(key);
            kvs_free(value);
            continue;
        }

        int set_ret = kvs_rbtree_set(&global_rbtree, key, value);
        if (set_ret < 0) {
            kvs_free(key);
            kvs_free(value);
            return RDB_ERR;
        }

        rdb_apply_expire_rbtree(key, key_len, expire_at_ms);

        kvs_free(key);
        kvs_free(value);
    }

    return RDB_OK;
}

//哈希表实现
static int rdb_save_hash_snapshot(FILE *fp, const kvs_rdb_snapshot_t *snapshot){
    if (!snapshot) {
        return RDB_ERR;
    }
    return rdb_save_snapshot_entries(
        fp,
        RDB_TYPE_HASH,
        snapshot->hash_entries,
        snapshot->hash_count
    );
}

//哈希表加载
static int rdb_load_hash(FILE *fp, uint8_t version, int64_t now_ms){
    uint32_t count;
    if (rdb_read_exact(fp, &count, sizeof(count)) != RDB_OK) {
        KVS_LOG_ERROR("rdb", "RDB hash load failed: unable to read count");
        return RDB_ERR;
    }
    for (uint32_t i = 0; i < count; i++){
        uint32_t key_len, value_len;

        if (rdb_read_exact(fp, &key_len, sizeof(key_len)) != RDB_OK) {
            return RDB_ERR;
        }
        char *key = kvs_malloc(key_len + 1);
        if (!key) return RDB_ERR;
        if (rdb_read_exact(fp, key, key_len) != RDB_OK) {
            kvs_free(key);
            return RDB_ERR;
        }
        key[key_len] = '\0';

        if (rdb_read_exact(fp, &value_len, sizeof(value_len)) != RDB_OK) {
            kvs_free(key);
            return RDB_ERR;
        }
        char *value = kvs_malloc(value_len + 1);
        if (!value) {
            kvs_free(key);
            return RDB_ERR;
        }
        if (rdb_read_exact(fp, value, value_len) != RDB_OK) {
            kvs_free(key);
            kvs_free(value);
            return RDB_ERR;
        }
        value[value_len] = '\0';

        int64_t expire_at_ms = KV_TTL_NO_EXPIRE;
        if (version >= 2) {
            if (rdb_read_int64(fp, &expire_at_ms) != RDB_OK) {
                kvs_free(key);
                kvs_free(value);
                return RDB_ERR;
            }
        }

        if (kv_is_expired(expire_at_ms, now_ms)) {
            kvs_free(key);
            kvs_free(value);
            continue;
        }

        int set_ret = kvs_hash_set(&global_hash, key, value);
        if (set_ret < 0) {
            kvs_free(key);
            kvs_free(value);
            return RDB_ERR;
        }

        rdb_apply_expire_hash(key, expire_at_ms);

        kvs_free(key);
        kvs_free(value);
    }

    return RDB_OK;
}

//跳表保存
static int rdb_save_skiptable_snapshot(FILE *fp, const kvs_rdb_snapshot_t *snapshot){
    if (!snapshot) {
        return RDB_ERR;
    }
    return rdb_save_snapshot_entries(
        fp,
        RDB_TYPE_SKIPTABLE,
        snapshot->skiptable_entries,
        snapshot->skiptable_count
    );
}

//跳表加载
static int rdb_load_skiptable(FILE *fp, uint8_t version, int64_t now_ms){
    uint32_t count;
    if (rdb_read_exact(fp, &count, sizeof(count)) != RDB_OK) {
        return RDB_ERR;
    }

    for (uint32_t i = 0; i < count; i++){
        uint32_t key_len, value_len;

        if (rdb_read_exact(fp, &key_len, sizeof(key_len)) != RDB_OK) {
            return RDB_ERR;
        }
        char *key = kvs_malloc(key_len + 1);
        if (!key) return RDB_ERR;
        if (rdb_read_exact(fp, key, key_len) != RDB_OK) {
            kvs_free(key);
            return RDB_ERR;
        }
        key[key_len] = '\0';

        if (rdb_read_exact(fp, &value_len, sizeof(value_len)) != RDB_OK) {
            kvs_free(key);
            return RDB_ERR;
        }
        char *value = kvs_malloc(value_len + 1);
        if (!value) {
            kvs_free(key);
            return RDB_ERR;
        }
        if (rdb_read_exact(fp, value, value_len) != RDB_OK) {
            kvs_free(key);
            kvs_free(value);
            return RDB_ERR;
        }
        value[value_len] = '\0';

        int64_t expire_at_ms = KV_TTL_NO_EXPIRE;
        if (version >= 2) {
            if (rdb_read_int64(fp, &expire_at_ms) != RDB_OK) {
                kvs_free(key);
                kvs_free(value);
                return RDB_ERR;
            }
        }

        if (kv_is_expired(expire_at_ms, now_ms)) {
            kvs_free(key);
            kvs_free(value);
            continue;
        }

        int set_ret = kvs_skiptable_set(&global_skiptable, key, value);
        if (set_ret < 0) {
            kvs_free(key);
            kvs_free(value);
            return RDB_ERR;
        }

        rdb_apply_expire_skiptable(key, expire_at_ms);
        
        kvs_free(key);
        kvs_free(value);
    }

    return RDB_OK;
}
