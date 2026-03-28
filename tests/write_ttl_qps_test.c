#include <arpa/inet.h>
#include <errno.h>
#include <netinet/tcp.h>
#include <stdio.h>
#include <stdlib.h>
#include <stdarg.h>
#include <stdint.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/time.h>
#include <unistd.h>

#define ARRAY_SIZE(a) (sizeof(a) / sizeof((a)[0]))
#define DEFAULT_COUNT 100000
#define DEFAULT_SAMPLE_COUNT 256
#define DEFAULT_TTL_SECS 8
#define DEFAULT_WAIT_BUFFER_SECS 4
#define PIPELINE_BATCH_KEYS 512
#define RESP_IO_BUF 8192
#define RESP_TEXT_BUF 1024

#define TIME_SUB_MS(tv1, tv2)  ((tv1.tv_sec - tv2.tv_sec) * 1000 + (tv1.tv_usec - tv2.tv_usec) / 1000)

typedef enum {
    RESP_KIND_UNKNOWN = 0,
    RESP_KIND_SIMPLE,
    RESP_KIND_ERROR,
    RESP_KIND_INTEGER,
    RESP_KIND_BULK,
    RESP_KIND_NIL,
} resp_kind_t;

typedef struct {
    int fd;
    char buf[RESP_IO_BUF];
    size_t pos;
    size_t len;
} resp_reader_t;

typedef struct {
    int mode;
    const char *name;
    const char *write_cmd;
    const char *read_cmd;
    const char *overwrite_cmd;
    const char *key_prefix;
} backend_cfg_t;

typedef struct {
    long long total_checks;
    long long passed_checks;
    long long failed_checks;
    long long sample_total;
    long long sample_passed;
    long long sample_failed;
} stats_t;

static const backend_cfg_t g_backends[] = {
    {1, "array",     "SET",  "GET",  "MOD",  "ttlq_array_"},
    {2, "rbtree",    "RSET", "RGET", "RMOD", "ttlq_rbtree_"},
    {3, "hash",      "HSET", "HGET", "HMOD", "ttlq_hash_"},
    {4, "skiptable", "SSET", "SGET", "SMOD", "ttlq_skiptable_"},
};

static int send_all(int fd, const char *buf, int len) {
    int sent = 0;
    while (sent < len) {
        int n = (int)send(fd, buf + sent, (size_t)(len - sent), 0);
        if (n <= 0) return n;
        sent += n;
    }
    return sent;
}

static int connect_tcpserver(const char *ip, unsigned short port) {
    int fd = socket(AF_INET, SOCK_STREAM, 0);
    if (fd < 0) {
        perror("socket");
        return -1;
    }

    int one = 1;
    setsockopt(fd, IPPROTO_TCP, TCP_NODELAY, &one, sizeof(one));

    struct timeval tv;
    tv.tv_sec = 5;
    tv.tv_usec = 0;
    setsockopt(fd, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv));
    setsockopt(fd, SOL_SOCKET, SO_SNDTIMEO, &tv, sizeof(tv));

    struct sockaddr_in addr;
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

static void reader_init(resp_reader_t *reader, int fd) {
    reader->fd = fd;
    reader->pos = 0;
    reader->len = 0;
}

static int reader_fill(resp_reader_t *reader) {
    ssize_t n = recv(reader->fd, reader->buf, sizeof(reader->buf), 0);
    if (n <= 0) return (int)n;
    reader->pos = 0;
    reader->len = (size_t)n;
    return (int)n;
}

static int reader_getc(resp_reader_t *reader, char *ch) {
    if (reader->pos >= reader->len) {
        int n = reader_fill(reader);
        if (n <= 0) return n;
    }
    *ch = reader->buf[reader->pos++];
    return 1;
}

static int reader_read_exact(resp_reader_t *reader, char *out, size_t need) {
    size_t copied = 0;
    while (copied < need) {
        if (reader->pos >= reader->len) {
            int n = reader_fill(reader);
            if (n <= 0) return -1;
        }

        size_t avail = reader->len - reader->pos;
        size_t chunk = need - copied;
        if (chunk > avail) chunk = avail;
        memcpy(out + copied, reader->buf + reader->pos, chunk);
        reader->pos += chunk;
        copied += chunk;
    }
    return 0;
}

