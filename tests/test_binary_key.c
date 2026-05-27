/**
 * Binary key support test.
 *
 * Tests SET/GET/EXIST/MOD/DEL with keys containing embedded '\0' bytes
 * across all four data structures (array, hash, rbtree, skiptable).
 *
 * Usage: ./test_binary_key <host> <port>
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <sys/socket.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#include <errno.h>
#include <stdint.h>

static int total_passed = 0;
static int total_failed = 0;

static int connect_server(const char *ip, int port) {
    int fd = socket(AF_INET, SOCK_STREAM, 0);
    if (fd < 0) { perror("socket"); return -1; }
    struct sockaddr_in addr;
    memset(&addr, 0, sizeof(addr));
    addr.sin_family = AF_INET;
    addr.sin_port = htons((uint16_t)port);
    inet_pton(AF_INET, ip, &addr.sin_addr);
    if (connect(fd, (struct sockaddr *)&addr, sizeof(addr)) < 0) {
        perror("connect"); close(fd); return -1;
    }
    return fd;
}

static int send_exact(int fd, const void *data, size_t len) {
    size_t sent = 0;
    while (sent < len) {
        ssize_t n = send(fd, (const char *)data + sent, len - sent, MSG_NOSIGNAL);
        if (n <= 0) return -1;
        sent += (size_t)n;
    }
    return 0;
}

/* Read full RESP response. Returns total bytes read. */
static int recv_full_resp(int fd, char *buf, size_t cap) {
    size_t total = 0;
    while (total < cap - 1) {
        ssize_t n = recv(fd, buf + total, cap - 1 - total, 0);
        if (n <= 0) break;
        total += (size_t)n;
        buf[total] = '\0';
        if (total < 2) continue;
        if (buf[0] == '+' || buf[0] == '-') {
            if (strstr(buf, "\r\n")) break;
        } else if (buf[0] == ':') {
            if (strstr(buf, "\r\n")) break;
        } else if (buf[0] == '$') {
            /* Bulk: $N\r\n<data>\r\n */
            int nlen = -1;
            if (buf[1] == '-') { nlen = -1; break; }
            sscanf(buf, "$%d", &nlen);
            if (nlen >= 0 && total >= (size_t)(5 + nlen) && buf[3 + nlen + 2] == '\0') break;
            if (nlen >= 0 && total >= (size_t)(5 + nlen)) {
                /* Check CRLF at end of data */
                if (buf[3 + nlen] == '\r' || (total > (size_t)(4 + nlen) && buf[4 + nlen] == '\r'))
                    break;
            }
        } else if (buf[0] == '*') {
            /* Array: read enough */
            if (strstr(buf, "\r\n")) break;
        }
    }
    return (int)total;
}

/* Check response for success pattern.
 * KVLite uses: +OK, +EXIST, :1 for positive; +NO EXIST for negative.
 */
static int resp_is_ok(const char *buf) {
    return strncmp(buf, "+OK", 3) == 0 || strncmp(buf, "+EXIST", 6) == 0 || strncmp(buf, ":1", 2) == 0;
}
static int resp_is_noexist(const char *buf) {
    return strstr(buf, "NO EXIST") != NULL || strstr(buf, "NO EXIST") != NULL;
}

/* Get value length from bulk response "$N\r\n..." */
static int resp_get_bulk_len(const char *buf) {
    int n = -1;
    sscanf(buf, "$%d", &n);
    return n;
}

/* Build and send a RESP command */
static int send_resp_cmd(int fd, const char *cmd_name,
                          const char *key, size_t key_len,
                          const char *value, size_t value_len,
                          int argc) {
    char header[256];
    int hlen = snprintf(header, sizeof(header), "*%d\r\n", argc);

    if (send_exact(fd, header, (size_t)hlen) < 0) return -1;

    /* $<cmd_len>\r\n<cmd>\r\n */
    char hdr[64];
    int l;
    l = snprintf(hdr, sizeof(hdr), "$%zu\r\n", strlen(cmd_name));
    if (send_exact(fd, hdr, (size_t)l) < 0) return -1;
    if (send_exact(fd, cmd_name, strlen(cmd_name)) < 0) return -1;
    if (send_exact(fd, "\r\n", 2) < 0) return -1;

    /* $<key_len>\r\n<key>\r\n */
    l = snprintf(hdr, sizeof(hdr), "$%zu\r\n", key_len);
    if (send_exact(fd, hdr, (size_t)l) < 0) return -1;
    if (send_exact(fd, key, key_len) < 0) return -1;
    if (send_exact(fd, "\r\n", 2) < 0) return -1;

    /* value if argc==3 */
    if (argc >= 3 && value) {
        l = snprintf(hdr, sizeof(hdr), "$%zu\r\n", value_len);
        if (send_exact(fd, hdr, (size_t)l) < 0) return -1;
        if (send_exact(fd, value, value_len) < 0) return -1;
        if (send_exact(fd, "\r\n", 2) < 0) return -1;
    }

    return 0;
}

