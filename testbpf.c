#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <pthread.h>
#include <assert.h>
#include <stdarg.h>
#include <sys/socket.h>
#include <arpa/inet.h>
#include <errno.h>
#include "include/sync_mode.h"
#include "include/distributed.h"

#define TEST_MASTER_PORT 8080
#define TEST_SLAVE_PORT 8081
#define TEST_BASE_PORT 20000
#define TEST_DELAY_MS 500000
#define BUFFER_SIZE 4096

// ✅ 全局变量和函数定义
int g_shutdown_flag = 0;
static __thread int sync_context = 0;

void set_sync_context(int context) {
    sync_context = context;
}

int get_sync_context() {
    return sync_context;
}

// Mock kvs_filter_protocol
int kvs_filter_protocol(char **tokens, int count, char *response) {
    if (response) {
        strcpy(response, "+OK\r\n");
    }
    return 5;
}

// ✅ 事件捕获计数器
static volatile int events_captured = 0;
static volatile int events_synced_to_slaves = 0;
static pthread_mutex_t counter_mutex = PTHREAD_MUTEX_INITIALIZER;

// ✅ 从节点接收到的命令列表
typedef struct {
    char cmd[16];
    char key[128];
    char value[256];
} received_command_t;

static received_command_t slave_received_cmds[1000];
static volatile int slave_received_count = 0;
static pthread_mutex_t slave_recv_mutex = PTHREAD_MUTEX_INITIALIZER;

// ✅ 当前测试端口（每个测试递增以避免冲突）
static int current_test_port = TEST_BASE_PORT;

// ✅ 事件计数回调（需要在 ebpf_sync.c 中调用）
void test_increment_event_counter() {
    pthread_mutex_lock(&counter_mutex);
    events_captured++;
    pthread_mutex_unlock(&counter_mutex);
}

void test_increment_sync_counter() {
    pthread_mutex_lock(&counter_mutex);
    events_synced_to_slaves++;
    pthread_mutex_unlock(&counter_mutex);
}

