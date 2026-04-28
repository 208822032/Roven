#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <errno.h>
#include <unistd.h>
#include <arpa/inet.h>
#include <sys/socket.h>
#include <sys/time.h>

#define MAX_RESP_LEN 8192

static int send_all(int fd, const char *buf, int len) {
    int sent = 0;

    while (sent < len) {
        int n = send(fd, buf + sent, len - sent, 0);
        if (n < 0) {
            if (errno == EINTR) {
                continue;
            }
            perror("send");
            return -1;
        }

        if (n == 0) {
            fprintf(stderr, "send_all: connection closed\n");
            return -1;
        }

        sent += n;
    }

    return sent;
}

static int read_line_len(const char *buf, int len) {
    int i;

    for (i = 0; i + 1 < len; ++i) {
        if (buf[i] == '\r' && buf[i + 1] == '\n') {
            return i + 2;
        }
    }

    return 0;
}

static int parse_int_line(const char *buf, int len, long long *value, int *line_len) {
    int l;
    int i = 0;
    int sign = 1;
    long long v = 0;

    l = read_line_len(buf, len);
    if (l == 0) {
        return 0;
    }

    if (l < 3) {
        return -1;
    }

    if (buf[0] == '-') {
        sign = -1;
        i = 1;
    } else if (buf[0] == '+') {
        i = 1;
    }

    if (i >= l - 2) {
        return -1;
    }

    for (; i < l - 2; ++i) {
        if (buf[i] < '0' || buf[i] > '9') {
            return -1;
        }
        v = v * 10 + (buf[i] - '0');
    }

    if (value) {
        *value = v * sign;
    }

    if (line_len) {
        *line_len = l;
    }

    return 1;
}

/*
 * 解析一个 RESP 对象。
 *
 * 返回：
 * 1  表示完整对象
 * 0  表示数据还不够
 * -1 表示协议错误
 */
static int parse_resp_object(const char *buf, int len, int *consumed) {
    char type;
    int used = 0;
    int line_len = 0;
    long long num = 0;
    int ret;
    int i;

    if (len <= 0 || !buf || !consumed) {
        return 0;
    }

    type = buf[0];
    used = 1;

    switch (type) {
        case '+':
        case '-':
        case ':': {
            int l = read_line_len(buf + 1, len - 1);
            if (l == 0) {
                return 0;
            }
            *consumed = 1 + l;
            return 1;
        }

        case '$': {
            ret = parse_int_line(buf + 1, len - 1, &num, &line_len);
            if (ret <= 0) {
                return ret;
            }

            used += line_len;

            if (num < 0) {
                *consumed = used;
                return 1;
            }

            if (len < used + (int)num + 2) {
                return 0;
            }

            if (buf[used + num] != '\r' || buf[used + num + 1] != '\n') {
                return -1;
            }

            *consumed = used + (int)num + 2;
            return 1;
        }

        case '*': {
            ret = parse_int_line(buf + 1, len - 1, &num, &line_len);
            if (ret <= 0) {
                return ret;
            }

            used += line_len;

            if (num < 0) {
                *consumed = used;
                return 1;
            }

            for (i = 0; i < num; ++i) {
                int child_consumed = 0;

                ret = parse_resp_object(buf + used, len - used, &child_consumed);
                if (ret <= 0) {
                    return ret;
                }

                used += child_consumed;
            }

            *consumed = used;
            return 1;
        }

        default:
            return -1;
    }
}

static int recv_full_resp(int fd, char *out, int out_cap) {
    int total = 0;

    if (!out || out_cap <= 1) {
        return -1;
    }

    while (1) {
        int consumed = 0;
        int ret;
        int n;

        n = recv(fd, out + total, out_cap - total - 1, 0);
        if (n < 0) {
            if (errno == EINTR) {
                continue;
            }
            perror("recv");
            return -1;
        }

        if (n == 0) {
            fprintf(stderr, "recv_full_resp: connection closed\n");
            return -1;
        }

        total += n;
        out[total] = '\0';

        ret = parse_resp_object(out, total, &consumed);
        if (ret < 0) {
            fprintf(stderr, "recv_full_resp: RESP parse error, raw=[%s]\n", out);
            return -1;
        }

        if (ret == 1) {
            out[consumed] = '\0';
            return consumed;
        }

        if (total >= out_cap - 1) {
            fprintf(stderr, "recv_full_resp: response buffer too small\n");
            return -1;
        }
    }
}

