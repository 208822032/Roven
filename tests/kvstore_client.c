#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <arpa/inet.h>
#include <ctype.h>
#include <errno.h>

#include <readline/readline.h>
#include <readline/history.h>

static int read_and_print_reply_with_save(int fd, const char *save_path);
static int read_bulk_and_save(int fd, const char *save_path);


#define BUF_SIZE 4096

/*==================== 颜色宏 ====================*/

#define CLR_RESET   "\x1b[0m"
#define CLR_RED     "\x1b[31m"
#define CLR_GREEN   "\x1b[32m"
#define CLR_YELLOW  "\x1b[33m"
#define CLR_CYAN    "\x1b[36m"
#define CLR_BOLD    "\x1b[1m"

/*==================== 帮助表 & 补全用命令表 ====================*/

typedef struct {
    const char *name;
    const char *summary;
    const char *usage;
} cli_help_entry_t;

static cli_help_entry_t g_help_table[] = {
    { "SET",      "Set string value",           "SET key value" },
    { "GET",      "Get string value",           "GET key" },
    { "DEL",      "Delete key",                 "DEL key" },
    { "RSET",     "RBTree set",                 "RSET key value" },
    { "RGET",     "RBTree get",                 "RGET key" },
    { "RDEL",     "RBTree delete",              "RDEL key" },
    { "HSET",     "Hash set",                   "HSET key value" },
    { "HGET",     "Hash get",                   "HGET key" },
    { "HDEL",     "Hash delete",                "HDEL key" },
    { "SSET",     "Skiplist set",               "SSET key value" },
    { "SGET",     "Skiplist get",               "SGET key" },
    { "SDEL",     "Skiplist delete",            "SDEL key" },
    { "PAPERSET", "Save paper from file",       "PAPERSET key /path/to/paper" },
    { "PAPERGET", "Get paper and save to file", "PAPERGET key /path/to/save" },
    { "PAPERDEL", "Delete paper",                "PAPERDEL key" },
    { "PAPERMOD", "Modify paper from file",      "PAPERMOD key /path/to/paper" },
    // 注意：服务器 QUIT 用 SQUIT 转发，避免与客户端退出冲突
    { "SQUIT",    "Send QUIT to server",        "SQUIT" },
    { "EXIT",     "Exit client",                "EXIT" },
    { "HELP",     "Show help",                  "HELP [command]" },
};

static size_t g_help_count = sizeof(g_help_table) / sizeof(g_help_table[0]);

/*==================== 基础连接与工具函数 ====================*/

static void print_usage(const char *prog) {
    fprintf(stderr,
        "Usage:\n"
        "  %s <ip> <port>                # interactive mode\n"
        "  %s <ip> <port> -c \"CMD ...\"   # run single command and exit\n"
    "  echo \"CMD ...\" | %s <ip> <port>  # read commands from stdin (pipe mode)\n"
    "\n"
    "Notes:\n"
    "  PAPERSET key /path/to/paper : read paper from local file and store as value\n"
    "  PAPERMOD key /path/to/paper : modify existing paper with content from file\n"
    "  PAPERGET key /path/to/save  : download paper, only print length, body saved to file\n"
    "  PAPERDEL key                : delete paper\n",
        prog, prog, prog);
}

static int connect_server(const char *ip, unsigned short port) {
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

    if (connect(fd, (struct sockaddr*)&addr, sizeof(addr)) < 0) {
        perror("connect");
        close(fd);
        return -1;
    }
    return fd;
}

// 简单按空白切分一行，返回 token 数，tokens[i] 指向 line 内部
static int split_line(char *line, char **tokens, int max_tokens) {
    int count = 0;
    char *p = line;

    // 去掉行尾换行
    size_t len = strlen(line);
    while (len > 0 && (line[len-1] == '\n' || line[len-1] == '\r')) {
        line[--len] = '\0';
    }

    while (*p && count < max_tokens) {
        // 跳过前导空白
        while (*p && isspace((unsigned char)*p)) p++;
        if (!*p) break;

        tokens[count++] = p;

        // 向后找到下一个空白并截断
        while (*p && !isspace((unsigned char)*p)) p++;
        if (*p) {
            *p = '\0';
            p++;
        }
    }
    return count;
}