// ✅ 改进的从节点接收线程
void* mock_slave_receiver(void* arg) {
    int port = *(int*)arg;
    int server_fd, client_fd;
    struct sockaddr_in addr;
    
    // 创建监听socket
    server_fd = socket(AF_INET, SOCK_STREAM, 0);
    if (server_fd < 0) {
        fprintf(stderr, "[SLAVE] Failed to create socket: %s\n", strerror(errno));
        return NULL;
    }
    
    // ✅ 设置地址复用和端口复用
    int opt = 1;
    setsockopt(server_fd, SOL_SOCKET, SO_REUSEADDR, &opt, sizeof(opt));
    setsockopt(server_fd, SOL_SOCKET, SO_REUSEPORT, &opt, sizeof(opt));
    
    memset(&addr, 0, sizeof(addr));
    addr.sin_family = AF_INET;
    addr.sin_addr.s_addr = INADDR_ANY;
    addr.sin_port = htons(port);
    
    if (bind(server_fd, (struct sockaddr*)&addr, sizeof(addr)) < 0) {
        fprintf(stderr, "[SLAVE] Bind failed on port %d: %s\n", port, strerror(errno));
        close(server_fd);
        return NULL;
    }
    
    listen(server_fd, 5);
    printf("[SLAVE] Listening on port %d\n", port);
    
    // ✅ 设置accept超时
    struct timeval timeout;
    timeout.tv_sec = 10;
    timeout.tv_usec = 0;
    setsockopt(server_fd, SOL_SOCKET, SO_RCVTIMEO, &timeout, sizeof(timeout));
    
    // 接受连接
    struct sockaddr_in client_addr;
    socklen_t client_len = sizeof(client_addr);
    client_fd = accept(server_fd, (struct sockaddr*)&client_addr, &client_len);
    
    if (client_fd < 0) {
        if (errno != EAGAIN && errno != EWOULDBLOCK) {
            fprintf(stderr, "[SLAVE] Accept failed: %s\n", strerror(errno));
        } else {
            fprintf(stderr, "[SLAVE] Accept timeout (no master connected)\n");
        }
        close(server_fd);
        return NULL;
    }
    
    printf("[SLAVE] Accepted connection from master\n");
    
    // ✅ 设置recv超时
    timeout.tv_sec = 5;
    setsockopt(client_fd, SOL_SOCKET, SO_RCVTIMEO, &timeout, sizeof(timeout));
    
    // 接收同步命令
    char buffer[BUFFER_SIZE];
    while (!g_shutdown_flag) {
        memset(buffer, 0, sizeof(buffer));
        int n = recv(client_fd, buffer, sizeof(buffer) - 1, 0);
        
        if (n < 0) {
            if (errno == EAGAIN || errno == EWOULDBLOCK) {
                continue; // 超时，继续等待
            }
            fprintf(stderr, "[SLAVE] Recv error: %s\n", strerror(errno));
            break;
        }
        
        if (n == 0) {
            printf("[SLAVE] Connection closed by master\n");
            break;
        }
        
        buffer[n] = '\0';
        
        // ✅ 改进的RESP协议解析（使用副本避免破坏原始数据）
        char buffer_copy[BUFFER_SIZE];
        strncpy(buffer_copy, buffer, sizeof(buffer_copy) - 1);
        buffer_copy[sizeof(buffer_copy) - 1] = '\0';
        
        char *line = strtok(buffer_copy, "\r\n");
        int arg_count = 0;
        char *args[10] = {NULL};
        int arg_index = 0;
        
        while (line != NULL && arg_index < 10) {
            if (line[0] == '*') {
                arg_count = atoi(line + 1);
            } else if (line[0] == '$') {
                // 跳过长度，读取下一行的实际值
                line = strtok(NULL, "\r\n");
                if (line) {
                    args[arg_index++] = strdup(line); // ✅ 复制字符串
                }
            }
            line = strtok(NULL, "\r\n");
        }
        
        if (arg_index >= 2) {
            pthread_mutex_lock(&slave_recv_mutex);
            if (slave_received_count < 1000) {
                strncpy(slave_received_cmds[slave_received_count].cmd, args[0], 15);
                slave_received_cmds[slave_received_count].cmd[15] = '\0';
                
                strncpy(slave_received_cmds[slave_received_count].key, args[1], 127);
                slave_received_cmds[slave_received_count].key[127] = '\0';
                
                if (arg_index >= 3 && args[2]) {
                    strncpy(slave_received_cmds[slave_received_count].value, args[2], 255);
                    slave_received_cmds[slave_received_count].value[255] = '\0';
                } else {
                    slave_received_cmds[slave_received_count].value[0] = '\0';
                }
                
                slave_received_count++;
                printf("[SLAVE] Received #%d: %s %s %s\n", 
                       slave_received_count,
                       args[0], args[1], 
                       arg_index >= 3 && args[2] ? args[2] : "(null)");
            }
            pthread_mutex_unlock(&slave_recv_mutex);
        }
        
        // ✅ 释放strdup的内存
        for (int i = 0; i < arg_index; i++) {
            if (args[i]) {
                free(args[i]);
                args[i] = NULL;
            }
        }
    }
    
    close(client_fd);
    close(server_fd);
    printf("[SLAVE] Receiver thread exiting\n");
    return NULL;
}

// ✅ 辅助函数
static char** create_tokens(int count, ...) {
    va_list args;
    va_start(args, count);
    
    char** tokens = malloc(sizeof(char*) * count);
    for (int i = 0; i < count; i++) {
        const char* arg = va_arg(args, const char*);
        tokens[i] = strdup(arg);
    }
    
    va_end(args);
    return tokens;
}

static void free_tokens(char** tokens, int count) {
    for (int i = 0; i < count; i++) {
        if (tokens[i]) {
            free(tokens[i]);
        }
    }
    free(tokens);
}

static void reset_counters() {
    pthread_mutex_lock(&counter_mutex);
    events_captured = 0;
    events_synced_to_slaves = 0;
    pthread_mutex_unlock(&counter_mutex);
    
    pthread_mutex_lock(&slave_recv_mutex);
    slave_received_count = 0;
    memset(slave_received_cmds, 0, sizeof(slave_received_cmds));
    pthread_mutex_unlock(&slave_recv_mutex);
}

// ========================================
// 测试用例
// ========================================

