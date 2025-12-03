#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/time.h>
#include <arpa/inet.h>
#include <unistd.h>

#define TIME_SUB_MS(tv1, tv2)  ((tv1.tv_sec - tv2.tv_sec) * 1000 + (tv1.tv_usec - tv2.tv_usec) / 1000)

static int send_all(int fd, const char *buf, int len) {
    int sent = 0;
    while (sent < len) {
        int n = send(fd, buf + sent, len - sent, 0);
        if (n <= 0) return n;
        sent += n;
    }
    return sent;
}

static int recv_once(int fd, char *buf, int len) {
    int n = recv(fd, buf, len, 0);
    return n;
}

static int connect_tcpserver(const char *ip, unsigned short port) {
    int fd = socket(AF_INET, SOCK_STREAM, 0);
    if (fd < 0) {
        perror("socket");
        return -1;
    }

    struct sockaddr_in addr;
    memset(&addr, 0, sizeof(addr));
    addr.sin_family = AF_INET;
    addr.sin_addr.s_addr = inet_addr(ip);
    addr.sin_port = htons(port);

    if (connect(fd, (struct sockaddr*)&addr, sizeof(addr)) != 0) {
        perror("connect");
        close(fd);
        return -1;
    }
    return fd;
}

static void bench_array_set_qps(int fd, int count) {
    char cmd[256];
    char key[64];
    char buf[128];
    struct timeval tv_begin, tv_end;

    gettimeofday(&tv_begin, NULL);
    for (int i = 0; i < count; ++i) {
        // key: arr_w000000
        int klen = snprintf(key, sizeof(key), "arr_w%06d", i);
        int n = snprintf(cmd, sizeof(cmd),
                         "*3\r\n"
                         "$3\r\nSET\r\n"
                         "$%d\r\n%s\r\n"
                         "$5\r\nvalue\r\n",
                         klen, key);
        send_all(fd, cmd, n);
        recv_once(fd, buf, sizeof(buf));
    }
    gettimeofday(&tv_end, NULL);

    int time_used = TIME_SUB_MS(tv_end, tv_begin);
    double qps = (double)count * 1000.0 / (double)time_used;
    printf("array write qps: time_used=%d ms, count=%d, qps=%.2f\n", time_used, count, qps);

    // 删除数据
    for (int i = 0; i < count; ++i) {
        int klen = snprintf(key, sizeof(key), "arr_w%06d", i);
        int n = snprintf(cmd, sizeof(cmd),
                         "*2\r\n"
                         "$3\r\nDEL\r\n"
                         "$%d\r\n%s\r\n",
                         klen, key);
        send_all(fd, cmd, n);
        recv_once(fd, buf, sizeof(buf));
    }
}

static void bench_rbtree_set_qps(int fd, int count) {
    char cmd[256];
    char key[64];
    char buf[128];
    struct timeval tv_begin, tv_end;

    gettimeofday(&tv_begin, NULL);
    for (int i = 0; i < count; ++i) {
        int klen = snprintf(key, sizeof(key), "rt_w%06d", i);
        int n = snprintf(cmd, sizeof(cmd),
                         "*3\r\n$4\r\nRSET\r\n$%d\r\n%s\r\n$5\r\nvalue\r\n", klen, key);
        send_all(fd, cmd, n);
        recv_once(fd, buf, sizeof(buf));
    }
    gettimeofday(&tv_end, NULL);

    int time_used = TIME_SUB_MS(tv_end, tv_begin);
    double qps = (double)count * 1000.0 / (double)time_used;
    printf("rbtree write qps: time_used=%d ms, count=%d, qps=%.2f\n", time_used, count, qps);

    for (int i = 0; i < count; ++i) {
        int klen = snprintf(key, sizeof(key), "rt_w%06d", i);
        int n = snprintf(cmd, sizeof(cmd),
                         "*2\r\n$4\r\nRDEL\r\n$%d\r\n%s\r\n", klen, key);
        send_all(fd, cmd, n);
        recv_once(fd, buf, sizeof(buf));
    }
}