#define PASS(fmt, ...) do { printf("  PASS: " fmt "\n", ##__VA_ARGS__); total_passed++; } while(0)
#define FAIL(fmt, ...) do { printf("  FAIL: " fmt "\n", ##__VA_ARGS__); total_failed++; } while(0)

static void test_ds(int fd, const char *ds_prefix, const char *ds_name,
                    const char *bin_key, size_t key_len,
                    const char *bin_val, size_t val_len,
                    const char *bin_val2, size_t val2_len) {
    char set_cmd[16], get_cmd[16], del_cmd[16], exist_cmd[32], mod_cmd[32];
    snprintf(set_cmd, sizeof(set_cmd), "%sSET", ds_prefix);
    snprintf(get_cmd, sizeof(get_cmd), "%sGET", ds_prefix);
    snprintf(del_cmd, sizeof(del_cmd), "%sDEL", ds_prefix);
    snprintf(exist_cmd, sizeof(exist_cmd), "%sEXIST", ds_prefix);
    snprintf(mod_cmd, sizeof(mod_cmd), "%sMOD", ds_prefix);

    char buf[4096];

    printf("--- %s ---\n", ds_name);

    /* 1. SET binary key + binary value */
    if (send_resp_cmd(fd, set_cmd, bin_key, key_len, bin_val, val_len, 3) < 0) {
        FAIL("%s SET send failed", ds_name); return;
    }
    recv_full_resp(fd, buf, sizeof(buf));
    if (resp_is_ok(buf)) {
        PASS("%s SET key1 val1", ds_name);
    } else {
        FAIL("%s SET key1 val1 (got: %s)", ds_name, buf);
    }

    /* 2. GET binary key - should return exact binary value */
    if (send_resp_cmd(fd, get_cmd, bin_key, key_len, NULL, 0, 2) < 0) {
        FAIL("%s GET send failed", ds_name); return;
    }
    recv_full_resp(fd, buf, sizeof(buf));
    int got_len = resp_get_bulk_len(buf);
    if (got_len == (int)val_len) {
        PASS("%s GET key1 -> %d bytes", ds_name, got_len);
    } else {
        FAIL("%s GET key1: expected %zu bytes, got %d", ds_name, val_len, got_len);
    }

    /* 3. EXIST binary key */
    if (send_resp_cmd(fd, exist_cmd, bin_key, key_len, NULL, 0, 2) < 0) {
        FAIL("%s EXIST send failed", ds_name); return;
    }
    recv_full_resp(fd, buf, sizeof(buf));
    if (resp_is_ok(buf)) {
        PASS("%s EXIST key1", ds_name);
    } else {
        FAIL("%s EXIST key1", ds_name);
    }

    /* 4. MOD binary key */
    if (send_resp_cmd(fd, mod_cmd, bin_key, key_len, bin_val2, val2_len, 3) < 0) {
        FAIL("%s MOD send failed", ds_name); return;
    }
    recv_full_resp(fd, buf, sizeof(buf));
    if (resp_is_ok(buf)) {
        PASS("%s MOD key1 val2", ds_name);
    } else {
        FAIL("%s MOD key1 val2", ds_name);
    }

    /* Verify MOD result */
    if (send_resp_cmd(fd, get_cmd, bin_key, key_len, NULL, 0, 2) < 0) {
        FAIL("%s GET(after MOD) send failed", ds_name); return;
    }
    recv_full_resp(fd, buf, sizeof(buf));
    got_len = resp_get_bulk_len(buf);
    if (got_len == (int)val2_len) {
        PASS("%s GET key1 after MOD -> %d bytes", ds_name, got_len);
    } else {
        FAIL("%s GET after MOD: expected %zu bytes, got %d", ds_name, val2_len, got_len);
    }

    /* 5. DEL binary key */
    if (send_resp_cmd(fd, del_cmd, bin_key, key_len, NULL, 0, 2) < 0) {
        FAIL("%s DEL send failed", ds_name); return;
    }
    recv_full_resp(fd, buf, sizeof(buf));
    if (resp_is_ok(buf)) {
        PASS("%s DEL key1", ds_name);
    } else {
        FAIL("%s DEL key1", ds_name);
    }

    /* 6. Verify DEL */
    if (send_resp_cmd(fd, exist_cmd, bin_key, key_len, NULL, 0, 2) < 0) {
        FAIL("%s EXIST(after DEL) send failed", ds_name); return;
    }
    recv_full_resp(fd, buf, sizeof(buf));
    if (resp_is_noexist(buf)) {
        PASS("%s EXIST key1 after DEL -> not found", ds_name);
    } else {
        FAIL("%s EXIST after DEL (should not exist)", ds_name);
    }
}

