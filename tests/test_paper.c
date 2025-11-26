#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <arpa/inet.h>
#include <errno.h>

#define MAX_BUF (1024 * 1024)

static int send_all(int fd, const char* buf, int len){
    int sent = 0;
    while(sent < len){
        ssize_t n = send(fd, buf + sent, len - sent, MSG_NOSIGNAL);
        if(n < 0){
            if(errno == EINTR) continue;
            return -1;
        }
        if(n == 0){
            break;
        }
        sent += (int)n;
    }
    return sent;
}

//简单阻塞收取
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

//连接到kvstore
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

//将paper.txt 写入到内存
static char* load_paper(const char* path, size_t* out_len){
    FILE* fp = fopen(path, "rb");
    if(!fp){
        perror("fopen");
        return NULL;
    }
    fseek(fp, 0, SEEK_END);
    long sz = ftell(fp);
    fseek(fp, 0, SEEK_SET);
    if(sz <= 0 || sz > MAX_BUF){
        fprintf(stderr, "file size invalid: %ld\n", sz);
        fclose(fp);
        return NULL;
    }

    char* buf = (char*)malloc(sz + 1);
    if(!buf){
        perror("malloc");
        fclose(fp);
        return NULL;
    }
    size_t read_len = fread(buf, 1, sz, fp);
    fclose(fp);
    if(read_len != (size_t)sz){
        fprintf(stderr, "fread size mismatch: %zu != %ld\n", read_len, sz);
        free(buf);
        return NULL;
    }
    buf[sz] = '\0';
    if(out_len) *out_len = sz;
    return buf;
}