static int connect_tcpserver(const char *ip, unsigned short port) {
    int fd;
    struct sockaddr_in addr;

    fd = socket(AF_INET, SOCK_STREAM, 0);
    if (fd < 0) {
        perror("socket");
        return -1;
    }

    memset(&addr, 0, sizeof(addr));
    addr.sin_family = AF_INET;
    addr.sin_addr.s_addr = inet_addr(ip);
    addr.sin_port = htons(port);

    if (connect(fd, (struct sockaddr *)&addr, sizeof(addr)) != 0) {
        perror("connect");
        close(fd);
        return -1;
    }

    return fd;
}

static void testcase_raw(int fd, const char *msg, const char *expected, const char *name) {
    char resp[MAX_RESP_LEN];

    memset(resp, 0, sizeof(resp));

    if (send_all(fd, msg, (int)strlen(msg)) < 0) {
        fprintf(stderr, "==> FAILED -> %s, send failed\n", name);
        exit(1);
    }

    if (recv_full_resp(fd, resp, sizeof(resp)) < 0) {
        fprintf(stderr, "==> FAILED -> %s, recv failed\n", name);
        exit(1);
    }

    if (strcmp(resp, expected) == 0) {
        printf("==> PASS -> %s\n", name);
        return;
    }

    printf("==> FAILED -> %s\n", name);
    printf("Expected:\n%s\n", expected);
    printf("Received:\n%s\n", resp);
    exit(1);
}

static void testcase_contains(int fd, const char *msg, const char *needle, const char *name) {
    char resp[MAX_RESP_LEN];

    memset(resp, 0, sizeof(resp));

    if (send_all(fd, msg, (int)strlen(msg)) < 0) {
        fprintf(stderr, "==> FAILED -> %s, send failed\n", name);
        exit(1);
    }

    if (recv_full_resp(fd, resp, sizeof(resp)) < 0) {
        fprintf(stderr, "==> FAILED -> %s, recv failed\n", name);
        exit(1);
    }

    if (strstr(resp, needle) != NULL) {
        printf("==> PASS -> %s\n", name);
        return;
    }

    printf("==> FAILED -> %s\n", name);
    printf("Expected response to contain:\n%s\n", needle);
    printf("Received:\n%s\n", resp);
    exit(1);
}

static void testcase_not_contains(int fd, const char *msg, const char *needle, const char *name) {
    char resp[MAX_RESP_LEN];

    memset(resp, 0, sizeof(resp));

    if (send_all(fd, msg, (int)strlen(msg)) < 0) {
        fprintf(stderr, "==> FAILED -> %s, send failed\n", name);
        exit(1);
    }

    if (recv_full_resp(fd, resp, sizeof(resp)) < 0) {
        fprintf(stderr, "==> FAILED -> %s, recv failed\n", name);
        exit(1);
    }

    if (strstr(resp, needle) == NULL) {
        printf("==> PASS -> %s\n", name);
        return;
    }

    printf("==> FAILED -> %s\n", name);
    printf("Expected response NOT to contain:\n%s\n", needle);
    printf("Received:\n%s\n", resp);
    exit(1);
}

static void testcase_ignore_response(int fd, const char *msg, const char *name) {
    char resp[MAX_RESP_LEN];

    memset(resp, 0, sizeof(resp));

    if (send_all(fd, msg, (int)strlen(msg)) < 0) {
        fprintf(stderr, "==> FAILED -> %s, send failed\n", name);
        exit(1);
    }

    if (recv_full_resp(fd, resp, sizeof(resp)) < 0) {
        fprintf(stderr, "==> FAILED -> %s, recv failed\n", name);
        exit(1);
    }

    printf("==> PASS -> %s, response ignored: %s", name, resp);
}

