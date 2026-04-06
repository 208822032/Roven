#define _POSIX_C_SOURCE 200809L

#include <arpa/inet.h>
#include <stdint.h>
#include <netinet/tcp.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/time.h>
#include <time.h>
#include <unistd.h>

#define ARRAY_SIZE(a) (sizeof(a) / sizeof((a)[0]))
#define DEFAULT_KEYSPACE 100000
#define DEFAULT_REQUESTS 100000
#define DEFAULT_WARMUP 10000
#define DEFAULT_VALUE_SIZE 32
#define DEFAULT_TTL_SECS 2
#define DEFAULT_WAIT_BUFFER_SECS 2
#define PIPELINE_BATCH_KEYS 256
#define RESP_IO_BUF 8192
#define RESP_TEXT_BUF 1024
#define KEY_BUFFER_SIZE 48

typedef enum {
    RESP_KIND_UNKNOWN = 0,
    RESP_KIND_SIMPLE,
    RESP_KIND_ERROR,
    RESP_KIND_INTEGER,
    RESP_KIND_BULK,
    RESP_KIND_NIL,
} resp_kind_t;

typedef enum {
    PHASE_PREPARE = 1,
    PHASE_MEASURE = 2,
} phase_t;

typedef enum {
    SCENARIO_NON_EXPIRED_READ = 1,
    SCENARIO_EXPIRED_DELETE_ONLY = 2,
    SCENARIO_EXPIRED_DELETE_TCP_SYNC = 3,
    SCENARIO_EXPIRED_DELETE_EBPF_SYNC = 4,
} scenario_t;

typedef struct {
    int mode;
    const char *name;
    const char *write_cmd;
    const char *read_cmd;
    const char *key_prefix;
} backend_cfg_t;

typedef struct {
    int fd;
    char buf[RESP_IO_BUF];
    size_t pos;
    size_t len;
} resp_reader_t;

typedef struct {
    char **keys;
    uint32_t *order;
    size_t key_count;
} keyset_t;

typedef struct {
    char *value;
    size_t value_size;
} bench_state_t;

static const backend_cfg_t g_backends[] = {
    {1, "array", "SET", "GET", "exp_array_"},
    {2, "rbtree", "RSET", "RGET", "exp_rbtree_"},
    {3, "hash", "HSET", "HGET", "exp_hash_"},
    {4, "skiptable", "SSET", "SGET", "exp_skiptable_"},
};

static volatile unsigned long g_checksum = 0;

static uint32_t lcg_next(uint32_t *state) {
    *state = (*state * 1664525u) + 1013904223u;
    return *state;
}

static double elapsed_seconds(const struct timespec *begin, const struct timespec *end) {
    return (double)(end->tv_sec - begin->tv_sec) + (double)(end->tv_nsec - begin->tv_nsec) / 1000000000.0;
}

