#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <arpa/inet.h>
#include <errno.h>

#define MAX_BUF (1024 * 1024)

// 发送完整缓冲区
static int send_all(int fd, const char* buf, int len){
    int sent = 0;
    while (sent < len) {
        ssize_t n = send(fd, buf + sent, len - sent, MSG_NOSIGNAL);
        if (n < 0) {
            if (errno == EINTR) continue;
            perror("send");
            return -1;
        }
        if (n == 0) break;
        sent += (int)n;
    }
    return sent;
}

// 简单阻塞读一行（直到 '\n'）
static int recv_line(int fd, char* buf, int buflen){
    int total = 0;
    while (total + 1 < buflen) {
        char c;
        int n = recv(fd, &c, 1, 0);
        if (n < 0) {
            if (errno == EINTR) continue;
            perror("recv");
            return -1;
        }
        if (n == 0) break;
        buf[total++] = c;
        if (c == '\n') break;
    }
    buf[total] = '\0';
    return total;
}

// 连接到 kvstore
static int connect_kvstore(const char* ip, int port){
    int fd = socket(AF_INET, SOCK_STREAM, 0);
    if (fd < 0) {
        perror("socket");
        return -1;
    }
    struct sockaddr_in addr;
    memset(&addr, 0, sizeof(addr));
    addr.sin_family      = AF_INET;
    addr.sin_port        = htons(port);
    addr.sin_addr.s_addr = inet_addr(ip);

    if (connect(fd, (struct sockaddr*)&addr, sizeof(addr)) != 0) {
        perror("connect");
        close(fd);
        return -1;
    }
    return fd;
}

// 读取 paper.txt 到内存
static char* load_paper(const char* path, size_t* out_len){
    FILE* fp = fopen(path, "rb");
    if (!fp) {
        perror("fopen");
        return NULL;
    }
    fseek(fp, 0, SEEK_END);
    long sz = ftell(fp);
    fseek(fp, 0, SEEK_SET);
    if (sz <= 0 || sz > MAX_BUF) {
        fprintf(stderr, "file size invalid: %ld\n", sz);
        fclose(fp);
        return NULL;
    }

    char* buf = (char*)malloc(sz + 1);
    if (!buf) {
        perror("malloc");
        fclose(fp);
        return NULL;
    }
    size_t read_len = fread(buf, 1, sz, fp);
    fclose(fp);
    if (read_len != (size_t)sz) {
        fprintf(stderr, "fread size mismatch: %zu != %ld\n", read_len, sz);
        free(buf);
        return NULL;
    }
    buf[sz] = '\0';
    if (out_len) *out_len = sz;
    return buf;
}

// 对某个连接执行 PAPERGET，并把结果读到 resp（长度 resp_len）
static int do_paperget(int fd, const char *key,
                       char **resp_out, size_t *len_out)
{
    char get_cmd[128];
    int n = snprintf(get_cmd, sizeof(get_cmd),
                     "*2\r\n$8\r\nPAPERGET\r\n$%zu\r\n%s\r\n",
                     strlen(key), key);
    if (send_all(fd, get_cmd, n) != n) {
        fprintf(stderr, "send PAPERGET failed\n");
        return -1;
    }

    char header[64];
    if (recv_line(fd, header, sizeof(header)) <= 0) {
        fprintf(stderr, "recv PAPERGET header failed\n");
        return -1;
    }
    if (header[0] != '$') {
        // 允许 "$-1\r\n" 或 "+NO EXIST\r\n" 等情况留给上层判断
        *resp_out = strdup(header);
        *len_out  = strlen(header);
        return 1; // 特殊返回，表示不是正常 bulk
    }

    long resp_len = atol(header + 1);
    if (resp_len < 0) {
        // "$-1\r\n" 表示不存在
        *resp_out = strdup(header);
        *len_out  = strlen(header);
        return 1;
    }

    // 读正文 + 结尾 "\r\n"
    char* resp = (char*)malloc(resp_len + 2);
    if (!resp) {
        perror("malloc");
        return -1;
    }
    int got = 0;
    while (got < resp_len + 2) {
        int m = recv(fd, resp + got, (int)(resp_len + 2 - got), 0);
        if (m <= 0) {
            perror("recv PAPERGET body");
            free(resp);
            return -1;
        }
        got += m;
    }
    *resp_out = resp;
    *len_out  = (size_t)resp_len; // 只返回正文长度
    return 0;
}