static int reader_discard_crlf(resp_reader_t *reader) {
    char tail[2];
    if (reader_read_exact(reader, tail, sizeof(tail)) != 0) return -1;
    if (tail[0] != '\r' || tail[1] != '\n') return -1;
    return 0;
}

static int reader_read_line(resp_reader_t *reader, char *out, size_t out_cap) {
    size_t n = 0;
    char ch;
    while (1) {
        int ret = reader_getc(reader, &ch);
        if (ret <= 0) return -1;
        if (ch == '\r') {
            if (reader_getc(reader, &ch) <= 0) return -1;
            if (ch != '\n') return -1;
            out[n] = '\0';
            return 0;
        }
        if (n + 1 >= out_cap) return -1;
        out[n++] = ch;
    }
}

static int reader_read_resp(resp_reader_t *reader, char *text, size_t text_cap, long long *integer, resp_kind_t *kind) {
    char prefix;
    char line[128];

    if (reader_getc(reader, &prefix) <= 0) return -1;

    switch (prefix) {
        case '+':
            if (reader_read_line(reader, line, sizeof(line)) != 0) return -1;
            snprintf(text, text_cap, "%s", line);
            if (kind) *kind = RESP_KIND_SIMPLE;
            return 0;

        case '-':
            if (reader_read_line(reader, line, sizeof(line)) != 0) return -1;
            snprintf(text, text_cap, "ERROR %s", line);
            if (kind) *kind = RESP_KIND_ERROR;
            return 0;

        case ':':
            if (reader_read_line(reader, line, sizeof(line)) != 0) return -1;
            if (integer) *integer = atoll(line);
            snprintf(text, text_cap, "%s", line);
            if (kind) *kind = RESP_KIND_INTEGER;
            return 0;

        case '$': {
            if (reader_read_line(reader, line, sizeof(line)) != 0) return -1;
            long long bulk_len = atoll(line);
            if (bulk_len < 0) {
                snprintf(text, text_cap, "NO EXIST");
                if (kind) *kind = RESP_KIND_NIL;
                return 0;
            }
            if (bulk_len >= (long long)text_cap) return -1;
            if (reader_read_exact(reader, text, (size_t)bulk_len) != 0) return -1;
            text[bulk_len] = '\0';
            if (reader_discard_crlf(reader) != 0) return -1;
            if (kind) *kind = RESP_KIND_BULK;
            return 0;
        }

        case '*':
            if (reader_read_line(reader, line, sizeof(line)) != 0) return -1;
            snprintf(text, text_cap, "ARRAY %s", line);
            if (kind) *kind = RESP_KIND_UNKNOWN;
            return 0;

        default:
            snprintf(text, text_cap, "UNKNOWN");
            if (kind) *kind = RESP_KIND_UNKNOWN;
            return -1;
    }
}

static int append_bytes(char **buf, size_t *cap, size_t *len, const char *data, size_t data_len) {
    if (*len + data_len >= *cap) {
        size_t new_cap = *cap;
        while (*len + data_len >= new_cap) {
            new_cap *= 2;
        }
        char *new_buf = (char *)realloc(*buf, new_cap);
        if (!new_buf) return -1;
        *buf = new_buf;
        *cap = new_cap;
    }
    memcpy(*buf + *len, data, data_len);
    *len += data_len;
    return 0;
}

static int append_resp_array(char **buf, size_t *cap, size_t *len, const char **parts, int argc) {
    char header[32];
    int header_len = snprintf(header, sizeof(header), "*%d\r\n", argc);
    if (header_len <= 0) return -1;
    if (append_bytes(buf, cap, len, header, (size_t)header_len) != 0) return -1;

    for (int i = 0; i < argc; ++i) {
        char prefix[32];
        int prefix_len = snprintf(prefix, sizeof(prefix), "$%zu\r\n", strlen(parts[i]));
        if (prefix_len <= 0) return -1;
        if (append_bytes(buf, cap, len, prefix, (size_t)prefix_len) != 0) return -1;
        if (append_bytes(buf, cap, len, parts[i], strlen(parts[i])) != 0) return -1;
        if (append_bytes(buf, cap, len, "\r\n", 2) != 0) return -1;
    }

    return 0;
}

