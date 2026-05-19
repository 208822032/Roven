/*
 * vector_benchmark.c — Unified KVLite V-family benchmark tool.
 *
 * Two modes:
 *   latency    — single-connection serial requests, measure per-request latency
 *   throughput — multi-connection + pipeline, measure server saturation QPS
 *
 * Supports: VSET, VGET, VSEARCH, VDEL
 */

#define _GNU_SOURCE
#include <arpa/inet.h>
#include <errno.h>
#include <fcntl.h>
#include <getopt.h>
#include <math.h>
#include <netdb.h>
#include <netinet/tcp.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/epoll.h>
#include <sys/socket.h>
#include <sys/time.h>
#include <unistd.h>

/* ── constants ──────────────────────────────────────────────────── */

#define MAX_KEY_LEN   512
#define MAX_VAL_LEN   512
#define RECV_BUF_SIZE (256 * 1024)
#define SEND_BUF_SIZE (8 * 1024)
#define MAX_EVENTS    1024
#define DEFAULT_TOPK  10

/* ── types ──────────────────────────────────────────────────────── */

typedef enum { OP_VSET, OP_VGET, OP_VSEARCH, OP_VDEL } op_t;

typedef enum { BENCH_MODE_LATENCY, BENCH_MODE_THROUGHPUT } bench_mode_t;

typedef struct {
    bench_mode_t mode;
    const char *host;
    int port;
    op_t op;
    int n_requests;
    int n_clients;
    int pipeline;
    const char *scope;
    const char *prefix;
    int topk;
    int prepare_count;
} config_t;

typedef struct {
    int fd;
    int pending;
    int max_pending;
    long long *pending_times;
    int head;
    int tail;
    char recv_buf[RECV_BUF_SIZE];
    int recv_len;
    int sent;
    int received;
    int dead;
} client_t;

/* ── forward decls ──────────────────────────────────────────────── */

static long long now_us(void);
static int connect_tcp(const char *host, int port);
static int set_nonblocking(int fd);
static int send_all(int fd, const char *buf, size_t len);
static int build_resp_command(char *out, size_t cap, const char **parts,
                              const size_t *lens, size_t count);
static int send_vset(int fd, const char *question, const char *answer,
                     const char *scope);
static int send_vget(int fd, const char *question, const char *scope);
static int send_vsearch(int fd, const char *query, const char *scope,
                        int topk);
static int send_vdel(int fd, const char *key, const char *scope);
static void make_question(char *buf, size_t cap, const char *prefix, int i);
static void make_answer(char *buf, size_t cap, const char *prefix, int i);
static int parse_resp_object(const char *buf, int len, int *consumed);
static int classify_resp(const char *data, int len, op_t op);
static int compare_longlong(const void *a, const void *b);
static void calc_stats(long long *latencies_us, int count, long long elapsed_us,
                       int success, int failed, op_t op);

/* ── time ───────────────────────────────────────────────────────── */

static long long now_us(void) {
    struct timeval tv;
    gettimeofday(&tv, NULL);
    return (long long)tv.tv_sec * 1000000LL + (long long)tv.tv_usec;
}

/* ── networking ─────────────────────────────────────────────────── */

static int connect_tcp(const char *host, int port) {
    char port_str[16];
    struct addrinfo hints, *result, *rp;
    int fd = -1;

    snprintf(port_str, sizeof(port_str), "%d", port);
    memset(&hints, 0, sizeof(hints));
    hints.ai_family = AF_UNSPEC;
    hints.ai_socktype = SOCK_STREAM;

    if (getaddrinfo(host, port_str, &hints, &result) != 0)
        return -1;

    for (rp = result; rp != NULL; rp = rp->ai_next) {
        fd = socket(rp->ai_family, rp->ai_socktype, rp->ai_protocol);
        if (fd < 0) continue;
        if (connect(fd, rp->ai_addr, rp->ai_addrlen) == 0) {
            int flag = 1;
            setsockopt(fd, IPPROTO_TCP, TCP_NODELAY, (char *)&flag, sizeof(flag));
            break;
        }
        close(fd);
        fd = -1;
    }
    freeaddrinfo(result);
    return fd;
}