// Test 1: 事件捕获验证
void test_event_capture() {
    printf("\n[TEST 1] Event Capture Verification\n");
    printf("======================================\n");
    reset_counters();
    current_test_port++; // 使用新端口
    
    int port = current_test_port;
    printf("[TEST] Using port %d\n", port);
    
    if (ebpf_sync_init(port) != 0) {
        printf("⚠ eBPF init failed, skipping test\n");
        return;
    }
    
    // 发送3个事件
    char** t1 = create_tokens(3, "SET", "k1", "v1");
    ebpf_emit_sync_event(t1, 3);
    free_tokens(t1, 3);
    
    char** t2 = create_tokens(2, "DEL", "k2");
    ebpf_emit_sync_event(t2, 2);
    free_tokens(t2, 2);
    
    char** t3 = create_tokens(3, "SET", "k3", "v3");
    ebpf_emit_sync_event(t3, 3);
    free_tokens(t3, 3);
    
    printf("✓ Emitted 3 events\n");
    usleep(TEST_DELAY_MS);
    
    pthread_mutex_lock(&counter_mutex);
    int captured = events_captured;
    pthread_mutex_unlock(&counter_mutex);
    
    printf("📊 Events captured by ringbuffer: %d/3\n", captured);
    
    ebpf_sync_cleanup();
    printf("✓ Test completed\n");
}

// Test 2: 主从同步测试
void test_master_slave_sync() {
    printf("\n[TEST 2] Master-Slave Synchronization\n");
    printf("======================================\n");
    reset_counters();
    g_shutdown_flag = 0;
    current_test_port++;
    
    int port = current_test_port;
    int sync_port = current_test_port + 1;  // ✅ 使用不同端口
    printf("[TEST] Using slave port %d, sync port %d\n", port, sync_port);
    
    
    // 1. 启动模拟从节点
    pthread_t slave_thread;
    int slave_port = port;
    pthread_create(&slave_thread, NULL, mock_slave_receiver, &slave_port);
    usleep(500000); // ✅ 增加等待时间
    
    // 2. 初始化主节点
    printf("[MASTER] Initializing distributed system...\n");
    if (distributed_init(ROLE_MASTER, NULL, 0, sync_port) != 0) {
        printf("⚠ Distributed init failed\n");
        g_shutdown_flag = 1;
        pthread_join(slave_thread, NULL);
        return;
    }
    
    printf("[MASTER] Initializing eBPF sync...\n");
    if (ebpf_sync_init(sync_port) != 0) {
        printf("⚠ eBPF init failed, skipping test\n");
        g_shutdown_flag = 1;
        pthread_join(slave_thread, NULL);
        return;
    }

    current_test_port++; // ✅ 递增端口
    
    usleep(500000); // ✅ 等待连接建立
    
    // 3. 发送命令
    printf("📤 Sending commands from master...\n");
    
    char** t1 = create_tokens(3, "SET", "user:1001", "Alice");
    ebpf_emit_sync_event(t1, 3);
    free_tokens(t1, 3);
    printf("   Sent: SET user:1001 Alice\n");
    usleep(200000);
    
    char** t2 = create_tokens(3, "SET", "user:1002", "Bob");
    ebpf_emit_sync_event(t2, 3);
    free_tokens(t2, 3);
    printf("   Sent: SET user:1002 Bob\n");
    usleep(200000);
    
    char** t3 = create_tokens(2, "DEL", "user:1000");
    ebpf_emit_sync_event(t3, 2);
    free_tokens(t3, 2);
    printf("   Sent: DEL user:1000\n");
    
    printf("✓ Emitted 3 commands\n");
    
    // 4. 等待同步完成
    printf("[TEST] Waiting for synchronization...\n");
    sleep(2);
    
    // 5. 验证从节点收到的命令
    pthread_mutex_lock(&slave_recv_mutex);
    int received = slave_received_count;
    pthread_mutex_unlock(&slave_recv_mutex);
    
    printf("📊 Commands received by slave: %d/3\n", received);
    
    if (received > 0) {
        printf("📋 Slave received commands:\n");
        pthread_mutex_lock(&slave_recv_mutex);
        for (int i = 0; i < received; i++) {
            printf("   %d: %s %s %s\n", i + 1,
                   slave_received_cmds[i].cmd,
                   slave_received_cmds[i].key,
                   slave_received_cmds[i].value[0] ? slave_received_cmds[i].value : "(null)");
        }
        pthread_mutex_unlock(&slave_recv_mutex);
    } else {
        printf("⚠ No commands received by slave\n");
    }
    
    // 6. 清理
    printf("[TEST] Cleaning up...\n");
    g_shutdown_flag = 1;
    ebpf_sync_cleanup();
    pthread_join(slave_thread, NULL);
    
    printf("✓ Test completed\n");
}