static int build_single_command(char *out, size_t out_cap, const char **parts, int argc) {
    size_t len = 0;
    int header_len = snprintf(out + len, out_cap - len, "*%d\r\n", argc);
    if (header_len <= 0 || (size_t)header_len >= out_cap - len) return -1;
    len += (size_t)header_len;

    for (int i = 0; i < argc; ++i) {
        int prefix_len = snprintf(out + len, out_cap - len, "$%zu\r\n", strlen(parts[i]));
        if (prefix_len <= 0 || (size_t)prefix_len >= out_cap - len) return -1;
        len += (size_t)prefix_len;

        size_t arg_len = strlen(parts[i]);
        if (arg_len + 2 >= out_cap - len) return -1;
        memcpy(out + len, parts[i], arg_len);
        len += arg_len;
        out[len++] = '\r';
        out[len++] = '\n';
    }

    return (int)len;
}

static void build_key(char *out, size_t out_cap, const char *prefix, int idx) {
    snprintf(out, out_cap, "%s%06d", prefix, idx);
}

static void build_value(char *out, size_t out_cap, const char *prefix, int idx) {
    snprintf(out, out_cap, "%s%06d", prefix, idx);
}

static void record_check(stats_t *stats, int is_sample, const char *backend, const char *case_name, int ok,
                         const char *expected, const char *actual, const char *cmd) {
    stats->total_checks++;
    if (ok) {
        stats->passed_checks++;
        if (is_sample) stats->sample_passed++;
        return;
    }

    stats->failed_checks++;
    if (is_sample) stats->sample_failed++;

    if (cmd && expected && actual) {
        printf("[FAIL][%s][%s] cmd=%s expected=%s actual=%s\n", backend, case_name, cmd, expected, actual);
    } else if (cmd && actual) {
        printf("[FAIL][%s][%s] cmd=%s actual=%s\n", backend, case_name, cmd, actual);
    } else {
        printf("[FAIL][%s][%s]\n", backend, case_name);
    }
}

static int send_command_request(resp_reader_t *reader, const char **parts, int argc, char *resp_text, size_t resp_cap,
                                long long *resp_int, resp_kind_t *resp_kind) {
    char request[512];
    int req_len = build_single_command(request, sizeof(request), parts, argc);
    if (req_len <= 0) return -1;
    if (send_all(reader->fd, request, req_len) != req_len) return -1;
    return reader_read_resp(reader, resp_text, resp_cap, resp_int, resp_kind);
}

static int send_pipeline_batch(resp_reader_t *reader, const char *backend, stats_t *stats,
                               const char *cmd_name, const char *key_prefix,
                               int start_idx, int batch_keys, int ttl_secs,
                               const char *write_value_prefix, const char *overwrite_value_prefix,
                               int phase_kind) {
    char *pipeline = NULL;
    size_t cap = (size_t)batch_keys * 192 + 128;
    size_t len = 0;
    int responses_per_key = 0;

    pipeline = (char *)malloc(cap);
    if (!pipeline) return -1;

    for (int i = 0; i < batch_keys; ++i) {
        int idx = start_idx + i;
        char key[64];
        char value[64];
        char ttl_buf[32];
        const char *parts[3];
        int argc = 0;

        build_key(key, sizeof(key), key_prefix, idx);

        switch (phase_kind) {
            case 0:
                build_value(value, sizeof(value), write_value_prefix, idx);
                parts[0] = cmd_name;
                parts[1] = key;
                parts[2] = value;
                argc = 3;
                if (append_resp_array(&pipeline, &cap, &len, parts, argc) != 0) goto fail;

                snprintf(ttl_buf, sizeof(ttl_buf), "%d", ttl_secs);
                parts[0] = "EXPIRE";
                parts[1] = key;
                parts[2] = ttl_buf;
                argc = 3;
                if (append_resp_array(&pipeline, &cap, &len, parts, argc) != 0) goto fail;
                responses_per_key = 2;
                break;

            case 1:
                build_value(value, sizeof(value), overwrite_value_prefix, idx);
                parts[0] = cmd_name;
                parts[1] = key;
                parts[2] = value;
                argc = 3;
                if (append_resp_array(&pipeline, &cap, &len, parts, argc) != 0) goto fail;
                responses_per_key = 1;
                break;

            case 2:
                parts[0] = "PERSIST";
                parts[1] = key;
                argc = 2;
                if (append_resp_array(&pipeline, &cap, &len, parts, argc) != 0) goto fail;
                responses_per_key = 1;
                break;

            default:
                goto fail;
        }
    }

    if (send_all(reader->fd, pipeline, (int)len) != (int)len) goto fail;

    for (int i = 0; i < batch_keys * responses_per_key; ++i) {
        char text[RESP_TEXT_BUF];
        long long integer_value = 0;
        resp_kind_t kind = RESP_KIND_UNKNOWN;
        if (reader_read_resp(reader, text, sizeof(text), &integer_value, &kind) != 0) goto fail;
        if (kind != RESP_KIND_SIMPLE || strcmp(text, "OK") != 0) {
            char cmd_buf[128];
            snprintf(cmd_buf, sizeof(cmd_buf), "%s(batch-response)", backend);
            record_check(stats, 0, backend, cmd_buf, 0, "OK", text, cmd_name);
        }
    }

    free(pipeline);
    return 0;

fail:
    free(pipeline);
    return -1;
}