static int set_nonblocking(int fd) {
    int flags = fcntl(fd, F_GETFL, 0);
    if (flags < 0) return -1;
    return fcntl(fd, F_SETFL, flags | O_NONBLOCK);
}

static int send_all(int fd, const char *buf, size_t len) {
    size_t sent = 0;
    while (sent < len) {
        ssize_t n = send(fd, buf + sent, len - sent, 0);
        if (n < 0) {
            if (errno == EINTR) continue;
            if (errno == EAGAIN || errno == EWOULDBLOCK) break;
            return -1;
        }
        if (n == 0) return -1;
        sent += (size_t)n;
    }
    return (sent == len) ? 0 : -2; /* -2 = would block / partial */
}

/* ── RESP command builder ───────────────────────────────────────── */

static int build_resp_command(char *out, size_t cap, const char **parts,
                              const size_t *lens, size_t count) {
    size_t pos = 0;
    int n;

    n = snprintf(out + pos, cap - pos, "*%zu\r\n", count);
    if (n <= 0 || (size_t)n >= cap - pos) return -1;
    pos += (size_t)n;

    for (size_t i = 0; i < count; i++) {
        n = snprintf(out + pos, cap - pos, "$%zu\r\n", lens[i]);
        if (n <= 0 || (size_t)n >= cap - pos) return -1;
        pos += (size_t)n;

        if (lens[i] > 0) {
            if (pos + lens[i] + 2 > cap) return -1;
            memcpy(out + pos, parts[i], lens[i]);
            pos += lens[i];
        }
        out[pos++] = '\r';
        out[pos++] = '\n';
    }
    return (int)pos;
}

static int do_send_command(int fd, const char **parts, const size_t *lens,
                           size_t count) {
    char buf[SEND_BUF_SIZE];
    int len = build_resp_command(buf, sizeof(buf), parts, lens, count);
    if (len < 0) return -1;
    return send_all(fd, buf, (size_t)len);
}

static int send_vset(int fd, const char *question, const char *answer,
                     const char *scope) {
    const char *parts[4];
    size_t lens[4];
    size_t count = 0;

    parts[count] = "VSET"; lens[count] = 4; count++;
    parts[count] = question; lens[count] = strlen(question); count++;
    parts[count] = answer; lens[count] = strlen(answer); count++;
    if (scope && scope[0]) {
        parts[count] = scope; lens[count] = strlen(scope); count++;
    }
    return do_send_command(fd, parts, lens, count);
}

static int send_vget(int fd, const char *question, const char *scope) {
    const char *parts[3];
    size_t lens[3];
    size_t count = 0;

    parts[count] = "VGET"; lens[count] = 4; count++;
    parts[count] = question; lens[count] = strlen(question); count++;
    if (scope && scope[0]) {
        parts[count] = scope; lens[count] = strlen(scope); count++;
    }
    return do_send_command(fd, parts, lens, count);
}

static int send_vsearch(int fd, const char *query, const char *scope,
                        int topk) {
    const char *parts[4];
    size_t lens[4];
    char topk_buf[32];
    size_t count = 0;

    snprintf(topk_buf, sizeof(topk_buf), "%d", topk);

    parts[count] = "VSEARCH"; lens[count] = 7; count++;
    parts[count] = query; lens[count] = strlen(query); count++;
    if (scope && scope[0]) {
        parts[count] = scope; lens[count] = strlen(scope); count++;
    }
    parts[count] = topk_buf; lens[count] = strlen(topk_buf); count++;

    return do_send_command(fd, parts, lens, count);
}

static int send_vdel(int fd, const char *key, const char *scope) {
    const char *parts[3];
    size_t lens[3];
    size_t count = 0;

    parts[count] = "VDEL"; lens[count] = 4; count++;
    parts[count] = key; lens[count] = strlen(key); count++;
    if (scope && scope[0]) {
        parts[count] = scope; lens[count] = strlen(scope); count++;
    }
    return do_send_command(fd, parts, lens, count);
}

/* ── key generation ─────────────────────────────────────────────── */

static void make_question(char *buf, size_t cap, const char *prefix, int i) {
    snprintf(buf, cap, "%s_question_%08d", prefix, i);
}