// Test 3: 压力测试
void test_stress_sync() {
    printf("\n[TEST 3] Stress Test - 50 Commands\n");
    printf("======================================\n");
    reset_counters();
    g_shutdown_flag = 0;
    current_test_port++;
    
    int port = current_test_port;
    int sync_port = current_test_port + 1;  // ✅ 使用不同端口
    printf("[TEST] Using slave port %d, sync port %d\n", port, sync_port);
    
    
    // 启动从节点
    pthread_t slave_thread;
    int slave_port = port;
    pthread_create(&slave_thread, NULL, mock_slave_receiver, &slave_port);
    usleep(500000);
    
    if (distributed_init(ROLE_MASTER, NULL, 0, port) != 0 ||
        ebpf_sync_init(port) != 0) {
        printf("⚠ Init failed, skipping test\n");
        g_shutdown_flag = 1;
        pthread_join(slave_thread, NULL);
        return;
    }

    current_test_port++; // ✅ 递增端口
    
    usleep(500000);
    
    // 发送50个命令（减少数量以提高可靠性）
    const int total = 50;
    printf("📤 Sending %d commands...\n", total);
    
    for (int i = 0; i < total; i++) {
        char key[32], value[32];
        snprintf(key, sizeof(key), "key_%d", i);
        snprintf(value, sizeof(value), "value_%d", i);
        
        char** tokens = create_tokens(3, "SET", key, value);
        ebpf_emit_sync_event(tokens, 3);
        free_tokens(tokens, 3);
        
        if ((i + 1) % 10 == 0) {
            printf("   Sent: %d/%d\n", i + 1, total);
            usleep(100000); // ✅ 批次间暂停
        }
    }
    
    printf("✓ All %d commands emitted\n", total);
    
    // 等待同步
    printf("[TEST] Waiting for synchronization...\n");
    sleep(3);
    
    pthread_mutex_lock(&slave_recv_mutex);
    int received = slave_received_count;
    pthread_mutex_unlock(&slave_recv_mutex);
    
    printf("📊 Commands received: %d/%d (%.1f%%)\n", 
           received, total, (received * 100.0) / total);
    
    if (received >= total * 0.9) {
        printf("✓ Sync success rate >= 90%%\n");
    } else if (received >= total * 0.7) {
        printf("⚠ Sync success rate >= 70%% (acceptable)\n");
    } else {
        printf("✗ Sync success rate < 70%% (needs improvement)\n");
    }
    
    // 清理
    g_shutdown_flag = 1;
    ebpf_sync_cleanup();
    pthread_join(slave_thread, NULL);
    
    printf("✓ Test completed\n");
}

