#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <fcntl.h>
#include <sys/stat.h>
#include <time.h>
#include "include/kvs_api.h"
#include "include/resp_protocol.h"

/*
gcc -o kvs_aof_test kvs_aof_test.c kvs_aof.c kvs_aof_rewrite.c kvs_operation.c kvstore.c kvs_array.c kvs_rbtree.c kvs_hash.c kvs_skiptable.c resp_protocol.c kvs_mempool.c distributed.c  -I. -lpthread -DENABLE_ARRAY=1 -DENABLE_RBTREE=1 -DENABLE_HASH=1 -DENABLE_SKIPTABLE=1 -DENABLE_AOF=1
*/

// 声明全局变量
#if ENABLE_ARRAY
kvs_array_t global_array;
#endif

#if ENABLE_RBTREE
kvs_rbtree_t global_rbtree;
#endif

#if ENABLE_HASH
kvs_hash_t global_hash;
#endif

#if ENABLE_SKIPTABLE
kvs_skiptable_t global_skiptable;
#endif

aof_state_t aof_state;

//读取AOF文件内容
char *read_aof_file(const char *filename){
    FILE *file = fopen(filename, "r");
    if(!file){
        perror("Failed to open AOF file");
        return NULL;
    }

    fseek(file, 0, SEEK_END);
    long length = ftell(file);
    fseek(file, 0, SEEK_SET);

    char *content = (char*)malloc(length + 1);
    if(!content){
        fclose(file);
        return NULL;
    }

    fread(content, 1, length, file);
    content[length] = '\0';

    fclose(file);
    return content;
}

//生成RESP格式命令
char *generate_resp_command(int argc, char **argv){
    //计算所需缓冲区大小
    size_t total_len = 0;
    total_len += snprintf(NULL, 0, "*%d\r\n", argc);

    for(int i = 0;i < argc;i++){
        total_len += snprintf(NULL, 0, "$%zu\r\n%s\r\n", strlen(argv[i]), argv[i]);
    }

    //分配缓冲区并构建命令
    char *cmd = (char *)malloc(total_len + 1);
    if(!cmd) return NULL;

    int pos = 0;
    pos += sprintf(cmd + pos, "*%d\r\n", argc);

    for(int i = 0;i < argc;i ++){
        pos += sprintf(cmd + pos, "$%zu\r\n%s\r\n", strlen(argv[i]), argv[i]);
    }

    return cmd;
}

//测试AOF基本功能
void test_aof_basic(){
    printf("=== Testing AOF Basic Functionality ===\n");

    //初始化AOF
    if(aof_init("test.aof", AOF_FSYNC_EVERYSEC) != 0){
        printf("FAIL: AOF initialization failed\n");
        return;
    }

    // 模拟一些命令操作
    char* set_cmd1[] = {"SET", "key1", "value1"};
    char* set_cmd2[] = {"SET", "key2", "value2"};
    char* del_cmd[] = {"DEL", "key2"};

    aof_append(3, set_cmd1);
    aof_append(3, set_cmd2);
    aof_append(2, del_cmd);

    // 强制同步到磁盘
    aof_fsync(1);

    //读取文件并验证内容
    char *aof_content = read_aof_file("test.aof");
    if(!aof_content){
        printf("FAIL: Could not read AOF file\n");
        aof_free();
        return;
    }

    //检查AOF文件是否包含预期的命令
    char *expected_set1 = generate_resp_command(3, set_cmd1);
    char *expected_set2 = generate_resp_command(3, set_cmd2);
    char *expected_del = generate_resp_command(2, del_cmd);

    if (strstr(aof_content, expected_set1) == NULL) {
        printf("FAIL: AOF does not contain first SET command\n");
    }else if (strstr(aof_content, expected_set2) == NULL) {
        printf("FAIL: AOF does not contain second SET command\n");
    }else if (strstr(aof_content, expected_del) == NULL) {
        printf("FAIL: AOF does not contain DEL command\n");
    }else if (strstr(aof_content, "GET") != NULL) {
        printf("FAIL: AOF should not contain GET command (read operation)\n");
    }else {
        printf("PASS: AOF basic functionality test\n");
    }

    free(expected_set1);
    free(expected_set2);
    free(expected_del);
    free(aof_content);

    // 清理
    aof_free();
    remove("test.aof");
}

