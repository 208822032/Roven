#include "kvstore.h"
#include <stdio.h>
#include <string.h>
#include <time.h>
#include "kvs_memory.h"

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

// 内部函数前向声明（需在首次调用前声明以避免隐式声明）
static void rdb_save_array(FILE *fp);
static int rdb_load_array(FILE *fp);
static void rdb_save_rbtree(FILE *fp);
static void rdb_load_rbtree(FILE *fp);
static void rdb_save_hash(FILE *fp);
static void rdb_load_hash(FILE *fp);
static void rdb_save_skiptable(FILE *fp);
static void rdb_load_skiptable(FILE *fp);

static int rdb_read_exact(FILE *fp, void *buf, size_t size) {
    return fread(buf, size, 1, fp) == 1 ? RDB_OK : RDB_ERR;
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
        fprintf(stderr, "RDB filename too long\n");
        return RDB_ERR;
    }

    FILE *fp = fopen(tmpname, "wb");
    if(!fp){
        perror("Failed to open tmp RDB file for writing");
        return RDB_ERR;
    }

    //写入魔数和版本
    if (fwrite(RDB_MAGIC, strlen(RDB_MAGIC), 1, fp) != 1) goto write_error;
    uint8_t version = RDB_VERSION;
    if (fwrite(&version, sizeof(version), 1, fp) != 1) goto write_error;

    //保存当前时间戳
    time_t timestamp = time(NULL);
    if (fwrite(&timestamp, sizeof(timestamp), 1, fp) != 1) goto write_error;

    //保存各数据结构
#if ENABLE_ARRAY
    rdb_save_array(fp);
#endif

#if ENABLE_RBTREE
    rdb_save_rbtree(fp);
#endif

#if ENABLE_HASH
    rdb_save_hash(fp);
#endif

#if ENABLE_SKIPTABLE
    rdb_save_skiptable(fp);
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
        perror("fsync RDB tmp file failed");
        goto write_error;
    }

    if (fclose(fp) == EOF) {
        fp = NULL;
        goto write_error_no_close; // 已经 close，无需再次 fclose
    }
    fp = NULL;

    // 原子替换：rename(tmp, filename)
    if (rename(tmpname, filename) != 0) {
        perror("rename RDB tmp file failed");
        unlink(tmpname); // 清理临时文件
        return RDB_ERR;
    }

    printf("RDB snapshot saved to %s\n", filename);
    return RDB_OK;

write_error:
    perror("Write RDB tmp file failed");
    fclose(fp);
    fp = NULL;
write_error_no_close:
    unlink(tmpname); // 写失败时删除半成品
    return RDB_ERR;
}

//加载rdb文件
int kvs_rdb_load(const char *filename){
    if(!file_exists(filename)){
        printf("No RDB file found\n");
        return RDB_ERR;
    }

    FILE *fp = fopen(filename, "rb");
    if (!fp) {
        perror("Failed to open RDB file for reading");
        return RDB_ERR;
    }

    //检查魔数和版本 
    char magic[sizeof(RDB_MAGIC)];
    if (rdb_read_exact(fp, magic, sizeof(RDB_MAGIC) - 1) != RDB_OK) {
        fprintf(stderr, "Failed to read RDB magic\n");
        fclose(fp);
        return RDB_ERR;
    }
    magic[sizeof(RDB_MAGIC) - 1] = '\0';

    if(strcmp(magic, RDB_MAGIC) != 0){
        fprintf(stderr, "Invalid RDB file format\n");
        fclose(fp);
        return RDB_ERR;
    }

    uint8_t version;
    if (rdb_read_exact(fp, &version, sizeof(version)) != RDB_OK) {
        fprintf(stderr, "Failed to read RDB version\n");
        fclose(fp);
        return RDB_ERR;
    }
    if(version != RDB_VERSION){
        fprintf(stderr, "Unsupported RDB version: %d\n", version);
        fclose(fp);
        return RESP_ERROR;
    }

    //读取时间戳
    time_t timestamp;
    if (rdb_read_exact(fp, &timestamp, sizeof(timestamp)) != RDB_OK) {
        fprintf(stderr, "Failed to read RDB timestamp\n");
        fclose(fp);
        return RDB_ERR;
    }
    printf("Loading RDB snapshot created at: %s", ctime(&timestamp));

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
                if (rdb_load_array(fp) != RDB_OK) {
                    fclose(fp);
                    return RDB_ERR;
                }
                break;
           #endif
                
           #if ENABLE_RBTREE
            case 0x02:
                rdb_load_rbtree(fp);
                break;
           #endif
                
           #if ENABLE_HASH
            case 0x03:
                rdb_load_hash(fp);
                break;
           #endif
                
           #if ENABLE_SKIPTABLE
            case 0x04:
                rdb_load_skiptable(fp);
                break; 
           #endif

           default:
                fprintf(stderr, "Unknown RDB data type: %d\n", type);
                fclose(fp);
                return RDB_ERR;
        }
    }

    fclose(fp);
    return RDB_OK;

}

 