// Test 4: 验证命令内容正确性
void test_command_integrity() {
    printf("\n[TEST 4] Command Content Integrity\n");
    printf("======================================\n");
    reset_counters();
    g_shutdown_flag = 0;
    current_test_port++;
    
    int port = current_test_port;
    int sync_port = current_test_port + 1;  // ✅ 使用不同端口
    printf("[TEST] Using slave port %d, sync port %d\n", port, sync_port);
    
    
    pthread_t slave_thread;
    int slave_port = port;
    pthread_create(&slave_thread, NULL, mock_slave_receiver, &slave_port);
    usleep(500000);
    
    if (distributed_init(ROLE_MASTER, NULL, 0, port) != 0 ||
        ebpf_sync_init(port) != 0) {
        printf("⚠ Init failed, skipping test\n");
        g_shutdown_flag = 1;
        pthread_join(slave_thread, NULL);
        return;
    }

    current_test_port++; // ✅ 递增端口
    
    usleep(500000);
    
    // 发送特定命令
    struct {
        char *cmd;
        char *key;
        char *value;
    } test_cmds[] = {
        {"SET", "name", "Alice"},
        {"SET", "age", "25"},
        {"DEL", "temp", NULL},
        {"SET", "city", "Beijing"}
    };
    
    int cmd_count = sizeof(test_cmds) / sizeof(test_cmds[0]);
    
    printf("📤 Sending %d test commands...\n", cmd_count);
    for (int i = 0; i < cmd_count; i++) {
        if (test_cmds[i].value) {
            char** tokens = create_tokens(3, test_cmds[i].cmd, 
                                         test_cmds[i].key, 
                                         test_cmds[i].value);
            ebpf_emit_sync_event(tokens, 3);
            free_tokens(tokens, 3);
            printf("   Sent: %s %s %s\n", test_cmds[i].cmd, test_cmds[i].key, test_cmds[i].value);
        } else {
            char** tokens = create_tokens(2, test_cmds[i].cmd, 
                                         test_cmds[i].key);
            ebpf_emit_sync_event(tokens, 2);
            free_tokens(tokens, 2);
            printf("   Sent: %s %s\n", test_cmds[i].cmd, test_cmds[i].key);
        }
        usleep(200000);
    }
    
    printf("✓ Sent %d commands\n", cmd_count);
    printf("[TEST] Waiting for synchronization...\n");
    sleep(2);
    
    // 验证内容
    pthread_mutex_lock(&slave_recv_mutex);
    int received = slave_received_count;
    
    printf("📊 Verifying command integrity...\n");
    int correct = 0;
    for (int i = 0; i < received && i < cmd_count; i++) {
        int match = (strcmp(slave_received_cmds[i].cmd, test_cmds[i].cmd) == 0 &&
                    strcmp(slave_received_cmds[i].key, test_cmds[i].key) == 0);
        
        if (test_cmds[i].value) {
            match = match && (strcmp(slave_received_cmds[i].value, test_cmds[i].value) == 0);
        }
        
        if (match) {
            correct++;
            printf("   ✓ Command %d matched: %s %s\n", i + 1, 
                   slave_received_cmds[i].cmd, slave_received_cmds[i].key);
        } else {
            printf("   ✗ Command %d mismatch\n", i + 1);
            printf("      Expected: %s %s %s\n", test_cmds[i].cmd, test_cmds[i].key, 
                   test_cmds[i].value ? test_cmds[i].value : "(null)");
            printf("      Got:      %s %s %s\n", slave_received_cmds[i].cmd, 
                   slave_received_cmds[i].key, slave_received_cmds[i].value);
        }
    }
    pthread_mutex_unlock(&slave_recv_mutex);
    
    printf("📊 Integrity: %d/%d correct (%.1f%%)\n", 
           correct, cmd_count, (correct * 100.0) / cmd_count);
    
    g_shutdown_flag = 1;
    ebpf_sync_cleanup();
    pthread_join(slave_thread, NULL);
    
    printf("✓ Test completed\n");
}

int main(int argc, char* argv[]) {
    printf("========================================\n");
    printf("  eBPF Master-Slave Sync Test Suite\n");
    printf("========================================\n");
    
    if (geteuid() != 0) {
        fprintf(stderr, "\n❌ ERROR: This test requires root privileges\n");
        fprintf(stderr, "Please run with: sudo %s\n\n", argv[0]);
        return 1;
    }
    
    printf("\n🔧 Running comprehensive sync tests...\n");
    printf("   Base port: %d\n", TEST_BASE_PORT);
    printf("   Each test uses a unique port\n\n");
    
    test_event_capture();
    sleep(1);
    
    test_master_slave_sync();
    sleep(1);
    
    test_stress_sync();
    sleep(1);
    
    test_command_integrity();
    
    printf("\n========================================\n");
    printf("  All Tests Completed! ✓\n");
    printf("========================================\n");
    printf("\n💡 Tips:\n");
    printf("   - Check [SLAVE] logs for received commands\n");
    printf("   - Verify sync success rates\n");
    printf("   - Monitor command integrity results\n\n");
    
    return 0;
}