// 构造 RESP 请求：*argc\r\n$len\r\narg\r\n...
static int build_resp_request(char **tokens, int argc, char *out, size_t out_size) {
    int n = 0;
    int written = snprintf(out + n, out_size - n, "*%d\r\n", argc);
    if (written < 0 || written >= (int)(out_size - n)) return -1;
    n += written;

    for (int i = 0; i < argc; ++i) {
        size_t len = strlen(tokens[i]);
        written = snprintf(out + n, out_size - n, "$%zu\r\n", len);
        if (written < 0 || written >= (int)(out_size - n)) return -1;
        n += written;

        if (len + 2 > out_size - n) return -1;
        memcpy(out + n, tokens[i], len);
        n += (int)len;
        out[n++] = '\r';
        out[n++] = '\n';
    }
    return n;
}

/*==================== 大文件相关工具 ====================*/

// 读取整个文件为一块内存；调用者负责 free(*out_buf)
// 返回 0 成功，<0 失败
static int read_file_all(const char *path, char **out_buf, size_t *out_len) {
    *out_buf = NULL;
    if (out_len) *out_len = 0;

    FILE *fp = fopen(path, "rb");
    if (!fp) {
        perror("fopen");
        return -1;
    }

    if (fseek(fp, 0, SEEK_END) != 0) {
        perror("fseek");
        fclose(fp);
        return -1;
    }

    long sz = ftell(fp);
    if (sz < 0) {
        perror("ftell");
        fclose(fp);
        return -1;
    }
    rewind(fp);

    // +1 预留 '\0'，方便调试；value 可以包含 '\0'
    char *buf = (char *)malloc((size_t)sz + 1);
    if (!buf) {
        perror("malloc");
        fclose(fp);
        return -1;
    }

    size_t n = fread(buf, 1, (size_t)sz, fp);
    fclose(fp);

    if (n != (size_t)sz) {
        fprintf(stderr, "fread mismatch: expect=%ld, got=%zu\n", sz, n);
        free(buf);
        return -1;
    }

    buf[n] = '\0';
    *out_buf = buf;
    if (out_len) *out_len = n;
    return 0;
}

// 根据 key + 原始二进制内容构造 RESP：PAPERSET key <binary>
// 与 build_savehuge_request 类似，只是命令名不同
static int build_paperset_request(const char *key,
                                  const char *file_data,
                                  size_t file_len,
                                  char **out_buf,
                                  size_t *out_len)
{
    *out_buf = NULL;
    if (out_len) *out_len = 0;

    char key_len_str[32];
    char val_len_str[64];
    int key_len_str_n = snprintf(key_len_str, sizeof(key_len_str), "%zu", strlen(key));
    int val_len_str_n = snprintf(val_len_str, sizeof(val_len_str), "%zu", file_len);
    if (key_len_str_n <= 0 || val_len_str_n <= 0) {
        return -1;
    }

    // 命令名 PAPERSET 长度为 8
    size_t max_req =
        4 +                         // "*3\r\n"
        4 + 8 + 2 +                 // "$8\r\nPAPERSET\r\n"
        1 + (size_t)key_len_str_n + 2 +  // "$<keylen>\r\n"
        strlen(key) + 2 +           // "<key>\r\n"
        1 + (size_t)val_len_str_n + 2 +  // "$<file_len>\r\n"
        file_len + 2;               // "<file_data>\r\n"

    char *buf = (char *)malloc(max_req);
    if (!buf) {
        perror("malloc");
        return -1;
    }

    size_t n = 0;
    int w = 0;

    // *3\r\n
    w = snprintf(buf + n, max_req - n, "*3\r\n");
    if (w < 0 || (size_t)w >= max_req - n) goto fail2;
    n += (size_t)w;

    // $8\r\nPAPERSET\r\n
    w = snprintf(buf + n, max_req - n, "$8\r\nPAPERSET\r\n");
    if (w < 0 || (size_t)w >= max_req - n) goto fail2;
    n += (size_t)w;

    // $<keylen>\r\n
    w = snprintf(buf + n, max_req - n, "$%s\r\n", key_len_str);
    if (w < 0 || (size_t)w >= max_req - n) goto fail2;
    n += (size_t)w;

    // <key>\r\n
    if (strlen(key) + 2 > max_req - n) goto fail2;
    memcpy(buf + n, key, strlen(key));
    n += strlen(key);
    buf[n++] = '\r';
    buf[n++] = '\n';

    // $<file_len>\r\n
    w = snprintf(buf + n, max_req - n, "$%s\r\n", val_len_str);
    if (w < 0 || (size_t)w >= max_req - n) goto fail2;
    n += (size_t)w;

    // <file_data>\r\n
    if (file_len + 2 > max_req - n) goto fail2;
    memcpy(buf + n, file_data, file_len);
    n += file_len;
    buf[n++] = '\r';
    buf[n++] = '\n';

    *out_buf = buf;
    if (out_len) *out_len = n;
    return 0;

fail2:
    free(buf);
    return -1;
}