//测试AOF重写功能
void test_aof_rewrite(){
    printf("\n=== Testing AOF Rewrite Functionality ===\n");

    //初始化AOF
    if (aof_init("test_rewrite.aof", AOF_FSYNC_EVERYSEC) != 0) {
        printf("FAIL: AOF initialization failed\n");
        return;
    }

// 添加一些数据到所有数据结构
#if ENABLE_ARRAY
    kvs_array_set(&global_array, "user1", "Alice");
    kvs_array_set(&global_array, "user2", "Bob");
    kvs_array_set(&global_array, "user3", "Charlie");
#endif

    // 模拟一些命令操作
    char* set_cmd1[] = {"SET", "key1", "value1"};
    kvs_array_set(&global_array, "key1", "value1");
    char* set_cmd2[] = {"SET", "key2", "value2"};
    kvs_array_set(&global_array, "key2", "value2");
    char* del_cmd[] = {"DEL", "key1"};
    kvs_array_del(&global_array, "key1");
    

    aof_append(3, set_cmd1);
    aof_append(3, set_cmd2);
    aof_append(2, del_cmd);
    
    // 强制同步到磁盘
    aof_fsync(1);

    // 手动触发重写
    aof_rewrite_background();

    // 读取重写后的AOF文件
    sleep(1);  // 等待重写完成
    char *aof_content = read_aof_file("test_rewrite.aof");
    if (!aof_content) {
        printf("FAIL: Could not read AOF file after rewrite\n");
        aof_free();
        return;
    }

    //检查重写后的AOF文件
    int has_user1 = (strstr(aof_content, "user1") != NULL);
    int has_user2 = (strstr(aof_content, "user2") != NULL);
    int has_user3 = (strstr(aof_content, "user3") != NULL);
    int has_key2 = (strstr(aof_content, "key2") != NULL);
    int has_key1 = (strstr(aof_content, "key1") != NULL);// 应该不存在，因为被删除了

    if (has_user1 && has_user2 && has_user3 && has_key2 && !has_key1) {
        printf("PASS: AOF rewrite test\n");
    } else {
        printf("FAIL: AOF rewrite did not produce expected result\n");
        printf("user:1: %s, user:2: %s, user:3: %s, key2: %s, key1: %s (should be absent)\n",
               has_user1 ? "YES" : "NO",
               has_user2 ? "YES" : "NO",
               has_user3 ? "YES" : "NO",
               has_key2 ? "YES" : "NO",
               has_key1 ? "YES" : "NO");
    }

    free(aof_content);

    // 清理
    aof_free();
//     remove("test_rewrite.aof");
//     remove("test_rewrite.aof.tmp");
}

//测试AOF加载功能
void test_aof_load(){
    printf("\n=== Testing AOF Load Functionality ===\n");

    //创建测试AOF文件
    FILE *test_file = fopen("test_load.aof", "w");
    if(!test_file){
        printf("FAIL: Could not create test AOF file\n");
        return;
    }

    // 写入一些RESP格式的命令
    char* set_cmd1 = "*3\r\n$3\r\nSET\r\n$5\r\nkey01\r\n$7\r\nvalue01\r\n";
    char* set_cmd2 = "*3\r\n$3\r\nSET\r\n$5\r\nkey02\r\n$7\r\nvalue02\r\n";
    char* del_cmd = "*2\r\n$3\r\nDEL\r\n$5\r\nkey01\r\n";

    fwrite(set_cmd1, 1, strlen(set_cmd1), test_file);
    fwrite(set_cmd2, 1, strlen(set_cmd2), test_file);
    fwrite(del_cmd, 1, strlen(del_cmd), test_file);
    fclose(test_file);

    // 加载AOF文件
    int result = aof_load("test_load.aof");

    // 检查加载结果
    if (result == 0) {
#if ENABLE_ARRAY
        // 检查key02应该存在，key01应该不存在（被删除了）
        char* value = kvs_array_get(&global_array, "key02");
        int key01_exists = kvs_array_exist(&global_array, "key01");
        
        if (value && strcmp(value, "value02") == 0 && key01_exists != 0) {
            printf("PASS: AOF load test\n");
        } else {
            printf("FAIL: AOF load did not produce expected state\n");
            printf("key02 value: %s (expected: value02), key01 exists: %s\n",
                   value ? value : "NULL", key01_exists ? "YES" : "NO");
        }
#else
        printf("SKIP: Array support not enabled, cannot verify AOF load\n");
#endif
    } else {
        printf("FAIL: AOF load returned error\n");
    }

    remove("test_load.aof");
}