static int run_set_expire_load(resp_reader_t *reader, const backend_cfg_t *cfg, stats_t *stats,
                               int count, int ttl_secs, long long *elapsed_ms) {
    struct timeval tv_begin, tv_end;
    gettimeofday(&tv_begin, NULL);

    int sent = 0;
    while (sent < count) {
        int batch = count - sent;
        if (batch > PIPELINE_BATCH_KEYS) batch = PIPELINE_BATCH_KEYS;
        if (send_pipeline_batch(reader, cfg->name, stats, cfg->write_cmd, cfg->key_prefix,
                                sent, batch, ttl_secs, "value-", NULL, 0) != 0) {
            return -1;
        }
        sent += batch;
    }

    gettimeofday(&tv_end, NULL);
    *elapsed_ms = TIME_SUB_MS(tv_end, tv_begin);
    if (*elapsed_ms <= 0) *elapsed_ms = 1;
    return 0;
}

static int run_overwrite_phase(resp_reader_t *reader, const backend_cfg_t *cfg, stats_t *stats,
                               int count, int overwrite_count) {
    int sent = 0;
    while (sent < overwrite_count) {
        int batch = overwrite_count - sent;
        if (batch > PIPELINE_BATCH_KEYS) batch = PIPELINE_BATCH_KEYS;
        if (send_pipeline_batch(reader, cfg->name, stats, cfg->overwrite_cmd, cfg->key_prefix,
                                sent, batch, 0, NULL, "overwrite-", 1) != 0) {
            return -1;
        }
        sent += batch;
    }
    (void)count;
    return 0;
}

static int run_persist_phase(resp_reader_t *reader, const backend_cfg_t *cfg, stats_t *stats,
                             int start_idx, int persist_count) {
    int sent = 0;
    while (sent < persist_count) {
        int batch = persist_count - sent;
        if (batch > PIPELINE_BATCH_KEYS) batch = PIPELINE_BATCH_KEYS;
        if (send_pipeline_batch(reader, cfg->name, stats, "PERSIST", cfg->key_prefix,
                                start_idx + sent, batch, 0, NULL, NULL, 2) != 0) {
            return -1;
        }
        sent += batch;
    }
    return 0;
}

static int sample_index_for_slot(int group_start, int group_count, int slot, int sample_count) {
    if (group_count <= 0 || sample_count <= 0) return -1;
    if (sample_count > group_count) sample_count = group_count;
    return group_start + (int)(((long long)slot * group_count) / sample_count);
}