// 根据 key + 原始二进制内容构造 RESP：PAPERMOD key <binary>
static int build_papermod_request(const char *key,
                                  const char *file_data,
                                  size_t file_len,
                                  char **out_buf,
                                  size_t *out_len)
{
    *out_buf = NULL;
    if (out_len) *out_len = 0;

    char key_len_str[32];
    char val_len_str[64];
    int key_len_str_n = snprintf(key_len_str, sizeof(key_len_str), "%zu", strlen(key));
    int val_len_str_n = snprintf(val_len_str, sizeof(val_len_str), "%zu", file_len);
    if (key_len_str_n <= 0 || val_len_str_n <= 0) {
        return -1;
    }

    // 命令名 PAPERMOD 长度为 8
    size_t max_req =
        4 +                         // "*3\r\n"
        4 + 8 + 2 +                 // "$8\r\nPAPERMOD\r\n"
        1 + (size_t)key_len_str_n + 2 +  // "$<keylen>\r\n"
        strlen(key) + 2 +           // "<key>\r\n"
        1 + (size_t)val_len_str_n + 2 +  // "$<file_len>\r\n"
        file_len + 2;               // "<file_data>\r\n"

    char *buf = (char *)malloc(max_req);
    if (!buf) {
        perror("malloc");
        return -1;
    }

    size_t n = 0;
    int w = 0;

    // *3\r\n
    w = snprintf(buf + n, max_req - n, "*3\r\n");
    if (w < 0 || (size_t)w >= max_req - n) goto fail3;
    n += (size_t)w;

    // $8\r\nPAPERMOD\r\n
    w = snprintf(buf + n, max_req - n, "$8\r\nPAPERMOD\r\n");
    if (w < 0 || (size_t)w >= max_req - n) goto fail3;
    n += (size_t)w;

    // $<keylen>\r\n
    w = snprintf(buf + n, max_req - n, "$%s\r\n", key_len_str);
    if (w < 0 || (size_t)w >= max_req - n) goto fail3;
    n += (size_t)w;

    // <key>\r\n
    if (strlen(key) + 2 > max_req - n) goto fail3;
    memcpy(buf + n, key, strlen(key));
    n += strlen(key);
    buf[n++] = '\r';
    buf[n++] = '\n';

    // $<file_len>\r\n
    w = snprintf(buf + n, max_req - n, "$%s\r\n", val_len_str);
    if (w < 0 || (size_t)w >= max_req - n) goto fail3;
    n += (size_t)w;

    // <file_data>\r\n
    if (file_len + 2 > max_req - n) goto fail3;
    memcpy(buf + n, file_data, file_len);
    n += file_len;
    buf[n++] = '\r';
    buf[n++] = '\n';

    *out_buf = buf;
    if (out_len) *out_len = n;
    return 0;

fail3:
    free(buf);
    return -1;
}

/*==================== RESP 输出美化（含颜色） ====================*/

static void print_resp_array(const char *buf, int n, char *p, char *cr) {
    *cr = '\0';
    long n_elem = strtol(p, NULL, 10);
    if (n_elem < 0) {
        printf(CLR_YELLOW "(nil)\n" CLR_RESET);
        return;
    }
    char *q = cr + 2; // 下一行开始
    long idx = 1;

    while (idx <= n_elem && q < buf + n) {
        char t = *q++;
        char *cr2 = strchr(q, '\r');
        if (!cr2 || q >= buf + n) break;

        if (t == '$') {
            *cr2 = '\0';
            long len_val = strtol(q, NULL, 10);
            char *data = cr2 + 2;
            if (len_val < 0) {
                printf("%ld) " CLR_YELLOW "(nil)\n" CLR_RESET, idx);
                q = cr2 + 2;
            } else if (data + len_val <= buf + n) {
                printf("%ld) " CLR_BOLD "\"%.*s\"" CLR_RESET "\n",
                       idx, (int)len_val, data);
                q = data + len_val + 2; // 跳过 val\r\n
            } else {
                fwrite(buf, 1, n, stdout);
                break;
            }
        } else if (t == '+' || t == '-' || t == ':') {
            *cr2 = '\0';
            printf("%ld) %s\n", idx, q);
            q = cr2 + 2;
        } else {
            // 其他复杂情况，直接原样打印
            fwrite(buf, 1, n, stdout);
            break;
        }
        idx++;
    }
}