static void make_answer(char *buf, size_t cap, const char *prefix, int i) {
    snprintf(buf, cap, "%s_answer_%08d", prefix, i);
}

/* ── RESP parser ────────────────────────────────────────────────── */

static int read_line_len(const char *buf, int len) {
    for (int i = 0; i + 1 < len; i++)
        if (buf[i] == '\r' && buf[i + 1] == '\n')
            return i + 2;
    return 0;
}

static int parse_int_line(const char *buf, int len, long long *value,
                          int *line_len) {
    int l = read_line_len(buf, len);
    if (l == 0) return 0;
    if (l < 3) return -1;

    int i = 0, sign = 1;
    long long v = 0;

    if (buf[0] == '-') { sign = -1; i = 1; }
    else if (buf[0] == '+') { i = 1; }

    for (; i < l - 2; i++) {
        if (buf[i] < '0' || buf[i] > '9') return -1;
        v = v * 10 + (buf[i] - '0');
    }
    if (value) *value = v * sign;
    if (line_len) *line_len = l;
    return 1;
}

/*
 * Parse one complete RESP object from buffer.
 * Returns: 1 = complete (consumed set), 0 = incomplete, -1 = protocol error
 */
static int parse_resp_object(const char *buf, int len, int *consumed) {
    if (len <= 0 || !buf || !consumed) return 0;

    char type = buf[0];
    int used = 1;

    switch (type) {
    case '+':
    case '-':
    case ':': {
        int l = read_line_len(buf + 1, len - 1);
        if (l == 0) return 0;
        *consumed = 1 + l;
        return 1;
    }
    case '$': {
        long long num;
        int line_len;
        int ret = parse_int_line(buf + 1, len - 1, &num, &line_len);
        if (ret <= 0) return ret;
        used += line_len;
        if (num < 0) { *consumed = used; return 1; }
        if (len < used + (int)num + 2) return 0;
        if (buf[used + num] != '\r' || buf[used + num + 1] != '\n') return -1;
        *consumed = used + (int)num + 2;
        return 1;
    }
    case '*': {
        long long num;
        int line_len;
        int ret = parse_int_line(buf + 1, len - 1, &num, &line_len);
        if (ret <= 0) return ret;
        used += line_len;
        if (num < 0) { *consumed = used; return 1; }
        for (long long i = 0; i < num; i++) {
            int child_consumed = 0;
            ret = parse_resp_object(buf + used, len - used, &child_consumed);
            if (ret <= 0) return ret;
            used += child_consumed;
        }
        *consumed = used;
        return 1;
    }
    default:
        return -1;
    }
}

/*
 * Classify a complete RESP response.
 * Returns: 1 = success, 0 = failed (NO EXIST, unexpected type), -1 = error
 */
static int classify_resp(const char *data, int len, op_t op) {
    if (len <= 0) return -1;

    /* Error response */
    if (data[0] == '-') return -1;

    switch (op) {
    case OP_VSET:
        return (len >= 5 && strncmp(data, "+OK\r\n", 5) == 0) ? 1 : 0;
    case OP_VGET:
        /* Bulk string = hit, +NO EXIST = miss */
        if (data[0] == '$') return 1;
        if (len >= 3 && strncmp(data, "+NO", 3) == 0) return 0;
        return 0;
    case OP_VSEARCH:
        /* Array response */
        return (data[0] == '*') ? 1 : 0;
    case OP_VDEL:
        /* :1 or +OK both mean success */
        if (data[0] == ':') return 1;
        if (len >= 5 && strncmp(data, "+OK\r\n", 5) == 0) return 1;
        if (data[0] == '+') return 0; /* +NO EXIST etc */
        return 0;
    }
    return -1;
}

/* ── statistics ─────────────────────────────────────────────────── */

static int compare_longlong(const void *a, const void *b) {
    long long va = *(const long long *)a;
    long long vb = *(const long long *)b;
    return (va > vb) - (va < vb);
}

