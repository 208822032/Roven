#include "kvstore.h"

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

//检查文件是否存在
static int file_exists(const char *filename){

	struct stat buffer;
	return (stat(filename, &buffer) == 0);

}

//保存rdb文件
int kvs_rdb_save(const char *filename){
    FILE *fp = fopen(filename, "wb");
    if(!fp){
        perror("Failed to open RDB file for writing");
        return RDB_ERR;
    }

    //写入魔数和版本
    fwrite(RDB_MAGIC, strlen(RDB_MAGIC), 1, fp);
    uint8_t version = RDB_VERSION;
    fwrite(&version, sizeof(version), 1, fp);

    //保存当前时间戳
    time_t timestamp = time(NULL);
    fwrite(&timestamp, sizeof(timestamp), 1, fp);

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
    uint8_t eof = 0xFF;
    fwrite(&eof, sizeof(eof), 1, fp);

    fclose(fp);
    printf("RDB snapshot saved to %s\n", filename);
    return RDB_OK;
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
    fread(magic, sizeof(RDB_MAGIC) - 1, 1, fp);
    magic[sizeof(RDB_MAGIC) - 1] = '\0';

    if(strcmp(magic, RDB_MAGIC) != 0){
        fprintf(stderr, "Invalid RDB file format\n");
        fclose(fp);
        return RDB_ERR;
    }

    uint8_t version;
    fread(&version, sizeof(version), 1, fp);
    if(version != RDB_VERSION){
        fprintf(stderr, "Unsupported RDB version: %d\n", version);
        fclose(fp);
        return RESP_ERROR;
    }

    //读取时间戳
    time_t timestamp;
    fread(&timestamp, sizeof(timestamp), 1, fp);
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
                rdb_load_array(fp);
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

}

//保存数组
void rdb_save_array(FILE *fp){
    uint8_t type = 0x01;//数据类型标识符
    fwrite(&type, sizeof(type), 1, fp);

    uint32_t count = global_array.idx;
    fwrite(&count, sizeof(count), 1, fp);

    for(int i = 0;i < global_array.idx; i++){
        if(global_array.table[i].key && global_array.table[i].value){
            uint16_t key_len = strlen(global_array.table[i].key);
            uint16_t value_len = strlen(global_array.table[i].value);

            fwrite(&key_len, sizeof(key_len), 1, fp);
            fwrite(global_array.table[i].key, key_len, 1, fp);
            fwrite(&value_len, sizeof(value_len), 1, fp);
            fwrite(global_array.table[i].value, value_len, 1, fp);
        }
    }
}

//加载数组
void rdb_load_array(FILE *fp){
    uint32_t count;
    fread(&count, sizeof(count), 1, fp);

    for(uint32_t i = 0;i < count;i++){
        uint16_t key_len, value_len;
        fread(&key_len, sizeof(key_len), 1, fp);
        char *key = kvs_malloc(key_len + 1);
        fread(key, key_len, 1, fp);
        key[key_len] = '\0';

        fread(&value_len, sizeof(value_len), 1, fp);
        char *value = kvs_malloc(value_len + 1);
        fread(value, value_len, 1, fp);
        value[value_len] = '\0';

        kvs_array_set(&global_array, key, value);

        kvs_free(key);
        kvs_free(value);
    }
}

//保存红黑树
void rdb_save_rbtree(FILE *fp){
    uint8_t type = 0x02;//红黑树类型
    fwrite(&type, sizeof(type), 1, fp);

    //先计算节点数量
    //先全写入0 遍历时计数 最后写回数量
    long count_pos = ftell(fp);
    uint32_t count = 0;
    fwrite(&count, sizeof(count), 1, fp);

    //中序遍历保存
    void save_node(rbtree_node *node){
        if(node == global_rbtree.nil) return;

        save_node(node->left);

        uint16_t key_len = strlen(node->key);
        uint16_t value_len = strlen((char*)node->value);

        fwrite(&key_len, sizeof(key_len), 1, fp);
        fwrite(node->key, key_len, 1, fp);
        fwrite(&value_len, sizeof(value_len), 1, fp);
        fwrite(node->value, value_len, 1, fp);

        count++;

        save_node(node->right);
    }

    save_node(global_rbtree.root);

    //回写实际数量
    long current_pos = ftell(fp);
    fseek(fp, count_pos, SEEK_SET);
    fwrite(&count, sizeof(count), 1, fp);
    fseek(fp, current_pos, SEEK_SET);
}

//红黑树加载实现
void rdb_load_rbtree(FILE *fp){
    uint32_t count;
    fread(&count, sizeof(count), 1, fp);

    for (uint32_t i = 0; i < count; i++){
        uint16_t key_len, value_len;

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
void rdb_save_hash(FILE *fp){
    uint8_t type = 0x03; // 哈希表类型标识
    fwrite(&type, sizeof(type), 1, fp);

    uint32_t count = 0;
    long count_pos = ftell(fp);
    fwrite(&count, sizeof(count), 1, fp); // 占位

    for (int i = 0; i < global_hash.max_slots; i++){
        hashnode_t *node = global_hash.nodes[i];
        while (node) {
            uint16_t key_len = strlen(node->key);
            uint16_t value_len = strlen(node->value);
            
            fwrite(&key_len, sizeof(key_len), 1, fp);
            fwrite(node->key, key_len, 1, fp);
            fwrite(&value_len, sizeof(value_len), 1, fp);
            fwrite(node->value, value_len, 1, fp);
            
            count++;
            node = node->next;
        }
    }

    // 回写实际数量
    long current_pos = ftell(fp);
    fseek(fp, count_pos, SEEK_SET);
    fwrite(&count, sizeof(count), 1, fp);
    fseek(fp, current_pos, SEEK_SET);
}

//哈希表加载
void rdb_load_hash(FILE *fp){
    uint32_t count;
    fread(&count, sizeof(count), 1, fp);

    for (uint32_t i = 0; i < count; i++){
        uint16_t key_len, value_len;

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
void rdb_save_skiptable(FILE *fp){
    uint8_t type = 0x04; // 跳表类型标识
    fwrite(&type, sizeof(type), 1, fp);

    uint32_t count = global_skiptable.count;
    fwrite(&count, sizeof(count), 1, fp);

    Node *current = global_skiptable.header->forward[0];
    while (current) {
        uint16_t key_len = strlen(current->key);
        uint16_t value_len = strlen(current->value);
        
        fwrite(&key_len, sizeof(key_len), 1, fp);
        fwrite(current->key, key_len, 1, fp);
        fwrite(&value_len, sizeof(value_len), 1, fp);
        fwrite(current->value, value_len, 1, fp);
        
        current = current->forward[0];
    }
    
}

//跳表加载
void rdb_load_skiptable(FILE *fp){
    uint32_t count;
    fread(&count, sizeof(count), 1, fp);

    for (uint32_t i = 0; i < count; i++){
        uint16_t key_len, value_len;

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