static void bench_hash_set_qps(int fd, int count) {
    char cmd[256];
    char buf[128];
    char key[64];
    struct timeval tv_begin, tv_end;

    gettimeofday(&tv_begin, NULL);
    for (int i = 0; i < count; ++i) {
        int klen = snprintf(key, sizeof(key), "hw_w%06d", i);
        int n = snprintf(cmd, sizeof(cmd),
                         "*3\r\n$4\r\nHSET\r\n$%d\r\n%s\r\n$5\r\nvalue\r\n", klen, key);
        send_all(fd, cmd, n);
        recv_once(fd, buf, sizeof(buf));
    }
    gettimeofday(&tv_end, NULL);

    int time_used = TIME_SUB_MS(tv_end, tv_begin);
    double qps = (double)count * 1000.0 / (double)time_used;
    printf("hash write qps: time_used=%d ms, count=%d, qps=%.2f\n", time_used, count, qps);

    for (int i = 0; i < count; ++i) {
        int klen = snprintf(key, sizeof(key), "hw_w%06d", i);
        int n = snprintf(cmd, sizeof(cmd),
                         "*2\r\n$4\r\nHDEL\r\n$%d\r\n%s\r\n", klen, key);
        send_all(fd, cmd, n);
        recv_once(fd, buf, sizeof(buf));
    }
}

static void bench_skiptable_set_qps(int fd, int count) {
    char cmd[256];
    char buf[128];
    char key[64];
    struct timeval tv_begin, tv_end;

    gettimeofday(&tv_begin, NULL);
    for (int i = 0; i < count; ++i) {
        int klen = snprintf(key, sizeof(key), "sw_w%06d", i);
        int n = snprintf(cmd, sizeof(cmd),
                         "*3\r\n$4\r\nSSET\r\n$%d\r\n%s\r\n$5\r\nvalue\r\n", klen, key);
        send_all(fd, cmd, n);
        recv_once(fd, buf, sizeof(buf));
    }
    gettimeofday(&tv_end, NULL);

    int time_used = TIME_SUB_MS(tv_end, tv_begin);
    double qps = (double)count * 1000.0 / (double)time_used;
    printf("skiptable write qps: time_used=%d ms, count=%d, qps=%.2f\n", time_used, count, qps);

    for (int i = 0; i < count; ++i) {
        int klen = snprintf(key, sizeof(key), "sw_w%06d", i);
        int n = snprintf(cmd, sizeof(cmd),
                         "*2\r\n$4\r\nSDEL\r\n$%d\r\n%s\r\n", klen, key);
        send_all(fd, cmd, n);
        recv_once(fd, buf, sizeof(buf));
    }
}

// 简单 RESP 对象计数：数一条 RESP 对象（+/-/:/$/*），不关心内容
static int count_resp_objects(char *buf, int *len) {
    int count = 0;
    int used  = 0;
    int i     = 0;

    while (i < *len) {
        // 跳过前导空白
        while (i < *len && (buf[i] == '\r' || buf[i] == '\n' || buf[i] == ' ' || buf[i] == '\t')) {
            i++;
        }
        if (i >= *len) break;

        char type = buf[i];
        int start = i;

        // 找到一条 RESP 对象的结束位置
        if (type == '+' || type == '-' || type == ':') {
            // 简单字符串 / 错误 / 整数: 一行结束
            const char *p = memchr(buf + i, '\n', *len - i);
            if (!p) break;   // 数据不够
            i = (int)(p - buf) + 1;
        } else if (type == '$') {
            // 批量字符串: $len\r\n数据\r\n
            const char *p = memchr(buf + i, '\n', *len - i);
            if (!p) break;
            int line_end = (int)(p - buf) + 1;
            long long bulk_len = atoll(buf + i + 1);
            if (bulk_len < 0) {
                // $-1\r\n
                i = line_end;
            } else {
                int need = (int)(line_end - start) + (int)bulk_len + 2;
                if (*len - start < need) break; // 数据不够
                i = start + need;
            }
        } else if (type == '*') {
            // 数组：这里只做“按一行”计数（不递归解析元素），够用来做“对象计数”
            const char *p = memchr(buf + i, '\n', *len - i);
            if (!p) break;
            i = (int)(p - buf) + 1;
        } else {
            // 不认识的，防止死锁，跳过一个字节
            i++;
            continue;
        }

        count++;
        used = i;
    }

    if (used > 0 && used < *len) {
        memmove(buf, buf + used, *len - used);
        *len -= used;
    } else if (used == *len) {
        *len = 0;
    }
    return count;
}