static void calc_stats(long long *latencies_us, int count, long long elapsed_us,
                       int success, int failed, op_t op) {
    if (count == 0) {
        printf("avg_ms=0.00\np50_ms=0.00\np95_ms=0.00\np99_ms=0.00\nmin_ms=0.00\nmax_ms=0.00\n");
        return;
    }

    qsort(latencies_us, count, sizeof(long long), compare_longlong);

    double sum = 0.0;
    for (int i = 0; i < count; i++)
        sum += (double)latencies_us[i] / 1000.0;

    double avg = sum / count;
    double p50 = (double)latencies_us[count * 50 / 100] / 1000.0;
    double p95 = (double)latencies_us[count * 95 / 100] / 1000.0;
    double p99 = (double)latencies_us[count * 99 / 100] / 1000.0;
    double min_ms = (double)latencies_us[0] / 1000.0;
    double max_ms = (double)latencies_us[count - 1] / 1000.0;

    double elapsed_sec = (double)elapsed_us / 1000000.0;
    if (elapsed_sec <= 0.0) elapsed_sec = 0.001;
    double qps = (double)(success + failed) / elapsed_sec;

    printf("elapsed_ms=%.0f\n", (double)elapsed_us / 1000.0);
    printf("qps=%.2f\n", qps);
    printf("avg_ms=%.2f\n", avg);
    printf("p50_ms=%.2f\n", p50);
    printf("p95_ms=%.2f\n", p95);
    printf("p99_ms=%.2f\n", p99);
    printf("min_ms=%.2f\n", min_ms);
    printf("max_ms=%.2f\n", max_ms);
}

/* ── latency mode ───────────────────────────────────────────────── */

static void run_latency(const config_t *cfg) {
    char question[MAX_KEY_LEN], answer[MAX_VAL_LEN];
    char recv_buf[RECV_BUF_SIZE];
    int total_keys = cfg->prepare_count > 0 ? cfg->prepare_count : cfg->n_requests;

    long long *latencies = malloc((size_t)cfg->n_requests * sizeof(long long));
    if (!latencies) {
        fprintf(stderr, "malloc latencies failed\n");
        exit(1);
    }

    int fd = connect_tcp(cfg->host, cfg->port);
    if (fd < 0) {
        fprintf(stderr, "connect failed\n");
        free(latencies);
        exit(1);
    }

    int success = 0, failed = 0, errors = 0;
    long long start_us = now_us();

    for (int i = 0; i < cfg->n_requests; i++) {
        int idx = i % total_keys;
        make_question(question, sizeof(question), cfg->prefix, idx);

        long long t0 = now_us();
        int send_ret = 0;

        switch (cfg->op) {
        case OP_VSET:
            make_answer(answer, sizeof(answer), cfg->prefix, i);
            send_ret = send_vset(fd, question, answer, cfg->scope);
            break;
        case OP_VGET:
            send_ret = send_vget(fd, question, cfg->scope);
            break;
        case OP_VSEARCH:
            send_ret = send_vsearch(fd, question, cfg->scope, cfg->topk);
            break;
        case OP_VDEL:
            send_ret = send_vdel(fd, question, cfg->scope);
            break;
        }

        if (send_ret != 0) {
            errors++;
            latencies[i] = 0;
            continue;
        }

        /* Read complete response */
        int total = 0;
        int parse_ok = 0, resp_len = 0;

        while (1) {
            ssize_t n = recv(fd, recv_buf + total, sizeof(recv_buf) - total - 1, 0);
            if (n < 0) {
                if (errno == EINTR) continue;
                errors++;
                break;
            }
            if (n == 0) { errors++; break; }
            total += n;
            recv_buf[total] = '\0';

            int consumed = 0;
            int ret = parse_resp_object(recv_buf, total, &consumed);
            if (ret < 0) { errors++; break; }
            if (ret == 1) {
                parse_ok = 1;
                resp_len = consumed;
                break;
            }
            if (total >= (int)sizeof(recv_buf) - 1) { errors++; break; }
        }

        long long t1 = now_us();
        if (!parse_ok) { continue; }

        latencies[i] = t1 - t0;

        int cls = classify_resp(recv_buf, resp_len, cfg->op);
        if (cls == 1) success++;
        else if (cls == 0) failed++;
        else errors++;
    }

    long long end_us = now_us();

    close(fd);

    printf("mode=latency\n");
    printf("op=");
    switch (cfg->op) {
    case OP_VSET:    printf("vset\n");    break;
    case OP_VGET:    printf("vget\n");    break;
    case OP_VSEARCH: printf("vsearch\n"); break;
    case OP_VDEL:    printf("vdel\n");    break;
    }
    printf("requests=%d\n", cfg->n_requests);
    printf("clients=%d\n", cfg->n_clients);
    printf("pipeline=%d\n", cfg->pipeline);
    printf("success=%d\n", success);
    printf("failed=%d\n", failed);
    printf("errors=%d\n", errors);

    calc_stats(latencies, cfg->n_requests - errors, end_us - start_us,
               success, failed, cfg->op);

    free(latencies);
}