static int read_paperget_reply(int fd, const char *save_path) {
    // 先读一个字节看类型
    char t;
    ssize_t r = recv(fd, &t, 1, MSG_PEEK);
    if (r <= 0) {
        perror("recv");
        return -1;
    }

    if (t == '$') {
        // 仍然按之前逻辑：bulk -> 写文件
        return read_bulk_and_save(fd, save_path);
    } else {
        // 非 bulk：用通用打印逻辑，不保存文件
        return read_and_print_reply_with_save(fd, NULL);
    }
}

// 专用于 PAPERGET：读取一个 bulk 响应，打印长度并流式写入文件
static int read_bulk_and_save(int fd, const char *save_path) {
    if (!save_path) return -1;
    
    char header[64];
    size_t used = 0;

    // 1) 先读出类型 + 长度行，直到遇到 "\r\n"
    while (used + 1 < sizeof(header)) {
        ssize_t r = recv(fd, header + used, 1, 0);
        if (r <= 0) {
            perror("recv");
            return -1;
        }
        used += (size_t)r;
        if (used >= 2 && header[used-2] == '\r' && header[used-1] == '\n') {
            break;
        }
    }

    if (used < 4 || header[0] != '$') {
        fwrite(header, 1, used, stdout);
        fprintf(stderr, CLR_RED "\nUnexpected RESP type, expect bulk string.\n" CLR_RESET);
        return -1;
    }

    header[used] = '\0';
    long len_val = strtol(header + 1, NULL, 10);
    if (len_val < 0) {
        printf(CLR_YELLOW "(nil)\n" CLR_RESET);
        return 0;
    }

    printf(CLR_CYAN "(bulk length) %ld bytes\n" CLR_RESET, len_val);

    FILE *fp = fopen(save_path, "wb");
    if (!fp) {
        perror("fopen");
        return -1;
    }

    // 2) 精确接收 len_val 字节正文
    long remaining = len_val;
    char buf[8192];
    while (remaining > 0) {
        size_t chunk = (remaining > (long)sizeof(buf)) ? sizeof(buf) : (size_t)remaining;
        ssize_t r = recv(fd, buf, chunk, 0);
        if (r <= 0) {
            perror("recv body");
            fclose(fp);
            return -1;
        }
        size_t w = fwrite(buf, 1, (size_t)r, fp);
        if (w != (size_t)r) {
            fprintf(stderr, CLR_RED "Failed to write full bulk to file\n" CLR_RESET);
            fclose(fp);
            return -1;
        }
        remaining -= (long)r;
    }

    // 3) 读掉结尾的 "\r\n"
    char tail[2];
    size_t t_used = 0;
    while (t_used < 2) {
        ssize_t r = recv(fd, tail + t_used, 2 - t_used, 0);
        if (r <= 0) {
            perror("recv crlf");
            fclose(fp);
            return -1;
        }
        t_used += (size_t)r;
    }

    fclose(fp);
    return 0;
}

