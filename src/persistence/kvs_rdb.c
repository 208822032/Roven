#include "kvstore.h"
#include <stdio.h>
#include <string.h>
#include <time.h>
#include <stdint.h>
#include "kvs_memory.h"
#include "kvs_log.h"

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
static void rdb_save_array(FILE *fp, int64_t now_ms);
static int rdb_load_array(FILE *fp, uint8_t version, int64_t now_ms);
static void rdb_save_rbtree(FILE *fp, int64_t now_ms);
static int rdb_load_rbtree(FILE *fp, uint8_t version, int64_t now_ms);
static void rdb_save_hash(FILE *fp, int64_t now_ms);
static int rdb_load_hash(FILE *fp, uint8_t version, int64_t now_ms);
static void rdb_save_skiptable(FILE *fp, int64_t now_ms);
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

static void rdb_apply_expire_array(const char *key, int64_t expire_at_ms) {
#if ENABLE_ARRAY
    for (int i = 0; i < global_array.len; ++i) {
        if (global_array.table[i].key && strcmp(global_array.table[i].key, key) == 0) {
            global_array.table[i].expire_at_ms = expire_at_ms;
            return;
        }
    }
#endif
}

static void rdb_apply_expire_rbtree(const char *key, int64_t expire_at_ms) {
#if ENABLE_RBTREE
    rbtree_node *node = rbtree_search(&global_rbtree, (char *)key);
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
        if (current->key && strcmp(current->key, key) == 0) {
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

//保存rdb文件
int kvs_rdb_save(const char *filename){
    char tmpname[1024];

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

    //保存各数据结构
    int64_t now_ms = kv_now_ms();

#if ENABLE_ARRAY

    rdb_save_array(fp, now_ms);
#endif

#if ENABLE_RBTREE
    rdb_save_rbtree(fp, now_ms);
#endif

#if ENABLE_HASH
    rdb_save_hash(fp, now_ms);
#endif

#if ENABLE_SKIPTABLE
    rdb_save_skiptable(fp, now_ms);
#endif

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

           default:
             KVS_LOG_ERROR("persistence", "Unknown RDB data type: %d", type);
                fclose(fp);
                return RDB_ERR;
        }
    }

    fclose(fp);
    return RDB_OK;

}

 

//保存数组
static void rdb_save_array(FILE *fp, int64_t now_ms){
    uint8_t type = 0x01; // 数据类型标识符
    fwrite(&type, sizeof(type), 1, fp);

    long count_pos = ftell(fp);
    uint32_t count = 0;
    fwrite(&count, sizeof(count), 1, fp);
    for (int i = 0; i < global_array.len; i++) {
        if (!global_array.table[i].key || !global_array.table[i].value) continue;
        if (kv_is_expired(global_array.table[i].expire_at_ms, now_ms)) continue;

        uint32_t key_len   = (uint32_t)strlen(global_array.table[i].key);
        uint32_t value_len = (uint32_t)strlen(global_array.table[i].value);

        fwrite(&key_len, sizeof(key_len), 1, fp);
        fwrite(global_array.table[i].key, key_len, 1, fp);
        fwrite(&value_len, sizeof(value_len), 1, fp);
        fwrite(global_array.table[i].value, value_len, 1, fp);

#if RDB_VERSION >= 2
        fwrite(&global_array.table[i].expire_at_ms, sizeof(int64_t), 1, fp);
#endif

        count++;
    }

    long current_pos = ftell(fp);
    fseek(fp, count_pos, SEEK_SET);
    fwrite(&count, sizeof(count), 1, fp);
    fseek(fp, current_pos, SEEK_SET);

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

static void rdb_save_rbtree_node(rbtree_node *node,
                                 rbtree *T,
                                 FILE *fp,
                                 uint32_t *count,
                                 int64_t now_ms)
{
    if (node == T->nil) return;

    rdb_save_rbtree_node(node->left, T, fp, count, now_ms);

    if (kv_is_expired(node->expire_at_ms, now_ms)) {
        rdb_save_rbtree_node(node->right, T, fp, count, now_ms);
        return;
    }

    uint32_t key_len   = (uint32_t)node->key_len;
    uint32_t value_len = (uint32_t)node->value_len;

    fwrite(&key_len,   sizeof(key_len),   1, fp);
    fwrite(node->key,  key_len,          1, fp);
    fwrite(&value_len, sizeof(value_len),1, fp);
    fwrite(node->value,value_len,        1, fp);
#if RDB_VERSION >= 2
    fwrite(&node->expire_at_ms, sizeof(int64_t), 1, fp);
#endif

    (*count)++;

    rdb_save_rbtree_node(node->right, T, fp, count, now_ms);
}

//保存红黑树
static void rdb_save_rbtree(FILE *fp, int64_t now_ms){
    uint8_t type = 0x02;//红黑树类型
    fwrite(&type, sizeof(type), 1, fp);

    // 先占位写入 0，稍后回填
    long count_pos = ftell(fp);
    uint32_t count = 0;
    fwrite(&count, sizeof(count), 1, fp);

    // 中序遍历保存所有节点
    rdb_save_rbtree_node(global_rbtree.root, &global_rbtree, fp, &count, now_ms);

    // 回写实际数量
    long current_pos = ftell(fp);
    fseek(fp, count_pos, SEEK_SET);
    fwrite(&count, sizeof(count), 1, fp);
    fseek(fp, current_pos, SEEK_SET);

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

        rdb_apply_expire_rbtree(key, expire_at_ms);

        kvs_free(key);
        kvs_free(value);
    }

    return RDB_OK;
}