/* ── throughput mode ─────────────────────────────────────────────── */

static void mod_epoll(int epfd, client_t *c, int enable_out) {
    struct epoll_event ev;
    ev.events = EPOLLIN;
    if (enable_out) ev.events |= EPOLLOUT;
    ev.data.ptr = c;
    epoll_ctl(epfd, EPOLL_CTL_MOD, c->fd, &ev);
}

static int try_send_one(client_t *c, const config_t *cfg, int idx, int *total_sent) {
    char question[MAX_KEY_LEN], answer[MAX_VAL_LEN];
    make_question(question, sizeof(question), cfg->prefix, idx);

    long long t0 = now_us();
    int send_ret = 0;

    switch (cfg->op) {
    case OP_VSET:
        make_answer(answer, sizeof(answer), cfg->prefix, *total_sent);
        send_ret = send_vset(c->fd, question, answer, cfg->scope);
        break;
    case OP_VGET:
        send_ret = send_vget(c->fd, question, cfg->scope);
        break;
    case OP_VSEARCH:
        send_ret = send_vsearch(c->fd, question, cfg->scope, cfg->topk);
        break;
    case OP_VDEL:
        send_ret = send_vdel(c->fd, question, cfg->scope);
        break;
    }

    if (send_ret == 0) {
        c->pending_times[c->tail] = t0;
        c->tail = (c->tail + 1) % c->max_pending;
        c->pending++;
        c->sent++;
        (*total_sent)++;
        return 1;
    }
    if (send_ret == -2) {
        /* Partial send / would block — retry later, don't count as sent */
        return 0;
    }
    /* Hard error */
    return -1;
}

