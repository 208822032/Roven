#include <stdio.h>
#include <string.h>
#include <stdlib.h>
#include <arpa/inet.h>
#include <sys/time.h>
#include <unistd.h>

#define MAX_MSG_LENGTH      1024
#define TIME_SUB_MS(tv1, tv2)  ((tv1.tv_sec - tv2.tv_sec) * 1000 + (tv1.tv_usec - tv2.tv_usec) / 1000)

// 实现其他辅助函数（与testcase.c中相同）
int send_msg(int connfd, char *msg, int length) {
    int res = send(connfd, msg, length, 0);
    if (res < 0) {
        perror("send");
        exit(1);
    }
    return res;
}

int recv_msg(int connfd, char *msg, int length) {
    int res = recv(connfd, msg, length, 0);
    if (res < 0) {
        perror("recv");
        exit(1);
    }
    return res;
}

void testcase(int connfd, char *msg, char *pattern, char *casename) {
    if (!msg || !pattern || !casename) return;
    
    send_msg(connfd, msg, strlen(msg));
    printf("sendmsg: %s\n", msg);
    
    char result[MAX_MSG_LENGTH] = {0};
    recv_msg(connfd, result, MAX_MSG_LENGTH);
    printf("recvmsg: %s\n", result);
    
    if (strcmp(result, pattern) == 0) {
        printf("==> PASS ->  %s\n", casename);
    } else {
        printf("==> FAILED -> %s, '%s' != '%s' \n", casename, result, pattern);
        exit(1);
    }
}

int connect_tcpserver(const char *ip, unsigned short port) {
    int connfd = socket(AF_INET, SOCK_STREAM, 0);
    if (connfd < 0) {
        perror("socket");
        return -1;
    }

    struct sockaddr_in server_addr;
    memset(&server_addr, 0, sizeof(struct sockaddr_in));
    
    server_addr.sin_family = AF_INET;
    server_addr.sin_addr.s_addr = inet_addr(ip);
    server_addr.sin_port = htons(port);
    
    if (connect(connfd, (struct sockaddr*)&server_addr, sizeof(struct sockaddr_in))) {
        perror("connect");
        close(connfd);
        return -1;
    }
    
    return connfd;
}

// 分布式主从同步测试
void distributed_sync_test(int master_fd, int slave_fd) {
    printf("Starting distributed sync test...\n");
    
    // === 测试1: 基本SET同步 ===
    printf("基本同步测试\n");
    testcase(master_fd, "*3\r\n$3\r\nSET\r\n$8\r\narr_key1\r\n$5\r\napple\r\n", "+OK\r\n", "SET-arr_key1");
     
    // 等待同步完成
    usleep(100000); // 100ms
    
    testcase(slave_fd, "*2\r\n$3\r\nGET\r\n$8\r\narr_key1\r\n", "$5\r\napple\r\n", "GET from slave (basic SET)");
    
    // === 测试2: 修改操作同步 ===
    testcase(master_fd, "*3\r\n$3\r\nMOD\r\n$8\r\narr_key1\r\n$5\r\nAPPLE\r\n", "+OK\r\n", "MOD on master");
    
    usleep(100000);
    testcase(slave_fd, "*2\r\n$3\r\nGET\r\n$8\r\narr_key1\r\n", "$5\r\nAPPLE\r\n", "GET from slave (after MOD)");
    
    // === 测试3: 删除操作同步 ===
    testcase(master_fd, "*2\r\n$3\r\nDEL\r\n$8\r\narr_key1\r\n", "+OK\r\n", "DEL on master");
    
    usleep(100000);
    testcase(slave_fd, "*2\r\n$5\r\nEXIST\r\n$8\r\narr_key1\r\n", "+NO EXIST\r\n", "GET from slave (after DEL)");
    
    // === 测试4: 多数据结构同步 ===
    testcase(master_fd, "*3\r\n$4\r\nRSET\r\n$9\r\ntree_key1\r\n$8\r\nelephant\r\n", "+OK\r\n", "RSET on master");
    testcase(master_fd, "*3\r\n$4\r\nHSET\r\n$9\r\nhash_key1\r\n$6\r\niguana\r\n", "+OK\r\n", "HSET on master");
    testcase(master_fd, "*3\r\n$4\r\nSSET\r\n$9\r\nskip_key1\r\n$6\r\nmonkey\r\n", "+OK\r\n", "SSET on master");
    
    usleep(150000);
    testcase(slave_fd, "*2\r\n$4\r\nRGET\r\n$9\r\ntree_key1\r\n", "$8\r\nelephant\r\n", "RGET from slave");
    testcase(slave_fd, "*2\r\n$4\r\nHGET\r\n$9\r\nhash_key1\r\n", "$6\r\niguana\r\n", "HGET from slave");
    testcase(slave_fd, "*2\r\n$4\r\nSGET\r\n$9\r\nskip_key1\r\n", "$6\r\nmonkey\r\n", "SGET from slave");
    
    // === 测试5: 从节点拒绝写操作 ===
    testcase(slave_fd, "*3\r\n$3\r\nSET\r\n$8\r\narr_key1\r\n$5\r\napple\r\n", "-ERROR SLAVE READ ONLY\r\n", "Write attempt on slave");
    
    // === 清理 ===
    testcase(master_fd, "*2\r\n$4\r\nRDEL\r\n$9\r\ntree_key1\r\n", "+OK\r\n", "Cleanup RBTree");
    testcase(master_fd, "*2\r\n$4\r\nHDEL\r\n$9\r\nhash_key1\r\n", "+OK\r\n", "Cleanup Hash");
    testcase(master_fd, "*2\r\n$4\r\nSDEL\r\n$9\r\nskip_key1\r\n", "+OK\r\n", "Cleanup SkipTable");
    
    printf("Distributed sync test completed!\n");
}

int main(int argc, char *argv[]) {
    if (argc != 5) {
        printf("Usage: %s <ip> <master_port> <slave_port> <test_mode>\n", argv[0]);
        printf("Test mode 10: Distributed sync test\n");
        return -1;
    }

    char *ip = argv[1];
    int master_port = atoi(argv[2]);
    int slave_port = atoi(argv[3]);
    int mode = atoi(argv[4]);

    printf("Usage: %s <ip> <master_port> <slave_port> <test_mode>\n", argv[0]);

    int master_fd = connect_tcpserver(ip, master_port);
    int slave_fd = connect_tcpserver(ip, slave_port);

    if (master_fd < 0 || slave_fd < 0) {
        printf("Failed to connect to servers\n");
        return -1;
    }

    if (mode == 10) {
        printf("Running distributed sync test...\n");
        printf("Master port: %d, Slave port: %d\n", master_port, slave_port);
        distributed_sync_test(master_fd, slave_fd);
    } else {
        printf("Unknown test mode: %d\n", mode);
    }

    close(master_fd);
    close(slave_fd);
    
    return 0;
}