// 支持大 bulk 的通用读响应：循环 recv + 动态缓冲
// 普通命令使用此函数打印到终端
static int read_and_print_reply_with_save(int fd, const char *save_path) {
    size_t cap = 8192;
    char *buf = (char *)malloc(cap);
    if (!buf) {
        perror("malloc");
        return -1;
    }
    size_t n = 0;

    while (1) {
        if (n == cap) {
            cap *= 2;
            char *nbuf = (char *)realloc(buf, cap);
            if (!nbuf) {
                perror("realloc");
                free(buf);
                return -1;
            }
            buf = nbuf;
        }

        ssize_t r = recv(fd, buf + n, cap - n, 0);
        if (r < 0) {
            perror("recv");
            free(buf);
            return -1;
        }
        if (r == 0) {
            if (n == 0) {
                printf(CLR_YELLOW "(connection closed by server)\n" CLR_RESET);
                free(buf);
                return -1;
            }
            break;
        }

        n += (size_t)r;

        // 简单结束条件：本次 recv 填不满剩余缓冲，认为服务器暂时写完了
        if ((size_t)r < cap - (n - (size_t)r)) {
            break;
        }
    }

    buf[n] = '\0';

    if (n == 0) {
        free(buf);
        return 0;
    }

    char *p = buf;
    char type = *p++;

    char *cr = strchr(p, '\r');
    if (!cr) {
        fwrite(buf, 1, n, stdout);
        free(buf);
        return 0;
    }

    switch (type) {
    case '+': { // Simple String
        *cr = '\0';
        printf(CLR_GREEN "%s\n" CLR_RESET, p);
        break;
    }
    case '-': { // Error
        *cr = '\0';
        printf(CLR_RED "(error) %s\n" CLR_RESET, p);
        break;
    }
    case ':': { // Integer
        *cr = '\0';
        printf(CLR_CYAN "(integer) %s\n" CLR_RESET, p);
        break;
    }
    case '$': { // Bulk String
        *cr = '\0';
        long len_val = strtol(p, NULL, 10);
        if (len_val < 0) {
            printf(CLR_YELLOW "(nil)\n" CLR_RESET);
            break;
        }
        char *data = cr + 2; // 跳过 \r\n

        if (save_path) {
            // 只输出长度，并将数据保存到文件
            printf(CLR_CYAN "(bulk length) %ld bytes\n" CLR_RESET, len_val);
            if (data + len_val <= buf + n) {
                FILE *fp = fopen(save_path, "wb");
                if (!fp) {
                    perror("fopen");
                } else {
                    size_t written = fwrite(data, 1, (size_t)len_val, fp);
                    if (written != (size_t)len_val) {
                        fprintf(stderr, CLR_RED "Failed to write full bulk to file" CLR_RESET "\n");
                    }
                    fclose(fp);
                }
            } else {
                // 当前实现简化：若一次未收全，则原样打印调试
                fwrite(buf, 1, n, stdout);
            }
        } else {
            // 旧行为：直接打印内容
            if (data + len_val <= buf + n) {
                printf(CLR_BOLD "%.*s\n" CLR_RESET, (int)len_val, data);
            } else {
                fwrite(buf, 1, n, stdout);
            }
        }
        break;
    }
    case '*': { // Array
        print_resp_array(buf, (int)n, p, cr);
        break;
    }
    default:
        fwrite(buf, 1, n, stdout);
        break;
    }

    free(buf);
    return 0;
}

static int read_and_print_reply(int fd) {
    return read_and_print_reply_with_save(fd, NULL);
}

/*==================== HELP ====================*/

static void print_help_one(const char *cmd) {
    for (size_t i = 0; i < g_help_count; ++i) {
        if (!strcasecmp(cmd, g_help_table[i].name)) {
            printf(CLR_BOLD "%s" CLR_RESET ": %s\n",
                   g_help_table[i].name, g_help_table[i].summary);
            printf("Usage: %s\n", g_help_table[i].usage);
            return;
        }
    }
    printf(CLR_YELLOW "(no help for '%s')\n" CLR_RESET, cmd);
}

static void print_help_all(void) {
    printf("Supported commands:\n");
    for (size_t i = 0; i < g_help_count; ++i) {
        printf("  %-8s - %s\n", g_help_table[i].name, g_help_table[i].summary);
    }
    printf("Type " CLR_BOLD "HELP <command>" CLR_RESET " for details.\n");
}

/*==================== readline 补全 ====================*/

// 从命令表中找补全项
static char *cli_cmd_generator(const char *text, int state) {
    static size_t list_index;
    static size_t len;

    if (!state) { // 第一次调用，初始化
        list_index = 0;
        len = strlen(text);
    }

    while (list_index < g_help_count) {
        const char *name = g_help_table[list_index].name;
        list_index++;
        if (strncasecmp(name, text, len) == 0) {
            return strdup(name); // readline 会负责释放
        }
    }
    return NULL;
}

// 顶层补全函数：仅在第一个单词时补全命令
static char **cli_completion(const char *text, int start, int end) {
    (void)end;
    if (start > 0) {
        return NULL;
    }
    return rl_completion_matches(text, cli_cmd_generator);
}

/*==================== 主流程 ====================*/