static void run_throughput(const config_t *cfg) {
    int total_keys = cfg->prepare_count > 0 ? cfg->prepare_count : cfg->n_requests;
    client_t *clients = calloc((size_t)cfg->n_clients, sizeof(client_t));
    long long *latencies = malloc((size_t)cfg->n_requests * sizeof(long long));
    int latency_count = 0;

    if (!clients || !latencies) {
        fprintf(stderr, "malloc failed\n");
        free(clients); free(latencies);
        exit(1);
    }

    int epfd = epoll_create1(0);
    if (epfd < 0) { perror("epoll_create1"); free(clients); free(latencies); exit(1); }

    for (int i = 0; i < cfg->n_clients; i++) {
        client_t *c = &clients[i];
        c->fd = connect_tcp(cfg->host, cfg->port);
        if (c->fd < 0) {
            fprintf(stderr, "client %d connect failed\n", i);
            c->dead = 1;
            continue;
        }
        set_nonblocking(c->fd);
        c->max_pending = cfg->pipeline;
        c->pending_times = calloc((size_t)cfg->pipeline, sizeof(long long));
        if (!c->pending_times) {
            close(c->fd); c->dead = 1;
            continue;
        }

        struct epoll_event ev;
        ev.events = EPOLLIN | EPOLLOUT; /* LT: enable OUT so we can start sending */
        ev.data.ptr = c;
        if (epoll_ctl(epfd, EPOLL_CTL_ADD, c->fd, &ev) < 0) {
            close(c->fd); free(c->pending_times); c->dead = 1;
        }
    }

    int total_sent = 0, total_received = 0, success = 0, failed = 0, errors = 0;
    long long start_us = now_us();
    struct epoll_event events[MAX_EVENTS];

    /* Initial send burst: distribute across all connections */
    for (int i = 0; i < cfg->n_clients && total_sent < cfg->n_requests; i++) {
        client_t *c = &clients[i];
        if (c->dead) continue;
        while (c->pending < c->max_pending && total_sent < cfg->n_requests) {
            int idx = total_sent % total_keys;
            int ret = try_send_one(c, cfg, idx, &total_sent);
            if (ret <= 0) { if (ret < 0) errors++; break; }
        }
        /* Disable EPOLLOUT for this fd if at capacity */
        if (c->pending >= c->max_pending)
            mod_epoll(epfd, c, 0);
    }

    while (total_received + errors < cfg->n_requests) {
        int nfds = epoll_wait(epfd, events, MAX_EVENTS, 100);
        if (nfds < 0) {
            if (errno == EINTR) continue;
            break;
        }

        for (int i = 0; i < nfds; i++) {
            client_t *c = (client_t *)events[i].data.ptr;
            if (c->dead) continue;

            uint32_t revents = events[i].events;

            /* Try to send more if we got EPOLLOUT or as a general retry */
            if (revents & EPOLLOUT) {
                while (c->pending < c->max_pending && total_sent < cfg->n_requests) {
                    int idx = total_sent % total_keys;
                    int ret = try_send_one(c, cfg, idx, &total_sent);
                    if (ret <= 0) { if (ret < 0) errors++; break; }
                }
                if (c->pending >= c->max_pending || total_sent >= cfg->n_requests)
                    mod_epoll(epfd, c, 0);
            }

            /* Receive */
            if (revents & (EPOLLIN | EPOLLERR | EPOLLHUP)) {
                if (revents & (EPOLLERR | EPOLLHUP)) {
                    errors += c->pending;
                    total_received += c->pending;
                    c->pending = 0;
                    c->dead = 1;
                    continue;
                }

                char tmp[65536];
                ssize_t n = recv(c->fd, tmp, sizeof(tmp), 0);
                if (n < 0) {
                    if (errno == EINTR) continue;
                    if (errno == EAGAIN || errno == EWOULDBLOCK) continue;
                    errors += c->pending;
                    total_received += c->pending;
                    c->pending = 0;
                    c->dead = 1;
                    continue;
                }
                if (n == 0) {
                    errors += c->pending;
                    total_received += c->pending;
                    c->pending = 0;
                    c->dead = 1;
                    continue;
                }

                if (c->recv_len + n > (int)sizeof(c->recv_buf)) {
                    errors += c->pending;
                    total_received += c->pending;
                    c->pending = 0;
                    c->recv_len = 0;
                    continue;
                }
                memcpy(c->recv_buf + c->recv_len, tmp, n);
                c->recv_len += n;

                int had_pending = c->pending;

                while (c->recv_len > 0 && c->pending > 0) {
                    int consumed = 0;
                    int ret = parse_resp_object(c->recv_buf, c->recv_len, &consumed);
                    if (ret < 0) {
                        errors++;
                        total_received++;
                        c->recv_len = 0;
                        c->head = (c->head + 1) % c->max_pending;
                        c->pending--;
                        break;
                    }
                    if (ret == 0) break;

                    long long t1 = now_us();
                    long long start = c->pending_times[c->head];
                    c->head = (c->head + 1) % c->max_pending;
                    c->pending--;
                    c->received++;
                    total_received++;

                    if (latency_count < cfg->n_requests)
                        latencies[latency_count++] = t1 - start;

                    int cls = classify_resp(c->recv_buf, consumed, cfg->op);
                    if (cls == 1) success++;
                    else if (cls == 0) failed++;
                    else errors++;

                    if (consumed < c->recv_len)
                        memmove(c->recv_buf, c->recv_buf + consumed, c->recv_len - consumed);
                    c->recv_len -= consumed;
                }

                /* Re-enable EPOLLOUT if we have room */
                if (had_pending >= c->max_pending && c->pending < c->max_pending
                    && total_sent < cfg->n_requests)
                    mod_epoll(epfd, c, 1);
            }
        }
    }

    long long end_us = now_us();

    for (int i = 0; i < cfg->n_clients; i++) {
        client_t *c = &clients[i];
        if (!c->dead && c->fd >= 0) close(c->fd);
        free(c->pending_times);
    }
    close(epfd);

    printf("mode=throughput\n");
    printf("op=");
    switch (cfg->op) {
    case OP_VSET:    printf("vset\n");    break;
    case OP_VGET:    printf("vget\n");    break;
    case OP_VSEARCH: printf("vsearch\n"); break;
    case OP_VDEL:    printf("vdel\n");    break;
    }
    printf("requests=%d\n", cfg->n_requests);
    printf("clients=%d\n", cfg->n_clients);
    printf("pipeline=%d\n", cfg->pipeline);
    printf("success=%d\n", success);
    printf("failed=%d\n", failed);
    printf("errors=%d\n", errors);

    calc_stats(latencies, latency_count, end_us - start_us,
               success, failed, cfg->op);

    free(clients);
    free(latencies);
}