// 通用 pipeline 写 QPS 内部工具：构造批量命令并统计 +OK\n 次数
static void bench_generic_pipeline_qps(int fd, int count, int batch,
                                       const char *cmd_name, int cmd_len,
                                       const char *key_prefix,
                                       const char *del_cmd_name, int del_cmd_len,
                                       const char *tag) {
    char  key[64];
    size_t buf_cap = 1024 * 1024;
    char *buf = (char*)malloc(buf_cap);
    if (!buf) {
        perror("malloc");
        return;
    }

    struct timeval tv_begin, tv_end;
    gettimeofday(&tv_begin, NULL);

    int sent_total = 0;
    int buf_len    = 0;

    while (sent_total < count) {
        int this_batch = batch;
        if (sent_total + this_batch > count)
            this_batch = count - sent_total;

        int max_len = this_batch * 128;
        char *pipeline = (char*)malloc(max_len);
        if (!pipeline) {
            perror("malloc pipeline");
            break;
        }

        int written = 0;
        for (int i = 0; i < this_batch; ++i) {
            int idx  = sent_total + i;
            int klen = snprintf(key, sizeof(key), "%s%06d", key_prefix, idx);
            int n = snprintf(pipeline + written, max_len - written,
                             "*3\r\n"
                             "$%d\r\n%s\r\n"
                             "$%d\r\n%s\r\n"
                             "$5\r\nvalue\r\n",
                             cmd_len, cmd_name,
                             klen,    key);
            if (n <= 0 || n >= max_len - written) {
                this_batch = i;
                break;
            }
            written += n;
        }

        if (this_batch <= 0) {
            free(pipeline);
            break;
        }

        send_all(fd, pipeline, written);
        // printf("Sent pipeline batch: size=%d, count=%d\n", written, this_batch);
        free(pipeline);

        int need_resp = this_batch;
        int got_resp  = 0;

        // 持续 recv，直到解析出 need_resp 个 RESP 对象
        while (got_resp < need_resp) {
            int n = recv(fd, buf + buf_len, (int)(buf_cap - buf_len - 1), 0);
            if (n <= 0) {
                break;
            }
            buf_len += n;
            buf[buf_len] = '\0';

            int c = count_resp_objects(buf, &buf_len);
            got_resp += c;

            // 简单防护：如果一直收不到足够响应，避免死循环
            if (buf_len >= (int)buf_cap - 1024) {
                // 缓冲区太满也退出
                break;
            }
        }

        // 可选：如果 got_resp < need_resp，可以打印告警帮助调试
        // if (got_resp < need_resp) {
        //     fprintf(stderr, "%s pipeline: expect %d resp, got %d\n",
        //             tag, need_resp, got_resp);
        // }

        sent_total += this_batch;
    }

    gettimeofday(&tv_end, NULL);

    int time_used = TIME_SUB_MS(tv_end, tv_begin);
    double qps = (double)count * 1000.0 / (double)time_used;
    printf("%s pipeline write qps: time_used=%d ms, count=%d, qps=%.2f\n",
           tag, time_used, count, qps);

    // 删除写入的数据（单条 DEL）
    char cmd[256];
    for (int i = 0; i < count; ++i) {
        int klen = snprintf(key, sizeof(key), "%s%06d", key_prefix, i);
        int n = snprintf(cmd, sizeof(cmd),
                         "*2\r\n"
                         "$%d\r\n%s\r\n"
                         "$%d\r\n%s\r\n",
                         del_cmd_len, del_cmd_name,
                         klen,        key);
        send_all(fd, cmd, n);
        recv_once(fd, buf, (int)buf_cap);
    }

    free(buf);
}
static void bench_array_pipeline_qps(int fd, int count, int batch) {
    bench_generic_pipeline_qps(fd, count, batch,
                               "SET", 3, "pa_w",
                               "DEL", 3,
                               "array");
}