//保存数组
static void rdb_save_array(FILE *fp){
    uint8_t type = 0x01; // 数据类型标识符
    fwrite(&type, sizeof(type), 1, fp);

    // 使用 len 作为元素数量（有效元素个数）
    uint32_t count = (uint32_t)global_array.len;
    fwrite(&count, sizeof(count), 1, fp);
    for (int i = 0; i < global_array.len; i++) {
        uint32_t key_len   = (uint32_t)strlen(global_array.table[i].key);
        uint32_t value_len = (uint32_t)strlen(global_array.table[i].value);

        fwrite(&key_len, sizeof(key_len), 1, fp);
        fwrite(global_array.table[i].key, key_len, 1, fp);
        fwrite(&value_len, sizeof(value_len), 1, fp);
        fwrite(global_array.table[i].value, value_len, 1, fp);
    }

}

//加载数组
static int rdb_load_array(FILE *fp){
    uint32_t count;
    if (rdb_read_exact(fp, &count, sizeof(count)) != RDB_OK) {
        fprintf(stderr, "RDB array load failed: unable to read count\n");
        return RDB_ERR;
    }

    for (uint32_t i = 0; i < count; i++) {
        uint32_t key_len, value_len;

        if (rdb_read_exact(fp, &key_len, sizeof(key_len)) != RDB_OK) {
            fprintf(stderr, "RDB array load failed: unable to read key_len at index %u\n", i);
            return RDB_ERR;
        }
        char *key = kvs_malloc(key_len + 1);
        if (!key) {
            fprintf(stderr, "RDB array load failed: key alloc failed at index %u\n", i);
            return RDB_ERR;
        }
        if (rdb_read_exact(fp, key, key_len) != RDB_OK) {
            fprintf(stderr, "RDB array load failed: unable to read key at index %u\n", i);
            kvs_free(key);
            return RDB_ERR;
        }
        key[key_len] = '\0';

        if (rdb_read_exact(fp, &value_len, sizeof(value_len)) != RDB_OK) {
            fprintf(stderr, "RDB array load failed: unable to read value_len at index %u\n", i);
            kvs_free(key);
            return RDB_ERR;
        }
        char *value = kvs_malloc(value_len + 1);
        if (!value) {
            fprintf(stderr, "RDB array load failed: value alloc failed at index %u\n", i);
            kvs_free(key);
            return RDB_ERR;
        }
        if (rdb_read_exact(fp, value, value_len) != RDB_OK) {
            fprintf(stderr, "RDB array load failed: unable to read value at index %u\n", i);
            kvs_free(key);
            kvs_free(value);
            return RDB_ERR;
        }
        value[value_len] = '\0';

        int set_ret = kvs_array_set(&global_array, key, value);
        kvs_free(key);
        kvs_free(value);

        if (set_ret != 0) {
            fprintf(stderr, "RDB array load failed: kvs_array_set ret=%d at index %u\n", set_ret, i);
            return RDB_ERR;
        }
    }

    return RDB_OK;
}

static void rdb_save_rbtree_node(rbtree_node *node,
                                 rbtree *T,
                                 FILE *fp,
                                 uint32_t *count)
{
    if (node == T->nil) return;

    rdb_save_rbtree_node(node->left, T, fp, count);

    uint32_t key_len   = (uint32_t)strlen(node->key);
    uint32_t value_len = (uint32_t)strlen((char*)node->value);

    fwrite(&key_len,   sizeof(key_len),   1, fp);
    fwrite(node->key,  key_len,          1, fp);
    fwrite(&value_len, sizeof(value_len),1, fp);
    fwrite(node->value,value_len,        1, fp);

    (*count)++;

    rdb_save_rbtree_node(node->right, T, fp, count);
}