/* ── prepare phase ──────────────────────────────────────────────── */

static void run_prepare(const config_t *cfg) {
    char question[MAX_KEY_LEN], answer[MAX_VAL_LEN];
    char recv_buf[RECV_BUF_SIZE];

    int fd = connect_tcp(cfg->host, cfg->port);
    if (fd < 0) {
        fprintf(stderr, "prepare: connect failed\n");
        exit(1);
    }

    int success = 0, failed = 0;
    long long start_us = now_us();

    for (int i = 0; i < cfg->prepare_count; i++) {
        make_question(question, sizeof(question), cfg->prefix, i);
        make_answer(answer, sizeof(answer), cfg->prefix, i);

        if (send_vset(fd, question, answer, cfg->scope) != 0) {
            failed++;
            continue;
        }

        int total = 0;
        int parse_ok = 0, resp_len = 0;

        while (1) {
            ssize_t n = recv(fd, recv_buf + total, sizeof(recv_buf) - total - 1, 0);
            if (n < 0) {
                if (errno == EINTR) continue;
                failed++; break;
            }
            if (n == 0) { failed++; break; }
            total += n;
            recv_buf[total] = '\0';

            int consumed = 0;
            int ret = parse_resp_object(recv_buf, total, &consumed);
            if (ret < 0) { failed++; break; }
            if (ret == 1) {
                parse_ok = 1;
                resp_len = consumed;
                break;
            }
            if (total >= (int)sizeof(recv_buf) - 1) { failed++; break; }
        }

        if (!parse_ok) continue;

        if (resp_len >= 5 && strncmp(recv_buf, "+OK\r\n", 5) == 0)
            success++;
        else
            failed++;

        if ((i + 1) % 10000 == 0 || i == cfg->prepare_count - 1)
            fprintf(stderr, "prepare: %d/%d success=%d failed=%d\n",
                    i + 1, cfg->prepare_count, success, failed);
    }

    long long end_us = now_us();
    close(fd);

    double elapsed_sec = (double)(end_us - start_us) / 1000000.0;
    if (elapsed_sec <= 0.0) elapsed_sec = 0.001;

    printf("prepare_count=%d\n", cfg->prepare_count);
    printf("prepare_success=%d\n", success);
    printf("prepare_failed=%d\n", failed);
    printf("prepare_elapsed_ms=%.0f\n", (double)(end_us - start_us) / 1000.0);
    printf("prepare_qps=%.2f\n", (double)success / elapsed_sec);
}

/* ── main ───────────────────────────────────────────────────────── */

static void print_usage(const char *prog) {
    printf("Usage: %s --mode <latency|throughput> [options]\n\n", prog);
    printf("Options:\n");
    printf("  --mode <latency|throughput>  Benchmark mode (required)\n");
    printf("  -h <host>                    Server host (default: 127.0.0.1)\n");
    printf("  -p <port>                    Server port (default: 9300)\n");
    printf("  -t <vset|vget|vsearch|vdel>  Operation (required)\n");
    printf("  -n <requests>                Total requests (required)\n");
    printf("  -c <clients>                 Concurrent connections\n");
    printf("                               (latency default: 1, throughput default: 50)\n");
    printf("  -P <pipeline>                Pipeline depth (default: 1)\n");
    printf("  --scope <scope>              Scope (default: bench_scope)\n");
    printf("  --prefix <prefix>            Key prefix (default: bench)\n");
    printf("  --topk <topk>                Top-K for VSEARCH (default: 10)\n");
    printf("  --prepare <count>            Pre-populate count records before benchmark\n");
}