//哈希表实现
static void rdb_save_hash(FILE *fp, int64_t now_ms){
    uint8_t type = 0x03; // 哈希表类型标识
    fwrite(&type, sizeof(type), 1, fp);

    if (!global_hash.nodes || global_hash.max_slots == 0) { // 防御
        uint32_t zero = 0;
        fwrite(&zero, sizeof(zero), 1, fp);
        return;
    }

    uint32_t count = 0;
    long count_pos = ftell(fp);
    fwrite(&count, sizeof(count), 1, fp); // 占位

    for (int i = 0; i < global_hash.max_slots; i++) {
        hashnode_t *slot = &global_hash.nodes[i];
        if (slot->used != 1) continue; // 只保存有效元素
        if (kv_is_expired(slot->expire_at_ms, now_ms)) continue;

        uint32_t key_len   = (uint32_t)slot->key_len;
        uint32_t value_len = (uint32_t)slot->value_len;

        fwrite(&key_len, sizeof(key_len), 1, fp);
        fwrite(slot->key, key_len, 1, fp);
        fwrite(&value_len, sizeof(value_len), 1, fp);
        fwrite(slot->value, value_len, 1, fp);
#if RDB_VERSION >= 2
        fwrite(&slot->expire_at_ms, sizeof(int64_t), 1, fp);
#endif

        count++;
    }


    // 回写实际数量
    long current_pos = ftell(fp);
    fseek(fp, count_pos, SEEK_SET);
    fwrite(&count, sizeof(count), 1, fp);
    fseek(fp, current_pos, SEEK_SET);

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
static void rdb_save_skiptable(FILE *fp, int64_t now_ms){
    uint8_t type = 0x04; // 跳表类型标识
    fwrite(&type, sizeof(type), 1, fp);

    long count_pos = ftell(fp);
    uint32_t count = 0;
    fwrite(&count, sizeof(count), 1, fp);

    Node *current = global_skiptable.header->forward[0];
    while (current) {
        if (!current->key || !current->value || kv_is_expired(current->expire_at_ms, now_ms)) {
            current = current->forward[0];
            continue;
        }
        uint32_t key_len = current->key_len;
        uint32_t value_len = current->value_len;
        
        fwrite(&key_len, sizeof(key_len), 1, fp);
        fwrite(current->key, key_len, 1, fp);
        fwrite(&value_len, sizeof(value_len), 1, fp);
        fwrite(current->value, value_len, 1, fp);
#if RDB_VERSION >= 2
        fwrite(&current->expire_at_ms, sizeof(int64_t), 1, fp);
#endif
        count++;
        
        current = current->forward[0];
    }

    long current_pos = ftell(fp);
    fseek(fp, count_pos, SEEK_SET);
    fwrite(&count, sizeof(count), 1, fp);
    fseek(fp, current_pos, SEEK_SET);

    
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