int main(int argc, char* argv[]){
    if(argc < 3){
        printf("Usage: %s <kvstore_ip> <kvstore_port>\n", argv[0]);
        return 1;
    }

    const char* ip = argv[1];
    int port = atoi(argv[2]);
    int fd = connect_kvstore(ip, port);
    if(fd < 0) return 1;

    size_t paper_len = 0;
    char* paper = load_paper("paper.txt", &paper_len);
    if(!paper){
        close(fd);
        return 1;
    }

    // PAPERSET paper1 <paper>
    {
        char* cmd = (char*)malloc(64 + paper_len);
        int n = snprintf(cmd, 64,
            "*3\r\n$8\r\nPAPERSET\r\n$6\r\npaper1\r\n$%zu\r\n",
        paper_len);
        memcpy(cmd + n, paper, paper_len);
        n += (int)paper_len;    
        memcpy(cmd + n, "\r\n", 2);
        n += 2;

        if (send_all(fd, cmd, n) != n) {
            fprintf(stderr, "send PAPERSET failed\n");
            free(cmd);
            free(paper);
            close(fd);
            return 1;
        }
        free(cmd);

        char line[128];
        recv_line(fd, line, sizeof(line));
        printf("PAPERSET resp: %s", line);
        if (strcmp(line, "+OK\r\n") != 0) {
            fprintf(stderr, "PAPERSET not OK\n");
            free(paper);
            close(fd);
            return 1;
        }
    }

    //PAPERGET paper1
    {
        char get_cmd[128];
        int n = snprintf(get_cmd, sizeof(get_cmd),
                         "*2\r\n$8\r\nPAPERGET\r\n$6\r\npaper1\r\n");
        if (send_all(fd, get_cmd, n) != n) {
            fprintf(stderr, "send PAPERGET failed\n");
            free(paper);
            close(fd);
            return 1;
        }

        char header[64];
        if(recv_line(fd, header, sizeof(header)) <= 0){
            fprintf(stderr, "recv PAPERGET header failed\n");
            free(paper);
            close(fd);
            return 1;
        }

        if(header[0] != '$'){
            fprintf(stderr, "PAPERGET response invalid: %s\n", header);
            free(paper);
            close(fd);
            return 1;
        }
        long resp_len = atol(header + 1);
        if(resp_len != (long)paper_len){
            fprintf(stderr, "PAPERGET length mismatch: %ld != %zu\n", resp_len, paper_len);
            free(paper);
            close(fd);
            return 1;
        }

        char* resp = (char*)malloc(resp_len + 2);
        int got = 0;
        while(got < resp_len + 2){
            int m = recv(fd, resp + got, (int)(resp_len + 2 - got), 0);
            if(m < 0){
                perror("recv");
                free(resp);
                free(paper);
                close(fd);
                return 1;
            }
            got += m;
        }

        //比较正文内容
        if (memcmp(resp, paper, paper_len) != 0) {
            fprintf(stderr, "PAPERGET body mismatch\n");
            free(resp);
            free(paper);
            close(fd);
            return 1;
        }
        free(resp);
        printf("PAPERGET after SET: content OK\n");
    }

    // 3) PAPERMOD paper1 <paper + suffix>
    const char *suffix = "\n--modified--\n";
    size_t mod_len = paper_len + strlen(suffix);
    char *paper_mod = (char*)malloc(mod_len);
    memcpy(paper_mod, paper, paper_len);
    memcpy(paper_mod + paper_len, suffix, strlen(suffix));

    {
        char *cmd = (char*)malloc(64 + mod_len);
        int n = snprintf(cmd, 64,
                         "*3\r\n$8\r\nPAPERMOD\r\n$6\r\npaper1\r\n$%zu\r\n",
                         mod_len);
        memcpy(cmd + n, paper_mod, mod_len);
        n += (int)mod_len;
        memcpy(cmd + n, "\r\n", 2);
        n += 2;

        if (send_all(fd, cmd, n) != n) {
            fprintf(stderr, "send PAPERMOD failed\n");
            free(cmd);
            free(paper_mod);
            free(paper);
            close(fd);
            return 1;
        }
        free(cmd);

        char line[128];
        recv_line(fd, line, sizeof(line));
        printf("PAPERMOD resp: %s", line);
        if (strcmp(line, "+OK\r\n") != 0) {
            fprintf(stderr, "PAPERMOD not OK\n");
            free(paper_mod);
            free(paper);
            close(fd);
            return 1;
        }
    }

    // 4) 再次 PAPERGET 检查修改后的内容
    {
        char get_cmd[128];
        int n = snprintf(get_cmd, sizeof(get_cmd),
                         "*2\r\n$8\r\nPAPERGET\r\n$6\r\npaper1\r\n");
        if (send_all(fd, get_cmd, n) != n) {
            fprintf(stderr, "send PAPERGET(2) failed\n");
            free(paper_mod);
            free(paper);
            close(fd);
            return 1;
        }

        char header[64];
        if (recv_line(fd, header, sizeof(header)) <= 0) {
            fprintf(stderr, "recv PAPERGET(2) header failed\n");
            free(paper_mod);
            free(paper);
            close(fd);
            return 1;
        }
        if (header[0] != '$') {
            fprintf(stderr, "unexpected PAPERGET(2) header: %s", header);
            free(paper_mod);
            free(paper);
            close(fd);
            return 1;
        }
        long resp_len = atol(header + 1);
        if (resp_len != (long)mod_len) {
            fprintf(stderr, "PAPERGET(2) len mismatch: expect %zu, got %ld\n",
                    mod_len, resp_len);
            free(paper_mod);
            free(paper);
            close(fd);
            return 1;
        }

        char *resp = (char*)malloc(resp_len + 2);
        int got = 0;
        while (got < resp_len + 2) {
            int m = recv(fd, resp + got, resp_len + 2 - got, 0);
            if (m <= 0) {
                perror("recv PAPERGET(2) body");
                free(resp);
                free(paper_mod);
                free(paper);
                close(fd);
                return 1;
            }
            got += m;
        }
        if (memcmp(resp, paper_mod, mod_len) != 0) {
            fprintf(stderr, "PAPERGET(2) body mismatch\n");
            free(resp);
            free(paper_mod);
            free(paper);
            close(fd);
            return 1;
        }
        free(resp);
        printf("PAPERGET after MOD: content OK\n");
    }

    // 5) PAPERDEL paper1
    {
        const char *del_cmd =
            "*2\r\n$8\r\nPAPERDEL\r\n$6\r\npaper1\r\n";
        if (send_all(fd, del_cmd, (int)strlen(del_cmd)) != (int)strlen(del_cmd)) {
            fprintf(stderr, "send PAPERDEL failed\n");
            free(paper_mod);
            free(paper);
            close(fd);
            return 1;
        }
        char line[128];
        recv_line(fd, line, sizeof(line));
        printf("PAPERDEL resp: %s", line);
        if (strcmp(line, "+OK\r\n") != 0) {
            fprintf(stderr, "PAPERDEL not OK\n");
            free(paper_mod);
            free(paper);
            close(fd);
            return 1;
        }
    }

    // 6) 再 PAPERGET，期望不存在
    {
        char get_cmd[128];
        int n = snprintf(get_cmd, sizeof(get_cmd),
                         "*2\r\n$8\r\nPAPERGET\r\n$6\r\npaper1\r\n");
        if (send_all(fd, get_cmd, n) != n) {
            fprintf(stderr, "send PAPERGET after DEL failed\n");
            free(paper_mod);
            free(paper);
            close(fd);
            return 1;
        }

        char header[64];
        if (recv_line(fd, header, sizeof(header)) <= 0) {
            fprintf(stderr, "recv PAPERGET after DEL header failed\n");
            free(paper_mod);
            free(paper);
            close(fd);
            return 1;
        }
        // 这里假设不存在时返回 "$-1\r\n"，如果你实现是 "+NO EXIST\r\n" 就改下面判断
        if (strcmp(header, "$-1\r\n") != 0 && strcmp(header, "+NO EXIST\r\n") != 0) {
            fprintf(stderr, "PAPERGET after DEL unexpected resp: %s", header);
            free(paper_mod);
            free(paper);
            close(fd);
            return 1;
        }
        printf("PAPERGET after DEL: not exist as expected\n");
    }


    printf("PAPERSET / PAPERMOD / PAPERDEL test PASSED\n");

    free(paper_mod);
    free(paper);
    close(fd);
    return 0;
}