int main(int argc, char **argv) {
    config_t cfg;
    memset(&cfg, 0, sizeof(cfg));
    cfg.host = "127.0.0.1";
    cfg.port = 9300;
    cfg.scope = "bench_scope";
    cfg.prefix = "bench";
    cfg.topk = DEFAULT_TOPK;
    cfg.pipeline = 1;

    int mode_set = 0, op_set = 0;

    enum {
        OPT_MODE = 256,
        OPT_SCOPE,
        OPT_PREFIX,
        OPT_TOPK,
        OPT_PREPARE,
    };

    static struct option long_opts[] = {
        {"mode",    required_argument, NULL, OPT_MODE},
        {"scope",   required_argument, NULL, OPT_SCOPE},
        {"prefix",  required_argument, NULL, OPT_PREFIX},
        {"topk",    required_argument, NULL, OPT_TOPK},
        {"prepare", required_argument, NULL, OPT_PREPARE},
        {NULL, 0, NULL, 0}
    };

    int opt;
    while ((opt = getopt_long(argc, argv, "h:p:t:n:c:P:", long_opts, NULL)) != -1) {
        switch (opt) {
        case OPT_MODE:
            if (strcmp(optarg, "latency") == 0) {
                cfg.mode = BENCH_MODE_LATENCY;
                cfg.n_clients = 1;
            } else if (strcmp(optarg, "throughput") == 0) {
                cfg.mode = BENCH_MODE_THROUGHPUT;
                cfg.n_clients = 50;
            } else {
                fprintf(stderr, "Invalid mode: %s\n", optarg);
                return 1;
            }
            mode_set = 1;
            break;
        case 'h': cfg.host = optarg; break;
        case 'p': cfg.port = atoi(optarg); break;
        case 't':
            if (strcmp(optarg, "vset") == 0) cfg.op = OP_VSET;
            else if (strcmp(optarg, "vget") == 0) cfg.op = OP_VGET;
            else if (strcmp(optarg, "vsearch") == 0) cfg.op = OP_VSEARCH;
            else if (strcmp(optarg, "vdel") == 0) cfg.op = OP_VDEL;
            else { fprintf(stderr, "Invalid op: %s\n", optarg); return 1; }
            op_set = 1;
            break;
        case 'n': cfg.n_requests = atoi(optarg); break;
        case 'c': cfg.n_clients = atoi(optarg); break;
        case 'P': cfg.pipeline = atoi(optarg); break;
        case OPT_SCOPE: cfg.scope = optarg; break;
        case OPT_PREFIX: cfg.prefix = optarg; break;
        case OPT_TOPK: cfg.topk = atoi(optarg); break;
        case OPT_PREPARE: cfg.prepare_count = atoi(optarg); break;
        default:
            print_usage(argv[0]);
            return 1;
        }
    }

    if (!mode_set || !op_set || cfg.n_requests <= 0) {
        print_usage(argv[0]);
        return 1;
    }

    if (cfg.n_clients <= 0) cfg.n_clients = 1;
    if (cfg.pipeline <= 0) cfg.pipeline = 1;

    /* Validate: latency mode warns but allows overrides */
    if (cfg.mode == BENCH_MODE_LATENCY && (cfg.n_clients > 1 || cfg.pipeline > 1))
        fprintf(stderr, "NOTE: latency mode with clients=%d pipeline=%d "
                "(recommend c=1 P=1)\n", cfg.n_clients, cfg.pipeline);

    /* Prepare phase */
    if (cfg.prepare_count > 0)
        run_prepare(&cfg);

    /* Benchmark */
    if (cfg.mode == BENCH_MODE_LATENCY)
        run_latency(&cfg);
    else
        run_throughput(&cfg);

    return 0;
}