static void print_backend_summary(const backend_cfg_t *cfg, const stats_t *stats, long long total_ms,
                                  long long write_ms, int count, int ttl_secs, int overwrite_count,
                                  int persist_count, int sample_count) {
    double pair_qps = (double)count * 1000.0 / (double)write_ms;
    double op_qps = (double)count * 2.0 * 1000.0 / (double)write_ms;
    double sample_rate = 0.0;
    if (stats->sample_total > 0) {
        sample_rate = (double)stats->sample_passed * 100.0 / (double)stats->sample_total;
    }

    printf("[RESULT][%s] total_ms=%lld write_ms=%lld ttl_secs=%d count=%d overwrite_count=%d persist_count=%d sample_count=%d\n",
           cfg->name, total_ms, write_ms, ttl_secs, count, overwrite_count, persist_count, sample_count);
    printf("[RESULT][%s] qps_pairs=%.2f qps_ops=%.2f sample_success=%lld/%lld sample_rate=%.2f%% failures=%lld\n",
           cfg->name, pair_qps, op_qps, stats->sample_passed, stats->sample_total, sample_rate, stats->failed_checks);
}

static int run_backend_suite(const backend_cfg_t *cfg, const char *ip, unsigned short port,
                             int count, int sample_count, int ttl_secs, int wait_buffer_secs) {
    int fd = connect_tcpserver(ip, port);
    if (fd < 0) return -1;

    resp_reader_t reader;
    reader_init(&reader, fd);

    int overwrite_count = count / 10;
    int persist_count = count / 10;
    if (overwrite_count < 1) overwrite_count = 1;
    if (persist_count < 1) persist_count = 1;
    if (overwrite_count + persist_count > count) {
        persist_count = count - overwrite_count;
        if (persist_count < 0) persist_count = 0;
    }

    int plain_count = count - overwrite_count - persist_count;
    if (plain_count < 0) plain_count = 0;

    stats_t stats;
    memset(&stats, 0, sizeof(stats));

    struct timeval tv_begin, tv_end;
    gettimeofday(&tv_begin, NULL);

    long long write_ms = 1;
    if (run_set_expire_load(&reader, cfg, &stats, count, ttl_secs, &write_ms) != 0) {
        close(fd);
        return -1;
    }

    printf("[INFO][%s] count=%d ttl_secs=%d sample_count=%d overwrite_count=%d persist_count=%d plain_count=%d\n",
           cfg->name, count, ttl_secs, sample_count, overwrite_count, persist_count, plain_count);

    int pre_sample_count = sample_count;
    if (pre_sample_count > plain_count) pre_sample_count = plain_count;
    for (int i = 0; i < pre_sample_count; ++i) {
        int idx = sample_index_for_slot(overwrite_count + persist_count, plain_count, i, pre_sample_count);
        if (idx < 0) continue;
        char key[64];
        char expected_value[64];
        char text[RESP_TEXT_BUF];
        long long integer_value = 0;
        resp_kind_t kind = RESP_KIND_UNKNOWN;
        const char *cmd_parts[2];

        build_key(key, sizeof(key), cfg->key_prefix, idx);
        build_value(expected_value, sizeof(expected_value), "value-", idx);
        cmd_parts[0] = cfg->read_cmd;
        cmd_parts[1] = key;

        if (send_command_request(&reader, cmd_parts, 2, text, sizeof(text), &integer_value, &kind) != 0) {
            record_check(&stats, 1, cfg->name, "pre-expire-read", 0, expected_value, "IO_ERROR", cfg->read_cmd);
            continue;
        }

        stats.sample_total++;
        stats.total_checks++;
        if (strcmp(text, expected_value) == 0) {
            stats.passed_checks++;
            stats.sample_passed++;
        } else {
            stats.failed_checks++;
            stats.sample_failed++;
            printf("[FAIL][%s][pre-expire-read] key=%s expected=%s actual=%s\n",
                   cfg->name, key, expected_value, text);
        }
    }

    if (run_overwrite_phase(&reader, cfg, &stats, count, overwrite_count) != 0) {
        close(fd);
        return -1;
    }

    for (int i = 0; i < overwrite_count && i < sample_count; ++i) {
        int idx = sample_index_for_slot(0, overwrite_count, i, overwrite_count < sample_count ? overwrite_count : sample_count);
        if (idx < 0) continue;
        char key[64];
        char expected_value[64];
        char text[RESP_TEXT_BUF];
        long long integer_value = 0;
        resp_kind_t kind = RESP_KIND_UNKNOWN;
        const char *read_cmd_parts[2];
        const char *ttl_cmd_parts[2];

        build_key(key, sizeof(key), cfg->key_prefix, idx);
        build_value(expected_value, sizeof(expected_value), "overwrite-", idx);
        read_cmd_parts[0] = cfg->read_cmd;
        read_cmd_parts[1] = key;

        if (send_command_request(&reader, read_cmd_parts, 2, text, sizeof(text), &integer_value, &kind) != 0) {
            record_check(&stats, 1, cfg->name, "overwrite-read", 0, expected_value, "IO_ERROR", cfg->read_cmd);
            continue;
        }

        stats.sample_total++;
        stats.total_checks++;
        if (strcmp(text, expected_value) == 0) {
            stats.passed_checks++;
            stats.sample_passed++;
        } else {
            stats.failed_checks++;
            stats.sample_failed++;
            printf("[FAIL][%s][overwrite-read] key=%s expected=%s actual=%s\n",
                   cfg->name, key, expected_value, text);
        }

        ttl_cmd_parts[0] = "TTL";
        ttl_cmd_parts[1] = key;
        if (send_command_request(&reader, ttl_cmd_parts, 2, text, sizeof(text), &integer_value, &kind) != 0) {
            record_check(&stats, 1, cfg->name, "overwrite-ttl", 0, "-1", "IO_ERROR", "TTL");
            continue;
        }

        stats.sample_total++;
        stats.total_checks++;
        long long ttl_value = (kind == RESP_KIND_INTEGER) ? integer_value : atoll(text);
        if (ttl_value == -1) {
            stats.passed_checks++;
            stats.sample_passed++;
        } else {
            stats.failed_checks++;
            stats.sample_failed++;
            printf("[FAIL][%s][overwrite-ttl] key=%s expected=-1 actual=%lld\n",
                   cfg->name, key, ttl_value);
        }
    }

    if (run_persist_phase(&reader, cfg, &stats, overwrite_count, persist_count) != 0) {
        close(fd);
        return -1;
    }

    for (int i = 0; i < persist_count && i < sample_count; ++i) {
        int idx = sample_index_for_slot(overwrite_count, persist_count, i, persist_count < sample_count ? persist_count : sample_count);
        if (idx < 0) continue;
        char key[64];
        char expected_value[64];
        char text[RESP_TEXT_BUF];
        long long integer_value = 0;
        resp_kind_t kind = RESP_KIND_UNKNOWN;
        const char *read_cmd_parts[2];
        const char *ttl_cmd_parts[2];

        build_key(key, sizeof(key), cfg->key_prefix, idx);
        build_value(expected_value, sizeof(expected_value), "value-", idx);
        read_cmd_parts[0] = cfg->read_cmd;
        read_cmd_parts[1] = key;

        if (send_command_request(&reader, read_cmd_parts, 2, text, sizeof(text), &integer_value, &kind) != 0) {
            record_check(&stats, 1, cfg->name, "persist-read", 0, expected_value, "IO_ERROR", cfg->read_cmd);
            continue;
        }

        stats.sample_total++;
        stats.total_checks++;
        if (strcmp(text, expected_value) == 0) {
            stats.passed_checks++;
            stats.sample_passed++;
        } else {
            stats.failed_checks++;
            stats.sample_failed++;
            printf("[FAIL][%s][persist-read] key=%s expected=%s actual=%s\n",
                   cfg->name, key, expected_value, text);
        }

        ttl_cmd_parts[0] = "TTL";
        ttl_cmd_parts[1] = key;
        if (send_command_request(&reader, ttl_cmd_parts, 2, text, sizeof(text), &integer_value, &kind) != 0) {
            record_check(&stats, 1, cfg->name, "persist-ttl", 0, "-1", "IO_ERROR", "TTL");
            continue;
        }

        stats.sample_total++;
        stats.total_checks++;
        long long ttl_value = (kind == RESP_KIND_INTEGER) ? integer_value : atoll(text);
        if (ttl_value == -1) {
            stats.passed_checks++;
            stats.sample_passed++;
        } else {
            stats.failed_checks++;
            stats.sample_failed++;
            printf("[FAIL][%s][persist-ttl] key=%s expected=-1 actual=%lld\n",
                   cfg->name, key, ttl_value);
        }
    }

    int wait_secs = ttl_secs + wait_buffer_secs;
    if (wait_secs < ttl_secs + 1) wait_secs = ttl_secs + 1;
    sleep((unsigned int)wait_secs);

    int post_plain_count = sample_count;
    if (post_plain_count > plain_count) post_plain_count = plain_count;
    for (int i = 0; i < post_plain_count; ++i) {
        int idx = sample_index_for_slot(overwrite_count + persist_count, plain_count, i, post_plain_count);
        if (idx < 0) continue;
        char key[64];
        char expected_value[64];
        char text[RESP_TEXT_BUF];
        long long integer_value = 0;
        resp_kind_t kind = RESP_KIND_UNKNOWN;
        const char *read_cmd_parts[2];

        build_key(key, sizeof(key), cfg->key_prefix, idx);
        build_value(expected_value, sizeof(expected_value), "value-", idx);
        read_cmd_parts[0] = cfg->read_cmd;
        read_cmd_parts[1] = key;

        if (send_command_request(&reader, read_cmd_parts, 2, text, sizeof(text), &integer_value, &kind) != 0) {
            record_check(&stats, 1, cfg->name, "expired-read", 0, "NO EXIST", "IO_ERROR", cfg->read_cmd);
            continue;
        }

        stats.sample_total++;
        stats.total_checks++;
        if (strcmp(text, "NO EXIST") == 0) {
            stats.passed_checks++;
            stats.sample_passed++;
        } else {
            stats.failed_checks++;
            stats.sample_failed++;
            printf("[FAIL][%s][expired-read] key=%s expected=NO EXIST actual=%s\n",
                   cfg->name, key, text);
        }
    }

    for (int i = 0; i < overwrite_count && i < sample_count; ++i) {
        int idx = sample_index_for_slot(0, overwrite_count, i, overwrite_count < sample_count ? overwrite_count : sample_count);
        if (idx < 0) continue;
        char key[64];
        char expected_value[64];
        char text[RESP_TEXT_BUF];
        long long integer_value = 0;
        resp_kind_t kind = RESP_KIND_UNKNOWN;
        const char *read_cmd_parts[2];

        build_key(key, sizeof(key), cfg->key_prefix, idx);
        build_value(expected_value, sizeof(expected_value), "overwrite-", idx);
        read_cmd_parts[0] = cfg->read_cmd;
        read_cmd_parts[1] = key;

        if (send_command_request(&reader, read_cmd_parts, 2, text, sizeof(text), &integer_value, &kind) != 0) {
            record_check(&stats, 1, cfg->name, "overwrite-survive", 0, expected_value, "IO_ERROR", cfg->read_cmd);
            continue;
        }

        stats.sample_total++;
        stats.total_checks++;
        if (strcmp(text, expected_value) == 0) {
            stats.passed_checks++;
            stats.sample_passed++;
        } else {
            stats.failed_checks++;
            stats.sample_failed++;
            printf("[FAIL][%s][overwrite-survive] key=%s expected=%s actual=%s\n",
                   cfg->name, key, expected_value, text);
        }
    }

    for (int i = 0; i < persist_count && i < sample_count; ++i) {
        int idx = sample_index_for_slot(overwrite_count, persist_count, i, persist_count < sample_count ? persist_count : sample_count);
        if (idx < 0) continue;
        char key[64];
        char expected_value[64];
        char text[RESP_TEXT_BUF];
        long long integer_value = 0;
        resp_kind_t kind = RESP_KIND_UNKNOWN;
        const char *read_cmd_parts[2];

        build_key(key, sizeof(key), cfg->key_prefix, idx);
        build_value(expected_value, sizeof(expected_value), "value-", idx);
        read_cmd_parts[0] = cfg->read_cmd;
        read_cmd_parts[1] = key;

        if (send_command_request(&reader, read_cmd_parts, 2, text, sizeof(text), &integer_value, &kind) != 0) {
            record_check(&stats, 1, cfg->name, "persist-survive", 0, expected_value, "IO_ERROR", cfg->read_cmd);
            continue;
        }

        stats.sample_total++;
        stats.total_checks++;
        if (strcmp(text, expected_value) == 0) {
            stats.passed_checks++;
            stats.sample_passed++;
        } else {
            stats.failed_checks++;
            stats.sample_failed++;
            printf("[FAIL][%s][persist-survive] key=%s expected=%s actual=%s\n",
                   cfg->name, key, expected_value, text);
        }
    }

    gettimeofday(&tv_end, NULL);
    long long total_ms = TIME_SUB_MS(tv_end, tv_begin);
    if (total_ms <= 0) total_ms = 1;

    print_backend_summary(cfg, &stats, total_ms, write_ms, count, ttl_secs, overwrite_count, persist_count, sample_count);

    close(fd);

    if (stats.failed_checks > 0) {
        printf("[FAIL][%s] failed_checks=%lld sample_failed=%lld sample_total=%lld\n",
               cfg->name, stats.failed_checks, stats.sample_failed, stats.sample_total);
        return -1;
    }

    printf("[PASS][%s] ttl qps + correctness suite completed\n", cfg->name);
    return 0;
}