int main(int argc, char *argv[]) {
    if (argc < 3) {
        print_usage(argv[0]);
        return 1;
    }
    const char *ip = argv[1];
    unsigned short port = (unsigned short)atoi(argv[2]);

    const char *one_cmd = NULL;
    if (argc >= 5 && strcmp(argv[3], "-c") == 0) {
        one_cmd = argv[4];
    }

    int fd = connect_server(ip, port);
    if (fd < 0) return 1;

    int stdin_is_tty = isatty(STDIN_FILENO);

    if (!one_cmd && stdin_is_tty) {
        printf("Connected to kvstore %s:%d\n", ip, port);
        printf("Type commands like: " CLR_BOLD "SET key value" CLR_RESET ", "
               CLR_BOLD "GET key" CLR_RESET ", ...\n");
        printf("Type '" CLR_BOLD "EXIT" CLR_RESET "' to exit client, ");
        printf("'" CLR_BOLD "SQUIT" CLR_RESET "' to send QUIT to server.\n");
        printf("Type '" CLR_BOLD "HELP" CLR_RESET "' for help.\n");

        rl_attempted_completion_function = cli_completion;
    }

    char line_buf[BUF_SIZE];
    char *tokens[64];

    /*------------- 单条命令模式：-c "CMD ..." -------------*/
    if (one_cmd) {
        strncpy(line_buf, one_cmd, sizeof(line_buf)-1);
        line_buf[sizeof(line_buf)-1] = '\0';

        int argcnt = split_line(line_buf, tokens, (int)(sizeof(tokens)/sizeof(tokens[0])));
        if (argcnt <= 0) {
            close(fd);
            return 0;
        }

        // 本地 HELP
        if (!strcasecmp(tokens[0], "HELP")) {
            if (argcnt == 1) print_help_all();
            else print_help_one(tokens[1]);
            close(fd);
            return 0;
        }

        // 本地 EXIT
        if (!strcasecmp(tokens[0], "EXIT")) {
            close(fd);
            return 0;
        }

        // PAPERSET key file
        if (!strcasecmp(tokens[0], "PAPERSET")) {
            if (argcnt != 3) {
                fprintf(stderr, CLR_RED "Usage: PAPERSET key /path/to/file\n" CLR_RESET);
                close(fd);
                return 1;
            }
            const char *key  = tokens[1];
            const char *path = tokens[2];

            char  *file_data = NULL;
            size_t file_len  = 0;
            if (read_file_all(path, &file_data, &file_len) != 0) {
                fprintf(stderr, CLR_RED "Failed to read file '%s'\n" CLR_RESET, path);
                close(fd);
                return 1;
            }

            char  *req = NULL;
            size_t req_len = 0;
            if (build_paperset_request(key, file_data, file_len, &req, &req_len) != 0) {
                fprintf(stderr, CLR_RED "Failed to build PAPERSET request (maybe file too large)\n" CLR_RESET);
                free(file_data);
                close(fd);
                return 1;
            }
            free(file_data);

            size_t sent = 0;
            while (sent < req_len) {
                ssize_t n = send(fd, req + sent, req_len - sent, 0);
                if (n <= 0) {
                    perror("send");
                    free(req);
                    close(fd);
                    return 1;
                }
                sent += (size_t)n;
            }
            free(req);

            (void)read_and_print_reply(fd);
            close(fd);
            return 0;
        }

        // PAPERMOD key file
        if (!strcasecmp(tokens[0], "PAPERMOD")) {
            if (argcnt != 3) {
                fprintf(stderr, CLR_RED "Usage: PAPERMOD key /path/to/file\n" CLR_RESET);
                close(fd);
                return 1;
            }
            const char *key  = tokens[1];
            const char *path = tokens[2];

            char  *file_data = NULL;
            size_t file_len  = 0;
            if (read_file_all(path, &file_data, &file_len) != 0) {
                fprintf(stderr, CLR_RED "Failed to read file '%s'\n" CLR_RESET, path);
                close(fd);
                return 1;
            }

            char  *req = NULL;
            size_t req_len = 0;
            if (build_papermod_request(key, file_data, file_len, &req, &req_len) != 0) {
                fprintf(stderr, CLR_RED "Failed to build PAPERMOD request (maybe file too large)\n" CLR_RESET);
                free(file_data);
                close(fd);
                return 1;
            }
            free(file_data);

            size_t sent = 0;
            while (sent < req_len) {
                ssize_t n = send(fd, req + sent, req_len - sent, 0);
                if (n <= 0) {
                    perror("send");
                    free(req);
                    close(fd);
                    return 1;
                }
                sent += (size_t)n;
            }
            free(req);

            (void)read_and_print_reply(fd);
            close(fd);
            return 0;
        }

        // PAPERGET key /path/to/save
        if (!strcasecmp(tokens[0], "PAPERGET")) {
            if (argcnt != 3) {
                fprintf(stderr, CLR_RED "Usage: PAPERGET key /path/to/save\n" CLR_RESET);
                close(fd);
                return 1;
            }
            const char *key  = tokens[1];
            const char *path = tokens[2];

            char *argv_local[2];
            argv_local[0] = "PAPERGET";
            argv_local[1] = (char *)key;

            char req[BUF_SIZE];
            int req_len = build_resp_request(argv_local, 2, req, sizeof(req));
            if (req_len < 0) {
                fprintf(stderr, CLR_RED "Error: PAPERGET build RESP failed\n" CLR_RESET);
                close(fd);
                return 1;
            }

            int sent = 0;
            while (sent < req_len) {
                int n = send(fd, req + sent, req_len - sent, 0);
                if (n <= 0) {
                    perror("send");
                    close(fd);
                    return 1;
                }
                sent += n;
            }

            (void)read_paperget_reply(fd, path);
            close(fd);
            return 0;
        }

        // SQUIT -> QUIT
        if (!strcasecmp(tokens[0], "SQUIT")) {
            tokens[0] = "QUIT";
        }

        char req[BUF_SIZE];
        int req_len = build_resp_request(tokens, argcnt, req, sizeof(req));
        if (req_len < 0) {
            fprintf(stderr, CLR_RED "Error: command too long or build RESP failed\n" CLR_RESET);
            close(fd);
            return 1;
        }

        int sent = 0;
        while (sent < req_len) {
            int n = send(fd, req + sent, req_len - sent, 0);
            if (n <= 0) {
                perror("send");
                close(fd);
                return 1;
            }
            sent += n;
        }

        (void)read_and_print_reply(fd);
        close(fd);
        return 0;
    }

    /*------------- 交互模式 or 管道模式 -------------*/

    if (stdin_is_tty) {
        // 交互模式：readline
        while (1) {
            char prompt[64];
            snprintf(prompt, sizeof(prompt), "kv> ");
            char *line = readline(prompt);
            if (!line) {
                break;
            }

            if (line[0] == '\0') {
                free(line);
                continue;
            }

            add_history(line);

            strncpy(line_buf, line, sizeof(line_buf)-1);
            line_buf[sizeof(line_buf)-1] = '\0';
            free(line);

            int argcnt = split_line(line_buf, tokens, (int)(sizeof(tokens)/sizeof(tokens[0])));
            if (argcnt <= 0) continue;

            // 客户端退出
            if (!strcasecmp(tokens[0], "EXIT") ||
                (!strcasecmp(tokens[0], "quit") && argcnt == 1) ||
                (!strcasecmp(tokens[0], "exit") && argcnt == 1)) {
                break;
            }

            // HELP
            if (!strcasecmp(tokens[0], "HELP")) {
                if (argcnt == 1) print_help_all();
                else print_help_one(tokens[1]);
                continue;
            }

            // PAPERGET key /path/to/save
            if (!strcasecmp(tokens[0], "PAPERGET")) {
                if (argcnt != 3) {
                    fprintf(stderr, CLR_RED "Usage: PAPERGET key /path/to/save\n" CLR_RESET);
                    continue;
                }
                const char *key  = tokens[1];
                const char *path = tokens[2];

                char *argv_local[2];
                argv_local[0] = "PAPERGET";
                argv_local[1] = (char *)key;

                char req[BUF_SIZE];
                int req_len = build_resp_request(argv_local, 2, req, sizeof(req));
                if (req_len < 0) {
                    fprintf(stderr, CLR_RED "Error: PAPERGET build RESP failed\n" CLR_RESET);
                    continue;
                }

                int sent = 0;
                while (sent < req_len) {
                    int n = send(fd, req + sent, req_len - sent, 0);
                    if (n <= 0) {
                        perror("send");
                        close(fd);
                        return 1;
                    }
                    sent += n;
                }

                if (read_paperget_reply(fd, path) != 0) {
                    break;
                }
                continue;
            }

            // PAPERSET key file
            if (!strcasecmp(tokens[0], "PAPERSET")) {
                if (argcnt != 3) {
                    fprintf(stderr, CLR_RED "Usage: PAPERSET key /path/to/file\n" CLR_RESET);
                    continue;
                }
                const char *key  = tokens[1];
                const char *path = tokens[2];

                char  *file_data = NULL;
                size_t file_len  = 0;
                if (read_file_all(path, &file_data, &file_len) != 0) {
                    fprintf(stderr, CLR_RED "Failed to read file '%s'\n" CLR_RESET, path);
                    continue;
                }

                char  *req = NULL;
                size_t req_len = 0;
                if (build_paperset_request(key, file_data, file_len, &req, &req_len) != 0) {
                    fprintf(stderr, CLR_RED "Failed to build PAPERSET request (maybe file too large)\n" CLR_RESET);
                    free(file_data);
                    continue;
                }
                free(file_data);

                size_t sent = 0;
                while (sent < req_len) {
                    ssize_t n = send(fd, req + sent, req_len - sent, 0);
                    if (n <= 0) {
                        perror("send");
                        free(req);
                        close(fd);
                        return 1;
                    }
                    sent += (size_t)n;
                }
                free(req);

                if (read_and_print_reply(fd) != 0) {
                    break;
                }
                continue;
            }

            // PAPERMOD key file
            if (!strcasecmp(tokens[0], "PAPERMOD")) {
                if (argcnt != 3) {
                    fprintf(stderr, CLR_RED "Usage: PAPERMOD key /path/to/file\n" CLR_RESET);
                    continue;
                }
                const char *key  = tokens[1];
                const char *path = tokens[2];

                char  *file_data = NULL;
                size_t file_len  = 0;
                if (read_file_all(path, &file_data, &file_len) != 0) {
                    fprintf(stderr, CLR_RED "Failed to read file '%s'\n" CLR_RESET, path);
                    continue;
                }

                char  *req = NULL;
                size_t req_len = 0;
                if (build_papermod_request(key, file_data, file_len, &req, &req_len) != 0) {
                    fprintf(stderr, CLR_RED "Failed to build PAPERMOD request (maybe file too large)\n" CLR_RESET);
                    free(file_data);
                    continue;
                }
                free(file_data);

                size_t sent = 0;
                while (sent < req_len) {
                    ssize_t n = send(fd, req + sent, req_len - sent, 0);
                    if (n <= 0) {
                        perror("send");
                        free(req);
                        close(fd);
                        return 1;
                    }
                    sent += (size_t)n;
                }
                free(req);

                if (read_and_print_reply(fd) != 0) {
                    break;
                }
                continue;
            }

            // SQUIT -> QUIT
            if (!strcasecmp(tokens[0], "SQUIT")) {
                tokens[0] = "QUIT";
            }

            char req[BUF_SIZE];
            int req_len = build_resp_request(tokens, argcnt, req, sizeof(req));
            if (req_len < 0) {
                fprintf(stderr, CLR_RED "Error: command too long or build RESP failed\n" CLR_RESET);
                continue;
            }

            int sent = 0;
            while (sent < req_len) {
                int n = send(fd, req + sent, req_len - sent, 0);
                if (n <= 0) {
                    perror("send");
                    close(fd);
                    return 1;
                }
                sent += n;
            }

            if (read_and_print_reply(fd) != 0) {
                break;
            }
        }
    } else {
        // 管道模式：按行读取 stdin
        while (1) {
            if (!fgets(line_buf, sizeof(line_buf), stdin)) {
                break;
            }

            if (line_buf[0] == '\n' || line_buf[0] == '\r') {
                continue;
            }

            int argcnt = split_line(line_buf, tokens, (int)(sizeof(tokens)/sizeof(tokens[0])));
            if (argcnt <= 0) continue;

            // HELP
            if (!strcasecmp(tokens[0], "HELP")) {
                if (argcnt == 1) print_help_all();
                else print_help_one(tokens[1]);
                continue;
            }

            // EXIT / quit / exit
            if (!strcasecmp(tokens[0], "EXIT") ||
                (!strcasecmp(tokens[0], "quit") && argcnt == 1) ||
                (!strcasecmp(tokens[0], "exit") && argcnt == 1)) {
                break;
            }

            // SAVEHUGE key file
            if (!strcasecmp(tokens[0], "SAVEHUGE")) {
                fprintf(stderr, CLR_RED "SAVEHUGE is no longer supported. Use PAPERSET / PAPERMOD instead.\n" CLR_RESET);
                continue;
            }

            // SQUIT -> QUIT
            if (!strcasecmp(tokens[0], "SQUIT")) {
                tokens[0] = "QUIT";
            }

            char req[BUF_SIZE];
            int req_len = build_resp_request(tokens, argcnt, req, sizeof(req));
            if (req_len < 0) {
                fprintf(stderr, CLR_RED "Error: command too long or build RESP failed\n" CLR_RESET);
                continue;
            }

            int sent = 0;
            while (sent < req_len) {
                int n = send(fd, req + sent, req_len - sent, 0);
                if (n <= 0) {
                    perror("send");
                    close(fd);
                    return 1;
                }
                sent += n;
            }

            if (read_and_print_reply(fd) != 0) {
                break;
            }
        }
    }

    close(fd);
    return 0;
}