static void make_value(char *buffer, size_t size) {
    if (!buffer || size == 0) return;
    if (size == 1) {
        buffer[0] = '\0';
        return;
    }
    memset(buffer, 'v', size - 1);
    buffer[size - 1] = '\0';
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
    return (tail[0] == '\r' && tail[1] == '\n') ? 0 : -1;
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

static int send_command_request(resp_reader_t *reader, const char **parts, int argc, char *resp_text, size_t resp_cap,
                                long long *resp_int, resp_kind_t *resp_kind) {
    char request[512];
    int req_len = build_single_command(request, sizeof(request), parts, argc);
    if (req_len <= 0) return -1;

    int sent = 0;
    while (sent < req_len) {
        int n = (int)send(reader->fd, request + sent, (size_t)(req_len - sent), 0);
        if (n <= 0) return -1;
        sent += n;
    }

    return reader_read_resp(reader, resp_text, resp_cap, resp_int, resp_kind);
}

static int prepare_keyset(keyset_t *set, const char *prefix, size_t key_count) {
    if (!set || !prefix || key_count == 0) return -1;

    memset(set, 0, sizeof(*set));
    set->keys = calloc(key_count, sizeof(char *));
    set->order = calloc(key_count, sizeof(uint32_t));
    if (!set->keys || !set->order) return -1;

    set->key_count = key_count;
    for (size_t i = 0; i < key_count; ++i) {
        set->keys[i] = calloc(KEY_BUFFER_SIZE, sizeof(char));
        if (!set->keys[i]) return -1;
        snprintf(set->keys[i], KEY_BUFFER_SIZE, "%s%06zu", prefix, i);
        set->order[i] = (uint32_t)i;
    }

    return 0;
}

static void destroy_keyset(keyset_t *set) {
    if (!set) return;
    if (set->keys) {
        for (size_t i = 0; i < set->key_count; ++i) {
            free(set->keys[i]);
        }
        free(set->keys);
    }
    free(set->order);
    memset(set, 0, sizeof(*set));
}

static int prepare_state(bench_state_t *state, size_t value_size) {
    if (!state || value_size == 0) return -1;
    memset(state, 0, sizeof(*state));
    state->value = calloc(value_size + 1, sizeof(char));
    if (!state->value) return -1;
    state->value_size = value_size;
    make_value(state->value, value_size + 1);
    return 0;
}

static void destroy_state(bench_state_t *state) {
    if (!state) return;
    free(state->value);
    memset(state, 0, sizeof(*state));
}

static void fill_shuffled_order(uint32_t *order, size_t count) {
    for (size_t i = 0; i < count; ++i) {
        order[i] = (uint32_t)i;
    }
    if (count <= 1) return;

    uint32_t seed = 0x12345678u;
    for (size_t i = count - 1; i > 0; --i) {
        seed = lcg_next(&seed);
        size_t j = (size_t)(seed % (uint32_t)(i + 1));
        uint32_t tmp = order[i];
        order[i] = order[j];
        order[j] = tmp;
    }
}

static int load_keyset_with_ttl(resp_reader_t *reader, const backend_cfg_t *cfg, const keyset_t *set,
                                const bench_state_t *state, int ttl_secs) {
    for (size_t base = 0; base < set->key_count; base += PIPELINE_BATCH_KEYS) {
        size_t batch = set->key_count - base;
        if (batch > PIPELINE_BATCH_KEYS) batch = PIPELINE_BATCH_KEYS;

        char *pipeline = NULL;
        size_t cap = batch * 192 + 128;
        size_t len = 0;
        pipeline = (char *)malloc(cap);
        if (!pipeline) return -1;

        for (size_t i = 0; i < batch; ++i) {
            size_t idx = base + i;
            char ttl_buf[32];
            const char *parts[3];
            snprintf(ttl_buf, sizeof(ttl_buf), "%d", ttl_secs);

            parts[0] = cfg->write_cmd;
            parts[1] = set->keys[idx];
            parts[2] = state->value;
            if (append_resp_array(&pipeline, &cap, &len, parts, 3) != 0) {
                free(pipeline);
                return -1;
            }

            parts[0] = "EXPIRE";
            parts[1] = set->keys[idx];
            parts[2] = ttl_buf;
            if (append_resp_array(&pipeline, &cap, &len, parts, 3) != 0) {
                free(pipeline);
                return -1;
            }
        }

        int sent = 0;
        while (sent < (int)len) {
            int n = (int)send(reader->fd, pipeline + sent, len - (size_t)sent, 0);
            if (n <= 0) {
                free(pipeline);
                return -1;
            }
            sent += n;
        }

        for (size_t i = 0; i < batch * 2; ++i) {
            char text[RESP_TEXT_BUF];
            long long integer_value = 0;
            resp_kind_t kind = RESP_KIND_UNKNOWN;
            if (reader_read_resp(reader, text, sizeof(text), &integer_value, &kind) != 0) {
                free(pipeline);
                return -1;
            }
            if (kind != RESP_KIND_SIMPLE || strcmp(text, "OK") != 0) {
                free(pipeline);
                return -1;
            }
        }

        free(pipeline);
    }

    return 0;
}

static int run_get_phase(resp_reader_t *reader, const backend_cfg_t *cfg, const keyset_t *set,
                         const bench_state_t *state, size_t request_count, const char *expected_text,
                         const char *scenario_name, double *seconds_out) {
    struct timespec begin;
    struct timespec end;
    volatile unsigned long checksum = 0;

    clock_gettime(CLOCK_MONOTONIC, &begin);
    for (size_t i = 0; i < request_count; ++i) {
        const char *parts[2];
        char text[RESP_TEXT_BUF];
        long long integer_value = 0;
        resp_kind_t kind = RESP_KIND_UNKNOWN;

        parts[0] = cfg->read_cmd;
        parts[1] = set->keys[set->order[i]];
        if (send_command_request(reader, parts, 2, text, sizeof(text), &integer_value, &kind) != 0) {
            fprintf(stderr, "[%s][%s] GET failed at request %zu\n", cfg->name, scenario_name, i);
            return -1;
        }

        if (strcmp(text, expected_text) != 0) {
            fprintf(stderr, "[%s][%s] unexpected response at request %zu: expected=%s actual=%s\n",
                    cfg->name, scenario_name, i, expected_text, text);
            return -1;
        }

        checksum += (unsigned long)(unsigned char)text[0];
        checksum += (unsigned long)(unsigned char)state->value[0];
        if (kind == RESP_KIND_INTEGER) {
            checksum += (unsigned long)integer_value;
        }
    }
    clock_gettime(CLOCK_MONOTONIC, &end);

    g_checksum += checksum;
    if (seconds_out) {
        *seconds_out = elapsed_seconds(&begin, &end);
    }
    return 0;
}

static const backend_cfg_t *find_backend_by_mode(int mode) {
    for (size_t i = 0; i < ARRAY_SIZE(g_backends); ++i) {
        if (g_backends[i].mode == mode) return &g_backends[i];
    }
    return NULL;
}

static const char *scenario_label(int scenario) {
    switch (scenario) {
        case SCENARIO_NON_EXPIRED_READ: return "non-expired-read";
        case SCENARIO_EXPIRED_DELETE_ONLY: return "expired-delete-only";
        case SCENARIO_EXPIRED_DELETE_TCP_SYNC: return "expired-delete-tcp-sync";
        case SCENARIO_EXPIRED_DELETE_EBPF_SYNC: return "expired-delete-ebpf-sync";
        default: return "unknown";
    }
}

static void print_usage(const char *prog) {
    fprintf(stderr,
            "usage: %s ip port mode phase scenario [keyspace] [requests] [warmup] [ttl_secs] [wait_buffer_secs] [value_size]\n"
            "phase: prepare | measure\n"
            "scenario: 1=non-expired-read, 2=expired-delete-only, 3=expired-delete-tcp-sync, 4=expired-delete-ebpf-sync\n"
            "mode: 1=array, 2=rbtree, 3=hash, 4=skiptable, 5=all\n"
            "defaults: keyspace=%d requests=%d warmup=%d ttl_secs=%d wait_buffer_secs=%d value_size=%d\n",
            prog, DEFAULT_KEYSPACE, DEFAULT_REQUESTS, DEFAULT_WARMUP, DEFAULT_TTL_SECS, DEFAULT_WAIT_BUFFER_SECS,
            DEFAULT_VALUE_SIZE);
}

static int run_one_backend(const backend_cfg_t *cfg, const char *ip, unsigned short port, phase_t phase,
                           scenario_t scenario, size_t keyspace, size_t requests, size_t warmup,
                           int ttl_secs, int wait_buffer_secs, size_t value_size) {
    int fd = connect_tcpserver(ip, port);
    if (fd < 0) return -1;

    resp_reader_t reader;
    reader_init(&reader, fd);

    keyset_t warmup_keys;
    keyset_t measured_keys;
    bench_state_t state;
    char warmup_prefix[64];
    snprintf(warmup_prefix, sizeof(warmup_prefix), "warm_s%d_%s_", scenario, cfg->name);

    if (prepare_keyset(&warmup_keys, warmup_prefix, warmup == 0 ? 1 : warmup) != 0) {
        fprintf(stderr, "[%s][%s] failed to prepare warmup keyset\n", cfg->name, scenario_label(scenario));
        close(fd);
        return -1;
    }
    char measured_prefix[64];
    snprintf(measured_prefix, sizeof(measured_prefix), "exp_s%d_%s_", scenario, cfg->name);

    if (prepare_keyset(&measured_keys, measured_prefix, keyspace) != 0) {
        fprintf(stderr, "[%s][%s] failed to prepare measured keyset\n", cfg->name, scenario_label(scenario));
        destroy_keyset(&warmup_keys);
        close(fd);
        return -1;
    }
    if (prepare_state(&state, value_size) != 0) {
        fprintf(stderr, "[%s][%s] failed to prepare value state\n", cfg->name, scenario_label(scenario));
        destroy_keyset(&measured_keys);
        destroy_keyset(&warmup_keys);
        close(fd);
        return -1;
    }

    fill_shuffled_order(warmup_keys.order, warmup_keys.key_count);
    fill_shuffled_order(measured_keys.order, measured_keys.key_count);

    size_t warmup_count = warmup_keys.key_count < warmup ? warmup_keys.key_count : warmup;
    if (warmup_count == 0) warmup_count = 1;

    size_t measure_count = requests;
    if (measure_count == 0) measure_count = measured_keys.key_count;
    if (scenario != SCENARIO_NON_EXPIRED_READ && measure_count > measured_keys.key_count) {
        measure_count = measured_keys.key_count;
    }
    if (measure_count > measured_keys.key_count) {
        measure_count = measured_keys.key_count;
    }

    int measured_ttl = ttl_secs;
    int warmup_ttl = ttl_secs;
    if (scenario == SCENARIO_NON_EXPIRED_READ) {
        if (measured_ttl < 60) measured_ttl = 60;
        if (warmup_ttl < 60) warmup_ttl = 60;
    } else {
        if (warmup_ttl < 60) warmup_ttl = 60;
    }

    if (phase == PHASE_PREPARE) {
        if (load_keyset_with_ttl(&reader, cfg, &warmup_keys, &state, warmup_ttl) != 0) {
            fprintf(stderr, "[%s][%s] failed to load warmup keys with ttl=%d\n", cfg->name, scenario_label(scenario), warmup_ttl);
            goto fail;
        }
        if (load_keyset_with_ttl(&reader, cfg, &measured_keys, &state, measured_ttl) != 0) {
            fprintf(stderr, "[%s][%s] failed to load measured keys with ttl=%d\n", cfg->name, scenario_label(scenario), measured_ttl);
            goto fail;
        }

        size_t warmup_ops = warmup_count;
        if (scenario == SCENARIO_NON_EXPIRED_READ && warmup_ops > measured_keys.key_count) {
            warmup_ops = measured_keys.key_count;
        }
        for (size_t i = 0; i < warmup_ops; ++i) {
            const char *parts[2];
            char text[RESP_TEXT_BUF];
            long long integer_value = 0;
            resp_kind_t kind = RESP_KIND_UNKNOWN;
            parts[0] = cfg->read_cmd;
            parts[1] = warmup_keys.keys[warmup_keys.order[i]];
            if (send_command_request(&reader, parts, 2, text, sizeof(text), &integer_value, &kind) != 0) {
                fprintf(stderr, "[%s][%s] warmup read failed at %zu\n", cfg->name, scenario_label(scenario), i);
                goto fail;
            }
            if (strcmp(text, state.value) != 0) {
                fprintf(stderr, "[%s][%s] warmup mismatch at %zu expected=%s actual=%s\n",
                        cfg->name, scenario_label(scenario), i, state.value, text);
                goto fail;
            }
        }

        printf("[PREPARE][%s][%s] keyspace=%zu requests=%zu warmup=%zu ttl_secs=%d value_size=%zu\n",
               cfg->name, scenario_label(scenario), keyspace, requests, warmup, measured_ttl, value_size);
    } else {
        const char *expected = (scenario == SCENARIO_NON_EXPIRED_READ) ? state.value : "NO EXIST";
        double seconds = 0.0;
        if (run_get_phase(&reader, cfg, &measured_keys, &state, measure_count, expected,
                          scenario_label(scenario), &seconds) != 0) {
            fprintf(stderr, "[%s][%s] measure phase failed\n", cfg->name, scenario_label(scenario));
            goto fail;
        }

        double qps = (double)measure_count / seconds;
        double ns_per_op = (seconds * 1000000000.0) / (double)measure_count;
        printf("[RESULT][%s][%s] requests=%zu seconds=%.6f qps=%.2f ns_per_op=%.2f\n",
               cfg->name, scenario_label(scenario), measure_count, seconds, qps, ns_per_op);
    }

    destroy_state(&state);
    destroy_keyset(&measured_keys);
    destroy_keyset(&warmup_keys);
    close(fd);
    return 0;

fail:
    destroy_state(&state);
    destroy_keyset(&measured_keys);
    destroy_keyset(&warmup_keys);
    close(fd);
    return -1;
}

static int run_mode_all(const char *ip, unsigned short port, phase_t phase, scenario_t scenario,
                        size_t keyspace, size_t requests, size_t warmup, int ttl_secs,
                        int wait_buffer_secs, size_t value_size) {
    int rc = 0;
    for (size_t i = 0; i < ARRAY_SIZE(g_backends); ++i) {
        if (run_one_backend(&g_backends[i], ip, port, phase, scenario, keyspace, requests,
                            warmup, ttl_secs, wait_buffer_secs, value_size) != 0) {
            rc = -1;
        }
    }
    return rc;
}

int main(int argc, char *argv[]) {
    if (argc < 6) {
        print_usage(argv[0]);
        return 1;
    }

    const char *ip = argv[1];
    int port = atoi(argv[2]);
    int mode = atoi(argv[3]);
    const char *phase_name = argv[4];
    int scenario = atoi(argv[5]);
    size_t keyspace = (argc >= 7) ? (size_t)strtoul(argv[6], NULL, 10) : DEFAULT_KEYSPACE;
    size_t requests = (argc >= 8) ? (size_t)strtoul(argv[7], NULL, 10) : DEFAULT_REQUESTS;
    size_t warmup = (argc >= 9) ? (size_t)strtoul(argv[8], NULL, 10) : DEFAULT_WARMUP;
    int ttl_secs = (argc >= 10) ? atoi(argv[9]) : DEFAULT_TTL_SECS;
    int wait_buffer_secs = (argc >= 11) ? atoi(argv[10]) : DEFAULT_WAIT_BUFFER_SECS;
    size_t value_size = (argc >= 12) ? (size_t)strtoul(argv[11], NULL, 10) : DEFAULT_VALUE_SIZE;

    if (keyspace == 0) keyspace = DEFAULT_KEYSPACE;
    if (requests == 0) requests = DEFAULT_REQUESTS;
    if (warmup == 0) warmup = DEFAULT_WARMUP;
    if (ttl_secs <= 0) ttl_secs = DEFAULT_TTL_SECS;
    if (wait_buffer_secs < 0) wait_buffer_secs = DEFAULT_WAIT_BUFFER_SECS;
    if (value_size == 0) value_size = DEFAULT_VALUE_SIZE;

    phase_t phase;
    if (strcmp(phase_name, "prepare") == 0) {
        phase = PHASE_PREPARE;
    } else if (strcmp(phase_name, "measure") == 0) {
        phase = PHASE_MEASURE;
    } else {
        fprintf(stderr, "unknown phase %s\n", phase_name);
        print_usage(argv[0]);
        return 1;
    }

    scenario_t sc = (scenario_t)scenario;
    const backend_cfg_t *cfg = find_backend_by_mode(mode);
    if (!cfg && mode != 5) {
        fprintf(stderr, "unknown mode %d\n", mode);
        print_usage(argv[0]);
        return 1;
    }

    if (phase == PHASE_MEASURE && requests > keyspace) {
        requests = keyspace;
    }

    if (mode == 5) {
        return run_mode_all(ip, (unsigned short)port, phase, sc, keyspace, requests, warmup,
                            ttl_secs, wait_buffer_secs, value_size) == 0 ? 0 : 1;
    }

    if (!cfg) {
        fprintf(stderr, "unknown mode %d\n", mode);
        return 1;
    }

    return run_one_backend(cfg, ip, (unsigned short)port, phase, sc, keyspace, requests, warmup,
                           ttl_secs, wait_buffer_secs, value_size) == 0 ? 0 : 1;
}