static const backend_cfg_t *find_backend_by_mode(int mode) {
    for (size_t i = 0; i < ARRAY_SIZE(g_backends); ++i) {
        if (g_backends[i].mode == mode) return &g_backends[i];
    }
    return NULL;
}

static int run_mode_all(const char *ip, unsigned short port, int count, int sample_count, int ttl_secs, int wait_buffer_secs) {
    int rc = 0;
    for (size_t i = 0; i < ARRAY_SIZE(g_backends); ++i) {
        printf("[INFO][%s] starting backend run\n", g_backends[i].name);
        if (run_backend_suite(&g_backends[i], ip, port, count, sample_count, ttl_secs, wait_buffer_secs) != 0) {
            rc = -1;
        }
    }
    return rc;
}

static void print_usage(const char *prog) {
    fprintf(stderr,
            "usage: %s ip port mode [count] [sample_count] [ttl_secs] [wait_buffer_secs]\n"
            "mode: 1=array, 2=rbtree, 3=hash, 4=skiptable, 5=all\n"
            "defaults: count=%d sample_count=%d ttl_secs=%d wait_buffer_secs=%d\n",
            prog, DEFAULT_COUNT, DEFAULT_SAMPLE_COUNT, DEFAULT_TTL_SECS, DEFAULT_WAIT_BUFFER_SECS);
}