int main(int argc, char* argv[]){
    if (argc < 5) {
        printf("Usage: %s <master_ip> <master_port> <slave_ip> <slave_port>\n",
               argv[0]);
        return 1;
    }

    const char* master_ip  = argv[1];
    int         master_port= atoi(argv[2]);
    const char* slave_ip   = argv[3];
    int         slave_port = atoi(argv[4]);

    int fd_master = connect_kvstore(master_ip, master_port);
    if (fd_master < 0) return 1;

    int fd_slave = connect_kvstore(slave_ip, slave_port);
    if (fd_slave < 0) {
        close(fd_master);
        return 1;
    }

    size_t paper_len = 0;
    char* paper = load_paper("paper.txt", &paper_len);
    if (!paper) {
        close(fd_master);
        close(fd_slave);
        return 1;
    }

    const char *key = "paper1";

    // 1) 在 master 上 PAPERSET
    {
        char* cmd = (char*)malloc(64 + paper_len);
        int n = snprintf(cmd, 64,
            "*3\r\n$8\r\nPAPERSET\r\n$%zu\r\n%s\r\n$%zu\r\n",
            strlen(key), key, paper_len);
        memcpy(cmd + n, paper, paper_len);
        n += (int)paper_len;
        memcpy(cmd + n, "\r\n", 2);
        n += 2;

        if (send_all(fd_master, cmd, n) != n) {
            fprintf(stderr, "[MASTER] send PAPERSET failed\n");
            free(cmd);
            free(paper);
            close(fd_master);
            close(fd_slave);
            return 1;
        }
        free(cmd);

        char line[128];
        if (recv_line(fd_master, line, sizeof(line)) <= 0) {
            fprintf(stderr, "[MASTER] recv PAPERSET resp failed\n");
            free(paper);
            close(fd_master);
            close(fd_slave);
            return 1;
        }
        printf("[MASTER] PAPERSET resp: %s", line);
        if (strcmp(line, "+OK\r\n") != 0) {
            fprintf(stderr, "[MASTER] PAPERSET not OK\n");
            free(paper);
            close(fd_master);
            close(fd_slave);
            return 1;
        }
    }

    // 2) 在 master 上 PAPERGET 校验
    {
        char *resp = NULL;
        size_t len = 0;
        if (do_paperget(fd_master, key, &resp, &len) != 0) {
            fprintf(stderr, "[MASTER] PAPERGET failed\n");
            free(paper);
            close(fd_master);
            close(fd_slave);
            return 1;
        }
        if (len != paper_len || memcmp(resp, paper, paper_len) != 0) {
            fprintf(stderr, "[MASTER] PAPERGET content mismatch\n");
            free(resp);
            free(paper);
            close(fd_master);
            close(fd_slave);
            return 1;
        }
        free(resp);
        printf("[MASTER] PAPERGET after SET: content OK\n");
    }

    // 3) 等待主从同步，然后在 slave 上 PAPERGET 校验
    sleep(2); // 给主从同步留一点时间，根据你的实现可调整

    {
        char *resp = NULL;
        size_t len = 0;
        int ret = do_paperget(fd_slave, key, &resp, &len);
        if (ret != 0) {
            fprintf(stderr, "[SLAVE] PAPERGET failed or not bulk\n");
            if (resp) free(resp);
            free(paper);
            close(fd_master);
            close(fd_slave);
            return 1;
        }
        if (len != paper_len || memcmp(resp, paper, paper_len) != 0) {
            fprintf(stderr, "[SLAVE] PAPERGET content mismatch after SET\n");
            free(resp);
            free(paper);
            close(fd_master);
            close(fd_slave);
            return 1;
        }
        free(resp);
        printf("[SLAVE] PAPERGET after SET sync: content OK\n");
    }

    // 4) 在 master 上 PAPERMOD（paper + suffix）
    const char *suffix = "\n--modified--\n";
    size_t mod_len = paper_len + strlen(suffix);
    char *paper_mod = (char*)malloc(mod_len);
    memcpy(paper_mod, paper, paper_len);
    memcpy(paper_mod + paper_len, suffix, strlen(suffix));

    {
        char *cmd = (char*)malloc(64 + mod_len);
        int n = snprintf(cmd, 64,
                         "*3\r\n$8\r\nPAPERMOD\r\n$%zu\r\n%s\r\n$%zu\r\n",
                         strlen(key), key, mod_len);
        memcpy(cmd + n, paper_mod, mod_len);
        n += (int)mod_len;
        memcpy(cmd + n, "\r\n", 2);
        n += 2;

        if (send_all(fd_master, cmd, n) != n) {
            fprintf(stderr, "[MASTER] send PAPERMOD failed\n");
            free(cmd);
            free(paper_mod);
            free(paper);
            close(fd_master);
            close(fd_slave);
            return 1;
        }
        free(cmd);

        char line[128];
        if (recv_line(fd_master, line, sizeof(line)) <= 0) {
            fprintf(stderr, "[MASTER] recv PAPERMOD resp failed\n");
            free(paper_mod);
            free(paper);
            close(fd_master);
            close(fd_slave);
            return 1;
        }
        printf("[MASTER] PAPERMOD resp: %s", line);
        if (strcmp(line, "+OK\r\n") != 0) {
            fprintf(stderr, "[MASTER] PAPERMOD not OK\n");
            free(paper_mod);
            free(paper);
            close(fd_master);
            close(fd_slave);
            return 1;
        }
    }

    // 5) 在 master 上 PAPERGET 校验修改后内容
    {
        char *resp = NULL;
        size_t len = 0;
        if (do_paperget(fd_master, key, &resp, &len) != 0) {
            fprintf(stderr, "[MASTER] PAPERGET(2) failed\n");
            free(paper_mod);
            free(paper);
            close(fd_master);
            close(fd_slave);
            return 1;
        }
        if (len != mod_len || memcmp(resp, paper_mod, mod_len) != 0) {
            fprintf(stderr, "[MASTER] PAPERGET(2) content mismatch\n");
            free(resp);
            free(paper_mod);
            free(paper);
            close(fd_master);
            close(fd_slave);
            return 1;
        }
        free(resp);
        printf("[MASTER] PAPERGET after MOD: content OK\n");
    }

    // 6) 再等一会儿，在 slave 上 PAPERGET 校验修改后内容已同步
    sleep(2);

    {
        char *resp = NULL;
        size_t len = 0;
        int ret = do_paperget(fd_slave, key, &resp, &len);
        if (ret != 0) {
            fprintf(stderr, "[SLAVE] PAPERGET(2) failed or not bulk\n");
            if (resp) free(resp);
            free(paper_mod);
            free(paper);
            close(fd_master);
            close(fd_slave);
            return 1;
        }
        if (len != mod_len || memcmp(resp, paper_mod, mod_len) != 0) {
            fprintf(stderr, "[SLAVE] PAPERGET(2) content mismatch after MOD\n");
            free(resp);
            free(paper_mod);
            free(paper);
            close(fd_master);
            close(fd_slave);
            return 1;
        }
        free(resp);
        printf("[SLAVE] PAPERGET after MOD sync: content OK\n");
    }

    printf("PAPERSET / PAPERGET / PAPERMOD master-slave sync test PASSED\n");

    free(paper_mod);
    free(paper);
    close(fd_master);
    close(fd_slave);
    return 0;
}