/*
 * V 族命令测试。
 *
 * 覆盖：
 * 1. VINFO 基础返回；
 * 2. VSET + VGET 精确命中；
 * 3. 重复 VSET 覆盖；
 * 4. scope 隔离；
 * 5. VDEL 删除；
 * 6. VINDEX REBUILD 清理 tombstone；
 * 7. VSEARCH 当前仍然 NOT IMPLEMENTED。
 */
static void vector_resp_testcase(int fd) {
    printf("========== vector testcase begin ==========\n");

    /*
     * VINFO 返回数组即可，这里只检查包含 capacity 字段。
     * 因为 total_records 可能受之前测试影响，不做固定值断言。
     */
    testcase_contains(
        fd,
        "*1\r\n$5\r\nVINFO\r\n",
        "capacity:",
        "VINFO-basic"
    );

    testcase_contains(
        fd,
        "*1\r\n$5\r\nVINFO\r\n",
        "dimension:64",
        "VINFO-vector-dimension-64"
    );

    testcase_contains(
        fd,
        "*1\r\n$5\r\nVINFO\r\n",
        "index_ready:1",
        "VINFO-index-ready"
    );

    /*
     * 清理可能残留的测试 key。
     * 如果不存在，当前实现会返回 NO EXIST。
     * 这里不使用 testcase_raw，避免残留状态影响测试启动。
     */
    testcase_ignore_response(
        fd,
        "*2\r\n$4\r\nVDEL\r\n$7\r\nvec_key\r\n",
        "VDEL-clean-vec_key-ignore-result"
    );

    testcase_ignore_response(
        fd,
        "*3\r\n$4\r\nVDEL\r\n$7\r\nvec_key\r\n$6\r\nscope1\r\n",
        "VDEL-clean-vec_key-scope1-ignore-result"
    );

    testcase_raw(
        fd,
        "*3\r\n$4\r\nVSET\r\n$7\r\nvec_key\r\n$10\r\nvec_answer\r\n",
        "+OK\r\n",
        "VSET-vec_key"
    );

    testcase_raw(
        fd,
        "*2\r\n$4\r\nVGET\r\n$7\r\nvec_key\r\n",
        "$10\r\nvec_answer\r\n",
        "VGET-vec_key"
    );

    /*
     * 同 question + 空 scope 覆盖 answer。
     */
    testcase_raw(
        fd,
        "*3\r\n$4\r\nVSET\r\n$7\r\nvec_key\r\n$11\r\nvec_answer2\r\n",
        "+OK\r\n",
        "VSET-vec_key-overwrite"
    );

    testcase_raw(
        fd,
        "*2\r\n$4\r\nVGET\r\n$7\r\nvec_key\r\n",
        "$11\r\nvec_answer2\r\n",
        "VGET-vec_key-overwritten"
    );

    /*
     * scope 隔离：
     * vec_key + scope1 和 vec_key + 空 scope 是两条记录。
     */
    testcase_raw(
        fd,
        "*4\r\n$4\r\nVSET\r\n$7\r\nvec_key\r\n$13\r\nscoped_answer\r\n$6\r\nscope1\r\n",
        "+OK\r\n",
        "VSET-vec_key-scope1"
    );

    testcase_raw(
        fd,
        "*3\r\n$4\r\nVGET\r\n$7\r\nvec_key\r\n$6\r\nscope1\r\n",
        "$13\r\nscoped_answer\r\n",
        "VGET-vec_key-scope1"
    );

    testcase_raw(
        fd,
        "*3\r\n$4\r\nVGET\r\n$7\r\nvec_key\r\n$6\r\nscope2\r\n",
        "+NO EXIST\r\n",
        "VGET-vec_key-scope2-not-exist"
    );

    /*
     * 删除 scope1 记录，不影响空 scope 记录。
     */
    testcase_raw(
        fd,
        "*3\r\n$4\r\nVDEL\r\n$7\r\nvec_key\r\n$6\r\nscope1\r\n",
        "+OK\r\n",
        "VDEL-vec_key-scope1"
    );

    testcase_raw(
        fd,
        "*3\r\n$4\r\nVGET\r\n$7\r\nvec_key\r\n$6\r\nscope1\r\n",
        "+NO EXIST\r\n",
        "VGET-vec_key-scope1-after-del"
    );

    testcase_raw(
        fd,
        "*2\r\n$4\r\nVGET\r\n$7\r\nvec_key\r\n",
        "$11\r\nvec_answer2\r\n",
        "VGET-vec_key-empty-scope-still-exists"
    );

    /*
     * 删除空 scope 记录。
     */
    testcase_raw(
        fd,
        "*2\r\n$4\r\nVDEL\r\n$7\r\nvec_key\r\n",
        "+OK\r\n",
        "VDEL-vec_key"
    );

    testcase_raw(
        fd,
        "*2\r\n$4\r\nVGET\r\n$7\r\nvec_key\r\n",
        "+NO EXIST\r\n",
        "VGET-vec_key-after-del"
    );

    /*
     * 删除不存在记录应该返回 NO EXIST。
     */
    testcase_raw(
        fd,
        "*2\r\n$4\r\nVDEL\r\n$7\r\nvec_key\r\n",
        "+NO EXIST\r\n",
        "VDEL-vec_key-not-exist"
    );

    /*
     * 删除后 needs_rebuild 应该为 1。
     */
    testcase_contains(
        fd,
        "*1\r\n$5\r\nVINFO\r\n",
        "needs_rebuild:1",
        "VINFO-needs-rebuild-after-vdel"
    );

    testcase_contains(
        fd,
        "*1\r\n$5\r\nVINFO\r\n",
        "active_records:0",
        "VINFO-active-records-zero-after-vdel"
    );

    testcase_contains(
        fd,
        "*1\r\n$5\r\nVINFO\r\n",
        "deleted_records:2",
        "VINFO-deleted-records-two-after-vdel"
    );

    testcase_contains(
        fd,
        "*1\r\n$5\r\nVINFO\r\n",
        "total_records:2",
        "VINFO-total-records-two-before-rebuild"
    );

    testcase_contains(
        fd,
        "*1\r\n$5\r\nVINFO\r\n",
        "index_size:0",
        "VINFO-logical-index-size-zero-after-vdel"
    );

    /*
     * VINDEX REBUILD 清理 tombstone。
     */
    testcase_raw(
        fd,
        "*2\r\n$6\r\nVINDEX\r\n$7\r\nREBUILD\r\n",
        "+OK\r\n",
        "VINDEX-REBUILD"
    );

    testcase_contains(
        fd,
        "*1\r\n$5\r\nVINFO\r\n",
        "needs_rebuild:0",
        "VINFO-needs-rebuild-cleared"
    );

    testcase_contains(
        fd,
        "*1\r\n$5\r\nVINFO\r\n",
        "deleted_records:0",
        "VINFO-deleted-records-cleared"
    );

    testcase_contains(
        fd,
        "*1\r\n$5\r\nVINFO\r\n",
        "active_records:0",
        "VINFO-active-records-zero-after-rebuild"
    );

    testcase_contains(
        fd,
        "*1\r\n$5\r\nVINFO\r\n",
        "total_records:0",
        "VINFO-total-records-zero-after-rebuild"
    );

    testcase_contains(
        fd,
        "*1\r\n$5\r\nVINFO\r\n",
        "index_size:0",
        "VINFO-index-size-zero-after-rebuild"
    );

    /*
    * VSEARCH MVP：字符级检索。
    */
    testcase_ignore_response(
        fd,
        "*2\r\n$4\r\nVDEL\r\n$10\r\nsearch_key\r\n",
        "VDEL-clean-search-key-ignore-result"
    );

    testcase_ignore_response(
        fd,
        "*2\r\n$4\r\nVDEL\r\n$11\r\nsearch_key2\r\n",
        "VDEL-clean-search-key2-ignore-result"
    );

    testcase_ignore_response(
        fd,
        "*3\r\n$4\r\nVDEL\r\n$12\r\nscoped_query\r\n$7\r\nvscope1\r\n",
        "VDEL-clean-scoped-query-vscope1-ignore-result"
    );

    testcase_ignore_response(
        fd,
        "*3\r\n$4\r\nVDEL\r\n$12\r\nscoped_query\r\n$7\r\nvscope2\r\n",
        "VDEL-clean-scoped-query-vscope2-ignore-result"
    );

    /*
    * 清理 search 相关 tombstone，保证后续 index_size 断言稳定。
    */
    testcase_raw(
        fd,
        "*2\r\n$6\r\nVINDEX\r\n$7\r\nREBUILD\r\n",
        "+OK\r\n",
        "VINDEX-REBUILD-before-search-index-tests"
    );

    testcase_raw(
        fd,
        "*3\r\n$4\r\nVSET\r\n$10\r\nsearch_key\r\n$13\r\nsearch_answer\r\n",
        "+OK\r\n",
        "VSET-search-key"
    );

    testcase_raw(
        fd,
        "*3\r\n$4\r\nVSET\r\n$11\r\nsearch_key2\r\n$14\r\nsearch_answer2\r\n",
        "+OK\r\n",
        "VSET-search-key2"
    );

    testcase_contains(
        fd,
        "*1\r\n$5\r\nVINFO\r\n",
        "dimension:64",
        "VINFO-after-vset-dimension-64"
    );

    testcase_contains(
        fd,
        "*1\r\n$5\r\nVINFO\r\n",
        "index_ready:1",
        "VINFO-after-vset-index-ready"
    );

    testcase_contains(
        fd,
        "*1\r\n$5\r\nVINFO\r\n",
        "index_size:2",
        "VINFO-after-vset-index-size-2"
    );

    testcase_contains(
        fd,
        "*1\r\n$5\r\nVINFO\r\n",
        "active_records:2",
        "VINFO-after-vset-active-records-2"
    );

    testcase_contains(
        fd,
        "*1\r\n$5\r\nVINFO\r\n",
        "deleted_records:0",
        "VINFO-after-vset-deleted-records-0"
    );

    testcase_contains(
        fd,
        "*1\r\n$5\r\nVINFO\r\n",
        "total_records:2",
        "VINFO-after-vset-total-records-2"
    );

    testcase_contains(
        fd,
        "*2\r\n$7\r\nVSEARCH\r\n$10\r\nsearch_key\r\n",
        "question:search_key",
        "VSEARCH-basic-contains-question"
    );

    testcase_contains(
        fd,
        "*2\r\n$7\r\nVSEARCH\r\n$10\r\nsearch_key\r\n",
        "answer:search_answer",
        "VSEARCH-basic-contains-answer"
    );

    /*
    * 新协议增强：
    * VSEARCH query topk
    *
    * 这里第三个参数 1 应该被解析成 topk，而不是 scope。
    * 返回数组头应该是 *1。
    */
    testcase_contains(
        fd,
        "*3\r\n$7\r\nVSEARCH\r\n$6\r\nsearch\r\n$1\r\n1\r\n",
        "*1\r\n",
        "VSEARCH-query-topk-array-size"
    );

    testcase_contains(
        fd,
        "*3\r\n$7\r\nVSEARCH\r\n$6\r\nsearch\r\n$1\r\n1\r\n",
        "score:",
        "VSEARCH-query-topk-score"
    );

    /*
    * scope 检索：
    * 同一个 query，不同 scope 应该只返回对应 scope 的记录。
    */
    testcase_raw(
        fd,
        "*4\r\n$4\r\nVSET\r\n$12\r\nscoped_query\r\n$14\r\nscoped_answer1\r\n$7\r\nvscope1\r\n",
        "+OK\r\n",
        "VSET-scoped-query-vscope1"
    );

    testcase_raw(
        fd,
        "*4\r\n$4\r\nVSET\r\n$12\r\nscoped_query\r\n$14\r\nscoped_answer2\r\n$7\r\nvscope2\r\n",
        "+OK\r\n",
        "VSET-scoped-query-vscope2"
    );

    testcase_contains(
        fd,
        "*4\r\n$7\r\nVSEARCH\r\n$12\r\nscoped_query\r\n$7\r\nvscope1\r\n$1\r\n1\r\n",
        "answer:scoped_answer1",
        "VSEARCH-scope-vscope1-contains-answer1"
    );

    testcase_not_contains(
        fd,
        "*4\r\n$7\r\nVSEARCH\r\n$12\r\nscoped_query\r\n$7\r\nvscope1\r\n$1\r\n1\r\n",
        "answer:scoped_answer2",
        "VSEARCH-scope-vscope1-not-answer2"
    );

    testcase_contains(
        fd,
        "*4\r\n$7\r\nVSEARCH\r\n$12\r\nscoped_query\r\n$7\r\nvscope2\r\n$1\r\n1\r\n",
        "answer:scoped_answer2",
        "VSEARCH-scope-vscope2-contains-answer2"
    );

    /*
    * threshold 过滤：
    * exact match 分数是 1.0，所以 threshold=0.9 应该保留。
    */
    testcase_contains(
        fd,
        "*5\r\n$7\r\nVSEARCH\r\n$12\r\nscoped_query\r\n$7\r\nvscope1\r\n$1\r\n1\r\n$3\r\n0.9\r\n",
        "answer:scoped_answer1",
        "VSEARCH-threshold-keep-exact"
    );

    /*
    * threshold 过高：
    * 当前最高分是 1.0，threshold=1.1 应该返回空数组。
    */
    testcase_raw(
        fd,
        "*5\r\n$7\r\nVSEARCH\r\n$12\r\nscoped_query\r\n$7\r\nvscope1\r\n$1\r\n1\r\n$3\r\n1.1\r\n",
        "*0\r\n",
        "VSEARCH-threshold-filter-all"
    );

    /*
    * 删除后，VSEARCH 不应该再返回 deleted record。
    */
    testcase_raw(
        fd,
        "*3\r\n$4\r\nVDEL\r\n$12\r\nscoped_query\r\n$7\r\nvscope1\r\n",
        "+OK\r\n",
        "VDEL-scoped-query-vscope1"
    );

    testcase_contains(
        fd,
        "*1\r\n$5\r\nVINFO\r\n",
        "needs_rebuild:1",
        "VINFO-after-scoped-delete-needs-rebuild"
    );

    testcase_contains(
        fd,
        "*1\r\n$5\r\nVINFO\r\n",
        "deleted_records:1",
        "VINFO-after-scoped-delete-deleted-records-1"
    );

    testcase_contains(
        fd,
        "*1\r\n$5\r\nVINFO\r\n",
        "active_records:3",
        "VINFO-after-scoped-delete-active-records-3"
    );

    testcase_contains(
        fd,
        "*1\r\n$5\r\nVINFO\r\n",
        "total_records:4",
        "VINFO-after-scoped-delete-total-records-4"
    );

    testcase_contains(
        fd,
        "*1\r\n$5\r\nVINFO\r\n",
        "index_size:3",
        "VINFO-after-scoped-delete-logical-index-size-3"
    );

    testcase_raw(
        fd,
        "*4\r\n$7\r\nVSEARCH\r\n$12\r\nscoped_query\r\n$7\r\nvscope1\r\n$1\r\n1\r\n",
        "*0\r\n",
        "VSEARCH-after-delete-empty"
    );

    /*
    * 空结果：
    * 完全不存在的 query 应该返回空数组。
    */
    testcase_raw(
        fd,
        "*2\r\n$7\r\nVSEARCH\r\n$18\r\nnot_existing_query\r\n",
        "*0\r\n",
        "VSEARCH-not-existing-empty"
    );

    printf("========== vector testcase pass ==========\n");
}

int main(int argc, char *argv[]) {
    const char *ip = "127.0.0.1";
    unsigned short port = 9300;
    int fd;

    if (argc >= 2) {
        ip = argv[1];
    }

    if (argc >= 3) {
        port = (unsigned short)atoi(argv[2]);
    }

    fd = connect_tcpserver(ip, port);
    if (fd < 0) {
        return 1;
    }

    printf("Connected to kvstore %s:%u\n", ip, port);

    vector_resp_testcase(fd);

    close(fd);
    return 0;
}