int main(int argc, char **argv) {
    if (argc < 3) {
        fprintf(stderr, "Usage: %s <host> <port>\n", argv[0]);
        return 1;
    }
    const char *host = argv[1];
    int port = atoi(argv[2]);

    int fd = connect_server(host, port);
    if (fd < 0) return 1;

    /* Key1: 'a','b','\0','c' (4 bytes) */
    char key1[5] = {'a', 'b', '\0', 'c', '\0'};
    size_t key1_len = 4;

    /* Key2: 'a','b' (2 bytes) - prefix of key1 */
    char key2[3] = {'a', 'b', '\0'};
    size_t key2_len = 2;

    /* Value1: 'x','y','\0','z' (4 bytes) */
    char val1[5] = {'x', 'y', '\0', 'z', '\0'};
    size_t val1_len = 4;

    /* Value2: 'm','n' (2 bytes) */
    char val2[3] = {'m', 'n', '\0'};
    size_t val2_len = 2;

    printf("=== Binary Key/Value Test Suite ===\n\n");

    /* For each DS: SET key1, SET key2, verify both coexist, then cleanup */
    const char *prefixes[] = {"", "R", "H", "S"};
    const char *names[]    = {"ARRAY", "RBTREE", "HASH", "SKIPTABLE"};

    for (int i = 0; i < 4; i++) {
        test_ds(fd, prefixes[i], names[i], key1, key1_len, val1, val1_len, val2, val2_len);
    }

    /* Key1 vs Key2 coexistence test for each DS */
    printf("--- Coexistence: key1 != key2 ---\n");
    for (int i = 0; i < 4; i++) {
        char set_cmd[16], get_cmd[16], del_cmd[16];
        snprintf(set_cmd, sizeof(set_cmd), "%sSET", prefixes[i]);
        snprintf(get_cmd, sizeof(get_cmd), "%sGET", prefixes[i]);
        snprintf(del_cmd, sizeof(del_cmd), "%sDEL", prefixes[i]);
        char buf[4096];

        /* SET key1 val1 and key2 val2 */
        send_resp_cmd(fd, set_cmd, key1, key1_len, val1, val1_len, 3);
        recv_full_resp(fd, buf, sizeof(buf));

        send_resp_cmd(fd, set_cmd, key2, key2_len, val2, val2_len, 3);
        recv_full_resp(fd, buf, sizeof(buf));

        /* GET key2 should return val2_len bytes */
        send_resp_cmd(fd, get_cmd, key2, key2_len, NULL, 0, 2);
        recv_full_resp(fd, buf, sizeof(buf));
        int gl = resp_get_bulk_len(buf);
        if (gl == (int)val2_len) {
            PASS("%s key2 coexists with key1 -> %d bytes", names[i], gl);
        } else {
            FAIL("%s key2: expected %zu bytes, got %d", names[i], val2_len, gl);
        }

        /* GET key1 should still return val1_len bytes */
        send_resp_cmd(fd, get_cmd, key1, key1_len, NULL, 0, 2);
        recv_full_resp(fd, buf, sizeof(buf));
        gl = resp_get_bulk_len(buf);
        if (gl == (int)val1_len) {
            PASS("%s key1 still intact -> %d bytes", names[i], gl);
        } else {
            FAIL("%s key1: expected %zu bytes, got %d", names[i], val1_len, gl);
        }

        /* Cleanup */
        send_resp_cmd(fd, del_cmd, key1, key1_len, NULL, 0, 2);
        recv_full_resp(fd, buf, sizeof(buf));
        send_resp_cmd(fd, del_cmd, key2, key2_len, NULL, 0, 2);
        recv_full_resp(fd, buf, sizeof(buf));
    }

    close(fd);

    printf("\n=== Results: %d passed, %d failed ===\n", total_passed, total_failed);
    return total_failed ? 1 : 0;
}