//保存红黑树
static void rdb_save_rbtree(FILE *fp){
    uint8_t type = 0x02;//红黑树类型
    fwrite(&type, sizeof(type), 1, fp);

    // 先占位写入 0，稍后回填
    long count_pos = ftell(fp);
    uint32_t count = 0;
    fwrite(&count, sizeof(count), 1, fp);

    // 中序遍历保存所有节点
    rdb_save_rbtree_node(global_rbtree.root, &global_rbtree, fp, &count);

    // 回写实际数量
    long current_pos = ftell(fp);
    fseek(fp, count_pos, SEEK_SET);
    fwrite(&count, sizeof(count), 1, fp);
    fseek(fp, current_pos, SEEK_SET);

}

//红黑树加载实现
static void rdb_load_rbtree(FILE *fp){
    uint32_t count;
    fread(&count, sizeof(count), 1, fp);

    for (uint32_t i = 0; i < count; i++){
        uint32_t key_len, value_len;
        fread(&key_len, sizeof(key_len), 1, fp);
        char *key = kvs_malloc(key_len + 1);
        fread(key, key_len, 1, fp);
        key[key_len] = '\0';

        fread(&value_len, sizeof(value_len), 1, fp);
        char *value = kvs_malloc(value_len + 1);
        fread(value, value_len, 1, fp);
        value[value_len] = '\0';

        kvs_rbtree_set(&global_rbtree, key, value);

        kvs_free(key);
        kvs_free(value);
    }
}

//哈希表实现
static void rdb_save_hash(FILE *fp){
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

        uint32_t key_len   = (uint32_t)strlen(slot->key);
        uint32_t value_len = (uint32_t)strlen(slot->value);

        fwrite(&key_len, sizeof(key_len), 1, fp);
        fwrite(slot->key, key_len, 1, fp);
        fwrite(&value_len, sizeof(value_len), 1, fp);
        fwrite(slot->value, value_len, 1, fp);

        count++;
    }


    // 回写实际数量
    long current_pos = ftell(fp);
    fseek(fp, count_pos, SEEK_SET);
    fwrite(&count, sizeof(count), 1, fp);
    fseek(fp, current_pos, SEEK_SET);

}

//哈希表加载
static void rdb_load_hash(FILE *fp){
    uint32_t count;
    fread(&count, sizeof(count), 1, fp);
    for (uint32_t i = 0; i < count; i++){
        uint32_t key_len, value_len;

        fread(&key_len, sizeof(key_len), 1, fp);
        char *key = kvs_malloc(key_len + 1);
        fread(key, key_len, 1, fp);
        key[key_len] = '\0';

        fread(&value_len, sizeof(value_len), 1, fp);
        char *value = kvs_malloc(value_len + 1);
        fread(value, value_len, 1, fp);
        value[value_len] = '\0';

        kvs_hash_set(&global_hash, key, value);

        kvs_free(key);
        kvs_free(value);
    }
}

//跳表保存
static void rdb_save_skiptable(FILE *fp){
    uint8_t type = 0x04; // 跳表类型标识
    fwrite(&type, sizeof(type), 1, fp);

    uint32_t count = global_skiptable.count;
    fwrite(&count, sizeof(count), 1, fp);

    Node *current = global_skiptable.header->forward[0];
    while (current) {
        uint32_t key_len = strlen(current->key);
        uint32_t value_len = strlen(current->value);
        
        fwrite(&key_len, sizeof(key_len), 1, fp);
        fwrite(current->key, key_len, 1, fp);
        fwrite(&value_len, sizeof(value_len), 1, fp);
        fwrite(current->value, value_len, 1, fp);
        
        current = current->forward[0];
    }

    
}

//跳表加载
static void rdb_load_skiptable(FILE *fp){
    uint32_t count;
    fread(&count, sizeof(count), 1, fp);

    for (uint32_t i = 0; i < count; i++){
        uint32_t key_len, value_len;

        fread(&key_len, sizeof(key_len), 1, fp);
        char *key = kvs_malloc(key_len + 1);
        fread(key, key_len, 1, fp);
        key[key_len] = '\0';

        fread(&value_len, sizeof(value_len), 1, fp);
        char *value = kvs_malloc(value_len + 1);
        fread(value, value_len, 1, fp);
        value[value_len] = '\0';

        kvs_skiptable_set(&global_skiptable, key, value);
        
        kvs_free(key);
        kvs_free(value);
    }
}