// 测试不同的同步模式
void test_aof_sync_modes() {
    printf("\n=== Testing AOF Sync Modes ===\n");
    
    // 测试AOF_FSYNC_ALWAYS模式
    printf("Testing AOF_FSYNC_ALWAYS mode...\n");
    if (aof_init("test_always.aof", AOF_FSYNC_ALWAYS) != 0) {
        printf("FAIL: AOF initialization with ALWAYS mode failed\n");
        return;
    }
    
    char* set_cmd[] = {"SET", "test_key", "test_value"};
    aof_append(3, set_cmd);
    
    // 检查是否立即同步（文件大小应该大于0）
    struct stat st;
    stat("test_always.aof", &st);
    if (st.st_size > 0) {
        printf("PASS: AOF_FSYNC_ALWAYS mode test\n");
    } else {
        printf("FAIL: AOF_FSYNC_ALWAYS mode did not sync immediately\n");
    }
    
    aof_free();
    remove("test_always.aof");

    //测试AOF_FSYNC_NO模式
    printf("Testing AOF_FSYNC_NO mode...\n");
    if (aof_init("test_no.aof", AOF_FSYNC_NO) != 0) {
        printf("FAIL: AOF initialization with NO mode failed\n");
        return;
    }
    
    kvs_array_set(&global_array, "test_key", "test_value");
    aof_append(3, set_cmd);
    
    // 检查是否没有同步（文件大小应该为0）
    stat("test_no.aof", &st);
    if (st.st_size == 0) {
        printf("PASS: AOF_FSYNC_NO mode test\n");
    } else {
        printf("FAIL: AOF_FSYNC_NO mode synced data\n");
    }
    
    // 手动同步后检查
    aof_fsync(1);
    stat("test_no.aof", &st);
    if (st.st_size > 0) {
        printf("PASS: AOF manual fsync test\n");
    } else {
        printf("FAIL: AOF manual fsync did not work\n");
    }
    
    aof_free();
    remove("test_no.aof");
}

int main() {
    printf("Starting AOF functionality tests...\n");
    
    // 初始化内存管理
    mem_init();

    #if ENABLE_ARRAY
        memset(&global_array, 0, sizeof(kvs_array_t));
        kvs_array_create(&global_array);
    #endif

    #if ENABLE_RBTREE
        memset(&global_rbtree, 0, sizeof(kvs_rbtree_t));
        kvs_rbtree_create(&global_rbtree);
    #endif

    #if ENABLE_HASH
        memset(&global_hash, 0, sizeof(kvs_hash_t));
        kvs_hash_create(&global_hash);
    #endif

    #if ENABLE_SKIPTABLE
        memset(&global_skiptable, 0, sizeof(kvs_skiptable_t));
        kvs_skiptable_create(&global_skiptable);
    #endif

    
    // 运行测试
    //test_aof_basic();
    test_aof_rewrite();
    //test_aof_load();
    //test_aof_sync_modes();

    #if ENABLE_ARRAY
        kvs_array_destory(&global_array);
    #endif
    #if ENABLE_RBTREE
        kvs_rbtree_destory(&global_rbtree);
    #endif
    #if ENABLE_HASH
        kvs_hash_destory(&global_hash);
    #endif

    #if ENABLE_SKIPTABLE
        kvs_skiptable_destroy(&global_skiptable);
    #endif
    
    // 清理内存管理
    mem_destroy();
    
    printf("\nAll AOF tests completed.\n");
    return 0;
}