static void bench_rbtree_pipeline_qps(int fd, int count, int batch) {
    bench_generic_pipeline_qps(fd, count, batch,
                               "RSET", 4, "pr_w",
                               "RDEL", 4,
                               "rbtree");
}

static void bench_hash_pipeline_qps(int fd, int count, int batch) {
    bench_generic_pipeline_qps(fd, count, batch,
                               "HSET", 4, "ph_w",
                               "HDEL", 4,
                               "hash");
}

static void bench_skiptable_pipeline_qps(int fd, int count, int batch) {
    bench_generic_pipeline_qps(fd, count, batch,
                               "SSET", 4, "ps_w",
                               "SDEL", 4,
                               "skiptable");
}

// 通用：某种命令的“先写两次/删一次/等待/再写一次/删两次”序列
static void bench_generic_custom_pattern(int fd, int n,
                                         const char *set_cmd, int set_cmd_len,
                                         const char *del_cmd, int del_cmd_len,
                                         const char *key_prefix,
                                         const char *tag) {
    char cmd[256];
    char key[64];
    char buf[128];
    struct timeval tv_begin, tv_end;

    gettimeofday(&tv_begin, NULL);

    // 1) 写 n 次（第一轮）
    for (int i = 0; i < n; ++i) {
        int klen = snprintf(key, sizeof(key), "%s%06d", key_prefix, i);
        int len = snprintf(cmd, sizeof(cmd),
                           "*3\r\n$%d\r\n%s\r\n$%d\r\n%s\r\n$5\r\nvalue\r\n",
                           set_cmd_len, set_cmd,
                           klen,        key);
        send_all(fd, cmd, len);
        recv_once(fd, buf, sizeof(buf));
    }

    // 2) 再写 n 次（第二轮）
    for (int i = 0; i < n; ++i) {
        int klen = snprintf(key, sizeof(key), "%s%06d", key_prefix, i);
        int len = snprintf(cmd, sizeof(cmd),
                           "*3\r\n$%d\r\n%s\r\n$%d\r\n%s\r\n$5\r\nvalue\r\n",
                           set_cmd_len, set_cmd,
                           klen,        key);
        send_all(fd, cmd, len);
        recv_once(fd, buf, sizeof(buf));
    }

    // 3) 删除 n 次（第一次删）
    for (int i = 0; i < n; ++i) {
        int klen = snprintf(key, sizeof(key), "%s%06d", key_prefix, i);
        int len = snprintf(cmd, sizeof(cmd),
                           "*2\r\n$%d\r\n%s\r\n$%d\r\n%s\r\n",
                           del_cmd_len, del_cmd,
                           klen,        key);
        send_all(fd, cmd, len);
        recv_once(fd, buf, sizeof(buf));
    }

    // 4) 等待一小段时间，模拟“隔一段时间”
    usleep(100 * 1000); // 100ms，可按需调整

    // 5) 再写 n 次（第三轮）
    for (int i = 0; i < n; ++i) {
        int klen = snprintf(key, sizeof(key), "%s%06d", key_prefix, i);
        int len = snprintf(cmd, sizeof(cmd),
                           "*3\r\n$%d\r\n%s\r\n$%d\r\n%s\r\n$5\r\nvalue\r\n",
                           set_cmd_len, set_cmd,
                           klen,        key);
        send_all(fd, cmd, len);
        recv_once(fd, buf, sizeof(buf));
    }

    // 6) 最后删 2n 次（第二次和第三次写的 key）
    for (int round = 0; round < 2; ++round) {
        for (int i = 0; i < n; ++i) {
            int klen = snprintf(key, sizeof(key), "%s%06d", key_prefix, i);
            int len = snprintf(cmd, sizeof(cmd),
                               "*2\r\n$%d\r\n%s\r\n$%d\r\n%s\r\n",
                               del_cmd_len, del_cmd,
                               klen,        key);
            send_all(fd, cmd, len);
            recv_once(fd, buf, sizeof(buf));
        }
    }

    gettimeofday(&tv_end, NULL);
    int time_used = TIME_SUB_MS(tv_end, tv_begin);
    if (time_used <= 0) time_used = 1;

    // 操作数：写 3n 次 + 删 3n 次 = 6n
    long long total_ops = (long long)n * 6;
    double qps = (double)total_ops * 1000.0 / (double)time_used;

    printf("%s custom mempool pattern: n=%d, total_ops=%lld, time_used=%d ms, qps=%.2f ops/s\n",
           tag, n, total_ops, time_used, qps);
}