int main(int argc, char *argv[]) {
    if (argc < 4) {
        print_usage(argv[0]);
        return 1;
    }

    const char *ip = argv[1];
    int port = atoi(argv[2]);
    int mode = atoi(argv[3]);
    int count = (argc >= 5) ? atoi(argv[4]) : DEFAULT_COUNT;
    int sample_count = (argc >= 6) ? atoi(argv[5]) : DEFAULT_SAMPLE_COUNT;
    int ttl_secs = (argc >= 7) ? atoi(argv[6]) : DEFAULT_TTL_SECS;
    int wait_buffer_secs = (argc >= 8) ? atoi(argv[7]) : DEFAULT_WAIT_BUFFER_SECS;

    if (count <= 0) count = DEFAULT_COUNT;
    if (sample_count <= 0) sample_count = DEFAULT_SAMPLE_COUNT;
    if (ttl_secs <= 0) ttl_secs = DEFAULT_TTL_SECS;
    if (wait_buffer_secs < 0) wait_buffer_secs = DEFAULT_WAIT_BUFFER_SECS;

    if (mode == 5) {
        return run_mode_all(ip, (unsigned short)port, count, sample_count, ttl_secs, wait_buffer_secs) == 0 ? 0 : 1;
    }

    const backend_cfg_t *cfg = find_backend_by_mode(mode);
    if (!cfg) {
        fprintf(stderr, "unknown mode %d\n", mode);
        print_usage(argv[0]);
        return 1;
    }

    return run_backend_suite(cfg, ip, (unsigned short)port, count, sample_count, ttl_secs, wait_buffer_secs) == 0 ? 0 : 1;
}