static void bench_all_ds_custom_pattern(int fd, int n) {
    bench_generic_custom_pattern(fd, n, "SET", 3, "DEL", 3, "mw_a", "array");
    bench_generic_custom_pattern(fd, n, "RSET", 4, "RDEL", 4, "mw_r", "rbtree");
    bench_generic_custom_pattern(fd, n, "HSET", 4, "HDEL", 4, "mw_h", "hash");
    bench_generic_custom_pattern(fd, n, "SSET", 4, "SDEL", 4, "mw_s", "skiptable");
}

int main(int argc, char *argv[]) {
    if (argc < 4) {
        fprintf(stderr, "usage: %s ip port mode [count]\n", argv[0]);
        fprintf(stderr, "mode: 1=array, 2=rbtree, 3=hash, 4=skiptable, 5=all, 6=pipeline-all-ds, 7=custom-mempool-all-ds, 8=cold-keys-all-ds\n");
        return 1;
    }

    const char *ip = argv[1];
    int port = atoi(argv[2]);
    int mode = atoi(argv[3]);
    int count = 100000;
    if (argc >= 5) {
        count = atoi(argv[4]);
        if (count <= 0) count = 100000;
    }

    int fd = connect_tcpserver(ip, (unsigned short)port);
    if (fd < 0) return 1;

    if (mode == 1) {
        bench_array_set_qps(fd, count);
    } else if (mode == 2) {
        bench_rbtree_set_qps(fd, count);
    } else if (mode == 3) {
        bench_hash_set_qps(fd, count);
    } else if (mode == 4) {
        bench_skiptable_set_qps(fd, count);
    } else if (mode == 5) {
        bench_array_set_qps(fd, count);
        bench_rbtree_set_qps(fd, count);
        bench_hash_set_qps(fd, count);
        bench_skiptable_set_qps(fd, count);
    } else if (mode == 6) {
        int batch = 1000;
        bench_array_pipeline_qps(fd, count, batch);
        bench_rbtree_pipeline_qps(fd, count, batch);
        bench_hash_pipeline_qps(fd, count, batch);
        bench_skiptable_pipeline_qps(fd, count, batch);
    } else if (mode == 7) {
        // 自定义 mempool 压测（四种数据结构统一）：
        // 先写 count 两次 -> 删 count 一次 -> 隔一段时间 -> 再写 count 一次 -> 最后删 2*count
        bench_all_ds_custom_pattern(fd, count);
    } else if (mode == 8) {
        // 冷 key 场景：每种数据结构用一批几乎不重复的 key，只写一次再删一次
        bench_array_set_qps(fd, count);
        bench_rbtree_set_qps(fd, count);
        bench_hash_set_qps(fd, count);
        bench_skiptable_set_qps(fd, count);
    } else {
        fprintf(stderr, "unknown mode %d\n", mode);
    }

    close(fd);
    return 0;
}
